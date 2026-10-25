/* Actual VAAPI map/transfer consumer oracle. API interposition is local to this
 * test executable and exercises unsupported, incompatible and failed cleanup. */
#include "gpu.h"

#include <dlfcn.h>
#include <errno.h>
#include <libavcodec/defs.h>
#include <libavutil/intreadwrite.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

#define REAL(name)                                                                                 \
    typeof(&name) call;                                                                            \
    do {                                                                                           \
        void *symbol = dlsym(RTLD_NEXT, #name);                                                    \
        static_assert(sizeof(call) == sizeof(symbol));                                             \
        memcpy(&call, &symbol, sizeof(call));                                                      \
        CHECK(call);                                                                               \
    } while (0)

static enum { NORMAL, UNSUPPORTED, WRONG_FORMAT, FAILED_DESTROY } fault;
static _Atomic unsigned derives, destroys, failed_destroys;
static _Atomic int images;
static _Atomic bool destroy_once;

VAStatus vaDeriveImage(VADisplay display, VASurfaceID id, VAImage *image)
{
    REAL(vaDeriveImage);
    atomic_fetch_add_explicit(&derives, 1, memory_order_relaxed);
    if (fault == UNSUPPORTED)
        return VA_STATUS_ERROR_UNIMPLEMENTED;
    VAStatus status = call(display, id, image);
    if (status == VA_STATUS_SUCCESS) {
        atomic_fetch_add_explicit(&images, 1, memory_order_relaxed);
        if (fault == WRONG_FORMAT || fault == FAILED_DESTROY)
            image->format.fourcc = VA_FOURCC_RGBA;
        if (fault == FAILED_DESTROY)
            atomic_store_explicit(&destroy_once, true, memory_order_relaxed);
    }
    return status;
}

VAStatus vaCreateImage(VADisplay display, VAImageFormat *format, int width, int height,
                       VAImage *image)
{
    REAL(vaCreateImage);
    VAStatus status = call(display, format, width, height, image);
    if (status == VA_STATUS_SUCCESS)
        atomic_fetch_add_explicit(&images, 1, memory_order_relaxed);
    return status;
}

VAStatus vaDestroyImage(VADisplay display, VAImageID id)
{
    REAL(vaDestroyImage);
    if (atomic_exchange_explicit(&destroy_once, false, memory_order_relaxed)) {
        atomic_fetch_add_explicit(&failed_destroys, 1, memory_order_relaxed);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    VAStatus status = call(display, id);
    if (status == VA_STATUS_SUCCESS) {
        atomic_fetch_sub_explicit(&images, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&destroys, 1, memory_order_relaxed);
    }
    return status;
}

static void pattern(AVFrame *frame, unsigned seed, bool check)
{
    bool wide = frame->format == AV_PIX_FMT_P010LE;
    CHECK(wide || frame->format == AV_PIX_FMT_NV12);
    for (unsigned plane = 0; plane < 2; ++plane) {
        unsigned width = plane ? ((unsigned)frame->width + 1) / 2 * 2 : (unsigned)frame->width;
        unsigned height = plane ? ((unsigned)frame->height + 1) / 2 : (unsigned)frame->height;
        CHECK(frame->data[plane] && frame->linesize[plane] >= (int)(width * (wide ? 2U : 1U)));
        for (unsigned y = 0; y < height; ++y) {
            uint8_t *row = frame->data[plane] + (size_t)y * (unsigned)frame->linesize[plane];
            for (unsigned x = 0; x < width; ++x) {
                unsigned sample =
                    (x * 17U + y * 31U + plane * 97U + seed * 43U) & (wide ? 1023U : 255U);
                if (wide) {
                    sample <<= 6;
                    if (check)
                        CHECK(AV_RL16(row + 2 * x) == sample);
                    else
                        AV_WL16(row + 2 * x, (uint16_t)sample);
                } else if (check)
                    CHECK(row[x] == sample);
                else
                    row[x] = (uint8_t)sample;
            }
        }
    }
}

static AVFrame *cpu_frame(enum AVPixelFormat format, int width, int height, unsigned seed)
{
    AVFrame *frame = av_frame_alloc();
    CHECK(frame);
    frame->format = format;
    frame->width = width;
    frame->height = height;
    CHECK(av_frame_get_buffer(frame, 32) == 0);
    pattern(frame, seed, false);
    return frame;
}

static void readback(const AVFrame *source, enum AVPixelFormat format, unsigned seed)
{
    /* Recycled host storage can contain the expected old image. Poison every
     * visible sample so a missing/partial transfer cannot accidentally pass. */
    const AVHWFramesContext *context = (void *)source->hw_frames_ctx->data;
    AVFrame *cpu = cpu_frame(format, context->width, context->height, seed + 1);
    cpu->width = source->width;
    cpu->height = source->height;
    CHECK(av_hwframe_transfer_data(cpu, source, 0) == 0);
    CHECK(cpu->format == format && cpu->width == source->width && cpu->height == source->height);
    pattern(cpu, seed, true);
    av_frame_free(&cpu);
}

static void mapped_roundtrip(const AVFrame *source, enum AVPixelFormat format, int flags,
                             unsigned old_seed, unsigned new_seed)
{
    AVFrame *mapped = av_frame_alloc();
    CHECK(mapped);
    mapped->format = format;
    CHECK(av_hwframe_map(mapped, source, flags) == 0);
    CHECK(mapped->format == format && mapped->width == source->width &&
          mapped->height == source->height);
    const AVFrameSideData *expected = av_frame_get_side_data(source, AV_FRAME_DATA_PANSCAN);
    if (expected) {
        const AVFrameSideData *copied = av_frame_get_side_data(mapped, AV_FRAME_DATA_PANSCAN);
        CHECK(copied && copied->size == expected->size &&
              !memcmp(copied->data, expected->data, expected->size));
        CHECK(mapped->pts == source->pts && mapped->duration == source->duration &&
              mapped->color_primaries == source->color_primaries);
    }
    if (flags & AV_HWFRAME_MAP_READ)
        pattern(mapped, old_seed, true);
    if (flags & AV_HWFRAME_MAP_WRITE)
        pattern(mapped, new_seed, false);
    av_frame_free(&mapped);
    readback(source, format, flags & AV_HWFRAME_MAP_WRITE ? new_seed : old_seed);
}

struct worker {
    AVBufferRef *frames;
    unsigned seed;
};

static void *map_worker(void *opaque)
{
    struct worker *worker = opaque;
    const AVHWFramesContext *context = (void *)worker->frames->data;
    AVFrame *hardware = av_frame_alloc();
    AVFrame *cpu = cpu_frame(context->sw_format, context->width, context->height, worker->seed);
    CHECK(hardware && av_hwframe_get_buffer(worker->frames, hardware, 0) == 0);
    for (unsigned i = 0; i < 12; ++i) {
        pattern(cpu, worker->seed + i, false);
        CHECK(av_hwframe_transfer_data(hardware, cpu, 0) == 0);
        readback(hardware, context->sw_format, worker->seed + i);
    }
    av_frame_free(&cpu);
    av_frame_free(&hardware);
    return nullptr;
}

static void concurrent_maps(AVBufferRef *frames)
{
    pthread_t threads[4];
    struct worker workers[4];
    for (unsigned i = 0; i < 4; ++i) {
        workers[i] = (struct worker){.frames = frames, .seed = 10 + i * 20};
        CHECK(pthread_create(&threads[i], nullptr, map_worker, &workers[i]) == 0);
    }
    for (unsigned i = 0; i < 4; ++i)
        CHECK(pthread_join(threads[i], nullptr) == 0);
}

static void fallback_case(AVBufferRef *device, enum AVPixelFormat format, int width, int height,
                          typeof(fault) mode)
{
    AVBufferRef *frames = av_hwframe_ctx_alloc(device);
    CHECK(frames);
    AVHWFramesContext *context = (void *)frames->data;
    context->format = AV_PIX_FMT_VAAPI;
    context->sw_format = format;
    context->width = width;
    context->height = height;
    CHECK(av_hwframe_ctx_init(frames) == 0);
    AVFrame *hardware = av_frame_alloc();
    CHECK(hardware && av_hwframe_get_buffer(frames, hardware, 0) == 0);
    fault = mode;
    unsigned before = atomic_load_explicit(&derives, memory_order_relaxed);
    mapped_roundtrip(hardware, format, AV_HWFRAME_MAP_WRITE | AV_HWFRAME_MAP_OVERWRITE, 0, 1);
    CHECK(atomic_load_explicit(&derives, memory_order_relaxed) == before + 1);
    /* A stable unsupported/incompatible result is learned once for ordinary
     * writes, including when another surface subsequently could be derived. */
    for (unsigned i = 0; i < 4; ++i)
        mapped_roundtrip(hardware, format, AV_HWFRAME_MAP_WRITE | AV_HWFRAME_MAP_OVERWRITE, 1, 1);
    CHECK(atomic_load_explicit(&derives, memory_order_relaxed) == before + 1);
    AVFrame *direct = av_frame_alloc();
    CHECK(direct);
    direct->format = format;
    CHECK(av_hwframe_map(direct, hardware, AV_HWFRAME_MAP_READ | AV_HWFRAME_MAP_DIRECT) < 0);
    av_frame_free(&direct);
    CHECK(atomic_load_explicit(&images, memory_order_relaxed) == 0);
    fault = NORMAL;
    /* An explicit direct request is never rejected by the cached copy policy. */
    mapped_roundtrip(hardware, format, AV_HWFRAME_MAP_READ | AV_HWFRAME_MAP_DIRECT, 1, 1);
    av_frame_free(&hardware);
    av_buffer_unref(&frames);
    CHECK(atomic_load_explicit(&images, memory_order_relaxed) == 0);
}

static void run(enum AVPixelFormat format, int width, int height, bool lazy)
{
    struct gk_gpu gpu;
    CHECK(gk_gpu_open(&gpu, GK_VAAPI, nullptr) == 0);
    AVBufferRef *frames = av_hwframe_ctx_alloc(gpu.device);
    CHECK(frames);
    AVHWFramesContext *context = (void *)frames->data;
    context->format = AV_PIX_FMT_VAAPI;
    context->sw_format = format;
    context->width = width;
    context->height = height;
    unsigned before = atomic_load_explicit(&derives, memory_order_relaxed);
    CHECK(av_hwframe_ctx_init(frames) == 0);
    CHECK(atomic_load_explicit(&derives, memory_order_relaxed) == before + (lazy ? 0U : 1U));
    AVFrame *hardware = av_frame_alloc();
    CHECK(hardware && av_hwframe_get_buffer(frames, hardware, 0) == 0);
    AVFrame *cpu = cpu_frame(format, width, height, 1);
    CHECK(av_hwframe_transfer_data(hardware, cpu, 0) == 0);
    av_frame_free(&cpu);
    AVFrameSideData *metadata =
        av_frame_new_side_data(hardware, AV_FRAME_DATA_PANSCAN, sizeof(AVPanScan));
    CHECK(metadata);
    AVPanScan *pan = (void *)metadata->data;
    *pan = (AVPanScan){.id = 7, .width = width * 16, .height = height * 16};
    hardware->pts = 123456;
    hardware->duration = 16667;
    hardware->color_primaries = AVCOL_PRI_BT709;
    readback(hardware, format, 1);
    mapped_roundtrip(hardware, format, AV_HWFRAME_MAP_READ | AV_HWFRAME_MAP_DIRECT, 1, 1);
    mapped_roundtrip(hardware, format, AV_HWFRAME_MAP_READ | AV_HWFRAME_MAP_WRITE, 1, 2);
    mapped_roundtrip(hardware, format, AV_HWFRAME_MAP_WRITE | AV_HWFRAME_MAP_OVERWRITE, 2, 3);
    mapped_roundtrip(hardware, format, AV_HWFRAME_MAP_WRITE | AV_HWFRAME_MAP_DIRECT, 3, 4);
    concurrent_maps(frames);

    if (lazy) {
        fallback_case(gpu.device, format, width, height, UNSUPPORTED);
        fallback_case(gpu.device, format, width, height, WRONG_FORMAT);
        fault = FAILED_DESTROY;
        before = atomic_load_explicit(&failed_destroys, memory_order_relaxed);
        AVFrame *mapped = av_frame_alloc();
        CHECK(mapped);
        mapped->format = format;
        CHECK(av_hwframe_map(mapped, hardware, AV_HWFRAME_MAP_WRITE | AV_HWFRAME_MAP_DIRECT) < 0);
        av_frame_free(&mapped);
        CHECK(atomic_load_explicit(&failed_destroys, memory_order_relaxed) == before + 1);
        CHECK(atomic_load_explicit(&images, memory_order_relaxed) == 0);
        fault = NORMAL;
        readback(hardware, format, 4);
    }

    if (lazy)
        concurrent_maps(frames); /* The failed derive selected copied writes. */

    /* Allocated and displayed dimensions may differ. Mapping must retain the
     * displayed extent and its size-dependent metadata, not expose padding. */
    hardware->width = width - 2;
    hardware->height = height - 2;
    pan->width = hardware->width * 16;
    pan->height = hardware->height * 16;
    mapped_roundtrip(hardware, format, AV_HWFRAME_MAP_READ | AV_HWFRAME_MAP_DIRECT, 4, 4);
    hardware->width = width;
    hardware->height = height;
    pan->width = width * 16;
    pan->height = height * 16;

    AVFrame *retained = av_frame_alloc();
    CHECK(retained);
    retained->format = format;
    CHECK(av_hwframe_map(retained, hardware, AV_HWFRAME_MAP_READ | AV_HWFRAME_MAP_DIRECT) == 0);
    CHECK(retained->width == width && retained->height == height && retained->format == format);
    av_frame_free(&hardware);
    av_buffer_unref(&frames);
    gk_gpu_close(&gpu);
    pattern(retained, 4, true); /* The mapping owns the source/context/device lifetime. */
    av_frame_free(&retained);
    CHECK(atomic_load_explicit(&images, memory_order_relaxed) == 0);
    printf("mapping: %s %dx%d transfers, flags%s, concurrency and retained lifetime pass\n",
           format == AV_PIX_FMT_NV12 ? "nv12" : "p010", width, height,
           lazy ? ", injected faults" : "");
}

int main(int argc, char **argv)
{
    CHECK(argc == 2 && (!strcmp(argv[1], "lazy") || !strcmp(argv[1], "eager")));
    bool lazy = !strcmp(argv[1], "lazy");
    for (unsigned bits = 8; bits <= 10; bits += 2) {
        enum AVPixelFormat format = bits == 8 ? AV_PIX_FMT_NV12 : AV_PIX_FMT_P010LE;
        run(format, 320, 240, lazy);
        run(format, 319, 241, lazy);
    }
    printf("mapping: %u derived and %u destroyed images, none retained\n",
           atomic_load_explicit(&derives, memory_order_relaxed),
           atomic_load_explicit(&destroys, memory_order_relaxed));
}

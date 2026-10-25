/* A transfer must report failures that occur while its temporary map closes.
 * Unmap/destroy faults are reported after the real release, so the test does
 * not invent an undocumented resource-lifetime state for the driver. */
#include "gpu.h"

#include <dlfcn.h>
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

static enum { NONE, UNMAP, PUT, DESTROY } fault;
static unsigned injected;

VAStatus vaDeriveImage(VADisplay display, VASurfaceID id, VAImage *image)
{
    (void)display;
    (void)id;
    (void)image;
    return VA_STATUS_ERROR_UNIMPLEMENTED; /* Valid copied-map capability regime. */
}

VAStatus vaUnmapBuffer(VADisplay display, VABufferID id)
{
    REAL(vaUnmapBuffer);
    VAStatus status = call(display, id);
    if (status == VA_STATUS_SUCCESS && fault == UNMAP) {
        ++injected;
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    return status;
}

VAStatus vaPutImage(VADisplay display, VASurfaceID surface, VAImageID image, int sx, int sy,
                    unsigned sw, unsigned sh, int dx, int dy, unsigned dw, unsigned dh)
{
    REAL(vaPutImage);
    if (fault == PUT) {
        ++injected;
        return VA_STATUS_ERROR_OPERATION_FAILED; /* No copy was submitted. */
    }
    return call(display, surface, image, sx, sy, sw, sh, dx, dy, dw, dh);
}

VAStatus vaDestroyImage(VADisplay display, VAImageID id)
{
    REAL(vaDestroyImage);
    VAStatus status = call(display, id);
    if (status == VA_STATUS_SUCCESS && fault == DESTROY) {
        ++injected;
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    return status;
}

static void pixels(AVFrame *frame, unsigned value, bool check)
{
    for (unsigned plane = 0; plane < 2; ++plane) {
        unsigned height = plane ? 120 : 240;
        for (unsigned y = 0; y < height; ++y) {
            uint8_t *row = frame->data[plane] + (size_t)y * (unsigned)frame->linesize[plane];
            for (unsigned x = 0; x < 320; ++x)
                if (check)
                    CHECK(row[x] == value + plane * 29);
                else
                    row[x] = (uint8_t)(value + plane * 29);
        }
    }
}

int main(void)
{
    struct gk_gpu gpu;
    CHECK(gk_gpu_open(&gpu, GK_VAAPI, nullptr) == 0);
    AVBufferRef *frames = av_hwframe_ctx_alloc(gpu.device);
    CHECK(frames);
    AVHWFramesContext *context = (void *)frames->data;
    context->format = AV_PIX_FMT_VAAPI;
    context->sw_format = AV_PIX_FMT_NV12;
    context->width = 320;
    context->height = 240;
    CHECK(av_hwframe_ctx_init(frames) == 0);
    AVFrame *hardware = av_frame_alloc(), *cpu = av_frame_alloc(), *readback = av_frame_alloc();
    CHECK(hardware && cpu && readback && av_hwframe_get_buffer(frames, hardware, 0) == 0);
    cpu->format = readback->format = AV_PIX_FMT_NV12;
    cpu->width = readback->width = 320;
    cpu->height = readback->height = 240;
    CHECK(av_frame_get_buffer(cpu, 32) == 0 && av_frame_get_buffer(readback, 32) == 0);
    bool passed = true;
    for (unsigned mode = UNMAP; mode <= DESTROY; ++mode) {
        pixels(cpu, 35, false);
        CHECK(av_hwframe_transfer_data(hardware, cpu, 0) == 0);
        pixels(cpu, 100, false);
        fault = (typeof(fault))mode;
        unsigned before = injected;
        int ret = av_hwframe_transfer_data(hardware, cpu, 0);
        CHECK(injected == before + 1);
        fault = NONE;
        passed &= ret < 0;
        printf("transfer failure: mode=%u returned=%d\n", mode, ret);
        if (mode == PUT || (mode == UNMAP && ret < 0)) {
            pixels(readback, 200, false);
            CHECK(av_hwframe_transfer_data(readback, hardware, 0) == 0);
            pixels(readback, 35, true);
        }
    }
    for (unsigned mode = UNMAP; mode <= DESTROY; ++mode) {
        if (mode == PUT)
            continue; /* Downloads do not write an image back. */
        AVFrame *clean = av_frame_alloc();
        CHECK(clean);
        fault = (typeof(fault))mode;
        unsigned before = injected;
        int ret = av_hwframe_transfer_data(clean, hardware, 0);
        CHECK(injected == before + 1);
        fault = NONE;
        passed &= ret < 0;
        printf("download failure: mode=%u returned=%d\n", mode, ret);
        if (ret < 0) {
            CHECK(clean->format == AV_PIX_FMT_NONE && !clean->width && !clean->height &&
                  !clean->hw_frames_ctx);
            for (unsigned i = 0; i < AV_NUM_DATA_POINTERS; ++i)
                CHECK(!clean->buf[i] && !clean->data[i]);
        }
        av_frame_free(&clean);
    }
    av_frame_free(&readback);
    av_frame_free(&cpu);
    av_frame_free(&hardware);
    av_buffer_unref(&frames);
    gk_gpu_close(&gpu);
    if (!passed)
        fputs("BUG: a failed map release/writeback was reported as a successful transfer\n",
              stderr);
    return passed ? 0 : 1;
}

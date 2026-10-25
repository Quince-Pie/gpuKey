/* Independent pixel oracle for unit-scale RGB -> NV12/P010 chroma positioning. */
#include "gpu.h"

#include <drm_fourcc.h>
#include <libavutil/intreadwrite.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <va/va_drmcommon.h>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static unsigned value(int x, int y, unsigned channel, unsigned seed)
{
    return ((unsigned)x * (37 + channel * 13) + (unsigned)y * (19 + channel * 7) + seed * 43 +
            channel * 61) %
           256;
}

static int clamp(int x, int n)
{
    return x < 0 ? 0 : x >= n ? n - 1 : x;
}

static double component(int x, int y, unsigned plane, unsigned seed)
{
    double r = value(x, y, 0, seed) / 255.0, g = value(x, y, 1, seed) / 255.0,
           b = value(x, y, 2, seed) / 255.0;
    double l = .2126 * r + .7152 * g + .0722 * b;
    return plane == 0   ? 16 + 219 * l
           : plane == 1 ? 128 + 112 * (b - l) / (1 - .0722)
                        : 128 + 112 * (r - l) / (1 - .2126);
}

static void check_pixels(AVFrame *hardware, int w, int h, unsigned bits, unsigned seed)
{
    AVFrame *raw = av_frame_alloc();
    CHECK(raw && av_hwframe_transfer_data(raw, hardware, 0) == 0);
    CHECK(raw->width == w && raw->height == h);
    CHECK(raw->format == (bits == 8 ? AV_PIX_FMT_NV12 : AV_PIX_FMT_P010LE));
    double scale = bits == 8 ? 1.0 : 4.0;
    for (unsigned plane = 0; plane < 3; ++plane) {
        int width = plane ? (w + 1) / 2 : w, height = plane ? (h + 1) / 2 : h;
        for (int y = 0; y < height; ++y) {
            const uint8_t *row =
                raw->data[plane ? 1 : 0] + (ptrdiff_t)y * raw->linesize[plane ? 1 : 0];
            for (int x = 0; x < width; ++x) {
                double expected = 0;
                if (!plane)
                    expected = component(x, y, 0, seed);
                else
                    for (int yy = 0; yy < 2; ++yy)
                        for (int xx = -1; xx <= 1; ++xx)
                            expected +=
                                (xx == 0 ? .25 : .125) *
                                component(clamp(2 * x + xx, w), clamp(2 * y + yy, h), plane, seed);
                unsigned index = plane ? 2 * (unsigned)x + plane - 1 : (unsigned)x;
                unsigned actual = bits == 8 ? row[index] : AV_RL16(row + 2 * index) >> 6;
                double error = fabs(actual - expected * scale);
                if (error > (bits == 8 ? .55 : 1.1)) {
                    fprintf(stderr, "chroma %dx%d %u-bit seed=%u p=%u (%d,%d): %u expected %.6f\n",
                            w, h, bits, seed, plane, x, y, actual, expected * scale);
                    exit(1);
                }
            }
        }
    }
    av_frame_free(&raw);
}

static void run(struct gk_gpu *gpu, int w, int h, unsigned fourcc, bool imported)
{
    bool rgb = fourcc == VA_FOURCC_RGBA || fourcc == VA_FOURCC_RGBX;
    VASurfaceAttrib attr = {.type = VASurfaceAttribPixelFormat,
                            .flags = VA_SURFACE_ATTRIB_SETTABLE,
                            .value = {.type = VAGenericValueTypeInteger, .value.i = (int)fourcc}};
    VASurfaceID source, read = VA_INVALID_ID;
    CHECK(vaCreateSurfaces(gpu->display, VA_RT_FORMAT_RGB32, (unsigned)w, (unsigned)h, &source, 1,
                           &attr, 1) == VA_STATUS_SUCCESS);
    VAImageFormat vf = {.fourcc = fourcc,
                        .byte_order = VA_LSB_FIRST,
                        .bits_per_pixel = 32,
                        .depth = 32,
                        .red_mask = rgb ? 0xff : 0xff0000,
                        .green_mask = 0xff00,
                        .blue_mask = rgb ? 0xff0000 : 0xff,
                        .alpha_mask = 0xff000000};
    VAImage image;
    CHECK(vaCreateImage(gpu->display, &vf, w, h, &image) == VA_STATUS_SUCCESS);
    struct gk_video_format format = {.width = w,
                                     .height = h,
                                     .pixel_format = rgb ? AV_PIX_FMT_RGBA : AV_PIX_FMT_BGRA,
                                     .transfer = AVCOL_TRC_IEC61966_2_1,
                                     .dmabuf = true};
    VADRMPRIMESurfaceDescriptor descriptor = {};
    if (imported) {
        CHECK(vaExportSurfaceHandle(gpu->display, source, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                    VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_COMPOSED_LAYERS,
                                    &descriptor) == VA_STATUS_SUCCESS);
        CHECK(descriptor.num_objects == 1 && descriptor.num_layers == 1 &&
              descriptor.layers[0].num_planes == 1);
        format.modifier = descriptor.objects[0].drm_format_modifier;
        format.drm_format = descriptor.layers[0].drm_format;
        struct gk_image external = {.plane_count = 1,
                                    .planes = {{.fd = descriptor.objects[0].fd,
                                                .size = descriptor.objects[0].size,
                                                .offset = descriptor.layers[0].offset[0],
                                                .pitch = descriptor.layers[0].pitch[0]}}};
        CHECK(gk_gpu_import(gpu, &format, &external, &read) == 0);
    }
    for (unsigned bits = 8; bits <= 10; bits += 2) {
        AVBufferRef *frames = av_hwframe_ctx_alloc(gpu->device);
        CHECK(frames);
        AVHWFramesContext *fc = (void *)frames->data;
        fc->format = AV_PIX_FMT_VAAPI;
        fc->sw_format = bits == 8 ? AV_PIX_FMT_NV12 : AV_PIX_FMT_P010LE;
        fc->width = w;
        fc->height = h;
        CHECK(av_hwframe_ctx_init(frames) == 0);
        AVFrame *target = av_frame_alloc();
        CHECK(target && av_hwframe_get_buffer(frames, target, 0) == 0);
        for (unsigned seed = 0; seed < 2; ++seed) {
            uint8_t *pixels;
            CHECK(vaMapBuffer(gpu->display, image.buf, (void **)&pixels) == VA_STATUS_SUCCESS);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    uint8_t *p =
                        pixels + image.offsets[0] + (size_t)y * image.pitches[0] + (size_t)x * 4;
                    p[rgb ? 0 : 2] = (uint8_t)value(x, y, 0, seed);
                    p[1] = (uint8_t)value(x, y, 1, seed);
                    p[rgb ? 2 : 0] = (uint8_t)value(x, y, 2, seed);
                    p[3] = 255;
                }
            CHECK(vaUnmapBuffer(gpu->display, image.buf) == VA_STATUS_SUCCESS);
            CHECK(vaPutImage(gpu->display, source, image.image_id, 0, 0, (unsigned)w, (unsigned)h,
                             0, 0, (unsigned)w, (unsigned)h) == VA_STATUS_SUCCESS);
            CHECK(vaSyncSurface(gpu->display, source) == VA_STATUS_SUCCESS);
            CHECK(gk_gpu_convert_submit(gpu, &format, imported ? read : source, target) == 0);
            CHECK(gk_gpu_convert_wait(gpu, target) == 0);
            check_pixels(target, w, h, bits, seed);
        }
        av_frame_free(&target);
        av_buffer_unref(&frames);
    }
    if (imported) {
        gk_gpu_unimport(gpu, &read);
        CHECK(close(descriptor.objects[0].fd) == 0);
    }
    CHECK(vaDestroyImage(gpu->display, image.image_id) == VA_STATUS_SUCCESS);
    CHECK(vaDestroySurfaces(gpu->display, &source, 1) == VA_STATUS_SUCCESS);
}

int main(void)
{
    struct gk_gpu gpu;
    CHECK(gk_gpu_open(&gpu, GK_VAAPI, nullptr) == 0 && gk_gpu_configure(&gpu, 1024, 768) == 0);
    static const int sizes[][2] = {{641, 481}, {1, 1}, {3, 5}, {17, 19}, {78, 66}, {80, 64}};
    static const unsigned formats[] = {VA_FOURCC_BGRA, VA_FOURCC_BGRX, VA_FOURCC_RGBA,
                                       VA_FOURCC_RGBX};
    for (unsigned s = 0; s < sizeof(sizes) / sizeof(sizes[0]); ++s)
        for (unsigned f = 0; f < sizeof(formats) / sizeof(formats[0]); ++f)
            for (unsigned imported = 0; imported < 2; ++imported)
                run(&gpu, sizes[s][0], sizes[s][1], formats[f], imported != 0);
    gk_gpu_close(&gpu);
    puts("GPU chroma: independent positioning/range oracle, four RGB formats, both depths, "
         "direct/imported and partial tiles pass");
}

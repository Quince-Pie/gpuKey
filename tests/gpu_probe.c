#include "media.h"

#include <drm_fourcc.h>
#include <errno.h>
#include <libavutil/hwcontext.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <va/va_drmcommon.h>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static void probe(unsigned bits, unsigned depth)
{
    struct gk_gpu gpu;
    CHECK(gk_gpu_open(&gpu, GK_VAAPI, nullptr) == 0);
    VASurfaceID source;
    VASurfaceAttrib attr = {
        .type = VASurfaceAttribPixelFormat,
        .flags = VA_SURFACE_ATTRIB_SETTABLE,
        .value = {.type = VAGenericValueTypeInteger, .value.i = VA_FOURCC_BGRA}};
    CHECK(vaCreateSurfaces(gpu.display, VA_RT_FORMAT_RGB32, 320, 240, &source, 1, &attr, 1) ==
          VA_STATUS_SUCCESS);
    VAImageFormat vf = {.fourcc = VA_FOURCC_BGRA,
                        .byte_order = VA_LSB_FIRST,
                        .bits_per_pixel = 32,
                        .depth = 32,
                        .red_mask = 0x00ff0000,
                        .green_mask = 0x0000ff00,
                        .blue_mask = 0xff,
                        .alpha_mask = 0xff000000};
    VAImage upload;
    CHECK(vaCreateImage(gpu.display, &vf, 320, 240, &upload) == VA_STATUS_SUCCESS);
    VADRMPRIMESurfaceDescriptor desc;
    CHECK(vaExportSurfaceHandle(gpu.display, source, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_COMPOSED_LAYERS,
                                &desc) == VA_STATUS_SUCCESS);
    CHECK(desc.num_layers == 1 && desc.num_objects <= 4);
    struct gk_video_format format = {
        .width = 320,
        .height = 240,
        .pixel_format = AV_PIX_FMT_BGRA,
        .transfer = AVCOL_TRC_IEC61966_2_1,
        .dmabuf = true,
        .modifier = desc.objects[0].drm_format_modifier,
        .drm_format = desc.layers[0].drm_format,
    };
    struct gk_image image = {.plane_count = desc.layers[0].num_planes};
    for (unsigned i = 0; i < image.plane_count; ++i) {
        unsigned object = desc.layers[0].object_index[i];
        image.planes[i] = (struct gk_plane){.fd = desc.objects[object].fd,
                                            .size = desc.objects[object].size,
                                            .offset = desc.layers[0].offset[i],
                                            .pitch = desc.layers[0].pitch[i]};
    }
    VASurfaceID imported;
    CHECK(gk_gpu_import(&gpu, &format, &image, &imported) == 0);
    char path[] = "/tmp/gpukey-gpu-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0 && close(fd) == 0 && unlink(path) == 0);
    struct gk_media_options options = {
        .output = path,
        .bits = bits,
        .encode_depth = depth,
        .qp = 32,
        .fps_num = 60,
        .fps_den = 1,
        .sink = {.slab_size = 65536, .slabs = 8, .stall_ms = 5000},
    };
    const char *compact = getenv("GPUKEY_TEST_COMPACT");
    options.compact_gpu = compact && !strcmp(compact, "1");
    struct gk_media *media;
    CHECK(gk_media_open(&media, &gpu, &format, &options) == 0);
    static const uint8_t colors[][3] = {
        {0, 0, 0},   {255, 255, 255}, {255, 0, 0},    {0, 255, 0},
        {0, 0, 255}, {128, 128, 128}, {12, 143, 231},
    };
    double worst = 0;
    unsigned count = sizeof(colors) / sizeof(colors[0]);
    for (unsigned i = 0; i < count; ++i) {
        uint8_t *pixels;
        CHECK(vaMapBuffer(gpu.display, upload.buf, (void **)&pixels) == VA_STATUS_SUCCESS);
        for (unsigned y = 0; y < 240; ++y)
            for (unsigned x = 0; x < 320; ++x) {
                uint8_t *p = pixels + upload.offsets[0] + y * upload.pitches[0] + x * 4;
                p[0] = colors[i][2];
                p[1] = colors[i][1];
                p[2] = colors[i][0];
                p[3] = 255;
            }
        CHECK(vaUnmapBuffer(gpu.display, upload.buf) == VA_STATUS_SUCCESS);
        CHECK(vaPutImage(gpu.display, source, upload.image_id, 0, 0, 320, 240, 0, 0, 320, 240) ==
              VA_STATUS_SUCCESS);
        CHECK(vaSyncSurface(gpu.display, source) == VA_STATUS_SUCCESS);
        AVFrame *hardware;
        CHECK(gk_media_prepare(media, &image, imported, &hardware) == 0);
        AVFrame *decoded = av_frame_alloc();
        CHECK(decoded != nullptr);
        CHECK(av_hwframe_transfer_data(decoded, hardware, 0) == 0);
        double r = colors[i][0] / 255.0, g = colors[i][1] / 255.0, b = colors[i][2] / 255.0;
        double luma = .2126 * r + .7152 * g + .0722 * b;
        double scale = bits == 10 ? 4.0 : 1.0;
        double expected[] = {(16 + 219 * luma) * scale,
                             (128 + 112 * (b - luma) / (1 - .0722)) * scale,
                             (128 + 112 * (r - luma) / (1 - .2126)) * scale};
        double actual[3];
        if (bits == 8) {
            actual[0] = decoded->data[0][8 * decoded->linesize[0] + 8];
            actual[1] = decoded->data[1][4 * decoded->linesize[1] + 8];
            actual[2] = decoded->data[1][4 * decoded->linesize[1] + 9];
        } else {
            uint16_t *y = (void *)(decoded->data[0] + 8 * decoded->linesize[0]);
            uint16_t *uv = (void *)(decoded->data[1] + 4 * decoded->linesize[1]);
            /* libva's VA_FOURCC_P010 contract explicitly ignores the bottom
             * six bits (va.h); native VPP need not zero them. Test the ten
             * meaningful bits, rather than impose FFmpeg's CPU padding rule. */
            actual[0] = y[8] >> 6;
            actual[1] = uv[8] >> 6;
            actual[2] = uv[9] >> 6;
        }
        for (unsigned k = 0; k < 3; ++k) {
            double error = fabs(actual[k] - expected[k]);
            if (error > worst)
                worst = error;
            if (error > (bits == 8 ? 1.5 : 4.0))
                fprintf(stderr, "%u-bit color %u component %u: got %.0f expected %.4f\n", bits, i,
                        k, actual[k], expected[k]);
            CHECK(error <= (bits == 8 ? 1.5 : 4.0));
        }
        av_frame_free(&decoded);
        CHECK(gk_media_submit(media, hardware, (int64_t)i * GK_NS / 60) == 0);
        if (depth == 1) {
            /* Even the in-place path must refuse reuse while another owner
             * pins its input. Refcount exclusivity follows actual ownership,
             * independently of the VAAPI handle's read-only byte flag. */
            AVFrame *pin = av_frame_clone(hardware), *blocked = nullptr;
            CHECK(pin);
            CHECK(gk_media_prepare(media, &image, imported, &blocked) == -EAGAIN);
            CHECK(!blocked);
            av_frame_free(&pin);
        }
    }
    struct gk_media_stats stats;
    CHECK(gk_media_close(&media, (int64_t)count * GK_NS / 60, &stats) == 0);
    CHECK(stats.encoder_inputs == depth && stats.encode_depth == depth);
    CHECK(stats.submitted == count && stats.packets == count);
    CHECK(unlink(path) == 0);
    gk_gpu_unimport(&gpu, &imported);
    vaDestroyImage(gpu.display, upload.image_id);
    vaDestroySurfaces(gpu.display, &source, 1);
    for (unsigned i = 0; i < desc.num_objects; ++i)
        close(desc.objects[i].fd);
    gk_gpu_close(&gpu);
    printf("GPU color: %u-bit, depth %u, persistent modifier import, seven RGB/YUV endpoints; "
           "max error %.4f codes\n",
           bits, depth, worst);
}

int main(void)
{
    probe(8, 2);
    probe(10, 2);
    probe(8, 1);
    probe(10, 1);
    return 0;
}

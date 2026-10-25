/* Differential raw-image probe for the optional driver, including transitions
 * between its optimized conversion and the general VPP path. Run the same
 * executable against both drivers and compare every dumped frame. */
#include "gpu.h"

#include <libavutil/hwcontext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <va/va_vpp.h>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static void dump(AVFrame *hardware, const char *directory, unsigned geometry, unsigned bits,
                 unsigned step)
{
    AVFrame *frame = av_frame_alloc();
    CHECK(frame && av_hwframe_transfer_data(frame, hardware, 0) == 0);
    char path[4096];
    int length = snprintf(path, sizeof(path), "%s/%u-%u-%02u.yuv", directory, geometry, bits, step);
    CHECK(length > 0 && (size_t)length < sizeof(path));
    FILE *file = fopen(path, "wbx");
    CHECK(file);
    size_t row_bytes = (unsigned)frame->width * (bits == 8 ? 1U : 2U);
    for (unsigned plane = 0; plane < 2; ++plane)
        for (unsigned y = 0; y < (unsigned)frame->height / (plane ? 2U : 1U); ++y)
            CHECK(fwrite(frame->data[plane] + y * (unsigned)frame->linesize[plane], 1, row_bytes,
                         file) == row_bytes);
    CHECK(fclose(file) == 0);
    av_frame_free(&frame);
}

int main(int argc, char **argv)
{
    CHECK(argc == 2 || (argc == 3 && !strcmp(argv[2], "--expect-failure")));
    bool expect_failure = argc == 3;
    unsigned failures = 0;
    struct gk_gpu gpu;
    CHECK(gk_gpu_open(&gpu, GK_VAAPI, nullptr) == 0);
    CHECK(gk_gpu_configure(&gpu, 512, 256) == 0);
    static const unsigned dimensions[][2] = {{80, 64}, {78, 66}, {512, 256}};
    static const unsigned formats[] = {VA_FOURCC_BGRA, VA_FOURCC_BGRX, VA_FOURCC_RGBA,
                                       VA_FOURCC_RGBX};
    static const char *const cases[] = {"normal",
                                        "new-source-content",
                                        "full-range",
                                        "normal",
                                        "top-chroma",
                                        "normal",
                                        "rotate-180",
                                        "normal",
                                        "crop-scale",
                                        "normal",
                                        "global-alpha",
                                        "normal",
                                        "premultiplied-alpha",
                                        "normal",
                                        "transfer-change",
                                        "normal",
                                        "primaries-change",
                                        "normal",
                                        "center-bottom-chroma",
                                        "normal",
                                        "matrix-601",
                                        "normal",
                                        "limited-rgb",
                                        "normal"};
    for (unsigned geometry = 0; geometry < 3; ++geometry) {
        unsigned width = dimensions[geometry][0], height = dimensions[geometry][1];
        for (unsigned kind = 0; kind < 4; ++kind) {
            VASurfaceAttrib attribute = {
                .type = VASurfaceAttribPixelFormat,
                .flags = VA_SURFACE_ATTRIB_SETTABLE,
                .value = {.type = VAGenericValueTypeInteger, .value.i = (int)formats[kind]}};
            VASurfaceID source;
            CHECK(vaCreateSurfaces(gpu.display, VA_RT_FORMAT_RGB32, width, height, &source, 1,
                                   &attribute, 1) == VA_STATUS_SUCCESS);
            VAImageFormat vf = {.fourcc = VA_FOURCC_BGRA,
                                .byte_order = VA_LSB_FIRST,
                                .bits_per_pixel = 32,
                                .depth = 32,
                                .red_mask = 0xff0000,
                                .green_mask = 0xff00,
                                .blue_mask = 0xff,
                                .alpha_mask = 0xff000000};
            VAImage upload;
            CHECK(vaCreateImage(gpu.display, &vf, (int)width, (int)height, &upload) ==
                  VA_STATUS_SUCCESS);
            for (unsigned bits = 8; bits <= 10; bits += 2) {
                AVBufferRef *frames = av_hwframe_ctx_alloc(gpu.device);
                CHECK(frames);
                AVHWFramesContext *context = (void *)frames->data;
                context->format = AV_PIX_FMT_VAAPI;
                context->sw_format = bits == 8 ? AV_PIX_FMT_NV12 : AV_PIX_FMT_P010LE;
                context->width = (int)width;
                context->height = (int)height;
                CHECK(av_hwframe_ctx_init(frames) == 0);
                AVFrame *destination = av_frame_alloc();
                CHECK(destination && av_hwframe_get_buffer(frames, destination, 0) == 0);
                VASurfaceID target = (VASurfaceID)(uintptr_t)destination->data[3];
                for (unsigned step = 0; step < sizeof(cases) / sizeof(cases[0]); ++step) {
                    uint8_t *pixels;
                    CHECK(vaMapBuffer(gpu.display, upload.buf, (void **)&pixels) ==
                          VA_STATUS_SUCCESS);
                    for (unsigned y = 0; y < height; ++y)
                        for (unsigned x = 0; x < width; ++x) {
                            uint8_t *p = pixels + upload.offsets[0] + y * upload.pitches[0] + x * 4;
                            unsigned alpha = (x + y + step) % 7 ? 128U : 0U;
                            p[3] = (uint8_t)alpha;
                            for (unsigned channel = 0; channel < 3; ++channel)
                                p[channel] =
                                    (uint8_t)((x * (11 + channel * 3) + y * 7 + step * 31) %
                                              (alpha + 1));
                        }
                    CHECK(vaUnmapBuffer(gpu.display, upload.buf) == VA_STATUS_SUCCESS);
                    CHECK(vaPutImage(gpu.display, source, upload.image_id, 0, 0, width, height, 0,
                                     0, width, height) == VA_STATUS_SUCCESS);
                    CHECK(vaSyncSurface(gpu.display, source) == VA_STATUS_SUCCESS);
                    VAProcPipelineParameterBuffer params = {
                        .surface = source,
                        .surface_color_standard = VAProcColorStandardExplicit,
                        .output_color_standard = VAProcColorStandardExplicit,
                        .input_color_properties = {.color_range = VA_SOURCE_RANGE_FULL,
                                                   .colour_primaries = AVCOL_PRI_BT709,
                                                   .transfer_characteristics =
                                                       AVCOL_TRC_IEC61966_2_1,
                                                   .matrix_coefficients = AVCOL_SPC_RGB},
                        .output_color_properties = {.color_range = VA_SOURCE_RANGE_REDUCED,
                                                    .colour_primaries = AVCOL_PRI_BT709,
                                                    .transfer_characteristics =
                                                        AVCOL_TRC_IEC61966_2_1,
                                                    .matrix_coefficients = AVCOL_SPC_BT709,
                                                    .chroma_sample_location =
                                                        VA_CHROMA_SITING_HORIZONTAL_LEFT |
                                                        VA_CHROMA_SITING_VERTICAL_CENTER},
                    };
                    VARectangle crop = {.x = 2,
                                        .y = 2,
                                        .width = (uint16_t)(width - 4),
                                        .height = (uint16_t)(height - 4)};
                    VABlendState blend = {.flags = VA_BLEND_GLOBAL_ALPHA, .global_alpha = .375f};
                    switch (step) {
                    case 2:
                        params.output_color_properties.color_range = VA_SOURCE_RANGE_FULL;
                        break;
                    case 4:
                        params.output_color_properties.chroma_sample_location =
                            VA_CHROMA_SITING_HORIZONTAL_LEFT | VA_CHROMA_SITING_VERTICAL_TOP;
                        break;
                    case 6:
                        params.rotation_state = VA_ROTATION_180;
                        break;
                    case 8:
                        params.surface_region = &crop;
                        break;
                    case 10:
                        params.blend_state = &blend;
                        break;
                    case 12:
                        blend.flags = VA_BLEND_PREMULTIPLIED_ALPHA;
                        params.blend_state = &blend;
                        break;
                    case 14:
                        params.output_color_properties.transfer_characteristics = AVCOL_TRC_BT709;
                        break;
                    case 16:
                        params.output_color_properties.colour_primaries = AVCOL_PRI_BT2020;
                        params.output_color_properties.matrix_coefficients = AVCOL_SPC_BT2020_NCL;
                        break;
                    case 18:
                        params.output_color_properties.chroma_sample_location =
                            VA_CHROMA_SITING_HORIZONTAL_CENTER | VA_CHROMA_SITING_VERTICAL_BOTTOM;
                        break;
                    case 20:
                        params.output_color_properties.matrix_coefficients = AVCOL_SPC_SMPTE170M;
                        break;
                    case 22:
                        params.input_color_properties.color_range = VA_SOURCE_RANGE_REDUCED;
                        break;
                    default:
                        break;
                    }
                    void *mapped;
                    CHECK(vaMapBuffer(gpu.display, gpu.vpp_parameters, &mapped) ==
                          VA_STATUS_SUCCESS);
                    memcpy(mapped, &params, sizeof(params));
                    CHECK(vaUnmapBuffer(gpu.display, gpu.vpp_parameters) == VA_STATUS_SUCCESS);
                    CHECK(vaBeginPicture(gpu.display, gpu.vpp_context, target) ==
                          VA_STATUS_SUCCESS);
                    CHECK(vaRenderPicture(gpu.display, gpu.vpp_context, &gpu.vpp_parameters, 1) ==
                          VA_STATUS_SUCCESS);
                    VAStatus status = vaEndPicture(gpu.display, gpu.vpp_context);
                    CHECK(vaSyncSurface(gpu.display, target) == VA_STATUS_SUCCESS);
                    if (status != VA_STATUS_SUCCESS) {
                        CHECK(expect_failure && !failures);
                        ++failures;
                        fprintf(stderr, "expected VPP failure: %u-%u-%02u\n", geometry * 4 + kind,
                                bits, step);
                        continue;
                    }
                    dump(destination, argv[1], geometry * 4 + kind, bits, step);
                    printf("%ux%u format-%u %u-bit %u %s\n", width, height, kind, bits, step,
                           cases[step]);
                }
                av_frame_free(&destination);
                av_buffer_unref(&frames);
            }
            CHECK(vaDestroyImage(gpu.display, upload.image_id) == VA_STATUS_SUCCESS);
            CHECK(vaDestroySurfaces(gpu.display, &source, 1) == VA_STATUS_SUCCESS);
        }
    }
    gk_gpu_close(&gpu);
    CHECK(failures == (expect_failure ? 1U : 0U));
    return 0;
}

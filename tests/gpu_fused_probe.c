/* A decisive color/eligibility probe, not a live recording implementation.
 * One immutable RGB source remains owned through encoder drain. The optional
 * diagnostic interposer changes only the VA submission path. */
#include "media.h"

#include <dlfcn.h>
#include <drm_fourcc.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avstring.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>
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

enum { WIDTH = 512, HEIGHT = 256 };
static const uint8_t colors[8][3] = {{0, 0, 0},   {255, 255, 255}, {255, 0, 0},    {0, 255, 0},
                                     {0, 0, 255}, {128, 128, 128}, {12, 143, 231}, {225, 37, 90}};

int main(int argc, char **argv)
{
    CHECK((argc == 4 || (argc == 5 && !strcmp(argv[4], "--raw"))) &&
          (!strcmp(argv[2], "native") || !strcmp(argv[2], "linear")));
    CHECK(argc != 5 || !dlsym(RTLD_DEFAULT, "gpukey_fused_probe_active"));
    unsigned bits = !strcmp(argv[3], "8") ? 8U : !strcmp(argv[3], "10") ? 10U : 0U;
    CHECK(bits);
    struct gk_gpu gpu;
    CHECK(gk_gpu_open(&gpu, GK_VAAPI, nullptr) == 0);
    uint64_t modifier = DRM_FORMAT_MOD_LINEAR;
    VADRMFormatModifierList modifiers = {.num_modifiers = 1, .modifiers = &modifier};
    VASurfaceAttrib attrs[] = {
        {.type = VASurfaceAttribPixelFormat,
         .flags = VA_SURFACE_ATTRIB_SETTABLE,
         .value = {.type = VAGenericValueTypeInteger, .value.i = VA_FOURCC_BGRA}},
        {.type = VASurfaceAttribDRMFormatModifiers,
         .flags = VA_SURFACE_ATTRIB_SETTABLE,
         .value = {.type = VAGenericValueTypePointer, .value.p = &modifiers}}};
    VASurfaceID source;
    CHECK(vaCreateSurfaces(gpu.display, VA_RT_FORMAT_RGB32, WIDTH, HEIGHT, &source, 1, attrs,
                           !strcmp(argv[2], "linear") ? 2 : 1) == VA_STATUS_SUCCESS);
    VAImageFormat vf = {.fourcc = VA_FOURCC_BGRA,
                        .byte_order = VA_LSB_FIRST,
                        .bits_per_pixel = 32,
                        .depth = 32,
                        .red_mask = 0xff0000,
                        .green_mask = 0xff00,
                        .blue_mask = 0xff,
                        .alpha_mask = 0xff000000};
    VAImage upload;
    CHECK(vaCreateImage(gpu.display, &vf, WIDTH, HEIGHT, &upload) == VA_STATUS_SUCCESS);
    uint8_t *pixels;
    CHECK(vaMapBuffer(gpu.display, upload.buf, (void **)&pixels) == VA_STATUS_SUCCESS);
    for (unsigned y = 0; y < HEIGHT; ++y)
        for (unsigned x = 0; x < WIDTH; ++x) {
            uint8_t rgb[3];
            if (y < HEIGHT / 2)
                memcpy(rgb, colors[x / 64], sizeof(rgb));
            else if (y < HEIGHT * 3 / 4) {
                unsigned band = (y - HEIGHT / 2) / 16;
                for (unsigned i = 0; i < 3; ++i)
                    rgb[i] = band == 3 || band == i ? (uint8_t)x : 0;
            } else {
                rgb[0] = x & 1 ? 255 : 0;
                rgb[1] = y & 1 ? 255 : 0;
                rgb[2] = x & 1 ? 0 : 255;
            }
            uint8_t *p = pixels + upload.offsets[0] + y * upload.pitches[0] + x * 4;
            p[0] = rgb[2];
            p[1] = rgb[1];
            p[2] = rgb[0];
            p[3] = 255;
        }
    CHECK(vaUnmapBuffer(gpu.display, upload.buf) == VA_STATUS_SUCCESS);
    CHECK(vaPutImage(gpu.display, source, upload.image_id, 0, 0, WIDTH, HEIGHT, 0, 0, WIDTH,
                     HEIGHT) == VA_STATUS_SUCCESS);
    CHECK(vaSyncSurface(gpu.display, source) == VA_STATUS_SUCCESS);
    VADRMPRIMESurfaceDescriptor descriptor;
    CHECK(vaExportSurfaceHandle(gpu.display, source, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_COMPOSED_LAYERS,
                                &descriptor) == VA_STATUS_SUCCESS);
    CHECK(descriptor.num_objects == 1 && descriptor.num_layers == 1);
    fprintf(stderr, "probe: modifier=0x%016llx bits=%u\n",
            (unsigned long long)descriptor.objects[0].drm_format_modifier, bits);
    struct gk_video_format format = {.width = WIDTH,
                                     .height = HEIGHT,
                                     .pixel_format = AV_PIX_FMT_BGRA,
                                     .transfer = AVCOL_TRC_IEC61966_2_1,
                                     .dmabuf = true};
    struct gk_media_options options = {.output = argv[1],
                                       .matroska = true,
                                       .bits = bits,
                                       .encode_depth = 1,
                                       /* The pinned FFmpeg AV1 VAAPI path rejects Q=0's
                                        * inferred lossless transform mode. Q=1 is a near-
                                        * lossless comparison, not an exact conversion oracle. */
                                       .qp = 1,
                                       .fps_num = 60,
                                       .fps_den = 1,
                                       .sink = {.slabs = 8, .slab_size = 65536, .stall_ms = 5000}};
    struct gk_media *media;
    CHECK(gk_media_open(&media, &gpu, &format, &options) == 0);
    struct gk_image image = {};
    AVFrame *frame;
    CHECK(gk_media_prepare(media, &image, source, &frame) == 0);
    if (argc == 5) {
        /* Only the ordinary VPP path has materialized YUV before encoding.
         * Use this exact pre-codec oracle for compute-kernel comparisons. */
        AVFrame *raw = av_frame_alloc();
        CHECK(raw && av_hwframe_transfer_data(raw, frame, 0) == 0);
        char *path = av_asprintf("%s.yuv", argv[1]);
        CHECK(path);
        FILE *output_file = fopen(path, "wbx");
        CHECK(output_file);
        size_t row_bytes = WIDTH * (bits == 8 ? 1U : 2U);
        for (unsigned plane = 0; plane < 2; ++plane)
            for (unsigned row = 0; row < (plane ? HEIGHT / 2 : HEIGHT); ++row)
                CHECK(fwrite(raw->data[plane] + row * (unsigned)raw->linesize[plane], 1, row_bytes,
                             output_file) == row_bytes);
        CHECK(fclose(output_file) == 0);
        av_free(path);
        av_frame_free(&raw);
    }
    CHECK(gk_media_submit(media, frame, 0) == 0);
    CHECK(gk_media_close(&media, GK_NS, nullptr) == 0);
    /* The native source has not been reused, changed or released during encode. */
    CHECK(vaDestroyImage(gpu.display, upload.image_id) == VA_STATUS_SUCCESS);
    CHECK(vaDestroySurfaces(gpu.display, &source, 1) == VA_STATUS_SUCCESS);
    close(descriptor.objects[0].fd);
    gk_gpu_close(&gpu);
    AVFormatContext *file = nullptr;
    CHECK(avformat_open_input(&file, argv[1], nullptr, nullptr) == 0);
    CHECK(avformat_find_stream_info(file, nullptr) == 0);
    AVCodecContext *decoder = avcodec_alloc_context3(avcodec_find_decoder_by_name("libdav1d"));
    CHECK(decoder && avcodec_parameters_to_context(decoder, file->streams[0]->codecpar) == 0);
    decoder->thread_count = 1;
    CHECK(avcodec_open2(decoder, nullptr, nullptr) == 0);
    AVPacket *packet = av_packet_alloc();
    AVFrame *decoded = av_frame_alloc();
    CHECK(packet && decoded && av_read_frame(file, packet) == 0);
    CHECK(avcodec_send_packet(decoder, packet) == 0);
    int ret = avcodec_receive_frame(decoder, decoded);
    if (ret == AVERROR(EAGAIN)) {
        CHECK(avcodec_send_packet(decoder, nullptr) == 0);
        ret = avcodec_receive_frame(decoder, decoded);
    }
    CHECK(ret == 0 && decoded->width == WIDTH && decoded->height == HEIGHT);
    CHECK(av_pix_fmt_desc_get(decoded->format)->comp[0].depth == (int)bits);
    for (unsigned color = 0; color < 8; ++color) {
        printf("color %u:", color);
        for (unsigned plane = 0; plane < 3; ++plane) {
            unsigned x = (color * 64 + 16) >> (plane != 0), y = 32U >> (plane != 0);
            const uint8_t *row = decoded->data[plane] + y * (unsigned)decoded->linesize[plane];
            unsigned value = bits == 8 ? row[x] : ((const uint16_t *)row)[x];
            printf(" %u", value);
        }
        putchar('\n');
    }
    av_frame_free(&decoded);
    av_packet_free(&packet);
    avcodec_free_context(&decoder);
    avformat_close_input(&file);
    return 0;
}

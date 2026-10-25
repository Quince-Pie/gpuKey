/* Compare conversion semantics and complete CPU conversion costs before
 * selecting an integration. Output is raw NV12/P010 for independent analysis. */
#include "convert.h"
#include "core.h"

#include <errno.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static int positive(const char *text)
{
    char *end;
    errno = 0;
    long value = strtol(text, &end, 10);
    CHECK(!errno && *text && !*end && value > 0 && value <= INT_MAX);
    return (int)value;
}

static int64_t cpu_ns(void)
{
    struct timespec ts;
    CHECK(clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) == 0);
    return (int64_t)ts.tv_sec * GK_NS + ts.tv_nsec;
}

static AVFrame *frame(enum AVPixelFormat format, int width, int height, bool rgb)
{
    AVFrame *f = av_frame_alloc();
    CHECK(f);
    f->format = format;
    f->width = width;
    f->height = height;
    f->color_primaries = AVCOL_PRI_BT709;
    f->color_trc = AVCOL_TRC_IEC61966_2_1;
    f->colorspace = rgb ? AVCOL_SPC_RGB : AVCOL_SPC_BT709;
    f->color_range = rgb ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
    f->chroma_location = rgb ? AVCHROMA_LOC_UNSPECIFIED : AVCHROMA_LOC_LEFT;
    CHECK(av_frame_get_buffer(f, 64) == 0);
    return f;
}

static struct SwsContext *context(const AVFrame *src, const AVFrame *dst, unsigned variant)
{
    if (variant >= 5)
        return nullptr;
    struct SwsContext *sws = sws_alloc_context();
    CHECK(sws);
    unsigned flags = SWS_BILINEAR | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT;
    if (variant == 1 || variant >= 3)
        flags |= SWS_FULL_CHR_H_INP;
    if (variant >= 4)
        flags |= SWS_UNSTABLE;
    CHECK(av_opt_set_int(sws, "sws_flags", flags, 0) == 0);
    CHECK(av_opt_set_int(sws, "threads", 1, 0) == 0);
    if (variant >= 2)
        return sws;
    CHECK(av_opt_set_int(sws, "srcw", src->width, 0) == 0);
    CHECK(av_opt_set_int(sws, "srch", src->height, 0) == 0);
    CHECK(av_opt_set_int(sws, "dstw", dst->width, 0) == 0);
    CHECK(av_opt_set_int(sws, "dsth", dst->height, 0) == 0);
    CHECK(av_opt_set_int(sws, "src_format", src->format, 0) == 0);
    CHECK(av_opt_set_int(sws, "dst_format", dst->format, 0) == 0);
    CHECK(av_opt_set_int(sws, "dst_h_chr_pos", 0, 0) == 0);
    CHECK(av_opt_set_int(sws, "dst_v_chr_pos", 128, 0) == 0);
    CHECK(sws_init_context(sws, nullptr, nullptr) == 0);
    const int *coefficients = sws_getCoefficients(SWS_CS_ITU709);
    CHECK(sws_setColorspaceDetails(sws, coefficients, 1, coefficients, 0, 0, 1 << 16, 1 << 16) ==
          0);
    return sws;
}

static void convert(struct SwsContext *sws, AVFrame *dst, const AVFrame *src, unsigned variant)
{
    if (variant >= 5) {
        struct gk_video_format format = {.width = src->width,
                                         .height = src->height,
                                         .pixel_format = src->format,
                                         .transfer = src->color_trc};
        struct gk_image image = {.data = src->data[0],
                                 .stride = src->linesize[0],
                                 .size = (size_t)src->height * (size_t)src->linesize[0]};
        CHECK((variant == 5
                   ? gk_rgb_to_yuv(&format, &image, dst)
                   : gk_rgb_to_yuv_rows(&format, &image, dst, dst->opaque,
                                        gk_rgb_workspace_size((unsigned)src->width))) == 0);
        return;
    }
    int ret = variant >= 2 ? sws_scale_frame(sws, dst, src)
                           : sws_scale(sws, (const uint8_t *const *)src->data, src->linesize, 0,
                                       src->height, dst->data, dst->linesize);
    CHECK(variant >= 2 ? ret >= 0 : ret == src->height);
}

int main(int argc, char **argv)
{
    CHECK(argc == 6 || argc == 7);
    int width = positive(argv[1]), height = positive(argv[2]), iterations = positive(argv[3]);
    bool ten = !strcmp(argv[4], "10");
    CHECK(width > 0 && height > 0 && iterations > 0 && (ten || !strcmp(argv[4], "8")));
    bool impulse = argc == 7 && !strcmp(argv[6], "impulse");
    CHECK(argc == 6 || impulse);
    AVFrame *src = frame(AV_PIX_FMT_BGRA, width, height, true);
    uint32_t random = 0x1f6ca34b;
    for (int y = 0; y < height; ++y) {
        uint8_t *row = src->data[0] + (ptrdiff_t)y * src->linesize[0];
        for (int x = 0; x < width; ++x) {
            for (unsigned c = 0; c < 3; ++c) {
                random = random * 1664525U + 1013904223U;
                row[4 * (size_t)x + c] = impulse ? (uint8_t)(c == 2 && x == width / 2 - 2 ? 255 : 0)
                                                 : (uint8_t)(random >> 24);
            }
            row[4 * (size_t)x + 3] = 255;
        }
    }
    static const char *names[] = {"legacy", "full-input", "dynamic", "dynamic-full",
                                  "ops",    "fixed",      "rows"};
    for (unsigned v = 0; v < sizeof(names) / sizeof(names[0]); ++v) {
        AVFrame *dst = frame(ten ? AV_PIX_FMT_P010LE : AV_PIX_FMT_NV12, width, height, false);
        int64_t init = gk_now_ns();
        if (v == 6) {
            dst->opaque = malloc(gk_rgb_workspace_size((unsigned)width));
            CHECK(dst->opaque);
        }
        struct SwsContext *sws = context(src, dst, v);
        convert(sws, dst, src, v);
        init = gk_now_ns() - init;
        for (unsigned warm = 0; warm < 7; ++warm)
            convert(sws, dst, src, v);
        int64_t wall = gk_now_ns(), cpu = cpu_ns();
        for (int i = 0; i < iterations; ++i)
            convert(sws, dst, src, v);
        cpu = cpu_ns() - cpu;
        wall = gk_now_ns() - wall;
        printf("%s %dx%d %u-bit init_ns=%lld wall_ns_per_frame=%.1f cpu_ns_per_frame=%.1f\n",
               names[v], width, height, ten ? 10U : 8U, (long long)init, (double)wall / iterations,
               (double)cpu / iterations);
        char path[4096];
        int n = snprintf(path, sizeof(path), "%s/%s.yuv", argv[5], names[v]);
        CHECK(n > 0 && (size_t)n < sizeof(path));
        FILE *out = fopen(path, "wbx");
        CHECK(out);
        for (unsigned p = 0; p < 2; ++p) {
            size_t row_bytes =
                (p ? 2 * (((size_t)width + 1) / 2) : (size_t)width) * (ten ? 2U : 1U);
            int rows = p ? height / 2 + height % 2 : height;
            for (int y = 0; y < rows; ++y)
                CHECK(fwrite(dst->data[p] + (ptrdiff_t)y * dst->linesize[p], 1, row_bytes, out) ==
                      row_bytes);
        }
        CHECK(fclose(out) == 0);
        sws_freeContext(sws);
        free(dst->opaque);
        av_frame_free(&dst);
    }
    av_frame_free(&src);
}

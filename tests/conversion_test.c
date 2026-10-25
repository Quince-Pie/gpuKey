#include "convert.h"

#include <errno.h>
#include <libavutil/intreadwrite.h>
#include <math.h>
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

/* Independent floating-point matrix oracle, followed by a spatial convolution
 * over a full-resolution Cb/Cr image. The implementation filters integer RGB
 * first and uses a single fixed-point matrix afterward. */
static void reference(double out[3], unsigned r, unsigned g, unsigned b, unsigned scale)
{
    double y = (0.2126 * r + 0.7152 * g + 0.0722 * b) / 255.0;
    out[0] = scale * (16.0 + 219.0 * y);
    out[1] = scale * (128.0 + 112.0 * (b / 255.0 - y) / (1.0 - 0.0722));
    out[2] = scale * (128.0 + 112.0 * (r / 255.0 - y) / (1.0 - 0.2126));
}

static unsigned read_sample(const AVFrame *f, unsigned plane, int x, int y, bool wide, bool planar)
{
    int p = planar || plane == 0 ? (int)plane : 1;
    int index = planar || plane == 0 ? x : 2 * x + (int)plane - 1;
    const uint8_t *row = f->data[p] + (ptrdiff_t)y * f->linesize[p];
    unsigned value = wide ? AV_RL16(row + 2 * index) : row[index];
    if (f->format == AV_PIX_FMT_P010LE) {
        CHECK((value & 63) == 0);
        value >>= 6;
    }
    return value;
}

static int clamp(int x, int size)
{
    return x < 0 ? 0 : x >= size ? size - 1 : x;
}

static void check(int width, int height, enum AVPixelFormat input, enum AVPixelFormat output)
{
    bool rgb = input == AV_PIX_FMT_RGBA || input == AV_PIX_FMT_RGB0;
    bool wide = output == AV_PIX_FMT_P010LE || output == AV_PIX_FMT_YUV420P10LE;
    bool planar = output == AV_PIX_FMT_YUV420P || output == AV_PIX_FMT_YUV420P10LE;
    unsigned scale = wide ? 4U : 1U;
    size_t stride = (size_t)width * 4 + 13;
    size_t bytes = (size_t)(height - 1) * stride + (size_t)width * 4;
    uint8_t *pixels = malloc(bytes);
    double (*ideal)[3] = malloc((size_t)width * (size_t)height * sizeof(*ideal));
    CHECK(pixels && ideal);
    uint32_t state = 0x12bcfe01;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            uint8_t *p = pixels + (size_t)y * stride + (size_t)x * 4;
            for (unsigned c = 0; c < 4; ++c) {
                state = 1664525U * state + 1013904223U;
                p[c] = (uint8_t)(state >> 24);
            }
            reference(ideal[(size_t)y * (unsigned)width + (unsigned)x], p[rgb ? 0 : 2], p[1],
                      p[rgb ? 2 : 0], scale);
        }
    }
    AVFrame *f = av_frame_alloc();
    CHECK(f);
    f->format = output;
    f->width = width;
    f->height = height;
    CHECK(av_frame_get_buffer(f, 64) == 0);
    for (unsigned p = 0; p < (planar ? 3U : 2U); ++p)
        memset(f->data[p], 0xa5, (size_t)f->linesize[p] * (size_t)(p ? (height + 1) / 2 : height));
    struct gk_video_format format = {.width = width,
                                     .height = height,
                                     .pixel_format = input,
                                     .transfer = AVCOL_TRC_IEC61966_2_1};
    struct gk_image image = {.data = pixels, .size = bytes, .stride = (int)stride};
    CHECK(gk_rgb_to_yuv(&format, &image, f) == 0);
    size_t copied_size = 0;
    for (unsigned p = 0; p < (planar ? 3U : 2U); ++p)
        copied_size += (size_t)f->linesize[p] * (size_t)(p ? (height + 1) / 2 : height);
    uint8_t *copied = malloc(copied_size);
    size_t scratch_size = gk_rgb_workspace_size((unsigned)width);
    uint16_t *scratch = malloc(scratch_size);
    CHECK(copied && scratch);
    size_t at = 0;
    for (unsigned p = 0; p < (planar ? 3U : 2U); ++p) {
        size_t size = (size_t)f->linesize[p] * (size_t)(p ? (height + 1) / 2 : height);
        memcpy(copied + at, f->data[p], size);
        at += size;
    }
    CHECK(gk_rgb_to_yuv_rows(&format, &image, f, scratch, scratch_size) == 0);
    at = 0;
    for (unsigned p = 0; p < (planar ? 3U : 2U); ++p) {
        size_t size = (size_t)f->linesize[p] * (size_t)(p ? (height + 1) / 2 : height);
        CHECK(memcmp(copied + at, f->data[p], size) == 0);
        at += size;
    }
    free(copied);
    CHECK(gk_rgb_to_yuv_rows(&format, &image, f, scratch, scratch_size - 1) == -EINVAL);
    free(scratch);
    for (unsigned p = 0; p < 3; ++p) {
        int w = p ? (width + 1) / 2 : width, h = p ? (height + 1) / 2 : height;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                double expected;
                if (!p)
                    expected = ideal[(size_t)y * (unsigned)width + (unsigned)x][0];
                else {
                    expected = 0;
                    double weight = 0, cx = 2.0 * x, cy = 2.0 * y + 0.5;
                    for (int yy = 2 * y - 1; yy <= 2 * y + 2; ++yy) {
                        for (int xx = 2 * x - 1; xx <= 2 * x + 1; ++xx) {
                            double wxy = (1.0 - fabs(xx - cx) / 2.0) * (1.0 - fabs(yy - cy) / 2.0);
                            expected += wxy * ideal[(size_t)clamp(yy, height) * (unsigned)width +
                                                    (unsigned)clamp(xx, width)][p];
                            weight += wxy;
                        }
                    }
                    expected /= weight;
                }
                unsigned actual = read_sample(f, p, x, y, wide, planar);
                if (fabs((double)actual - expected) > 0.505) {
                    fprintf(stderr,
                            "conversion %dx%d in=%d out=%d p=%u (%d,%d): %u expected %.8f\n", width,
                            height, input, output, p, x, y, actual, expected);
                    exit(1);
                }
                CHECK(actual >= 16 * scale && actual <= (p ? 240U : 235U) * scale);
            }
        }
    }
    for (unsigned p = 0; p < (planar ? 3U : 2U); ++p) {
        size_t used = (size_t)(p ? (width + 1) / 2 * (planar ? 1 : 2) : width) * (wide ? 2U : 1U);
        int rows = p ? (height + 1) / 2 : height;
        for (int y = 0; y < rows; ++y)
            for (size_t x = used; x < (size_t)f->linesize[p]; ++x)
                CHECK(f->data[p][(size_t)y * (size_t)f->linesize[p] + x] == 0xa5);
    }
    /* Validation precedes writes, including invalid lengths and output strides. */
    uint8_t saved = f->data[0][0];
    image.size--;
    CHECK(gk_rgb_to_yuv(&format, &image, f) == -EINVAL && f->data[0][0] == saved);
    image.size++;
    int saved_stride = f->linesize[1];
    f->linesize[1] = 0;
    CHECK(gk_rgb_to_yuv(&format, &image, f) == -EINVAL && f->data[0][0] == saved);
    f->linesize[1] = saved_stride;
    format.pixel_format = AV_PIX_FMT_YUV444P;
    CHECK(gk_rgb_to_yuv(&format, &image, f) == -ENOTSUP && f->data[0][0] == saved);
    av_frame_free(&f);
    free(ideal);
    free(pixels);
}

int main(void)
{
    static const int sizes[][2] = {{1, 1},   {1, 7},     {2, 2},    {3, 5},  {7, 1},
                                   {15, 16}, {16, 15},   {17, 19},  {31, 9}, {32, 8},
                                   {63, 65}, {319, 241}, {320, 240}};
    static const enum AVPixelFormat inputs[] = {AV_PIX_FMT_BGRA, AV_PIX_FMT_BGR0, AV_PIX_FMT_RGBA,
                                                AV_PIX_FMT_RGB0};
    static const enum AVPixelFormat outputs[] = {AV_PIX_FMT_NV12, AV_PIX_FMT_YUV420P,
                                                 AV_PIX_FMT_P010LE, AV_PIX_FMT_YUV420P10LE};
    for (unsigned s = 0; s < sizeof(sizes) / sizeof(sizes[0]); ++s)
        for (unsigned i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i)
            for (unsigned o = 0; o < sizeof(outputs) / sizeof(outputs[0]); ++o)
                check(sizes[s][0], sizes[s][1], inputs[i], outputs[o]);
    puts("conversion: independent matrix/filter oracle, 208 format/geometry combinations, padding "
         "and validation pass");
}

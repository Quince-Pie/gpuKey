#include "convert.h"

#include <errno.h>
#include <libavutil/intreadwrite.h>
#include <stdckdint.h>

/* BT.709 nonlinear matrix and limited-range scaling, rounded to Q18.
 * Chroma rows sum exactly to zero. Quantization happens after filtering;
 * no intermediate YUV frame or per-pixel metadata is materialized. */
static unsigned luma(unsigned r, unsigned g, unsigned b, unsigned scale)
{
    unsigned shift = scale == 4 ? 16U : 18U;
    return (47864 * r + 161017 * g + 16255 * b + (16U << 18) + (1U << (shift - 1))) >> shift;
}

static unsigned chroma(int r, int g, int b, unsigned scale, bool cr)
{
    int value = cr ? 115138 * r - 104580 * g - 10558 * b : -26383 * r - 88755 * g + 115138 * b;
    /* Filtered channel sums are in [0, 32*255]. Including the offset, the
     * largest 10-bit numerator is below INT32_MAX. Every output is positive. */
    unsigned shift = scale == 4 ? 21U : 23U;
    return (unsigned)(value + (1 << 30) + (1 << (shift - 1))) >> shift;
}

/* Three independent 16-bit lanes in one word. The entire 3x4 filter sums
 * to 32, so a lane reaches at most 8160 and cannot carry into its neighbor. */
static uint64_t lanes(const uint8_t *pixel)
{
    uint32_t p = AV_RL32(pixel);
    return (p & 255U) | ((uint64_t)(p & 0xff00U) << 8) | ((uint64_t)(p & 0xff0000U) << 16);
}

static uint64_t horizontal(const uint8_t *row, size_t left, size_t mid, size_t right)
{
    return lanes(row + 4 * left) + 2 * lanes(row + 4 * mid) + lanes(row + 4 * right);
}

static void horizontal_store(uint16_t *c0, uint16_t *c1, uint16_t *c2, const uint8_t *src, size_t x,
                             size_t left, size_t right)
{
    uint64_t sum = horizontal(src, left, 2 * x, right);
    c0[x] = (uint16_t)sum;
    c1[x] = (uint16_t)(sum >> 16);
    c2[x] = (uint16_t)(sum >> 32);
}

static int vertical(const uint16_t *const rows[4], size_t index)
{
    return rows[0][index] + 3 * (rows[1][index] + rows[2][index]) + rows[3][index];
}

static void put(uint8_t *row, size_t x, unsigned value, unsigned shift, bool wide)
{
    if (wide)
        AV_WL16(row + 2 * x, (uint16_t)(value << shift));
    else
        row[x] = (uint8_t)value;
}

static int validate(const struct gk_video_format *format, const struct gk_image *image,
                    AVFrame *destination, bool *is_rgb, bool *is_wide, bool *is_planar)
{
    bool rgb;
    switch (format->pixel_format) {
    case AV_PIX_FMT_RGBA:
    case AV_PIX_FMT_RGB0:
        rgb = true;
        break;
    case AV_PIX_FMT_BGRA:
    case AV_PIX_FMT_BGR0:
        rgb = false;
        break;
    default:
        return -ENOTSUP;
    }
    bool wide, planar;
    switch (destination->format) {
    case AV_PIX_FMT_NV12:
        wide = false;
        planar = false;
        break;
    case AV_PIX_FMT_YUV420P:
        wide = false;
        planar = true;
        break;
    case AV_PIX_FMT_P010LE:
        wide = true;
        planar = false;
        break;
    case AV_PIX_FMT_YUV420P10LE:
        wide = true;
        planar = true;
        break;
    default:
        return -ENOTSUP;
    }
    if (!image->data || format->width <= 0 || format->height <= 0 || image->stride <= 0 ||
        destination->width != format->width || destination->height != format->height)
        return -EINVAL;
    size_t w = (unsigned)format->width, h = (unsigned)format->height;
    size_t row_bytes, required;
    if (ckd_mul(&row_bytes, w, 4) || row_bytes > (size_t)image->stride ||
        ckd_mul(&required, h - 1, (size_t)image->stride) ||
        ckd_add(&required, required, row_bytes) || required > image->size)
        return -EINVAL;
    size_t cw = w / 2 + w % 2;
    for (unsigned p = 0; p < (planar ? 3U : 2U); ++p) {
        size_t samples = p ? cw * (planar ? 1U : 2U) : w;
        if (!destination->data[p] || destination->linesize[p] <= 0 ||
            samples * (wide ? 2U : 1U) > (size_t)destination->linesize[p])
            return -EINVAL;
    }
    *is_rgb = rgb;
    *is_wide = wide;
    *is_planar = planar;
    return 0;
}

int gk_rgb_to_yuv(const struct gk_video_format *format, const struct gk_image *image,
                  AVFrame *destination)
{
    bool rgb, wide, planar;
    int ret = validate(format, image, destination, &rgb, &wide, &planar);
    if (ret < 0)
        return ret;
    size_t w = (unsigned)format->width, h = (unsigned)format->height;
    size_t cw = w / 2 + w % 2, ch = h / 2 + h % 2;
    unsigned shift = destination->format == AV_PIX_FMT_P010LE ? 6U : 0U;
    unsigned red = rgb ? 0U : 2U, blue = rgb ? 2U : 0U, scale = wide ? 4U : 1U;
    for (size_t y = 0; y < h; ++y) {
        const uint8_t *src = image->data + y * (size_t)image->stride;
        uint8_t *dst = destination->data[0] + y * (size_t)destination->linesize[0];
        for (size_t x = 0; x < w; ++x)
            put(dst, x, luma(src[4 * x + red], src[4 * x + 1], src[4 * x + blue], scale), shift,
                wide);
    }
    for (size_t y = 0; y < ch; ++y) {
        size_t center = 2 * y;
        size_t rows[] = {center ? center - 1 : 0, center, center + 1 < h ? center + 1 : h - 1,
                         center + 2 < h ? center + 2 : h - 1};
        const uint8_t *src[4];
        for (unsigned r = 0; r < 4; ++r)
            src[r] = image->data + rows[r] * (size_t)image->stride;
        uint8_t *u = destination->data[1] + y * (size_t)destination->linesize[1];
        uint8_t *v = planar ? destination->data[2] + y * (size_t)destination->linesize[2] : u;
        for (size_t x = 0; x < cw; ++x) {
            size_t mid = 2 * x, left = mid ? mid - 1 : 0, right = mid + 1 < w ? mid + 1 : w - 1;
            uint64_t sum =
                horizontal(src[0], left, mid, right) +
                3 * (horizontal(src[1], left, mid, right) + horizontal(src[2], left, mid, right)) +
                horizontal(src[3], left, mid, right);
            int c0 = (int)(sum & 65535), c1 = (int)((sum >> 16) & 65535), c2 = (int)(sum >> 32);
            int r = rgb ? c0 : c2, b = rgb ? c2 : c0;
            put(u, planar ? x : 2 * x, chroma(r, c1, b, scale, false), shift, wide);
            put(v, planar ? x : 2 * x + 1, chroma(r, c1, b, scale, true), shift, wide);
        }
    }
    return 0;
}

size_t gk_rgb_workspace_size(unsigned width)
{
    size_t pitch = (size_t)width / 2 + width % 2, bytes;
    if (!width || ckd_add(&pitch, pitch, 31) || ckd_mul(&bytes, pitch & ~(size_t)31, 24))
        return 0;
    return bytes;
}

#if defined(__x86_64__) && __has_c_attribute(gnu::target_clones) && !defined(GK_RGB_PORTABLE)
[[gnu::target_clones("arch=x86-64-v4", "arch=x86-64-v3", "default")]]
#endif
int gk_rgb_to_yuv_rows(const struct gk_video_format *format, const struct gk_image *image,
                       AVFrame *destination, uint16_t *restrict workspace, size_t bytes)
{
    bool rgb, wide, planar;
    int ret = validate(format, image, destination, &rgb, &wide, &planar);
    if (ret < 0)
        return ret;
    size_t needed = gk_rgb_workspace_size((unsigned)format->width);
    if (!workspace || !needed || bytes < needed)
        return -EINVAL;
    size_t w = (unsigned)format->width, h = (unsigned)format->height;
    size_t cw = (w + 1) / 2, ch = (h + 1) / 2, pitch = needed / 24;
    unsigned shift = destination->format == AV_PIX_FMT_P010LE ? 6U : 0U;
    unsigned red = rgb ? 0U : 2U, blue = rgb ? 2U : 0U, scale = wide ? 4U : 1U;
    const uint8_t *restrict input = image->data;
    uint8_t *restrict luma_data = destination->data[0];
    uint8_t *restrict chroma_data = destination->data[1];
    uint8_t *restrict v_data = planar ? destination->data[2] : nullptr;
    size_t next_row = 0;
    for (size_t cy = 0; cy < ch; ++cy) {
        size_t center = 2 * cy, end = center + 2 < h ? center + 2 : h - 1;
        while (next_row <= end) {
            size_t y = next_row++;
            const uint8_t *src = input + y * (size_t)image->stride;
            uint8_t *dst = luma_data + y * (size_t)destination->linesize[0];
            uint16_t *c0 = workspace + (y & 3) * 3 * pitch;
            uint16_t *c1 = c0 + pitch, *c2 = c1 + pitch;
            for (size_t x = 0; x < w; ++x)
                put(dst, x, luma(src[4 * x + red], src[4 * x + 1], src[4 * x + blue], scale), shift,
                    wide);
            horizontal_store(c0, c1, c2, src, 0, 0, w > 1 ? 1 : 0);
            size_t end_x = cw - w % 2;
            for (size_t x = 1; x < end_x; ++x)
                horizontal_store(c0, c1, c2, src, x, 2 * x - 1, 2 * x + 1);
            if (w % 2 && cw > 1)
                horizontal_store(c0, c1, c2, src, cw - 1, w - 2, w - 1);
        }
        size_t rows[] = {center ? center - 1 : 0, center, center + 1 < h ? center + 1 : h - 1, end};
        const uint16_t *r[4];
        for (unsigned i = 0; i < 4; ++i)
            r[i] = workspace + (rows[i] & 3) * 3 * pitch;
        uint8_t *u = chroma_data + cy * (size_t)destination->linesize[1];
        uint8_t *v = planar ? v_data + cy * (size_t)destination->linesize[2] : u;
        for (size_t x = 0; x < cw; ++x) {
            int cr = vertical(r, red * pitch + x), cg = vertical(r, pitch + x),
                cb = vertical(r, blue * pitch + x);
            put(u, planar ? x : 2 * x, chroma(cr, cg, cb, scale, false), shift, wide);
            put(v, planar ? x : 2 * x + 1, chroma(cr, cg, cb, scale, true), shift, wide);
        }
    }
    return 0;
}

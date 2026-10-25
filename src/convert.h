#ifndef GPUKEY_CONVERT_H
#define GPUKEY_CONVERT_H

#include "gpu.h"

/* Same-size RGB8 -> limited BT.709 4:2:0. Preserve the nonlinear source
 * transfer. Chroma centers are (2*x, 2*y+1/2), with separable triangular
 * downsampling: [1,2,1]/4 horizontally, [1,3,3,1]/8 vertically. Edges clamp.
 * No allocations or retained source references. Buffers must provide their
 * stated row extents and must not overlap. Failed validation writes nothing. */
[[nodiscard]] int gk_rgb_to_yuv(const struct gk_video_format *format, const struct gk_image *image,
                                AVFrame *destination);
/* Alternate separable execution, with four reusable RGB-sum rows. Storage is
 * caller-owned, uint16_t-aligned and disjoint from source/destination. */
size_t gk_rgb_workspace_size(unsigned width);
[[nodiscard]] int gk_rgb_to_yuv_rows(const struct gk_video_format *format,
                                     const struct gk_image *image, AVFrame *destination,
                                     uint16_t *workspace, size_t bytes);

#endif

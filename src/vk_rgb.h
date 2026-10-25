#ifndef GPUKEY_VK_RGB_H
#define GPUKEY_VK_RGB_H

#include "gpu.h"

/* Linked model/range pairs; never advertise their unsupported cross-product. */
enum gk_rgb_conversion : uint8_t { GK_RGB_NONE, GK_RGB_709_LIMITED, GK_RGB_2020_FULL };

/* Cold capability/import state; pixel storage remains owned by capture. */
[[nodiscard]] int gk_vk_rgb_open(struct gk_gpu *gpu);
void gk_vk_rgb_close(struct gk_gpu *gpu);
enum gk_rgb_conversion gk_vk_rgb_conversion(const struct gk_gpu *gpu);
const uint64_t *gk_gpu_modifiers(const struct gk_gpu *gpu, enum AVPixelFormat format,
                                 unsigned *count);
[[nodiscard]] int gk_vk_rgb_frames(struct gk_gpu *gpu, const struct gk_video_format *format,
                                   AVBufferRef **frames);
[[nodiscard]] int gk_vk_rgb_import(struct gk_gpu *gpu, const struct gk_video_format *format,
                                   const struct gk_image *image, AVFrame **frame);

#endif

#ifndef GPUKEY_GPU_H
#define GPUKEY_GPU_H

#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <va/va.h>

enum gk_backend { GK_VAAPI, GK_NVENC, GK_VULKAN, GK_SOFTWARE };

/* Immutable source interpretation belongs to a generation, not every token. */
struct gk_video_format {
    int width, height;
    enum AVPixelFormat pixel_format;
    enum AVColorTransferCharacteristic transfer;
    uint32_t drm_format;
    uint64_t modifier;
    bool dmabuf;
};

struct gk_plane {
    int fd; /* owned by the capture allocation record */
    uint32_t offset, pitch, size;
};

struct gk_image {
    struct gk_plane planes[4];
    unsigned plane_count;
    const uint8_t *data; /* valid through the capture lease only */
    size_t size;
    int stride;
};

struct gk_gpu {
    enum gk_backend backend;
    AVBufferRef *device;
    struct gk_vk_rgb *rgb;
    VADisplay display;
    VAConfigID vpp_config;
    VAContextID vpp_context;
    VABufferID vpp_parameters;
    char name[256];
    uint64_t modifiers[4];
    unsigned modifier_count;
    bool explicit_color;
};

[[nodiscard]] int gk_gpu_open(struct gk_gpu *gpu, enum gk_backend backend, const char *device);
void gk_gpu_close(struct gk_gpu *gpu);
[[nodiscard]] int gk_gpu_configure(struct gk_gpu *gpu, int width, int height);
[[nodiscard]] int gk_gpu_import(struct gk_gpu *gpu, const struct gk_video_format *format,
                                const struct gk_image *image, VASurfaceID *surface);
void gk_gpu_unimport(struct gk_gpu *gpu, VASurfaceID *surface);
/* Submit while source removal is excluded; wait after releasing that exclusion. */
[[nodiscard]] int gk_gpu_convert_submit(struct gk_gpu *gpu, const struct gk_video_format *format,
                                        VASurfaceID source, AVFrame *destination);
[[nodiscard]] int gk_gpu_convert_wait(struct gk_gpu *gpu, AVFrame *destination);
[[nodiscard]] int gk_gpu_probe(void);

#endif

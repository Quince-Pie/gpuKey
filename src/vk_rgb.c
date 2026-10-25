#include "vk_rgb.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#ifdef GK_VULKAN_RGB
#include <libavutil/hwcontext_drm.h>
#include <libavutil/hwcontext_vulkan.h>
#include <stdckdint.h>

struct gk_vk_rgb {
    uint64_t *modifiers;
    unsigned count;
    enum gk_rgb_conversion conversion;
    AVBufferRef *frames;
};

struct frame_description {
    VkVideoProfileListInfoKHR independent;
    VkImageDrmFormatModifierListCreateInfoEXT modifiers;
    VkImageFormatListCreateInfo views;
    uint64_t modifier;
    VkFormat formats[2];
};

static bool enabled(const AVVulkanDeviceContext *vk, const char *name)
{
    for (int i = 0; i < vk->nb_enabled_dev_extensions; ++i)
        if (!strcmp(vk->enabled_dev_extensions[i], name))
            return true;
    return false;
}

static bool rgb8(enum AVPixelFormat format)
{
    return format == AV_PIX_FMT_BGRA || format == AV_PIX_FMT_BGR0 || format == AV_PIX_FMT_RGBA ||
           format == AV_PIX_FMT_RGB0;
}

int gk_vk_rgb_open(struct gk_gpu *g)
{
    AVHWDeviceContext *device = (void *)g->device->data;
    AVVulkanDeviceContext *vk = device->hwctx;
    if (!enabled(vk, VK_VALVE_VIDEO_ENCODE_RGB_CONVERSION_EXTENSION_NAME) ||
        !enabled(vk, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME))
        return 0; /* The existing explicit CPU-input path remains available. */
    PFN_vkGetPhysicalDeviceFormatProperties2 get_formats =
        (PFN_vkGetPhysicalDeviceFormatProperties2)vk->get_proc_addr(
            vk->inst, "vkGetPhysicalDeviceFormatProperties2");
    PFN_vkGetPhysicalDeviceImageFormatProperties2 get_image =
        (PFN_vkGetPhysicalDeviceImageFormatProperties2)vk->get_proc_addr(
            vk->inst, "vkGetPhysicalDeviceImageFormatProperties2");
    PFN_vkGetPhysicalDeviceVideoCapabilitiesKHR get_caps =
        (PFN_vkGetPhysicalDeviceVideoCapabilitiesKHR)vk->get_proc_addr(
            vk->inst, "vkGetPhysicalDeviceVideoCapabilitiesKHR");
    if (!get_formats || !get_image || !get_caps)
        return -ENOSYS;
    VkVideoEncodeProfileRgbConversionInfoVALVE conversion = {
        .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_PROFILE_RGB_CONVERSION_INFO_VALVE,
        .performEncodeRgbConversion = VK_TRUE};
    VkVideoEncodeAV1ProfileInfoKHR codec = {.sType =
                                                VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_PROFILE_INFO_KHR,
                                            .pNext = &conversion,
                                            .stdProfile = STD_VIDEO_AV1_PROFILE_MAIN};
    VkVideoProfileInfoKHR profile = {.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR,
                                     .pNext = &codec,
                                     .videoCodecOperation =
                                         VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR,
                                     .chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR,
                                     .lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR,
                                     .chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR};
    VkVideoEncodeRgbConversionCapabilitiesVALVE colors = {
        .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_RGB_CONVERSION_CAPABILITIES_VALVE};
    VkVideoEncodeAV1CapabilitiesKHR av1 = {
        .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_CAPABILITIES_KHR, .pNext = &colors};
    VkVideoEncodeCapabilitiesKHR encode = {.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_CAPABILITIES_KHR,
                                           .pNext = &av1};
    VkVideoCapabilitiesKHR caps = {.sType = VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR,
                                   .pNext = &encode};
    if (get_caps(vk->phys_dev, &profile, &caps) != VK_SUCCESS)
        return 0;
    enum gk_rgb_conversion selected;
    if ((colors.rgbModels & VK_VIDEO_ENCODE_RGB_MODEL_CONVERSION_YCBCR_709_BIT_VALVE) &&
        (colors.rgbRanges & VK_VIDEO_ENCODE_RGB_RANGE_COMPRESSION_NARROW_RANGE_BIT_VALVE))
        selected = GK_RGB_709_LIMITED;
    else if ((colors.rgbModels & VK_VIDEO_ENCODE_RGB_MODEL_CONVERSION_YCBCR_2020_BIT_VALVE) &&
             (colors.rgbRanges & VK_VIDEO_ENCODE_RGB_RANGE_COMPRESSION_FULL_RANGE_BIT_VALVE))
        selected = GK_RGB_2020_FULL;
    else
        return 0;

    /* Both byte orders use the same compatible R8/B8 image/view pair. Offer
     * their intersection, queried with the exact import flags and RGB profile. */
    VkDrmFormatModifierPropertiesList2EXT list = {
        .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_2_EXT};
    VkFormatProperties2 properties = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
                                      .pNext = &list};
    get_formats(vk->phys_dev, VK_FORMAT_R8G8B8A8_UNORM, &properties);
    uint32_t capacity = list.drmFormatModifierCount;
    if (!capacity)
        return 0;
    list.pDrmFormatModifierProperties =
        calloc(capacity, sizeof(*list.pDrmFormatModifierProperties));
    if (!list.pDrmFormatModifierProperties)
        return -ENOMEM;
    get_formats(vk->phys_dev, VK_FORMAT_R8G8B8A8_UNORM, &properties);
    struct gk_vk_rgb *state = calloc(1, sizeof(*state));
    if (!state) {
        free(list.pDrmFormatModifierProperties);
        return -ENOMEM;
    }
    g->rgb = state;
    state->conversion = selected;
    state->modifiers = calloc(capacity, sizeof(*state->modifiers));
    if (!state->modifiers || list.drmFormatModifierCount > capacity) {
        free(list.pDrmFormatModifierProperties);
        return state->modifiers ? -EPROTO : -ENOMEM;
    }
    VkFormat formats[] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM};
    for (uint32_t i = 0; i < list.drmFormatModifierCount; ++i) {
        const VkDrmFormatModifierProperties2EXT *p = &list.pDrmFormatModifierProperties[i];
        if (p->drmFormatModifierPlaneCount != 1 ||
            !(p->drmFormatModifierTilingFeatures & VK_FORMAT_FEATURE_2_VIDEO_ENCODE_INPUT_BIT_KHR))
            continue;
        VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifier = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
            .drmFormatModifier = p->drmFormatModifier,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        VkImageFormatListCreateInfo views = {.sType =
                                                 VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
                                             .pNext = &modifier,
                                             .viewFormatCount = 2,
                                             .pViewFormats = formats};
        VkVideoProfileListInfoKHR profiles = {.sType =
                                                  VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR,
                                              .pNext = &views,
                                              .profileCount = 1,
                                              .pProfiles = &profile};
        VkPhysicalDeviceExternalImageFormatInfo external = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
            .pNext = &profiles,
            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
        VkPhysicalDeviceImageFormatInfo2 info = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
            .pNext = &external,
            .type = VK_IMAGE_TYPE_2D,
            .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
            .usage = VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR,
            .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT |
                     VK_IMAGE_CREATE_VIDEO_PROFILE_INDEPENDENT_BIT_KHR};
        bool supported = true;
        for (unsigned j = 0; j < 2; ++j) {
            VkExternalImageFormatProperties ext = {
                .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
            VkImageFormatProperties2 output = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
                                               .pNext = &ext};
            info.format = formats[j];
            supported &= get_image(vk->phys_dev, &info, &output) == VK_SUCCESS &&
                         (ext.externalMemoryProperties.externalMemoryFeatures &
                          VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) != 0;
        }
        if (supported)
            state->modifiers[state->count++] = p->drmFormatModifier;
    }
    free(list.pDrmFormatModifierProperties);
    return 0;
}

void gk_vk_rgb_close(struct gk_gpu *g)
{
    if (!g->rgb)
        return;
    av_buffer_unref(&g->rgb->frames);
    free(g->rgb->modifiers);
    free(g->rgb);
    g->rgb = nullptr;
}

enum gk_rgb_conversion gk_vk_rgb_conversion(const struct gk_gpu *g)
{
    return g->rgb ? g->rgb->conversion : GK_RGB_NONE;
}

static void free_description(AVHWFramesContext *context)
{
    free(context->user_opaque);
}

int gk_vk_rgb_frames(struct gk_gpu *g, const struct gk_video_format *f, AVBufferRef **out)
{
    *out = nullptr;
    if (!g->rgb || !f->dmabuf || !rgb8(f->pixel_format) || g->rgb->frames)
        return -ENOTSUP;
    bool found = false;
    for (unsigned i = 0; i < g->rgb->count; ++i)
        found |= g->rgb->modifiers[i] == f->modifier;
    if (!found)
        return -ENOTSUP;
    AVBufferRef *frames = av_hwframe_ctx_alloc(g->device);
    if (!frames)
        return -ENOMEM;
    AVHWFramesContext *fc = (void *)frames->data;
    struct frame_description *d = calloc(1, sizeof(*d));
    int ret = -ENOMEM;
    if (!d)
        goto fail;
    fc->free = free_description;
    fc->user_opaque = d;
    d->modifier = f->modifier;
    d->formats[0] = VK_FORMAT_R8G8B8A8_UNORM;
    d->formats[1] = VK_FORMAT_B8G8R8A8_UNORM;
    d->independent.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR;
    d->modifiers = (VkImageDrmFormatModifierListCreateInfoEXT){
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
        .pNext = &d->independent,
        .drmFormatModifierCount = 1,
        .pDrmFormatModifiers = &d->modifier};
    d->views =
        (VkImageFormatListCreateInfo){.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
                                      .pNext = &d->modifiers,
                                      .viewFormatCount = 2,
                                      .pViewFormats = d->formats};
    fc->format = AV_PIX_FMT_VULKAN;
    fc->sw_format = f->pixel_format;
    fc->width = f->width;
    fc->height = f->height;
    AVVulkanFramesContext *vk = fc->hwctx;
    vk->tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    vk->usage = VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR;
    vk->img_flags =
        VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_VIDEO_PROFILE_INDEPENDENT_BIT_KHR;
    vk->create_pnext = &d->views;
    ret = av_hwframe_ctx_init(frames);
    if (ret < 0)
        goto fail;
    *out = av_buffer_ref(frames);
    if (!*out) {
        ret = -ENOMEM;
        goto fail;
    }
    g->rgb->frames = frames;
    return 0;
fail:
    av_buffer_unref(&frames);
    return ret;
}

int gk_vk_rgb_import(struct gk_gpu *g, const struct gk_video_format *f,
                     const struct gk_image *image, AVFrame **out)
{
    *out = nullptr;
    if (!g->rgb || !g->rgb->frames || image->plane_count != 1)
        return -ENOTSUP;
    AVFrame *frame = av_frame_alloc();
    if (!frame)
        return -ENOMEM;
    AVDRMFrameDescriptor descriptor = {.nb_objects = 1,
                                       .nb_layers = 1,
                                       .objects = {{.fd = image->planes[0].fd,
                                                    .size = image->planes[0].size,
                                                    .format_modifier = f->modifier}},
                                       .layers = {{.format = f->drm_format,
                                                   .nb_planes = 1,
                                                   .planes = {{.object_index = 0,
                                                               .offset = image->planes[0].offset,
                                                               .pitch = image->planes[0].pitch}}}}};
    int ret = av_vk_frame_import_drm(g->rgb->frames, frame, &descriptor, f->width, f->height);
    if (ret < 0)
        av_frame_free(&frame);
    *out = frame;
    return ret;
}
#else
int gk_vk_rgb_open(struct gk_gpu *g)
{
    (void)g;
    return 0;
}
void gk_vk_rgb_close(struct gk_gpu *g)
{
    (void)g;
}
enum gk_rgb_conversion gk_vk_rgb_conversion(const struct gk_gpu *g)
{
    (void)g;
    return GK_RGB_NONE;
}
int gk_vk_rgb_frames(struct gk_gpu *g, const struct gk_video_format *f, AVBufferRef **out)
{
    (void)g;
    (void)f;
    *out = nullptr;
    return -ENOTSUP;
}
int gk_vk_rgb_import(struct gk_gpu *g, const struct gk_video_format *f,
                     const struct gk_image *image, AVFrame **out)
{
    (void)g;
    (void)f;
    (void)image;
    *out = nullptr;
    return -ENOTSUP;
}
#endif

const uint64_t *gk_gpu_modifiers(const struct gk_gpu *g, enum AVPixelFormat format, unsigned *count)
{
#ifdef GK_VULKAN_RGB
    if (g->rgb && rgb8(format)) {
        *count = g->rgb->count;
        return g->rgb->modifiers;
    }
#else
    (void)format;
#endif
    *count = g->modifier_count;
    return g->modifiers;
}

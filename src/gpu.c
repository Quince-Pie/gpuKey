#include "gpu.h"
#include "vk_rgb.h"

#include <drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <libavutil/hwcontext_vaapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tracy/TracyC.h>
#include <unistd.h>
#include <va/va_drmcommon.h>
#include <va/va_vpp.h>

static int va_error(VAStatus status, const char *operation)
{
    if (status == VA_STATUS_SUCCESS)
        return 0;
    fprintf(stderr, "%s: %s\n", operation, vaErrorStr(status));
    return -EIO;
}

static void add_modifier(struct gk_gpu *g, uint64_t modifier)
{
    for (unsigned i = 0; i < g->modifier_count; ++i)
        if (g->modifiers[i] == modifier)
            return;
    if (g->modifier_count < sizeof(g->modifiers) / sizeof(g->modifiers[0]))
        g->modifiers[g->modifier_count++] = modifier;
}

static void probe_modifiers(struct gk_gpu *g)
{
    /* VA has no modifier enumeration API. Offer only modifiers actually allocated
     * and exported on this device, then test each real import. Never substitute
     * implicit tiling if a PRIME_2 import rejects its explicit modifier. */
    for (unsigned linear = 0; linear < 2; ++linear) {
        uint64_t modifier = DRM_FORMAT_MOD_LINEAR;
        VADRMFormatModifierList list = {.num_modifiers = 1, .modifiers = &modifier};
        VASurfaceAttrib attrs[] = {
            {.type = VASurfaceAttribPixelFormat,
             .flags = VA_SURFACE_ATTRIB_SETTABLE,
             .value = {.type = VAGenericValueTypeInteger, .value.i = VA_FOURCC_BGRX}},
            {.type = VASurfaceAttribUsageHint,
             .flags = VA_SURFACE_ATTRIB_SETTABLE,
             .value = {.type = VAGenericValueTypeInteger,
                       .value.i = VA_SURFACE_ATTRIB_USAGE_HINT_VPP_READ |
                                  VA_SURFACE_ATTRIB_USAGE_HINT_EXPORT}},
            {.type = VASurfaceAttribDRMFormatModifiers,
             .flags = VA_SURFACE_ATTRIB_SETTABLE,
             .value = {.type = VAGenericValueTypePointer, .value.p = &list}},
        };
        VASurfaceID surface;
        if (vaCreateSurfaces(g->display, VA_RT_FORMAT_RGB32, 64, 64, &surface, 1, attrs,
                             linear ? 3 : 2) != VA_STATUS_SUCCESS)
            continue;
        VADRMPRIMESurfaceDescriptor desc = {};
        if (vaExportSurfaceHandle(g->display, surface, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                  VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_COMPOSED_LAYERS,
                                  &desc) == VA_STATUS_SUCCESS) {
            if (desc.num_objects == 1)
                add_modifier(g, desc.objects[0].drm_format_modifier);
            for (unsigned j = 0; j < desc.num_objects; ++j)
                close(desc.objects[j].fd);
        }
        vaDestroySurfaces(g->display, &surface, 1);
    }
}

static bool supports_av1(VADisplay display)
{
    int maximum = vaMaxNumEntrypoints(display);
    if (maximum <= 0)
        return false;
    VAEntrypoint *entries = calloc((size_t)maximum, sizeof(*entries));
    if (!entries)
        return false;
    int count = 0;
    bool found = false;
    if (vaQueryConfigEntrypoints(display, VAProfileAV1Profile0, entries, &count) !=
        VA_STATUS_SUCCESS) {
        free(entries);
        return false;
    }
    for (int i = 0; i < count; ++i)
        found |= entries[i] == VAEntrypointEncSlice || entries[i] == VAEntrypointEncSliceLP;
    free(entries);
    return found;
}

int gk_gpu_open(struct gk_gpu *g, enum gk_backend backend, const char *device)
{
    *g = (struct gk_gpu){.backend = backend,
                         .vpp_config = VA_INVALID_ID,
                         .vpp_context = VA_INVALID_ID,
                         .vpp_parameters = VA_INVALID_ID};
    if (backend == GK_SOFTWARE) {
        snprintf(g->name, sizeof(g->name), "explicit software AV1 control");
        return 0;
    }
    enum AVHWDeviceType type = backend == GK_VAAPI   ? AV_HWDEVICE_TYPE_VAAPI
                               : backend == GK_NVENC ? AV_HWDEVICE_TYPE_CUDA
                                                     : AV_HWDEVICE_TYPE_VULKAN;
    int ret = -ENODEV;
    if (backend == GK_VAAPI && !device) {
        glob_t devices = {};
        if (glob("/dev/dri/renderD*", 0, nullptr, &devices) == 0) {
            for (size_t i = 0; i < devices.gl_pathc; ++i) {
                ret = av_hwdevice_ctx_create(&g->device, type, devices.gl_pathv[i], nullptr, 0);
                if (ret < 0)
                    continue;
                AVHWDeviceContext *ctx = (void *)g->device->data;
                g->display = ((AVVAAPIDeviceContext *)ctx->hwctx)->display;
                if (supports_av1(g->display)) {
                    snprintf(g->name, sizeof(g->name), "%s: %s", devices.gl_pathv[i],
                             vaQueryVendorString(g->display));
                    break;
                }
                av_buffer_unref(&g->device);
                g->display = nullptr;
                ret = -ENOTSUP;
            }
        }
        globfree(&devices);
    } else {
        AVDictionary *device_options = nullptr;
        /* One issuer uses one queue per family. FFmpeg otherwise retains two
         * upload contexts per exposed transfer queue, needlessly pinning a large
         * input pool even after GPU completion. Keep encode/transfer families. */
        if (backend == GK_VULKAN)
            av_dict_set(&device_options, "limit_queues", "1", 0);
        ret = av_hwdevice_ctx_create(&g->device, type, device, device_options, 0);
        av_dict_free(&device_options);
        if (ret >= 0) {
            snprintf(g->name, sizeof(g->name), "%s device %s", av_hwdevice_get_type_name(type),
                     device ? device : "default");
            if (backend == GK_VAAPI) {
                AVHWDeviceContext *ctx = (void *)g->device->data;
                g->display = ((AVVAAPIDeviceContext *)ctx->hwctx)->display;
                if (!supports_av1(g->display))
                    ret = -ENOTSUP;
            }
        }
    }
    if (ret >= 0 && backend == GK_VULKAN)
        ret = gk_vk_rgb_open(g);
    if (ret < 0) {
        gk_gpu_close(g);
        return ret;
    }
    if (backend == GK_VAAPI) {
        ret = va_error(vaCreateConfig(g->display, VAProfileNone, VAEntrypointVideoProc, nullptr, 0,
                                      &g->vpp_config),
                       "create VPP config");
        if (ret < 0) {
            gk_gpu_close(g);
            return ret;
        }
        probe_modifiers(g);
    }
    return 0;
}

int gk_gpu_configure(struct gk_gpu *g, int width, int height)
{
    if (g->backend != GK_VAAPI)
        return 0;
    if (g->vpp_context != VA_INVALID_ID)
        return -EALREADY;
    int ret = va_error(vaCreateContext(g->display, g->vpp_config, width, height, VA_PROGRESSIVE,
                                       nullptr, 0, &g->vpp_context),
                       "create VPP context");
    if (ret < 0)
        return ret;
    VAProcPipelineCaps caps = {};
    ret = va_error(vaQueryVideoProcPipelineCaps(g->display, g->vpp_context, nullptr, 0, &caps),
                   "query VPP capabilities");
    if (ret < 0)
        return ret;
    bool input = false, output = false;
    for (unsigned i = 0; i < caps.num_input_color_standards; ++i)
        input |= caps.input_color_standards[i] == VAProcColorStandardExplicit;
    for (unsigned i = 0; i < caps.num_output_color_standards; ++i)
        output |= caps.output_color_standards[i] == VAProcColorStandardExplicit;
    g->explicit_color = input && output;
#ifndef GK_TRANSIENT_VPP
    VAProcPipelineParameterBuffer parameters = {};
    ret = va_error(vaCreateBuffer(g->display, g->vpp_context, VAProcPipelineParameterBufferType,
                                  sizeof(parameters), 1, &parameters, &g->vpp_parameters),
                   "preallocate VPP parameters");
#endif
    return ret;
}

int gk_gpu_import(struct gk_gpu *g, const struct gk_video_format *f, const struct gk_image *image,
                  VASurfaceID *surface)
{
    *surface = VA_INVALID_ID;
    if (g->backend != GK_VAAPI || !f->dmabuf || image->plane_count == 0 || image->plane_count > 4)
        return -ENOTSUP;
    uint32_t fourcc = f->pixel_format == AV_PIX_FMT_BGRA   ? VA_FOURCC_BGRA
                      : f->pixel_format == AV_PIX_FMT_BGR0 ? VA_FOURCC_BGRX
                      : f->pixel_format == AV_PIX_FMT_RGBA ? VA_FOURCC_RGBA
                                                           : VA_FOURCC_RGBX;
    if (f->modifier == DRM_FORMAT_MOD_INVALID)
        return -ENOTSUP;
    VADRMPRIMESurfaceDescriptor desc = {
        .fourcc = fourcc,
        .width = (uint32_t)f->width,
        .height = (uint32_t)f->height,
        .num_objects = image->plane_count,
        .num_layers = 1,
    };
    desc.layers[0].drm_format = f->drm_format;
    desc.layers[0].num_planes = image->plane_count;
    for (unsigned i = 0; i < image->plane_count; ++i) {
        desc.objects[i].fd = image->planes[i].fd;
        desc.objects[i].size = image->planes[i].size;
        desc.objects[i].drm_format_modifier = f->modifier;
        desc.layers[0].object_index[i] = i;
        desc.layers[0].offset[i] = image->planes[i].offset;
        desc.layers[0].pitch[i] = image->planes[i].pitch;
    }
    VASurfaceAttrib attrs[] = {
        {.type = VASurfaceAttribMemoryType,
         .flags = VA_SURFACE_ATTRIB_SETTABLE,
         .value = {.type = VAGenericValueTypeInteger,
                   .value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2}},
        {.type = VASurfaceAttribExternalBufferDescriptor,
         .flags = VA_SURFACE_ATTRIB_SETTABLE,
         .value = {.type = VAGenericValueTypePointer, .value.p = &desc}},
    };
    return va_error(vaCreateSurfaces(g->display, VA_RT_FORMAT_RGB32, desc.width, desc.height,
                                     surface, 1, attrs, 2),
                    "import exact DMA-BUF modifier");
}

void gk_gpu_unimport(struct gk_gpu *g, VASurfaceID *surface)
{
    if (*surface != VA_INVALID_ID && g->display)
        vaDestroySurfaces(g->display, surface, 1);
    *surface = VA_INVALID_ID;
}

int gk_gpu_convert_submit(struct gk_gpu *g, const struct gk_video_format *f, VASurfaceID source,
                          AVFrame *destination)
{
    if (!g->explicit_color)
        return -ENOTSUP;
    VAProcPipelineParameterBuffer params = {
        .surface = source,
        .surface_color_standard = VAProcColorStandardExplicit,
        .output_color_standard = VAProcColorStandardExplicit,
        .input_color_properties =
            {
                .color_range = VA_SOURCE_RANGE_FULL,
                .colour_primaries = AVCOL_PRI_BT709,
                .transfer_characteristics = (uint8_t)f->transfer,
                .matrix_coefficients = AVCOL_SPC_RGB,
            },
        .output_color_properties =
            {
                .color_range = VA_SOURCE_RANGE_REDUCED,
                .colour_primaries = AVCOL_PRI_BT709,
                .transfer_characteristics = (uint8_t)f->transfer,
                .matrix_coefficients = AVCOL_SPC_BT709,
                .chroma_sample_location =
                    VA_CHROMA_SITING_HORIZONTAL_LEFT | VA_CHROMA_SITING_VERTICAL_CENTER,
            },
    };
    VABufferID buffer = g->vpp_parameters;
    VASurfaceID dest = (VASurfaceID)(uintptr_t)destination->data[3];
    TracyCZoneN(zone, "gpu.vpp-submit", true);
    int ret;
#ifdef GK_TRANSIENT_VPP
    ret = va_error(vaCreateBuffer(g->display, g->vpp_context, VAProcPipelineParameterBufferType,
                                  sizeof(params), 1, &params, &buffer),
                   "VPP parameters");
#else
    void *mapped = nullptr;
    ret = va_error(vaMapBuffer(g->display, buffer, &mapped), "map pooled VPP parameters");
    if (ret >= 0) {
        memcpy(mapped, &params, sizeof(params));
        ret = va_error(vaUnmapBuffer(g->display, buffer), "publish pooled VPP parameters");
    }
#endif
    if (ret < 0)
        goto done;
    ret = va_error(vaBeginPicture(g->display, g->vpp_context, dest), "VPP begin");
    if (ret >= 0) {
        ret = va_error(vaRenderPicture(g->display, g->vpp_context, &buffer, 1), "VPP render");
        int end = va_error(vaEndPicture(g->display, g->vpp_context), "VPP submit");
        if (ret >= 0)
            ret = end;
    }
#ifdef GK_TRANSIENT_VPP
    vaDestroyBuffer(g->display, buffer);
#endif
done:
    TracyCZoneEnd(zone);
    return ret;
}

int gk_gpu_convert_wait(struct gk_gpu *g, AVFrame *destination)
{
    TracyCZoneN(zone, "gpu.source-read-completion", true);
    int ret = va_error(vaSyncSurface(g->display, (VASurfaceID)(uintptr_t)destination->data[3]),
                       "VPP source retirement");
    TracyCZoneEnd(zone);
    return ret;
}

void gk_gpu_close(struct gk_gpu *g)
{
    gk_vk_rgb_close(g);
    if (g->display && g->vpp_parameters != VA_INVALID_ID)
        vaDestroyBuffer(g->display, g->vpp_parameters);
    if (g->display && g->vpp_context != VA_INVALID_ID)
        vaDestroyContext(g->display, g->vpp_context);
    if (g->display && g->vpp_config != VA_INVALID_ID)
        vaDestroyConfig(g->display, g->vpp_config);
    av_buffer_unref(&g->device);
    g->display = nullptr;
    g->vpp_context = g->vpp_config = VA_INVALID_ID;
    g->vpp_parameters = VA_INVALID_ID;
}

int gk_gpu_probe(void)
{
    glob_t devices = {};
    if (glob("/dev/dri/renderD*", 0, nullptr, &devices) != 0) {
        puts("No DRM render nodes exposed.");
        return -ENODEV;
    }
    for (size_t i = 0; i < devices.gl_pathc; ++i) {
        struct gk_gpu gpu;
        int ret = gk_gpu_open(&gpu, GK_VAAPI, devices.gl_pathv[i]);
        printf("%s: %s\n", devices.gl_pathv[i], ret < 0 ? "AV1/VPP unavailable" : gpu.name);
        if (ret >= 0) {
            ret = gk_gpu_configure(&gpu, 64, 64);
            printf("  explicit VPP color: %s; allocated RGB modifiers:",
                   ret >= 0 && gpu.explicit_color ? "yes" : "no");
            for (unsigned j = 0; j < gpu.modifier_count; ++j)
                printf(" 0x%016llx", (unsigned long long)gpu.modifiers[j]);
            puts("");
            gk_gpu_close(&gpu);
        }
    }
    globfree(&devices);
    puts("Enumeration is eligibility evidence; use a synthetic encode/decode probe for "
         "functionality.");
    return 0;
}

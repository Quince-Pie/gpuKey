/* Owned RGB input control. DMA-BUF capture/lifetime is tested separately. */
#include <errno.h>
#include <fcntl.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/crc.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/hwcontext_vaapi.h>
#include <libavutil/hwcontext_vulkan.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/opt.h>
#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <va/va_drmcommon.h>

static PFN_vkGetInstanceProcAddr real_instance_proc;
static PFN_vkGetDeviceProcAddr real_device_proc;
static PFN_vkCreateVideoSessionKHR real_create_session;
static PFN_vkDestroyVideoSessionKHR real_destroy_session;
static PFN_vkCmdBeginVideoCodingKHR real_begin;
static PFN_vkQueueSubmit2 real_submit;
static PFN_vkGetFenceFdKHR real_fence_fd;
static PFN_vkCreateImageView real_create_view;
static PFN_vkDestroyImageView real_destroy_view;
static VkCommandBuffer encoding_command;
static unsigned encode_submissions, fence_exports;
static unsigned created_views;
static VkImageView reconstruction_views[37];
static unsigned reconstructions_created, reconstructions_destroyed;
static AVPacket *held_packet;
static uint32_t held_crc;
static const char *hold_packet_mode;
static unsigned device_destructions;
static bool device_tracked;
static void (*original_device_free)(AVHWDeviceContext *);

static void count_device_free(AVHWDeviceContext *device)
{
    ++device_destructions;
    if (original_device_free)
        original_device_free(device);
}

static void release_held_packet(void)
{
    if (!held_packet)
        return;
    if (held_crc !=
        av_crc(av_crc_get_table(AV_CRC_32_IEEE), 0, held_packet->data, (size_t)held_packet->size)) {
        fputs("Retained packet bytes changed.\n", stderr);
        abort();
    }
    av_packet_free(&held_packet);
}

static VkResult VKAPI_CALL create_view(VkDevice device, const VkImageViewCreateInfo *info,
                                       const VkAllocationCallbacks *allocator, VkImageView *view)
{
    if (getenv("GPUKEY_RGB_FAIL_VIEW") && ++created_views == 7) {
        fputs("Injected failure before seventh image-view creation.\n", stderr);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    bool reconstruction = false;
    for (const VkBaseInStructure *item = info->pNext; item; item = item->pNext)
        if (item->sType == VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO)
            reconstruction = ((const VkImageViewUsageCreateInfo *)item)->usage &
                             VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR;
    if (reconstruction && reconstructions_destroyed && getenv("GPUKEY_RGB_FAIL_RECON_RESTORE")) {
        fputs("Injected reconstruction-view failure after reclamation.\n", stderr);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    VkResult result = real_create_view(device, info, allocator, view);
    if (result == VK_SUCCESS && reconstruction) {
        for (unsigned i = 0; i < sizeof(reconstruction_views) / sizeof(*reconstruction_views); ++i)
            if (!reconstruction_views[i]) {
                reconstruction_views[i] = *view;
                ++reconstructions_created;
                return result;
            }
        abort(); /* This fixture never needs more than 37 simultaneous views. */
    }
    return result;
}

static void VKAPI_CALL destroy_view(VkDevice device, VkImageView view,
                                    const VkAllocationCallbacks *allocator)
{
    for (unsigned i = 0; i < sizeof(reconstruction_views) / sizeof(*reconstruction_views); ++i)
        if (view && reconstruction_views[i] == view) {
            reconstruction_views[i] = VK_NULL_HANDLE;
            ++reconstructions_destroyed;
            break;
        }
    real_destroy_view(device, view, allocator);
}

static void VKAPI_CALL begin_coding(VkCommandBuffer command, const VkVideoBeginCodingInfoKHR *info)
{
    encoding_command = command;
    real_begin(command, info);
}

static VkResult VKAPI_CALL fail_submit(VkQueue queue, uint32_t count, const VkSubmitInfo2 *submits,
                                       VkFence fence)
{
    bool encoding = false;
    for (uint32_t i = 0; i < count; ++i)
        for (uint32_t j = 0; j < submits[i].commandBufferInfoCount; ++j)
            encoding |= submits[i].pCommandBufferInfos[j].commandBuffer == encoding_command;
    if (encoding && ++encode_submissions == 7) {
        fputs("Injected failure before seventh encode submission (reused fence).\n", stderr);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    return real_submit(queue, count, submits, fence);
}

static VkResult VKAPI_CALL fail_export(VkDevice device, const VkFenceGetFdInfoKHR *info, int *fd)
{
    if (++fence_exports == 7) {
        fputs("Injected failure before seventh fence export (native payload retained).\n", stderr);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    return real_fence_fd(device, info, fd);
}

static VkResult VKAPI_CALL bad_session(VkDevice device, const VkVideoSessionCreateInfoKHR *info,
                                       const VkAllocationCallbacks *allocator,
                                       VkVideoSessionKHR *session)
{
    /* A negative validation control: preserve the RGB input format but remove
     * RGB conversion from the profile. Stop before submitting any GPU work. */
    VkVideoEncodeAV1ProfileInfoKHR codec = {.sType =
                                                VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_PROFILE_INFO_KHR,
                                            .stdProfile = STD_VIDEO_AV1_PROFILE_MAIN};
    VkVideoProfileInfoKHR profile = *info->pVideoProfile;
    profile.pNext = &codec;
    VkVideoSessionCreateInfoKHR bad = *info;
    bad.pVideoProfile = &profile;
    VkResult result = real_create_session(device, &bad, allocator, session);
    if (result == VK_SUCCESS) {
        real_destroy_session(device, *session, allocator);
        *session = VK_NULL_HANDLE;
    }
    return VK_ERROR_VALIDATION_FAILED_EXT;
}

static PFN_vkVoidFunction VKAPI_CALL device_proc(VkDevice device, const char *name)
{
    PFN_vkVoidFunction function = real_device_proc(device, name);
    if (getenv("GPUKEY_RGB_BAD_PROFILE") && !strcmp(name, "vkCreateVideoSessionKHR")) {
        real_create_session = (PFN_vkCreateVideoSessionKHR)function;
        real_destroy_session =
            (PFN_vkDestroyVideoSessionKHR)real_device_proc(device, "vkDestroyVideoSessionKHR");
        return (PFN_vkVoidFunction)bad_session;
    }
    if (getenv("GPUKEY_RGB_FAIL_SUBMIT")) {
        if (!strcmp(name, "vkCmdBeginVideoCodingKHR")) {
            real_begin = (PFN_vkCmdBeginVideoCodingKHR)function;
            return (PFN_vkVoidFunction)begin_coding;
        }
        if (!strcmp(name, "vkQueueSubmit2") || !strcmp(name, "vkQueueSubmit2KHR")) {
            real_submit = (PFN_vkQueueSubmit2)function;
            return (PFN_vkVoidFunction)fail_submit;
        }
    }
    if (getenv("GPUKEY_RGB_FAIL_EXPORT") && !strcmp(name, "vkGetFenceFdKHR")) {
        real_fence_fd = (PFN_vkGetFenceFdKHR)function;
        return (PFN_vkVoidFunction)fail_export;
    }
    if (getenv("GPUKEY_RGB_FAIL_VIEW") || getenv("GPUKEY_RGB_DPB_AUDIT") ||
        getenv("GPUKEY_RGB_FAIL_RECON_RESTORE")) {
        if (!strcmp(name, "vkCreateImageView")) {
            real_create_view = (PFN_vkCreateImageView)function;
            return (PFN_vkVoidFunction)create_view;
        }
        if (!strcmp(name, "vkDestroyImageView")) {
            real_destroy_view = (PFN_vkDestroyImageView)function;
            return (PFN_vkVoidFunction)destroy_view;
        }
    }
    return function;
}

static PFN_vkVoidFunction VKAPI_CALL instance_proc(VkInstance instance, const char *name)
{
    PFN_vkVoidFunction function = real_instance_proc(instance, name);
    if (!strcmp(name, "vkGetDeviceProcAddr")) {
        real_device_proc = (PFN_vkGetDeviceProcAddr)function;
        return (PFN_vkVoidFunction)device_proc;
    }
    return function;
}

static void fill(AVFrame *frame, unsigned bits, unsigned index, unsigned cube)
{
    static constexpr unsigned colors[8][3] = {{255, 255, 255}, {0, 255, 255}, {255, 255, 0},
                                              {0, 255, 0},     {255, 0, 255}, {0, 0, 255},
                                              {255, 0, 0},     {0, 0, 0}};
    for (int y = 0; y < frame->height; ++y)
        for (int x = 0; x < frame->width; ++x) {
            unsigned bar = (unsigned)((uint64_t)(unsigned)x * 8 / (unsigned)frame->width);
            unsigned b = colors[bar][0], g = colors[bar][1], r = colors[bar][2];
            if (cube) {
                r = (unsigned)x / 8 * 255 / (cube - 1);
                g = (unsigned)y / 8 * 255 / (cube - 1);
                b = index * 255 / (cube - 1);
            } else if (y >= frame->height / 2)
                b = g = r = ((unsigned)x + index) & 255U;
            uint8_t *p = frame->data[0] + (size_t)y * (unsigned)frame->linesize[0] + (size_t)x * 4;
            if (bits == 8) {
                bool rgba = frame->format == AV_PIX_FMT_RGBA;
                p[0] = (uint8_t)(rgba ? r : b);
                p[1] = (uint8_t)g;
                p[2] = (uint8_t)(rgba ? b : r);
                p[3] = 255;
            } else {
                b = b * 1023 / 255;
                g = g * 1023 / 255;
                r = r * 1023 / 255;
                AV_WL32(p, (r << 20) | (g << 10) | b);
            }
        }
}

static int receive(AVCodecContext *codec, AVFormatContext *mux, AVPacket *packet, unsigned *count)
{
    int ret;
    while ((ret = avcodec_receive_packet(codec, packet)) == 0) {
        if (hold_packet_mode && !held_packet) {
            held_packet = av_packet_clone(packet);
            if (!held_packet)
                return AVERROR(ENOMEM);
            held_crc = av_crc(av_crc_get_table(AV_CRC_32_IEEE), 0, held_packet->data,
                              (size_t)held_packet->size);
        }
        packet->stream_index = 0;
        av_packet_rescale_ts(packet, codec->time_base, mux->streams[0]->time_base);
        ret = av_interleaved_write_frame(mux, packet);
        av_packet_unref(packet);
        if (ret < 0)
            return ret;
        ++*count;
    }
    return ret == AVERROR(EAGAIN) || ret == AVERROR_EOF ? 0 : ret;
}

int main(int argc, char **argv)
{
    hold_packet_mode = getenv("GPUKEY_RGB_PACKET_HOLD");
    if (hold_packet_mode && strcmp(hold_packet_mode, "before") &&
        strcmp(hold_packet_mode, "encoder") && strcmp(hold_packet_mode, "device"))
        return 2;
    if ((argc != 3 && argc != 4) || (strcmp(argv[2], "8") && strcmp(argv[2], "10")) ||
        (argc == 4 && (strcmp(argv[3], "rgba") || strcmp(argv[2], "8")))) {
        fputs("Usage: rgb_encode_probe NEW_MKV 8|10 [rgba]\n", stderr);
        return 2;
    }
    unsigned bits = !strcmp(argv[2], "10") ? 10U : 8U;
    unsigned cube = 0;
    const char *cube_text = getenv("GPUKEY_RGB_CUBE");
    if (cube_text) {
        char *end;
        errno = 0;
        unsigned long value = strtoul(cube_text, &end, 10);
        if (errno || end == cube_text || *end || value < 2 || value > 256 || bits != 8 ||
            getenv("GPUKEY_RGB_SIZE") || getenv("GPUKEY_RGB_FRAMES"))
            return 2;
        cube = (unsigned)value;
    }
    unsigned frame_count = 60;
    const char *count_text = getenv("GPUKEY_RGB_FRAMES");
    if (count_text) {
        char *end;
        errno = 0;
        unsigned long count = strtoul(count_text, &end, 10);
        if (errno || end == count_text || *end || !count || count > INT_MAX)
            return 2;
        frame_count = (unsigned)count;
    }
    if (cube)
        frame_count = cube;
    int ret = AVERROR(EINVAL), output_fd = -1;
    unsigned packets = 0;
    AVBufferRef *device = nullptr, *frames = nullptr;
    AVBufferRef *va_device = nullptr, *va_frames = nullptr;
    AVFrame *va_source[2] = {}, *imported[2] = {};
    VADRMPRIMESurfaceDescriptor exported[2] = {};
    bool have_export[2] = {};
    bool drm_input = getenv("GPUKEY_RGB_DMABUF") != nullptr;
    VADisplay va_display = nullptr;
    AVCodecContext *codec = nullptr;
    AVFormatContext *mux = nullptr;
    AVFrame *cpu = av_frame_alloc(), *gpu = av_frame_alloc();
    AVPacket *packet = av_packet_alloc();
    AVDictionary *options = nullptr;
    av_log_set_level(AV_LOG_VERBOSE);
    if (!cpu || !gpu || !packet)
        goto out;
    av_dict_set(&options, "limit_queues", "1", 0);
    if (getenv("GPUKEY_RGB_STAGING"))
        av_dict_set(&options, "avoid_host_import", "1", 0);
    if (getenv("GPUKEY_RGB_VALIDATE"))
        av_dict_set(&options, "debug", "1", 0);
    ret = av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_VULKAN, "0", options, 0);
    av_dict_free(&options);
    if (ret < 0)
        goto out;
    if (hold_packet_mode) {
        AVHWDeviceContext *dc = (void *)device->data;
        original_device_free = dc->free;
        dc->free = count_device_free;
        device_tracked = true;
    }
    ret = AVERROR(ENOMEM);
    frames = av_hwframe_ctx_alloc(device);
    if (!frames)
        goto out;
    AVHWFramesContext *fc = (void *)frames->data;
    AVVulkanFramesContext *vkfc = fc->hwctx;
    fc->format = AV_PIX_FMT_VULKAN;
    fc->sw_format =
        bits == 8 ? (argc == 4 ? AV_PIX_FMT_RGBA : AV_PIX_FMT_BGRA) : AV_PIX_FMT_X2RGB10LE;
    fc->width = 320;
    fc->height = 240;
    if (cube)
        fc->width = fc->height = (int)cube * 8;
    const char *size = getenv("GPUKEY_RGB_SIZE");
    if (size) {
        char *end;
        errno = 0;
        unsigned long width = strtoul(size, &end, 10);
        if (errno || end == size || *end != 'x' || !width || width > INT_MAX) {
            ret = AVERROR(EINVAL);
            goto out;
        }
        const char *next = end + 1;
        unsigned long height = strtoul(next, &end, 10);
        if (errno || end == next || *end || !height || height > INT_MAX) {
            ret = AVERROR(EINVAL);
            goto out;
        }
        fc->width = (int)width;
        fc->height = (int)height;
    }
    vkfc->tiling = VK_IMAGE_TILING_OPTIMAL;
    vkfc->usage = VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkVideoProfileListInfoKHR independent = {.sType =
                                                 VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR};
    VkFormat views[] = {bits == 8 ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_A2R10G10B10_UNORM_PACK32,
                        VK_FORMAT_B8G8R8A8_UNORM};
    VkImageFormatListCreateInfo view_list = {.sType =
                                                 VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
                                             .pNext = &independent,
                                             .viewFormatCount = bits == 8 ? 2U : 1U,
                                             .pViewFormats = views};
    vkfc->create_pnext = &view_list;
    vkfc->img_flags =
        VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_VIDEO_PROFILE_INDEPENDENT_BIT_KHR;
    VkImageDrmFormatModifierListCreateInfoEXT modifiers = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
        .pNext = &independent,
        .drmFormatModifierCount = 1};
    if (drm_input) {
        ret = av_hwdevice_ctx_create(&va_device, AV_HWDEVICE_TYPE_VAAPI, "/dev/dri/renderD128",
                                     nullptr, 0);
        if (ret < 0)
            goto out;
        va_display =
            ((AVVAAPIDeviceContext *)((AVHWDeviceContext *)va_device->data)->hwctx)->display;
        va_frames = av_hwframe_ctx_alloc(va_device);
        ret = AVERROR(ENOMEM);
        if (!va_frames)
            goto out;
        AVHWFramesContext *vfc = (void *)va_frames->data;
        vfc->format = AV_PIX_FMT_VAAPI;
        vfc->sw_format = fc->sw_format;
        vfc->width = fc->width;
        vfc->height = fc->height;
        ret = av_hwframe_ctx_init(va_frames);
        if (ret < 0)
            goto out;
        for (unsigned i = 0; i < 2; ++i) {
            va_source[i] = av_frame_alloc();
            imported[i] = av_frame_alloc();
            ret = AVERROR(ENOMEM);
            if (!va_source[i] || !imported[i])
                goto out;
            ret = av_hwframe_get_buffer(va_frames, va_source[i], 0);
            if (ret < 0)
                goto out;
            VAStatus status = vaExportSurfaceHandle(
                va_display, (VASurfaceID)(uintptr_t)va_source[i]->data[3],
                VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_COMPOSED_LAYERS, &exported[i]);
            if (status != VA_STATUS_SUCCESS) {
                ret = AVERROR_EXTERNAL;
                goto out;
            }
            have_export[i] = true;
        }
        if (exported[0].objects[0].drm_format_modifier !=
            exported[1].objects[0].drm_format_modifier) {
            ret = AVERROR(ENOSYS);
            goto out;
        }
        modifiers.pDrmFormatModifiers = &exported[0].objects[0].drm_format_modifier;
        view_list.pNext = &modifiers;
        vkfc->tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
        vkfc->usage = VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR;
    }
    ret = av_hwframe_ctx_init(frames);
    if (ret < 0)
        goto out;
    if (drm_input) {
        for (unsigned i = 0; i < 2; ++i) {
            const VADRMPRIMESurfaceDescriptor *d = &exported[i];
            AVDRMFrameDescriptor descriptor = {.nb_objects = (int)d->num_objects,
                                               .nb_layers = (int)d->num_layers};
            for (unsigned j = 0; j < d->num_objects; ++j)
                descriptor.objects[j] =
                    (AVDRMObjectDescriptor){.fd = d->objects[j].fd,
                                            .size = d->objects[j].size,
                                            .format_modifier = d->objects[j].drm_format_modifier};
            for (unsigned j = 0; j < d->num_layers; ++j) {
                descriptor.layers[j].format = d->layers[j].drm_format;
                descriptor.layers[j].nb_planes = (int)d->layers[j].num_planes;
                for (unsigned k = 0; k < d->layers[j].num_planes; ++k)
                    descriptor.layers[j].planes[k] =
                        (AVDRMPlaneDescriptor){.object_index = (int)d->layers[j].object_index[k],
                                               .offset = d->layers[j].offset[k],
                                               .pitch = d->layers[j].pitch[k]};
            }
            ret = av_vk_frame_import_drm(frames, imported[i], &descriptor, fc->width, fc->height);
            if (ret < 0)
                goto out;
        }
        fprintf(stderr, "Persistent imports: 2, modifier=%#llx\n",
                (unsigned long long)modifiers.pDrmFormatModifiers[0]);
    }
    if (getenv("GPUKEY_RGB_BAD_PROFILE") || getenv("GPUKEY_RGB_FAIL_SUBMIT") ||
        getenv("GPUKEY_RGB_FAIL_EXPORT") || getenv("GPUKEY_RGB_FAIL_VIEW") ||
        getenv("GPUKEY_RGB_DPB_AUDIT") || getenv("GPUKEY_RGB_FAIL_RECON_RESTORE")) {
        AVHWDeviceContext *context = (void *)device->data;
        AVVulkanDeviceContext *vk = context->hwctx;
        real_instance_proc = vk->get_proc_addr;
        vk->get_proc_addr = instance_proc;
    }
    ret = AVERROR(ENOMEM);
    codec = avcodec_alloc_context3(avcodec_find_encoder_by_name("av1_vulkan"));
    if (!codec)
        goto out;
    codec->hw_frames_ctx = av_buffer_ref(frames);
    if (!codec->hw_frames_ctx)
        goto out;
    codec->width = fc->width;
    codec->height = fc->height;
    codec->pix_fmt = AV_PIX_FMT_VULKAN;
    codec->sw_pix_fmt = fc->sw_format;
    codec->time_base = (AVRational){1, 60};
    codec->framerate = (AVRational){60, 1};
    codec->sample_aspect_ratio = (AVRational){1, 1};
    codec->gop_size = 120;
    codec->max_b_frames = 0;
    const char *trim = getenv("GPUKEY_RGB_TRIM");
    if (trim && (ret = av_opt_set(codec->priv_data, "async_trim", trim, 0)) < 0)
        goto out;
    codec->color_primaries = AVCOL_PRI_BT709;
    codec->colorspace = AVCOL_SPC_BT709;
    codec->color_trc = AVCOL_TRC_IEC61966_2_1;
    codec->color_range = AVCOL_RANGE_MPEG;
    if (getenv("GPUKEY_RGB_FULL_RANGE"))
        codec->color_range = AVCOL_RANGE_JPEG;
    if (getenv("GPUKEY_RGB_BT2020")) {
        codec->colorspace = AVCOL_SPC_BT2020_NCL;
        codec->color_primaries = AVCOL_PRI_BT2020;
    }
    if (getenv("GPUKEY_RGB_MATRIX_2020"))
        codec->colorspace = AVCOL_SPC_BT2020_NCL;
    codec->chroma_sample_location = AVCHROMA_LOC_LEFT;
    codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    av_dict_set(&options, "rc_mode", "cqp", 0);
    av_dict_set(&options, "qp", "32", 0);
    if (getenv("GPUKEY_RGB_QP"))
        av_dict_set(&options, "qp", getenv("GPUKEY_RGB_QP"), 0);
    av_dict_set(&options, "async_depth", "2", 0);
    if (getenv("GPUKEY_RGB_EVENTS"))
        av_dict_set(&options, "async_events", "1", 0);
    if (getenv("GPUKEY_RGB_QUEUE_DPB"))
        av_dict_set(&options, "queue_dpb", "1", 0);
    ret = avcodec_open2(codec, nullptr, &options);
    if (ret >= 0 && av_dict_count(options))
        ret = AVERROR(ENOSYS);
    av_dict_free(&options);
    if (ret < 0)
        goto out;
    cpu->format = fc->sw_format;
    cpu->width = fc->width;
    cpu->height = fc->height;
    ret = av_frame_get_buffer(cpu, 32);
    if (ret < 0)
        goto out;
    ret = avformat_alloc_output_context2(&mux, nullptr, "matroska", argv[1]);
    if (ret < 0 || !mux)
        goto out;
    ret = AVERROR(ENOMEM);
    AVStream *stream = avformat_new_stream(mux, nullptr);
    if (!stream)
        goto out;
    stream->time_base = codec->time_base;
    ret = avcodec_parameters_from_context(stream->codecpar, codec);
    if (ret < 0)
        goto out;
    output_fd = open(argv[1], O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
    if (output_fd < 0) {
        ret = AVERROR(errno);
        goto out;
    }
    char owned_path[64];
    snprintf(owned_path, sizeof(owned_path), "/proc/self/fd/%d", output_fd);
    ret = avio_open(&mux->pb, owned_path, AVIO_FLAG_WRITE);
    if (ret < 0)
        goto out;
    ret = avformat_write_header(mux, nullptr);
    if (ret < 0)
        goto out;
    for (unsigned i = 0; i < frame_count; ++i) {
        AVFrame *input = drm_input ? imported[i % 2] : gpu;
        if (drm_input && av_buffer_get_ref_count(input->buf[0]) != 1) {
            ret = AVERROR(EBUSY); /* No producer write while the encoder can read. */
            goto out;
        }
        ret = av_frame_make_writable(cpu);
        if (ret < 0)
            goto out;
        fill(cpu, bits, getenv("GPUKEY_RGB_STATIC") ? 0 : i, cube);
        if (drm_input) {
            ret = av_hwframe_transfer_data(va_source[i % 2], cpu, 0);
            if (ret < 0)
                goto out;
            if (vaSyncSurface(va_display, (VASurfaceID)(uintptr_t)va_source[i % 2]->data[3]) !=
                VA_STATUS_SUCCESS) {
                ret = AVERROR_EXTERNAL;
                goto out;
            }
            /* A derived-image unmap can submit a copy without associating its
             * fence with the VA surface. Like the recorder, wait for the
             * DMA-BUF producer writes before an external Vulkan read. */
            for (unsigned j = 0; j < exported[i % 2].num_objects; ++j) {
                struct pollfd producer = {.fd = exported[i % 2].objects[j].fd, .events = POLLIN};
                do {
                    ret = poll(&producer, 1, -1);
                } while (ret < 0 && errno == EINTR);
                if (ret < 0 || !(producer.revents & POLLIN) ||
                    (producer.revents & (POLLERR | POLLHUP | POLLNVAL))) {
                    ret = ret < 0 ? AVERROR(errno) : AVERROR(EIO);
                    goto out;
                }
            }
        } else {
            if (i == 0 && getenv("GPUKEY_RGB_RETRY_ALLOC"))
                fputs("ALLOCATE first owned input frame\n", stderr);
            ret = av_hwframe_get_buffer(frames, gpu, 0);
            if (ret < 0 && i == 0 && getenv("GPUKEY_RGB_RETRY_ALLOC")) {
                fputs("Retrying initial frame allocation after injected failure.\n", stderr);
                av_frame_unref(gpu);
                ret = av_hwframe_get_buffer(frames, gpu, 0);
            }
            if (ret < 0)
                goto out;
            ret = av_hwframe_transfer_data(gpu, cpu, 0);
            if (ret < 0)
                goto out;
        }
        input->pts = i;
        input->duration = 1;
        input->pict_type = getenv("GPUKEY_RGB_FORCED_KEYS") && (i == 17 || i == 41 || i == 121)
                               ? AV_PICTURE_TYPE_I
                               : AV_PICTURE_TYPE_NONE;
        ret = avcodec_send_frame(codec, input);
        if (!drm_input)
            av_frame_unref(gpu);
        if (ret < 0 || (ret = receive(codec, mux, packet, &packets)) < 0)
            goto out;
    }
    ret = avcodec_send_frame(codec, nullptr);
    if (ret < 0 || (ret = receive(codec, mux, packet, &packets)) < 0)
        goto out;
    ret = av_write_trailer(mux);
    if (ret >= 0 && packets != frame_count)
        ret = AVERROR_INVALIDDATA;
out:
    av_dict_free(&options);
    if (mux && mux->pb) {
        int close_result = avio_closep(&mux->pb);
        if (ret >= 0 && close_result < 0)
            ret = close_result;
    }
    avformat_free_context(mux);
    if (output_fd >= 0)
        close(output_fd);
    if (hold_packet_mode && !strcmp(hold_packet_mode, "before"))
        release_held_packet();
    if (getenv("GPUKEY_RGB_DPB_AUDIT"))
        fprintf(stderr, "DPB_AUDIT created=%u retired=%u live=%u\n", reconstructions_created,
                reconstructions_destroyed, reconstructions_created - reconstructions_destroyed);
    avcodec_free_context(&codec);
    if (reconstructions_created != reconstructions_destroyed)
        abort();
    if (hold_packet_mode && !strcmp(hold_packet_mode, "encoder"))
        release_held_packet();
    for (unsigned i = 0; i < 2; ++i) {
        av_frame_free(&imported[i]);
        if (have_export[i])
            for (unsigned j = 0; j < exported[i].num_objects; ++j)
                close(exported[i].objects[j].fd);
        av_frame_free(&va_source[i]);
    }
    av_buffer_unref(&va_frames);
    av_buffer_unref(&va_device);
    av_frame_free(&cpu);
    av_frame_free(&gpu);
    av_packet_free(&packet);
    av_buffer_unref(&frames);
    av_buffer_unref(&device);
    if (held_packet) {
        if (device_destructions) {
            fputs("Device destroyed before retained packet release.\n", stderr);
            abort(); /* The pool must retain the device through the final packet. */
        }
        release_held_packet();
    }
    if (device_tracked && device_destructions != 1)
        abort();
    char description[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(ret, description, sizeof(description));
    fprintf(stderr, "RGB%u encode: %u packets, %s\n", bits, packets, description);
    return ret < 0;
}

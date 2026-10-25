#include "core.h"
#include "gpu.h"
#include "pattern.h"
#include "portal_mock.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <limits.h>
#include <math.h>
#include <pipewire/pipewire.h>
#include <signal.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/port-config.h>
#include <spa/param/video/format-utils.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <va/va_drmcommon.h>

#pragma GCC diagnostic ignored "-Wpedantic"

extern char **environ;
struct fixture;
struct audio_fixture {
    struct fixture *fixture;
    struct pw_stream *stream;
    struct pw_node *input_proxy;
    struct pw_proxy *link;
    struct spa_hook hook;
    uint32_t source_node, input_node, output_port, input_port;
    unsigned channels, rate;
    uint64_t position;
    int64_t origin;
    struct spa_io_clock *clock;
};
struct fixture {
    struct pw_main_loop *loop;
    struct pw_context *context;
    struct pw_core *core;
    struct pw_registry *registry;
    struct pw_stream *stream;
    struct pw_proxy *link;
    struct spa_hook stream_hook, registry_hook, core_hook;
    struct spa_source *timer;
    uint32_t source_node, input_node, output_port, input_port;
    const char *binary, *output;
    uint64_t sequence, video_tick, first_tick, last_tick;
    pid_t child;
    pid_t saves[2];
    char second[PATH_MAX];
    int64_t start, video_origin, last_produced;
    int64_t pause_begin, pause_end;
    int result;
    bool spawned;
    bool gpu_mode;
    bool revoke, revoked, still, audio_mode, audio_revoke, fps_mode;
    bool list_mode, aux_mode, pcm_mode, replay_mode, save_failed, duration_boundary;
    bool fixate_mode, fixated, saw_proposal;
    bool reconfigure_mode, reconfigured;
    bool missing_pts;
    bool cycle_pts, zero_epoch, same_sequence, lost_pts, lost_cycle;
    bool portal_mode, paced_pts, scene, late_saves, early_saves, external_mode;
    struct gk_portal_mock portal;
    char remote_path[108], duration[32];
    unsigned duration_seconds, video_rate, buffer_min, buffer_max;
    unsigned allocated_buffers, peak_buffers;
    uint64_t allocated_bytes, peak_bytes, empty_processes, coalesced_ticks;
    struct spa_io_clock *video_clock;
    uint64_t modifier;
    unsigned save_count;
    unsigned selection_failure;
    struct audio_fixture audio[2];
    struct gk_gpu gpu;
    int width, height;
    VAImage upload;
    struct {
        bool used;
        VASurfaceID surface;
        VADRMPRIMESurfaceDescriptor desc;
    } buffers[8];
};

static void add_buffer(void *opaque, struct pw_buffer *buffer)
{
    struct fixture *f = opaque;
    if (!f->gpu_mode)
        return;
    unsigned index;
    for (index = 0; index < 8 && f->buffers[index].used; ++index) {
    }
    if (index == 8)
        abort();
    f->buffers[index].used = true;
    buffer->user_data = &f->buffers[index];
    uint64_t modifier = f->modifier;
    VADRMFormatModifierList list = {.num_modifiers = 1, .modifiers = &modifier};
    VASurfaceAttrib attrs[] = {
        {.type = VASurfaceAttribPixelFormat,
         .flags = VA_SURFACE_ATTRIB_SETTABLE,
         .value = {.type = VAGenericValueTypeInteger, .value.i = VA_FOURCC_BGRA}},
        {.type = VASurfaceAttribDRMFormatModifiers,
         .flags = VA_SURFACE_ATTRIB_SETTABLE,
         .value = {.type = VAGenericValueTypePointer, .value.p = &list}},
    };
    if (vaCreateSurfaces(f->gpu.display, VA_RT_FORMAT_RGB32, (unsigned)f->width,
                         (unsigned)f->height, &f->buffers[index].surface, 1, attrs,
                         2) != VA_STATUS_SUCCESS ||
        vaExportSurfaceHandle(f->gpu.display, f->buffers[index].surface,
                              VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                              VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_COMPOSED_LAYERS,
                              &f->buffers[index].desc) != VA_STATUS_SUCCESS)
        abort();
    VADRMPRIMESurfaceDescriptor *d = &f->buffers[index].desc;
    ++f->allocated_buffers;
    if (f->allocated_buffers > f->peak_buffers)
        f->peak_buffers = f->allocated_buffers;
    for (unsigned i = 0; i < d->num_objects; ++i)
        f->allocated_bytes += d->objects[i].size;
    if (f->allocated_bytes > f->peak_bytes)
        f->peak_bytes = f->allocated_bytes;
    if (d->num_objects != 1 || d->num_layers != 1 || d->layers[0].num_planes != 1 ||
        d->objects[0].drm_format_modifier != f->modifier || buffer->buffer->n_datas != 1)
        abort();
    struct spa_data *data = &buffer->buffer->datas[0];
    data->type = SPA_DATA_DmaBuf;
    data->flags = SPA_DATA_FLAG_READABLE;
    data->fd = d->objects[0].fd;
    data->mapoffset = 0;
    /* Regression modes exercise zero-size DMA-BUF descriptors. A cross-recorder
     * producer supplies the allocation extent for consumers that require it. */
    if (d->objects[0].size > UINT32_MAX)
        abort();
    data->maxsize = f->external_mode ? (uint32_t)d->objects[0].size : 0;
    data->data = nullptr;
    *data->chunk = (struct spa_chunk){.offset = d->layers[0].offset[0],
                                      .stride = (int32_t)d->layers[0].pitch[0],
                                      .size = data->maxsize};
}

static void remove_buffer(void *opaque, struct pw_buffer *buffer)
{
    struct fixture *f = opaque;
    if (!f->gpu_mode || !buffer->user_data)
        return;
    typeof(f->buffers[0]) *item = buffer->user_data;
    --f->allocated_buffers;
    for (unsigned i = 0; i < item->desc.num_objects; ++i)
        f->allocated_bytes -= item->desc.objects[i].size;
    for (unsigned i = 0; i < item->desc.num_objects; ++i)
        close(item->desc.objects[i].fd);
    vaDestroySurfaces(f->gpu.display, &item->surface, 1);
    item->used = false;
    buffer->user_data = nullptr;
}

static void spawn_recorder(struct fixture *f);

static void maybe_spawn(struct fixture *f)
{
    if (!f->spawned && f->source_node &&
        (!f->audio_mode || (f->audio[0].source_node && f->audio[1].source_node))) {
        f->spawned = true;
        if (f->portal_mode)
            atomic_store(&f->portal.node, f->source_node);
        spawn_recorder(f);
    }
}

static void audio_state(void *opaque, enum pw_stream_state old, enum pw_stream_state state,
                        const char *message)
{
    (void)old;
    struct audio_fixture *a = opaque;
    if (state == PW_STREAM_STATE_ERROR) {
        fprintf(stderr, "fixture audio: %s\n", message);
        a->fixture->result = 1;
        pw_main_loop_quit(a->fixture->loop);
    }
    if (state == PW_STREAM_STATE_PAUSED) {
        a->source_node = pw_stream_get_node_id(a->stream);
        maybe_spawn(a->fixture);
    }
}

static void audio_param(void *opaque, uint32_t id, const struct spa_pod *param)
{
    struct audio_fixture *a = opaque;
    if (id != SPA_PARAM_Format || !param)
        return;
    uint8_t storage[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
    unsigned stride = a->channels * sizeof(float);
    const struct spa_pod *params[2];
    params[0] = spa_pod_builder_add_object(
        &b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers, SPA_PARAM_BUFFERS_buffers,
        SPA_POD_CHOICE_RANGE_Int(8, 2, 8), SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
        SPA_PARAM_BUFFERS_size, SPA_POD_Int((int)(4096 * stride)), SPA_PARAM_BUFFERS_stride,
        SPA_POD_Int((int)stride),
        /* Audio's port mixer consumes mapped pointers. PipeWire transports
         * MemPtr data inline in shared memory; raw MemFd-only data bypasses
         * that mapping and is rejected by the mixer before the stream sees it. */
        SPA_PARAM_BUFFERS_dataType,
        SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemPtr) | (1 << SPA_DATA_MemFd)));
    params[1] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
                                           SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
                                           SPA_PARAM_META_size,
                                           SPA_POD_Int(sizeof(struct spa_meta_header)));
    if (pw_stream_update_params(a->stream, params, 2) < 0)
        abort();
}

static void audio_process(void *opaque)
{
    struct audio_fixture *a = opaque;
    struct pw_buffer *buffer = pw_stream_dequeue_buffer(a->stream);
    if (!buffer)
        return;
    struct spa_buffer *b = buffer->buffer;
    unsigned n = a->rate / 50;
    if (b->n_datas != 1 || !b->datas[0].data ||
        b->datas[0].maxsize < n * a->channels * sizeof(float))
        abort();
    float *samples = b->datas[0].data;
    for (unsigned i = 0; i < n; ++i)
        for (unsigned ch = 0; ch < a->channels; ++ch)
            samples[i * a->channels + ch] =
                a->fixture->aux_mode ? (float)(0.15 * sin(6.2831853071795864769 * (300 + ch * 29) *
                                                          (double)(a->position + i) / a->rate))
                                     : (float)(ch + 1) * 0.03f;
    /* A known event on the source clock tests initial A/V alignment, not just
     * duration and phase-insensitive channel recognition. */
    if (a->position <= a->rate / 5 && a->position + n > a->rate / 5)
        samples[(a->rate / 5 - a->position) * a->channels] = 0.8f;
    if (!a->position)
        a->origin = gk_now_ns();
    if (a->clock) {
        /* A stream used as a synthetic graph driver must publish its clock;
         * unlike video, audio mixers do not necessarily preserve Header. */
        a->clock->nsec = (uint64_t)(a->origin + (int64_t)a->position * GK_NS / a->rate);
        a->clock->rate = SPA_FRACTION(1, a->rate);
        a->clock->position = a->position;
        a->clock->duration = n;
        a->clock->delay = 0;
        a->clock->rate_diff = 1;
        a->clock->next_nsec = a->clock->nsec + (uint64_t)n * (uint64_t)GK_NS / a->rate;
    }
    struct spa_meta_header *header = spa_buffer_find_meta_data(b, SPA_META_Header, sizeof(*header));
    if (!header)
        abort();
    *header = (struct spa_meta_header){.pts = a->origin + (int64_t)a->position * GK_NS / a->rate,
                                       .seq = a->position / n};
    *b->datas[0].chunk = (struct spa_chunk){.size = n * a->channels * sizeof(float),
                                            .stride = (int32_t)(a->channels * sizeof(float))};
    a->position += n;
    buffer->size = n;
    pw_stream_queue_buffer(a->stream, buffer);
}

static void audio_io(void *opaque, uint32_t id, void *area, uint32_t size)
{
    struct audio_fixture *a = opaque;
    if (id == SPA_IO_Clock)
        a->clock = area && size >= sizeof(*a->clock) ? area : nullptr;
}

static const struct pw_stream_events audio_events = {.version = PW_VERSION_STREAM_EVENTS,
                                                     .state_changed = audio_state,
                                                     .param_changed = audio_param,
                                                     .process = audio_process,
                                                     .io_changed = audio_io};

static struct spa_pod *video_format(struct fixture *f, struct spa_pod_builder *b, bool fixed)
{
    struct spa_pod_frame object;
    spa_pod_builder_push_object(b, &object, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
    spa_pod_builder_add(
        b, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), SPA_FORMAT_mediaSubtype,
        SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), SPA_FORMAT_VIDEO_format,
        SPA_POD_Id(SPA_VIDEO_FORMAT_BGRA), SPA_FORMAT_VIDEO_size,
        SPA_POD_Rectangle(&SPA_RECTANGLE((unsigned)f->width, (unsigned)f->height)),
        SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&SPA_FRACTION(0, 1)),
        SPA_FORMAT_VIDEO_maxFramerate,
        SPA_POD_Fraction(&SPA_FRACTION(f->fps_mode    ? 25
                                       : f->paced_pts ? f->video_rate
                                                      : 60,
                                       1)),
        SPA_FORMAT_VIDEO_colorRange, SPA_POD_Id(SPA_VIDEO_COLOR_RANGE_0_255),
        SPA_FORMAT_VIDEO_colorMatrix, SPA_POD_Id(SPA_VIDEO_COLOR_MATRIX_RGB),
        SPA_FORMAT_VIDEO_colorPrimaries, SPA_POD_Id(SPA_VIDEO_COLOR_PRIMARIES_BT709),
        SPA_FORMAT_VIDEO_transferFunction,
        SPA_POD_Id(f->reconfigured ? SPA_VIDEO_TRANSFER_BT709 : SPA_VIDEO_TRANSFER_SRGB), 0);
    if (f->gpu_mode) {
        spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_modifier,
                             SPA_POD_PROP_FLAG_MANDATORY |
                                 (fixed ? 0 : SPA_POD_PROP_FLAG_DONT_FIXATE));
        if (fixed)
            spa_pod_builder_long(b, (int64_t)f->modifier);
        else {
            struct spa_pod_frame choice;
            spa_pod_builder_push_choice(b, &choice, SPA_CHOICE_Enum, 0);
            spa_pod_builder_long(b, (int64_t)f->modifier);
            for (unsigned i = 0; i < f->gpu.modifier_count; ++i)
                spa_pod_builder_long(b, (int64_t)f->gpu.modifiers[i]);
            spa_pod_builder_pop(b, &choice);
        }
    }
    return spa_pod_builder_pop(b, &object);
}

static void tick(void *opaque, uint64_t expirations)
{
    struct fixture *f = opaque;
    f->video_tick += expirations;
    if (expirations > 1)
        f->coalesced_ticks += expirations - 1;
    if (f->replay_mode) {
        for (unsigned i = 0; i < f->save_count; ++i)
            if (f->saves[i] > 0) {
                int status;
                if (waitpid(f->saves[i], &status, WNOHANG) == f->saves[i]) {
                    f->saves[i] = 0;
                    f->save_failed |= !WIFEXITED(status) || WEXITSTATUS(status) != 0;
                }
            }
        int64_t save_at = f->save_count ? 3300000000 : 1300000000;
        if (f->early_saves)
            save_at = f->save_count ? 350000000 : 150000000;
        if (f->late_saves)
            save_at =
                (int64_t)f->duration_seconds * GK_NS - (f->save_count ? 700000000 : 2700000000);
        if (f->video_origin && f->save_count < (f->revoke ? 1U : 2U) &&
            gk_now_ns() - f->video_origin >= save_at) {
            unsigned i = f->save_count++;
            char *args[] = {(char *)f->binary, "save",    (char *)(i ? f->second : f->output),
                            "--session",       "fixture", nullptr};
            if (posix_spawn(&f->saves[i], f->binary, nullptr, nullptr, args, environ)) {
                f->saves[i] = 0;
                f->save_failed = true;
            }
        }
    }
    if (f->child > 0) {
        int status;
        pid_t result = waitpid(f->child, &status, WNOHANG);
        if (result == f->child) {
            f->child = 0;
            f->result =
                WIFEXITED(status) && WEXITSTATUS(status) ==
                                         ((f->revoke || f->audio_revoke || f->selection_failure ||
                                           f->reconfigure_mode || f->lost_pts || f->lost_cycle)
                                              ? 1
                                              : 0)
                    ? 0
                    : 1;
            pw_main_loop_quit(f->loop);
            return;
        }
    }
    if (gk_now_ns() - f->start > ((int64_t)f->duration_seconds + 10) * GK_NS) {
        f->result = 1;
        pw_main_loop_quit(f->loop);
        return;
    }
    if (f->reconfigure_mode && !f->reconfigured && f->sequence >= 20) {
        f->reconfigured = true;
        uint8_t storage[1024];
        struct spa_pod_builder b = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
        const struct spa_pod *param = video_format(f, &b, true);
        if (pw_stream_update_params(f->stream, &param, 1) < 0) {
            f->result = 1;
            pw_main_loop_quit(f->loop);
        }
        return;
    }
    if (f->audio_revoke && !f->revoked && f->sequence >= 20) {
        f->revoked = true;
        pw_stream_disconnect(f->audio[1].stream);
    }
    for (unsigned i = 0; i < 2 && f->audio_mode; ++i)
        if (pw_stream_get_state(f->audio[i].stream, nullptr) == PW_STREAM_STATE_STREAMING)
            pw_stream_trigger_process(f->audio[i].stream);
    if (f->revoke && !f->revoked && f->sequence >= (f->replay_mode ? 90U : 20U)) {
        f->revoked = true;
        pw_stream_disconnect(f->stream);
    }
    int64_t position = f->video_origin ? gk_now_ns() - f->video_origin : -1;
    bool paused = f->pause_end && position >= f->pause_begin && position < f->pause_end;
    if (!paused && pw_stream_get_state(f->stream, nullptr) == PW_STREAM_STATE_STREAMING &&
        (!f->still || f->sequence == 0))
        pw_stream_trigger_process(f->stream);
}

static void draw_fixture_image(struct fixture *f, uint8_t *pixels, uint32_t pitch)
{
    if (f->scene) {
        if (pitch % 4 || pitch / 4 > INT_MAX)
            abort();
        gk_test_pattern(pixels, (int)(pitch / 4), f->height, f->sequence);
    }
    for (int y = 0; y < f->height; ++y) {
        int from = 0, to = f->width;
        if (f->scene) {
            /* Keep an independently checkable moving black/white marker
             * inside the more complex bars/gradient/thin-line image. */
            if (abs(y - f->height / 2) > 32)
                continue;
            from = f->width / 2 > 32 ? f->width / 2 - 32 : 0;
            to = f->width / 2 + 32 < f->width ? f->width / 2 + 32 : f->width;
        }
        memset(pixels + (size_t)y * pitch + (size_t)from * 4, f->sequence & 1 ? 255 : 0,
               (size_t)(to - from) * 4);
    }
    if (f->external_mode && f->width >= 256 && f->height >= 16) {
        /* Frame identity plus its complement survives ordinary lossy
         * quantization. External analysis checks actual pictures, including
         * repeats, gaps and torn identities, independently of timestamps. */
        for (int y = 0; y < 16; ++y)
            for (unsigned bit = 0; bit < 32; ++bit) {
                uint8_t value = (((f->sequence >> bit) & 1U) != (y >= 8)) ? 255 : 0;
                uint8_t *p = pixels + (size_t)y * pitch + bit * 32;
                for (unsigned x = 0; x < 8; ++x) {
                    p[x * 4] = p[x * 4 + 1] = p[x * 4 + 2] = value;
                    p[x * 4 + 3] = 255;
                }
            }
    }
}

static void process(void *opaque)
{
    struct fixture *f = opaque;
    struct pw_buffer *buffer = pw_stream_dequeue_buffer(f->stream);
    if (!buffer) {
        ++f->empty_processes;
        return;
    }
    struct spa_buffer *b = buffer->buffer;
    if (b->n_datas != 1 || (!f->gpu_mode && !b->datas[0].data)) {
        f->result = 1;
        pw_main_loop_quit(f->loop);
        return;
    }
    if (f->gpu_mode) {
        void *mapped;
        typeof(f->buffers[0]) *item = buffer->user_data;
        if (vaMapBuffer(f->gpu.display, f->upload.buf, &mapped) != VA_STATUS_SUCCESS)
            abort();
        uint8_t *pixels = (uint8_t *)mapped + f->upload.offsets[0];
        draw_fixture_image(f, pixels, f->upload.pitches[0]);
        vaUnmapBuffer(f->gpu.display, f->upload.buf);
        if (vaPutImage(f->gpu.display, item->surface, f->upload.image_id, 0, 0, (unsigned)f->width,
                       (unsigned)f->height, 0, 0, (unsigned)f->width,
                       (unsigned)f->height) != VA_STATUS_SUCCESS ||
            vaSyncSurface(f->gpu.display, item->surface) != VA_STATUS_SUCCESS)
            abort();
    } else {
        draw_fixture_image(f, b->datas[0].data, (uint32_t)(f->width * 4));
        *b->datas[0].chunk = (struct spa_chunk){
            .offset = 0, .stride = f->width * 4, .size = (uint32_t)(f->width * f->height * 4)};
    }
    int64_t now = gk_now_ns();
    f->last_produced = now;
    if (!f->sequence) {
        f->video_origin = now;
        f->first_tick = f->video_tick;
    }
    struct spa_meta_header *h = spa_buffer_find_meta_data(b, SPA_META_Header, sizeof(*h));
    if (h) {
        int64_t pts = f->duration_boundary ? f->video_origin + (int64_t)f->sequence * 20000000 -
                                                 (f->sequence == 49 ? 5000000 : 0)
                                           : now;
        if (f->paced_pts) {
            if (f->sequence && f->video_tick <= f->last_tick)
                abort();
            /* Missing producer buffers/timer ticks advance time too. Counting
             * only successful images would slow the oracle's presentation clock
             * under overload and conceal lost source updates. */
            pts = f->video_origin + av_rescale_q((int64_t)(f->video_tick - f->first_tick),
                                                 (AVRational){1, (int)f->video_rate},
                                                 (AVRational){1, 1000000000});
            f->last_tick = f->video_tick;
        }
        if (f->cycle_pts && f->video_clock && (!f->lost_cycle || f->sequence < 20)) {
            f->video_clock->nsec = (uint64_t)pts;
            f->video_clock->rate = SPA_FRACTION(1, 50);
            f->video_clock->position = f->sequence;
            f->video_clock->duration = 1;
            f->video_clock->rate_diff = 1;
            f->video_clock->next_nsec = (uint64_t)pts + 20000000;
        }
        if (f->zero_epoch)
            pts = (int64_t)f->sequence * 20000000;
        if (f->missing_pts || (f->lost_pts && f->sequence >= 20))
            pts = 0;
        *h = (struct spa_meta_header){.pts = pts, .seq = f->same_sequence ? 0 : f->sequence};
    }
    ++f->sequence; /* Source progression does not require consumer-requested metadata. */
    struct spa_meta_region *crop = spa_buffer_find_meta_data(b, SPA_META_VideoCrop, sizeof(*crop));
    if (crop)
        *crop = (struct spa_meta_region){
            .region = SPA_REGION(0, 0, (unsigned)f->width, (unsigned)f->height)};
    struct spa_meta *damage = spa_buffer_find_meta(b, SPA_META_VideoDamage);
    if (damage && damage->size >= sizeof(struct spa_meta_region)) {
        memset(damage->data, 0, damage->size);
        *(struct spa_meta_region *)damage->data = (struct spa_meta_region){
            .region = SPA_REGION(0, 0, (unsigned)f->width, (unsigned)f->height)};
    }
    pw_stream_queue_buffer(f->stream, buffer);
}

static void param_changed(void *opaque, uint32_t id, const struct spa_pod *param)
{
    struct fixture *f = opaque;
    if (id != SPA_PARAM_Format || !param)
        return;
    struct spa_video_info_raw raw = {};
    if (spa_format_video_raw_parse(param, &raw) < 0)
        abort();
    if (raw.flags & SPA_VIDEO_FLAG_MODIFIER_FIXATION_REQUIRED) {
        if (!f->fixate_mode)
            abort();
        f->saw_proposal = true;
        uint8_t storage[2048];
        struct spa_pod_builder b = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
        /* Match PipeWire's producer protocol: publish the preferred fixed
         * EnumFormat followed by the original offer before this callback ends. */
        const struct spa_pod *params[] = {video_format(f, &b, true), video_format(f, &b, false)};
        f->fixated = true;
        if (pw_stream_update_params(f->stream, params, 2) < 0) {
            f->result = 1;
            pw_main_loop_quit(f->loop);
        }
        return;
    }
    if (f->fixate_mode && (!f->fixated || raw.modifier != f->modifier))
        abort();
    uint8_t bytes[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(bytes, sizeof(bytes));
    const struct spa_pod *params[3];
    params[0] = spa_pod_builder_add_object(
        &b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers, SPA_PARAM_BUFFERS_buffers,
        SPA_POD_CHOICE_RANGE_Int((int)f->buffer_max, (int)f->buffer_min, (int)f->buffer_max),
        SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1), SPA_PARAM_BUFFERS_size,
        SPA_POD_Int(f->gpu_mode ? 0 : f->width * f->height * 4), SPA_PARAM_BUFFERS_stride,
        SPA_POD_Int(f->width * 4), SPA_PARAM_BUFFERS_dataType,
        SPA_POD_CHOICE_FLAGS_Int(1 << (f->gpu_mode ? SPA_DATA_DmaBuf : SPA_DATA_MemFd)));
    params[1] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
                                           SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
                                           SPA_PARAM_META_size,
                                           SPA_POD_Int(sizeof(struct spa_meta_header)));
    params[2] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
                                           SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoCrop),
                                           SPA_PARAM_META_size,
                                           SPA_POD_Int(sizeof(struct spa_meta_region)));
    if (pw_stream_update_params(f->stream, params, 3) < 0) {
        f->result = 1;
        pw_main_loop_quit(f->loop);
    }
}

static void spawn_recorder(struct fixture *f)
{
    if (f->external_mode) {
        /* A consumer-neutral runner owns options, stop and save behavior. Its
         * output is verified independently, outside gpuKey-specific oracles. */
        char width[16], height[16], rate[16];
        snprintf(width, sizeof(width), "%d", f->width);
        snprintf(height, sizeof(height), "%d", f->height);
        snprintf(rate, sizeof(rate), "%u", f->video_rate);
        char *args[] = {
            (char *)f->binary, (char *)f->output, f->duration, width, height, rate, nullptr};
        if (posix_spawn(&f->child, f->binary, nullptr, nullptr, args, environ)) {
            f->result = 1;
            pw_main_loop_quit(f->loop);
        }
        return;
    }
    if (f->list_mode) {
        char *args[] = {(char *)f->binary, "devices", nullptr};
        if (posix_spawn(&f->child, f->binary, nullptr, nullptr, args, environ)) {
            f->result = 1;
            pw_main_loop_quit(f->loop);
        }
        return;
    }
    int fd = -1;
    if (!f->portal_mode) {
        fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        struct sockaddr_un address = {.sun_family = AF_UNIX};
        int n = snprintf(address.sun_path, sizeof(address.sun_path), "%s/gpukey-test",
                         getenv("PIPEWIRE_RUNTIME_DIR"));
        if (fd < 0 || n < 0 || (size_t)n >= sizeof(address.sun_path) ||
            connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
            perror("private capture socket");
            f->result = 1;
            pw_main_loop_quit(f->loop);
            if (fd >= 0)
                close(fd);
            return;
        }
    }
    char node[32];
    snprintf(node, sizeof(node), "%u", f->source_node);
    char *args[32] = {(char *)f->binary,
                      "--pipewire-fd",
                      "3",
                      "--node",
                      node,
                      "--allow-shm",
                      "--backend",
                      f->gpu_mode ? "vaapi" : "software",
                      "--duration",
                      f->duration,
                      "--container",
                      f->still || f->aux_mode ? "mkv" : "mp4",
                      "--static-interval",
                      f->still ? "60" : "1",
                      "--output",
                      (char *)f->output,
                      nullptr};
    unsigned arg = 16;
    if (f->replay_mode) {
        args[11] = "mkv";
        args[13] = "1";
        args[14] = "--session";
        args[15] = "fixture";
        memmove(args + 3, args + 1, 15 * sizeof(*args));
        args[1] = "replay";
        args[2] = f->portal_mode ? "30" : "1";
        arg += 2;
    }
    if (f->audio_mode) {
        args[arg++] = "--audio";
        args[arg++] = "gpukey-fixture-mic";
        args[arg++] = f->selection_failure == 3 ? "--audio-node" : "--audio";
        args[arg++] = f->selection_failure == 1   ? "missing-device"
                      : f->selection_failure == 2 ? "gpukey-fixture-mic"
                                                  : "gpukey-fixture-sink";
    }
    if (f->fps_mode) {
        args[arg++] = "--fps";
        args[arg++] = "25";
    }
    char video_rate[32];
    if (f->paced_pts) {
        snprintf(video_rate, sizeof(video_rate), "%u", f->video_rate);
        args[arg++] = "--fps";
        args[arg++] = video_rate;
    }
    if (f->pcm_mode) {
        args[arg++] = "--audio-codec";
        args[arg++] = "pcm";
    }
    if (f->zero_epoch || f->cycle_pts) {
        args[arg++] = "--video-clock";
        args[arg++] = f->zero_epoch ? "source" : "cycle";
    }
    const char *stats = getenv("GPUKEY_FIXTURE_STATS");
    if (stats) {
        args[arg++] = "--stats";
        args[arg++] = (char *)stats;
    }
    if (f->portal_mode) {
        unsigned at = f->replay_mode ? 3 : 1;
        memmove(args + at, args + at + 4, (arg - at - 4) * sizeof(*args));
        arg -= 4; /* Exercise portal authorization and its real event loop. */
        args[arg++] = "--display";
    }
    args[arg] = nullptr;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (fd >= 0)
        posix_spawn_file_actions_adddup2(&actions, fd, 3);
    int ret = posix_spawn(&f->child, f->binary, &actions, nullptr, args, environ);
    posix_spawn_file_actions_destroy(&actions);
    if (fd >= 0)
        close(fd);
    if (ret) {
        fprintf(stderr, "spawn recorder: %s\n", strerror(ret));
        f->result = 1;
        pw_main_loop_quit(f->loop);
    }
}

static void state_changed(void *opaque, enum pw_stream_state old, enum pw_stream_state state,
                          const char *message)
{
    (void)old;
    struct fixture *f = opaque;
    if (state == PW_STREAM_STATE_ERROR) {
        fprintf(stderr, "fixture: %s\n", message);
        f->result = 1;
        pw_main_loop_quit(f->loop);
    }
    if (state == PW_STREAM_STATE_PAUSED && !f->spawned) {
        f->source_node = pw_stream_get_node_id(f->stream);
        maybe_spawn(f);
    }
}

static void video_io(void *opaque, uint32_t id, void *area, uint32_t size)
{
    struct fixture *f = opaque;
    if (id == SPA_IO_Clock)
        f->video_clock = size >= sizeof(struct spa_io_clock) ? area : nullptr;
}

static const struct pw_stream_events stream_events = {
    .version = PW_VERSION_STREAM_EVENTS,
    .process = process,
    .param_changed = param_changed,
    .state_changed = state_changed,
    .io_changed = video_io,
    .add_buffer = add_buffer,
    .remove_buffer = remove_buffer,
};

static void global(void *opaque, uint32_t id, uint32_t permissions, const char *type,
                   uint32_t version, const struct spa_dict *props)
{
    (void)permissions;
    (void)version;
    struct fixture *f = opaque;
    /* Rejection tests exercise discovery, not media links. Creating a link to
     * the deliberately rejected client's disappearing port races its exit.
     * integration.sh also checks the specific selection error, so a timeout
     * cannot masquerade as the expected rejection. */
    if (!props || f->selection_failure)
        return;
    const char *name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    if (!strcmp(type, PW_TYPE_INTERFACE_Node)) {
        const char *media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
        if ((name && !strcmp(name, "gpukey-video")) ||
            (f->external_mode && media_class && !strcmp(media_class, "Stream/Input/Video"))) {
            if (f->input_node && f->input_node != id)
                abort(); /* The private graph has exactly one video consumer. */
            f->input_node = id;
        }
    }
    if (!strcmp(type, PW_TYPE_INTERFACE_Port)) {
        const char *node = spa_dict_lookup(props, PW_KEY_NODE_ID);
        const char *direction = spa_dict_lookup(props, PW_KEY_PORT_DIRECTION);
        if (node && direction) {
            uint32_t owner = (uint32_t)strtoul(node, nullptr, 10);
            if (owner == f->source_node && !strcmp(direction, "out"))
                f->output_port = id;
            if (owner == f->input_node && !strcmp(direction, "in"))
                f->input_port = id;
        }
    }
    for (unsigned i = 0; i < 2 && f->audio_mode; ++i) {
        struct audio_fixture *a = &f->audio[i];
        if (!strcmp(type, PW_TYPE_INTERFACE_Node) && name) {
            char wanted[32];
            snprintf(wanted, sizeof(wanted), "gpukey-audio-%u", i + 1);
            if (!strcmp(name, wanted)) {
                a->input_node = id;
                a->input_proxy =
                    pw_registry_bind(f->registry, id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0);
                if (!a->input_proxy)
                    abort();
                uint8_t storage[256];
                struct spa_pod_builder b = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
                const struct spa_pod *config = spa_pod_builder_add_object(
                    &b, SPA_TYPE_OBJECT_ParamPortConfig, SPA_PARAM_PortConfig,
                    SPA_PARAM_PORT_CONFIG_direction, SPA_POD_Id(SPA_DIRECTION_INPUT),
                    SPA_PARAM_PORT_CONFIG_mode, SPA_POD_Id(SPA_PARAM_PORT_CONFIG_MODE_passthrough));
                if (pw_node_set_param(a->input_proxy, SPA_PARAM_PortConfig, 0, config) < 0)
                    abort();
                /* Registry globals expose a filtered property set; stream
                 * controls such as stream.capture.sink are not present here. */
            }
        }
        if (!strcmp(type, PW_TYPE_INTERFACE_Port)) {
            const char *node = spa_dict_lookup(props, PW_KEY_NODE_ID);
            const char *direction = spa_dict_lookup(props, PW_KEY_PORT_DIRECTION);
            if (node && direction) {
                uint32_t owner = (uint32_t)strtoul(node, nullptr, 10);
                if (owner == a->source_node && !strcmp(direction, "out"))
                    a->output_port = id;
                if (owner == a->input_node && !strcmp(direction, "in"))
                    a->input_port = id;
            }
        }
        if (a->input_port && a->output_port && !a->link) {
            struct pw_properties *props_link = pw_properties_new(nullptr, nullptr);
            pw_properties_setf(props_link, PW_KEY_LINK_OUTPUT_NODE, "%u", a->source_node);
            pw_properties_setf(props_link, PW_KEY_LINK_INPUT_NODE, "%u", a->input_node);
            pw_properties_setf(props_link, PW_KEY_LINK_OUTPUT_PORT, "%u", a->output_port);
            pw_properties_setf(props_link, PW_KEY_LINK_INPUT_PORT, "%u", a->input_port);
            a->link = pw_core_create_object(f->core, "link-factory", PW_TYPE_INTERFACE_Link,
                                            PW_VERSION_LINK, &props_link->dict, 0);
            pw_properties_free(props_link);
        }
    }
    if (f->input_port && f->output_port && !f->link) {
        struct pw_properties *link = pw_properties_new(nullptr, nullptr);
        pw_properties_setf(link, PW_KEY_LINK_OUTPUT_NODE, "%u", f->source_node);
        pw_properties_setf(link, PW_KEY_LINK_INPUT_NODE, "%u", f->input_node);
        pw_properties_setf(link, PW_KEY_LINK_OUTPUT_PORT, "%u", f->output_port);
        pw_properties_setf(link, PW_KEY_LINK_INPUT_PORT, "%u", f->input_port);
        f->link = pw_core_create_object(f->core, "link-factory", PW_TYPE_INTERFACE_Link,
                                        PW_VERSION_LINK, &link->dict, 0);
        pw_properties_free(link);
    }
}

static const struct pw_registry_events registry_events = {
    .version = PW_VERSION_REGISTRY_EVENTS,
    .global = global,
};

static void core_error(void *opaque, uint32_t id, int sequence, int result, const char *message)
{
    (void)id;
    (void)sequence;
    (void)result;
    struct fixture *f = opaque;
    fprintf(stderr, "fixture core: %s\n", message);
    f->result = 1;
    pw_main_loop_quit(f->loop);
}

static const struct pw_core_events core_events = {
    .version = PW_VERSION_CORE_EVENTS,
    .error = core_error,
};

struct picture_check {
    AVCodecContext *decoder;
    AVFrame *frame;
    unsigned count, changes;
    bool white;
};

static struct picture_check picture_check_open(const AVStream *stream)
{
    struct picture_check check = {
        .decoder = avcodec_alloc_context3(avcodec_find_decoder_by_name("libdav1d")),
        .frame = av_frame_alloc()};
    if (!check.decoder || !check.frame ||
        avcodec_parameters_to_context(check.decoder, stream->codecpar) < 0)
        abort();
    check.decoder->thread_count = 1;
    if (avcodec_open2(check.decoder, nullptr, nullptr) < 0)
        abort();
    return check;
}

static void picture_check_packet(struct picture_check *check, const AVPacket *packet)
{
    if (avcodec_send_packet(check->decoder, packet) < 0)
        abort();
    int ret;
    while ((ret = avcodec_receive_frame(check->decoder, check->frame)) >= 0) {
        AVFrame *frame = check->frame;
        if (frame->format != AV_PIX_FMT_YUV420P)
            abort();
        unsigned y = frame->data[0][frame->height / 2 * frame->linesize[0] + frame->width / 2];
        /* The declared range defines the producer's black/white endpoints.
         * Quantization changes alone are not scene motion. */
        if (frame->color_range != AVCOL_RANGE_MPEG && frame->color_range != AVCOL_RANGE_JPEG)
            abort();
        int black = frame->color_range == AVCOL_RANGE_JPEG ? 0 : 16;
        int white_level = frame->color_range == AVCOL_RANGE_JPEG ? 255 : 235;
        if (abs((int)y - black) > 3 && abs((int)y - white_level) > 3)
            abort();
        bool white = y >= 128;
        if (check->count && white != check->white)
            ++check->changes;
        check->white = white;
        ++check->count;
        av_frame_unref(frame);
    }
    if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
        abort();
}

static unsigned picture_check_close(struct picture_check *check, bool moving)
{
    picture_check_packet(check, nullptr);
    if (moving && check->changes < 10) {
        fprintf(stderr, "fixture: only %u visible changes in %u decoded pictures\n", check->changes,
                check->count);
        abort();
    }
    avcodec_free_context(&check->decoder);
    av_frame_free(&check->frame);
    return check->count;
}

int main(int argc, char **argv)
{
    if ((argc != 3 && argc != 4) || !getenv("PIPEWIRE_RUNTIME_DIR"))
        return 2;
    struct fixture f = {.binary = argv[1],
                        .output = argv[2],
                        .result = 1,
                        .start = gk_now_ns(),
                        .video_rate = 50,
                        .buffer_min = 2,
                        .buffer_max = 8};
    bool shm_portal = argc == 4 && (!strcmp(argv[3], "--shm-portal-source") ||
                                    !strcmp(argv[3], "--shm-portal-replay"));
    f.external_mode = argc == 4 && (!strcmp(argv[3], "--gpu-portal-source") ||
                                    !strcmp(argv[3], "--shm-portal-source"));
    bool portal_gpu = f.external_mode || shm_portal ||
                      (argc == 4 && (!strcmp(argv[3], "--gpu-portal-replay") ||
                                     !strcmp(argv[3], "--gpu-portal-replay-static")));
    bool portal_replay = portal_gpu || (argc == 4 && !strcmp(argv[3], "--portal-replay"));
    f.portal_mode =
        portal_replay ||
        (argc == 4 && (!strcmp(argv[3], "--portal") || !strcmp(argv[3], "--portal-static")));
    bool niri = portal_gpu || (argc == 4 && !strcmp(argv[3], "--gpu-replay-niri"));
    f.lost_cycle = argc == 4 && !strcmp(argv[3], "--lost-cycle");
    f.cycle_pts = f.lost_cycle || (argc == 4 && !strcmp(argv[3], "--cycle-pts"));
    f.zero_epoch = argc == 4 && !strcmp(argv[3], "--zero-epoch");
    f.same_sequence = argc == 4 && !strcmp(argv[3], "--same-sequence");
    f.lost_pts = argc == 4 && !strcmp(argv[3], "--lost-pts");
    f.missing_pts = niri || f.cycle_pts || (argc == 4 && !strcmp(argv[3], "--missing-pts"));
    bool portrait = niri || (argc == 4 && !strcmp(argv[3], "--gpu-replay-portrait"));
    f.fixate_mode =
        !shm_portal && (portrait || (argc == 4 && (!strcmp(argv[3], "--gpu-fixate") ||
                                                   !strcmp(argv[3], "--gpu-replay-fixate"))));
    f.reconfigure_mode =
        argc == 4 && (!strcmp(argv[3], "--reconfigure") || !strcmp(argv[3], "--gpu-reconfigure"));
    bool gpu_static_replay = argc == 4 && !strcmp(argv[3], "--gpu-static-replay");
    f.gpu_mode = !shm_portal &&
                 (f.fixate_mode || gpu_static_replay ||
                  (argc == 4 &&
                   (!strcmp(argv[3], "--gpu") || !strcmp(argv[3], "--gpu-aux") ||
                    !strcmp(argv[3], "--gpu-replay") || !strcmp(argv[3], "--gpu-reconfigure"))));
    f.replay_mode =
        portrait || portal_replay || gpu_static_replay ||
        (argc == 4 && (!strcmp(argv[3], "--replay") || !strcmp(argv[3], "--gpu-replay") ||
                       !strcmp(argv[3], "--static-replay") || !strcmp(argv[3], "--replay-revoke") ||
                       !strcmp(argv[3], "--gpu-replay-fixate")));
    if (f.replay_mode &&
        snprintf(f.second, sizeof(f.second), "%s.second.mkv", f.output) >= (int)sizeof(f.second))
        return 2;
    f.revoke = argc == 4 && (!strcmp(argv[3], "--revoke") || !strcmp(argv[3], "--replay-revoke"));
    f.still = gpu_static_replay ||
              (argc == 4 && (!strcmp(argv[3], "--static") || !strcmp(argv[3], "--static-replay") ||
                             !strcmp(argv[3], "--portal-static") ||
                             !strcmp(argv[3], "--gpu-portal-replay-static")));
    f.audio_revoke = argc == 4 && !strcmp(argv[3], "--audio-revoke");
    f.audio_mode = f.audio_revoke || (argc == 4 && !strcmp(argv[3], "--audio"));
    f.list_mode = argc == 4 && !strcmp(argv[3], "--devices");
    f.pcm_mode = argc == 4 && !strcmp(argv[3], "--aux-pcm");
    f.aux_mode =
        f.pcm_mode || (argc == 4 && (!strcmp(argv[3], "--aux") || !strcmp(argv[3], "--gpu-aux")));
    f.aux_mode |=
        f.replay_mode && f.gpu_mode; /* Combined native capture + two 16-channel replay tracks. */
    f.selection_failure = argc != 4                               ? 0
                          : !strcmp(argv[3], "--audio-missing")   ? 1
                          : !strcmp(argv[3], "--audio-duplicate") ? 2
                          : !strcmp(argv[3], "--audio-kind")      ? 3
                                                                  : 0;
    f.audio_mode |= f.list_mode || f.selection_failure || f.aux_mode || f.replay_mode;
    if (portal_gpu)
        f.audio_mode = f.aux_mode = false; /* Match the user's video-only command. */
    if (f.external_mode)
        f.replay_mode = false;
    f.duration_boundary = argc == 4 && !strcmp(argv[3], "--duration-boundary");
    f.audio_mode |= f.duration_boundary;
    f.fps_mode = argc == 4 && !strcmp(argv[3], "--fps");
    f.width = portrait ? 2560 : f.gpu_mode ? 320 : 64;
    f.height = portrait ? 2880 : f.gpu_mode ? 240 : 48;
    const char *size = getenv("GPUKEY_FIXTURE_SIZE");
    const char *rate = getenv("GPUKEY_FIXTURE_RATE");
    const char *buffer_min = getenv("GPUKEY_FIXTURE_BUFFER_MIN");
    const char *buffer_max = getenv("GPUKEY_FIXTURE_BUFFER_MAX");
    const char *pattern = getenv("GPUKEY_FIXTURE_PATTERN");
    const char *pause = getenv("GPUKEY_FIXTURE_PAUSE");
    if (pause) {
        if (!portal_gpu)
            return 2;
        char *end;
        errno = 0;
        unsigned long begin = strtoul(pause, &end, 10);
        if (errno || end == pause || *end != ':')
            return 2;
        const char *tail = end + 1;
        unsigned long until = strtoul(tail, &end, 10);
        if (errno || end == tail || *end || begin >= until || until > INT64_MAX / GK_NS)
            return 2;
        f.pause_begin = (int64_t)begin * GK_NS;
        f.pause_end = (int64_t)until * GK_NS;
    }
    if (size || rate || buffer_min || buffer_max || pattern) {
        /* Geometry is independent of the audio/cadence oracles. Rate, pool and
         * scene sweeps remain confined to the video-only benchmark. */
        if (!portal_gpu && (rate || buffer_min || buffer_max || pattern))
            return 2;
        if (size) {
            char *end;
            errno = 0;
            unsigned long width = strtoul(size, &end, 10);
            if (errno || *end != 'x' || width < 2 || width > 8192 || (width & 1))
                return 2;
            unsigned long height = strtoul(end + 1, &end, 10);
            if (errno || *end || height < 2 || height > 8192 || (height & 1))
                return 2;
            f.width = (int)width;
            f.height = (int)height;
        }
        if (rate) {
            char *end;
            errno = 0;
            unsigned long n = strtoul(rate, &end, 10);
            if (errno || *end || n < 10 || n > 1000)
                return 2;
            f.video_rate = (unsigned)n;
        }
        if (buffer_min) {
            char *end;
            unsigned long n = strtoul(buffer_min, &end, 10);
            if (*end || n < 2 || n > 8)
                return 2;
            f.buffer_min = (unsigned)n;
        }
        if (buffer_max) {
            char *end;
            errno = 0;
            unsigned long n = strtoul(buffer_max, &end, 10);
            if (errno || *end || n < f.buffer_min || n > 8)
                return 2;
            f.buffer_max = (unsigned)n;
        }
        if (pattern) {
            if (strcmp(pattern, "scene"))
                return 2;
            f.scene = true;
        }
        /* Use deterministic, populated timestamps for cadence sweeps. The
         * unchanged default mode continues to test Niri's missing PTS. */
        if (portal_gpu) {
            f.missing_pts = false;
            f.paced_pts = true;
        }
    }
    f.duration_seconds = f.replay_mode ? 4 : 1;
    const char *seconds = getenv("GPUKEY_FIXTURE_SECONDS");
    if (seconds) {
        /* Duration sweeps use video-only portal replay; other fixtures have
         * fixed-duration audio/sample-count oracles. */
        if (!f.portal_mode || (!f.replay_mode && !f.external_mode) || f.audio_mode)
            return 2;
        char *end;
        errno = 0;
        unsigned long n = strtoul(seconds, &end, 10);
        if (errno || *end || n < (f.replay_mode ? 4U : 1U) || n > UINT_MAX)
            return 2;
        f.duration_seconds = (unsigned)n;
    }
    snprintf(f.duration, sizeof(f.duration), "%u", f.duration_seconds);
    const char *late = getenv("GPUKEY_FIXTURE_LATE_SAVES");
    if (late) {
        /* This oracle requires both triggers beyond the 30 s replay window
         * and its preceding keyframe. It is a test scale, not a recorder limit. */
        if (strcmp(late, "1") || !portal_gpu || f.duration_seconds < 35)
            return 2;
        f.late_saves = true;
    }
    const char *early = getenv("GPUKEY_FIXTURE_EARLY_SAVES");
    if (early) {
        if (strcmp(early, "1") || !portal_gpu || !f.still || f.late_saves)
            return 2;
        f.early_saves = true;
    }
    if (f.portal_mode) {
        int n = snprintf(f.remote_path, sizeof(f.remote_path), "%s/gpukey-test",
                         getenv("PIPEWIRE_RUNTIME_DIR"));
        if (n < 0 || (size_t)n >= sizeof(f.remote_path))
            return 2;
        gk_portal_mock_start(&f.portal, f.remote_path);
    }
    pw_init(nullptr, nullptr);
    if (f.gpu_mode) {
        if (gk_gpu_open(&f.gpu, GK_VAAPI, nullptr) < 0)
            return 1;
        if (f.fixate_mode) {
            if (!f.gpu.modifier_count)
                return 1;
            f.modifier = f.gpu.modifiers[0];
            fprintf(stderr, "fixture: two-stage fixation to modifier 0x%016llx\n",
                    (unsigned long long)f.modifier);
        }
        VAImageFormat vf = {.fourcc = VA_FOURCC_BGRA,
                            .byte_order = VA_LSB_FIRST,
                            .bits_per_pixel = 32,
                            .depth = 32,
                            .red_mask = 0xff0000,
                            .green_mask = 0xff00,
                            .blue_mask = 0xff,
                            .alpha_mask = 0xff000000};
        if (vaCreateImage(f.gpu.display, &vf, f.width, f.height, &f.upload) != VA_STATUS_SUCCESS)
            return 1;
    }
    f.loop = pw_main_loop_new(nullptr);
    f.context = pw_context_new(pw_main_loop_get_loop(f.loop), nullptr, 0);
    f.core = pw_context_connect(f.context, nullptr, 0);
    if (!f.core)
        return 1;
    pw_core_add_listener(f.core, &f.core_hook, &core_events, &f);
    f.registry = pw_core_get_registry(f.core, PW_VERSION_REGISTRY, 0);
    pw_registry_add_listener(f.registry, &f.registry_hook, &registry_events, &f);
    f.stream = pw_stream_new(f.core, "gpuKey isolated test image",
                             pw_properties_new(PW_KEY_MEDIA_CLASS, "Video/Source", PW_KEY_NODE_NAME,
                                               "gpukey-fixture", nullptr));
    pw_stream_add_listener(f.stream, &f.stream_hook, &stream_events, &f);
    uint8_t bytes[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(bytes, sizeof(bytes));
    /* Still emit at 50 Hz in the 25 Hz test: admission must enforce the cap. */
    const struct spa_pod *param = video_format(&f, &b, !f.fixate_mode);
    int ret = pw_stream_connect(f.stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                                PW_STREAM_FLAG_DRIVER | PW_STREAM_FLAG_MAP_BUFFERS |
                                    (f.gpu_mode ? PW_STREAM_FLAG_ALLOC_BUFFERS : 0),
                                &param, 1);
    if (ret < 0)
        return 1;
    for (unsigned i = 0; i < 2 && f.audio_mode; ++i) {
        struct audio_fixture *a = &f.audio[i];
        a->fixture = &f;
        a->rate = i ? 48000 : 44100;
        a->channels = f.aux_mode ? 16 : i ? 6 : 1;
        a->stream = pw_stream_new(
            f.core, "gpuKey isolated audio",
            pw_properties_new(
                PW_KEY_MEDIA_CLASS, i ? "Audio/Sink" : "Audio/Source", PW_KEY_NODE_NAME,
                i ? "gpukey-fixture-sink" : "gpukey-fixture-mic", PW_KEY_NODE_DESCRIPTION,
                i ? "Synthetic surround monitor" : "Synthetic mono mic", "adapter.auto-port-config",
                "{ mode = passthrough position = preserve }", nullptr));
        if (!a->stream)
            return 1;
        pw_stream_add_listener(a->stream, &a->hook, &audio_events, a);
        struct spa_audio_info_raw af = {.format = SPA_AUDIO_FORMAT_F32,
                                        .rate = a->rate,
                                        .channels = a->channels,
                                        .position = {SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR,
                                                     SPA_AUDIO_CHANNEL_FC, SPA_AUDIO_CHANNEL_LFE,
                                                     SPA_AUDIO_CHANNEL_RL, SPA_AUDIO_CHANNEL_RR}};
        if (f.aux_mode)
            for (unsigned ch = 0; ch < a->channels; ++ch)
                af.position[ch] = SPA_AUDIO_CHANNEL_AUX0 + ch;
        else if (!i)
            af.position[0] = SPA_AUDIO_CHANNEL_MONO;
        spa_pod_builder_init(&b, bytes, sizeof(bytes));
        param = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &af);
        if (pw_stream_connect(a->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                              PW_STREAM_FLAG_DRIVER | PW_STREAM_FLAG_MAP_BUFFERS, &param, 1) < 0)
            return 1;
    }
    f.timer = pw_loop_add_timer(pw_main_loop_get_loop(f.loop), tick, &f);
    struct timespec first = {.tv_nsec = 1}, interval = {.tv_nsec = GK_NS / f.video_rate};
    pw_loop_update_timer(pw_main_loop_get_loop(f.loop), f.timer, &first, &interval, false);
    pw_main_loop_run(f.loop);
    if (f.child > 0) {
        kill(f.child, SIGTERM);
        waitpid(f.child, nullptr, 0);
    }
    for (unsigned i = 0; i < f.save_count; ++i)
        if (f.saves[i] > 0) {
            int status;
            if (waitpid(f.saves[i], &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status))
                f.save_failed = true;
        }
    for (unsigned i = 0; i < 2 && f.audio_mode; ++i) {
        if (f.audio[i].link)
            pw_proxy_destroy(f.audio[i].link);
        if (f.audio[i].input_proxy)
            pw_proxy_destroy((struct pw_proxy *)f.audio[i].input_proxy);
        pw_stream_destroy(f.audio[i].stream);
    }
    if (f.link)
        pw_proxy_destroy(f.link);
    pw_stream_destroy(f.stream);
    pw_proxy_destroy((struct pw_proxy *)f.registry);
    pw_core_disconnect(f.core);
    pw_context_destroy(f.context);
    pw_main_loop_destroy(f.loop);
    if (f.gpu_mode) {
        vaDestroyImage(f.gpu.display, f.upload.image_id);
        gk_gpu_close(&f.gpu);
    }
    if (f.portal_mode) {
        if (f.external_mode ? !(atomic_load(&f.portal.selected_types) & 1U)
                            : (atomic_load(&f.portal.selected_types) != 1 ||
                               atomic_load(&f.portal.closes) != 1))
            f.result = 1;
        gk_portal_mock_stop(&f.portal);
    }
    if (f.allocated_buffers || f.allocated_bytes)
        abort();
    const char *report_path = getenv("GPUKEY_FIXTURE_REPORT");
    if (report_path) {
        FILE *report = fopen(report_path, "wx");
        if (!report)
            abort();
        if (fprintf(report,
                    "{\"produced\": %llu, \"empty_processes\": %llu, "
                    "\"coalesced_ticks\": %llu, \"peak_source_buffers\": %u, "
                    "\"peak_source_bytes\": %llu, \"width\": %d, \"height\": %d, "
                    "\"pattern_width\": %u, \"pause_begin_ns\": %lld, \"pause_end_ns\": %lld, "
                    "\"rate\": %u, \"buffer_min\": %u, \"buffer_max\": %u, "
                    "\"first_video_ns\": %lld, \"last_video_ns\": %lld}\n",
                    (unsigned long long)f.sequence, (unsigned long long)f.empty_processes,
                    (unsigned long long)f.coalesced_ticks, f.peak_buffers,
                    (unsigned long long)f.peak_bytes, f.width, f.height,
                    f.gpu_mode ? f.upload.pitches[0] / 4 : (unsigned)f.width,
                    (long long)f.pause_begin, (long long)f.pause_end, f.video_rate, f.buffer_min,
                    f.buffer_max, (long long)f.video_origin, (long long)f.last_produced) < 0 ||
            fclose(report))
            abort();
    }
    if (f.fixate_mode && (!f.saw_proposal || !f.fixated))
        return 1;
    if (f.external_mode)
        return f.result || !f.sequence;
    if (f.reconfigure_mode && !f.reconfigured)
        return 1;
    if (f.list_mode || f.selection_failure)
        return f.result || access(f.output, F_OK) == 0;
    if (f.replay_mode) {
        unsigned wanted = f.revoke ? 1 : 2;
        if (f.result || f.save_failed || f.save_count != wanted)
            return 1;
        for (unsigned i = 0; i < wanted; ++i) {
            AVFormatContext *input = nullptr;
            if (avformat_open_input(&input, i ? f.second : f.output, nullptr, nullptr) < 0 ||
                avformat_find_stream_info(input, nullptr) < 0 ||
                input->nb_streams != (f.audio_mode ? 3U : 1U) ||
                input->streams[0]->codecpar->width != f.width ||
                (f.audio_mode &&
                 (input->streams[1]->codecpar->ch_layout.nb_channels != (int)f.audio[0].channels ||
                  input->streams[2]->codecpar->ch_layout.nb_channels != (int)f.audio[1].channels)))
                return 1;
            const AVDictionaryEntry *start =
                av_dict_get(input->metadata, "GPUKEY_REPLAY_START_NS", nullptr, 0);
            const AVDictionaryEntry *end =
                av_dict_get(input->metadata, "GPUKEY_REPLAY_END_NS", nullptr, 0);
            if (!start || !end || (f.late_saves && strtoll(start->value, nullptr, 10) <= 0) ||
                (i && !f.portal_mode && strtoll(start->value, nullptr, 10) < 1900000000) ||
                strtoll(end->value, nullptr, 10) - strtoll(start->value, nullptr, 10) <
                    (f.early_saves ? 50000000 : GK_NS) ||
                (f.early_saves && (strtoll(start->value, nullptr, 10) != 0 ||
                                   strtoll(end->value, nullptr, 10) >= 750000000)))
                return 1;
            AVPacket *packet = av_packet_alloc();
            struct picture_check check = picture_check_open(input->streams[0]);
            unsigned pictures = 0;
            while (av_read_frame(input, packet) >= 0) {
                if (!packet->stream_index) {
                    if (!pictures && (!(packet->flags & AV_PKT_FLAG_KEY) || packet->pts != 0))
                        return 1;
                    ++pictures;
                    picture_check_packet(&check, packet);
                }
                av_packet_unref(packet);
            }
            av_packet_free(&packet);
            if (picture_check_close(&check, !f.still) != pictures)
                return 1;
            avformat_close_input(&input);
            if (pictures < (f.still ? 1U : 30U))
                return 1;
        }
        printf("private PipeWire replay: %u independent clips, %u + %u audio channels pass\n",
               wanted, f.audio[0].channels, f.audio[1].channels);
        return 0;
    }
    if (!f.result) {
        AVFormatContext *input = nullptr;
        if (avformat_open_input(&input, f.output, nullptr, nullptr) < 0 ||
            avformat_find_stream_info(input, nullptr) < 0)
            return 1;
        if (input->nb_streams != (f.audio_mode ? 3U : 1U) ||
            input->streams[0]->codecpar->width != f.width ||
            input->streams[0]->codecpar->height != f.height)
            f.result = 1;
        if (f.audio_mode &&
            (input->nb_streams != 3 ||
             input->streams[1]->codecpar->ch_layout.nb_channels != (int)f.audio[0].channels ||
             input->streams[2]->codecpar->ch_layout.nb_channels != (int)f.audio[1].channels))
            f.result = 1;
        AVPacket *packet = av_packet_alloc();
        AVCodecContext *audio_decoder[2] = {};
        unsigned audio_samples[2] = {}, observed[2] = {};
        double sums[2][16] = {}, quadrature[2][16] = {};
        float peak[2] = {};
        unsigned peak_position[2] = {};
        AVFrame *audio_frame = av_frame_alloc();
        for (unsigned i = 0; i < 2 && f.audio_mode; ++i) {
            AVCodecParameters *parameters = input->streams[i + 1]->codecpar;
            audio_decoder[i] = avcodec_alloc_context3(avcodec_find_decoder(parameters->codec_id));
            if (!audio_decoder[i] ||
                avcodec_parameters_to_context(audio_decoder[i], parameters) < 0)
                abort();
            audio_decoder[i]->pkt_timebase = input->streams[i + 1]->time_base;
            if (avcodec_open2(audio_decoder[i], nullptr, nullptr) < 0)
                abort();
        }
        unsigned count = 0;
        struct picture_check check = picture_check_open(input->streams[0]);
        while (av_read_frame(input, packet) >= 0) {
            if (packet->stream_index == 0) {
                ++count;
                picture_check_packet(&check, packet);
            } else if (f.audio_mode && packet->stream_index <= 2) {
                unsigned i = (unsigned)packet->stream_index - 1;
                if (avcodec_send_packet(audio_decoder[i], packet) < 0)
                    abort();
                while (avcodec_receive_frame(audio_decoder[i], audio_frame) >= 0) {
                    if (audio_frame->format != AV_SAMPLE_FMT_FLT &&
                        audio_frame->format != AV_SAMPLE_FMT_FLTP)
                        abort();
                    unsigned channels = (unsigned)audio_frame->ch_layout.nb_channels;
                    if (channels != f.audio[i].channels || channels > 16)
                        abort();
                    for (unsigned n = 0; n < (unsigned)audio_frame->nb_samples; ++n) {
                        unsigned position = audio_samples[i]++;
                        float event = audio_frame->format == AV_SAMPLE_FMT_FLT
                                          ? ((float *)audio_frame->data[0])[n * channels]
                                          : ((float *)audio_frame->extended_data[0])[n];
                        if (fabsf(event) > peak[i]) {
                            peak[i] = fabsf(event);
                            peak_position[i] = position;
                        }
                        if (position >= 4800 && position < 12000) {
                            ++observed[i];
                            for (unsigned ch = 0; ch < channels; ++ch) {
                                double sample =
                                    audio_frame->format == AV_SAMPLE_FMT_FLT
                                        ? (double)((float *)audio_frame->data[0])[n * channels + ch]
                                        : (double)((float *)audio_frame->extended_data[ch])[n];
                                if (f.aux_mode) {
                                    double phase =
                                        6.2831853071795864769 * (300 + ch * 29) * position / 48000;
                                    sums[i][ch] += sample * sin(phase);
                                    quadrature[i][ch] += sample * cos(phase);
                                } else
                                    sums[i][ch] += sample;
                            }
                        }
                    }
                    av_frame_unref(audio_frame);
                }
            }
            av_packet_unref(packet);
        }
        for (unsigned i = 0; i < 2 && f.audio_mode; ++i) {
            if (observed[i] != 7200 || (!f.audio_revoke && audio_samples[i] != 48000)) {
                fprintf(stderr, "audio %u: observed %u tone samples, decoded %u (expected 48000)\n",
                        i + 1, observed[i], audio_samples[i]);
                f.result = 1;
            }
            int64_t expected_peak = 9600 + (f.audio[i].origin - f.video_origin) * 48000 / GK_NS;
            if (peak[i] < 0.3f || llabs((int64_t)peak_position[i] - expected_peak) > 96) {
                fprintf(stderr,
                        "audio %u event at sample %u; source-clock expectation %" PRId64 "\n",
                        i + 1, peak_position[i], expected_peak);
                f.result = 1;
            }
            for (unsigned ch = 0; ch < f.audio[i].channels; ++ch) {
                double value = f.aux_mode ? 2 * hypot(sums[i][ch], quadrature[i][ch]) : sums[i][ch];
                double expected = f.aux_mode ? 0.15 : (double)(ch + 1) * 0.03;
                if (!observed[i] ||
                    fabs(value / observed[i] - expected) > (f.aux_mode ? 0.04 : 0.002))
                    f.result = 1;
            }
            avcodec_free_context(&audio_decoder[i]);
        }
        av_frame_free(&audio_frame);
        if (picture_check_close(&check, !f.still && !f.fps_mode) != count)
            f.result = 1;
        if ((f.still && count != 1) || (!f.still && count < 10))
            f.result = 1;
        if (f.fps_mode && (count < 23 || count > 26))
            f.result = 1;
        av_packet_free(&packet);
        avformat_close_input(&input);
        printf("private PipeWire integration: %u video packets, %s\n", count,
               f.result ? "FAILED" : "passed");
    }
    return f.result;
}

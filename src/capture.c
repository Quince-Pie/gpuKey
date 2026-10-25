#include "capture.h"
#include "core.h"
#include "devices.h"
#include "vk_rgb.h"

#include <drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw-json.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>
#include <stdbit.h>
#include <stdckdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <tracy/TracyC.h>
#include <unistd.h>

/* SPA's public pod-building macros intentionally use GNU statement expressions. */
#pragma GCC diagnostic ignored "-Wpedantic"

enum { AUDIO_SLOTS = 256 };
static_assert(GK_AUDIO_CHANNELS == SPA_AUDIO_MAX_CHANNELS);
struct record {
    struct pw_buffer *wrapper; /* Only touched with the PipeWire loop lock. */
    struct gk_capture_frame frame;
    uint8_t *pixels;
    uint32_t generation;
    bool occupied, held;
};
struct audio_record {
    int64_t pts;
    unsigned frames;
};
struct target_watch {
    struct gk_capture *capture;
    struct pw_registry *registry;
    struct spa_hook listener;
    uint32_t node;
    uint64_t serial;
    const char *target;
    bool seen;
};
struct audio_input {
    struct gk_capture *capture;
    struct pw_stream *stream;
    struct pw_node *node_proxy;
    struct spa_hook listener, node_listener;
    struct gk_ring ready, free;
    struct audio_record records[AUDIO_SLOTS];
    float *samples;
    struct gk_audio_format format;
    struct spa_audio_info_raw requested;
    char *title;
    char serial[32];
    uint32_t node;
    bool selected, monitor, seen, header_clock;
    _Atomic bool formatted;
    alignas(64) uint64_t blocks, dropped, silenced;
};
struct gk_capture {
    struct pw_thread_loop *loop;
    struct pw_context *context;
    struct pw_core *core, *audio_core;
    struct pw_stream *video;
    struct audio_input *audio;
    struct spa_hook video_listener, core_listener, audio_core_listener, audio_registry_listener;
    struct target_watch video_watch;
    struct pw_registry *audio_registry;
    int audio_sync;
    unsigned audio_cursor, source_buffers;
    struct gk_capture_options options;
    struct gk_video_format format;
    struct record *records;
    struct gk_ring video_ready;
    int wake;
    bool started, have_timestamp;
    uint64_t last_sequence;
    int64_t last_timestamp;
    int64_t rate_origin;
    uint64_t rate_slot;
    bool rate_started;
    size_t frame_bytes;
    /* Shared control is read by every audio RT producer. Keep video writes
     * and owner counters off its cache line. */
    alignas(64) _Atomic bool stopping;
    _Atomic bool formatted;
    _Atomic int error;
    struct gk_capture_stats stats;
};

static void error(struct gk_capture *c, int code, const char *message)
{
    int expected = 0;
    if (atomic_compare_exchange_strong(&c->error, &expected, code))
        fprintf(stderr, "capture: %s (%s)\n", message, strerror(-code));
    int ignored = gk_notify(c->wake);
    (void)ignored;
}

static void wake(struct gk_capture *c)
{
    int ret = gk_notify(c->wake);
    if (ret < 0)
        error(c, ret, "notify owner");
}

static void audio_error(struct gk_capture *c, int code)
{
    int expected = 0;
    (void)atomic_compare_exchange_strong(&c->error, &expected, code);
    int ignored = gk_notify(c->wake);
    (void)ignored;
}

static void target_global(void *opaque, uint32_t id, uint32_t permissions, const char *type,
                          uint32_t version, const struct spa_dict *props)
{
    (void)permissions;
    (void)version;
    struct target_watch *watch = opaque;
    if (strcmp(type, PW_TYPE_INTERFACE_Node) || !props)
        return;
    const char *serial = spa_dict_lookup(props, PW_KEY_OBJECT_SERIAL);
    const char *name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    bool matches;
    if (watch->target)
        matches =
            (serial && !strcmp(serial, watch->target)) || (name && !strcmp(name, watch->target));
    else if (watch->serial)
        matches = serial && strtoull(serial, nullptr, 10) == watch->serial;
    else
        matches = id == watch->node;
    if (matches) {
        if (watch->seen && watch->node != id)
            error(watch->capture, -EEXIST, "selected source identity is ambiguous");
        watch->node = id;
        watch->seen = true;
    }
}

static void target_removed(void *opaque, uint32_t id)
{
    struct target_watch *watch = opaque;
    if (watch->seen && watch->node == id && !watch->capture->stopping)
        error(watch->capture, -EPIPE, "selected source was removed");
}

static const struct pw_registry_events target_events = {
    .version = PW_VERSION_REGISTRY_EVENTS,
    .global = target_global,
    .global_remove = target_removed,
};

static void clear_record(struct record *r)
{
    for (unsigned i = 0; i < r->frame.image.plane_count; ++i)
        if (r->frame.image.planes[i].fd >= 0)
            close(r->frame.image.planes[i].fd);
    free(r->pixels);
    uint32_t generation = r->generation;
    *r = (struct record){.generation = generation};
}

static void add_buffer(void *opaque, struct pw_buffer *buffer)
{
    struct gk_capture *c = opaque;
    if (c->stopping || atomic_load(&c->error))
        return;
    struct record *r = nullptr;
    unsigned index;
    for (index = 0; index < c->options.buffers; ++index)
        if (!c->records[index].occupied) {
            r = &c->records[index];
            break;
        }
    if (!r || r->generation == UINT32_MAX) {
        error(c, -ENOBUFS, "negotiated allocation/generation budget exhausted");
        return;
    }
    r->occupied = true;
    r->wrapper = buffer;
    if (++c->source_buffers > c->stats.peak_source_buffers)
        c->stats.peak_source_buffers = c->source_buffers;
    ++r->generation;
    r->frame.token = ((uint64_t)r->generation << 32) | index;
    buffer->user_data = r;
    struct spa_buffer *b = buffer->buffer;
    if (c->format.dmabuf) {
        for (unsigned i = 0; i < b->n_datas; ++i) {
            struct spa_data *data = &b->datas[i];
            if (data->type != SPA_DATA_DmaBuf || data->fd < 0 || data->fd > INT32_MAX ||
                r->frame.image.plane_count == 4) {
                error(c, -ENOTSUP, "unexpected DMA-BUF plane/synchronization layout");
                return;
            }
            int fd = fcntl((int)data->fd, F_DUPFD_CLOEXEC, 0);
            if (fd < 0) {
                error(c, -errno, "duplicate DMA-BUF");
                return;
            }
            /* DMA-BUF maxsize/chunk.size may be zero. Query the allocation. */
            off_t bytes = lseek(fd, 0, SEEK_END);
            if (bytes < 0 || (uint64_t)bytes > UINT32_MAX) {
                close(fd);
                error(c, -EOVERFLOW,
                      "DMA-BUF allocation size unavailable or exceeds VA descriptor");
                return;
            }
            struct gk_plane *plane = &r->frame.image.planes[r->frame.image.plane_count++];
            *plane = (struct gk_plane){.fd = fd, .size = (uint32_t)bytes};
        }
    } else {
        r->pixels = malloc(c->frame_bytes);
        if (!r->pixels) {
            error(c, -ENOMEM, "shared-memory copy pool");
            return;
        }
        r->frame.image.data = r->pixels;
        r->frame.image.size = c->frame_bytes;
        r->frame.image.stride = c->format.width * 4;
    }
}

static void remove_buffer(void *opaque, struct pw_buffer *buffer)
{
    struct gk_capture *c = opaque;
    struct record *r = buffer->user_data;
    buffer->user_data = nullptr;
    if (!r)
        return;
    --c->source_buffers;
    r->wrapper = nullptr;
    /* Imports live on the media owner. No GPU wait in this callback. */
    if (!r->held)
        clear_record(r);
}

static void core_error(void *opaque, uint32_t id, int seq, int result, const char *message)
{
    (void)seq;
    struct gk_capture *c = opaque;
    if (!c->stopping && (id == PW_ID_CORE || result == -EPIPE))
        error(c, result < 0 ? result : -EIO, message);
}

static const struct pw_core_events core_events = {
    .version = PW_VERSION_CORE_EVENTS,
    .error = core_error,
};

static void state_changed(void *opaque, enum pw_stream_state old, enum pw_stream_state state,
                          const char *message)
{
    struct gk_capture *c = opaque;
    if (!c->stopping && (state == PW_STREAM_STATE_ERROR || (state == PW_STREAM_STATE_UNCONNECTED &&
                                                            old != PW_STREAM_STATE_UNCONNECTED)))
        error(c, -EPIPE, message ? message : "source disconnected");
}

static void video_param(void *opaque, uint32_t id, const struct spa_pod *param)
{
    struct gk_capture *c = opaque;
    if (id != SPA_PARAM_Format || !param || c->stopping)
        return;
    /* A missing optional rate means VFR; an explicitly invalid denominator
     * still overwrites this default and fails validation below. */
    struct spa_video_info_raw raw = {.framerate = {.num = 0, .denom = 1}};
    if (spa_format_video_raw_parse(param, &raw) < 0 || !raw.size.width || !raw.size.height ||
        raw.size.width > INT32_MAX / 4 || raw.size.height > INT32_MAX || !raw.framerate.denom) {
        error(c, -EINVAL, "invalid negotiated video format/rate");
        return;
    }
    /* The producer must allocate from the offered modifier intersection before
     * publishing a fixed Format. The optional Long parser does not extract a
     * scalar from an unfixed choice: raw.modifier is not an allocation layout
     * yet. Do not publish a media plan or request buffers for this proposal. */
    if (raw.flags & SPA_VIDEO_FLAG_MODIFIER_FIXATION_REQUIRED)
        return;
    struct gk_video_format f = {
        .width = (int)raw.size.width,
        .height = (int)raw.size.height,
        .transfer = raw.transfer_function == SPA_VIDEO_TRANSFER_BT709 ? AVCOL_TRC_BT709
                                                                      : AVCOL_TRC_IEC61966_2_1,
        .dmabuf = (raw.flags & SPA_VIDEO_FLAG_MODIFIER) != 0,
        .modifier = raw.modifier,
    };
    switch (raw.format) {
    case SPA_VIDEO_FORMAT_BGRx:
        f.pixel_format = AV_PIX_FMT_BGR0;
        f.drm_format = DRM_FORMAT_XRGB8888;
        break;
    case SPA_VIDEO_FORMAT_BGRA:
        f.pixel_format = AV_PIX_FMT_BGRA;
        f.drm_format = DRM_FORMAT_ARGB8888;
        break;
    case SPA_VIDEO_FORMAT_RGBx:
        f.pixel_format = AV_PIX_FMT_RGB0;
        f.drm_format = DRM_FORMAT_XBGR8888;
        break;
    case SPA_VIDEO_FORMAT_RGBA:
        f.pixel_format = AV_PIX_FMT_RGBA;
        f.drm_format = DRM_FORMAT_ABGR8888;
        break;
    default:
        error(c, -ENOTSUP, "unsupported negotiated pixel format");
        return;
    }
    if ((raw.transfer_function != SPA_VIDEO_TRANSFER_UNKNOWN &&
         raw.transfer_function != SPA_VIDEO_TRANSFER_SRGB &&
         raw.transfer_function != SPA_VIDEO_TRANSFER_BT709) ||
        (raw.color_primaries != SPA_VIDEO_COLOR_PRIMARIES_UNKNOWN &&
         raw.color_primaries != SPA_VIDEO_COLOR_PRIMARIES_BT709) ||
        (raw.color_range != SPA_VIDEO_COLOR_RANGE_UNKNOWN &&
         raw.color_range != SPA_VIDEO_COLOR_RANGE_0_255) ||
        (raw.color_matrix != SPA_VIDEO_COLOR_MATRIX_UNKNOWN &&
         raw.color_matrix != SPA_VIDEO_COLOR_MATRIX_RGB)) {
        error(c, -ENOTSUP, "source color is outside the SDR RGB plan; refusing relabeling");
        return;
    }
    if (atomic_load(&c->formatted)) {
        if (f.width != c->format.width || f.height != c->format.height ||
            f.pixel_format != c->format.pixel_format || f.modifier != c->format.modifier ||
            f.transfer != c->format.transfer || f.dmabuf != c->format.dmabuf)
            error(c, -ESTALE, "source configuration changed; finalize this segment and restart");
        return;
    }
    if (!f.dmabuf && !c->options.allow_shm) {
        error(c, -ENOTSUP, "DMA-BUF not negotiated; --allow-shm selects explicit upload fallback");
        return;
    }
    size_t bytes, total;
    if (ckd_mul(&bytes, (size_t)f.width * 4, (size_t)f.height) ||
        ckd_mul(&total, bytes, c->options.buffers) ||
        (!f.dmabuf && total > c->options.cpu_budget)) {
        error(c, -ENOMEM, "capture copy pool exceeds configured CPU memory budget");
        return;
    }
    c->format = f;
    c->frame_bytes = bytes;
    if (raw.transfer_function == SPA_VIDEO_TRANSFER_UNKNOWN)
        fprintf(stderr, "capture: untagged SDR RGB uses the declared sRGB default\n");
    uint8_t storage[2048];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
    constexpr unsigned max_preferences = 8;
    const struct spa_pod *params[max_preferences + 4];
    unsigned preferred =
        c->options.preferred_buffers ? c->options.preferred_buffers : c->options.buffers;
    /* A producer's range default can survive filtering. Rank exact counts
     * first, including non-powers of two; our token ring capacity is independent.
     * Bound only the preference list, never the accepted range. This covers
     * the whole default 2..8 range and preserves larger explicit capacities. */
    unsigned count = c->options.buffers - preferred + 1;
    if (count > max_preferences)
        count = max_preferences;
    for (unsigned i = 0; i < count; ++i)
        params[i] = spa_pod_builder_add_object(
            &b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers, SPA_PARAM_BUFFERS_buffers,
            SPA_POD_Int((int)(preferred + i)), SPA_PARAM_BUFFERS_dataType,
            SPA_POD_CHOICE_FLAGS_Int(f.dmabuf ? 1 << SPA_DATA_DmaBuf
                                              : (1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr)));
    params[count++] = spa_pod_builder_add_object(
        &b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers, SPA_PARAM_BUFFERS_buffers,
        SPA_POD_CHOICE_RANGE_Int((int)preferred, 2, (int)c->options.buffers),
        SPA_PARAM_BUFFERS_dataType,
        SPA_POD_CHOICE_FLAGS_Int(f.dmabuf ? 1 << SPA_DATA_DmaBuf
                                          : (1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr)));
    params[count++] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
                                                 SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
                                                 SPA_PARAM_META_size,
                                                 SPA_POD_Int(sizeof(struct spa_meta_header)));
    params[count++] = spa_pod_builder_add_object(
        &b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type,
        SPA_POD_Id(SPA_META_VideoCrop), SPA_PARAM_META_size,
        SPA_POD_Int(sizeof(struct spa_meta_region)));
    params[count++] = spa_pod_builder_add_object(
        &b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type,
        SPA_POD_Id(SPA_META_VideoTransform), SPA_PARAM_META_size,
        SPA_POD_Int(sizeof(struct spa_meta_videotransform)));
    int ret = pw_stream_update_params(c->video, params, count);
    if (ret < 0)
        error(c, ret, "negotiate buffer metadata");
    else
        atomic_store_explicit(&c->formatted, true, memory_order_release);
    wake(c);
}

static bool validate_metadata(struct gk_capture *c, struct spa_buffer *b,
                              struct spa_meta_header **header)
{
    *header = spa_buffer_find_meta_data(b, SPA_META_Header, sizeof(**header));
    if (*header &&
        ((*header)->flags & (SPA_META_HEADER_FLAG_CORRUPTED | SPA_META_HEADER_FLAG_GAP))) {
        ++c->stats.corrupted;
        if (c->options.stop_on_overload)
            error(c, -EIO, "corrupt/missing frame under stop continuity policy");
        return false;
    }
    if (c->have_timestamp && *header && ((*header)->flags & SPA_META_HEADER_FLAG_DISCONT)) {
        error(c, -ESTALE, "source timestamp discontinuity");
        return false;
    }
    struct spa_meta_region *crop = spa_buffer_find_meta_data(b, SPA_META_VideoCrop, sizeof(*crop));
    if (crop && spa_meta_region_is_valid(crop) &&
        (crop->region.position.x || crop->region.position.y ||
         crop->region.size.width != (unsigned)c->format.width ||
         crop->region.size.height != (unsigned)c->format.height)) {
        error(c, -ENOTSUP, "source crop requires a new output plan");
        return false;
    }
    struct spa_meta_videotransform *transform =
        spa_buffer_find_meta_data(b, SPA_META_VideoTransform, sizeof(*transform));
    if (transform && transform->transform != SPA_META_TRANSFORMATION_None) {
        error(c, -ENOTSUP, "source rotation requires a new output plan");
        return false;
    }
    if (spa_buffer_find_meta(b, SPA_META_SyncTimeline)) {
        error(c, -EPROTO, "explicit sync was not negotiated by this implicit-sync plan");
        return false;
    }
    return true;
}

static bool video_timestamp(struct gk_capture *c, const struct pw_buffer *buffer,
                            const struct spa_meta_header *header, int64_t arrival, int64_t *pts)
{
    /* Some producers (including Niri 26.04) populate Header.seq but leave pts
     * zero. An initialized graph timestamp does not establish a running clock
     * either. Auto uses receive time from the first frame when source PTS is
     * absent; explicit source mode also accepts a valid zero-based epoch. */
    if (c->stats.video_clock == GK_VIDEO_CLOCK_AUTO) {
        enum gk_video_clock clock = c->options.video_clock;
        if (clock == GK_VIDEO_CLOCK_AUTO)
            clock = header && header->pts > 0 ? GK_VIDEO_CLOCK_SOURCE : GK_VIDEO_CLOCK_RECEIVE;
        c->stats.video_clock = clock;
        if (clock == GK_VIDEO_CLOCK_CYCLE)
            fputs("capture: using PipeWire graph-cycle timestamps\n", stderr);
        else if (clock == GK_VIDEO_CLOCK_RECEIVE)
            fputs("capture: using monotonic receive timestamps; source capture latency is "
                  "unknown (--video-clock source requires source PTS)\n",
                  stderr);
    }
    switch (c->stats.video_clock) {
    case GK_VIDEO_CLOCK_SOURCE:
        if (!header || header->pts < 0) {
            error(c, -EPROTO, "selected source timestamp is unavailable");
            return false;
        }
        *pts = header->pts;
        break;
    case GK_VIDEO_CLOCK_CYCLE:
        if (!buffer->time || buffer->time > INT64_MAX) {
            error(c, -EPROTO, "selected PipeWire cycle timestamp is unavailable");
            return false;
        }
        *pts = (int64_t)buffer->time;
        break;
    case GK_VIDEO_CLOCK_RECEIVE:
        *pts = arrival;
        break;
    default:
        error(c, -EINVAL, "invalid video clock");
        return false;
    }
    if (c->have_timestamp && *pts <= c->last_timestamp) {
        /* Sequence alone is not frame identity: its frequency is media-specific.
         * Only an identical source (PTS, sequence) pair identifies a duplicate.
         * Do not hide a lost clock by indefinitely repeating the last picture. */
        if (c->stats.video_clock == GK_VIDEO_CLOCK_SOURCE && header && *pts == c->last_timestamp &&
            header->seq == c->last_sequence)
            return false;
        error(c, -ESTALE, "video clock stopped or moved backwards");
        return false;
    }
    c->last_timestamp = *pts;
    c->last_sequence = header ? header->seq : 0;
    c->have_timestamp = true;
    return true;
}

static void video_process(void *opaque)
{
    struct gk_capture *c = opaque;
    struct pw_buffer *buffer;
    TracyCZoneN(zone, "capture.video-handoff", true);
    bool published = false;
    while ((buffer = pw_stream_dequeue_buffer(c->video))) {
        struct record *r = buffer->user_data;
        struct spa_buffer *b = buffer->buffer;
        ++c->stats.offered;
        struct spa_meta_header *header;
        if (r && r->held && c->options.stop_on_overload)
            error(c, -ENOBUFS, "source copy slot exhausted under stop continuity policy");
        if (c->stopping || atomic_load(&c->error) || !r || r->held ||
            !validate_metadata(c, b, &header))
            goto skipped;
        /* Sample receipt before diagnostics, copies or other variable work. */
        int64_t pts, arrival = gk_now_ns();
        if (!video_timestamp(c, buffer, header, arrival, &pts))
            goto skipped;
        if (!c->rate_started)
            c->rate_origin = pts;
        uint64_t slot;
        if (!gk_rate_slot(pts - c->rate_origin, c->options.fps_num, c->options.fps_den, &slot)) {
            error(c, -ERANGE, "video timestamp precedes the capture epoch");
            goto skipped;
        }
        if (c->rate_started && slot <= c->rate_slot) {
            ++c->stats.rate_limited;
            pw_stream_queue_buffer(c->video, buffer);
            continue;
        }
        for (unsigned i = 0; i < b->n_datas; ++i) {
            if (b->datas[i].chunk &&
                ((uint32_t)b->datas[i].chunk->flags & SPA_CHUNK_FLAG_CORRUPTED)) {
                ++c->stats.corrupted;
                if (c->options.stop_on_overload)
                    error(c, -EIO, "corrupt video chunk under stop continuity policy");
                goto skipped;
            }
        }
        if (c->format.dmabuf) {
            if (b->n_datas != r->frame.image.plane_count)
                goto malformed;
            for (unsigned i = 0; i < b->n_datas; ++i) {
                struct spa_data *data = &b->datas[i];
                struct spa_chunk *chunk = data->chunk;
                uint32_t offset;
                if (!chunk || chunk->stride <= 0 ||
                    ckd_add(&offset, data->mapoffset, chunk->offset) ||
                    offset >= r->frame.image.planes[i].size)
                    goto malformed;
                r->frame.image.planes[i].offset = offset;
                r->frame.image.planes[i].pitch = (uint32_t)chunk->stride;
            }
        } else {
            if (b->n_datas != 1 || !b->datas[0].data || !b->datas[0].chunk)
                goto malformed;
            struct spa_data *data = &b->datas[0];
            struct spa_chunk *chunk = data->chunk;
            size_t extent;
            if (chunk->stride < c->format.width * 4 ||
                ckd_mul(&extent, (size_t)(c->format.height - 1), (size_t)chunk->stride) ||
                ckd_add(&extent, extent, (size_t)c->format.width * 4) || extent > chunk->size ||
                chunk->offset > data->maxsize || extent > data->maxsize - chunk->offset)
                goto malformed;
            for (int y = 0; y < c->format.height; ++y)
                memcpy(r->pixels + (size_t)y * (size_t)c->format.width * 4,
                       (uint8_t *)data->data + chunk->offset + (size_t)y * (size_t)chunk->stride,
                       (size_t)c->format.width * 4);
        }
        r->frame.pts = pts;
        r->frame.arrival = arrival;
        r->held = true;
        if (!gk_ring_push(&c->video_ready, r->frame.token)) {
            r->held = false;
            if (c->options.stop_on_overload)
                error(c, -ENOBUFS, "source queue exhausted under stop continuity policy");
            goto skipped;
        }
        ++c->stats.admitted;
        c->rate_started = true;
        c->rate_slot = slot;
        published = true;
        if (!c->format.dmabuf)
            pw_stream_queue_buffer(c->video, buffer);
        continue;
    malformed:
        error(c, -EINVAL, "malformed frame extent/plane layout");
    skipped:
        ++c->stats.skipped;
        pw_stream_queue_buffer(c->video, buffer);
    }
    if (published)
        wake(c);
    TracyCZoneEnd(zone);
}

static const struct pw_stream_events video_events = {
    .version = PW_VERSION_STREAM_EVENTS,
    .state_changed = state_changed,
    .param_changed = video_param,
    .add_buffer = add_buffer,
    .remove_buffer = remove_buffer,
    .process = video_process,
};

static enum AVChannel audio_channel(uint32_t position)
{
    switch (position) {
    case SPA_AUDIO_CHANNEL_MONO:
    case SPA_AUDIO_CHANNEL_FC:
        return AV_CHAN_FRONT_CENTER;
    case SPA_AUDIO_CHANNEL_FL:
        return AV_CHAN_FRONT_LEFT;
    case SPA_AUDIO_CHANNEL_FR:
        return AV_CHAN_FRONT_RIGHT;
    case SPA_AUDIO_CHANNEL_LFE:
        return AV_CHAN_LOW_FREQUENCY;
    case SPA_AUDIO_CHANNEL_RL:
        return AV_CHAN_BACK_LEFT;
    case SPA_AUDIO_CHANNEL_RR:
        return AV_CHAN_BACK_RIGHT;
    case SPA_AUDIO_CHANNEL_FLC:
        return AV_CHAN_FRONT_LEFT_OF_CENTER;
    case SPA_AUDIO_CHANNEL_FRC:
        return AV_CHAN_FRONT_RIGHT_OF_CENTER;
    case SPA_AUDIO_CHANNEL_RC:
        return AV_CHAN_BACK_CENTER;
    case SPA_AUDIO_CHANNEL_SL:
        return AV_CHAN_SIDE_LEFT;
    case SPA_AUDIO_CHANNEL_SR:
        return AV_CHAN_SIDE_RIGHT;
    case SPA_AUDIO_CHANNEL_TC:
        return AV_CHAN_TOP_CENTER;
    case SPA_AUDIO_CHANNEL_TFL:
        return AV_CHAN_TOP_FRONT_LEFT;
    case SPA_AUDIO_CHANNEL_TFC:
        return AV_CHAN_TOP_FRONT_CENTER;
    case SPA_AUDIO_CHANNEL_TFR:
        return AV_CHAN_TOP_FRONT_RIGHT;
    case SPA_AUDIO_CHANNEL_TRL:
        return AV_CHAN_TOP_BACK_LEFT;
    case SPA_AUDIO_CHANNEL_TRC:
        return AV_CHAN_TOP_BACK_CENTER;
    case SPA_AUDIO_CHANNEL_TRR:
        return AV_CHAN_TOP_BACK_RIGHT;
    default:
        return AV_CHAN_NONE;
    }
}

static void audio_state_changed(void *opaque, enum pw_stream_state old, enum pw_stream_state state,
                                const char *message)
{
    struct audio_input *a = opaque;
    state_changed(a->capture, old, state, message);
}

static void audio_param(void *opaque, uint32_t id, const struct spa_pod *param)
{
    struct audio_input *a = opaque;
    struct gk_capture *c = a->capture;
    if (id != SPA_PARAM_Format || c->stopping)
        return;
    if (!param) {
        if (atomic_load(&a->formatted))
            error(c, -ESTALE, "selected audio format was removed");
        return;
    }
    struct spa_audio_info_raw raw = {};
    if (spa_format_audio_raw_parse(param, &raw) < 0 || raw.format != SPA_AUDIO_FORMAT_F32 ||
        !raw.channels || raw.channels > GK_AUDIO_CHANNELS ||
        (raw.flags & SPA_AUDIO_FLAG_UNPOSITIONED)) {
        error(c, -ENOTSUP, "audio requires positioned, interleaved float channels");
        return;
    }
    struct gk_audio_format format = {.rate = raw.rate, .channels = raw.channels};
    format.discrete = raw.position[0] >= SPA_AUDIO_CHANNEL_AUX0 &&
                      raw.position[0] < SPA_AUDIO_CHANNEL_AUX0 + GK_AUDIO_CHANNELS;
    for (unsigned i = 0; i < raw.channels; ++i)
        if (format.discrete) {
            if (raw.position[i] < SPA_AUDIO_CHANNEL_AUX0 ||
                raw.position[i] >= SPA_AUDIO_CHANNEL_AUX0 + GK_AUDIO_CHANNELS) {
                error(c, -ENOTSUP, "mixed AUX/speaker layouts require an explicit routing plan");
                return;
            }
            format.auxiliary[i] = (uint8_t)(raw.position[i] - SPA_AUDIO_CHANNEL_AUX0);
        } else
            format.position[i] = audio_channel(raw.position[i]);
    AVChannelLayout input = {}, output = {};
    int check = gk_audio_layout(&format, &input, &output);
    av_channel_layout_uninit(&input);
    av_channel_layout_uninit(&output);
    if (check < 0) {
        error(c, check, "unsupported audio rate or channel identity; no downmix was performed");
        return;
    }
    if (atomic_load(&a->formatted)) {
        size_t positions =
            format.channels * (format.discrete ? sizeof(uint8_t) : sizeof(enum AVChannel));
        if (format.rate != a->format.rate || format.channels != a->format.channels ||
            format.discrete != a->format.discrete ||
            memcmp(&format.position, &a->format.position, positions))
            error(c, -ESTALE, "selected audio rate/layout changed");
        return;
    }
    size_t bytes = (size_t)AUDIO_SLOTS * GK_AUDIO_FRAMES * format.channels * sizeof(float);
    a->samples = malloc(bytes);
    if (!a->samples) {
        error(c, -ENOMEM, "allocate bounded audio slabs");
        return;
    }
    /* First-touch every page on the control loop, before the RT callback can
     * publish. Explicit stores prevent malloc+memset folding to lazy calloc. */
    memset_explicit(a->samples, 0, bytes);
    a->format = format;
    unsigned stride = format.channels * sizeof(float);
    uint8_t storage[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
    const struct spa_pod *params[2];
    params[0] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
                                           SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
                                           SPA_PARAM_META_size,
                                           SPA_POD_Int(sizeof(struct spa_meta_header)));
    params[1] = spa_pod_builder_add_object(
        &b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers, SPA_PARAM_BUFFERS_size,
        SPA_POD_CHOICE_RANGE_Int((int)(GK_AUDIO_FRAMES * stride), (int)stride,
                                 (int)(GK_AUDIO_FRAMES * stride)),
        SPA_PARAM_BUFFERS_dataType,
        SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr)));
    int ret = pw_stream_update_params(a->stream, params, 2);
    if (ret < 0)
        error(c, ret, "negotiate audio metadata");
    else {
        atomic_store_explicit(&a->formatted, true, memory_order_release);
        wake(c);
    }
}

static void audio_process(void *opaque)
{
    struct audio_input *a = opaque;
    struct gk_capture *c = a->capture;
    struct pw_buffer *buffer;
    bool published = false;
    TracyCZoneN(zone, "capture.audio-copy", true);
    while ((buffer = pw_stream_dequeue_buffer(a->stream))) {
        struct spa_buffer *b = buffer->buffer;
        uint64_t slot;
        if (c->stopping || atomic_load(&c->error) ||
            !atomic_load_explicit(&a->formatted, memory_order_acquire))
            goto next;
        struct spa_meta_header *header =
            spa_buffer_find_meta_data(b, SPA_META_Header, sizeof(*header));
        if (b->n_datas != 1 || !b->datas[0].data || !b->datas[0].chunk) {
            audio_error(c, -EPROTO);
            goto next;
        }
        if (a->seen && header && (header->flags & SPA_META_HEADER_FLAG_DISCONT)) {
            audio_error(c, -ESTALE);
            goto next;
        }
        int64_t pts;
        /* Audio converters/mixers may leave Header unpopulated. Select one
         * timing authority at the first block and keep it for this generation.
         * This is graph/source time, never the callback's wall-clock arrival. */
        if (!a->seen) {
            a->header_clock = header && header->pts > 0;
            a->seen = true;
        }
        if (a->header_clock) {
            if (!header || header->pts <= 0) {
                audio_error(c, -ESTALE);
                goto next;
            }
            pts = header->pts;
        } else {
            struct pw_time time;
            if (pw_stream_get_time_n(a->stream, &time, sizeof(time)) < 0 ||
                !gk_audio_cycle_pts(buffer->time, time.delay, time.rate.num, time.rate.denom,
                                    time.buffered, a->format.rate, &pts)) {
                audio_error(c, -EPROTO);
                goto next;
            }
        }
        struct spa_data *data = &b->datas[0];
        struct spa_chunk *chunk = data->chunk;
        unsigned stride = a->format.channels * sizeof(float);
        if (chunk->size > GK_AUDIO_FRAMES * stride || chunk->size % stride ||
            chunk->offset > data->maxsize || chunk->size > data->maxsize - chunk->offset) {
            audio_error(c, -ENOBUFS);
            goto next;
        }
        if (!gk_ring_pop(&a->free, &slot)) {
            ++a->dropped;
            audio_error(c, -ENOBUFS);
            goto next;
        }
        struct audio_record *r = &a->records[slot];
        float *samples = a->samples + slot * GK_AUDIO_FRAMES * a->format.channels;
        r->pts = pts;
        r->frames = chunk->size / stride;
        if ((header &&
             (header->flags & (SPA_META_HEADER_FLAG_CORRUPTED | SPA_META_HEADER_FLAG_GAP))) ||
            ((uint32_t)chunk->flags & (SPA_CHUNK_FLAG_CORRUPTED | SPA_CHUNK_FLAG_EMPTY))) {
            memset(samples, 0, chunk->size);
            a->silenced += r->frames;
        } else
            memcpy(samples, (uint8_t *)data->data + chunk->offset, chunk->size);
        if (!gk_ring_push(&a->ready, slot))
            audio_error(c, -EIO);
        ++a->blocks;
        published = true;
    next:
        pw_stream_queue_buffer(a->stream, buffer);
    }
    if (published) {
        int notify = gk_notify(c->wake);
        if (notify < 0)
            audio_error(c, notify);
    }
    TracyCZoneEnd(zone);
}

static const struct pw_stream_events audio_events = {
    .version = PW_VERSION_STREAM_EVENTS,
    .state_changed = audio_state_changed,
    .param_changed = audio_param,
    .process = audio_process,
};

static int connect_audio(struct gk_capture *c, unsigned track)
{
    struct audio_input *a = &c->audio[track];
    int ret = gk_ring_init(&a->ready, AUDIO_SLOTS);
    if (ret < 0)
        return ret;
    ret = gk_ring_init(&a->free, AUDIO_SLOTS);
    if (ret < 0)
        return ret;
    for (unsigned i = 0; i < AUDIO_SLOTS; ++i)
        if (!gk_ring_push(&a->free, i))
            return -EIO;
    char name[64];
    snprintf(name, sizeof(name), "gpukey-audio-%u", track + 1);
    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE,
        "Production", PW_KEY_APP_NAME, "gpuKey", PW_KEY_NODE_NAME, name, PW_KEY_TARGET_OBJECT,
        a->serial, PW_KEY_STREAM_DONT_REMIX, "true", "node.dont-fallback", "true", "node.dont-move",
        "true", PW_KEY_STREAM_CAPTURE_SINK, a->monitor ? "true" : "false", nullptr);
    if (!props)
        return -ENOMEM;
    a->stream = pw_stream_new(c->audio_core, a->title, props);
    if (!a->stream)
        return -errno;
    pw_stream_add_listener(a->stream, &a->listener, &audio_events, a);
    uint8_t storage[1024];
    struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
    /* PipeWire's native capture contract: supply the target node's advertised
     * channel positions when present so WirePlumber and stream.dont-remix do
     * not default an unspecified format to unpositioned stereo. */
    struct spa_audio_info_raw info = a->requested;
    info.format = SPA_AUDIO_FORMAT_F32;
    const struct spa_pod *param = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info);
    return pw_stream_connect(a->stream, PW_DIRECTION_INPUT, PW_ID_ANY,
                             PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
                                 PW_STREAM_FLAG_DONT_RECONNECT | PW_STREAM_FLAG_RT_PROCESS,
                             &param, 1);
}

static void audio_node_info(void *opaque, const struct pw_node_info *info)
{
    struct audio_input *a = opaque;
    if (!info || !info->props || a->requested.channels)
        return;
    const char *pos = spa_dict_lookup(info->props, SPA_KEY_AUDIO_POSITION);
    if (pos)
        spa_audio_parse_position_n(pos, strlen(pos), a->requested.position, SPA_AUDIO_MAX_CHANNELS,
                                   &a->requested.channels);
}

static const struct pw_node_events audio_node_events = {
    .version = PW_VERSION_NODE_EVENTS,
    .info = audio_node_info,
};

static void audio_global(void *opaque, uint32_t id, uint32_t permissions, const char *type,
                         uint32_t version, const struct spa_dict *props)
{
    (void)permissions;
    (void)version;
    struct gk_capture *c = opaque;
    struct gk_device_info info;
    if (c->stopping || atomic_load(&c->error) || strcmp(type, PW_TYPE_INTERFACE_Node) ||
        !gk_device_info(props, &info))
        return;
    size_t name_len = strlen(info.name);
    for (unsigned i = 0; i < c->options.audio_count; ++i) {
        const struct gk_audio_selection *selection = &c->options.audio[i];
        struct audio_input *a = &c->audio[i];
        bool monitor_alias = info.monitor && !strncmp(selection->target, info.name, name_len) &&
                             !strcmp(selection->target + name_len, ".monitor");
        if (strcmp(selection->target, info.serial) && strcmp(selection->target, info.name) &&
            !monitor_alias)
            continue;
        if (a->selected) {
            if (a->node != id)
                error(c, -EEXIST, "audio identity is ambiguous; select a device ID");
            continue;
        }
        if ((selection->kind == GK_AUDIO_SOURCE && info.monitor) ||
            (selection->kind == GK_AUDIO_MONITOR && !info.monitor)) {
            error(c, -EINVAL, "audio source/monitor kind does not match; use --audio ID");
            return;
        }
        if (strlen(info.serial) >= sizeof(a->serial)) {
            error(c, -EOVERFLOW, "invalid PipeWire object serial");
            return;
        }
        a->title = strdup(info.description);
        if (!a->title) {
            error(c, -ENOMEM, "copy audio track title");
            return;
        }
        for (char *p = a->title; *p; ++p)
            if ((unsigned char)*p < 32 || *p == 127)
                *p = '?';
        strcpy(a->serial, info.serial);
        a->node = id;
        a->monitor = info.monitor;
        a->selected = true;
        const char *pos = spa_dict_lookup(props, SPA_KEY_AUDIO_POSITION);
        if (pos)
            spa_audio_parse_position_n(pos, strlen(pos), a->requested.position,
                                       SPA_AUDIO_MAX_CHANNELS, &a->requested.channels);
        a->node_proxy =
            pw_registry_bind(c->audio_registry, id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0);
        if (a->node_proxy) {
            pw_node_add_listener(a->node_proxy, &a->node_listener, &audio_node_events, a);
            c->audio_sync = pw_core_sync(c->audio_core, PW_ID_CORE, c->audio_sync);
        }
    }
}

static void audio_removed(void *opaque, uint32_t id)
{
    struct gk_capture *c = opaque;
    if (c->stopping)
        return;
    for (unsigned i = 0; i < c->options.audio_count; ++i)
        if (c->audio[i].selected && c->audio[i].node == id)
            error(c, -EPIPE, "selected audio device was removed; no fallback");
}

static void audio_done(void *opaque, uint32_t id, int seq)
{
    struct gk_capture *c = opaque;
    if (id != PW_ID_CORE || seq != c->audio_sync || c->stopping || atomic_load(&c->error))
        return;
    /* Resolve the entire registry snapshot before connecting any stream.
     * Pin object serials on this same connection, rather than re-resolving names
     * against another server/connection or allowing session-manager fallback. */
    for (unsigned i = 0; i < c->options.audio_count; ++i) {
        if (!c->audio[i].selected) {
            fprintf(stderr, "audio device not found: %s; run 'gpukey devices'\n",
                    c->options.audio[i].target);
            error(c, -ENOENT, "selected audio is unavailable");
            return;
        }
        for (unsigned j = 0; j < i; ++j)
            if (c->audio[i].node == c->audio[j].node) {
                error(c, -EEXIST, "the same audio device was selected twice");
                return;
            }
    }
    for (unsigned i = 0; i < c->options.audio_count; ++i) {
        struct audio_input *a = &c->audio[i];
        if (a->node_proxy) {
            spa_hook_remove(&a->node_listener);
            pw_proxy_destroy((struct pw_proxy *)a->node_proxy);
            a->node_proxy = nullptr;
        }
        int ret = connect_audio(c, i);
        if (ret < 0) {
            error(c, ret, "connect selected audio");
            return;
        }
    }
}

static const struct pw_core_events audio_core_events = {
    .version = PW_VERSION_CORE_EVENTS, .done = audio_done, .error = core_error};
static const struct pw_registry_events audio_registry_events = {
    .version = PW_VERSION_REGISTRY_EVENTS, .global = audio_global, .global_remove = audio_removed};

static struct spa_pod *video_format(struct spa_pod_builder *b, enum spa_video_format format,
                                    const struct gk_capture *c, const uint64_t *modifiers,
                                    unsigned modifier_count)
{
    struct spa_pod_frame object;
    spa_pod_builder_push_object(b, &object, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
    spa_pod_builder_add(
        b, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), SPA_FORMAT_mediaSubtype,
        SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), SPA_FORMAT_VIDEO_format, SPA_POD_Id(format),
        SPA_FORMAT_VIDEO_size,
        SPA_POD_CHOICE_RANGE_Rectangle(&SPA_RECTANGLE(1920, 1080), &SPA_RECTANGLE(1, 1),
                                       &SPA_RECTANGLE(INT32_MAX / 4, INT32_MAX)),
        SPA_FORMAT_VIDEO_framerate,
        SPA_POD_CHOICE_RANGE_Fraction(&SPA_FRACTION(0, 1), &SPA_FRACTION(0, 1),
                                      &SPA_FRACTION(c->options.fps_num, c->options.fps_den)),
        SPA_FORMAT_VIDEO_maxFramerate,
        SPA_POD_CHOICE_RANGE_Fraction(&SPA_FRACTION(c->options.fps_num, c->options.fps_den),
                                      &SPA_FRACTION(1, 1),
                                      &SPA_FRACTION(c->options.fps_num, c->options.fps_den)),
        0);
    if (modifier_count) {
        struct spa_pod_frame choice;
        spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_modifier,
                             SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE);
        spa_pod_builder_push_choice(b, &choice, SPA_CHOICE_Enum, 0);
        spa_pod_builder_long(b, (int64_t)modifiers[0]);
        for (unsigned i = 0; i < modifier_count; ++i)
            spa_pod_builder_long(b, (int64_t)modifiers[i]);
        spa_pod_builder_pop(b, &choice);
    }
    return spa_pod_builder_pop(b, &object);
}

int gk_capture_open(struct gk_capture **out, struct gk_portal_source source,
                    const struct gk_gpu *gpu, const struct gk_capture_options *options)
{
    *out = nullptr;
    size_t aligned = (sizeof(struct gk_capture) + 63) & ~(size_t)63;
    struct gk_capture *c = aligned_alloc(64, aligned);
    int ret = -ENOMEM;
    if (!c) {
        if (source.fd >= 0)
            close(source.fd);
        return ret;
    }
    *c = (struct gk_capture){.options = *options, .wake = -1};
    atomic_init(&c->formatted, false);
    atomic_init(&c->error, 0);
    atomic_init(&c->stopping, false);
    if (options->video_clock > GK_VIDEO_CLOCK_RECEIVE || options->buffers < 2 ||
        options->buffers > INT32_MAX / 2 ||
        (options->preferred_buffers &&
         (options->preferred_buffers < 2 || options->preferred_buffers > options->buffers))) {
        ret = -EINVAL;
        goto fail;
    }
    c->records = calloc(options->buffers, sizeof(*c->records));
    if (!c->records)
        goto fail;
    ret = gk_ring_init(&c->video_ready, stdc_bit_ceil(options->buffers));
    if (ret < 0)
        goto fail;
    static const enum AVPixelFormat pixels[] = {AV_PIX_FMT_BGR0, AV_PIX_FMT_BGRA, AV_PIX_FMT_RGB0,
                                                AV_PIX_FMT_RGBA};
    unsigned modifier_count[4] = {};
    const uint64_t *modifiers[4] = {};
    size_t modifier_total = 0;
    for (unsigned i = 0; !options->force_shm && i < 4; ++i) {
        modifiers[i] = gk_gpu_modifiers(gpu, pixels[i], &modifier_count[i]);
        if (ckd_add(&modifier_total, modifier_total, modifier_count[i])) {
            ret = -EOVERFLOW;
            goto fail;
        }
    }
    if (!modifier_total && !options->allow_shm) {
        ret = -ENOTSUP;
        goto fail;
    }
    c->wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (c->wake < 0) {
        ret = -errno;
        goto fail;
    }
    pw_init(nullptr, nullptr);
    c->loop = pw_thread_loop_new("gpukey-capture", nullptr);
    if (!c->loop)
        goto fail;
    c->context = pw_context_new(pw_thread_loop_get_loop(c->loop), nullptr, 0);
    if (!c->context)
        goto fail;
    c->core = source.fd >= 0 ? pw_context_connect_fd(c->context, source.fd, nullptr, 0)
                             : pw_context_connect(c->context, nullptr, 0);
    if (!c->core) {
        ret = -errno;
        goto fail;
    }
    /* The audited native protocol only adopts/auto-closes the fd after its
     * event-loop source was installed. Failed connect_fd leaves it to us. */
    source.fd = -1;
    pw_core_add_listener(c->core, &c->core_listener, &core_events, c);
    c->video_watch =
        (struct target_watch){.capture = c, .node = source.node, .serial = source.serial};
    c->video_watch.registry = pw_core_get_registry(c->core, PW_VERSION_REGISTRY, 0);
    if (!c->video_watch.registry)
        goto fail;
    pw_registry_add_listener(c->video_watch.registry, &c->video_watch.listener, &target_events,
                             &c->video_watch);
    char target[32];
    snprintf(target, sizeof(target), "%" PRIu64, source.serial ? source.serial : source.node);
    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Screen",
        PW_KEY_APP_NAME, "gpuKey", PW_KEY_NODE_NAME, "gpukey-video", "node.dont-fallback", "true",
        "node.dont-move", "true", nullptr);
    if (source.serial)
        pw_properties_set(props, PW_KEY_TARGET_OBJECT, target);
    c->video = pw_stream_new(c->core, "gpuKey screen", props);
    if (!c->video)
        goto fail;
    pw_stream_add_listener(c->video, &c->video_listener, &video_events, c);
    size_t storage_bytes;
    if (ckd_mul(&storage_bytes, modifier_total, sizeof(uint64_t)) ||
        ckd_add(&storage_bytes, storage_bytes, 8192U) || storage_bytes > UINT32_MAX) {
        ret = -EOVERFLOW;
        goto fail;
    }
    uint8_t *storage = malloc(storage_bytes);
    if (!storage) {
        ret = -ENOMEM;
        goto fail;
    }
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(storage, (uint32_t)storage_bytes);
    const struct spa_pod *params[8];
    unsigned count = 0;
    static const enum spa_video_format formats[] = {SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA,
                                                    SPA_VIDEO_FORMAT_RGBx, SPA_VIDEO_FORMAT_RGBA};
    for (unsigned i = 0; i < 4; ++i)
        if (modifier_count[i])
            params[count++] = video_format(&b, formats[i], c, modifiers[i], modifier_count[i]);
    if (options->allow_shm)
        for (unsigned i = 0; i < 4; ++i)
            params[count++] = video_format(&b, formats[i], c, nullptr, 0);
    for (unsigned i = 0; i < count; ++i)
        if (!params[i]) {
            free(storage);
            ret = -EOVERFLOW;
            goto fail;
        }
    ret = pw_stream_connect(c->video, PW_DIRECTION_INPUT, source.serial ? PW_ID_ANY : source.node,
                            PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
                                PW_STREAM_FLAG_DONT_RECONNECT,
                            params, count);
    free(storage);
    if (ret < 0)
        goto fail;
    if (options->audio_count) {
        if (!options->audio) {
            ret = -EINVAL;
            goto fail;
        }
        size_t bytes;
        if (ckd_mul(&bytes, options->audio_count, sizeof(*c->audio))) {
            ret = -EOVERFLOW;
            goto fail;
        }
        c->audio = aligned_alloc(alignof(struct audio_input), bytes);
        if (!c->audio) {
            ret = -ENOMEM;
            goto fail;
        }
        memset(c->audio, 0, bytes);
        for (unsigned i = 0; i < options->audio_count; ++i) {
            c->audio[i].capture = c;
            atomic_init(&c->audio[i].formatted, false);
        }
        c->audio_core = pw_context_connect(c->context, nullptr, 0);
        if (!c->audio_core) {
            ret = -errno;
            goto fail;
        }
        pw_core_add_listener(c->audio_core, &c->audio_core_listener, &audio_core_events, c);
        c->audio_registry = pw_core_get_registry(c->audio_core, PW_VERSION_REGISTRY, 0);
        if (!c->audio_registry)
            goto fail;
        pw_registry_add_listener(c->audio_registry, &c->audio_registry_listener,
                                 &audio_registry_events, c);
        c->audio_sync = pw_core_sync(c->audio_core, PW_ID_CORE, 0);
        if (c->audio_sync < 0) {
            ret = c->audio_sync;
            goto fail;
        }
    }
    ret = pw_thread_loop_start(c->loop);
    if (ret < 0)
        goto fail;
    c->started = true;
    *out = c;
    return 0;
fail:
    if (source.fd >= 0)
        close(source.fd);
    gk_capture_close(&c, nullptr);
    return ret < 0 ? ret : -ENOMEM;
}

int gk_capture_fd(const struct gk_capture *c)
{
    return c->wake;
}

int gk_capture_error(const struct gk_capture *c)
{
    return atomic_load(&c->error);
}

bool gk_capture_format(const struct gk_capture *c, struct gk_video_format *format)
{
    if (!atomic_load_explicit(&c->formatted, memory_order_acquire))
        return false;
    *format = c->format;
    return true;
}

bool gk_capture_next(struct gk_capture *c, struct gk_capture_frame *frame)
{
    uint64_t token;
    if (!gk_ring_pop(&c->video_ready, &token))
        return false;
    *frame = c->records[(uint32_t)token].frame;
    return true;
}

bool gk_capture_begin_submission(struct gk_capture *c, uint64_t token)
{
    pw_thread_loop_lock(c->loop);
    struct record *r = &c->records[(uint32_t)token];
    if (!r->held || r->frame.token != token || !r->wrapper || c->stopping ||
        atomic_load(&c->error)) {
        pw_thread_loop_unlock(c->loop);
        return false;
    }
    return true;
}

void gk_capture_end_submission(struct gk_capture *c)
{
    pw_thread_loop_unlock(c->loop);
}

void gk_capture_release(struct gk_capture *c, uint64_t token)
{
    pw_thread_loop_lock(c->loop);
    struct record *r = &c->records[(uint32_t)token];
    if (!r->held || r->frame.token != token) {
        error(c, -EINVAL, "stale source retirement token");
    } else {
        if (r->wrapper && c->format.dmabuf)
            pw_stream_queue_buffer(c->video, r->wrapper);
        r->held = false;
        if (!r->wrapper)
            clear_record(r);
    }
    pw_thread_loop_unlock(c->loop);
}

bool gk_capture_audio_formats(const struct gk_capture *c, struct gk_audio_track *tracks)
{
    for (unsigned i = 0; i < c->options.audio_count; ++i) {
        const struct audio_input *a = &c->audio[i];
        if (!atomic_load_explicit(&a->formatted, memory_order_acquire))
            return false;
        tracks[i] = (struct gk_audio_track){.format = a->format, .title = a->title};
    }
    return true;
}

bool gk_capture_audio_next(struct gk_capture *c, struct gk_audio_frame *frame)
{
    /* Round-robin protects a slow source from a busy source. Each edge remains
     * SPSC even when PipeWire runs different devices on different RT threads. */
    for (unsigned n = 0; n < c->options.audio_count; ++n) {
        unsigned track = c->audio_cursor++;
        if (c->audio_cursor == c->options.audio_count)
            c->audio_cursor = 0;
        struct audio_input *a = &c->audio[track];
        if (!atomic_load_explicit(&a->formatted, memory_order_acquire))
            continue;
        uint64_t slot;
        if (!gk_ring_pop(&a->ready, &slot))
            continue;
        const struct audio_record *r = &a->records[slot];
        *frame = (struct gk_audio_frame){.samples = a->samples +
                                                    slot * GK_AUDIO_FRAMES * a->format.channels,
                                         .frames = r->frames,
                                         .track = track,
                                         .token = (uint64_t)track << 32 | slot,
                                         .pts = r->pts};
        return true;
    }
    return false;
}

void gk_capture_audio_release(struct gk_capture *c, uint64_t token)
{
    unsigned track = (unsigned)(token >> 32), slot = (uint32_t)token;
    if (track >= c->options.audio_count || slot >= AUDIO_SLOTS ||
        !gk_ring_push(&c->audio[track].free, slot))
        error(c, -EIO, "audio retirement invariant");
}

void gk_capture_stop(struct gk_capture *c)
{
    if (!c || !c->loop)
        return;
    if (c->started)
        pw_thread_loop_lock(c->loop);
    if (atomic_exchange(&c->stopping, true)) {
        if (c->started)
            pw_thread_loop_unlock(c->loop);
        return;
    }
    if (c->video)
        pw_stream_set_active(c->video, false);
    if (c->audio)
        for (unsigned i = 0; i < c->options.audio_count; ++i)
            if (c->audio[i].stream) {
                /* Audited PipeWire set_active holds the data-loop lock before
                 * returning. The stopping flag prevents later publication.
                 * The owner may now consume the final immutable queue prefix. */
                int ret = pw_stream_set_active(c->audio[i].stream, false);
                if (ret < 0 && atomic_load(&c->audio[i].formatted))
                    error(c, ret, "quiesce audio producer");
            }
    if (c->started)
        pw_thread_loop_unlock(c->loop);
}

void gk_capture_close(struct gk_capture **capture, struct gk_capture_stats *stats)
{
    struct gk_capture *c = *capture;
    if (!c)
        return;
    gk_capture_stop(c);
    if (c->started)
        pw_thread_loop_stop(c->loop);
    if (c->video)
        pw_stream_destroy(c->video);
    if (c->audio)
        for (unsigned i = 0; i < c->options.audio_count; ++i) {
            struct audio_input *a = &c->audio[i];
            if (a->node_proxy) {
                spa_hook_remove(&a->node_listener);
                pw_proxy_destroy((struct pw_proxy *)a->node_proxy);
            }
            /* Destroy synchronizes the RT data loop before reclaiming its pools. */
            if (a->stream)
                pw_stream_destroy(a->stream);
            c->stats.audio_blocks += a->blocks;
            c->stats.audio_dropped += a->dropped;
            c->stats.audio_silenced_samples += a->silenced;
            gk_ring_destroy(&a->ready);
            gk_ring_destroy(&a->free);
            free(a->samples);
            free(a->title);
        }
    free(c->audio);
    if (c->video_watch.registry)
        pw_proxy_destroy((struct pw_proxy *)c->video_watch.registry);
    if (c->audio_registry)
        pw_proxy_destroy((struct pw_proxy *)c->audio_registry);
    if (c->core)
        pw_core_disconnect(c->core);
    if (c->audio_core)
        pw_core_disconnect(c->audio_core);
    if (c->context)
        pw_context_destroy(c->context);
    if (c->loop)
        pw_thread_loop_destroy(c->loop);
    if (c->records)
        for (unsigned i = 0; i < c->options.buffers; ++i)
            clear_record(&c->records[i]);
    free(c->records);
    gk_ring_destroy(&c->video_ready);
    if (c->wake >= 0)
        close(c->wake);
    if (stats)
        *stats = c->stats;
    free(c);
    *capture = nullptr;
}

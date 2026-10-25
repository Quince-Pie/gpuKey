#include "media.h"
#include "av1.h"
#include "convert.h"
#include "vk_rgb.h"

#include <errno.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <stdckdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tracy/TracyC.h>

struct audio_track {
    AVCodecContext *codec;
    AVStream *stream;
    AVFrame *frame;
    AVAudioFifo *fifo;
    SwrContext *resampler;
    float *resampled;
    unsigned rate, channels;
    int quantum, capacity;
    bool started;
    int64_t observation, next_input, end, last_position, written, encoded;
};

struct gk_media {
    struct gk_gpu *gpu;
    struct gk_video_format source;
    struct gk_media_options options;
    AVCodecContext *video;
    const int *completion_fd;
    const uint64_t *completion_generation;
    struct audio_track *audio;
    AVFormatContext *mux;
    AVStream *video_stream;
    AVIOContext *io;
    AVBufferRef *frames, *imported_hold;
    AVFrame **pool, *staging, *last, *prepared;
    unsigned pool_count, pool_capacity;
#ifdef GK_CPU_CONVERSION_CONTROL
    struct SwsContext *sws;
#else
    uint16_t *conversion_rows;
#endif
    AVPacket *packet, *pending, *repeat_key;
    struct gk_sink *sink;
    struct gk_replay *replay;
    int64_t last_pts, last_keyframe;
    int64_t prepare_started;
    bool opened, header, finished, output_failed, direct, video_failed, static_key_pending;
    int error;
    struct gk_media_stats stats;
};

static int report_failure(struct gk_media *m, int ret, const char *where, bool output_failed)
{
    if (ret < 0) {
        char description[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, description, sizeof(description));
        fprintf(stderr, "%s: %s\n", where, description);
        if (!m->error)
            m->error = ret;
        m->output_failed |= output_failed;
    }
    return ret;
}

static int failed(struct gk_media *m, int ret, const char *where)
{
    return report_failure(m, ret, where, true);
}

static int failed_input(struct gk_media *m, int ret, const char *where)
{
    /* No new image reached the encoder. Stop admission, but preserve output
     * from earlier accepted inputs unless codec/mux validation also fails. */
    return report_failure(m, ret, where, false);
}

static void color(AVFrame *frame, const AVCodecContext *codec)
{
    frame->color_primaries = codec->color_primaries;
    frame->color_trc = codec->color_trc;
    frame->colorspace = codec->colorspace;
    frame->color_range = codec->color_range;
    frame->chroma_location = AVCHROMA_LOC_LEFT;
    frame->sample_aspect_ratio = (AVRational){1, 1};
}

static int allocate_input_impl(struct gk_media *m, AVFrame **out)
{
    *out = nullptr;
    if (m->pool_count == m->options.surfaces)
        return -EAGAIN;
    if (m->pool_count == m->pool_capacity) {
        unsigned capacity = 1;
        if (m->pool_capacity && (ckd_add(&capacity, m->pool_capacity, m->pool_capacity) ||
                                 capacity > m->options.surfaces))
            capacity = m->options.surfaces;
        size_t bytes;
        if (ckd_mul(&bytes, capacity, sizeof(*m->pool)))
            return -EOVERFLOW;
        void *grown = realloc(m->pool, bytes);
        if (!grown)
            return -ENOMEM;
        m->pool = grown;
        m->pool_capacity = capacity;
    }
    AVFrame *frame = av_frame_alloc();
    if (!frame)
        return -ENOMEM;
    m->pool[m->pool_count++] = frame;
    frame->format = m->video->pix_fmt;
    frame->width = m->source.width;
    frame->height = m->source.height;
    int ret =
        m->frames ? av_hwframe_get_buffer(m->frames, frame, 0) : av_frame_get_buffer(frame, 64);
    if (ret < 0)
        return ret;
    color(frame, m->video);
    *out = frame;
    return 0;
}

static int allocate_input(struct gk_media *m, AVFrame **out)
{
    TracyCZoneN(zone, "media.allocate-input", true);
    int ret = allocate_input_impl(m, out);
    TracyCZoneEnd(zone);
    return ret;
}

static int ensure_staging(struct gk_media *m)
{
    if (m->staging)
        return 0;
    m->staging = av_frame_alloc();
    if (!m->staging)
        return -ENOMEM;
    m->staging->format = m->video->sw_pix_fmt;
    m->staging->width = m->source.width;
    m->staging->height = m->source.height;
    int ret = av_frame_get_buffer(m->staging, 64);
    if (ret < 0)
        av_frame_free(&m->staging);
    return ret;
}

static int write_header(struct gk_media *m, const AVPacket *first)
{
    if (m->header)
        return 0;
    int configured = gk_av1_configuration(m->video_stream->codecpar, first, m->video->time_base,
                                          m->source.width, m->source.height, m->options.bits);
    if (configured < 0)
        return configured;
    if (m->replay) {
        m->header = true; /* Codec configuration ready; live replay does no mux I/O. */
        return 0;
    }
    AVDictionary *options = nullptr;
    if (!m->options.matroska) {
        av_dict_set(&options, "movflags", "delay_moov+default_base_moof+frag_keyframe", 0);
        av_dict_set(&options, "video_track_timescale", "1000000", 0);
        av_dict_set(&options, "frag_duration", "1000000", 0);
    } else {
        av_dict_set(&options, "cluster_time_limit", "1000", 0);
        if (av_opt_find(m->mux->priv_data, "write_video_duration", nullptr, 0, 0))
            av_dict_set(&options, "write_video_duration", "1", 0);
    }
    int ret = avformat_write_header(m->mux, &options);
    if (ret >= 0 && av_dict_count(options))
        ret = -ENOTSUP;
    av_dict_free(&options);
    if (ret >= 0)
        m->header = true;
    return ret;
}

static int write_video(struct gk_media *m, int64_t end)
{
    if (!m->pending->buf)
        return 0;
    if (m->output_failed) {
        av_packet_unref(m->pending);
        return 0;
    }
    if (end <= m->pending->pts)
        return failed(m, -EPROTO, "nonpositive displayed duration");
    if (!m->options.matroska && end - m->pending->pts > INT_MAX)
        return failed(m, -ERANGE, "MP4 sample duration exceeds the mux API field");
    /* Round endpoints once; rounding the delta independently can overlap the
     * next presentation by a tick (e.g. 60 Hz on a millisecond time base). */
    int64_t serialized_start =
        av_rescale_q(m->pending->pts, m->video->time_base, m->video_stream->time_base);
    int64_t duration =
        av_rescale_q(end, m->video->time_base, m->video_stream->time_base) - serialized_start;
    if (duration <= 0)
        return failed(m, -ERANGE, "container precision cannot represent this frame interval");
    av_packet_rescale_ts(m->pending, m->video->time_base, m->video_stream->time_base);
    m->pending->duration = duration;
    m->pending->stream_index = m->video_stream->index;
    return failed(m, av_interleaved_write_frame(m->mux, m->pending), "mux video");
}

static int send_video(struct gk_media *m, const AVFrame *frame)
{
    int ret = avcodec_send_frame(m->video, frame);
    if (ret < 0 && ret != AVERROR(EAGAIN) && (frame || ret != AVERROR_EOF))
        m->video_failed = true;
    return ret;
}

static int receive_video(struct gk_media *m)
{
    for (;;) {
        int ret = avcodec_receive_packet(m->video, m->packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return 0;
        if (ret < 0) {
            m->video_failed = true;
            return report_failure(m, ret, "receive AV1", false);
        }
        if (m->output_failed) {
            av_packet_unref(m->packet);
            continue; /* Invalid output state: retire without extending the mux prefix. */
        }
        /* This plan disables reorder/lookahead. Refuse an unexpected temporal
         * contract rather than fabricating DTS or replaying compressed packets. */
        if (m->packet->pts == AV_NOPTS_VALUE || m->packet->dts != m->packet->pts) {
            av_packet_unref(m->packet);
            return failed(m, -ENOTSUP, "backend returned reordered/untimed video");
        }
        ret = write_header(m, m->packet);
        if (ret < 0) {
            av_packet_unref(m->packet);
            return failed(m, ret, "initialize video output");
        }
        if (m->replay) {
            m->packet->stream_index = 0;
            m->packet->duration = 0;
            TracyCZoneN(retention, "replay.retain_video", true);
            ret = gk_replay_add(m->replay, m->packet);
            TracyCZoneEnd(retention);
            av_packet_unref(m->packet);
            if (ret < 0)
                return failed(
                    m, ret,
                    ret == -ENOBUFS
                        ? "replay memory budget exhausted; increase --replay-memory-mib or shorten "
                          "retention"
                        : "retain replay video (AV1 random-access / source timing contract)");
            ++m->stats.packets;
            continue;
        }
        ret = write_video(m, m->packet->pts);
        if (ret < 0) {
            av_packet_unref(m->packet);
            return ret;
        }
        av_packet_move_ref(m->pending, m->packet);
        ++m->stats.packets;
    }
}

static int receive_audio(struct gk_media *m, struct audio_track *a)
{
    for (;;) {
        int ret = avcodec_receive_packet(a->codec, m->packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return 0;
        if (ret < 0)
            return failed(m, ret, "receive audio");
        if (m->output_failed) {
            av_packet_unref(m->packet);
            continue;
        }
        m->packet->stream_index = a->stream->index;
        if (m->replay) {
            TracyCZoneN(retention, "replay.retain_audio", true);
            ret = gk_replay_add(m->replay, m->packet);
            TracyCZoneEnd(retention);
            av_packet_unref(m->packet);
            if (ret < 0)
                return failed(m, ret,
                              ret == -ENOBUFS ? "replay memory budget exhausted; increase "
                                                "--replay-memory-mib or shorten retention"
                                              : "retain replay audio");
            continue;
        }
        av_packet_rescale_ts(m->packet, a->codec->time_base, a->stream->time_base);
        ret = av_interleaved_write_frame(m->mux, m->packet);
        if (ret < 0)
            return failed(m, ret, "mux audio");
    }
}

static int open_audio(struct gk_media *m, struct audio_track *a, const struct gk_audio_track *track)
{
    bool opus = m->options.audio_codec == GK_AUDIO_CODEC_OPUS ||
                (m->options.audio_codec == GK_AUDIO_CODEC_AUTO && m->options.matroska);
    if (opus && !m->options.matroska) {
        fputs("Exact-length Opus requires a .mkv output; use PCM for fragmented MP4.\n", stderr);
        return -ENOTSUP;
    }
    if (track->format.discrete && !m->options.matroska) {
        fputs("Discrete AUX channels require a .mkv output with this FFmpeg muxer.\n", stderr);
        return -ENOTSUP;
    }
    if (!opus && m->options.matroska && !track->format.discrete && track->format.channels > 2) {
        fputs("Positioned surround PCM requires .mp4; use Opus for surround in .mkv.\n", stderr);
        return -ENOTSUP;
    }
    /* Fragmented MP4's unknown final edit duration does not reliably trim Opus
     * tail padding in the independent decoder. PCM has neither priming nor tail.
     * Matroska explicitly represents Opus pre-skip and DiscardPadding. */
    const AVCodec *codec = avcodec_find_encoder_by_name(opus ? "libopus" : "pcm_f32le");
    if (!codec)
        return -ENOSYS;
    a->codec = avcodec_alloc_context3(codec);
    if (!a->codec)
        return -ENOMEM;
    a->codec->sample_fmt = AV_SAMPLE_FMT_FLT;
    a->codec->sample_rate = 48000;
    AVChannelLayout input = {}, output = {};
    int ret = gk_audio_layout(&track->format, &input, &output);
    if (ret >= 0)
        ret = av_channel_layout_copy(&a->codec->ch_layout, &output);
    if (ret >= 0)
        ret = swr_alloc_set_opts2(&a->resampler, &output, AV_SAMPLE_FMT_FLT, 48000, &input,
                                  AV_SAMPLE_FMT_FLT, (int)track->format.rate, 0, nullptr);
    av_channel_layout_uninit(&input);
    av_channel_layout_uninit(&output);
    if (ret < 0)
        return ret;
    a->rate = track->format.rate;
    a->channels = track->format.channels;
    /* movenc omits chnl for one-channel PCM, whose identity is implicit mono.
     * Do not silently erase a different single-speaker identity. */
    if (!m->options.matroska && a->channels == 1 && a->codec->ch_layout.u.mask != AV_CH_LAYOUT_MONO)
        return -ENOTSUP;
    a->codec->time_base = (AVRational){1, 48000};
    a->codec->bit_rate = (int64_t)a->channels * 64000;
    a->codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    AVDictionary *options = nullptr;
    if (opus) {
        av_dict_set(&options, "frame_duration", "20", 0);
        av_dict_set(&options, "application", "audio", 0);
        /* Use the surround API with its actual Vorbis mapping/reordering.
         * Unsupported layouts fail; they are never relabeled or downmixed. */
        av_dict_set(&options, "mapping_family",
                    track->format.discrete ? "255"
                    : a->channels > 2      ? "1"
                                           : "0",
                    0);
    }
    ret = avcodec_open2(a->codec, codec, &options);
    if (av_dict_count(options) && ret >= 0)
        ret = -ENOTSUP;
    av_dict_free(&options);
    if (ret < 0)
        return ret;
    a->quantum = a->codec->frame_size ? a->codec->frame_size : 960;
    a->end = INT64_MAX;
    a->last_position = -1;
    /* Worst block at native rate, plus 0.5% compensation and filter slack. */
    a->capacity = (int)av_rescale_rnd(GK_AUDIO_FRAMES, 48240, a->rate, AV_ROUND_UP) + 256;
    if (a->capacity < 8192)
        a->capacity = 8192;
    a->resampled = av_malloc_array((size_t)a->capacity * a->channels, sizeof(float));
    if (!a->resampled)
        return -ENOMEM;
    ret = av_opt_set_int(a->resampler, "flags", SWR_FLAG_RESAMPLE, 0);
    if (ret < 0 || (ret = swr_init(a->resampler)) < 0)
        return ret;
    a->fifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLT, (int)a->channels, 48000);
    a->frame = av_frame_alloc();
    if (!a->fifo || !a->frame)
        return -ENOMEM;
    a->frame->format = AV_SAMPLE_FMT_FLT;
    a->frame->sample_rate = 48000;
    ret = av_channel_layout_copy(&a->frame->ch_layout, &a->codec->ch_layout);
    if (ret < 0)
        return ret;
    a->frame->nb_samples = a->quantum;
    ret = av_frame_get_buffer(a->frame, 0);
    if (ret < 0)
        return ret;
    a->stream = avformat_new_stream(m->mux, nullptr);
    if (!a->stream)
        return -ENOMEM;
    a->stream->time_base = a->codec->time_base;
    ret = av_dict_set(&a->stream->metadata, m->options.matroska ? "title" : "handler_name",
                      track->title ? track->title : "Audio", 0);
    if (ret < 0)
        return ret;
    if (track->format.discrete) {
        /* Mapping family 255 preserves ordered independent channels, without
         * assigning speakers. Matroska tags retain the explicit AUX identities. */
        char labels[GK_AUDIO_CHANNELS * 7];
        size_t used = 0;
        for (unsigned i = 0; i < track->format.channels; ++i) {
            int n = snprintf(labels + used, sizeof(labels) - used, "%sAUX%u", i ? "," : "",
                             track->format.auxiliary[i]);
            if (n < 0 || (size_t)n >= sizeof(labels) - used)
                return -EOVERFLOW;
            used += (size_t)n;
        }
        ret = av_dict_set(&a->stream->metadata, "GPUKEY_CHANNELS", labels, 0);
        if (ret < 0)
            return ret;
    }
    return avcodec_parameters_from_context(a->stream->codecpar, a->codec);
}

int gk_media_open(struct gk_media **out, struct gk_gpu *gpu, const struct gk_video_format *source,
                  const struct gk_media_options *options)
{
    *out = nullptr;
    if (options->compact_gpu && gpu->backend != GK_VAAPI && gpu->backend != GK_VULKAN)
        return -ENOTSUP;
#ifndef AV_HAVE_BUFFER_POOL_TRIM
    if (options->compact_gpu)
        return -ENOTSUP;
#endif
    if (source->width <= 0 || source->height <= 0 || options->surfaces > INT32_MAX ||
        (options->bits != 8 && options->bits != 10) || !options->fps_num || !options->fps_den)
        return -EINVAL;
    bool pipeline = gpu->backend == GK_VAAPI || gpu->backend == GK_VULKAN;
    bool direct = gpu->backend == GK_VULKAN && source->dmabuf;
    bool compact_inputs = options->compact_gpu && pipeline && !direct;
    if (direct && (options->bits != 8 || options->surfaces))
        return -ENOTSUP;
    if (options->encode_depth && !pipeline)
        return -ENOTSUP;
    unsigned depth = pipeline ? (options->encode_depth ? options->encode_depth : 2) : 0;
    unsigned minimum = gpu->backend == GK_VAAPI ? depth : 2;
    if (gpu->backend == GK_VULKAN && ckd_add(&minimum, depth, 2U))
        return -ERANGE;
    unsigned surfaces = options->surfaces          ? options->surfaces
                        : gpu->backend == GK_VAAPI ? minimum
                        : minimum > 8              ? minimum
                                                   : 8;
    if (surfaces < minimum || surfaces > INT32_MAX)
        return -EINVAL;
    if (direct)
        surfaces = 0; /* Imported capture images are also encoder inputs. */
    struct gk_media *m = calloc(1, sizeof(*m));
    if (!m)
        return -ENOMEM;
    m->gpu = gpu;
    m->direct = direct;
    m->source = *source;
    m->options = *options;
    m->options.surfaces = surfaces;
    m->options.encode_depth = depth;
    m->stats.encoder_inputs = surfaces;
    m->stats.encode_depth = depth;
    m->last_pts = m->last_keyframe = -1;
    const char *name = gpu->backend == GK_VAAPI    ? "av1_vaapi"
                       : gpu->backend == GK_NVENC  ? "av1_nvenc"
                       : gpu->backend == GK_VULKAN ? "av1_vulkan"
                                                   : "libaom-av1";
    const AVCodec *codec = avcodec_find_encoder_by_name(name);
    int ret = -ENOSYS;
    if (!codec)
        goto fail;
    m->video = avcodec_alloc_context3(codec);
    if (!m->video) {
        ret = -ENOMEM;
        goto fail;
    }
    /* Let the installed encoder validate its supported depth before allocating
     * the corresponding input pool. Do not duplicate a private FFmpeg limit. */
    if (pipeline && (ret = av_opt_set_int(m->video->priv_data, "async_depth", depth, 0)) < 0)
        goto fail;
    if (options->compact_gpu) {
        const AVOption *trim = av_opt_find(m->video->priv_data, "async_trim", nullptr, 0, 0);
        if (!trim || trim->type != AV_OPT_TYPE_INT || trim->max < 2 ||
            !(trim->flags & AV_OPT_FLAG_RUNTIME_PARAM)) {
            ret = -ENOTSUP;
            goto fail;
        }
        if ((ret = av_opt_set_int(m->video->priv_data, "async_trim", 1, 0)) < 0)
            goto fail;
    }
    bool readout_supported =
        pipeline && av_opt_find(m->video->priv_data, "async_readout", nullptr, 0, 0);
    if (pipeline &&
        ((!readout_supported && gpu->backend == GK_VULKAN) ||
         options->packet_readout == GK_READOUT_BUFFERED) &&
        av_opt_find(m->video->priv_data, "initial_output", nullptr, 0, 0) &&
        (ret = av_opt_set_int(m->video->priv_data, "initial_output", 1, 0)) < 0)
        goto fail;
    if (options->packet_readout != GK_READOUT_BUFFERED) {
        if (readout_supported) {
            ret = av_opt_set_int(m->video->priv_data, "async_readout", 1, 0);
            if (ret < 0)
                goto fail;
            m->stats.available_readout = true;
            if (options->packet_readout != GK_READOUT_AVAILABLE &&
                av_opt_find(m->video->priv_data, "async_events", nullptr, 0, 0)) {
                ret = av_opt_set_int(m->video->priv_data, "async_events", 1, 0);
                if (ret < 0)
                    goto fail;
            }
        } else if (options->packet_readout == GK_READOUT_AVAILABLE ||
                   options->packet_readout == GK_READOUT_EVENTS) {
            ret = -ENOTSUP;
            goto fail;
        }
    }
    m->packet = av_packet_alloc();
    m->pending = av_packet_alloc();
    m->pool_capacity = compact_inputs ? 1 : surfaces;
    if (m->pool_capacity)
        m->pool = calloc(m->pool_capacity, sizeof(*m->pool));
    if (!m->video || !m->packet || !m->pending || (m->pool_capacity && !m->pool)) {
        ret = -ENOMEM;
        goto fail;
    }
    enum AVPixelFormat software = options->bits == 10 ? AV_PIX_FMT_P010LE : AV_PIX_FMT_NV12;
    if (direct)
        software = source->pixel_format;
    if (gpu->backend == GK_SOFTWARE)
        software = options->bits == 10 ? AV_PIX_FMT_YUV420P10LE : AV_PIX_FMT_YUV420P;
    enum AVPixelFormat hardware = gpu->backend == GK_VAAPI   ? AV_PIX_FMT_VAAPI
                                  : gpu->backend == GK_NVENC ? AV_PIX_FMT_CUDA
                                                             : AV_PIX_FMT_VULKAN;
    /* This is the explicit CPU-input NVENC control. Its audited upload path
     * copies synchronously into an encoder-owned locked input buffer. Generic
     * CUDA hwframe upload is asynchronous and does not retain our host staging. */
    bool hardware_frames = gpu->device && gpu->backend != GK_NVENC;
    m->video->width = source->width;
    m->video->height = source->height;
    m->video->pix_fmt = hardware_frames ? hardware : software;
    m->video->sw_pix_fmt = software;
    m->video->time_base = (AVRational){1, 1000000};
    m->video->framerate = (AVRational){(int)options->fps_num, (int)options->fps_den};
    m->video->sample_aspect_ratio = (AVRational){1, 1};
    m->video->color_primaries = AVCOL_PRI_BT709;
    m->video->color_trc = source->transfer;
    m->video->colorspace = AVCOL_SPC_BT709;
    m->video->color_range = AVCOL_RANGE_MPEG;
    if (direct) {
        switch (gk_vk_rgb_conversion(gpu)) {
        case GK_RGB_709_LIMITED:
            break;
        case GK_RGB_2020_FULL:
            m->video->colorspace = AVCOL_SPC_BT2020_NCL;
            m->video->color_range = AVCOL_RANGE_JPEG;
            break;
        case GK_RGB_NONE:
            ret = -ENOTSUP;
            goto fail;
        }
    }
    m->video->chroma_sample_location = AVCHROMA_LOC_LEFT;
    m->video->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    m->video->max_b_frames = 0;
    m->video->gop_size = (int)av_rescale_rnd(options->fps_num, 2, options->fps_den, AV_ROUND_UP);
    m->video->thread_count = 4;
    if (hardware_frames) {
        if (direct) {
            ret = gk_vk_rgb_frames(gpu, source, &m->frames);
            if (ret < 0)
                goto fail;
        } else
            m->frames = av_hwframe_ctx_alloc(gpu->device);
        if (!m->frames) {
            ret = -ENOMEM;
            goto fail;
        }
        if (!direct) {
            AVHWFramesContext *fc = (void *)m->frames->data;
            fc->format = hardware;
            fc->sw_format = software;
            fc->width = source->width;
            fc->height = source->height;
            /* We allocate and retain every input exactly once below. VAAPI's
             * fixed-pool preallocation first acquires/releases a second set of
             * frame wrappers; its LIFO reversal strands the already-backed probe
             * surface at the unused end of our array on lazy-allocation drivers.
             * A dynamic library pool lets our bounded owner pool reuse that first
             * surface immediately. The encoder owns a separate reconstruction pool. */
            fc->initial_pool_size = 0;
            ret = av_hwframe_ctx_init(m->frames);
            if (ret < 0)
                goto fail;
        }
        m->video->hw_frames_ctx = av_buffer_ref(m->frames);
        if (!m->video->hw_frames_ctx) {
            ret = -ENOMEM;
            goto fail;
        }
    } else if (gpu->backend == GK_NVENC) {
        m->video->hw_device_ctx = av_buffer_ref(gpu->device);
        if (!m->video->hw_device_ctx) {
            ret = -ENOMEM;
            goto fail;
        }
    }
    AVDictionary *encoder_options = nullptr;
    if (direct ||
        (gpu->backend == GK_VULKAN && av_opt_find(m->video->priv_data, "queue_dpb", nullptr, 0, 0)))
        av_dict_set(&encoder_options, "queue_dpb", "1", 0);
    if (gpu->backend == GK_VAAPI || gpu->backend == GK_VULKAN) {
        av_dict_set(&encoder_options, "rc_mode", "CQP", 0);
        if (gpu->backend == GK_VULKAN)
            av_dict_set(&encoder_options, "rc_mode", "cqp", 0);
        if (gpu->backend == GK_VAAPI) {
            m->video->global_quality = options->qp;
            if (!options->qp) {
                m->video->flags |= AV_CODEC_FLAG_QSCALE;
                m->video->global_quality = 1; /* positive lambda value rounds to native Q=0 */
            }
        } else
            av_dict_set_int(&encoder_options, "qp", options->qp, 0);
    } else if (gpu->backend == GK_NVENC) {
        av_dict_set(&encoder_options, "preset", "p4", 0);
        av_dict_set(&encoder_options, "tune", "ll", 0);
        av_dict_set(&encoder_options, "rc", "constqp", 0);
        av_dict_set_int(&encoder_options, "qp", options->qp, 0);
        av_dict_set(&encoder_options, "rc-lookahead", "0", 0);
        av_dict_set(&encoder_options, "delay", "0", 0);
        av_dict_set(&encoder_options, "forced-idr", "1", 0);
    } else {
        av_dict_set(&encoder_options, "usage", "realtime", 0);
        av_dict_set(&encoder_options, "cpu-used", "8", 0);
        av_dict_set(&encoder_options, "lag-in-frames", "0", 0);
        av_dict_set_int(&encoder_options, "crf", options->qp, 0);
    }
    ret = avcodec_open2(m->video, codec, &encoder_options);
    if (ret >= 0 && av_dict_count(encoder_options)) {
        const AVDictionaryEntry *unused = nullptr;
        while ((unused = av_dict_iterate(encoder_options, unused)))
            fprintf(stderr, "unconsumed encoder option: %s=%s\n", unused->key, unused->value);
        ret = -ENOTSUP;
    }
    av_dict_free(&encoder_options);
    if (ret < 0)
        goto fail;
    const AVOption *generation_option =
        av_opt_find(m->video->priv_data, "async_fd_generation", nullptr, 0, 0);
    if (generation_option || (options->compact_gpu && gpu->backend == GK_VAAPI)) {
        const AVOption *option = generation_option;
        if (!option || option->type != AV_OPT_TYPE_UINT64 ||
            !(option->flags & AV_OPT_FLAG_READONLY)) {
            ret = -EPROTO;
            goto fail;
        }
        m->completion_generation =
            (const void *)((const uint8_t *)m->video->priv_data + option->offset);
    }
    if (m->stats.available_readout) {
        int64_t events = 0;
        if (av_opt_get_int(m->video->priv_data, "async_events", 0, &events) >= 0 && events) {
            const AVOption *option =
                av_opt_find(m->video->priv_data, "async_output_fd", nullptr, 0, 0);
            if (!option || option->type != AV_OPT_TYPE_INT ||
                !(option->flags & AV_OPT_FLAG_READONLY)) {
                ret = -EPROTO;
                goto fail;
            }
            /* Public AVOption storage/type metadata, not a private codec layout.
             * One media owner reads this field between encoder calls. */
            m->completion_fd =
                (const void *)((const uint8_t *)m->video->priv_data + option->offset);
            m->stats.completion_events = true;
        }
    }
    if (options->packet_readout == GK_READOUT_EVENTS && !m->stats.completion_events) {
        ret = -ENOTSUP;
        goto fail;
    }
    ret = gk_gpu_configure(gpu, source->width, source->height);
    if (ret < 0)
        goto fail;
    for (unsigned i = 0; i < (compact_inputs ? 1U : surfaces); ++i) {
        AVFrame *frame;
        ret = allocate_input(m, &frame);
        if (ret < 0)
            goto fail;
    }
    if (!source->dmabuf) {
        if (m->frames && (ret = ensure_staging(m)) < 0)
            goto fail;
#ifdef GK_CPU_CONVERSION_CONTROL
        m->sws = sws_alloc_context();
        if (!m->sws) {
            ret = -ENOMEM;
            goto fail;
        }
        if (av_opt_set_int(m->sws, "srcw", source->width, 0) < 0 ||
            av_opt_set_int(m->sws, "srch", source->height, 0) < 0 ||
            av_opt_set_int(m->sws, "dstw", source->width, 0) < 0 ||
            av_opt_set_int(m->sws, "dsth", source->height, 0) < 0 ||
            av_opt_set_int(m->sws, "src_format", source->pixel_format, 0) < 0 ||
            av_opt_set_int(m->sws, "dst_format", software, 0) < 0 ||
            av_opt_set_int(m->sws, "sws_flags",
                           SWS_BILINEAR | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INP, 0) < 0 ||
            av_opt_set_int(m->sws, "dst_h_chr_pos", 0, 0) < 0 ||
            av_opt_set_int(m->sws, "dst_v_chr_pos", 128, 0) < 0 ||
            (ret = sws_init_context(m->sws, nullptr, nullptr)) < 0) {
            ret = -ENOTSUP;
            goto fail;
        }
        const int *coefficients = sws_getCoefficients(SWS_CS_ITU709);
        ret =
            sws_setColorspaceDetails(m->sws, coefficients, 1, coefficients, 0, 0, 1 << 16, 1 << 16);
        if (ret < 0)
            goto fail;
#else
        size_t bytes = gk_rgb_workspace_size((unsigned)source->width);
        m->conversion_rows = bytes ? av_malloc(bytes) : nullptr;
        /* The row cache is an optimization, not required storage. The direct
         * kernel preserves the same pixels when its allocation is unavailable. */
#endif
    }
    ret = avformat_alloc_output_context2(&m->mux, nullptr, options->matroska ? "matroska" : "mp4",
                                         nullptr);
    if (ret < 0)
        goto fail;
    m->video_stream = avformat_new_stream(m->mux, nullptr);
    if (!m->video_stream) {
        ret = -ENOMEM;
        goto fail;
    }
    m->video_stream->time_base = m->video->time_base;
    /* The capture cap configures the encoder; it is not a measured average.
     * Packet PTS/durations define this VFR track, including long static holds. */
    m->video_stream->avg_frame_rate = (AVRational){0, 1};
    m->video_stream->r_frame_rate = (AVRational){0, 1};
    /* Older muxers need DefaultDuration to emit explicit video intervals.
     * The pinned muxer accepts actual durations without a fictitious rate. */
    if (options->matroska && !av_opt_find(m->mux->priv_data, "write_video_duration", nullptr, 0, 0))
        m->video_stream->r_frame_rate = (AVRational){1000, 1};
    ret = avcodec_parameters_from_context(m->video_stream->codecpar, m->video);
    if (ret < 0)
        goto fail;
    if (options->audio_count) {
        if (!options->audio) {
            ret = -EINVAL;
            goto fail;
        }
        m->audio = calloc(options->audio_count, sizeof(*m->audio));
        if (!m->audio) {
            ret = -ENOMEM;
            goto fail;
        }
        for (unsigned i = 0; i < options->audio_count; ++i)
            if ((ret = open_audio(m, &m->audio[i], &options->audio[i])) < 0)
                goto fail;
    }
    if (options->replay.duration_ns) {
        struct gk_replay_options replay = options->replay;
        replay.matroska = options->matroska;
        replay.sink = options->sink;
        /* Vulkan's AVBufferRef can own a mapped FFVkBuffer, unlike the copied
         * CPU packets in the audited VAAPI/NVENC/libaom encoders. */
        replay.copy_video = gpu->backend == GK_VULKAN;
        ret = gk_replay_open(&m->replay, m->mux, &replay);
        if (ret < 0)
            goto fail;
    } else {
        ret = gk_sink_open(&m->sink, options->output, &options->sink);
        if (ret < 0)
            goto fail;
        uint8_t *io_buffer = av_malloc(65536);
        if (!io_buffer) {
            ret = -ENOMEM;
            goto fail;
        }
        m->io = avio_alloc_context(io_buffer, 65536, 1, m->sink, nullptr, gk_sink_write, nullptr);
        if (!m->io) {
            av_free(io_buffer);
            ret = -ENOMEM;
            goto fail;
        }
        m->mux->pb = m->io;
        m->mux->flags |= AVFMT_FLAG_CUSTOM_IO;
        m->mux->max_interleave_delta = 100000;
        m->mux->avoid_negative_ts = AVFMT_AVOID_NEG_TS_DISABLED;
    }
    /* The first actual picture determines coded geometry/configuration. Audio
     * remains in its bounded FIFO until that verified header is available. */
    m->opened = true;
    fprintf(stderr,
            "plan: %s; %dx%d %u-bit 4:2:0; %s transfer; %s %s; "
            "%s; %s\n",
            name, source->width, source->height, options->bits,
            av_color_transfer_name(source->transfer),
            m->video->colorspace == AVCOL_SPC_BT2020_NCL ? "BT.2020 matrix / BT.709 primaries"
                                                         : "BT.709",
            m->video->color_range == AVCOL_RANGE_JPEG ? "full" : "limited",
            direct           ? "persistent DMA-BUF + direct RGB encode"
            : source->dmabuf ? "persistent DMA-BUF + GPU VPP"
                             : "explicit CPU conversion/upload",
            options->matroska ? "Matroska 1 ms timing" : "fragmented MP4 1 us timing");
    for (unsigned i = 0; i < options->audio_count; ++i) {
        struct audio_track *a = &m->audio[i];
        char layout[256];
        if (options->audio[i].format.discrete)
            snprintf(layout, sizeof(layout), "%u discrete AUX channels", a->channels);
        else
            av_channel_layout_describe(&a->codec->ch_layout, layout, sizeof(layout));
        fprintf(stderr, "audio %u: %s; %s; %u → 48000 Hz; %s\n", i + 1,
                options->audio[i].title ? options->audio[i].title : "Audio", layout, a->rate,
                a->codec->codec->name);
    }
    *out = m;
    return 0;
fail:
    failed(m, ret, "initialize media plan");
    {
        int ignored = gk_media_close(&m, 0, nullptr);
        (void)ignored;
    }
    return ret;
}

static void release_idle_inputs(struct gk_media *m)
{
#ifdef AV_HAVE_BUFFER_POOL_TRIM
    if (!m->options.compact_gpu || (m->gpu->backend != GK_VAAPI && m->gpu->backend != GK_VULKAN) ||
        (!m->last && !m->repeat_key) || !m->pool_count || m->stats.packets != m->stats.submitted)
        return;
    TracyCZoneN(zone, "media.release-idle-inputs", true);
    /* Uploaded Vulkan preparation preserves m->last until submit, so active
     * capture retains one writable spare to avoid per-frame image churn.
     * Standby clears m->last first and releases that spare as well. */
    unsigned spare = m->gpu->backend == GK_VULKAN && m->last && !m->prepared ? 1U : 0U;
    bool released = false;
    for (unsigned i = 0; i < m->pool_count;) {
        AVFrame *frame = m->pool[i];
        bool writable = m->gpu->backend == GK_VAAPI ? av_buffer_get_ref_count(frame->buf[0]) == 1
                                                    : av_frame_is_writable(frame);
        if (frame != m->last && frame != m->prepared && writable) {
            if (spare) {
                --spare;
                ++i;
                continue;
            }
            /* Encoder completion releases its last input reference. Keep the
             * latest image for static repeats and any prepared, unsubmitted
             * image. Run this outside the PipeWire source-removal exclusion. */
            m->pool[i] = m->pool[--m->pool_count];
            m->pool[m->pool_count] = nullptr;
            av_frame_free(&frame);
            released = true;
        } else {
            ++i;
        }
    }
    if (released)
        av_buffer_pool_trim(((AVHWFramesContext *)m->frames->data)->pool, 0);
    TracyCZoneEnd(zone);
#else
    (void)m;
#endif
}

static AVPacket *copy_repeat_key(struct gk_media *m)
{
    AVPacket *key = av_packet_alloc();
    int obtained = key ? 0 : -ENOMEM;
    if (key && m->replay)
        obtained = gk_replay_key_ref(m->replay, key);
    else if (key && m->pending->buf && (m->pending->flags & AV_PKT_FLAG_KEY)) {
        if (m->gpu->backend == GK_VULKAN) {
            obtained = av_new_packet(key, m->pending->size);
            if (obtained >= 0) {
                memcpy(key->data, m->pending->data, (size_t)m->pending->size);
                obtained = av_packet_copy_props(key, m->pending);
                if (obtained >= 0)
                    obtained = 1;
            }
        } else {
            obtained = av_packet_ref(key, m->pending);
            if (obtained >= 0)
                obtained = 1;
        }
    }
    if (obtained != 1 || !gk_av1_reusable_key(m->video_stream->codecpar, key))
        av_packet_free(&key);
    return key;
}

static int release_static_resources(struct gk_media *m)
{
    if (!m->options.compact_gpu)
        return 0;
    if (m->options.independent_repeats &&
        (m->gpu->backend == GK_VAAPI || m->gpu->backend == GK_VULKAN)) {
        m->last = nullptr;
        av_buffer_unref(&m->imported_hold);
        release_idle_inputs(m);
        if (!m->prepared)
            av_frame_free(&m->staging);
    }
    /* The CPU key owns static presentation and all accepted inputs have been
     * collected. A receive can trim unused storage without issuing a picture. */
    TracyCZoneN(trim, "media.release-idle-output", true);
    int ret = av_opt_set_int(m->video->priv_data, "async_trim", 2, 0);
    if (ret >= 0)
        ret = receive_video(m);
    TracyCZoneEnd(trim);
    return ret;
}

static int cache_static_output(struct gk_media *m)
{
    if (!m->static_key_pending || m->stats.packets != m->stats.submitted || m->prepared)
        return 0;
    m->static_key_pending = false;
    AVPacket *key = copy_repeat_key(m);
    if (!key)
        return 0; /* Optional setup failure retains the raw fallback. */
    if (!m->replay && m->gpu->backend == GK_VULKAN) {
        /* Replace the pending mapped packet atomically with CPU-owned bytes;
         * its PTS/properties stay intact, and it no longer pins a GPU output. */
        int ret = av_packet_ref(m->packet, key);
        if (ret < 0) {
            av_packet_free(&key);
            return 0;
        }
        av_packet_unref(m->pending);
        av_packet_move_ref(m->pending, m->packet);
    }
    m->repeat_key = key;
    return release_static_resources(m);
}

int gk_media_prepare_begin(struct gk_media *m, const struct gk_image *image, VASurfaceID imported,
                           AVFrame **frame)
{
    *frame = nullptr;
    if (m->error)
        return m->error;
    m->prepare_started = m->options.measure ? gk_now_ns() : 0;
    /* Only allocated frame handles occupy this dense list. Spare configured
     * capacity cannot displace a reusable input or add empty-slot scans. */
    for (unsigned i = 0; !*frame && i < m->pool_count; ++i)
        /* VAAPI marks the bytes containing the surface ID read-only. GPU writes
         * to that surface are legal once the encoder releases its last input
         * reference after vaSyncBuffer/vaSyncSurface. av_frame_is_writable()
         * tests the handle bytes and rejects fresh, unrecycled VAAPI buffers.
         * A completed input can be updated in place at any configured depth.
         * Additional slots provide capacity without forcing their backing to
         * be allocated when encoding finishes before the next capture.
         * Preparation failures are terminal, so a partial image is not repeated. */
        if ((m->pool[i] != m->last || m->gpu->backend == GK_VAAPI) &&
            (m->gpu->backend == GK_VAAPI ? av_buffer_get_ref_count(m->pool[i]->buf[0]) == 1
                                         : av_frame_is_writable(m->pool[i]))) {
            *frame = m->pool[i];
            break;
        }
    if (!*frame && m->options.compact_gpu && m->pool_count < m->options.surfaces) {
        int ret = allocate_input(m, frame);
        if (ret < 0)
            return failed_input(m, ret, "restore encoder input capacity");
    }
    if (!*frame)
        return -EAGAIN;
    m->prepared = *frame;
    TracyCZoneN(zone, "media.prepare-submit", true);
    int ret;
    if (m->source.dmabuf) {
        ret = gk_gpu_convert_submit(m->gpu, &m->source, imported, *frame);
    } else {
        ret = m->frames ? ensure_staging(m) : 0;
        AVFrame *output = m->frames ? m->staging : *frame;
        if (ret >= 0) {
#ifdef GK_CPU_CONVERSION_CONTROL
            const uint8_t *data[4] = {image->data};
            int stride[4] = {image->stride};
            ret = sws_scale(m->sws, data, stride, 0, m->source.height, output->data,
                            output->linesize);
            if (ret == m->source.height)
                ret = m->frames ? av_hwframe_transfer_data(*frame, output, 0) : 0;
            else if (ret >= 0)
                ret = -EIO;
#else
            ret = m->conversion_rows
                      ? gk_rgb_to_yuv_rows(&m->source, image, output, m->conversion_rows,
                                           gk_rgb_workspace_size((unsigned)m->source.width))
                      : gk_rgb_to_yuv(&m->source, image, output);
            if (ret >= 0 && m->frames)
                ret = av_hwframe_transfer_data(*frame, output, 0);
#endif
        }
    }
    if (ret < 0)
        m->prepared = nullptr;
    TracyCZoneEnd(zone);
    return failed_input(m, ret, "prepare encoder input");
}

int gk_media_prepare_finish(struct gk_media *m, AVFrame *frame)
{
    int ret = m->source.dmabuf ? gk_gpu_convert_wait(m->gpu, frame) : 0;
    if (m->options.measure)
        gk_hist_add(&m->stats.preparation, (uint64_t)(gk_now_ns() - m->prepare_started));
    return failed_input(m, ret, "retire source reads");
}

int gk_media_prepare(struct gk_media *m, const struct gk_image *image, VASurfaceID imported,
                     AVFrame **frame)
{
    int ret = gk_media_prepare_begin(m, image, imported, frame);
    return ret < 0 ? ret : gk_media_prepare_finish(m, *frame);
}

static int submit_video(struct gk_media *m, AVFrame *frame, int64_t pts_ns, bool force_key)
{
    if (m->error)
        return m->error;
    if (pts_ns < 0)
        return failed_input(m, -ERANGE, "negative video timestamp");
    int64_t pts = av_rescale_q(pts_ns, (AVRational){1, 1000000000}, m->video->time_base);
    if (pts <= m->last_pts)
        return failed_input(m, -ERANGE, "nonmonotonic video timestamp");
    frame->pts = pts;
    frame->duration = 0; /* Actual displayed duration is known at the next packet/stop. */
    bool key = force_key || m->last_keyframe < 0 || pts - m->last_keyframe >= 2000000;
    frame->pict_type = key ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    int64_t start = m->options.measure ? gk_now_ns() : 0;
    TracyCZoneN(zone, "media.encode-and-packets", true);
    int ret;
    if (!m->source.dmabuf && m->stats.available_readout && m->stats.packets < m->stats.submitted) {
        ret = receive_video(m);
        if (ret < 0)
            goto finished;
        release_idle_inputs(m);
    }
    /* With every older input collected, this input's timestamp is exactly the
     * successor of the pending packet. Finalize it before the encoder asks for
     * another large mapped output buffer. If work is still in flight, retain
     * the ordinary receive-order path; its next timestamp is not known here. */
    if (m->gpu->backend == GK_VULKAN && !m->replay && m->pending->buf &&
        m->stats.packets == m->stats.submitted) {
        ret = write_video(m, pts);
        if (ret < 0)
            goto finished;
    }
    if (m->repeat_key && m->options.compact_gpu &&
        (ret = av_opt_set_int(m->video->priv_data, "async_trim", 1, 0)) < 0)
        goto finished;
    ret = send_video(m, frame);
    if (ret == AVERROR(EAGAIN)) {
        ret = receive_video(m);
        if (ret >= 0)
            ret = send_video(m, frame);
    }
    if (ret >= 0) {
        if (m->repeat_key)
            av_packet_free(&m->repeat_key);
        m->last = frame;
        m->static_key_pending = force_key && m->options.compact_gpu &&
                                m->options.independent_repeats && !m->options.encode_repeats;
        if (m->prepared == frame)
            m->prepared = nullptr;
        m->last_pts = pts;
        if (key)
            m->last_keyframe = pts;
        ++m->stats.submitted;
        ret = receive_video(m);
        if (ret >= 0) {
            release_idle_inputs(m);
            ret = cache_static_output(m);
        }
    }
finished:
    if (m->options.measure)
        gk_hist_add(&m->stats.submission, (uint64_t)(gk_now_ns() - start));
    TracyCZoneEnd(zone);
    TracyCFrameMark;
    return report_failure(m, ret, "submit AV1", !m->video_failed);
}

int gk_media_submit(struct gk_media *m, AVFrame *frame, int64_t pts_ns)
{
    return submit_video(m, frame, pts_ns, false);
}

int gk_media_repeat(struct gk_media *m, int64_t pts_ns)
{
    if (m->error)
        return m->error;
    int64_t pts = av_rescale_q(pts_ns, (AVRational){1, 1000000000}, m->video->time_base);
    if (pts_ns < 0 || pts <= m->last_pts)
        return failed_input(m, -ERANGE, "nonmonotonic repeat timestamp");
    /* Preserve P-picture refinement unless the caller declares independent-only
     * refreshes. Otherwise replace only a requested independent refresh.
     * No input has intervened since the cached key, so encoder and decoder
     * reference/entropy state remain equal after every repeated key. */
    if (!m->options.encode_repeats && m->stats.packets == m->stats.submitted &&
        (!m->prepared || m->repeat_key) &&
        (m->options.independent_repeats || pts - m->last_keyframe >= 2000000) &&
        (m->repeat_key || (m->replay ? gk_replay_key_ref(m->replay, nullptr) == 1
                                     : m->pending->buf && (m->pending->flags & AV_PKT_FLAG_KEY)))) {
        bool created_key = false;
        if (!m->repeat_key)
            created_key = (m->repeat_key = copy_repeat_key(m)) != nullptr;
        int referenced = m->repeat_key ? av_packet_ref(m->packet, m->repeat_key) : 0;
        if (m->repeat_key && referenced < 0 && !m->last)
            return failed_input(m, referenced, "retain independent repeat after raw release");
        if (m->repeat_key && referenced >= 0) {
            TracyCZoneN(reuse, "media.reuse-independent-key", true);
            m->packet->pts = m->packet->dts = pts;
            m->packet->duration = 0;
            int ret;
            if (m->replay) {
                ret = gk_replay_add_cpu(m->replay, m->packet);
                av_packet_unref(m->packet);
            } else {
                ret = write_video(m, pts);
                if (ret >= 0)
                    av_packet_move_ref(m->pending, m->packet);
                else
                    av_packet_unref(m->packet);
            }
            if (ret >= 0) {
                m->last_pts = m->last_keyframe = pts;
                ++m->stats.reused_packets;
                if (created_key)
                    ret = release_static_resources(m);
            }
            TracyCZoneEnd(reuse);
            return report_failure(m, ret, "repeat independent AV1 key", !m->video_failed);
        }
    }
    return m->last ? submit_video(m, m->last, pts_ns, m->options.independent_repeats) : 0;
}

int gk_media_submit_imported(struct gk_media *m, AVFrame *frame, int64_t pts_ns)
{
    if (!m->direct)
        return -ENOTSUP;
    AVBufferRef *hold = av_buffer_ref(frame->buf[0]);
    if (!hold)
        return failed_input(m, -ENOMEM, "retain direct input");
    color(frame, m->video);
    int ret = gk_media_submit(m, frame, pts_ns);
    if (m->last == frame) {
        /* The cache's AVFrame and frames context remain live while this
         * image pin prevents capture from returning/replacing the allocation. */
        av_buffer_unref(&m->imported_hold);
        m->imported_hold = hold;
    } else
        av_buffer_unref(&hold);
    return ret;
}

int gk_media_completion_fd(const struct gk_media *m)
{
    return m && !m->error && m->stats.packets < m->stats.submitted && m->stats.completion_events &&
                   m->completion_fd
               ? *m->completion_fd
               : -1;
}

uint64_t gk_media_completion_generation(const struct gk_media *m)
{
    return m && m->completion_generation ? *m->completion_generation : 0;
}

int gk_media_error(const struct gk_media *m)
{
    return m ? m->error : 0;
}

int gk_media_collect(struct gk_media *m, bool notified)
{
    if (!m)
        return 0;
    if (m->error)
        return m->error;
    int before_fd = gk_media_completion_fd(m);
    uint64_t before_packets = m->stats.packets;
    if (notified)
        ++m->stats.completion_wakeups;
    int ret = 0;
    if (m->stats.packets < m->stats.submitted) {
        TracyCZoneN(collection, "media.collect-completed-output", true);
        ret = receive_video(m);
        TracyCZoneEnd(collection);
    }
    if (ret < 0)
        return ret;
    release_idle_inputs(m);
    ret = cache_static_output(m);
    if (ret < 0)
        return report_failure(m, ret, "cache static AV1 output", !m->video_failed);
    if (notified && before_fd >= 0 && before_fd == gk_media_completion_fd(m) &&
        before_packets == m->stats.packets) {
        /* An early/unreliable notification must never turn the idle wait into
         * a busy loop. Keep demand readout, or fail an explicit event request. */
        ++m->stats.completion_fallbacks;
        m->stats.completion_events = false;
        if (m->options.packet_readout == GK_READOUT_EVENTS)
            return failed(m, -EPROTO, "completion fence did not cover encoded output");
        fputs("encode: completion notification unavailable; using demand readout\n", stderr);
    }
    return 0;
}

int gk_media_save(struct gk_media *m, const char *path, int64_t endpoint_ns)
{
    if (!m || !m->replay)
        return -EAGAIN;
    if (m->error)
        return m->error;
    /* A trigger is a consumer of completed output, even if no new source frame
     * arrived. Do not send an extra frame or end-of-stream merely to fill the
     * encoder's asynchronous input queue. */
    int ready = gk_media_collect(m, false);
    if (ready < 0)
        return ready;
    if (!m->header)
        return -EAGAIN;
    TracyCZoneN(snapshot, "replay.snapshot", true);
    int ret = gk_replay_save(m->replay, path, endpoint_ns);
    TracyCZoneEnd(snapshot);
    return ret;
}

bool gk_media_saved(struct gk_media *m, struct gk_replay_result *result)
{
    return m && m->replay && gk_replay_poll(m->replay, result);
}

int gk_media_replay_fd(const struct gk_media *m)
{
    return m && m->replay ? gk_replay_fd(m->replay) : -1;
}

static int encode_audio(struct gk_media *m, struct audio_track *a, bool final)
{
    if (!m->header)
        return 0;
    int count;
    while ((count = av_audio_fifo_size(a->fifo)) >= a->quantum * 2 || (final && count)) {
        if (a->encoded >= a->end)
            break;
        int ret = av_frame_make_writable(a->frame);
        if (ret < 0)
            return ret;
        int n = count < a->quantum ? count : a->quantum;
        if (n > a->end - a->encoded)
            n = (int)(a->end - a->encoded);
        a->frame->nb_samples = n;
        a->frame->pts = a->encoded;
        if (av_audio_fifo_read(a->fifo, (void **)a->frame->data, n) != n)
            return -EIO;
        a->encoded += n;
        ret = avcodec_send_frame(a->codec, a->frame);
        if (ret < 0)
            return ret;
        ret = receive_audio(m, a);
        if (ret < 0)
            return ret;
    }
    return 0;
}

static int audio_append(struct gk_media *m, struct audio_track *a, float *samples, int frames)
{
    if (frames > av_audio_fifo_space(a->fifo))
        return -ENOBUFS;
    void *data[] = {samples};
    if (av_audio_fifo_write(a->fifo, data, frames) != frames)
        return -EIO;
    a->written += frames;
    return encode_audio(m, a, false);
}

static int audio_silence(struct gk_media *m, struct audio_track *a, int64_t frames)
{
    /* Reuse the owner's conversion scratch; no channel-scaled stack object. */
    float *silence = a->resampled;
    memset(silence, 0, (size_t)960 * a->channels * sizeof(float));
    while (frames > 0) {
        int n = frames > 960 ? 960 : (int)frames;
        int ret = audio_append(m, a, silence, n);
        if (ret < 0)
            return ret;
        m->stats.audio_silence += (unsigned)n;
        frames -= n;
    }
    return 0;
}

int gk_media_audio(struct gk_media *m, unsigned track, const float *samples, unsigned frames,
                   int64_t pts_ns)
{
    if (track >= m->options.audio_count)
        return -EINVAL;
    struct audio_track *a = &m->audio[track];
    /* Capture shutdown may still deliver already-admitted PCM after an
     * unsubmitted video input failed. Codec/mux failures forbid further output. */
    if (!a->codec || m->output_failed || frames > GK_AUDIO_FRAMES || (frames && !samples))
        return m->error ? m->error : -EINVAL;
    if (!frames)
        return 0;
    int64_t position =
        av_rescale_q(pts_ns, (AVRational){1, 1000000000}, (AVRational){1, (int)a->rate});
    /* Clip only the recording's leading boundary. Gradual clock drift is
     * corrected by a resampler, not periodic sample/frame deletion. */
    if (position < 0) {
        unsigned trim = (uint64_t)-position < frames ? (unsigned)-position : frames;
        samples += (size_t)trim * a->channels;
        frames -= trim;
        m->stats.audio_trimmed += trim;
        position += trim;
    }
    if (!frames)
        return 0;
    if (position <= a->last_position)
        return failed(m, -ERANGE, "nonmonotonic audio source timestamp");
    a->last_position = position;
    if (!a->started) {
        if (position > a->rate)
            return failed(m, -ERANGE, "audio starts more than one second after video");
        int ret = audio_silence(m, a, av_rescale(position, 48000, a->rate));
        if (ret < 0)
            return failed(m, ret, "initial audio silence");
        a->started = true;
        a->next_input = a->observation = position;
    }
    int64_t gap = position - a->next_input;
    if (gap > a->rate / 10 || gap < -(int64_t)(a->rate / 10))
        return failed(m, -ERANGE, "audio clock/source discontinuity exceeds 100 ms");
    if (position - a->observation >= a->rate) {
        int64_t phase =
            av_rescale(position, 48000, a->rate) - a->written - swr_get_delay(a->resampler, 48000);
        if (phase > 4800 || phase < -4800)
            return failed(m, -ERANGE, "audio drift exceeds correction budget");
        /* One observation per second; ignore sub-millisecond timestamp jitter.
         * At most 0.5% rate correction over the next second. */
        int delta = phase > 240 ? 240 : phase < -240 ? -240 : (int)phase;
        if (delta > -48 && delta < 48)
            delta = 0;
        int ret = swr_set_compensation(a->resampler, delta, 48000);
        if (ret < 0)
            return failed(m, ret, "audio clock correction");
        a->observation = position;
    }
    a->next_input = position + frames;
    const uint8_t *input[] = {(const uint8_t *)samples};
    uint8_t *output[] = {(uint8_t *)a->resampled};
    TracyCZoneN(zone, "media.audio-resample-encode", true);
    int produced = swr_convert(a->resampler, output, a->capacity, input, (int)frames);
    int ret = produced;
    if (produced >= 0) {
        m->stats.audio_samples += frames;
        ret = audio_append(m, a, a->resampled, produced);
    }
    TracyCZoneEnd(zone);
    return failed(m, ret, "resample/encode audio");
}

int gk_media_close(struct gk_media **media, int64_t endpoint_ns, struct gk_media_stats *stats)
{
    struct gk_media *m = *media;
    if (!m)
        return 0;
    if (m->opened && !m->finished) {
        /* Nothing is pending when no input was accepted. FFmpeg 9.0.2's
         * hw_base_encode_send_frame(NULL) dereferences pic_end in this state.
         * Free the empty codec normally; never manufacture a frame to drain it. */
        if (!m->stats.submitted) {
            failed(m, -ENODATA, "no video frames submitted");
            goto cleanup;
        }
        /* Capture/input failures still permit draining accepted work. A fatal
         * codec error can leave private reference state unsuitable for another
         * call; preserve the collected prefix and close that codec directly. */
        int ret = 0;
        if (!m->video_failed) {
            ret = send_video(m, nullptr);
            if (ret == AVERROR(EAGAIN)) {
                ret = receive_video(m);
                if (ret >= 0)
                    ret = send_video(m, nullptr);
            }
            if (ret >= 0)
                ret = receive_video(m);
            report_failure(m, ret, "drain video", !m->video_failed);
        }
        if (!m->header) {
            failed(m, -ENODATA, "no decodable video configuration produced");
            goto cleanup;
        }
        int64_t end = av_rescale_q(endpoint_ns, (AVRational){1, 1000000000}, m->video->time_base);
        /* A final update can quantize to the same encoder/container tick as stop. The
         * previous packet already ends at that tick. Retire this zero-duration
         * terminal packet: no later video packet can depend on it after drain.
         * A one-frame recording shorter than a tick still cannot be represented. */
        if (!m->output_failed && m->pending->buf &&
            (m->stats.packets > 1 || m->stats.reused_packets) && end >= m->pending->pts &&
            av_rescale_q(end, m->video->time_base, m->video_stream->time_base) ==
                av_rescale_q(m->pending->pts, m->video->time_base, m->video_stream->time_base)) {
            av_packet_unref(m->pending);
            ++m->stats.video_boundary_omitted;
        } else
            failed(m, write_video(m, end), "last video duration");
        for (unsigned i = 0; i < m->options.audio_count; ++i) {
            struct audio_track *a = &m->audio[i];
            if (!a->started)
                failed(m, -ENODATA, "selected audio source supplied no samples");
            a->end = av_rescale(endpoint_ns, 48000, GK_NS);
            uint8_t *output[] = {(uint8_t *)a->resampled};
            int produced;
            while ((produced = swr_convert(a->resampler, output, a->capacity, nullptr, 0)) > 0) {
                int append = audio_append(m, a, a->resampled, produced);
                if (append < 0) {
                    failed(m, append, "drain resampler output");
                    break;
                }
            }
            failed(m, produced < 0 ? produced : 0, "drain resampler");
            int64_t missing = a->end - a->written;
            if (missing > 48000 || a->end < a->encoded)
                failed(m, -ERANGE, "audio endpoint outside buffered finalization window");
            else if (missing > 0)
                failed(m, audio_silence(m, a, missing), "extend audio to endpoint");
            failed(m, encode_audio(m, a, true), "last audio samples");
            ret = avcodec_send_frame(a->codec, nullptr);
            if (ret >= 0)
                ret = receive_audio(m, a);
            failed(m, ret, "drain audio");
        }
        if (!m->replay) {
            failed(m, av_interleaved_write_frame(m->mux, nullptr), "flush interleaving");
            failed(m, av_write_trailer(m->mux), "finalize container");
            avio_flush(m->io);
            failed(m, m->io->error, "flush output");
        }
        m->finished = true;
    }
cleanup:
    gk_replay_close(&m->replay, &m->stats.replay);
    int ret = gk_sink_close(&m->sink, &m->stats.sink);
    failed(m, ret, "complete sink");
    ret = m->error;
    if (stats)
        *stats = m->stats;
    /* avcodec owns input/reference leases. Destroy it before our native pools. */
    avcodec_free_context(&m->video);
    av_buffer_unref(&m->imported_hold);
    if (m->audio)
        for (unsigned i = 0; i < m->options.audio_count; ++i) {
            struct audio_track *a = &m->audio[i];
            avcodec_free_context(&a->codec);
            av_frame_free(&a->frame);
            av_audio_fifo_free(a->fifo);
            swr_free(&a->resampler);
            av_free(a->resampled);
        }
    free(m->audio);
    if (m->pool)
        for (unsigned i = 0; i < m->pool_count; ++i)
            av_frame_free(&m->pool[i]);
    free_sized(m->pool, (size_t)m->pool_capacity * sizeof(*m->pool));
    av_frame_free(&m->staging);
#ifdef GK_CPU_CONVERSION_CONTROL
    sws_freeContext(m->sws);
#else
    av_free(m->conversion_rows);
#endif
    av_buffer_unref(&m->frames);
    av_packet_free(&m->packet);
    av_packet_free(&m->pending);
    av_packet_free(&m->repeat_key);
    avformat_free_context(m->mux);
    if (m->io)
        av_freep(&m->io->buffer);
    avio_context_free(&m->io);
    free_sized(m, sizeof(*m));
    *media = nullptr;
    return ret;
}

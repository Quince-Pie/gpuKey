/* Consumer probe: export one real independent AV1 picture with a long duration. */
#include "replay.h"

#include <errno.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <stdio.h>
#include <string.h>

static int terminal_probe(const AVFormatContext *format, const AVPacket *key,
                          const AVPacket *second, const char *path, bool matroska)
{
    AVFormatContext *mux = nullptr;
    AVIOContext *io = nullptr;
    AVPacket *packet = av_packet_alloc();
    struct gk_sink *sink = nullptr;
    AVDictionary *options = nullptr;
    struct gk_sink_options storage = {.slabs = 8, .slab_size = 4096, .stall_ms = 5000};
    int ret = avformat_alloc_output_context2(&mux, nullptr, matroska ? "matroska" : "mp4", nullptr);
    if (!packet)
        ret = AVERROR(ENOMEM);
    if (ret < 0)
        goto done;
    AVStream *stream = avformat_new_stream(mux, nullptr);
    if (!stream) {
        ret = AVERROR(ENOMEM);
        goto done;
    }
    stream->time_base = format->streams[0]->time_base;
    if ((ret = avcodec_parameters_copy(stream->codecpar, format->streams[0]->codecpar)) < 0 ||
        (ret = gk_sink_open(&sink, path, &storage)) < 0)
        goto done;
    uint8_t *buffer = av_malloc(65536);
    if (!buffer) {
        ret = AVERROR(ENOMEM);
        goto done;
    }
    io = avio_alloc_context(buffer, 65536, 1, sink, nullptr, gk_sink_write, nullptr);
    if (!io) {
        av_free(buffer);
        ret = AVERROR(ENOMEM);
        goto done;
    }
    mux->pb = io;
    mux->flags |= AVFMT_FLAG_CUSTOM_IO;
    mux->avoid_negative_ts = AVFMT_AVOID_NEG_TS_DISABLED;
    mux->duration = 30000000;
    if (matroska) {
        av_dict_set(&options, "cluster_time_limit", "1000", 0);
        if (av_opt_find(mux->priv_data, "write_video_duration", nullptr, 0, 0))
            av_dict_set(&options, "write_video_duration", "1", 0);
    } else {
        av_dict_set(&options, "movflags", "delay_moov+default_base_moof+frag_keyframe", 0);
        av_dict_set(&options, "video_track_timescale", "1000000", 0);
        av_dict_set(&options, "frag_duration", "1000000", 0);
    }
    if ((ret = avformat_write_header(mux, &options)) < 0 || av_dict_count(options))
        goto done;
    unsigned count = second ? 3 : 2;
    for (unsigned i = 0; i < count; ++i) {
        if ((ret = av_packet_ref(packet, i && second ? second : key)) < 0)
            goto done;
        packet->pts = packet->dts = i == 0 ? 0 : i + 1 == count ? 29999000 : 15000000;
        packet->duration = i + 1 == count ? 1000 : second ? (i ? 14999000 : 15000000) : 29999000;
        av_packet_rescale_ts(packet, format->streams[0]->time_base, stream->time_base);
        ret = av_write_frame(mux, packet);
        av_packet_unref(packet);
        if (ret < 0)
            goto done;
    }
    ret = av_write_trailer(mux);
    avio_flush(io);
    if (ret >= 0 && io->error < 0)
        ret = io->error;
done:
    av_dict_free(&options);
    av_packet_free(&packet);
    avformat_free_context(mux);
    if (io)
        av_freep(&io->buffer);
    avio_context_free(&io);
    int closed = gk_sink_close(&sink, nullptr);
    return ret < 0 ? ret : closed;
}

int main(int argc, char **argv)
{
    if (argc != 3 && argc != 4)
        return 2;
    bool periodic = argc == 4 && !strcmp(argv[3], "--periodic");
    bool invalid = argc == 4 && !strcmp(argv[3], "--invalid-show-existing");
    bool terminal = argc == 4 && !strcmp(argv[3], "--terminal");
    bool transition = argc == 4 && !strcmp(argv[3], "--transition");
    bool long_span = argc == 4 && !strcmp(argv[3], "--long");
    if (argc == 4 && !periodic && !invalid && !terminal && !transition && !long_span)
        return 2;
    periodic |= long_span;
    int64_t seconds = long_span ? 6000 : 30;
    AVFormatContext *input = nullptr, *format = avformat_alloc_context();
    AVPacket *packet = av_packet_alloc(), *key = nullptr;
    struct gk_replay *replay = nullptr;
    int ret = AVERROR(ENOMEM);
    if (!format || !packet)
        goto done;
    if ((ret = avformat_open_input(&input, argv[1], nullptr, nullptr)) < 0 ||
        (ret = avformat_find_stream_info(input, nullptr)) < 0)
        goto done;
    if (input->nb_streams != 1 || input->streams[0]->codecpar->codec_id != AV_CODEC_ID_AV1) {
        ret = AVERROR(EINVAL);
        goto done;
    }
    AVStream *stream = avformat_new_stream(format, nullptr);
    if (!stream) {
        ret = AVERROR(ENOMEM);
        goto done;
    }
    stream->time_base = (AVRational){1, 1000000};
    stream->avg_frame_rate = stream->r_frame_rate = (AVRational){0, 1};
    if ((ret = avcodec_parameters_copy(stream->codecpar, input->streams[0]->codecpar)) < 0 ||
        (ret = av_read_frame(input, packet)) < 0)
        goto done;
    if (!(packet->flags & AV_PKT_FLAG_KEY)) {
        ret = AVERROR(EINVAL);
        goto done;
    }
    packet->pts = packet->dts = packet->duration = 0;
    packet->stream_index = 0;
    key = av_packet_clone(packet);
    if (!key) {
        ret = AVERROR(ENOMEM);
        goto done;
    }
    if (terminal || transition) {
        if (transition) {
            do {
                av_packet_unref(packet);
                ret = av_read_frame(input, packet);
                if (ret < 0)
                    goto done;
            } while (!(packet->flags & AV_PKT_FLAG_KEY));
        }
        ret = terminal_probe(format, key, transition ? packet : nullptr, argv[2],
                             strstr(argv[2], ".mkv") != nullptr);
        goto done;
    }
    struct gk_replay_options options = {.duration_ns = seconds * GK_NS,
                                        .bytes = (long_span ? 64U : 8U) * 1024 * 1024,
                                        .matroska = strstr(argv[2], ".mkv") != nullptr,
                                        .sink = {.slabs = 8, .slab_size = 4096, .stall_ms = 5000}};
    if ((ret = gk_replay_open(&replay, format, &options)) < 0 ||
        (ret = gk_replay_add(replay, packet)) < 0)
        goto done;
    for (unsigned i = 1; periodic && i < (unsigned)seconds / 2; ++i) {
        if ((ret = av_packet_ref(packet, key)) < 0)
            goto done;
        packet->pts = packet->dts = (int64_t)i * 2000000;
        if ((ret = gk_replay_add(replay, packet)) < 0)
            goto done;
    }
    if (invalid) {
        ret = av_new_packet(packet, 3);
        if (ret < 0)
            goto done;
        memcpy(packet->data, (const uint8_t[]){0x1a, 0x01, 0x88}, 3);
        packet->pts = packet->dts = 1000000;
        packet->stream_index = 0;
        if ((ret = gk_replay_add(replay, packet)) < 0)
            goto done;
    }
    if ((ret = gk_replay_save(replay, argv[2], seconds * GK_NS)) < 0)
        goto done;
    struct gk_replay_result result;
    if (gk_wait(gk_replay_fd(replay), 10000) != 1 || !gk_replay_poll(replay, &result)) {
        ret = AVERROR(ETIMEDOUT);
        goto done;
    }
    ret = result.error;
    if (!ret && (result.start_ns != 0 || result.end_ns != seconds * GK_NS))
        ret = AVERROR_INVALIDDATA;
done:
    gk_replay_close(&replay, nullptr);
    av_packet_free(&key);
    av_packet_free(&packet);
    avformat_free_context(format);
    avformat_close_input(&input);
    char message[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(ret, message, sizeof(message));
    fprintf(stderr, "Independent hold export: %s\n", message);
    return ret < 0;
}

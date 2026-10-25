/* Isolate retention, export and teardown with identical real encoded packets.
 * Input loading/normalization simulates the audited CPU encoder allocation and
 * is outside the timer. This is not an end-to-end capture throughput benchmark. */
#include "replay.h"

#include <inttypes.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/mem.h>
#include <stdckdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

int main(int argc, char **argv)
{
    CHECK(argc == 4 &&
          (!strcmp(argv[2], "auto") || !strcmp(argv[2], "copy") || !strcmp(argv[2], "reference")));
    av_log_set_level(AV_LOG_ERROR);
    AVFormatContext *input = nullptr, *template = avformat_alloc_context();
    CHECK(template && avformat_open_input(&input, argv[1], nullptr, nullptr) == 0);
    CHECK(avformat_find_stream_info(input, nullptr) == 0);
    int64_t *opus_clock = av_calloc(input->nb_streams, sizeof(*opus_clock));
    CHECK(opus_clock);
    for (unsigned i = 0; i < input->nb_streams; ++i) {
        const AVStream *in = input->streams[i];
        AVStream *out = avformat_new_stream(template, nullptr);
        CHECK(out && avcodec_parameters_copy(out->codecpar, in->codecpar) == 0);
        out->codecpar->codec_tag = 0; /* Input MP4 tags are not Matroska codec IDs. */
        out->time_base = i ? (AVRational){1, 48000} : (AVRational){1, 1000000};
        out->avg_frame_rate = out->r_frame_rate = (AVRational){1000, 1};
        CHECK(av_dict_copy(&out->metadata, in->metadata, 0) == 0);
        if (out->codecpar->codec_id == AV_CODEC_ID_OPUS) {
            CHECK(out->codecpar->sample_rate == 48000 && out->codecpar->extradata_size >= 19 &&
                  !memcmp(out->codecpar->extradata, "OpusHead", 8));
            opus_clock[i] = -(int64_t)AV_RL16(out->codecpar->extradata + 10);
        }
    }
    AVPacket *packet = av_packet_alloc();
    AVPacket **packets = nullptr;
    size_t count = 0, capacity = 0;
    while (av_read_frame(input, packet) >= 0) {
        if (count == capacity) {
            CHECK(capacity < SIZE_MAX / 2 / sizeof(*packets));
            capacity = capacity ? capacity * 2 : 1024;
            AVPacket **grown = av_realloc_array(packets, capacity, sizeof(*packets));
            CHECK(grown);
            packets = grown;
        }
        AVPacket *owned = av_packet_alloc();
        CHECK(owned && av_new_packet(owned, packet->size) == 0);
        memcpy(owned->data, packet->data, (size_t)packet->size);
        CHECK(av_packet_copy_props(owned, packet) == 0);
        av_packet_rescale_ts(owned, input->streams[owned->stream_index]->time_base,
                             template->streams[owned->stream_index]->time_base);
        if (template->streams[owned->stream_index]->codecpar->codec_id == AV_CODEC_ID_OPUS) {
            /* The script produces continuous 20 ms Opus packets. Recover that
             * exact sample clock instead of treating Matroska's rounded packet
             * duration as the codec's valid sample count. Check the file clock
             * and preserve the actual encoder's final discard padding. */
            int64_t *pts = &opus_clock[owned->stream_index], difference;
            int64_t serialized = av_rescale_q(*pts, (AVRational){1, 48000},
                                              input->streams[owned->stream_index]->time_base);
            CHECK(packet->pts != AV_NOPTS_VALUE && !ckd_sub(&difference, serialized, packet->pts) &&
                  difference >= -1 && difference <= 1);
            size_t side_size = 0;
            const uint8_t *side =
                av_packet_get_side_data(packet, AV_PKT_DATA_SKIP_SAMPLES, &side_size);
            CHECK(!side || side_size >= 10);
            uint32_t padding = side ? AV_RL32(side + 4) : 0;
            CHECK(padding <= 960);
            owned->pts = owned->dts = *pts;
            owned->duration = 960 - padding;
            CHECK(!ckd_add(pts, *pts, 960));
        }
        packets[count++] = owned;
        av_packet_unref(packet);
    }
    av_packet_free(&packet);
    avformat_close_input(&input);
    av_free(opus_clock);
    struct gk_replay_options options = {
        .duration_ns = 5 * GK_NS,
        .bytes = 64U * 1048576,
        .matroska = true,
        .copy_packets = !strcmp(argv[2], "copy"),
        .unique_keys = strcmp(argv[2], "auto") != 0,
        .sink = {.slabs = 16, .slab_size = 65536, .stall_ms = 5000}};
    struct rusage before, after;
    CHECK(getrusage(RUSAGE_SELF, &before) == 0);
    int64_t begin = gk_now_ns(), snapshot_ns = 0;
    struct gk_replay *replay;
    CHECK(gk_replay_open(&replay, template, &options) == 0);
    struct gk_histogram admission = {};
    bool saved = false;
    for (size_t i = 0; i < count; ++i) {
        int stream = packets[i]->stream_index;
        int64_t pts = av_rescale_q(packets[i]->pts, template->streams[stream]->time_base,
                                   (AVRational){1, 1000000000});
        int64_t start = gk_now_ns();
        CHECK(gk_replay_add(replay, packets[i]) == 0);
        gk_hist_add(&admission, (uint64_t)(gk_now_ns() - start));
        av_packet_free(&packets[i]);
        if (!saved && stream == 0 && pts >= 16 * GK_NS) {
            start = gk_now_ns();
            CHECK(gk_replay_save(replay, argv[3], 16 * GK_NS) == 0);
            snapshot_ns = gk_now_ns() - start;
            saved = true;
        }
    }
    CHECK(saved);
    struct gk_replay_stats stats;
    gk_replay_close(&replay, &stats);
    int64_t elapsed = gk_now_ns() - begin;
    CHECK(getrusage(RUSAGE_SELF, &after) == 0 && stats.saved == 1 && stats.failed == 0);
    int64_t cpu = (after.ru_utime.tv_sec - before.ru_utime.tv_sec + after.ru_stime.tv_sec -
                   before.ru_stime.tv_sec) *
                      1000000 +
                  after.ru_utime.tv_usec - before.ru_utime.tv_usec + after.ru_stime.tv_usec -
                  before.ru_stime.tv_usec;
    printf("{\"elapsed_ns\":%" PRId64 ",\"cpu_us\":%" PRId64 ",\"packets\":%zu,"
           "\"admission_total_ns\":%" PRIu64 ",\"admission_p99_upper_ns\":%" PRIu64 ","
           "\"admission_max_ns\":%" PRIu64 ",\"snapshot_ns\":%" PRId64 ","
           "\"live_bytes\":%" PRIu64 ",\"owned_bytes\":%" PRIu64 ","
           "\"copied_bytes\":%" PRIu64 ",\"saved_bytes\":%" PRIu64 ",\"shared_keyframes\":%" PRIu64
           ",\"shared_key_bytes\":%" PRIu64 "}\n",
           elapsed, cpu, count, admission.total, gk_hist_percentile(&admission, 99),
           admission.maximum, snapshot_ns, stats.peak_live_bytes, stats.peak_owned_bytes,
           stats.copied_bytes, stats.saved_bytes, stats.shared_keyframes, stats.shared_key_bytes);
    av_free(packets);
    avformat_free_context(template);
    return 0;
}

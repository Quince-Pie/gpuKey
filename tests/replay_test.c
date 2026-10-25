#include "media.h"

#include <errno.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <math.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static void inspect(const char *path, const struct gk_replay_result *result, bool audio)
{
    AVFormatContext *input = nullptr;
    CHECK(avformat_open_input(&input, path, nullptr, nullptr) == 0);
    CHECK(avformat_find_stream_info(input, nullptr) == 0);
    CHECK(input->nb_streams == (audio ? 3U : 1U));
    AVCodecContext *decoders[3] = {};
    uint64_t samples[3] = {};
    float peak[3] = {};
    uint64_t peak_at[3] = {};
    int64_t video_end = -1;
    unsigned pictures = 0;
    for (unsigned i = 0; i < input->nb_streams; ++i) {
        const AVCodec *codec = i ? avcodec_find_decoder(input->streams[i]->codecpar->codec_id)
                                 : avcodec_find_decoder_by_name("libdav1d");
        CHECK(codec);
        decoders[i] = avcodec_alloc_context3(codec);
        CHECK(decoders[i] &&
              avcodec_parameters_to_context(decoders[i], input->streams[i]->codecpar) == 0);
        decoders[i]->thread_count = 1;
        decoders[i]->pkt_timebase = input->streams[i]->time_base;
        CHECK(avcodec_open2(decoders[i], codec, nullptr) == 0);
    }
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    while (av_read_frame(input, packet) >= 0) {
        unsigned track = (unsigned)packet->stream_index;
        CHECK(track < input->nb_streams);
        if (!track) {
            if (!pictures)
                CHECK((packet->flags & AV_PKT_FLAG_KEY) && packet->pts == 0);
            video_end = av_rescale_q(packet->pts + packet->duration, input->streams[0]->time_base,
                                     (AVRational){1, 1000000000});
        }
        CHECK(avcodec_send_packet(decoders[track], packet) == 0);
        int ret;
        while ((ret = avcodec_receive_frame(decoders[track], frame)) >= 0) {
            if (!track) {
                int64_t absolute =
                    result->start_ns + av_rescale_q(frame->pts, input->streams[0]->time_base,
                                                    (AVRational){1, 1000000000});
                int64_t index = av_rescale(absolute, 60, GK_NS);
                CHECK(frame->data[0][0] == (index & 1 ? 235 : 16));
                ++pictures;
            } else {
                int64_t expected_pts = av_rescale((int64_t)samples[track], GK_NS, 48000);
                int64_t actual_pts = av_rescale_q(frame->pts, input->streams[track]->time_base,
                                                  (AVRational){1, 1000000000});
                CHECK(llabs(actual_pts - expected_pts) <= 1000000);
                CHECK(frame->format == AV_SAMPLE_FMT_FLT || frame->format == AV_SAMPLE_FMT_FLTP);
                for (unsigned n = 0; n < (unsigned)frame->nb_samples; ++n) {
                    float value = frame->format == AV_SAMPLE_FMT_FLT
                                      ? ((float *)frame->data[0])[n * 2]
                                      : ((float *)frame->data[0])[n];
                    if (fabsf(value) > peak[track]) {
                        peak[track] = fabsf(value);
                        peak_at[track] = samples[track] + n;
                    }
                }
                samples[track] += (unsigned)frame->nb_samples;
            }
            av_frame_unref(frame);
        }
        CHECK(ret == AVERROR(EAGAIN));
        av_packet_unref(packet);
    }
    for (unsigned i = 0; i < input->nb_streams; ++i) {
        CHECK(avcodec_send_packet(decoders[i], nullptr) == 0);
        while (avcodec_receive_frame(decoders[i], frame) >= 0) {
            CHECK(i > 0);
            samples[i] += (unsigned)frame->nb_samples;
            av_frame_unref(frame);
        }
        if (i) {
            CHECK(samples[i] == (uint64_t)(av_rescale(result->end_ns, 48000, GK_NS) -
                                           av_rescale(result->start_ns, 48000, GK_NS)));
            if (result->start_ns < 5 * GK_NS && result->end_ns > 5 * GK_NS) {
                int64_t expected = av_rescale(5 * GK_NS - result->start_ns, 48000, GK_NS);
                CHECK(peak[i] > 0.3f && llabs((int64_t)peak_at[i] - expected) <= 96);
            }
        }
        avcodec_free_context(&decoders[i]);
    }
    CHECK(pictures > 30);
    CHECK(llabs(video_end - (result->end_ns - result->start_ns)) <= 1000000);
    av_frame_free(&frame);
    av_packet_free(&packet);
    avformat_close_input(&input);
}

static void run(bool audio, bool pcm, bool copy, bool matroska, bool io_fault)
{
    struct gk_gpu gpu;
    CHECK(gk_gpu_open(&gpu, GK_SOFTWARE, nullptr) == 0);
    struct gk_video_format format = {.width = 64,
                                     .height = 48,
                                     .pixel_format = AV_PIX_FMT_BGRA,
                                     .transfer = AVCOL_TRC_IEC61966_2_1};
    static const struct gk_audio_track tracks[] = {{.format = GK_AUDIO_STEREO, .title = "First"},
                                                   {.format = GK_AUDIO_STEREO, .title = "Second"}};
    int gate[2];
    CHECK(pipe(gate) == 0);
    int started = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    CHECK(started >= 0);
    struct gk_media_options options = {.matroska = matroska,
                                       .bits = 8,
                                       .qp = 0,
                                       .surfaces = 4,
                                       .fps_num = 60,
                                       .fps_den = 1,
                                       .audio = tracks,
                                       .audio_count = audio ? 2 : 0,
                                       .audio_codec =
                                           pcm ? GK_AUDIO_CODEC_PCM : GK_AUDIO_CODEC_AUTO,
                                       .sink = {.slabs = 16, .slab_size = 4096, .stall_ms = 5000},
                                       .replay = {.duration_ns = 1100000000,
                                                  .bytes = 8U * 1024 * 1024,
                                                  .copy_packets = copy,
                                                  .test_gate = gate[0],
                                                  .test_started = started}};
    struct gk_media *media;
    if (io_fault)
        options.sink.fail_after = 256;
    CHECK(gk_media_open(&media, &gpu, &format, &options) == 0);
    char path[] = "/tmp/gpukey-replay-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0 && close(fd) == 0 && unlink(path) == 0);
    CHECK(gk_media_save(media, path, GK_NS) == -EAGAIN);
    uint8_t pixels[64 * 48 * 4];
    struct gk_image image = {.data = pixels, .size = sizeof(pixels), .stride = 64 * 4};
    float sound[800 * 2] = {};
    for (uint64_t i = 0; i < 1200; ++i) {
        memset(pixels, i & 1 ? 255 : 0, sizeof(pixels));
        AVFrame *frame;
        int64_t pts;
        CHECK(gk_timestamp(i, 60, 1, &pts));
        CHECK(gk_media_prepare(media, &image, VA_INVALID_ID, &frame) == 0);
        CHECK(gk_media_submit(media, frame, pts) == 0);
        if (audio)
            for (unsigned track = 0; track < 2; ++track) {
                for (unsigned j = 0; j < 800; ++j) {
                    uint64_t sample = i * 800 + j;
                    sound[j * 2] = sample == 5 * 48000
                                       ? 0.8f
                                       : (float)(0.1 * sin((double)sample * (track + 1) * 220 *
                                                           6.283185307179586 / 48000));
                    sound[j * 2 + 1] = (float)(0.1 * sin((double)sample * (track + 1) * 330 *
                                                         6.283185307179586 / 48000));
                }
                CHECK(gk_media_audio(media, track, sound, 800, pts) == 0);
            }
        if (i == 336) {
            CHECK(gk_media_save(media, path, pts) == 0);
            CHECK(gk_wait(started, 5000) == 1);
            CHECK(gk_media_save(media, path, pts) == -EBUSY);
        }
    }
    CHECK(write(gate[1], "x", 1) == 1);
    struct gk_replay_result result;
    CHECK(gk_wait(gk_media_replay_fd(media), 10000) == 1);
    CHECK(gk_media_saved(media, &result) && result.error == (io_fault ? -ENOSPC : 0));
    CHECK(result.start_ns == 4 * GK_NS);
    CHECK(result.end_ns > 5500000000 && result.end_ns <= 5600000000);
    if (!io_fault)
        inspect(path, &result, audio);
    else {
        AVFrame *frame;
        CHECK(gk_media_prepare(media, &image, VA_INVALID_ID, &frame) == 0);
        CHECK(gk_media_submit(media, frame, 20 * GK_NS) == 0);
    }
    /* Existing output refuses overwrite; the live encoder remains usable. */
    CHECK(gk_media_save(media, path, 19990000000) == 0);
    CHECK(write(gate[1], "x", 1) == 1);
    CHECK(gk_wait(gk_media_replay_fd(media), 10000) == 1);
    CHECK(gk_media_saved(media, &result) && result.error == -EEXIST);
    char final[] = "/tmp/gpukey-replay-stop-XXXXXX";
    fd = mkstemp(final);
    CHECK(fd >= 0 && close(fd) == 0 && unlink(final) == 0);
    CHECK(gk_media_save(media, final, 19990000000) == 0);
    CHECK(write(gate[1], "x", 1) == 1);
    struct gk_media_stats stats;
    CHECK(gk_media_close(&media, 20 * GK_NS + (io_fault ? GK_NS / 60 : 0), &stats) == 0);
    CHECK(stats.replay.saved == (io_fault ? 0U : 2U) &&
          stats.replay.failed == (io_fault ? 3U : 1U));
    CHECK(stats.replay.last.error == (io_fault ? -ENOSPC : 0));
    if (!io_fault)
        inspect(final, &stats.replay.last, audio);
    CHECK(stats.replay.peak_live_bytes <= options.replay.bytes);
    CHECK(stats.replay.peak_owned_bytes <= options.replay.bytes * 2 + 16384);
    CHECK((stats.replay.copied_bytes != 0) == copy);
    CHECK(stats.sink.accepted == 0 && stats.sink.written == 0);
    gk_gpu_close(&gpu);
    close(gate[0]);
    close(gate[1]);
    close(started);
    CHECK(unlink(path) == 0);
    CHECK(unlink(final) == 0);
}

static void invalid_and_full(void)
{
    AVFormatContext *format = avformat_alloc_context();
    CHECK(format);
    AVStream *video = avformat_new_stream(format, nullptr);
    AVStream *audio = avformat_new_stream(format, nullptr);
    CHECK(video && audio);
    video->time_base = (AVRational){1, 1000000};
    video->codecpar->codec_id = AV_CODEC_ID_AV1;
    audio->time_base = (AVRational){1, 48000};
    audio->codecpar->codec_id = AV_CODEC_ID_PCM_F32LE;
    audio->codecpar->sample_rate = 48000;
    av_channel_layout_default(&audio->codecpar->ch_layout, 2);
    struct gk_replay_options options = {.duration_ns = 30 * GK_NS, .bytes = 1048576};
    struct gk_replay *replay;
    CHECK(gk_replay_open(&replay, format, &options) == 0);
    AVPacket *packet = av_packet_alloc();
    CHECK(packet && av_new_packet(packet, 4) == 0);
    memset(packet->data, 0xff, 4);
    packet->pts = packet->dts = 0;
    packet->flags = AV_PKT_FLAG_KEY;
    CHECK(gk_replay_add(replay, packet) == -EPROTO); /* A flag is not a valid AV1 RAP. */
    CHECK(packet->buf);                              /* Failure leaves caller ownership intact. */
    packet->flags = 0;
    CHECK(gk_replay_add(replay, packet) == -ENOTSUP);
    packet->dts = 1;
    CHECK(gk_replay_add(replay, packet) == -EPROTO);
    av_packet_unref(packet);
    int ret = 0;
    for (int64_t i = 0; ret == 0 && i < 300; ++i) {
        CHECK(av_new_packet(packet, 960 * 2 * 4) == 0);
        memset(packet->data, 0, (size_t)packet->size);
        packet->stream_index = 1;
        packet->pts = packet->dts = i * 960;
        packet->duration = 960;
        ret = gk_replay_add(replay, packet);
        if (!ret)
            CHECK(!packet->buf);
    }
    CHECK(ret == -ENOBUFS && packet->buf);
    av_packet_free(&packet);
    struct gk_replay_stats stats;
    gk_replay_close(&replay, &stats);
    CHECK(stats.peak_live_bytes <= options.bytes && stats.packets > 100);
    avformat_free_context(format);
}

static void rapid_saves(void)
{
    struct gk_gpu gpu;
    CHECK(gk_gpu_open(&gpu, GK_SOFTWARE, nullptr) == 0);
    struct gk_video_format format = {.width = 64,
                                     .height = 48,
                                     .pixel_format = AV_PIX_FMT_BGRA,
                                     .transfer = AVCOL_TRC_IEC61966_2_1};
    struct gk_media_options options = {.matroska = true,
                                       .bits = 8,
                                       .qp = 0,
                                       .surfaces = 4,
                                       .fps_num = 60,
                                       .fps_den = 1,
                                       .sink = {.slabs = 8, .slab_size = 4096, .stall_ms = 5000},
                                       .replay = {.duration_ns = GK_NS, .bytes = 1048576}};
    struct gk_media *media;
    CHECK(gk_media_open(&media, &gpu, &format, &options) == 0);
    uint8_t pixels[64 * 48 * 4];
    struct gk_image image = {.data = pixels, .size = sizeof(pixels), .stride = 64 * 4};
    for (unsigned i = 0; i < 80; ++i) {
        memset(pixels, i & 1 ? 255 : 0, sizeof(pixels));
        AVFrame *frame;
        int64_t pts;
        CHECK(gk_timestamp(i, 60, 1, &pts));
        CHECK(gk_media_prepare(media, &image, VA_INVALID_ID, &frame) == 0);
        CHECK(gk_media_submit(media, frame, pts) == 0);
    }
    char directory[] = "/tmp/gpukey-rapid-save-XXXXXX", path[256];
    CHECK(mkdtemp(directory));
    struct gk_replay_result result;
    for (unsigned i = 0; i < 64; ++i) {
        snprintf(path, sizeof(path), "%s/%u.mkv", directory, i);
        CHECK(gk_media_save(media, path, 1300000000) == 0);
        int64_t deadline = gk_now_ns() + 5 * GK_NS;
        /* Read completion publication directly, without waiting for the worker
         * to finish its notification, then immediately publish the next job. */
        while (!gk_media_saved(media, &result)) {
            CHECK(gk_now_ns() < deadline);
            sched_yield();
        }
        CHECK(result.error == 0);
    }
    snprintf(path, sizeof(path), "%s/final.mkv", directory);
    CHECK(gk_media_save(media, path, 1300000000) == 0);
    struct gk_media_stats stats;
    CHECK(gk_media_close(&media, 1400000000, &stats) == 0);
    CHECK(stats.replay.saved == 65 && stats.replay.failed == 0);
    inspect(path, &stats.replay.last, false);
    CHECK(unlink(path) == 0);
    for (unsigned i = 0; i < 64; ++i) {
        snprintf(path, sizeof(path), "%s/%u.mkv", directory, i);
        CHECK(unlink(path) == 0);
    }
    CHECK(rmdir(directory) == 0);
    gk_gpu_close(&gpu);
}

static void precision(bool audio, bool opus, bool leading)
{
    struct gk_gpu gpu;
    CHECK(gk_gpu_open(&gpu, GK_SOFTWARE, nullptr) == 0);
    struct gk_video_format format = {.width = 64,
                                     .height = 48,
                                     .pixel_format = AV_PIX_FMT_BGRA,
                                     .transfer = AVCOL_TRC_IEC61966_2_1};
    static const struct gk_audio_track track = {.format = GK_AUDIO_STEREO, .title = "Precision"};
    uint32_t fps = audio ? 60 : 2000;
    struct gk_media_options options = {
        .matroska = true,
        .bits = 8,
        .qp = 0,
        .surfaces = 4,
        .fps_num = fps,
        .fps_den = 1,
        .audio = &track,
        .audio_count = audio ? 1 : 0,
        .audio_codec = opus ? GK_AUDIO_CODEC_AUTO : GK_AUDIO_CODEC_PCM,
        .sink = {.slabs = 8, .slab_size = 4096, .stall_ms = 5000},
        .replay = {.duration_ns = leading ? 100000000 : GK_NS, .bytes = 1048576}};
    struct gk_media *media;
    CHECK(gk_media_open(&media, &gpu, &format, &options) == 0);
    uint8_t pixels[64 * 48 * 4] = {};
    float sound[800 * 2] = {};
    struct gk_image image = {.data = pixels, .size = sizeof(pixels), .stride = 64 * 4};
    /* Feed beyond the requested cut so resampler/encoder buffering cannot
     * move the common completed watermark before that test endpoint. */
    for (unsigned i = 0; i < (leading ? 144U : 6U); ++i) {
        AVFrame *frame;
        int64_t pts;
        CHECK(gk_timestamp(i, fps, 1, &pts));
        CHECK(gk_media_prepare(media, &image, VA_INVALID_ID, &frame) == 0);
        CHECK(gk_media_submit(media, frame, pts + (leading && i >= 120 ? 19750000 : 0)) == 0);
        if (audio)
            CHECK(gk_media_audio(media, 0, sound, 800, pts) == 0);
    }
    char path[] = "/tmp/gpukey-replay-precision-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0 && close(fd) == 0 && unlink(path) == 0);
    /* Pinned Opus priming is 312 samples: the second packet starts at 13.5 ms.
     * Both audio cuts retain a 12-sample tail that rounds to zero MKV ticks. */
    int64_t end = leading ? 2180000000 : audio ? (opus ? 13750000 : 20250000) : 3000000;
    CHECK(gk_media_save(media, path, end) == 0);
    CHECK(gk_wait(gk_media_replay_fd(media), 5000) == 1);
    struct gk_replay_result result;
    CHECK(gk_media_saved(media, &result));
    CHECK(result.error == (audio ? 0 : -ERANGE));
    CHECK(gk_media_close(&media, leading ? 2420000000 : audio ? 100000000 : 3000000, nullptr) == 0);
    if (audio) {
        AVFormatContext *file = nullptr;
        CHECK(avformat_open_input(&file, path, nullptr, nullptr) == 0);
        CHECK(avformat_find_stream_info(file, nullptr) == 0 && file->nb_streams == 2);
        const AVCodec *codec = avcodec_find_decoder(file->streams[1]->codecpar->codec_id);
        AVCodecContext *decoder = avcodec_alloc_context3(codec);
        CHECK(decoder && avcodec_parameters_to_context(decoder, file->streams[1]->codecpar) == 0);
        CHECK(avcodec_open2(decoder, codec, nullptr) == 0);
        AVPacket *packet = av_packet_alloc();
        AVFrame *frame = av_frame_alloc();
        uint64_t samples = 0;
        while (av_read_frame(file, packet) >= 0) {
            if (packet->stream_index == 1) {
                CHECK(avcodec_send_packet(decoder, packet) == 0);
                while (avcodec_receive_frame(decoder, frame) >= 0) {
                    samples += (unsigned)frame->nb_samples;
                    av_frame_unref(frame);
                }
            }
            av_packet_unref(packet);
        }
        CHECK(avcodec_send_packet(decoder, nullptr) == 0);
        while (avcodec_receive_frame(decoder, frame) >= 0) {
            samples += (unsigned)frame->nb_samples;
            av_frame_unref(frame);
        }
        CHECK(result.start_ns == (leading ? 2019750000 : 0) && result.end_ns == end);
        CHECK(samples == (uint64_t)(av_rescale(end, 48000, GK_NS) -
                                    av_rescale(result.start_ns, 48000, GK_NS)));
        avcodec_free_context(&decoder);
        av_packet_free(&packet);
        av_frame_free(&frame);
        avformat_close_input(&file);
    }
    CHECK(unlink(path) == 0);
    gk_gpu_close(&gpu);
}

int main(void)
{
    av_log_set_level(AV_LOG_ERROR);
    run(false, false, false, true, false);
    run(true, true, false, true, false);
    run(true, false, false, true, false);
    run(true, false, true, true, false);
    run(true, true, false, false, false);
    run(false, false, false, true, true);
    invalid_and_full();
    rapid_saves();
    precision(false, false, false);
    precision(true, false, false);
    precision(true, true, false);
    precision(true, false, true);
    puts("replay: independent cuts, eviction, paused export, audio timing, copy control, stop, "
         "invalid packets and byte limit pass");
    return 0;
}

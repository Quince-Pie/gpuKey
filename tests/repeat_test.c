#include "media.h"

#include <dlfcn.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <threads.h>
#include <unistd.h>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static once_flag lookup = ONCE_FLAG_INIT;
static typeof(&av_packet_alloc) real_packet_alloc;
static typeof(&av_buffer_ref) real_buffer_ref;
static _Atomic int deny_once;

static void symbols(void)
{
    void *packet = dlsym(RTLD_NEXT, "av_packet_alloc");
    void *buffer = dlsym(RTLD_NEXT, "av_buffer_ref");
    static_assert(sizeof(packet) == sizeof(real_packet_alloc));
    static_assert(sizeof(buffer) == sizeof(real_buffer_ref));
    memcpy(&real_packet_alloc, &packet, sizeof(packet));
    memcpy(&real_buffer_ref, &buffer, sizeof(buffer));
    CHECK(real_packet_alloc && real_buffer_ref);
}

AVPacket *av_packet_alloc(void)
{
    call_once(&lookup, symbols);
    int expected = 1;
    return atomic_compare_exchange_strong(&deny_once, &expected, 0) ? nullptr : real_packet_alloc();
}

AVBufferRef *av_buffer_ref(const AVBufferRef *buffer)
{
    call_once(&lookup, symbols);
    int expected = 2;
    return atomic_compare_exchange_strong(&deny_once, &expected, 0) ? nullptr
                                                                    : real_buffer_ref(buffer);
}

static void inspect(const char *path, int64_t endpoint, bool old_snapshot, bool audio)
{
    AVFormatContext *input = nullptr;
    CHECK(avformat_open_input(&input, path, nullptr, nullptr) == 0);
    CHECK(avformat_find_stream_info(input, nullptr) == 0);
    const AVCodec *decoder = avcodec_find_decoder_by_name("libdav1d");
    AVCodecContext *codec = avcodec_alloc_context3(decoder);
    CHECK(codec && avcodec_parameters_to_context(codec, input->streams[0]->codecpar) == 0);
    codec->thread_count = 1;
    CHECK(avcodec_open2(codec, decoder, nullptr) == 0);
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    CHECK(packet && frame);
    unsigned pictures = 0;
    int64_t end = -1, previous = -1, audio_samples = 0;
    while (av_read_frame(input, packet) >= 0) {
        if (packet->stream_index) {
            CHECK(audio && packet->stream_index == 1 && packet->size % 8 == 0);
            audio_samples += packet->size / 8;
            av_packet_unref(packet);
            continue;
        }
        int64_t pts =
            av_rescale_q(packet->pts, input->streams[0]->time_base, (AVRational){1, 1000000000});
        CHECK(pts > previous);
        previous = pts;
        end = av_rescale_q(packet->pts + packet->duration, input->streams[0]->time_base,
                           (AVRational){1, 1000000000});
        CHECK(avcodec_send_packet(codec, packet) == 0);
        while (avcodec_receive_frame(codec, frame) == 0) {
            unsigned expected = old_snapshot || pts < 7 * GK_NS ? 16 : 235;
            CHECK(frame->format == AV_PIX_FMT_YUV420P && frame->data[0][0] == expected);
            ++pictures;
            av_frame_unref(frame);
        }
        av_packet_unref(packet);
    }
    CHECK(end == endpoint && pictures >= 2);
    if (old_snapshot)
        CHECK(pictures == 2 && previous == endpoint - av_rescale_q(1, input->streams[0]->time_base,
                                                                   (AVRational){1, 1000000000}));
    if (audio)
        CHECK(audio_samples == av_rescale(endpoint, 48000, GK_NS));
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&codec);
    avformat_close_input(&input);
}

static void run(bool replay, bool matroska, bool audio, int fault)
{
    char directory[] = "/tmp/gpukey-repeat-test-XXXXXX", path[256];
    CHECK(mkdtemp(directory));
    snprintf(path, sizeof(path), "%s/clip.%s", directory, matroska ? "mkv" : "mp4");
    struct gk_gpu gpu;
    CHECK(gk_gpu_open(&gpu, GK_SOFTWARE, nullptr) == 0);
    struct gk_video_format format = {.width = 64,
                                     .height = 48,
                                     .pixel_format = AV_PIX_FMT_BGRA,
                                     .transfer = AVCOL_TRC_IEC61966_2_1};
    static const struct gk_audio_track track = {.format = GK_AUDIO_STEREO, .title = "Clock"};
    int gate[2] = {-1, -1}, started = 0;
    if (replay) {
        CHECK(pipe(gate) == 0);
        started = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        CHECK(started > 0);
    }
    struct gk_media_options options = {.output = path,
                                       .matroska = matroska,
                                       .bits = 8,
                                       .qp = 0,
                                       .surfaces = 4,
                                       .fps_num = 60,
                                       .fps_den = 1,
                                       .audio = &track,
                                       .audio_count = audio ? 1U : 0U,
                                       .audio_codec = GK_AUDIO_CODEC_PCM,
                                       .sink = {.slabs = 8, .slab_size = 4096, .stall_ms = 5000},
                                       .replay = {.duration_ns = replay ? 30 * GK_NS : 0,
                                                  .bytes = 32 * 1024 * 1024,
                                                  .test_gate = replay ? gate[0] : 0,
                                                  .test_started = started}};
    struct gk_media *media;
    CHECK(gk_media_open(&media, &gpu, &format, &options) == 0);
    uint8_t pixels[64 * 48 * 4] = {};
    struct gk_image image = {.data = pixels, .size = sizeof(pixels), .stride = 64 * 4};
    AVFrame *frame;
    CHECK(gk_media_prepare(media, &image, VA_INVALID_ID, &frame) == 0);
    CHECK(gk_media_submit(media, frame, 0) == 0);
    float samples[960 * 2];
    for (unsigned i = 0; i < 960 * 2; ++i)
        samples[i] = 0.125f;
    unsigned steps = replay ? 2000 : 400;
    for (unsigned i = 0; i < steps; ++i) {
        int64_t pts = (int64_t)i * 20000000;
        if (i == 100)
            atomic_store(&deny_once, fault);
        if (i && i <= 300 && i % 100 == 0) {
            CHECK(gk_media_repeat(media, pts) == 0);
            CHECK(atomic_load(&deny_once) == 0);
        }
        if (i >= 350) {
            memset(pixels, 255, sizeof(pixels));
            CHECK(gk_media_prepare(media, &image, VA_INVALID_ID, &frame) == 0);
            CHECK(gk_media_submit(media, frame, pts) == 0);
        }
        if (audio)
            CHECK(gk_media_audio(media, 0, samples, 960, pts) == 0);
        if (replay && i == 330) {
            CHECK(gk_media_save(media, path, 6500000000) == 0);
            CHECK(gk_wait(started, 5000) == 1);
        }
    }
    if (replay) {
        /* Resume only after the original key's live history and media cache
         * have been replaced. The export's immutable payload must still live. */
        CHECK(write(gate[1], "x", 1) == 1);
        CHECK(gk_wait(gk_media_replay_fd(media), 10000) == 1);
        struct gk_replay_result result;
        CHECK(gk_media_saved(media, &result) && result.error == 0);
        CHECK(result.start_ns == 0 && result.end_ns == 6500000000);
    }
    struct gk_media_stats stats;
    CHECK(gk_media_close(&media, (int64_t)steps * 20000000, &stats) == 0);
    CHECK(stats.reused_packets == (fault ? 2U : 3U));
    CHECK(stats.submitted == 1 + steps - 350 + (fault ? 1U : 0U) &&
          stats.packets == stats.submitted);
    if (replay) {
        CHECK(stats.replay.saved == 1 && stats.replay.failed == 0);
        CHECK(close(gate[0]) == 0 && close(gate[1]) == 0 && close(started) == 0);
    }
    gk_gpu_close(&gpu);
    inspect(path, replay ? 6500000000 : 8 * GK_NS, replay, audio);
    CHECK(unlink(path) == 0 && rmdir(directory) == 0);
}

static void independent_cadence(bool independent, bool fault)
{
    char path[] = "/tmp/gpukey-independent-repeat-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0 && close(fd) == 0 && unlink(path) == 0);
    struct gk_gpu gpu;
    CHECK(gk_gpu_open(&gpu, GK_SOFTWARE, nullptr) == 0);
    struct gk_video_format format = {.width = 64,
                                     .height = 48,
                                     .pixel_format = AV_PIX_FMT_BGRA,
                                     .transfer = AVCOL_TRC_IEC61966_2_1};
    struct gk_media_options options = {.output = path,
                                       .matroska = true,
                                       .bits = 8,
                                       .qp = 0,
                                       .surfaces = 4,
                                       .fps_num = 60,
                                       .fps_den = 1,
                                       .independent_repeats = independent,
                                       .sink = {.slabs = 8, .slab_size = 4096, .stall_ms = 5000}};
    struct gk_media *media;
    CHECK(gk_media_open(&media, &gpu, &format, &options) == 0);
    uint8_t pixels[64 * 48 * 4] = {};
    struct gk_image image = {.data = pixels, .size = sizeof(pixels), .stride = 64 * 4};
    AVFrame *frame;
    CHECK(gk_media_prepare(media, &image, VA_INVALID_ID, &frame) == 0);
    CHECK(gk_media_submit(media, frame, 0) == 0);
    CHECK(gk_media_repeat(media, GK_NS / 2) == 0);
    if (fault)
        atomic_store(&deny_once, 2); /* A later reference, after a successful cache. */
    CHECK(gk_media_repeat(media, GK_NS) == 0);
    CHECK(atomic_load(&deny_once) == 0);
    memset(pixels, 255, sizeof(pixels));
    CHECK(gk_media_prepare(media, &image, VA_INVALID_ID, &frame) == 0);
    /* Preparing an unsubmitted image does not replace the displayed key. */
    CHECK(gk_media_repeat(media, 5 * GK_NS / 4) == 0);
    CHECK(gk_media_submit(media, frame, 3 * GK_NS / 2) == 0);
    CHECK(gk_media_repeat(media, 7 * GK_NS / 4) == 0);
    CHECK(gk_media_repeat(media, 2 * GK_NS) == 0);
    CHECK(gk_media_close(&media, 9 * GK_NS / 4, nullptr) == 0);
    gk_gpu_close(&gpu);

    AVFormatContext *input = nullptr;
    CHECK(avformat_open_input(&input, path, nullptr, nullptr) == 0);
    CHECK(avformat_find_stream_info(input, nullptr) == 0);
    AVCodecContext *decoder = avcodec_alloc_context3(avcodec_find_decoder_by_name("libdav1d"));
    CHECK(decoder && avcodec_parameters_to_context(decoder, input->streams[0]->codecpar) == 0);
    decoder->thread_count = 1;
    CHECK(avcodec_open2(decoder, nullptr, nullptr) == 0);
    AVPacket *packet = av_packet_alloc();
    frame = av_frame_alloc();
    CHECK(packet && frame);
    static constexpr int64_t points[] = {
        0, GK_NS / 2, GK_NS, 5 * GK_NS / 4, 3 * GK_NS / 2, 7 * GK_NS / 4, 2 * GK_NS, 9 * GK_NS / 4};
    unsigned count = 0, decoded = 0;
    while (av_read_frame(input, packet) >= 0) {
        CHECK(count < 7 && packet->stream_index == 0);
        CHECK(av_rescale_q(packet->pts, input->streams[0]->time_base,
                           (AVRational){1, 1000000000}) == points[count]);
        CHECK(av_rescale_q(packet->pts + packet->duration, input->streams[0]->time_base,
                           (AVRational){1, 1000000000}) == points[count + 1]);
        if (independent && count != 4)
            CHECK(packet->flags & AV_PKT_FLAG_KEY);
        CHECK(avcodec_send_packet(decoder, packet) == 0);
        int ret;
        while ((ret = avcodec_receive_frame(decoder, frame)) == 0) {
            CHECK(decoded < 7 && frame->format == AV_PIX_FMT_YUV420P);
            CHECK(frame->data[0][0] == (decoded < 4 ? 16 : 235));
            ++decoded;
            av_frame_unref(frame);
        }
        CHECK(ret == AVERROR(EAGAIN));
        ++count;
        av_packet_unref(packet);
    }
    CHECK(count == 7 && decoded == 7);
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&decoder);
    avformat_close_input(&input);
    CHECK(unlink(path) == 0);
}

int main(void)
{
    run(false, false, false, 0);
    run(false, true, false, 0);
    run(true, false, false, 0);
    run(true, true, false, 0);
    run(true, false, true, 0);
    run(true, true, true, 0);
    run(false, true, false, 1);
    run(true, true, false, 2);
    independent_cadence(false, false);
    independent_cadence(true, false);
    independent_cadence(true, true);
    puts("repeat: independent keys, resumed inputs, paused exports, eviction, PCM clocks and "
         "teardown pass");
}

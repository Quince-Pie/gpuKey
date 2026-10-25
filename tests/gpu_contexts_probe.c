/* Independent AV1 streams exercise shared driver allocation and teardown. */
#include "media.h"

#include <errno.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/intreadwrite.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

struct worker {
    const char *directory;
    pthread_barrier_t *barrier;
    unsigned index;
};

static void synchronize(pthread_barrier_t *barrier)
{
    int ret = pthread_barrier_wait(barrier);
    CHECK(ret == 0 || ret == PTHREAD_BARRIER_SERIAL_THREAD);
}

static unsigned gray(unsigned worker, unsigned cycle, unsigned frame)
{
    return ((worker + cycle + frame) % 4) * 85;
}

static void collect_all(struct gk_media *media)
{
    CHECK(gk_media_collect(media, false) == 0);
    int fd;
    while ((fd = gk_media_completion_fd(media)) >= 0) {
        struct pollfd descriptor = {.fd = fd, .events = POLLIN};
        int ready;
        do {
            ready = poll(&descriptor, 1, 1000);
        } while (ready < 0 && errno == EINTR);
        CHECK(ready == 1 && (descriptor.revents & POLLIN) &&
              !(descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)));
        CHECK(gk_media_collect(media, true) == 0);
    }
    CHECK(gk_media_error(media) == 0);
}

static void inspect_owned_input(const AVFrame *hardware, unsigned bits, unsigned value)
{
    AVFrame *cpu = av_frame_alloc();
    CHECK(cpu && av_hwframe_transfer_data(cpu, hardware, 0) == 0);
    CHECK(cpu->width == 320 && cpu->height == 240);
    CHECK(cpu->format == (bits == 8 ? AV_PIX_FMT_NV12 : AV_PIX_FMT_P010LE));
    const uint8_t *row = cpu->data[0] + 8 * cpu->linesize[0];
    int sample = bits == 8 ? row[8] : AV_RL16(row + 16) >> 6;
    int scale = bits == 8 ? 1 : 4;
    int expected = (16 + (int)(219 * value / 255)) * scale;
    CHECK(abs(sample - expected) <= scale);
    av_frame_free(&cpu);
}

static void inspect(const AVFrame *frame, unsigned bits, unsigned worker, unsigned cycle,
                    unsigned index)
{
    CHECK(index < 8 && frame->width == 320 && frame->height == 240);
    CHECK(frame->format == (bits == 8 ? AV_PIX_FMT_YUV420P : AV_PIX_FMT_YUV420P10LE));
    CHECK(frame->color_range == AVCOL_RANGE_MPEG);
    int scale = bits == 8 ? 1 : 4;
    int expected = (16 + (int)(219 * gray(worker, cycle, index) / 255)) * scale;
    for (unsigned plane = 0; plane < 3; ++plane) {
        unsigned width = plane ? 160 : 320, height = plane ? 120 : 240;
        int value = plane ? 128 * scale : expected;
        for (unsigned y = 0; y < height; y += 7) {
            const uint8_t *row = frame->data[plane] + (size_t)y * (unsigned)frame->linesize[plane];
            for (unsigned x = 0; x < width; x += 11) {
                int sample = bits == 8 ? row[x] : AV_RL16(row + 2 * x);
                CHECK(abs(sample - value) <= 4 * scale);
            }
        }
    }
}

static void decode(const char *path, unsigned bits, unsigned worker, unsigned cycle, bool repeats)
{
    AVFormatContext *input = nullptr;
    CHECK(avformat_open_input(&input, path, nullptr, nullptr) == 0);
    CHECK(avformat_find_stream_info(input, nullptr) == 0 && input->nb_streams == 1);
    const AVCodec *decoder = avcodec_find_decoder_by_name("libdav1d");
    AVCodecContext *context = avcodec_alloc_context3(decoder);
    CHECK(decoder && context &&
          avcodec_parameters_to_context(context, input->streams[0]->codecpar) == 0);
    context->thread_count = 1;
    CHECK(avcodec_open2(context, decoder, nullptr) == 0);
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    CHECK(packet && frame);
    unsigned count = 0;
    int read_result;
    while ((read_result = av_read_frame(input, packet)) >= 0) {
        CHECK(packet->stream_index == 0 && avcodec_send_packet(context, packet) == 0);
        int ret;
        while ((ret = avcodec_receive_frame(context, frame)) == 0) {
            inspect(frame, bits, worker, cycle, count++ / (repeats ? 3U : 1U));
            av_frame_unref(frame);
        }
        CHECK(ret == AVERROR(EAGAIN));
        av_packet_unref(packet);
    }
    CHECK(read_result == AVERROR_EOF);
    CHECK(avcodec_send_packet(context, nullptr) == 0);
    int ret;
    while ((ret = avcodec_receive_frame(context, frame)) == 0) {
        inspect(frame, bits, worker, cycle, count++ / (repeats ? 3U : 1U));
        av_frame_unref(frame);
    }
    CHECK(ret == AVERROR_EOF && count == (repeats ? 24U : 8U));
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&context);
    avformat_close_input(&input);
}

static void *run(void *opaque)
{
    const struct worker *worker = opaque;
    const char *repeat_mode = getenv("GPUKEY_TEST_INDEPENDENT_REPEATS");
    bool repeats = repeat_mode && !strcmp(repeat_mode, "1");
    int64_t step = GK_NS / 60 + (repeats ? 4 * GK_NS : 0);
    uint8_t pixels[320 * 240 * 4];
    for (unsigned cycle = 0; cycle < 4; ++cycle) {
        unsigned bits = (worker->index + cycle) % 2 ? 10 : 8;
        char path[PATH_MAX];
        int n =
            snprintf(path, sizeof(path), "%s/%u-%u.mkv", worker->directory, worker->index, cycle);
        CHECK(n > 0 && (size_t)n < sizeof(path));
        struct gk_gpu gpu;
        CHECK(gk_gpu_open(&gpu, GK_VAAPI, nullptr) == 0);
        struct gk_video_format format = {.width = 320,
                                         .height = 240,
                                         .pixel_format = AV_PIX_FMT_BGRA,
                                         .transfer = AVCOL_TRC_IEC61966_2_1};
        struct gk_media_options options = {
            .output = path,
            .bits = bits,
            .qp = 32,
            .fps_num = 60,
            .fps_den = 1,
            .matroska = true,
            .sink = {.slabs = 8, .slab_size = 65536, .stall_ms = 5000}};
        const char *compact = getenv("GPUKEY_TEST_COMPACT");
        options.compact_gpu = compact && !strcmp(compact, "1");
        options.independent_repeats = repeats;
        if (options.compact_gpu) {
            static constexpr unsigned capacities[] = {1, 3, 257, 1'000'000};
            options.surfaces = capacities[worker->index];
            options.encode_depth = worker->index ? 2 : 1;
            options.packet_readout = GK_READOUT_EVENTS;
        }
        struct gk_media *media;
        CHECK(gk_media_open(&media, &gpu, &format, &options) == 0);
        synchronize(worker->barrier);
        struct gk_image image = {.data = pixels, .size = sizeof(pixels), .stride = 320 * 4};
        AVFrame *pin = nullptr, *tail_pin = nullptr;
        for (unsigned i = 0; i < 8; ++i) {
            memset(pixels, (int)gray(worker->index, cycle, i), sizeof(pixels));
            for (size_t alpha = 3; alpha < sizeof(pixels); alpha += 4)
                pixels[alpha] = 255;
            AVFrame *frame;
            CHECK(gk_media_prepare(media, &image, VA_INVALID_ID, &frame) == 0);
            if (i == 1 && pin) {
                /* Finish the prior picture while this new input is prepared
                 * but not submitted. Collection must not evict this input. */
                collect_all(media);
                inspect_owned_input(frame, bits, gray(worker->index, cycle, i));
            }
            int64_t pts = repeats ? (int64_t)i * step : (int64_t)i * GK_NS / 60;
            CHECK(gk_media_submit(media, frame, pts) == 0);
            if (options.compact_gpu && options.surfaces > 1 && i == 0) {
                pin = av_frame_clone(frame);
                CHECK(pin);
            }
            if (i == 1 && pin) {
                inspect_owned_input(pin, bits, gray(worker->index, cycle, 0));
                av_frame_free(&pin);
                collect_all(media);
            }
            if (options.compact_gpu && i == 7) {
                tail_pin = av_frame_clone(frame);
                CHECK(tail_pin);
            }
            if (repeats) {
                collect_all(media);
                CHECK(gk_media_repeat(media, pts + 2 * GK_NS) == 0);
                collect_all(media);
                CHECK(gk_media_repeat(media, pts + 4 * GK_NS) == 0);
                collect_all(media);
            }
            if (i == 0)
                synchronize(worker->barrier);
        }
        struct gk_media_stats stats;
        CHECK(gk_media_close(&media, repeats ? 8 * step : 8 * GK_NS / 60, &stats) == 0);
        CHECK(stats.submitted == stats.packets);
        CHECK(stats.packets + stats.reused_packets == (repeats ? 24U : 8U));
        CHECK(!repeats || stats.reused_packets >= 8);
        gk_gpu_close(&gpu);
        if (tail_pin) {
            /* This reference must keep its surface/device alive after both
             * the encoder and the recorder's device owner have closed. */
            inspect_owned_input(tail_pin, bits, gray(worker->index, cycle, 7));
            av_frame_free(&tail_pin);
        }
        decode(path, bits, worker->index, cycle, repeats);
    }
    return nullptr;
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    pthread_t threads[4];
    pthread_barrier_t barrier;
    CHECK(pthread_barrier_init(&barrier, nullptr, 4) == 0);
    struct worker workers[4];
    for (unsigned i = 0; i < 4; ++i) {
        workers[i] = (struct worker){.directory = argv[1], .barrier = &barrier, .index = i};
        CHECK(pthread_create(&threads[i], nullptr, run, &workers[i]) == 0);
    }
    for (unsigned i = 0; i < 4; ++i)
        CHECK(pthread_join(threads[i], nullptr) == 0);
    CHECK(pthread_barrier_destroy(&barrier) == 0);
    puts("contexts: four concurrent workers, 16 encoder lifetimes, independent 8/10-bit pixels "
         "and configured repeated presentations pass");
}

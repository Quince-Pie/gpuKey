#include "media.h"

#include <dlfcn.h>
#include <errno.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
static typeof(&avcodec_send_frame) real_send;
static typeof(&avcodec_receive_packet) real_receive;
static AVCodecContext *target;
static unsigned inputs, outputs;
static bool receive_failure, failed_once;

static void symbols(void)
{
    void *send = dlsym(RTLD_NEXT, "avcodec_send_frame");
    void *receive = dlsym(RTLD_NEXT, "avcodec_receive_packet");
    static_assert(sizeof(send) == sizeof(real_send));
    static_assert(sizeof(receive) == sizeof(real_receive));
    memcpy(&real_send, &send, sizeof(send));
    memcpy(&real_receive, &receive, sizeof(receive));
    CHECK(real_send && real_receive);
}

int avcodec_send_frame(AVCodecContext *codec, const AVFrame *frame)
{
    call_once(&lookup, symbols);
    if (!target && frame)
        target = codec;
    if (codec == target) {
        CHECK(!failed_once); /* Another encoder call after a fatal error is unsafe. */
        if (frame && !receive_failure && inputs == 3) {
            failed_once = true;
            return AVERROR(ENOMEM);
        }
        if (frame)
            ++inputs;
    }
    return real_send(codec, frame);
}

int avcodec_receive_packet(AVCodecContext *codec, AVPacket *packet)
{
    call_once(&lookup, symbols);
    if (codec == target) {
        CHECK(!failed_once);
        if (receive_failure && outputs == 3) {
            failed_once = true;
            return AVERROR(ENOMEM);
        }
    }
    int ret = real_receive(codec, packet);
    if (codec == target && ret >= 0)
        ++outputs;
    return ret;
}

static void inspect(const char *path)
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
    int64_t end = 0;
    while (av_read_frame(input, packet) >= 0) {
        CHECK(packet->pts == end && packet->duration > 0);
        end = packet->pts + packet->duration;
        CHECK(avcodec_send_packet(codec, packet) == 0);
        while (avcodec_receive_frame(codec, frame) == 0) {
            CHECK(pictures < 3 && frame->format == AV_PIX_FMT_YUV420P);
            CHECK(frame->data[0][0] == (pictures % 2 ? 235 : 16));
            ++pictures;
            av_frame_unref(frame);
        }
        av_packet_unref(packet);
    }
    CHECK(pictures == 3);
    CHECK(av_rescale_q(end, input->streams[0]->time_base, (AVRational){1, 1000000000}) ==
          GK_NS / 20);
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&codec);
    avformat_close_input(&input);
}

static void run(bool matroska, bool on_receive)
{
    char directory[] = "/tmp/gpukey-codec-failure-XXXXXX", path[256];
    CHECK(mkdtemp(directory));
    snprintf(path, sizeof(path), "%s/clip.%s", directory, matroska ? "mkv" : "mp4");
    struct gk_gpu gpu;
    CHECK(gk_gpu_open(&gpu, GK_SOFTWARE, nullptr) == 0);
    struct gk_video_format format = {.width = 64,
                                     .height = 48,
                                     .pixel_format = AV_PIX_FMT_BGRA,
                                     .transfer = AVCOL_TRC_IEC61966_2_1};
    struct gk_media_options options = {.output = path,
                                       .matroska = matroska,
                                       .bits = 8,
                                       .qp = 0,
                                       .surfaces = 4,
                                       .fps_num = 60,
                                       .fps_den = 1,
                                       .sink = {.slabs = 8, .slab_size = 4096, .stall_ms = 5000}};
    struct gk_media *media;
    target = nullptr;
    inputs = outputs = 0;
    receive_failure = on_receive;
    failed_once = false;
    CHECK(gk_media_open(&media, &gpu, &format, &options) == 0);
    uint8_t pixels[64 * 48 * 4];
    struct gk_image image = {.data = pixels, .size = sizeof(pixels), .stride = 64 * 4};
    for (unsigned i = 0; i < 4; ++i) {
        memset(pixels, i % 2 ? 255 : 0, sizeof(pixels));
        AVFrame *frame;
        CHECK(gk_media_prepare(media, &image, VA_INVALID_ID, &frame) == 0);
        int ret = gk_media_submit(media, frame, (int64_t)i * GK_NS / 60);
        if (ret < 0) {
            CHECK(ret == AVERROR(ENOMEM) && failed_once);
            break;
        }
    }
    CHECK(failed_once && outputs == 3);
    CHECK(gk_media_collect(media, false) == AVERROR(ENOMEM));
    struct gk_media_stats stats;
    CHECK(gk_media_close(&media, GK_NS / 20, &stats) == AVERROR(ENOMEM));
    CHECK(!media && stats.packets == 3 && stats.submitted == 3);
    target = nullptr;
    gk_gpu_close(&gpu);
    inspect(path);
    CHECK(unlink(path) == 0 && rmdir(directory) == 0);
}

int main(void)
{
    for (unsigned container = 0; container < 2; ++container)
        for (unsigned failure = 0; failure < 2; ++failure)
            run(container != 0, failure != 0);
    puts("codec failure: no further encode/drain calls; completed pixels, timestamps and error "
         "status "
         "survive");
}

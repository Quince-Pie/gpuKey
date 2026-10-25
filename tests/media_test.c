#include "media.h"

#include <errno.h>
#include <fcntl.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static const struct gk_audio_track stereo = {.format = GK_AUDIO_STEREO, .title = "Test stereo"};

static void check_chroma_impulse(const AVFrame *f, unsigned phase)
{
    CHECK(f->format == AV_PIX_FMT_YUV420P);
    for (int y = 0; y < f->height; ++y)
        for (int x = 0; x < f->width; ++x)
            CHECK(f->data[0][(ptrdiff_t)y * f->linesize[0] + x] ==
                  (x == 12 + (int)phase ? 63 : 16));
    for (int y = 0; y < (f->height + 1) / 2; ++y)
        for (int x = 0; x < (f->width + 1) / 2; ++x) {
            bool active = x == 6 || (phase && x == 7);
            unsigned u = active ? (phase ? 122U : 115U) : 128U;
            unsigned v = active ? (phase ? 156U : 184U) : 128U;
            CHECK(f->data[1][(ptrdiff_t)y * f->linesize[1] + x] == u);
            CHECK(f->data[2][(ptrdiff_t)y * f->linesize[2] + x] == v);
        }
}

static void chroma_registration(int width, int height)
{
    char path[] = "/tmp/gpukey-chroma-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0 && close(fd) == 0 && unlink(path) == 0);
    struct gk_gpu gpu;
    CHECK(gk_gpu_open(&gpu, GK_SOFTWARE, nullptr) == 0);
    struct gk_video_format format = {.width = width,
                                     .height = height,
                                     .pixel_format = AV_PIX_FMT_BGRA,
                                     .transfer = AVCOL_TRC_IEC61966_2_1};
    struct gk_media_options options = {.output = path,
                                       .matroska = true,
                                       .bits = 8,
                                       .surfaces = 4,
                                       .qp = 0,
                                       .fps_num = 60,
                                       .fps_den = 1,
                                       .sink = {.slab_size = 4096, .slabs = 8, .stall_ms = 5000}};
    struct gk_media *media;
    CHECK(gk_media_open(&media, &gpu, &format, &options) == 0);
    size_t bytes = (size_t)width * (size_t)height * 4;
    uint8_t *pixels = calloc(1, bytes);
    CHECK(pixels);
    struct gk_image image = {.data = pixels, .size = bytes, .stride = width * 4};
    for (unsigned phase = 0; phase < 2; ++phase) {
        memset(pixels, 0, bytes);
        for (size_t i = 3; i < bytes; i += 4)
            pixels[i] = 255;
        for (int y = 0; y < height; ++y)
            pixels[((size_t)y * (unsigned)width + 12 + phase) * 4 + 2] = 255;
        AVFrame *frame;
        CHECK(gk_media_prepare(media, &image, VA_INVALID_ID, &frame) == 0);
        check_chroma_impulse(frame, phase);
        CHECK(gk_media_submit(media, frame, (int64_t)phase * GK_NS / 60) == 0);
    }
    CHECK(gk_media_close(&media, 2 * GK_NS / 60, nullptr) == 0);
    free(pixels);
    gk_gpu_close(&gpu);
    AVFormatContext *input = nullptr;
    CHECK(avformat_open_input(&input, path, nullptr, nullptr) == 0);
    CHECK(avformat_find_stream_info(input, nullptr) == 0);
    const AVCodec *decoder = avcodec_find_decoder_by_name("libdav1d");
    AVCodecContext *ctx = avcodec_alloc_context3(decoder);
    CHECK(ctx && avcodec_parameters_to_context(ctx, input->streams[0]->codecpar) == 0);
    ctx->thread_count = 1;
    CHECK(avcodec_open2(ctx, decoder, nullptr) == 0);
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    CHECK(packet && frame);
    unsigned count = 0;
    int read_result;
    while ((read_result = av_read_frame(input, packet)) >= 0) {
        CHECK(avcodec_send_packet(ctx, packet) == 0);
        int ret;
        while ((ret = avcodec_receive_frame(ctx, frame)) == 0) {
            CHECK(count < 2 && frame->chroma_location == AVCHROMA_LOC_LEFT);
            check_chroma_impulse(frame, count++);
            av_frame_unref(frame);
        }
        CHECK(ret == AVERROR(EAGAIN));
        av_packet_unref(packet);
    }
    CHECK(read_result == AVERROR_EOF && avcodec_send_packet(ctx, nullptr) == 0);
    int ret;
    while ((ret = avcodec_receive_frame(ctx, frame)) == 0) {
        CHECK(count < 2);
        check_chroma_impulse(frame, count++);
        av_frame_unref(frame);
    }
    CHECK(ret == AVERROR_EOF && count == 2);
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&ctx);
    avformat_close_input(&input);
    CHECK(unlink(path) == 0);
}

static void record(const char *path, bool audio, bool matroska)
{
    struct gk_gpu gpu;
    CHECK(gk_gpu_open(&gpu, GK_SOFTWARE, nullptr) == 0);
    struct gk_video_format format = {
        .width = 64,
        .height = 48,
        .pixel_format = AV_PIX_FMT_BGRA,
        .transfer = AVCOL_TRC_IEC61966_2_1,
    };
    struct gk_media_options options = {
        .output = path,
        .bits = 8,
        .surfaces = 4,
        .qp = 0,
        .fps_num = 60,
        .fps_den = 1,
        .audio_count = audio ? 1U : 0,
        .audio = &stereo,
        .matroska = matroska,
        .sink = {.slab_size = 4096, .slabs = 8, .stall_ms = 5000},
    };
    struct gk_media *media;
    CHECK(gk_media_open(&media, &gpu, &format, &options) == 0);
    uint8_t pixels[64 * 48 * 4];
    struct gk_image image = {.data = pixels, .size = sizeof(pixels), .stride = 64 * 4};
    static constexpr unsigned positions[] = {0, 1, 2, 12, 13, 60};
    unsigned audio_position = 0;
    for (unsigned i = 0; i < sizeof(positions) / sizeof(positions[0]); ++i) {
        /* Independent gray endpoint oracle: BT.709 limited-range neutral YUV. */
        memset(pixels, i & 1 ? 255 : 0, sizeof(pixels));
        int64_t pts;
        CHECK(gk_timestamp(positions[i], 60, 1, &pts));
        AVFrame *frame;
        CHECK(gk_media_prepare(media, &image, VA_INVALID_ID, &frame) == 0);
        CHECK(gk_media_submit(media, frame, pts) == 0);
        if (audio) {
            unsigned end = positions[i] * 800;
            float samples[960 * 2] = {};
            while (audio_position < end) {
                unsigned n = end - audio_position < 960 ? end - audio_position : 960;
                CHECK(gk_media_audio(media, 0, samples, n,
                                     (int64_t)audio_position * GK_NS / 48000) == 0);
                audio_position += n;
            }
        }
    }
    if (audio) {
        float samples[800 * 2] = {};
        CHECK(gk_media_audio(media, 0, samples, 800, GK_NS) == 0);
    }
    int64_t end;
    CHECK(gk_timestamp(61, 60, 1, &end));
    if (!audio)
        end = 2 * GK_NS; /* Final static interval has no next captured frame. */
    struct gk_media_stats stats;
    CHECK(gk_media_close(&media, end, &stats) == 0);
    CHECK(stats.submitted == 6 && stats.packets == 6);
    CHECK(stats.sink.accepted == stats.sink.written);
    gk_gpu_close(&gpu);
}

static void inspect(const char *path, bool audio, bool matroska)
{
    AVFormatContext *input = nullptr;
    CHECK(avformat_open_input(&input, path, nullptr, nullptr) == 0);
    CHECK(avformat_find_stream_info(input, nullptr) == 0);
    CHECK(input->nb_streams == (audio ? 2U : 1U));
    AVStream *stream = input->streams[0];
    CHECK(stream->codecpar->codec_id == AV_CODEC_ID_AV1);
    CHECK(stream->codecpar->color_trc == AVCOL_TRC_IEC61966_2_1);
    CHECK(stream->codecpar->color_space == AVCOL_SPC_BT709);
    CHECK(stream->codecpar->color_range == AVCOL_RANGE_MPEG);
    const AVCodec *decoder = avcodec_find_decoder_by_name("libdav1d");
    CHECK(decoder != nullptr);
    AVCodecContext *ctx = avcodec_alloc_context3(decoder);
    CHECK(ctx != nullptr);
    CHECK(avcodec_parameters_to_context(ctx, stream->codecpar) == 0);
    ctx->thread_count = 1;
    CHECK(avcodec_open2(ctx, decoder, nullptr) == 0);
    AVCodecContext *audio_ctx = nullptr;
    if (audio) {
        AVStream *as = input->streams[1];
        const AVCodec *ad = avcodec_find_decoder(as->codecpar->codec_id);
        CHECK(ad != nullptr);
        audio_ctx = avcodec_alloc_context3(ad);
        CHECK(audio_ctx != nullptr);
        CHECK(avcodec_parameters_to_context(audio_ctx, as->codecpar) == 0);
        CHECK(avcodec_open2(audio_ctx, ad, nullptr) == 0);
    }
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    unsigned count = 0;
    unsigned samples = 0;
    int64_t last_end = 0;
    static constexpr int64_t pts_us[] = {0, 16667, 33333, 200000, 216667, 1000000};
    while (av_read_frame(input, packet) >= 0) {
        if (packet->stream_index == 0) {
            CHECK(count < 6);
            int64_t expected = matroska ? ((pts_us[count] + 500) / 1000) * 1000 : pts_us[count];
            CHECK(av_rescale_q(packet->pts, stream->time_base, (AVRational){1, 1000000}) ==
                  expected);
            last_end = av_rescale_q(packet->pts + packet->duration, stream->time_base,
                                    (AVRational){1, 1000000});
            int64_t expected_end = count < 5 ? pts_us[count + 1] : audio ? 1016667 : 2000000;
            if (matroska)
                expected_end = ((expected_end + 500) / 1000) * 1000;
            CHECK(last_end == expected_end);
            CHECK(avcodec_send_packet(ctx, packet) == 0);
            CHECK(avcodec_receive_frame(ctx, frame) == 0);
            CHECK(frame->width == 64 && frame->height == 48);
            CHECK(frame->data[0][0] == (count & 1 ? 235 : 16));
            CHECK(frame->data[1][0] == 128 && frame->data[2][0] == 128);
            av_frame_unref(frame);
            ++count;
        } else if (audio_ctx && packet->stream_index == 1) {
            CHECK(avcodec_send_packet(audio_ctx, packet) == 0);
            int result;
            while ((result = avcodec_receive_frame(audio_ctx, frame)) >= 0) {
                samples += (unsigned)frame->nb_samples;
                av_frame_unref(frame);
            }
            CHECK(result == AVERROR(EAGAIN));
        }
        av_packet_unref(packet);
    }
    CHECK(count == 6);
    if (audio_ctx) {
        CHECK(avcodec_send_packet(audio_ctx, nullptr) == 0);
        while (avcodec_receive_frame(audio_ctx, frame) >= 0) {
            samples += (unsigned)frame->nb_samples;
            av_frame_unref(frame);
        }
        CHECK(samples == 48800);
    }
    CHECK(llabs(last_end - (audio ? 1016667 : 2000000)) <= (matroska ? 1000 : 0));
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&ctx);
    avcodec_free_context(&audio_ctx);
    avformat_close_input(&input);
}

static void drift(void)
{
    char path[] = "/tmp/gpukey-drift-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0 && close(fd) == 0 && unlink(path) == 0);
    struct gk_gpu gpu;
    CHECK(gk_gpu_open(&gpu, GK_SOFTWARE, nullptr) == 0);
    struct gk_video_format format = {.width = 64,
                                     .height = 48,
                                     .pixel_format = AV_PIX_FMT_BGRA,
                                     .transfer = AVCOL_TRC_IEC61966_2_1};
    struct gk_media_options options = {
        .output = path,
        .bits = 8,
        .surfaces = 4,
        .qp = 32,
        .fps_num = 60,
        .fps_den = 1,
        .audio_count = 1,
        .audio = &stereo,
        .sink = {.slab_size = 65536, .slabs = 8, .stall_ms = 5000},
    };
    struct gk_media *m;
    CHECK(gk_media_open(&m, &gpu, &format, &options) == 0);
    uint8_t pixels[64 * 48 * 4] = {};
    struct gk_image image = {.data = pixels, .size = sizeof(pixels), .stride = 64 * 4};
    AVFrame *video;
    CHECK(gk_media_prepare(m, &image, VA_INVALID_ID, &video) == 0);
    float samples[960 * 2] = {};
    for (unsigned block = 0; block < 250; ++block) {
        /* A clock running 1000 ppm slower: exactly 240000 input samples span
         * 5.005 s on the video timeline. The reference length is arithmetic,
         * independent of the resampler and packet count. */
        int64_t pts = (int64_t)block * 20020000;
        if (block % 50 == 0)
            CHECK(gk_media_submit(m, video, pts) == 0);
        CHECK(gk_media_audio(m, 0, samples, 960, pts) == 0);
    }
    struct gk_media_stats stats;
    CHECK(gk_media_close(&m, 5005000000, &stats) == 0);
    CHECK(stats.audio_samples == 240000 && stats.audio_trimmed == 0);
    CHECK(stats.audio_silence < 240); /* Real drift correction, not just end padding. */
    gk_gpu_close(&gpu);
    AVFormatContext *input = nullptr;
    CHECK(avformat_open_input(&input, path, nullptr, nullptr) == 0);
    CHECK(avformat_find_stream_info(input, nullptr) == 0);
    AVCodecContext *decoder = avcodec_alloc_context3(avcodec_find_decoder(AV_CODEC_ID_PCM_F32LE));
    CHECK(decoder != nullptr);
    CHECK(avcodec_parameters_to_context(decoder, input->streams[1]->codecpar) == 0);
    CHECK(avcodec_open2(decoder, nullptr, nullptr) == 0);
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    unsigned count = 0;
    while (av_read_frame(input, packet) >= 0) {
        if (packet->stream_index == 1) {
            CHECK(avcodec_send_packet(decoder, packet) == 0);
            CHECK(avcodec_receive_frame(decoder, frame) == 0);
            count += (unsigned)frame->nb_samples;
            av_frame_unref(frame);
        }
        av_packet_unref(packet);
    }
    CHECK(count == 240240);
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&decoder);
    avformat_close_input(&input);
    CHECK(unlink(path) == 0);
}

static void missing_audio(void)
{
    char path[] = "/tmp/gpukey-missing-audio-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0 && close(fd) == 0 && unlink(path) == 0);
    struct gk_gpu gpu;
    CHECK(gk_gpu_open(&gpu, GK_SOFTWARE, nullptr) == 0);
    struct gk_video_format format = {.width = 64,
                                     .height = 48,
                                     .pixel_format = AV_PIX_FMT_BGRA,
                                     .transfer = AVCOL_TRC_IEC61966_2_1};
    struct gk_media_options options = {
        .output = path,
        .bits = 8,
        .surfaces = 4,
        .qp = 32,
        .fps_num = 60,
        .fps_den = 1,
        .audio_count = 1,
        .audio = &stereo,
        .sink = {.slab_size = 4096, .slabs = 8, .stall_ms = 5000},
    };
    struct gk_media *m;
    CHECK(gk_media_open(&m, &gpu, &format, &options) == 0);
    uint8_t pixels[64 * 48 * 4] = {};
    struct gk_image image = {.data = pixels, .size = sizeof(pixels), .stride = 64 * 4};
    AVFrame *frame;
    CHECK(gk_media_prepare(m, &image, VA_INVALID_ID, &frame) == 0);
    CHECK(gk_media_submit(m, frame, 0) == 0);
    CHECK(gk_media_close(&m, GK_NS, nullptr) == -ENODATA);
    gk_gpu_close(&gpu);
    CHECK(unlink(path) == 0);
}

static void quantized_stop(bool matroska, int64_t last_pts)
{
    char path[] = "/tmp/gpukey-quantized-stop-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0 && close(fd) == 0 && unlink(path) == 0);
    struct gk_gpu gpu;
    CHECK(gk_gpu_open(&gpu, GK_SOFTWARE, nullptr) == 0);
    struct gk_video_format format = {.width = 64,
                                     .height = 48,
                                     .pixel_format = AV_PIX_FMT_BGRA,
                                     .transfer = AVCOL_TRC_IEC61966_2_1};
    struct gk_media_options options = {.output = path,
                                       .matroska = matroska,
                                       .bits = 8,
                                       .surfaces = 4,
                                       .qp = 32,
                                       .fps_num = 60,
                                       .fps_den = 1,
                                       .sink = {.slab_size = 4096, .slabs = 8, .stall_ms = 5000}};
    struct gk_media *m;
    CHECK(gk_media_open(&m, &gpu, &format, &options) == 0);
    uint8_t pixels[64 * 48 * 4] = {};
    struct gk_image image = {.data = pixels, .size = sizeof(pixels), .stride = 64 * 4};
    AVFrame *frame;
    CHECK(gk_media_prepare(m, &image, VA_INVALID_ID, &frame) == 0);
    CHECK(gk_media_submit(m, frame, 0) == 0);
    CHECK(gk_media_repeat(m, 500000000) == 0);
    CHECK(gk_media_repeat(m, last_pts) == 0);
    struct gk_media_stats stats;
    CHECK(gk_media_close(&m, GK_NS, &stats) == 0);
    CHECK(stats.packets == 3 && stats.video_boundary_omitted == 1);
    gk_gpu_close(&gpu);
    AVFormatContext *input = nullptr;
    CHECK(avformat_open_input(&input, path, nullptr, nullptr) == 0);
    CHECK(avformat_find_stream_info(input, nullptr) == 0);
    AVPacket *packet = av_packet_alloc();
    unsigned packets = 0;
    while (av_read_frame(input, packet) >= 0) {
        CHECK(packets < 2);
        CHECK(av_rescale_q(packet->pts, input->streams[0]->time_base, (AVRational){1, 1000}) ==
              (int64_t)packets * 500);
        CHECK(av_rescale_q(packet->duration, input->streams[0]->time_base, (AVRational){1, 1000}) ==
              500);
        ++packets;
        av_packet_unref(packet);
    }
    CHECK(packets == 2);
    av_packet_free(&packet);
    avformat_close_input(&input);
    CHECK(unlink(path) == 0);
}

static void expected_admission_audio(float expected[static 1920 * 2])
{
    /* The recorder deliberately enables resampling even at equal rates, so a
     * step has a filtered transition. Use an independent resampler instance
     * with the same declared format/rate and identical input/flush ordering. */
    AVChannelLayout layout = AV_CHANNEL_LAYOUT_STEREO;
    SwrContext *reference = nullptr;
    CHECK(swr_alloc_set_opts2(&reference, &layout, AV_SAMPLE_FMT_FLT, 48000, &layout,
                              AV_SAMPLE_FMT_FLT, 48000, 0, nullptr) == 0);
    CHECK(av_opt_set_int(reference, "flags", SWR_FLAG_RESAMPLE, 0) == 0 &&
          swr_init(reference) == 0);
    float samples[960 * 2], output[2048 * 2];
    unsigned count = 0;
    for (unsigned part = 0; part < 3; ++part) {
        for (unsigned i = 0; i < 960 * 2; ++i)
            samples[i] = part ? 0.25f : 0.125f;
        const uint8_t *input[] = {(const uint8_t *)samples};
        uint8_t *result[] = {(uint8_t *)output};
        int n =
            swr_convert(reference, result, 2048, part < 2 ? input : nullptr, part < 2 ? 960 : 0);
        CHECK(n >= 0 && count + (unsigned)n <= 1920);
        memcpy(expected + count * 2, output, (size_t)n * 2 * sizeof(float));
        count += (unsigned)n;
    }
    uint8_t *result[] = {(uint8_t *)output};
    CHECK(swr_convert(reference, result, 2048, nullptr, 0) == 0 && count == 1920);
    swr_free(&reference);
    CHECK(expected[0] == 0.125f && expected[1919 * 2] == 0.25f);
}

static void inspect_admission_picture(const AVFrame *decoded)
{
    CHECK(decoded->width == 64 && decoded->height == 48 && decoded->format == AV_PIX_FMT_YUV420P);
    CHECK(abs(decoded->data[0][0] - 16) <= 1 && abs(decoded->data[1][0] - 128) <= 1 &&
          abs(decoded->data[2][0] - 128) <= 1);
}

static void admission_failure(bool matroska, bool audio, bool bad_timestamp)
{
    char path[] = "/tmp/gpukey-admission-failure-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0 && close(fd) == 0 && unlink(path) == 0);
    struct gk_gpu gpu;
    CHECK(gk_gpu_open(&gpu, GK_SOFTWARE, nullptr) == 0);
    struct gk_video_format format = {.width = 64,
                                     .height = 48,
                                     .pixel_format = AV_PIX_FMT_BGRA,
                                     .transfer = AVCOL_TRC_IEC61966_2_1};
    struct gk_media_options options = {.output = path,
                                       .matroska = matroska,
                                       .bits = 8,
                                       .qp = 32,
                                       .fps_num = 60,
                                       .fps_den = 1,
                                       .audio = &stereo,
                                       .audio_count = audio ? 1U : 0U,
                                       .audio_codec = GK_AUDIO_CODEC_PCM,
                                       .sink = {.slab_size = 4096, .slabs = 8, .stall_ms = 5000}};
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
    if (audio)
        CHECK(gk_media_audio(media, 0, samples, 960, 0) == 0);
    int expected_error;
    if (bad_timestamp) {
        CHECK(gk_media_prepare(media, &image, VA_INVALID_ID, &frame) == 0);
        expected_error = -ERANGE;
        CHECK(gk_media_submit(media, frame, 0) == expected_error);
    } else {
        image.size = 1;
        expected_error = -EINVAL;
        CHECK(gk_media_prepare(media, &image, VA_INVALID_ID, &frame) == expected_error);
    }
    CHECK(gk_media_error(media) == expected_error);
    /* Simulate PCM admitted by capture before shutdown and handed off afterward.
     * Different values distinguish this tail from fabricated silence. */
    for (unsigned i = 0; i < 960 * 2; ++i)
        samples[i] = 0.25f;
    if (audio)
        CHECK(gk_media_audio(media, 0, samples, 960, 20'000'000) == 0);
    struct gk_media_stats stats;
    CHECK(gk_media_close(&media, 40'000'000, &stats) == expected_error);
    CHECK(stats.submitted == 1 && stats.packets == 1);
    gk_gpu_close(&gpu);

    AVFormatContext *input = nullptr;
    CHECK(avformat_open_input(&input, path, nullptr, nullptr) == 0);
    CHECK(avformat_find_stream_info(input, nullptr) == 0);
    CHECK(input->nb_streams == (audio ? 2U : 1U));
    const AVCodec *decoder = avcodec_find_decoder_by_name("libdav1d");
    AVCodecContext *context = avcodec_alloc_context3(decoder);
    CHECK(context && avcodec_parameters_to_context(context, input->streams[0]->codecpar) == 0);
    context->thread_count = 1;
    CHECK(avcodec_open2(context, decoder, nullptr) == 0);
    AVPacket *packet = av_packet_alloc();
    AVFrame *decoded = av_frame_alloc();
    CHECK(packet && decoded);
    float expected_audio[1920 * 2];
    if (audio)
        expected_admission_audio(expected_audio);
    unsigned video_packets = 0, video_frames = 0, audio_values = 0;
    int ret;
    while ((ret = av_read_frame(input, packet)) >= 0) {
        if (packet->stream_index == 0) {
            CHECK(++video_packets == 1 && packet->pts == 0);
            CHECK(av_rescale_q(packet->duration, input->streams[0]->time_base,
                               (AVRational){1, 1000}) == 40);
            CHECK(avcodec_send_packet(context, packet) == 0);
            int decoded_result;
            while ((decoded_result = avcodec_receive_frame(context, decoded)) == 0) {
                inspect_admission_picture(decoded);
                ++video_frames;
                av_frame_unref(decoded);
            }
            CHECK(decoded_result == AVERROR(EAGAIN));
        } else {
            CHECK(audio && packet->stream_index == 1 && packet->size % 8 == 0);
            CHECK(input->streams[1]->codecpar->codec_id == AV_CODEC_ID_PCM_F32LE);
            for (int i = 0; i < packet->size; i += 4) {
                CHECK(audio_values < 1920 * 2);
                uint32_t expected_bits;
                static_assert(sizeof(float) == sizeof(expected_bits));
                memcpy(&expected_bits, &expected_audio[audio_values], sizeof(expected_bits));
                CHECK(AV_RL32(packet->data + i) == expected_bits);
                ++audio_values;
            }
        }
        av_packet_unref(packet);
    }
    CHECK(ret == AVERROR_EOF && video_packets == 1 && audio_values == (audio ? 1920U * 2 : 0U));
    CHECK(avcodec_send_packet(context, nullptr) == 0);
    while ((ret = avcodec_receive_frame(context, decoded)) == 0) {
        inspect_admission_picture(decoded);
        ++video_frames;
        av_frame_unref(decoded);
    }
    CHECK(ret == AVERROR_EOF && video_frames == 1);
    av_frame_free(&decoded);
    av_packet_free(&packet);
    avcodec_free_context(&context);
    avformat_close_input(&input);
    CHECK(unlink(path) == 0);
}

int main(void)
{
    chroma_registration(64, 48);
    chroma_registration(63, 47);
    for (unsigned mode = 0; mode < 4; ++mode) {
        char path[] = "/tmp/gpukey-media-XXXXXX";
        int fd = mkstemp(path);
        CHECK(fd >= 0);
        CHECK(close(fd) == 0 && unlink(path) == 0);
        record(path, mode == 1 || mode == 2, mode >= 2);
        inspect(path, mode == 1 || mode == 2, mode >= 2);
        CHECK(unlink(path) == 0);
    }
    drift();
    missing_audio();
    quantized_stop(true, 999900000);
    quantized_stop(true, 999999900);
    quantized_stop(false, 999999900);
    for (unsigned mode = 0; mode < 8; ++mode)
        admission_failure((mode & 1) != 0, (mode & 2) != 0, (mode & 4) != 0);
    puts("media: independent AV1 decode, color endpoints, VFR gaps, final duration and audio mux "
         "passed");
    return 0;
}

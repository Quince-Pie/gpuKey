#include "media.h"

#include <errno.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <math.h>
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

/* Different rates and a deliberately reversed surround input order. Expected
 * tones are keyed by speaker identity, not by the implementation's permutation. */
static const struct gk_audio_track tracks[] = {
    {.format = {.rate = 44100, .channels = 1, .position = {AV_CHAN_FRONT_CENTER}},
     .title = "Microphone"},
    {.format = GK_AUDIO_STEREO, .title = "Desktop"},
    {.format = {.rate = 96000,
                .channels = 6,
                .position = {AV_CHAN_BACK_RIGHT, AV_CHAN_BACK_LEFT, AV_CHAN_LOW_FREQUENCY,
                             AV_CHAN_FRONT_CENTER, AV_CHAN_FRONT_RIGHT, AV_CHAN_FRONT_LEFT}},
     .title = "Surround"},
    {.format = {.rate = 8000,
                .channels = 8,
                .position = {AV_CHAN_FRONT_LEFT, AV_CHAN_FRONT_RIGHT, AV_CHAN_FRONT_CENTER,
                             AV_CHAN_LOW_FREQUENCY, AV_CHAN_BACK_LEFT, AV_CHAN_BACK_RIGHT,
                             AV_CHAN_SIDE_LEFT, AV_CHAN_SIDE_RIGHT}},
     .title = "Eight channels"},
    {.format = {.rate = 48000,
                .channels = 16,
                .discrete = true,
                .auxiliary = {8, 9, 10, 11, 12, 13, 14, 15, 0, 1, 2, 3, 4, 5, 6, 7}},
     .title = "Discrete interface"},
};
enum { TRACKS = sizeof(tracks) / sizeof(tracks[0]), END_SAMPLES = 96624 };
static constexpr int64_t endpoint = 2013000000;
static constexpr double tau = 6.2831853071795864769;

static double frequency(unsigned track, enum AVChannel channel)
{
    if (track == TRACKS - 1)
        return 1000.0 + channel * 17;
    return channel == AV_CHAN_LOW_FREQUENCY ? 40.0 : (double)(track + 1) * 200 + channel * 19;
}

static void record_tracks(const char *path, bool matroska, bool pcm)
{
    unsigned first = pcm ? TRACKS - 1 : 0;
    unsigned count = pcm ? 1 : matroska ? TRACKS : TRACKS - 1;
    struct gk_gpu gpu;
    CHECK(gk_gpu_open(&gpu, GK_SOFTWARE, nullptr) == 0);
    struct gk_video_format format = {.width = 64,
                                     .height = 48,
                                     .pixel_format = AV_PIX_FMT_BGRA,
                                     .transfer = AVCOL_TRC_IEC61966_2_1};
    struct gk_media_options options = {.output = path,
                                       .bits = 8,
                                       .surfaces = 4,
                                       .qp = 32,
                                       .fps_num = 60,
                                       .fps_den = 1,
                                       .audio = tracks + first,
                                       .audio_codec =
                                           pcm ? GK_AUDIO_CODEC_PCM : GK_AUDIO_CODEC_AUTO,
                                       .audio_count = count,
                                       .matroska = matroska,
                                       .sink = {.slab_size = 65536, .slabs = 8, .stall_ms = 5000}};
    struct gk_media *media;
    CHECK(gk_media_open(&media, &gpu, &format, &options) == 0);
    uint8_t pixels[64 * 48 * 4] = {};
    struct gk_image image = {.data = pixels, .size = sizeof(pixels), .stride = 64 * 4};
    AVFrame *video;
    CHECK(gk_media_prepare(media, &image, VA_INVALID_ID, &video) == 0);
    unsigned positions[TRACKS] = {};
    float samples[1920 * 16];
    for (unsigned block = 0; block < 101; ++block) {
        int64_t pts = (int64_t)block * 20000000;
        if (block % 10 == 0)
            CHECK(gk_media_submit(media, video, pts) == 0);
        int64_t end = pts + 20000000;
        if (end > endpoint)
            end = endpoint;
        /* Alternate visitation to catch shared per-track clocks or scratch. */
        for (unsigned t = 0; t < count; ++t) {
            unsigned track = first + (block & 1 ? count - t - 1 : t);
            const struct gk_audio_format *f = &tracks[track].format;
            unsigned goal = (unsigned)(end * f->rate / GK_NS);
            unsigned n = goal - positions[track];
            CHECK(n <= 1920);
            for (unsigned i = 0; i < n; ++i)
                for (unsigned ch = 0; ch < f->channels; ++ch)
                    samples[i * f->channels + ch] =
                        (float)(0.15 *
                                sin(tau *
                                    frequency(track, f->discrete ? (enum AVChannel)f->auxiliary[ch]
                                                                 : f->position[ch]) *
                                    (positions[track] + i) / f->rate));
            CHECK(gk_media_audio(media, track - first, samples, n,
                                 (int64_t)positions[track] * GK_NS / f->rate) == 0);
            positions[track] = goal;
        }
    }
    struct gk_media_stats stats;
    CHECK(gk_media_close(&media, endpoint, &stats) == 0);
    CHECK(stats.submitted == 11 && stats.packets == 11);
    CHECK(stats.sink.accepted == stats.sink.written);
    gk_gpu_close(&gpu);
}

struct oracle {
    AVCodecContext *decoder;
    unsigned samples, observed;
    double sine[16], cosine[16];
};

static void inspect_samples(struct oracle *o, unsigned track, const AVFrame *frame)
{
    unsigned channels = tracks[track].format.channels;
    CHECK(frame->sample_rate == 48000 && frame->ch_layout.nb_channels == (int)channels);
    CHECK(frame->format == AV_SAMPLE_FMT_FLT || frame->format == AV_SAMPLE_FMT_FLTP);
    for (unsigned i = 0; i < (unsigned)frame->nb_samples; ++i) {
        unsigned index = o->samples++;
        if (index < 4800 || index > 90000)
            continue;
        ++o->observed;
        for (unsigned ch = 0; ch < channels; ++ch) {
            enum AVChannel speaker = av_channel_layout_channel_from_index(&frame->ch_layout, ch);
            /* FFmpeg's MP4 writer omits chnl for one-channel PCM. This fixture
             * has exactly one mono channel; no permutation can be ambiguous. */
            if (channels == 1 && speaker == AV_CHAN_NONE)
                speaker = AV_CHAN_FRONT_CENTER;
            if (tracks[track].format.discrete)
                speaker = (enum AVChannel)tracks[track].format.auxiliary[ch];
            CHECK(speaker != AV_CHAN_NONE);
            float sample = frame->format == AV_SAMPLE_FMT_FLT
                               ? ((const float *)frame->data[0])[i * channels + ch]
                               : ((const float *)frame->extended_data[ch])[i];
            double phase = tau * frequency(track, speaker) * index / 48000;
            o->sine[ch] += (double)sample * sin(phase);
            o->cosine[ch] += (double)sample * cos(phase);
        }
    }
}

static void inspect_tracks(const char *path, bool matroska, bool pcm)
{
    AVFormatContext *input = nullptr;
    CHECK(avformat_open_input(&input, path, nullptr, nullptr) == 0);
    CHECK(avformat_find_stream_info(input, nullptr) == 0);
    unsigned first = pcm ? TRACKS - 1 : 0;
    unsigned count = pcm ? 1 : matroska ? TRACKS : TRACKS - 1;
    CHECK(input->nb_streams == count + 1);
    struct oracle oracle[TRACKS] = {};
    for (unsigned i = 0; i < count; ++i) {
        AVStream *stream = input->streams[i + 1];
        CHECK(stream->codecpar->ch_layout.nb_channels == (int)tracks[first + i].format.channels);
        AVDictionaryEntry *title = av_dict_get(stream->metadata, "title", nullptr, 0);
        if (!title)
            title = av_dict_get(stream->metadata, "handler_name", nullptr, 0);
        CHECK(title && !strcmp(title->value, tracks[first + i].title));
        if (tracks[first + i].format.discrete) {
            AVDictionaryEntry *labels =
                av_dict_get(stream->metadata, "GPUKEY_CHANNELS", nullptr, 0);
            CHECK(labels && !strcmp(labels->value, "AUX8,AUX9,AUX10,AUX11,AUX12,AUX13,AUX14,AUX15,"
                                                   "AUX0,AUX1,AUX2,AUX3,AUX4,AUX5,AUX6,AUX7"));
            if (!pcm) {
                CHECK(stream->codecpar->extradata_size >= 21 + 16);
                CHECK(stream->codecpar->extradata[18] == 255);
                for (unsigned ch = 0; ch < 16; ++ch)
                    CHECK(stream->codecpar->extradata[21 + ch] == ch);
            } else
                CHECK(stream->codecpar->codec_id == AV_CODEC_ID_PCM_F32LE);
        }
        oracle[i].decoder =
            avcodec_alloc_context3(avcodec_find_decoder(stream->codecpar->codec_id));
        CHECK(oracle[i].decoder);
        CHECK(avcodec_parameters_to_context(oracle[i].decoder, stream->codecpar) == 0);
        CHECK(avcodec_open2(oracle[i].decoder, nullptr, nullptr) == 0);
    }
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    CHECK(packet && frame);
    while (av_read_frame(input, packet) >= 0) {
        if (packet->stream_index > 0) {
            unsigned i = (unsigned)packet->stream_index - 1;
            CHECK(i < count);
            CHECK(avcodec_send_packet(oracle[i].decoder, packet) == 0);
            int ret;
            while ((ret = avcodec_receive_frame(oracle[i].decoder, frame)) >= 0) {
                inspect_samples(&oracle[i], first + i, frame);
                av_frame_unref(frame);
            }
            CHECK(ret == AVERROR(EAGAIN));
        }
        av_packet_unref(packet);
    }
    for (unsigned i = 0; i < count; ++i) {
        CHECK(avcodec_send_packet(oracle[i].decoder, nullptr) == 0);
        while (avcodec_receive_frame(oracle[i].decoder, frame) >= 0) {
            inspect_samples(&oracle[i], first + i, frame);
            av_frame_unref(frame);
        }
        CHECK(oracle[i].samples == END_SAMPLES);
        CHECK(oracle[i].observed > 80000);
        for (unsigned ch = 0; ch < tracks[first + i].format.channels; ++ch) {
            double amplitude =
                2 * hypot(oracle[i].sine[ch], oracle[i].cosine[ch]) / oracle[i].observed;
            CHECK(amplitude > 0.11 && amplitude < 0.19);
        }
        avcodec_free_context(&oracle[i].decoder);
    }
    av_frame_free(&frame);
    av_packet_free(&packet);
    avformat_close_input(&input);
}

int main(void)
{
    av_log_set_level(AV_LOG_ERROR);
    AVChannelLayout in = {}, out = {};
    struct gk_audio_format bad = GK_AUDIO_STEREO;
    bad.position[1] = bad.position[0];
    CHECK(gk_audio_layout(&bad, &in, &out) == -EINVAL);
    bad.position[1] = AV_CHAN_NONE;
    CHECK(gk_audio_layout(&bad, &in, &out) == -ENOTSUP);
    int64_t pts;
    CHECK(gk_audio_cycle_pts(GK_NS, 480, 1, 48000, 441, 44100, &pts) && pts == 980000000);
    CHECK(gk_audio_cycle_pts(GK_NS, -480, 1, 48000, 0, 48000, &pts) && pts == 1010000000);
    CHECK(!gk_audio_cycle_pts(0, 0, 1, 48000, 0, 48000, &pts));
    CHECK(!gk_audio_cycle_pts(GK_NS, 0, 1, 0, 0, 48000, &pts));
    CHECK(!gk_audio_cycle_pts(GK_NS, INT64_MAX, UINT32_MAX, 1, 0, 48000, &pts));
    CHECK(!gk_audio_cycle_pts(GK_NS, INT64_MIN, UINT32_MAX, 1, UINT64_MAX, 1, &pts));
    struct gk_audio_format auxiliary = {
        .rate = 384000, .channels = GK_AUDIO_CHANNELS, .discrete = true};
    for (unsigned i = 0; i < GK_AUDIO_CHANNELS; ++i)
        auxiliary.auxiliary[i] = (uint8_t)i;
    CHECK(gk_audio_layout(&auxiliary, &in, &out) == 0);
    CHECK(in.nb_channels == 64 && out.nb_channels == 64);
    av_channel_layout_uninit(&in);
    av_channel_layout_uninit(&out);
    auxiliary.auxiliary[1] = 0;
    CHECK(gk_audio_layout(&auxiliary, &in, &out) == -EINVAL);
    for (unsigned mkv = 0; mkv < 3; ++mkv) {
        char path[] = "/tmp/gpukey-audio-XXXXXX";
        int fd = mkstemp(path);
        CHECK(fd >= 0 && close(fd) == 0 && unlink(path) == 0);
        record_tracks(path, mkv != 0, mkv == 2);
        inspect_tracks(path, mkv != 0, mkv == 2);
        CHECK(unlink(path) == 0);
    }
    puts("audio: independent tracks, native rates, mono/stereo/surround/16 AUX, Opus/PCM, "
         "channel identity and exact endpoints pass");
    return 0;
}

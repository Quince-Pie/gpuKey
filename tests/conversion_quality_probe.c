/* Quality oracle only for gpuKey's deterministic `--test-source pattern`.
 * Reconstruct RGB by frame index, then independently evaluate nonlinear
 * BT.709 and the declared triangular chroma filter in double precision. */
#include "pattern.h"

#include <errno.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/intreadwrite.h>
#include <math.h>
#include <stdckdint.h>
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

struct result {
    double error[3], worst[3];
    uint64_t samples[3], frames, bytes;
    unsigned bits;
    bool native;
};

static int clamp(int x, int n)
{
    return x < 0 ? 0 : x >= n ? n - 1 : x;
}

static void presentation(AVFrame *frame, int width, int height, const uint32_t crop[4])
{
    CHECK(frame->width == width && frame->height == height);
    /* libavcodec returns coded pictures. Container crop is a presentation
     * operation, as in ffmpeg_demux.c and mpv, and must not alter prediction. */
    CHECK(!frame->crop_top && !frame->crop_bottom && !frame->crop_left && !frame->crop_right);
    frame->crop_top = crop[0];
    frame->crop_bottom = crop[1];
    frame->crop_left = crop[2];
    frame->crop_right = crop[3];
    CHECK(av_frame_apply_cropping(frame, 0) == 0);
}

static void inspect(const AVFrame *f, struct result *result, uint8_t *rgb, double (*ideal)[3],
                    AVRational time_base)
{
    int w = f->width, h = f->height;
    bool wide = f->format == AV_PIX_FMT_YUV420P10LE;
    CHECK(wide || f->format == AV_PIX_FMT_YUV420P);
    CHECK(f->color_range == AVCOL_RANGE_MPEG && f->colorspace == AVCOL_SPC_BT709 &&
          f->color_trc == AVCOL_TRC_IEC61966_2_1);
    if (!result->bits)
        result->bits = wide ? 10U : 8U;
    CHECK(result->bits == (wide ? 10U : 8U));
    CHECK(av_rescale_q(f->pts, time_base, (AVRational){1, 60}) == (int64_t)result->frames);
    gk_test_pattern(rgb, w, h, result->frames);
    unsigned scale = wide ? 4U : 1U;
    for (size_t i = 0; i < (size_t)w * (size_t)h; ++i) {
        double r = rgb[4 * i + 2] / 255.0, g = rgb[4 * i + 1] / 255.0, b = rgb[4 * i] / 255.0;
        double y = 0.2126 * r + 0.7152 * g + 0.0722 * b;
        ideal[i][0] = scale * (16.0 + 219.0 * y);
        ideal[i][1] = scale * (128.0 + 112.0 * (b - y) / (1.0 - 0.0722));
        ideal[i][2] = scale * (128.0 + 112.0 * (r - y) / (1.0 - 0.2126));
    }
    for (unsigned p = 0; p < 3; ++p) {
        int width = p ? (w + 1) / 2 : w, height = p ? (h + 1) / 2 : h;
        for (int y = 0; y < height; ++y) {
            const uint8_t *row = f->data[p] + (ptrdiff_t)y * f->linesize[p];
            for (int x = 0; x < width; ++x) {
                double expected;
                if (!p)
                    expected = ideal[(size_t)y * (unsigned)w + (unsigned)x][0];
                else {
                    expected = 0;
                    double norm = 0, cx = 2.0 * x, cy = 2.0 * y + 0.5;
                    int from_y = result->native ? 2 * y : 2 * y - 1;
                    int to_y = result->native ? 2 * y + 1 : 2 * y + 2;
                    for (int yy = from_y; yy <= to_y; ++yy) {
                        for (int xx = 2 * x - 1; xx <= 2 * x + 1; ++xx) {
                            double weight = (1.0 - fabs(xx - cx) / 2.0) *
                                            (result->native ? 1.0 : 1.0 - fabs(yy - cy) / 2.0);
                            expected += weight * ideal[(size_t)clamp(yy, h) * (unsigned)w +
                                                       (unsigned)clamp(xx, w)][p];
                            norm += weight;
                        }
                    }
                    expected /= norm;
                }
                unsigned sample = wide ? AV_RL16(row + 2 * x) : row[x];
                double error = sample - expected;
                result->error[p] += error * error;
                if (fabs(error) > result->worst[p])
                    result->worst[p] = fabs(error);
                ++result->samples[p];
            }
        }
    }
    ++result->frames;
}

int main(int argc, char **argv)
{
    CHECK(argc == 2 || (argc == 3 && !strcmp(argv[2], "native")));
    AVFormatContext *input = nullptr;
    CHECK(avformat_open_input(&input, argv[1], nullptr, nullptr) == 0);
    CHECK(avformat_find_stream_info(input, nullptr) == 0 && input->nb_streams == 1);
    const AVCodec *codec = avcodec_find_decoder_by_name("libdav1d");
    AVCodecContext *context = avcodec_alloc_context3(codec);
    CHECK(codec && context &&
          avcodec_parameters_to_context(context, input->streams[0]->codecpar) == 0);
    context->thread_count = 1;
    context->pkt_timebase = input->streams[0]->time_base;
    CHECK(avcodec_open2(context, codec, nullptr) == 0);
    int width = context->width, height = context->height;
    uint32_t crop[4] = {};
    const AVPacketSideData *side = av_packet_side_data_get(
        input->streams[0]->codecpar->coded_side_data,
        input->streams[0]->codecpar->nb_coded_side_data, AV_PKT_DATA_FRAME_CROPPING);
    if (side) {
        CHECK(side->size == 16);
        for (unsigned i = 0; i < 4; ++i)
            crop[i] = AV_RL32(side->data + 4 * i);
        CHECK(!crop[0] && !crop[2] && crop[1] < (unsigned)height && crop[3] < (unsigned)width);
    }
    size_t pixels;
    CHECK(context->width > 0 && context->height > 0 &&
          !ckd_mul(&pixels, (size_t)context->width, (size_t)context->height));
    uint8_t *rgb = av_malloc_array(pixels, 4);
    double (*ideal)[3] = av_malloc_array(pixels, sizeof(*ideal));
    AVFrame *frame = av_frame_alloc();
    AVPacket *packet = av_packet_alloc();
    CHECK(rgb && ideal && frame && packet);
    struct result result = {.native = argc == 3};
    int read_result;
    while ((read_result = av_read_frame(input, packet)) >= 0) {
        CHECK(packet->stream_index == 0 && packet->size >= 0);
        result.bytes += (unsigned)packet->size;
        CHECK(avcodec_send_packet(context, packet) == 0);
        int ret;
        while ((ret = avcodec_receive_frame(context, frame)) == 0) {
            presentation(frame, width, height, crop);
            inspect(frame, &result, rgb, ideal, input->streams[0]->time_base);
            av_frame_unref(frame);
        }
        CHECK(ret == AVERROR(EAGAIN));
        av_packet_unref(packet);
    }
    CHECK(read_result == AVERROR_EOF && avcodec_send_packet(context, nullptr) == 0);
    int ret;
    while ((ret = avcodec_receive_frame(context, frame)) == 0) {
        presentation(frame, width, height, crop);
        inspect(frame, &result, rgb, ideal, input->streams[0]->time_base);
        av_frame_unref(frame);
    }
    CHECK(ret == AVERROR_EOF && result.frames);
    double peak = result.bits == 10 ? 1023.0 : 255.0;
    printf("{\"frames\":%llu,\"video_payload_bytes\":%llu,\"planes\":[",
           (unsigned long long)result.frames, (unsigned long long)result.bytes);
    for (unsigned p = 0; p < 3; ++p) {
        double mse = result.error[p] / (double)result.samples[p];
        printf("%s{\"mse\":%.9f,\"psnr\":", p ? "," : "", mse);
        if (mse > 0)
            printf("%.6f", 10.0 * log10(peak * peak / mse));
        else
            fputs("null", stdout);
        printf(",\"max_abs_error\":%.6f}", result.worst[p]);
    }
    puts("]}");
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&context);
    avformat_close_input(&input);
    av_free(ideal);
    av_free(rgb);
}

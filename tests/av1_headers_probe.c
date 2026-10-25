/* Public VAAPI/codec API: CQP and rate-controlled headers, both depths, metadata
 * submission and repeated keys. Driver-emitted metadata is logged separately:
 * supplying metadata does not establish driver support for preserving it. */
#include "gpu.h"

#include <errno.h>
#include <libavcodec/avcodec.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/opt.h>
#include <limits.h>
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

static void inspect(AVCodecContext *decoder, AVFrame *frame, unsigned bits, unsigned *count)
{
    int ret;
    while ((ret = avcodec_receive_frame(decoder, frame)) == 0) {
        CHECK(*count < 12 && frame->pts == (int64_t)*count);
        CHECK(frame->width == 640 && frame->height == 480);
        CHECK(frame->format == (bits == 8 ? AV_PIX_FMT_YUV420P : AV_PIX_FMT_YUV420P10LE));
        CHECK(frame->color_range == AVCOL_RANGE_MPEG && frame->colorspace == AVCOL_SPC_BT709);
        unsigned scale = bits == 8 ? 1U : 4U;
        int expected = (int)((16 + (*count % 4) * 73) * scale);
        for (unsigned p = 0; p < 3; ++p) {
            unsigned width = p ? 320 : 640, height = p ? 240 : 480;
            for (unsigned y = 0; y < height; y += 13) {
                const uint8_t *row = frame->data[p] + (size_t)y * (unsigned)frame->linesize[p];
                for (unsigned x = 0; x < width; x += 17) {
                    int value = bits == 8 ? row[x] : AV_RL16(row + 2 * x);
                    CHECK(abs(value - (p ? (int)(128 * scale) : expected)) <= (int)(4 * scale));
                }
            }
        }
        const AVFrameSideData *cll =
            av_frame_get_side_data(frame, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL);
        const AVFrameSideData *md =
            av_frame_get_side_data(frame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);
        printf("decoded %u bits=%u metadata_cll=%u metadata_mastering=%u\n", *count, bits,
               cll != nullptr, md != nullptr);
        if (cll) {
            const AVContentLightMetadata *light = (const void *)cll->data;
            CHECK(light->MaxCLL == 1000 && light->MaxFALL == 400);
        }
        ++*count;
        av_frame_unref(frame);
    }
    CHECK(ret == AVERROR(EAGAIN) || ret == AVERROR_EOF);
}

static int receive(AVCodecContext *encoder, AVCodecContext *decoder, AVPacket *packet,
                   AVFrame *decoded, FILE *file, unsigned bits, unsigned *count)
{
    int ret;
    while ((ret = avcodec_receive_packet(encoder, packet)) == 0) {
        CHECK(packet->size > 0 &&
              fwrite(packet->data, 1, (size_t)packet->size, file) == (size_t)packet->size);
        CHECK(avcodec_send_packet(decoder, packet) == 0);
        inspect(decoder, decoded, bits, count);
        av_packet_unref(packet);
    }
    CHECK(ret == AVERROR(EAGAIN) || ret == AVERROR_EOF);
    return ret;
}

static void metadata(AVFrame *cpu, unsigned flags)
{
    av_frame_remove_side_data(cpu, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL);
    av_frame_remove_side_data(cpu, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);
    AVContentLightMetadata *light = av_content_light_metadata_create_side_data(cpu);
    AVMasteringDisplayMetadata *master = av_mastering_display_metadata_create_side_data(cpu);
    CHECK(light && master);
    light->MaxCLL = 1000;
    light->MaxFALL = 400;
    for (unsigned c = 0; c < 3; ++c) {
        master->display_primaries[c][0] = (AVRational){1, 4};
        master->display_primaries[c][1] = (AVRational){1, 2};
    }
    master->white_point[0] = master->white_point[1] = (AVRational){1, 4};
    master->min_luminance = (AVRational){1, 1024};
    master->max_luminance = (AVRational){1000, 1};
    master->has_luminance = master->has_primaries = 1;
    if (!(flags & 1))
        av_frame_remove_side_data(cpu, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);
    if (!(flags & 2))
        av_frame_remove_side_data(cpu, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL);
}

static void run(const char *directory, unsigned bits, bool rate_control)
{
    char path[PATH_MAX];
    int n = snprintf(path, sizeof(path), "%s/%s-%u.packets", directory,
                     rate_control ? "qvbr" : "cqp", bits);
    CHECK(n > 0 && (size_t)n < sizeof(path));
    FILE *file = fopen(path, "wbx");
    CHECK(file);
    struct gk_gpu gpu;
    CHECK(gk_gpu_open(&gpu, GK_VAAPI, nullptr) == 0);
    AVBufferRef *frames = av_hwframe_ctx_alloc(gpu.device);
    CHECK(frames);
    enum AVPixelFormat format = bits == 8 ? AV_PIX_FMT_NV12 : AV_PIX_FMT_P010LE;
    AVHWFramesContext *pool = (void *)frames->data;
    pool->format = AV_PIX_FMT_VAAPI;
    pool->sw_format = format;
    pool->width = 640;
    pool->height = 480;
    CHECK(av_hwframe_ctx_init(frames) == 0);
    const AVCodec *codec = avcodec_find_encoder_by_name("av1_vaapi");
    AVCodecContext *encoder = avcodec_alloc_context3(codec);
    CHECK(codec && encoder);
    encoder->width = 640;
    encoder->height = 480;
    encoder->pix_fmt = AV_PIX_FMT_VAAPI;
    encoder->sw_pix_fmt = format;
    encoder->hw_frames_ctx = av_buffer_ref(frames);
    encoder->time_base = (AVRational){1, 60};
    encoder->framerate = (AVRational){60, 1};
    encoder->gop_size = 4;
    encoder->max_b_frames = 0;
    encoder->global_quality = 24;
    encoder->bit_rate = rate_control ? 2000000 : 0;
    encoder->color_primaries = AVCOL_PRI_BT709;
    encoder->color_trc = AVCOL_TRC_IEC61966_2_1;
    encoder->colorspace = AVCOL_SPC_BT709;
    encoder->color_range = AVCOL_RANGE_MPEG;
    encoder->chroma_sample_location = AVCHROMA_LOC_LEFT;
    CHECK(encoder->hw_frames_ctx && av_opt_set_int(encoder->priv_data, "async_depth", 2, 0) == 0);
    CHECK(av_opt_set(encoder->priv_data, "rc_mode", rate_control ? "QVBR" : "CQP", 0) == 0);
    CHECK(avcodec_open2(encoder, codec, nullptr) == 0);
    codec = avcodec_find_decoder_by_name("libdav1d");
    AVCodecContext *decoder = avcodec_alloc_context3(codec);
    CHECK(codec && decoder);
    decoder->thread_count = 1;
    decoder->pkt_timebase = encoder->time_base;
    CHECK(avcodec_open2(decoder, codec, nullptr) == 0);
    AVFrame *cpu = av_frame_alloc(), *hw = av_frame_alloc(), *decoded = av_frame_alloc();
    AVPacket *packet = av_packet_alloc();
    CHECK(cpu && hw && decoded && packet);
    cpu->format = format;
    cpu->width = 640;
    cpu->height = 480;
    CHECK(av_frame_get_buffer(cpu, 64) == 0);
    unsigned count = 0;
    for (unsigned i = 0; i < 12; ++i) {
        if (i % 4 == 0)
            metadata(cpu, i == 0 ? 3U : i == 4 ? 2U : 1U);
        for (unsigned p = 0; p < 2; ++p) {
            unsigned value = p ? 128 : 16 + (i % 4) * 73;
            for (unsigned y = 0; y < (p ? 240U : 480U); ++y) {
                uint8_t *row = cpu->data[p] + (size_t)y * (unsigned)cpu->linesize[p];
                for (unsigned x = 0; x < 640; ++x) {
                    if (bits == 8)
                        row[x] = (uint8_t)value;
                    else
                        AV_WL16(row + 2 * x, (uint16_t)(value << 8));
                }
            }
        }
        CHECK(av_hwframe_get_buffer(frames, hw, 0) == 0);
        CHECK(av_hwframe_transfer_data(hw, cpu, 0) == 0);
        CHECK(av_frame_copy_props(hw, cpu) == 0);
        hw->pts = i;
        CHECK(avcodec_send_frame(encoder, hw) == 0);
        CHECK(receive(encoder, decoder, packet, decoded, file, bits, &count) == AVERROR(EAGAIN));
        av_frame_unref(hw);
    }
    CHECK(avcodec_send_frame(encoder, nullptr) == 0);
    CHECK(receive(encoder, decoder, packet, decoded, file, bits, &count) == AVERROR_EOF);
    CHECK(avcodec_send_packet(decoder, nullptr) == 0);
    inspect(decoder, decoded, bits, &count);
    CHECK(count == 12 && fclose(file) == 0);
    av_packet_free(&packet);
    av_frame_free(&decoded);
    av_frame_free(&hw);
    av_frame_free(&cpu);
    avcodec_free_context(&decoder);
    avcodec_free_context(&encoder);
    av_buffer_unref(&frames);
    gk_gpu_close(&gpu);
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    for (unsigned bits = 8; bits <= 10; bits += 2) {
        run(argv[1], bits, false);
        run(argv[1], bits, true);
    }
    puts("AV1 headers: CQP/QVBR, 8/10-bit, repeated keys and independent pixels pass; emitted "
         "metadata is reported separately.");
}

#include "av1.h"

#include <errno.h>
#include <libavcodec/avcodec.h>
#include <libavutil/intreadwrite.h>
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

static void check_configuration(enum AVColorSpace matrix, enum AVColorRange range, bool still)
{
    const AVCodec *codec = avcodec_find_encoder_by_name("libaom-av1");
    AVCodecContext *context = avcodec_alloc_context3(codec);
    CHECK(codec && context);
    context->width = 64;
    context->height = 48;
    context->pix_fmt = AV_PIX_FMT_YUV420P;
    context->time_base = (AVRational){1, 60};
    context->framerate = (AVRational){60, 1};
    context->color_primaries = AVCOL_PRI_BT709;
    context->color_trc = AVCOL_TRC_IEC61966_2_1;
    context->colorspace = matrix;
    context->color_range = range;
    context->chroma_sample_location = AVCHROMA_LOC_LEFT;
    context->sample_aspect_ratio = (AVRational){1, 1};
    context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    context->thread_count = 1;
    AVDictionary *options = nullptr;
    CHECK(av_dict_set(&options, "usage", "realtime", 0) == 0);
    CHECK(av_dict_set(&options, "cpu-used", "8", 0) == 0);
    CHECK(av_dict_set(&options, "lag-in-frames", "0", 0) == 0);
    if (still)
        CHECK(av_dict_set(&options, "still-picture", "1", 0) == 0);
    CHECK(avcodec_open2(context, codec, &options) == 0 && !av_dict_count(options));
    av_dict_free(&options);
    AVFrame *frame = av_frame_alloc();
    AVPacket *packet = av_packet_alloc(), *saved = av_packet_alloc();
    AVCodecParameters *par = avcodec_parameters_alloc();
    CHECK(frame && packet && saved && par);
    frame->format = context->pix_fmt;
    frame->width = context->width;
    frame->height = context->height;
    frame->pts = 0;
    CHECK(av_frame_get_buffer(frame, 64) == 0);
    for (unsigned p = 0; p < 3; ++p)
        for (unsigned y = 0; y < (p ? 24U : 48U); ++y)
            memset(frame->data[p] + (size_t)y * (unsigned)frame->linesize[p], p ? 128 : 16,
                   p ? 32 : 64);
    CHECK(avcodec_send_frame(context, frame) == 0);
    int ret = avcodec_receive_packet(context, packet);
    if (ret == AVERROR(EAGAIN)) {
        CHECK(avcodec_send_frame(context, nullptr) == 0);
        ret = avcodec_receive_packet(context, packet);
    }
    CHECK(ret == 0 && packet->size > 0);
    CHECK(av_packet_ref(saved, packet) == 0 && av_packet_make_writable(saved) == 0);
#ifdef PARSER_FLAG_BORROWED_INPUT
    CHECK(gk_av1_parser_flags(saved) & PARSER_FLAG_BORROWED_INPUT);
    saved->data[saved->size] = 1;
    CHECK(!(gk_av1_parser_flags(saved) & PARSER_FLAG_BORROWED_INPUT));
    saved->data[saved->size] = 0;
    AVPacket short_buffer = *saved;
    AVBufferRef short_ref = *saved->buf;
    short_ref.size = (size_t)saved->size;
    short_buffer.buf = &short_ref;
    CHECK(!(gk_av1_parser_flags(&short_buffer) & PARSER_FLAG_BORROWED_INPUT));
#endif
    CHECK(avcodec_parameters_from_context(par, context) == 0);
    par->width = 63;
    par->height = 47;
    unsigned refs = (unsigned)av_buffer_get_ref_count(packet->buf);
    CHECK(gk_av1_configuration(par, packet, context->time_base, 63, 47, 8) == 0);
    CHECK(par->width == 64 && par->height == 48 && par->format == AV_PIX_FMT_YUV420P);
    const AVPacketSideData *crop = av_packet_side_data_get(
        par->coded_side_data, par->nb_coded_side_data, AV_PKT_DATA_FRAME_CROPPING);
    CHECK(crop && crop->size == 16 && AV_RL32(crop->data) == 0 && AV_RL32(crop->data + 4) == 1 &&
          AV_RL32(crop->data + 8) == 0 && AV_RL32(crop->data + 12) == 1);
    CHECK(gk_av1_configuration(par, packet, context->time_base, 64, 48, 8) == 0);
    CHECK(gk_av1_reusable_key(par, packet) == !still);
    AVPacket malformed = {.flags = AV_PKT_FLAG_KEY, .data = (uint8_t[]){0x1a, 1, 0x88}, .size = 3};
    CHECK(!gk_av1_reusable_key(par, &malformed));
    malformed.data = (uint8_t[]){0x32, 0x80};
    malformed.size = 2;
    CHECK(!gk_av1_reusable_key(par, &malformed));
    CHECK(!av_packet_side_data_get(par->coded_side_data, par->nb_coded_side_data,
                                   AV_PKT_DATA_FRAME_CROPPING));
    CHECK(gk_av1_configuration(par, packet, context->time_base, 65, 48, 8) == -EPROTO);
    CHECK(gk_av1_configuration(par, packet, context->time_base, 64, 48, 10) == -EPROTO);
    par->color_trc = AVCOL_TRC_BT709;
    CHECK(gk_av1_configuration(par, packet, context->time_base, 64, 48, 8) == -EPROTO);
    par->color_trc = AVCOL_TRC_IEC61966_2_1;
    par->color_space = matrix == AVCOL_SPC_BT709 ? AVCOL_SPC_BT2020_NCL : AVCOL_SPC_BT709;
    CHECK(gk_av1_configuration(par, packet, context->time_base, 64, 48, 8) == -EPROTO);
    par->color_space = matrix;
    par->color_range = range == AVCOL_RANGE_MPEG ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
    CHECK(gk_av1_configuration(par, packet, context->time_base, 64, 48, 8) == -EPROTO);
    par->color_range = range;
    packet->flags &= ~AV_PKT_FLAG_KEY;
    CHECK(gk_av1_configuration(par, packet, context->time_base, 64, 48, 8) == -EPROTO);
    packet->flags = saved->flags;
    CHECK(gk_av1_configuration(par, packet, (AVRational){0, 1}, 64, 48, 8) == -EPROTO);
    CHECK(packet->size == saved->size && !memcmp(packet->data, saved->data, (size_t)packet->size));
    CHECK(packet->pts == saved->pts && packet->dts == saved->dts && packet->flags == saved->flags &&
          packet->side_data_elems == saved->side_data_elems);
    for (int i = 0; i < packet->side_data_elems; ++i)
        CHECK(packet->side_data[i].type == saved->side_data[i].type &&
              packet->side_data[i].size == saved->side_data[i].size &&
              !memcmp(packet->side_data[i].data, saved->side_data[i].data,
                      packet->side_data[i].size));
    CHECK((unsigned)av_buffer_get_ref_count(packet->buf) == refs);
    avcodec_parameters_free(&par);
    av_frame_free(&frame);
    av_packet_free(&packet);
    av_packet_free(&saved);
    avcodec_free_context(&context);
}

int main(void)
{
    check_configuration(AVCOL_SPC_BT709, AVCOL_RANGE_MPEG, false);
    check_configuration(AVCOL_SPC_BT2020_NCL, AVCOL_RANGE_JPEG, false);
    check_configuration(AVCOL_SPC_BT709, AVCOL_RANGE_MPEG, true);
    puts("AV1: observed configuration, matrix/range agreement, crop metadata and unchanged packet "
         "ownership pass");
}

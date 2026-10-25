#include "av1.h"

#include <errno.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/pixdesc.h>
#include <limits.h>
#include <string.h>

struct obu {
    const uint8_t *data;
    size_t size;
    unsigned type;
};

static bool next_obu(const uint8_t **cursor, size_t *left, struct obu *out)
{
    if (!*left)
        return false;
    uint8_t header = *(*cursor)++;
    --*left;
    if ((header & 0x81) || !(header & 2))
        return false;
    out->type = (header >> 3) & 15;
    if (header & 4) {
        if (!*left || *(*cursor)++ != 0)
            return false;
        --*left;
    }
    uint64_t length = 0;
    unsigned shift;
    for (shift = 0; shift < 56; shift += 7) {
        if (!*left)
            return false;
        uint8_t byte = *(*cursor)++;
        --*left;
        length |= (uint64_t)(byte & 127) << shift;
        if (!(byte & 128))
            break;
    }
    if (shift == 56 || length > *left)
        return false;
    out->data = *cursor;
    out->size = (size_t)length;
    *cursor += out->size;
    *left -= out->size;
    return true;
}

static bool reusable_sequence(const struct obu *sequence)
{
    /* seq_profile, still_picture, reduced_still_picture_header,
     * timing_info_present_flag, initial_display_delay_present_flag,
     * operating_points_cnt_minus_1 and operating_point_idc[0]. Timing-free
     * independent keys reset reference IDs, order hints and entropy state. */
    return sequence->size >= 3 && (sequence->data[0] >> 5) <= 2 && !(sequence->data[0] & 0x1d) &&
           !sequence->data[1] && !sequence->data[2];
}

bool gk_av1_reusable_key(const AVCodecParameters *par, const AVPacket *packet)
{
    if (!par || par->codec_id != AV_CODEC_ID_AV1 || par->extradata_size <= 0 || !par->extradata ||
        !packet || packet->size <= 0 || !packet->data || !(packet->flags & AV_PKT_FLAG_KEY) ||
        (packet->flags & (AV_PKT_FLAG_CORRUPT | AV_PKT_FLAG_DISCARD)))
        return false;
    for (int i = 0; i < packet->side_data_elems; ++i) {
        const AVPacketSideData *side = &packet->side_data[i];
        if (side->type == AV_PKT_DATA_QUALITY_STATS)
            continue;
        if (side->type != AV_PKT_DATA_NEW_EXTRADATA || side->size != (size_t)par->extradata_size ||
            memcmp(side->data, par->extradata, side->size))
            return false;
    }
    const uint8_t *data = par->extradata;
    size_t left = (size_t)par->extradata_size;
    if (*data == 0x81) {
        if (left <= 4)
            return false;
        data += 4;
        left -= 4;
    }
    struct obu sequence = {}, current;
    while (left) {
        if (!next_obu(&data, &left, &current))
            return false;
        if (current.type == 1) {
            if (sequence.data || !reusable_sequence(&current))
                return false;
            sequence = current;
        } else if (current.type != 15)
            return false;
    }
    if (!sequence.data)
        return false;
    data = packet->data;
    left = (size_t)packet->size;
    bool frame = false, complete = false, delimiter = false, separate_tiles = false;
    while (left) {
        if (!next_obu(&data, &left, &current))
            return false;
        switch (current.type) {
        case 1:
            if (frame || current.size != sequence.size ||
                memcmp(current.data, sequence.data, current.size))
                return false;
            break;
        case 2:
            if (delimiter || frame || current.size)
                return false;
            delimiter = true;
            break;
        case 3:
        case 6:
            if (frame || !current.size || (current.data[0] & 0xf0) != 0x10)
                return false;
            frame = true;
            complete = current.type == 6;
            separate_tiles = current.type == 3;
            break;
        case 4:
            if (!frame || !separate_tiles || !current.size)
                return false;
            complete = true;
            break;
        case 15:
            break;
        default:
            return false; /* Metadata/redundancy and layered plans keep ordinary encoding. */
        }
    }
    return frame && complete;
}

int gk_av1_parser_flags(const AVPacket *packet)
{
    int flags = PARSER_FLAG_COMPLETE_FRAMES;
#ifdef PARSER_FLAG_BORROWED_INPUT
    static constexpr uint8_t zero[AV_INPUT_BUFFER_PADDING_SIZE] = {};
    if (packet->buf && packet->buf->data && packet->data && packet->size > 0) {
        uintptr_t base = (uintptr_t)packet->buf->data, data = (uintptr_t)packet->data;
        if (data >= base && data - base <= packet->buf->size) {
            size_t available = packet->buf->size - (size_t)(data - base);
            if ((size_t)packet->size <= available &&
                sizeof(zero) <= available - (size_t)packet->size &&
                !memcmp(packet->data + packet->size, zero, sizeof(zero)))
                flags |= PARSER_FLAG_BORROWED_INPUT;
        }
    }
#else
    (void)packet;
#endif
    return flags;
}

static int configuration_bytes(AVCodecParameters *par, const uint8_t *data, size_t size)
{
    if (!data || !size || size > INT_MAX)
        return -EPROTO;
    if (size == (size_t)par->extradata_size && !memcmp(data, par->extradata, size))
        return 0;
    uint8_t *copy = av_mallocz(size + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!copy)
        return -ENOMEM;
    memcpy(copy, data, size);
    av_free(par->extradata);
    par->extradata = copy;
    par->extradata_size = (int)size;
    return 0;
}

int gk_av1_configuration(AVCodecParameters *par, const AVPacket *first, AVRational time_base,
                         int width, int height, unsigned bits)
{
    if (par->codec_id != AV_CODEC_ID_AV1 || width <= 0 || height <= 0 || time_base.num <= 0 ||
        time_base.den <= 0 || (bits != 8 && bits != 10) || first->size <= 0 || !first->data ||
        !(first->flags & AV_PKT_FLAG_KEY) ||
        (first->flags & (AV_PKT_FLAG_CORRUPT | AV_PKT_FLAG_DISCARD)))
        return -EPROTO;
    const enum AVColorTransferCharacteristic transfer = par->color_trc;
    const enum AVColorSpace matrix = par->color_space;
    const enum AVColorRange range = par->color_range;
    AVBSFContext *bsf = nullptr;
    AVPacket *probe = av_packet_clone(first);
    AVCodecParserContext *parser = av_parser_init(AV_CODEC_ID_AV1);
    AVCodecContext *context = avcodec_alloc_context3(nullptr);
    const AVBitStreamFilter *filter = av_bsf_get_by_name("extract_extradata");
    int ret = -ENOMEM;
    if (!probe || !parser || !context)
        goto done;
    if (!filter) {
        ret = -ENOSYS;
        goto done;
    }
    if ((ret = av_bsf_alloc(filter, &bsf)) < 0)
        goto done;
    bsf->time_base_in = time_base;
    if ((ret = avcodec_parameters_copy(bsf->par_in, par)) < 0 || (ret = av_bsf_init(bsf)) < 0 ||
        (ret = av_bsf_send_packet(bsf, probe)) < 0 || (ret = av_bsf_receive_packet(bsf, probe)) < 0)
        goto done;
    size_t size = 0;
    const uint8_t *extra = av_packet_get_side_data(probe, AV_PKT_DATA_NEW_EXTRADATA, &size);
    if (extra && (ret = configuration_bytes(par, extra, size)) < 0)
        goto done;
    if (!par->extradata_size) {
        ret = -EPROTO;
        goto done;
    }
    if ((ret = avcodec_parameters_to_context(context, par)) < 0)
        goto done;
    parser->flags |= gk_av1_parser_flags(first);
    uint8_t *data;
    int length;
    ret = av_parser_parse2(parser, context, &data, &length, first->data, first->size, first->pts,
                           first->dts, 0);
    const AVPixFmtDescriptor *format = av_pix_fmt_desc_get(parser->format);
    if (ret != first->size || length != first->size || parser->key_frame != 1 ||
        parser->width < width || parser->height < height || !format ||
        format->comp[0].depth != (int)bits || format->log2_chroma_w != 1 ||
        format->log2_chroma_h != 1 || context->color_primaries != AVCOL_PRI_BT709 ||
        context->colorspace != matrix || context->color_range != range ||
        context->color_trc != transfer ||
        (context->sample_aspect_ratio.num &&
         av_cmp_q(context->sample_aspect_ratio, (AVRational){1, 1}))) {
        ret = -EPROTO;
        goto done;
    }
    par->width = parser->width;
    par->height = parser->height;
    par->format = parser->format;
    par->bits_per_raw_sample = (int)bits;
    par->profile = context->profile;
    par->level = context->level;
    par->color_primaries = context->color_primaries;
    par->color_trc = context->color_trc;
    par->color_space = context->colorspace;
    par->color_range = context->color_range;
    par->sample_aspect_ratio = (AVRational){1, 1};
    av_packet_side_data_remove(par->coded_side_data, &par->nb_coded_side_data,
                               AV_PKT_DATA_FRAME_CROPPING);
    if (par->width != width || par->height != height) {
        AVPacketSideData *crop = av_packet_side_data_new(
            &par->coded_side_data, &par->nb_coded_side_data, AV_PKT_DATA_FRAME_CROPPING, 16, 0);
        if (!crop) {
            ret = -ENOMEM;
            goto done;
        }
        AV_WL32(crop->data, 0);
        AV_WL32(crop->data + 4, (uint32_t)(par->height - height));
        AV_WL32(crop->data + 8, 0);
        AV_WL32(crop->data + 12, (uint32_t)(par->width - width));
    }
    ret = 0;
done:
    av_packet_free(&probe);
    av_bsf_free(&bsf);
    av_parser_close(parser);
    avcodec_free_context(&context);
    return ret;
}

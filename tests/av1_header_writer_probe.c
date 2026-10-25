/* Build with tools/check-av1-header-writer.sh and the audited FFmpeg sources.
 * The unchanged fragment writer is the serialization/offset control. dav1d
 * independently decodes the transformed hidden/show-existing frame stream. */
#include "libavcodec/cbs_av1.h"
#include "libavcodec/put_bits.h"
#include <errno.h>
#include <libavcodec/avcodec.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/opt.h>
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

struct offsets {
    int qindex, loop, cdef, cdef_size;
    unsigned events;
};

static void trace(void *opaque, PutBitContext *bits, int length, const char *name,
                  const int *subscripts, int64_t value)
{
    (void)subscripts;
    (void)value;
    struct offsets *o = opaque;
    int position = put_bits_count(bits);
    ++o->events;
    if (!strcmp(name, "base_q_idx"))
        o->qindex = position - length;
    else if (!strcmp(name, "loop_filter_level[0]"))
        o->loop = position - length;
    else if (!strcmp(name, "cdef_damping_minus_3"))
        o->cdef = position - length;
    else if (!strcmp(name, "cdef_uv_sec_strength[i]"))
        o->cdef_size = position - o->cdef;
}

struct writer {
    CodedBitstreamContext *control, *direct;
    struct offsets control_offsets, direct_offsets;
    AVCodecContext *decoder;
    AVFrame *frame;
    uint8_t *pending;
    size_t capacity, size;
    unsigned displayed;
};

static void check_decoded(struct writer *w)
{
    int ret;
    while ((ret = avcodec_receive_frame(w->decoder, w->frame)) == 0) {
        const AVFrame *frame = w->frame;
        CHECK(w->displayed < 12 && frame->width == 640 && frame->height == 480);
        bool wide = frame->format == AV_PIX_FMT_YUV420P10LE;
        CHECK(wide || frame->format == AV_PIX_FMT_YUV420P);
        int expected = (16 + (int)(w->displayed % 4) * 73) * (wide ? 4 : 1);
        for (unsigned y = 0; y < 480; y += 13) {
            const uint8_t *row = frame->data[0] + (size_t)y * (unsigned)frame->linesize[0];
            for (unsigned x = 0; x < 640; x += 17) {
                int actual = wide ? AV_RL16(row + 2 * x) : row[x];
                CHECK(abs(actual - expected) <= (wide ? 16 : 4));
            }
        }
        ++w->displayed;
        av_frame_unref(w->frame);
    }
    CHECK(ret == AVERROR(EAGAIN) || ret == AVERROR_EOF);
}

static void emit(struct writer *w)
{
    AVPacket *packet = av_packet_alloc();
    CHECK(packet && w->size && w->size <= INT_MAX && av_new_packet(packet, (int)w->size) == 0);
    memcpy(packet->data, w->pending, w->size);
    CHECK(avcodec_send_packet(w->decoder, packet) == 0);
    av_packet_free(&packet);
    w->size = 0;
    check_decoded(w);
}

static void write_unit(struct writer *w, AV1RawOBU *control, AV1RawOBU *direct)
{
    CodedBitstreamFragment fragment = {};
    w->control_offsets = w->direct_offsets =
        (struct offsets){.qindex = -1, .loop = -1, .cdef = -1, .cdef_size = -1};
    CHECK(ff_cbs_insert_unit_content(&fragment, -1, control->header.obu_type, control, nullptr) ==
          0);
    CHECK(ff_cbs_write_fragment_data(w->control, &fragment) == 0);
    size_t capacity;
    CHECK(!ckd_add(&capacity, fragment.data_size, 80));
    uint8_t *output = av_malloc(capacity + 32);
    CHECK(output);
    memset(output, 0xa5, capacity + 32);
    size_t bits = 0;
    CHECK(ff_cbs_av1_write_obu_buffer(w->direct, direct, output + 16, capacity, &bits) == 0);
    CHECK(bits == fragment.data_size * 8 - fragment.data_bit_padding);
    CHECK(!memcmp(output + 16, fragment.data, fragment.data_size));
    CHECK(w->control_offsets.qindex == w->direct_offsets.qindex &&
          w->control_offsets.loop == w->direct_offsets.loop &&
          w->control_offsets.cdef == w->direct_offsets.cdef &&
          w->control_offsets.cdef_size == w->direct_offsets.cdef_size &&
          w->control_offsets.events == w->direct_offsets.events);
    for (size_t i = 0; i < 16; ++i)
        CHECK(output[i] == 0xa5 && output[capacity + 16 + i] == 0xa5);
    CHECK(fragment.data_size <= w->capacity - w->size);
    memcpy(w->pending + w->size, output + 16, fragment.data_size);
    w->size += fragment.data_size;
    const CodedBitstreamAV1Context *a = w->control->priv_data, *b = w->direct->priv_data;
    CHECK(a->seen_frame_header == b->seen_frame_header && a->order_hint == b->order_hint);
    for (unsigned i = 0; i < AV1_NUM_REF_FRAMES; ++i) {
        CHECK(a->ref[i].valid == b->ref[i].valid && a->ref[i].order_hint == b->ref[i].order_hint &&
              a->ref[i].frame_width == b->ref[i].frame_width &&
              a->ref[i].frame_height == b->ref[i].frame_height);
    }
    CHECK(!b->frame_header_ref && !b->frame_header && !b->frame_header_size);
    CHECK(!w->direct->write_buffer && !w->direct->write_buffer_size);
    /* A direct call must not retain pointers into its borrowed raw OBU. */
    memset(direct, 0x3c, sizeof(*direct));
    ff_cbs_fragment_free(&fragment);
    av_free(output);
}

static void run(const CodedBitstreamFragment *input, int size_length, bool hide)
{
    struct writer w = {};
    CHECK(ff_cbs_init(&w.control, AV_CODEC_ID_AV1, nullptr) == 0 &&
          ff_cbs_init(&w.direct, AV_CODEC_ID_AV1, nullptr) == 0);
    CHECK(av_opt_set_int(w.control->priv_data, "fixed_obu_size_length", size_length, 0) == 0 &&
          av_opt_set_int(w.direct->priv_data, "fixed_obu_size_length", size_length, 0) == 0);
    w.control->trace_enable = w.direct->trace_enable = 1;
    w.control->trace_write_callback = w.direct->trace_write_callback = trace;
    w.control->trace_context = &w.control_offsets;
    w.direct->trace_context = &w.direct_offsets;
    ((CodedBitstreamAV1Context *)w.direct->priv_data)->omit_redundant_header_copy = 1;
    const AVCodec *codec = avcodec_find_decoder_by_name("libdav1d");
    w.decoder = avcodec_alloc_context3(codec);
    w.frame = av_frame_alloc();
    CHECK(codec && w.decoder && w.frame);
    w.decoder->thread_count = 1;
    CHECK(avcodec_open2(w.decoder, codec, nullptr) == 0);
    size_t extra;
    CHECK(!ckd_mul(&extra, (size_t)input->nb_units, 32) &&
          !ckd_add(&w.capacity, input->data_size, extra) &&
          !ckd_add(&w.capacity, w.capacity, 1024));
    w.pending = av_malloc(w.capacity);
    AV1RawOBU *a = av_calloc((size_t)input->nb_units + 2, sizeof(*a));
    AV1RawOBU *b = av_calloc((size_t)input->nb_units + 2, sizeof(*b));
    CHECK(w.pending && a && b);
    bool hidden = false;
    int pending_slot = -1;
    for (int i = 0; i < input->nb_units; ++i) {
        CHECK(input->units[i].content);
        a[i] = b[i] = *(const AV1RawOBU *)input->units[i].content;
        AV1RawFrameHeader *frame = a[i].header.obu_type == AV1_OBU_FRAME ? &a[i].obu.frame.header
                                   : a[i].header.obu_type == AV1_OBU_FRAME_HEADER
                                       ? &a[i].obu.frame_header
                                       : nullptr;
        if (hide && !hidden && frame && frame->frame_type == AV1_FRAME_INTER && frame->show_frame &&
            frame->refresh_frame_flags) {
            frame->show_frame = 0;
            frame->showable_frame = 1;
            for (unsigned slot = 0; slot < AV1_NUM_REF_FRAMES; ++slot)
                if (frame->refresh_frame_flags & (1U << slot)) {
                    pending_slot = (int)slot;
                    break;
                }
            b[i] = a[i];
            hidden = true;
        }
        write_unit(&w, &a[i], &b[i]);
        CodedBitstreamAV1Context *state = w.control->priv_data;
        bool complete =
            !state->seen_frame_header &&
            (a[i].header.obu_type == AV1_OBU_FRAME || a[i].header.obu_type == AV1_OBU_TILE_GROUP ||
             (a[i].header.obu_type == AV1_OBU_FRAME_HEADER &&
              a[i].obu.frame_header.show_existing_frame));
        if (complete) {
            if (pending_slot >= 0) {
                const AV1ReferenceFrameState *ref = &state->ref[pending_slot];
                AV1RawOBU *repeat = &a[input->nb_units];
                repeat->header.obu_type = AV1_OBU_FRAME_HEADER;
                repeat->obu.frame_header = (AV1RawFrameHeader){
                    .show_existing_frame = 1,
                    .frame_to_show_map_idx = (uint8_t)pending_slot,
                    .frame_type = AV1_FRAME_INTER,
                    .frame_width_minus_1 = (uint16_t)(ref->frame_width - 1),
                    .frame_height_minus_1 = (uint16_t)(ref->frame_height - 1),
                    .render_width_minus_1 = (uint16_t)(ref->render_width - 1),
                    .render_height_minus_1 = (uint16_t)(ref->render_height - 1)};
                b[input->nb_units] = *repeat;
                write_unit(&w, repeat, &b[input->nb_units]);
                pending_slot = -1;
            }
            emit(&w);
        }
    }
    CHECK(!w.size && (!hide || hidden) && pending_slot == -1);
    CHECK(avcodec_send_packet(w.decoder, nullptr) == 0);
    check_decoded(&w);
    CHECK(w.displayed == 12);
    /* Contexts close before their borrowed raw unit storage is reclaimed. */
    ff_cbs_close(&w.direct);
    ff_cbs_close(&w.control);
    avcodec_free_context(&w.decoder);
    av_frame_free(&w.frame);
    av_free(a);
    av_free(b);
    av_free(w.pending);
}

static void failures(const AV1RawOBU *sequence)
{
    for (size_t capacity = 0; capacity < 64; ++capacity) {
        CodedBitstreamContext *ctx = nullptr;
        CHECK(ff_cbs_init(&ctx, AV_CODEC_ID_AV1, nullptr) == 0);
        CHECK(av_opt_set_int(ctx->priv_data, "fixed_obu_size_length", 4, 0) == 0);
        AV1RawOBU raw = *sequence;
        uint8_t output[96];
        memset(output, 0xa5, sizeof(output));
        size_t bits = 0;
        int ret = ff_cbs_av1_write_obu_buffer(ctx, &raw, output + 16, capacity, &bits);
        CHECK((ret == 0 && bits <= capacity * 8) || (ret < 0 && bits == 0));
        for (size_t i = 0; i < 16; ++i)
            CHECK(output[i] == 0xa5);
        for (size_t i = capacity + 16; i < sizeof(output); ++i)
            CHECK(output[i] == 0xa5);
        if (capacity >= 48)
            CHECK(ret == 0);
        /* A failed write is terminal; verify cleanup without retrying state. */
        ff_cbs_close(&ctx);
    }
    CodedBitstreamContext *ctx = nullptr;
    CHECK(ff_cbs_init(&ctx, AV_CODEC_ID_AV1, nullptr) == 0);
    AV1RawOBU invalid = *sequence;
    invalid.obu.sequence_header.seq_profile = 8;
    uint8_t output[1024];
    size_t bits = 0;
    CHECK(ff_cbs_av1_write_obu_buffer(ctx, &invalid, output, sizeof(output), &bits) < 0);
    CHECK(bits == 0);
    ff_cbs_close(&ctx);
    CHECK(ff_cbs_init(&ctx, AV_CODEC_ID_AV1, nullptr) == 0);
    AV1RawOBU raw = *sequence;
    /* No codec/decoder workers exist yet. Limit only the new sequence owner,
     * then restore the audited libavutil default before any later operation. */
    av_max_alloc(sizeof(AV1RawOBU) - 1);
    int ret = ff_cbs_av1_write_obu_buffer(ctx, &raw, output, sizeof(output), &bits);
    av_max_alloc(INT_MAX);
    CHECK(ret == AVERROR(ENOMEM) && bits == 0);
    ff_cbs_close(&ctx);
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    FILE *file = fopen(argv[1], "rb");
    CHECK(file && fseek(file, 0, SEEK_END) == 0);
    long length = ftell(file);
    CHECK(length > 0 && length < INT_MAX / 2 && fseek(file, 0, SEEK_SET) == 0);
    uint8_t *data = av_mallocz((size_t)length + AV_INPUT_BUFFER_PADDING_SIZE);
    CHECK(data && fread(data, 1, (size_t)length, file) == (size_t)length && fclose(file) == 0);
    CodedBitstreamContext *reader = nullptr;
    CodedBitstreamFragment input = {};
    CHECK(ff_cbs_init(&reader, AV_CODEC_ID_AV1, nullptr) == 0 &&
          ff_cbs_read(reader, &input, nullptr, data, (size_t)length) == 0);
    bool checked_failures = false;
    for (int i = 0; i < input.nb_units; ++i) {
        if (input.units[i].type == AV1_OBU_SEQUENCE_HEADER) {
            failures(input.units[i].content);
            checked_failures = true;
            break;
        }
    }
    CHECK(checked_failures);
    for (int size_length = 0; size_length <= 8; size_length += 2) {
        run(&input, size_length, false);
        run(&input, size_length, true);
    }
    ff_cbs_fragment_free(&input);
    ff_cbs_close(&reader);
    av_free(data);
    puts("Header writers: exact bytes/offsets, bounds, reference state and independent "
         "hidden/show-existing decode pass.");
}

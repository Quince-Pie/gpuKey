#include "replay.h"
#include "av1.h"

#include <errno.h>
#include <libavcodec/avcodec.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <linux/futex.h>
#include <pthread.h>
#include <stdbit.h>
#include <stdckdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/syscall.h>
#include <tracy/TracyC.h>
#include <unistd.h>

enum { BLOCK_PACKETS = 256, KEY = 1U << 31, SHARED_KEY = KEY | 1U, NO_BLOCK = UINT32_MAX };
static constexpr uint64_t no_position = UINT64_MAX;
static constexpr int64_t opus_preroll_ns = 80000000;

/* All other AVPacket fields are derived from stream configuration, or are
 * irrelevant encoder statistics. Offsets preserve CPU sub-buffer ownership. */
/* Only identical video keys share storage. These counters belong to the media
 * owner; the exporter reads only the immutable buffer through a pinned block.
 * A live-history reference and an export-owned reference have different costs. */
struct shared_payload {
    AVBufferRef *buffer;
    uint64_t live_refs, owned_refs;
};
static_assert(sizeof(struct shared_payload) == 24);
struct packet {
    union {
        AVBufferRef *buffer;
        struct shared_payload *shared;
    };
    int64_t pts;
    uint32_t offset, size, stream_flags;
    uint16_t duration, skip_end; /* Audio sample counts; video duration is derived. */
};
static_assert(sizeof(struct packet) == 32);
struct block {
    _Atomic uint32_t refs;
    uint32_t used, next;
    uint64_t keys[BLOCK_PACKETS / 64];
    int64_t max_end_ns;
    size_t bytes;
    struct packet packets[BLOCK_PACKETS];
};
struct snapshot {
    struct block **blocks;
    uint32_t *ids;
    size_t count, packets;
    char *path;
    struct gk_replay_result result;
};
enum work_state { IDLE, QUEUED, RUNNING, DONE, STOP };
static_assert(ATOMIC_INT_LOCK_FREE == 2 && sizeof(_Atomic uint32_t) == 4 &&
              alignof(_Atomic uint32_t) >= 4);
struct gk_replay {
    const AVFormatContext *format;
    struct gk_replay_options options;
    struct block **blocks;
    uint32_t *available;
    uint32_t capacity, unused, available_count, head, tail, spare;
    uint64_t keep_key, next_key, last_key;
    int64_t last_key_ns;
    int64_t block_started_ns;
    int64_t *last_pts, *ends;
    size_t live_bytes, owned_bytes;
    struct gk_ring released;
    int wake;
#ifdef GK_REPLAY_EVENTFD
    int request;
#endif
    pthread_t worker;
    bool thread_started, sealed, has_opus;
    _Atomic uint32_t state; /* Low two bits: job state; STOP is monotonic until close. */
    struct snapshot job;
    struct gk_replay_stats stats;
};

/* KEY only occurs on video stream zero. Its otherwise unused low bits tag a
 * shared owner without reducing the supported audio stream-index domain. */
static unsigned packet_stream(const struct packet *p)
{
    return p->stream_flags & KEY ? 0 : p->stream_flags;
}
static AVBufferRef *packet_buffer(const struct packet *p)
{
    return p->stream_flags == SHARED_KEY ? p->shared->buffer : p->buffer;
}

/* This is a bounded allocation charge, not an exact process RSS measurement. */
static size_t charge(const AVBufferRef *buffer)
{
    return buffer->size + sizeof(AVBufferRef) + 128;
}
static size_t shared_charge(const struct shared_payload *shared)
{
    return charge(shared->buffer) + sizeof(*shared);
}

static void release_live(struct gk_replay *r, const struct block *b)
{
    r->live_bytes -= b->bytes;
    for (unsigned word = 0; word < BLOCK_PACKETS / 64; ++word) {
        uint64_t keys = b->keys[word];
        while (keys) {
            const struct packet *p = &b->packets[word * 64 + stdc_trailing_zeros(keys)];
            if (p->stream_flags == SHARED_KEY && --p->shared->live_refs == 0)
                r->live_bytes -= shared_charge(p->shared);
            keys &= keys - 1;
        }
    }
}

static uint64_t position(uint32_t id, unsigned slot)
{
    return (uint64_t)id << 32 | slot;
}
static struct packet *at(const struct gk_replay *r, uint64_t pos)
{
    return &r->blocks[(uint32_t)(pos >> 32)]->packets[(uint32_t)pos];
}
static int64_t time_ns(const struct gk_replay *r, const struct packet *p)
{
    return av_rescale_q(p->pts, r->format->streams[packet_stream(p)]->time_base,
                        (AVRational){1, 1000000000});
}

/* Only the owner reads links/key masks. Export views contain sealed blocks and
 * never traverse mutable live-list bookkeeping. */
static uint64_t following_key(const struct gk_replay *r, uint64_t after)
{
    uint32_t id = (uint32_t)(after >> 32), slot = (uint32_t)after + 1;
    while (id != NO_BLOCK) {
        const struct block *b = r->blocks[id];
        for (unsigned word = slot / 64; word < BLOCK_PACKETS / 64; ++word) {
            uint64_t bits = b->keys[word];
            if (slot % 64)
                bits &= UINT64_MAX << (slot % 64);
            if (bits)
                return position(id, word * 64 + stdc_trailing_zeros(bits));
            slot = (word + 1) * 64;
        }
        id = b->next;
        slot = 0;
    }
    return no_position;
}

static void free_payloads(struct gk_replay *r, uint32_t id)
{
    struct block *b = r->blocks[id];
    r->owned_bytes -= b->bytes;
    for (unsigned i = 0; i < b->used; ++i) {
        struct packet *p = &b->packets[i];
        if (p->stream_flags == SHARED_KEY) {
            struct shared_payload *shared = p->shared;
            if (--shared->owned_refs == 0) {
                r->owned_bytes -= shared_charge(shared);
                av_buffer_unref(&shared->buffer);
                free_sized(shared, sizeof(*shared));
            }
        } else
            av_buffer_unref(&p->buffer);
    }
    if (r->spare == NO_BLOCK) {
        *b = (struct block){};
        atomic_init(&b->refs, 0);
        r->spare = id;
        r->owned_bytes += sizeof(*b);
    } else {
        free_sized(b, sizeof(*b));
        r->blocks[id] = nullptr;
        r->available[r->available_count++] = id;
    }
}
static void recycle(struct gk_replay *r)
{
    uint64_t id;
    while (gk_ring_pop(&r->released, &id))
        free_payloads(r, (uint32_t)id);
}
static void owner_release(struct gk_replay *r, uint32_t id)
{
    if (atomic_fetch_sub_explicit(&r->blocks[id]->refs, 1, memory_order_acq_rel) == 1)
        free_payloads(r, id);
}

static int new_block(struct gk_replay *r)
{
    if (r->live_bytes > r->options.bytes - sizeof(struct block))
        return -ENOBUFS;
    uint32_t id;
    if (r->spare != NO_BLOCK) {
        id = r->spare;
        r->spare = NO_BLOCK;
    } else {
        if (r->available_count)
            id = r->available[--r->available_count];
        else if (r->unused < r->capacity)
            id = r->unused++;
        else
            return -ENOBUFS;
        r->blocks[id] = calloc(1, sizeof(struct block));
        if (!r->blocks[id]) {
            r->available[r->available_count++] = id;
            return -ENOMEM;
        }
        r->owned_bytes += sizeof(struct block);
    }
    struct block *b = r->blocks[id];
    atomic_init(&b->refs, 1);
    b->next = NO_BLOCK;
    b->max_end_ns = INT64_MIN;
    b->bytes = sizeof(*b);
    if (r->tail != NO_BLOCK)
        r->blocks[r->tail]->next = id;
    else
        r->head = id;
    r->tail = id;
    r->sealed = false;
    r->live_bytes += sizeof(*b);
    return 0;
}

static void trim(struct gk_replay *r)
{
    if (r->keep_key == no_position)
        return;
    int64_t end = r->ends[0];
    for (unsigned i = 1; i < r->format->nb_streams; ++i)
        if (r->ends[i] < end)
            end = r->ends[i];
    if (end < r->options.duration_ns)
        return;
    int64_t cut = end - r->options.duration_ns;
    for (;;) {
        if (r->next_key == no_position || time_ns(r, at(r, r->next_key)) > cut)
            break;
        r->keep_key = r->next_key;
        r->next_key = following_key(r, r->keep_key);
    }
    int64_t floor = time_ns(r, at(r, r->keep_key)) - (r->has_opus ? opus_preroll_ns : 0);
    uint32_t key_block = (uint32_t)(r->keep_key >> 32);
    while (r->head != key_block && r->head != r->tail &&
           (r->blocks[r->head]->max_end_ns < floor ||
            (!r->has_opus && r->blocks[r->head]->max_end_ns == floor))) {
        uint32_t id = r->head;
        r->head = r->blocks[id]->next;
        release_live(r, r->blocks[id]);
        owner_release(r, id);
    }
}

/* A conservative prefix check complements the full AV1 parser: a temporal unit
 * whose first frame is hidden/inter/show-existing cannot become a replay start
 * merely because a later displayed frame was reported as a keyframe. */
static int obu_prefix(const uint8_t *data, size_t size, bool *reduced, bool *sequence,
                      bool require_frame)
{
    size_t at_byte = 0;
    while (at_byte < size) {
        uint8_t header = data[at_byte++];
        if (header & 0x81 || !(header & 2))
            return -EPROTO;
        unsigned type = (header >> 3) & 15;
        if (header & 4) {
            if (at_byte == size || data[at_byte++] != 0)
                return -ENOTSUP; /* This recording plan is a single base layer. */
        }
        uint64_t length = 0;
        unsigned shift;
        for (shift = 0; shift < 56; shift += 7) {
            if (at_byte == size)
                return -EPROTO;
            uint8_t byte = data[at_byte++];
            length |= (uint64_t)(byte & 127) << shift;
            if (!(byte & 128))
                break;
        }
        if (shift == 56 || length > size - at_byte)
            return -EPROTO;
        if (type == 1) {
            if (!length)
                return -EPROTO;
            *reduced = (data[at_byte] & 8) != 0;
            *sequence = true;
        }
        if (type == 3 || type == 6) {
            if (!require_frame)
                return -EPROTO;
            return *sequence && length && (*reduced || (data[at_byte] & 0xf0) == 0x10) ? 0
                                                                                       : -ENOTSUP;
        }
        if (type == 4 || type == 7 || type == 8)
            return -ENOTSUP;
        at_byte += (size_t)length;
    }
    return require_frame ? -EPROTO : 0;
}

static int verified_key(const AVCodecParameters *par, const AVPacket *pkt)
{
    bool reduced = false, sequence = false;
    if (par->extradata_size) {
        unsigned skip = par->extradata[0] == 0x81 ? 4 : 0;
        if (par->extradata_size < (int)skip ||
            obu_prefix(par->extradata + skip, (size_t)par->extradata_size - skip, &reduced,
                       &sequence, false) < 0)
            return -EPROTO;
    }
    int ret = obu_prefix(pkt->data, (size_t)pkt->size, &reduced, &sequence, true);
    if (ret < 0)
        return ret;
    AVCodecParserContext *parser = av_parser_init(AV_CODEC_ID_AV1);
    AVCodecContext *ctx = avcodec_alloc_context3(nullptr);
    if (!parser || !ctx) {
        av_parser_close(parser);
        avcodec_free_context(&ctx);
        return -ENOMEM;
    }
    ret = avcodec_parameters_to_context(ctx, par);
    if (ret >= 0) {
        parser->flags |= gk_av1_parser_flags(pkt);
        uint8_t *output;
        int size;
        int consumed = av_parser_parse2(parser, ctx, &output, &size, pkt->data, pkt->size, pkt->pts,
                                        pkt->dts, 0);
        ret = consumed == pkt->size && size == pkt->size && parser->key_frame == 1 ? 0 : -ENOTSUP;
    }
    av_parser_close(parser);
    avcodec_free_context(&ctx);
    return ret;
}

static int add_packet(struct gk_replay *r, AVPacket *pkt, bool cpu)
{
    if (pkt->size <= 0 || !pkt->data || pkt->stream_index < 0 ||
        (unsigned)pkt->stream_index >= r->format->nb_streams || pkt->pts == AV_NOPTS_VALUE ||
        pkt->dts != pkt->pts || pkt->duration < 0 ||
        (pkt->stream_index && pkt->duration > UINT16_MAX) ||
        (pkt->flags & (AV_PKT_FLAG_CORRUPT | AV_PKT_FLAG_DISCARD)))
        return -EPROTO;
    if (!pkt->stream_index && pkt->pts < 0)
        return -ERANGE;
    unsigned stream = (unsigned)pkt->stream_index;
    if (pkt->pts <= r->last_pts[stream])
        return -ERANGE;
    recycle(r);
    AVBufferRef *buffer = pkt->buf;
    uint32_t offset = 0;
    bool copied = r->options.copy_packets || (!cpu && !stream && r->options.copy_video) || !buffer;
    if (!copied) {
        uintptr_t base = (uintptr_t)buffer->data, ptr = (uintptr_t)pkt->data;
        copied = ptr < base || ptr - base > UINT32_MAX || ptr - base > buffer->size ||
                 (size_t)pkt->size + AV_INPUT_BUFFER_PADDING_SIZE > buffer->size - (ptr - base);
        if (!copied)
            offset = (uint32_t)(ptr - base);
    }
    if (copied) {
        buffer = av_buffer_alloc((size_t)pkt->size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!buffer)
            return -ENOMEM;
        memcpy(buffer->data, pkt->data, (size_t)pkt->size);
        memset(buffer->data + pkt->size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    }
    int ret = 0;
    bool key = !stream && (pkt->flags & AV_PKT_FLAG_KEY);
    bool share_keys = key && !r->options.unique_keys && !r->options.copy_packets;
    struct shared_payload *shared = nullptr, *created = nullptr;
    bool same_key = false;
    uint32_t shared_offset = 0;
    if (share_keys && r->last_key != no_position) {
        /* The newest key remains in live history until its successor is
         * committed. No worker can acquire a new reference independently. */
        const struct packet *previous = at(r, r->last_key);
        AVBufferRef *previous_buffer = packet_buffer(previous);
        same_key = previous->size == (unsigned)pkt->size &&
                   (previous_buffer->data + previous->offset == buffer->data + offset ||
                    !memcmp(previous_buffer->data + previous->offset, buffer->data + offset,
                            previous->size));
        if (same_key && previous->stream_flags == SHARED_KEY &&
            previous_buffer->size <= buffer->size) {
            shared = previous->shared;
            shared_offset = previous->offset;
        }
    }
    if (key && !same_key) {
        AVPacket probe = *pkt;
        probe.buf = buffer;
        probe.data = buffer->data + offset;
        ret = verified_key(r->format->streams[0]->codecpar, &probe);
        if (ret < 0)
            goto fail;
    } else if (!key && !stream && r->keep_key == no_position) {
        ret = -ENOTSUP;
        goto fail;
    }
    size_t side_size = 0;
    const uint8_t *skip = av_packet_get_side_data(pkt, AV_PKT_DATA_SKIP_SAMPLES, &side_size);
    if (skip && (side_size < 10 || AV_RL32(skip + 4) > UINT16_MAX)) {
        ret = -EPROTO;
        goto fail;
    }
    const uint8_t *extra = av_packet_get_side_data(pkt, AV_PKT_DATA_NEW_EXTRADATA, &side_size);
    const AVCodecParameters *par = r->format->streams[stream]->codecpar;
    if (extra &&
        (side_size != (size_t)par->extradata_size || memcmp(extra, par->extradata, side_size))) {
        ret = -ESTALE;
        goto fail;
    }
    if (par->codec_id == AV_CODEC_ID_PCM_F32LE &&
        ((uint64_t)pkt->duration * (unsigned)par->ch_layout.nb_channels * 4 !=
             (unsigned)pkt->size ||
         (skip && (AV_RL32(skip) || AV_RL32(skip + 4))))) {
        ret = -EPROTO;
        goto fail;
    }
    int64_t packet_end, end;
    if (ckd_add(&packet_end, pkt->pts, stream ? pkt->duration : 0) ||
        (end = av_rescale_q(packet_end, r->format->streams[stream]->time_base,
                            (AVRational){1, 1000000000})) == INT64_MIN ||
        buffer->size > SIZE_MAX - sizeof(AVBufferRef) - 128 - sizeof(struct shared_payload)) {
        ret = -EOVERFLOW;
        goto fail;
    }
    /* The encoder requests a RAP every two seconds and replay permits at most
     * a two-second static-repeat interval. A larger gap violates that plan;
     * fail explicitly instead of retaining an unbounded dependency history. */
    if (!stream && r->keep_key != no_position && end - r->last_key_ns > 4 * GK_NS) {
        ret = -ESTALE;
        goto fail;
    }
    if (!stream)
        for (unsigned i = 1; i < r->format->nb_streams; ++i)
            if (end > GK_NS && r->ends[i] < end - GK_NS) {
                ret = -ENODATA;
                goto fail;
            }
    int64_t start = end;
    if (stream) {
        /* The validated audio clock is 1/48000: a tick is exactly 62500/3 ns.
         * When duration is divisible by three, subtracting the integer span
         * preserves nearest rounding for positive and negative PTS alike.
         * Keep the original PTS-based chunk boundaries; using endpoints here
         * retained an extra PCM chunk in the fixed-input comparison. */
        if (pkt->duration % 3 == 0) {
            if (ckd_sub(&start, end, pkt->duration / 3 * 62500)) {
                ret = -EOVERFLOW;
                goto fail;
            }
        } else {
            start = av_rescale_q(pkt->pts, r->format->streams[stream]->time_base,
                                 (AVRational){1, 1000000000});
            if (start == INT64_MIN) {
                ret = -EOVERFLOW;
                goto fail;
            }
        }
    }
    size_t bytes = shared ? 0 : charge(buffer);
    size_t block_cost = r->tail == NO_BLOCK || r->sealed ||
                                r->blocks[r->tail]->used == BLOCK_PACKETS ||
                                (start >= r->block_started_ns &&
                                 (uint64_t)start - (uint64_t)r->block_started_ns >= (uint64_t)GK_NS)
                            ? sizeof(struct block)
                            : 0;
    if (bytes > r->options.bytes || block_cost > r->options.bytes - bytes ||
        r->live_bytes > r->options.bytes - bytes - block_cost) {
        ret = -ENOBUFS;
        goto fail;
    }
    /* Extra metadata is optional. Under a tight budget or allocator failure,
     * retain an ordinary reference rather than make a valid packet fail solely
     * because an optimization could not allocate its small shared owner. */
    if (share_keys && !shared &&
        sizeof(*created) <= r->options.bytes - bytes - block_cost - r->live_bytes) {
        created = malloc(sizeof(*created));
        if (created) {
            *created = (struct shared_payload){.buffer = buffer};
            bytes += sizeof(*created);
        }
    }
    if (block_cost && (ret = new_block(r)) < 0)
        goto fail;
    if (block_cost)
        r->block_started_ns = start;
    struct block *b = r->blocks[r->tail];
    unsigned slot = b->used++;
    struct packet *p = &b->packets[slot];
    *p = (struct packet){.buffer = buffer,
                         .pts = pkt->pts,
                         .offset = offset,
                         .size = (uint32_t)pkt->size,
                         .duration = stream ? (uint16_t)pkt->duration : 0,
                         .stream_flags = stream | (key ? KEY : 0),
                         .skip_end = skip ? (uint16_t)AV_RL32(skip + 4) : 0};
    if (shared || created) {
        p->stream_flags = SHARED_KEY;
        p->shared = shared ? shared : created;
        if (shared) {
            p->offset = shared_offset;
            ++r->stats.shared_keyframes;
            r->stats.shared_key_bytes += p->size;
            if (copied)
                av_buffer_unref(&buffer);
        }
        ++p->shared->live_refs;
        ++p->shared->owned_refs;
    } else
        b->bytes += bytes;
    if (!copied && !shared)
        pkt->buf = nullptr;
    if (copied)
        r->stats.copied_bytes += p->size;
    if (key) {
        b->keys[slot / 64] |= UINT64_C(1) << (slot % 64);
        if (r->keep_key == no_position)
            r->keep_key = position(r->tail, slot);
        else if (r->next_key == no_position)
            r->next_key = position(r->tail, slot);
        r->last_key_ns = end;
        r->last_key = position(r->tail, slot);
    }
    r->live_bytes += bytes;
    r->owned_bytes += bytes;
    r->last_pts[stream] = p->pts;
    if (end > b->max_end_ns)
        b->max_end_ns = end;
    r->ends[stream] = end;
    ++r->stats.packets;
    if (r->live_bytes > r->stats.peak_live_bytes)
        r->stats.peak_live_bytes = r->live_bytes;
    if (r->owned_bytes > r->stats.peak_owned_bytes)
        r->stats.peak_owned_bytes = r->owned_bytes;
    av_packet_unref(pkt);
    trim(r);
    return 0;
fail:
    free_sized(created, sizeof(*created));
    if (copied)
        av_buffer_unref(&buffer);
    return ret;
}

int gk_replay_add(struct gk_replay *r, AVPacket *packet)
{
    return add_packet(r, packet, false);
}

int gk_replay_add_cpu(struct gk_replay *r, AVPacket *packet)
{
    return add_packet(r, packet, true);
}

int gk_replay_key_ref(struct gk_replay *r, AVPacket *dst)
{
    if (dst)
        av_packet_unref(dst);
    if (r->last_key == no_position)
        return 0;
    const struct packet *key = at(r, r->last_key);
    if (key->pts != r->last_pts[0])
        return 0;
    if (!dst)
        return 1;
    dst->buf = av_buffer_ref(packet_buffer(key));
    if (!dst->buf)
        return -ENOMEM;
    dst->data = dst->buf->data + key->offset;
    dst->size = (int)key->size;
    dst->pts = dst->dts = key->pts;
    dst->flags = AV_PKT_FLAG_KEY;
    dst->stream_index = 0;
    return 1;
}

struct export_stream {
    size_t begin, count, cursor;
    int64_t start, end;
};
struct export_state {
    const struct gk_replay *replay;
    const struct snapshot *snapshot;
    uint64_t *order;
    struct export_stream *streams;
    uint32_t *heap;
    unsigned heap_count;
    size_t terminal_cursor;
    int64_t terminal_pts;
};
static const struct packet *saved_packet(const struct export_state *e, uint64_t pos)
{
    return &e->snapshot->blocks[(uint32_t)(pos >> 32)]->packets[(uint32_t)pos];
}
static const struct packet *stream_packet(const struct export_state *e, unsigned stream,
                                          size_t offset)
{
    const struct export_stream *s = &e->streams[stream];
    return saved_packet(e, e->order[s->begin + s->cursor + offset]);
}
static int64_t packet_pts(const struct export_state *e, unsigned stream, size_t offset)
{
    return !stream && e->streams[0].cursor + offset == e->terminal_cursor
               ? e->terminal_pts
               : stream_packet(e, stream, offset)->pts;
}
static bool earlier(const struct export_state *e, unsigned a, unsigned b)
{
    return av_compare_ts(packet_pts(e, a, 0) - e->streams[a].start,
                         e->replay->format->streams[a]->time_base,
                         packet_pts(e, b, 0) - e->streams[b].start,
                         e->replay->format->streams[b]->time_base) < 0;
}

static void compact_keys(struct export_state *e, const AVStream *output)
{
    struct export_stream *s = &e->streams[0];
    const AVStream *input = e->replay->format->streams[0];
    size_t end = s->cursor;
    while (end < s->count && saved_packet(e, e->order[s->begin + end])->pts < s->end)
        ++end;
    /* movenc validates against signed INT_MAX, narrower than the MP4 field. */
    int64_t span = e->replay->options.matroska ? INT64_MAX
                                               : av_rescale_q_rnd(INT_MAX, output->time_base,
                                                                  input->time_base, AV_ROUND_DOWN);
    int64_t tick = av_rescale_q_rnd(1, output->time_base, input->time_base, AV_ROUND_UP);
    if (tick < 1)
        tick = 1;
    uint64_t tested = no_position;
    bool reusable = false, dropped = false;
    size_t write = s->cursor;
    for (size_t read = s->cursor; read < end; ++read) {
        uint64_t pos = e->order[s->begin + read];
        const struct packet *p = saved_packet(e, pos);
        const struct packet *previous =
            write > s->cursor ? saved_packet(e, e->order[s->begin + write - 1]) : nullptr;
        bool same = previous && (p->stream_flags & KEY) && (previous->stream_flags & KEY) &&
                    p->size == previous->size;
        if (same) {
            const uint8_t *a = packet_buffer(p)->data + p->offset;
            const uint8_t *b = packet_buffer(previous)->data + previous->offset;
            same = a == b || !memcmp(a, b, p->size);
        }
        uint64_t previous_pos = write > s->cursor ? e->order[s->begin + write - 1] : no_position;
        if (same && tested != previous_pos) {
            AVBufferRef *payload = packet_buffer(p);
            AVPacket key = {.buf = payload,
                            .data = payload->data + p->offset,
                            .size = (int)p->size,
                            .flags = AV_PKT_FLAG_KEY};
            reusable = gk_av1_reusable_key(input->codecpar, &key);
            tested = previous_pos;
        }
        if (same && reusable) {
            int64_t next =
                read + 1 < end ? saved_packet(e, e->order[s->begin + read + 1])->pts : s->end;
            if (read + 1 < end && next - previous->pts <= span) {
                dropped = true;
                continue;
            }
            if (read + 1 == end) {
                /* Keep the final key as a one-tick terminal sample. Some
                 * consumers end a one-picture clip immediately despite a
                 * declared duration. Never move it across a different image. */
                int64_t terminal = s->end - tick;
                if (terminal > previous->pts && terminal - previous->pts <= span &&
                    av_rescale_q(terminal - s->start, input->time_base, output->time_base) >
                        av_rescale_q(previous->pts - s->start, input->time_base,
                                     output->time_base)) {
                    e->terminal_cursor = write;
                    e->terminal_pts = terminal;
                }
            }
        }
        e->order[s->begin + write++] = pos;
    }
    /* A lone independent picture otherwise reaches EOS immediately in some
     * video-only players. The spare video index permits a terminal copy without
     * copying payload storage or displacing any audio indices. */
    if (e->terminal_cursor == SIZE_MAX && (write == s->cursor + 1 || dropped)) {
        uint64_t last_pos = e->order[s->begin + write - 1];
        const struct packet *last = saved_packet(e, last_pos);
        int64_t terminal = s->end - tick;
        AVBufferRef *payload = packet_buffer(last);
        AVPacket key = {.buf = payload,
                        .data = payload->data + last->offset,
                        .size = (int)last->size,
                        .flags = last->stream_flags & KEY ? AV_PKT_FLAG_KEY : 0};
        if (terminal > last->pts && terminal - last->pts <= span &&
            av_rescale_q(terminal - s->start, input->time_base, output->time_base) >
                av_rescale_q(last->pts - s->start, input->time_base, output->time_base) &&
            gk_av1_reusable_key(input->codecpar, &key)) {
            e->order[s->begin + write] = last_pos;
            e->terminal_cursor = write++;
            e->terminal_pts = terminal;
        }
    }
    s->count = write;
}
static void heap_down(struct export_state *e, unsigned at_index)
{
    for (;;) {
        unsigned left = at_index * 2 + 1;
        if (left >= e->heap_count)
            return;
        unsigned child = left;
        if (left + 1 < e->heap_count && earlier(e, e->heap[left + 1], e->heap[left]))
            child = left + 1;
        if (!earlier(e, e->heap[child], e->heap[at_index]))
            return;
        uint32_t value = e->heap[at_index];
        e->heap[at_index] = e->heap[child];
        e->heap[child] = value;
        at_index = child;
    }
}

static int export_clip(struct gk_replay *r, struct snapshot *view)
{
    TracyCZoneN(zone, "replay.export", true);
    const unsigned tracks = r->format->nb_streams;
    struct export_state e = {.replay = r, .snapshot = view, .terminal_cursor = SIZE_MAX};
    AVFormatContext *mux = nullptr;
    AVIOContext *io = nullptr;
    AVPacket *packet = nullptr;
    struct gk_sink *sink = nullptr;
    struct gk_sink_stats sink_stats = {};
    bool video_written = false;
    int ret = -ENOMEM;
    e.streams = calloc(tracks, sizeof(*e.streams));
    e.heap = calloc(tracks, sizeof(*e.heap));
    size_t indices;
    if (ckd_add(&indices, view->packets, 1U))
        goto cleanup;
    e.order = av_malloc_array(indices, sizeof(*e.order));
    packet = av_packet_alloc();
    if (!e.streams || !e.heap || !e.order || !packet)
        goto cleanup;
    ret = avformat_alloc_output_context2(&mux, nullptr, r->options.matroska ? "matroska" : "mp4",
                                         nullptr);
    if (ret < 0)
        goto cleanup;
    for (unsigned i = 0; i < tracks; ++i) {
        AVStream *out = avformat_new_stream(mux, nullptr);
        const AVStream *in = r->format->streams[i];
        if (!out) {
            ret = -ENOMEM;
            goto cleanup;
        }
        if ((ret = avcodec_parameters_copy(out->codecpar, in->codecpar)) < 0 ||
            (ret = av_dict_copy(&out->metadata, in->metadata, 0)) < 0)
            goto cleanup;
        /* Valid Opus spans vary at cuts. Do not let generic mux duration
         * guessing replace a sub-tick valid span with a full coded frame. */
        if (out->codecpar->codec_id == AV_CODEC_ID_OPUS)
            out->codecpar->frame_size = 0;
        out->time_base = in->time_base;
        out->avg_frame_rate = in->avg_frame_rate;
        out->r_frame_rate = in->r_frame_rate;
        e.streams[i].start =
            av_rescale_q(view->result.start_ns, (AVRational){1, 1000000000}, in->time_base);
        e.streams[i].end =
            av_rescale_q(view->result.end_ns, (AVRational){1, 1000000000}, in->time_base);
    }
    /* Materialize exact per-stream indices once. The merge then reads each
     * packet once and gives av_write_frame explicitly ordered DTS. */
    for (size_t b = 0; b < view->count; ++b)
        for (unsigned j = 0; j < view->blocks[b]->used; ++j)
            ++e.streams[packet_stream(&view->blocks[b]->packets[j])].count;
    size_t offset = 0;
    for (unsigned i = 0; i < tracks; ++i) {
        e.streams[i].begin = offset;
        offset += e.streams[i].count;
        if (!i)
            ++offset; /* At most one synthetic terminal index, only for video. */
        e.streams[i].count = 0;
    }
    for (size_t b = 0; b < view->count; ++b)
        for (unsigned j = 0; j < view->blocks[b]->used; ++j) {
            struct export_stream *s = &e.streams[packet_stream(&view->blocks[b]->packets[j])];
            e.order[s->begin + s->count++] = position((uint32_t)b, j);
        }
    for (unsigned i = 0; i < tracks; ++i) {
        struct export_stream *s = &e.streams[i];
        AVCodecParameters *par = mux->streams[i]->codecpar;
        int64_t threshold = s->start;
        if (par->codec_id == AV_CODEC_ID_OPUS)
            threshold -= 3840;
        while (s->cursor + 1 < s->count && stream_packet(&e, i, 1)->pts <= threshold)
            ++s->cursor;
        if (i == 0) {
            while (s->cursor < s->count && stream_packet(&e, i, 0)->pts < s->start)
                ++s->cursor;
        } else if (par->codec_id == AV_CODEC_ID_PCM_F32LE) {
            while (s->cursor < s->count &&
                   stream_packet(&e, i, 0)->pts + stream_packet(&e, i, 0)->duration <= s->start)
                ++s->cursor;
        } else if (par->codec_id != AV_CODEC_ID_OPUS) {
            ret = -ENOTSUP;
            goto cleanup;
        }
        if (s->cursor == s->count || stream_packet(&e, i, 0)->pts >= s->end) {
            ret = -ENODATA;
            goto cleanup;
        }
        if (i == 0 && !(stream_packet(&e, i, 0)->stream_flags & KEY)) {
            ret = -EPROTO;
            goto cleanup;
        }
        if (par->codec_id == AV_CODEC_ID_OPUS) {
            int64_t preskip = s->start - stream_packet(&e, i, 0)->pts;
            if (preskip < 0 || preskip > UINT16_MAX || par->extradata_size < 19 ||
                memcmp(par->extradata, "OpusHead", 8)) {
                ret = -EPROTO;
                goto cleanup;
            }
            par->initial_padding = (int)preskip;
            AV_WL16(par->extradata + 10, (uint16_t)preskip);
        }
        e.heap[e.heap_count++] = i;
    }
    for (unsigned i = e.heap_count / 2; i > 0; --i)
        heap_down(&e, i - 1);
    ret = gk_sink_open(&sink, view->path, &r->options.sink);
    if (ret < 0)
        goto cleanup;
    uint8_t *buffer = av_malloc(65536);
    if (!buffer) {
        ret = -ENOMEM;
        goto cleanup;
    }
    io = avio_alloc_context(buffer, 65536, 1, sink, nullptr, gk_sink_write, nullptr);
    if (!io) {
        av_free(buffer);
        ret = -ENOMEM;
        goto cleanup;
    }
    mux->pb = io;
    mux->flags |= AVFMT_FLAG_CUSTOM_IO;
    mux->avoid_negative_ts = AVFMT_AVOID_NEG_TS_DISABLED;
    /* The immutable snapshot already knows its complete presentation span.
     * Matroska can publish Duration even on our append-only writer. */
    mux->duration = av_rescale_q(view->result.end_ns - view->result.start_ns,
                                 (AVRational){1, 1000000000}, AV_TIME_BASE_Q);
    AVDictionary *options = nullptr;
    if (r->options.matroska) {
        av_dict_set(&options, "cluster_time_limit", "1000", 0);
        if (av_opt_find(mux->priv_data, "write_video_duration", nullptr, 0, 0))
            av_dict_set(&options, "write_video_duration", "1", 0);
    } else {
        av_dict_set(&options, "movflags", "delay_moov+default_base_moof+frag_keyframe", 0);
        av_dict_set(&options, "video_track_timescale", "1000000", 0);
        av_dict_set(&options, "frag_duration", "1000000", 0);
    }
    av_dict_set(&mux->metadata, "GPUKEY_REPLAY", "encoded snapshot; keyframe-aligned start", 0);
    av_dict_set_int(&mux->metadata, "GPUKEY_REPLAY_START_NS", view->result.start_ns, 0);
    av_dict_set_int(&mux->metadata, "GPUKEY_REPLAY_END_NS", view->result.end_ns, 0);
    av_dict_set_int(&mux->metadata, "GPUKEY_TRIGGER_NS", view->result.requested_end_ns, 0);
    ret = avformat_write_header(mux, &options);
    if (ret >= 0 && av_dict_count(options))
        ret = -ENOTSUP;
    av_dict_free(&options);
    if (ret < 0)
        goto cleanup;
    /* Compact only byte-identical independent keys whose decoded state is
     * equal. The immutable packets remain pinned by the snapshot; only its
     * existing flat index is rewritten. Audio order and payloads are untouched. */
    compact_keys(&e, mux->streams[0]);
    while (e.heap_count) {
        unsigned track = e.heap[0];
        struct export_stream *s = &e.streams[track];
        const AVStream *in = r->format->streams[track];
        AVStream *out = mux->streams[track];
        const struct packet *p = stream_packet(&e, track, 0);
        int64_t pts = packet_pts(&e, track, 0), end = pts + p->duration;
        uint32_t skip_end = p->skip_end, data_offset = 0;
        uint32_t size = p->size;
        if (!track) {
            end = s->cursor + 1 < s->count ? packet_pts(&e, track, 1) : s->end;
            if (end > s->end)
                end = s->end;
        } else {
            if (end > s->end) {
                if (out->codecpar->codec_id == AV_CODEC_ID_OPUS)
                    skip_end += (uint32_t)(end - s->end);
                end = s->end;
            }
            if (out->codecpar->codec_id == AV_CODEC_ID_PCM_F32LE) {
                uint64_t stride = (uint64_t)(unsigned)out->codecpar->ch_layout.nb_channels * 4;
                if (pts < s->start) {
                    data_offset = (uint32_t)((uint64_t)(s->start - pts) * stride);
                    pts = s->start;
                }
                uint64_t bytes = (uint64_t)(end - pts) * stride;
                if (data_offset > size || bytes > size - data_offset) {
                    ret = -EPROTO;
                    goto cleanup;
                }
                size = (uint32_t)bytes;
            }
        }
        int64_t output_pts = av_rescale_q(pts - s->start, in->time_base, out->time_base);
        int64_t output_end = av_rescale_q(end - s->start, in->time_base, out->time_base);
        if (!track && !r->options.matroska && output_end - output_pts > INT_MAX) {
            ret = -ERANGE;
            goto cleanup;
        }
        if (!track && end < s->end && output_end <= output_pts) {
            ret = -ERANGE; /* Never drop an intermediate reference-dependent packet. */
            goto cleanup;
        }
        /* PCM length and Opus discard padding preserve sub-tick audio spans.
         * Only a terminal, unrepresentable video picture may be omitted. */
        if (end > pts && (track || output_end > output_pts)) {
            AVBufferRef *payload = packet_buffer(p);
            packet->buf = av_buffer_ref(payload);
            if (!packet->buf) {
                ret = -ENOMEM;
                goto cleanup;
            }
            packet->data = payload->data + p->offset + data_offset;
            packet->size = (int)size;
            packet->pts = packet->dts = output_pts;
            packet->duration = output_end - output_pts;
            packet->stream_index = (int)track;
            packet->flags = (p->stream_flags & KEY) || track ? AV_PKT_FLAG_KEY : 0;
            if (skip_end) {
                uint8_t *side = av_packet_new_side_data(packet, AV_PKT_DATA_SKIP_SAMPLES, 10);
                if (!side) {
                    ret = -ENOMEM;
                    goto cleanup;
                }
                memset(side, 0, 10);
                AV_WL32(side + 4, skip_end);
            }
            ret = av_write_frame(mux, packet);
            av_packet_unref(packet);
            if (ret < 0)
                goto cleanup;
            video_written |= track == 0;
        }
        ++s->cursor;
        if (s->cursor == s->count || packet_pts(&e, track, 0) >= s->end)
            e.heap[0] = e.heap[--e.heap_count];
        heap_down(&e, 0);
    }
    if (!video_written) {
        ret = -ERANGE;
        goto cleanup;
    }
    ret = av_write_trailer(mux);
    avio_flush(io);
    if (ret >= 0 && io->error < 0)
        ret = io->error;
cleanup:
    av_packet_free(&packet);
    avformat_free_context(mux);
    if (io)
        av_freep(&io->buffer);
    avio_context_free(&io);
    int finished = gk_sink_close(&sink, &sink_stats);
    if (ret >= 0)
        ret = finished;
    view->result.bytes = sink_stats.written;
    av_free(e.order);
    free(e.streams);
    free(e.heap);
    TracyCZoneEnd(zone);
    return ret;
}

static void wake_worker(struct gk_replay *r)
{
#ifdef GK_REPLAY_EVENTFD
    if (gk_notify(r->request) < 0)
        abort();
#else
    /* Dedicated worker: unlike completion readiness, this need not be pollable.
     * The aligned atomic word is also the Linux futex predicate (material Q04). */
    if (syscall(SYS_futex, &r->state, (unsigned long)FUTEX_WAKE_PRIVATE, 1UL) < 0)
        abort();
#endif
}

static void *export_worker(void *opaque)
{
    struct gk_replay *r = opaque;
    for (;;) {
        uint32_t state = atomic_load_explicit(&r->state, memory_order_acquire);
        if ((state & 3U) != QUEUED) {
            if (state & STOP)
                break;
#ifdef GK_REPLAY_EVENTFD
            if (gk_wait(r->request, -1) < 0)
                abort();
            gk_drain_event(r->request);
#else
            long ret = syscall(SYS_futex, &r->state, (unsigned long)FUTEX_WAIT_PRIVATE,
                               (unsigned long)state, (void *)nullptr);
            if (ret < 0 && errno != EAGAIN && errno != EINTR)
                abort(); /* Aligned, live private word; no timeout or PI state. */
#endif
            continue;
        }
        /* RMW transitions preserve a concurrent STOP request. A queued job is
         * consumed even with STOP set; close never abandons an accepted save. */
        atomic_fetch_add_explicit(&r->state, RUNNING - QUEUED, memory_order_relaxed);
        int gate = 0;
        if (r->options.test_gate > 0) {
            int ignored = gk_notify(r->options.test_started);
            (void)ignored;
            char byte;
            ssize_t n;
            do {
                n = read(r->options.test_gate, &byte, 1);
            } while (n < 0 && errno == EINTR);
            if (n != 1)
                gate = n < 0 ? -errno : -ECANCELED;
        }
        r->job.result.error = gate < 0 ? gate : export_clip(r, &r->job);
        for (size_t i = 0; i < r->job.count; ++i)
            if (atomic_fetch_sub_explicit(&r->job.blocks[i]->refs, 1, memory_order_acq_rel) == 1) {
                bool pushed = gk_ring_push(&r->released, r->job.ids[i]);
                if (!pushed)
                    abort(); /* Queue capacity covers every possible block exactly once. */
            }
        free(r->job.blocks);
        free(r->job.ids);
        free(r->job.path);
        r->job.blocks = nullptr;
        r->job.ids = nullptr;
        r->job.path = nullptr;
        /* No job access follows this publication. The owner may immediately
         * consume the result and publish another snapshot, even if this worker
         * pauses before its completion notification. */
        atomic_fetch_add_explicit(&r->state, DONE - RUNNING, memory_order_release);
        int ignored = gk_notify(r->wake);
        (void)ignored;
    }
    return nullptr;
}

int gk_replay_open(struct gk_replay **out, const AVFormatContext *format,
                   const struct gk_replay_options *options)
{
    *out = nullptr;
    if (!format || !format->nb_streams || format->nb_streams >= KEY || options->duration_ns <= 0 ||
        options->bytes < 1048576 || options->bytes > SIZE_MAX / 2 ||
        options->bytes * 2 / sizeof(struct block) > UINT32_MAX / 4)
        return -EINVAL;
    bool has_opus = false;
    for (unsigned i = 0; i < format->nb_streams; ++i) {
        const AVStream *s = format->streams[i];
        if (s->time_base.num <= 0 || s->time_base.den <= 0 ||
            av_cmp_q(s->time_base, (AVRational){1, 1000000000}) < 0 ||
            (!i && s->codecpar->codec_id != AV_CODEC_ID_AV1) ||
            (i && (s->codecpar->sample_rate != 48000 || s->codecpar->ch_layout.nb_channels <= 0 ||
                   av_cmp_q(s->time_base, (AVRational){1, 48000}) != 0 ||
                   (s->codecpar->codec_id != AV_CODEC_ID_PCM_F32LE &&
                    (s->codecpar->codec_id != AV_CODEC_ID_OPUS || !options->matroska)))))
            return -ENOTSUP;
        has_opus |= s->codecpar->codec_id == AV_CODEC_ID_OPUS;
    }
    struct gk_replay *r = aligned_alloc(alignof(struct gk_replay), sizeof(*r));
    if (!r)
        return -ENOMEM;
    *r = (struct gk_replay){.format = format,
                            .options = *options,
                            .has_opus = has_opus,
                            .wake = -1,
                            .head = NO_BLOCK,
                            .tail = NO_BLOCK,
                            .spare = NO_BLOCK,
                            .keep_key = no_position,
                            .next_key = no_position,
                            .last_key = no_position};
#ifdef GK_REPLAY_EVENTFD
    r->request = -1;
#endif
    atomic_init(&r->state, IDLE);
    r->capacity = (uint32_t)(options->bytes * 2 / sizeof(struct block)) + 3;
    r->blocks = calloc(r->capacity, sizeof(*r->blocks));
    r->available = calloc(r->capacity, sizeof(*r->available));
    r->last_pts = calloc(format->nb_streams, sizeof(*r->last_pts));
    r->ends = calloc(format->nb_streams, sizeof(*r->ends));
    int ret = -ENOMEM;
    if (!r->blocks || !r->available || !r->last_pts || !r->ends)
        goto fail;
    for (unsigned i = 0; i < format->nb_streams; ++i)
        r->last_pts[i] = r->ends[i] = INT64_MIN;
    ret = gk_ring_init(&r->released, stdc_bit_ceil(r->capacity));
    if (ret < 0)
        goto fail;
    r->wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (r->wake < 0) {
        ret = -errno;
        goto fail;
    }
#ifdef GK_REPLAY_EVENTFD
    r->request = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (r->request < 0) {
        ret = -errno;
        goto fail;
    }
#endif
    ret = pthread_create(&r->worker, nullptr, export_worker, r);
    if (ret) {
        ret = -ret;
        goto fail;
    }
    r->thread_started = true;
    *out = r;
    return 0;
fail:
    gk_replay_close(&r, nullptr);
    return ret;
}

int gk_replay_save(struct gk_replay *r, const char *path, int64_t endpoint_ns)
{
    if (atomic_load_explicit(&r->state, memory_order_acquire) != IDLE)
        return -EBUSY;
    if (!path || !*path || endpoint_ns <= 0 || r->keep_key == no_position)
        return -EAGAIN;
    int64_t end = endpoint_ns;
    for (unsigned i = 1; i < r->format->nb_streams; ++i)
        if (r->ends[i] < end)
            end = r->ends[i];
    if (end <= 0)
        return -EAGAIN;
    int64_t target = end > r->options.duration_ns ? end - r->options.duration_ns : 0;
    uint64_t key = r->keep_key;
    for (uint64_t next = following_key(r, key); next != no_position;
         next = following_key(r, next)) {
        if (time_ns(r, at(r, next)) > target)
            break;
        key = next;
    }
    int64_t start = time_ns(r, at(r, key));
    if (start >= end)
        return -EAGAIN;
    struct snapshot view = {
        .path = strdup(path),
        .result = {.start_ns = start, .end_ns = end, .requested_end_ns = endpoint_ns}};
    for (uint32_t id = r->head; id != NO_BLOCK; id = r->blocks[id]->next)
        ++view.count;
    view.blocks = calloc(view.count, sizeof(*view.blocks));
    view.ids = calloc(view.count, sizeof(*view.ids));
    if (!view.path || !view.blocks || !view.ids) {
        free(view.path);
        free(view.blocks);
        free(view.ids);
        return -ENOMEM;
    }
    size_t i = 0;
    for (uint32_t id = r->head; id != NO_BLOCK; id = r->blocks[id]->next) {
        struct block *b = r->blocks[id];
        view.blocks[i] = b;
        view.ids[i++] = id;
        view.packets += b->used;
        atomic_fetch_add_explicit(&b->refs, 1, memory_order_relaxed);
    }
    /* Once published, even the tail's unused slots remain immutable. A new live
     * block ensures an indefinitely paused exporter cannot accumulate new data. */
    r->sealed = true;
    r->job = view;
    atomic_store_explicit(&r->state, QUEUED, memory_order_release);
    wake_worker(r);
    return 0;
}

bool gk_replay_poll(struct gk_replay *r, struct gk_replay_result *result)
{
    if (!r)
        return false;
    gk_drain_event(r->wake);
    bool done = (atomic_load_explicit(&r->state, memory_order_acquire) & 3U) == DONE;
    if (done) {
        r->stats.last = r->job.result;
        if (r->job.result.error < 0)
            ++r->stats.failed;
        else {
            ++r->stats.saved;
            r->stats.saved_bytes += r->job.result.bytes;
        }
        *result = r->job.result;
        atomic_fetch_and_explicit(&r->state, STOP, memory_order_release);
    }
    if (done)
        gk_drain_event(r->wake); /* Publication may have followed the first drain. */
    recycle(r);
    return done;
}

int gk_replay_fd(const struct gk_replay *r)
{
    return r->wake;
}

void gk_replay_close(struct gk_replay **replay, struct gk_replay_stats *stats)
{
    struct gk_replay *r = *replay;
    if (!r)
        return;
    if (r->thread_started) {
        /* Changing the same predicate word closes the check/sleep race, even
         * when shutdown wins before the worker enters FUTEX_WAIT. */
        atomic_fetch_or_explicit(&r->state, STOP, memory_order_release);
        wake_worker(r);
        pthread_join(r->worker, nullptr);
        struct gk_replay_result result;
        (void)gk_replay_poll(r, &result);
    }
    if (stats)
        *stats = r->stats;
    /* The worker has stopped and returned every export lease. Release live
     * and owned references separately even during teardown. */
    while (r->head != NO_BLOCK) {
        uint32_t id = r->head;
        r->head = r->blocks[id]->next;
        release_live(r, r->blocks[id]);
        owner_release(r, id);
    }
    for (uint32_t id = 0; id < r->unused; ++id)
        free_sized(r->blocks[id], sizeof(struct block));
    free(r->blocks);
    free(r->available);
    free(r->last_pts);
    free(r->ends);
    gk_ring_destroy(&r->released);
    if (r->wake >= 0)
        close(r->wake);
#ifdef GK_REPLAY_EVENTFD
    if (r->request >= 0)
        close(r->request);
#endif
    free(r);
    *replay = nullptr;
}

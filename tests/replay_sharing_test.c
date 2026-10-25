#include "media.h"

#include <errno.h>
#include <libavcodec/avcodec.h>
#include <libavutil/mem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

struct allocation {
    size_t bytes;
    _Atomic bool freed;
};

static void release(void *opaque, uint8_t *data)
{
    struct allocation *allocation = opaque;
    CHECK(!atomic_exchange(&allocation->freed, true));
    free_sized(data, allocation->bytes);
}

static AVPacket *packet_from(const AVPacket *key, int64_t pts, size_t capacity, size_t prefix,
                             struct allocation *allocation)
{
    CHECK(prefix + (unsigned)key->size + AV_INPUT_BUFFER_PADDING_SIZE <= capacity);
    allocation->bytes = capacity;
    atomic_init(&allocation->freed, false);
    uint8_t *data = calloc(1, capacity);
    AVPacket *packet = av_packet_alloc();
    CHECK(data && packet);
    packet->buf = av_buffer_create(data, capacity, release, allocation, 0);
    CHECK(packet->buf);
    packet->data = data + prefix;
    packet->size = key->size;
    memcpy(packet->data, key->data, (size_t)key->size);
    packet->pts = packet->dts = pts;
    packet->flags = AV_PKT_FLAG_KEY;
    return packet;
}

static AVFormatContext *keys(AVPacket *result[static 2])
{
    char path[] = "/tmp/gpukey-share-source-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0 && close(fd) == 0 && unlink(path) == 0);
    struct gk_gpu gpu;
    CHECK(gk_gpu_open(&gpu, GK_SOFTWARE, nullptr) == 0);
    struct gk_video_format format = {.width = 64,
                                     .height = 48,
                                     .pixel_format = AV_PIX_FMT_BGRA,
                                     .transfer = AVCOL_TRC_IEC61966_2_1};
    struct gk_media_options options = {.output = path,
                                       .matroska = true,
                                       .bits = 8,
                                       .qp = 0,
                                       .surfaces = 2,
                                       .fps_num = 1,
                                       .fps_den = 1,
                                       .sink = {.slabs = 8, .slab_size = 4096, .stall_ms = 5000}};
    struct gk_media *media;
    CHECK(gk_media_open(&media, &gpu, &format, &options) == 0);
    uint8_t pixels[64 * 48 * 4];
    struct gk_image image = {.data = pixels, .size = sizeof(pixels), .stride = 64 * 4};
    for (unsigned i = 0; i < 2; ++i) {
        memset(pixels, i ? 255 : 0, sizeof(pixels));
        AVFrame *frame;
        CHECK(gk_media_prepare(media, &image, VA_INVALID_ID, &frame) == 0);
        CHECK(gk_media_submit(media, frame, (int64_t)i * 2 * GK_NS) == 0);
    }
    CHECK(gk_media_close(&media, 4 * GK_NS, nullptr) == 0);
    gk_gpu_close(&gpu);
    AVFormatContext *input = nullptr;
    CHECK(avformat_open_input(&input, path, nullptr, nullptr) == 0);
    CHECK(avformat_find_stream_info(input, nullptr) == 0);
    for (unsigned i = 0; i < 2; ++i) {
        result[i] = av_packet_alloc();
        CHECK(result[i] && av_read_frame(input, result[i]) == 0);
        CHECK(result[i]->stream_index == 0 && (result[i]->flags & AV_PKT_FLAG_KEY));
    }
    CHECK(result[0]->size != result[1]->size ||
          memcmp(result[0]->data, result[1]->data, (size_t)result[0]->size));
    CHECK(unlink(path) == 0);
    return input;
}

static void decode(const char *path, unsigned luma, const struct gk_replay_result *result)
{
    AVFormatContext *file = nullptr;
    CHECK(avformat_open_input(&file, path, nullptr, nullptr) == 0);
    CHECK(avformat_find_stream_info(file, nullptr) == 0 && file->nb_streams == 1);
    AVCodecContext *decoder = avcodec_alloc_context3(avcodec_find_decoder_by_name("libdav1d"));
    CHECK(decoder && avcodec_parameters_to_context(decoder, file->streams[0]->codecpar) == 0);
    decoder->thread_count = 1;
    CHECK(avcodec_open2(decoder, nullptr, nullptr) == 0);
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    CHECK(packet && frame);
    unsigned count = 0;
    int64_t covered = 0;
    for (;;) {
        bool eof = av_read_frame(file, packet) < 0;
        if (!eof) {
            CHECK(packet->pts == covered && packet->duration > 0);
            if (!count)
                CHECK(packet->pts == 0 && (packet->flags & AV_PKT_FLAG_KEY));
            covered = packet->pts + packet->duration;
        }
        CHECK(avcodec_send_packet(decoder, eof ? nullptr : packet) == 0);
        while (avcodec_receive_frame(decoder, frame) >= 0) {
            CHECK(frame->width == 64 && frame->height == 48);
            CHECK(frame->data[0][0] == luma);
            ++count;
            av_frame_unref(frame);
        }
        av_packet_unref(packet);
        if (eof)
            break;
    }
    CHECK(count == 2);
    CHECK(av_rescale_q(covered, file->streams[0]->time_base, (AVRational){1, 1000000000}) ==
          result->end_ns - result->start_ns);
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&decoder);
    avformat_close_input(&file);
}

static void paused(AVFormatContext *format, AVPacket *keys[static 2])
{
    int gate[2];
    CHECK(pipe(gate) == 0);
    int started = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    CHECK(started >= 0);
    struct gk_replay_options options = {.duration_ns = 2 * GK_NS,
                                        .bytes = 1048576,
                                        .matroska = true,
                                        .sink = {.slabs = 8, .slab_size = 4096, .stall_ms = 5000},
                                        .test_gate = gate[0],
                                        .test_started = started};
    struct gk_replay *replay;
    CHECK(gk_replay_open(&replay, format, &options) == 0);
    struct allocation allocations[35];
    char path[] = "/tmp/gpukey-share-export-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0 && close(fd) == 0 && unlink(path) == 0);
    struct gk_replay_result result;
    for (unsigned i = 0; i < 35; ++i) {
        bool white = (i >= 10 && i < 20) || i >= 30;
        int64_t pts = av_rescale_q((int64_t)i * GK_NS, (AVRational){1, 1000000000},
                                   format->streams[0]->time_base);
        AVPacket *packet = packet_from(keys[white], pts, 65536, i & 1 ? 32 : 0, &allocations[i]);
        CHECK(gk_replay_add(replay, packet) == 0 && !packet->buf);
        av_packet_free(&packet);
        if (i == 4) {
            CHECK(gk_replay_save(replay, path, 4500000000) == 0);
            CHECK(gk_wait(started, 5000) == 1);
        }
        if (i == 29) {
            CHECK(!atomic_load(&allocations[0].freed)); /* Snapshot still owns the old key. */
            CHECK(atomic_load(&allocations[1].freed));  /* Duplicate input was released promptly. */
            CHECK(
                atomic_load(&allocations[10].freed)); /* Expired, unpinned white key is retired. */
            CHECK(write(gate[1], "x", 1) == 1);
            CHECK(gk_wait(gk_replay_fd(replay), 5000) == 1);
            CHECK(gk_replay_poll(replay, &result) && result.error == 0);
            CHECK(atomic_load(&allocations[0].freed));
            decode(path, 16, &result);
            CHECK(unlink(path) == 0);
        }
    }
    CHECK(gk_replay_save(replay, path, 34500000000) == 0);
    CHECK(write(gate[1], "x", 1) == 1);
    struct gk_replay_stats stats;
    gk_replay_close(&replay, &stats); /* An accepted export must finish before destruction. */
    CHECK(stats.saved == 2 && stats.failed == 0 && stats.shared_keyframes == 31);
    CHECK(stats.peak_live_bytes < 200000 && stats.peak_owned_bytes < 300000);
    for (unsigned i = 0; i < 35; ++i)
        CHECK(atomic_load(&allocations[i].freed));
    decode(path, 235, &stats.last);
    CHECK(unlink(path) == 0);
    close(gate[0]);
    close(gate[1]);
    close(started);
}

static void budget(AVFormatContext *format, const AVPacket *key, bool unique)
{
    struct gk_replay_options options = {
        .duration_ns = 30 * GK_NS, .bytes = 1048576, .matroska = true, .unique_keys = unique};
    struct gk_replay *replay;
    CHECK(gk_replay_open(&replay, format, &options) == 0);
    struct allocation allocations[32];
    unsigned admitted = 0, allocated = 0;
    for (unsigned i = 0; i < 32; ++i) {
        int64_t pts = av_rescale_q((int64_t)i * GK_NS, (AVRational){1, 1000000000},
                                   format->streams[0]->time_base);
        AVPacket *packet = packet_from(key, pts, 65536, i & 1 ? 32 : 0, &allocations[i]);
        ++allocated;
        int ret = gk_replay_add(replay, packet);
        if (ret < 0) {
            CHECK(unique && ret == -ENOBUFS && packet->buf);
            av_packet_free(&packet);
            break;
        }
        ++admitted;
        av_packet_free(&packet);
    }
    struct gk_replay_stats stats;
    gk_replay_close(&replay, &stats);
    CHECK(unique ? admitted < 32 : admitted == 32);
    CHECK(stats.shared_keyframes == (unique ? 0 : 31));
    CHECK(stats.peak_live_bytes <= options.bytes);
    for (unsigned i = 0; i < allocated; ++i)
        CHECK(atomic_load(&allocations[i].freed));
}

static void validation_and_sizes(AVFormatContext *format, const AVPacket *key)
{
    struct gk_replay_options options = {.duration_ns = GK_NS, .bytes = 1048576, .matroska = true};
    struct gk_replay *replay;
    CHECK(gk_replay_open(&replay, format, &options) == 0);
    struct allocation allocations[5];
    for (unsigned i = 0; i < 5; ++i) {
        int64_t pts = av_rescale_q((int64_t)i * GK_NS, (AVRational){1, 1000000000},
                                   format->streams[0]->time_base);
        AVPacket *packet = packet_from(key, pts, i ? 4096 : 65536, 16, &allocations[i]);
        if (i == 2) {
            uint8_t *extra = av_packet_new_side_data(packet, AV_PKT_DATA_NEW_EXTRADATA, 1);
            CHECK(extra);
            *extra = 0xff;
            CHECK(gk_replay_add(replay, packet) == -ESTALE && packet->buf);
            av_packet_free_side_data(packet);
            packet->data[0] ^= 0x80;
            CHECK(gk_replay_add(replay, packet) == -EPROTO && packet->buf);
            packet->data[0] ^= 0x80;
        }
        CHECK(gk_replay_add(replay, packet) == 0);
        av_packet_free(&packet);
        if (i == 2)
            CHECK(atomic_load(&allocations[0].freed));
    }
    struct gk_replay_stats stats;
    gk_replay_close(&replay, &stats);
    CHECK(stats.shared_keyframes == 3); /* Replace the oversized first representative. */
    for (unsigned i = 0; i < 5; ++i)
        CHECK(atomic_load(&allocations[i].freed));
}

static int admit_one(AVFormatContext *format, const AVPacket *key, size_t bytes, bool unique)
{
    struct gk_replay_options options = {
        .duration_ns = GK_NS, .bytes = 1048576, .matroska = true, .unique_keys = unique};
    struct gk_replay *replay;
    CHECK(gk_replay_open(&replay, format, &options) == 0);
    struct allocation allocation;
    AVPacket *packet = packet_from(key, 0, bytes, 0, &allocation);
    int result = gk_replay_add(replay, packet);
    CHECK(result == 0 || result == -ENOBUFS);
    av_packet_free(&packet);
    gk_replay_close(&replay, nullptr);
    CHECK(atomic_load(&allocation.freed));
    return result;
}

static void budget_edge(AVFormatContext *format, const AVPacket *key)
{
    /* Find the reference policy's exact admission edge through its public API.
     * Optional sharing metadata must not reject an otherwise fitting first key. */
    size_t low = (unsigned)key->size + AV_INPUT_BUFFER_PADDING_SIZE, high = 1048576;
    CHECK(admit_one(format, key, low, true) == 0);
    CHECK(admit_one(format, key, high, true) == -ENOBUFS);
    while (high - low > 1) {
        size_t middle = low + (high - low) / 2;
        if (admit_one(format, key, middle, true) == 0)
            low = middle;
        else
            high = middle;
    }
    CHECK(admit_one(format, key, low, false) == 0);
}

int main(void)
{
    AVPacket *packets[2];
    AVFormatContext *format = keys(packets);
    paused(format, packets);
    budget(format, packets[0], false);
    budget(format, packets[0], true);
    validation_and_sizes(format, packets[0]);
    budget_edge(format, packets[0]);
    for (unsigned i = 0; i < 2; ++i)
        av_packet_free(&packets[i]);
    avformat_close_input(&format);
    puts("replay sharing: exact keys, sub-buffer offsets, paused export, eviction, budget and "
         "teardown pass");
    return 0;
}

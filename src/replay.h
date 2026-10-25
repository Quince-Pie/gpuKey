#ifndef GPUKEY_REPLAY_H
#define GPUKEY_REPLAY_H

#include "sink.h"

#include <libavformat/avformat.h>

struct gk_replay_options {
    int64_t duration_ns;
    size_t bytes;
    bool matroska, copy_packets, copy_video;
    bool unique_keys; /* Comparison control: do not share byte-identical video keys. */
    struct gk_sink_options sink;
    /* Deterministic paused-export tests only; positive borrowed FDs, zero disables. */
    int test_gate, test_started;
};
struct gk_replay_result {
    int error;
    int64_t start_ns, end_ns, requested_end_ns;
    uint64_t bytes;
};
struct gk_replay_stats {
    uint64_t packets, copied_bytes, peak_live_bytes, peak_owned_bytes;
    uint64_t shared_keyframes, shared_key_bytes;
    uint64_t saved, failed, saved_bytes;
    struct gk_replay_result last;
};
struct gk_replay;
/* One media owner serializes all API calls, including close.
 * format remains alive through close. Time bases/layouts are fixed; initial
 * extradata may be completed before that stream's first packet. All stream
 * configuration is immutable before the first save is published. */
[[nodiscard]] int gk_replay_open(struct gk_replay **out, const AVFormatContext *format,
                                 const struct gk_replay_options *options);
/* Transfers CPU packet storage on success. copy_packets also handles mapped GPU
 * bitstream packets whose AVBufferRef owns a driver object instead of CPU bytes. */
[[nodiscard]] int gk_replay_add(struct gk_replay *replay, AVPacket *packet);
/* Same transfer contract for known CPU storage, bypassing automatic GPU readback
 * copying. An explicit copy_packets control is still honored. */
[[nodiscard]] int gk_replay_add_cpu(struct gk_replay *replay, AVPacket *packet);
/* Empty dst, then retain the newest video packet iff it is an independent key.
 * Returns 1 with a CPU-storage reference, 0 when ineligible, or a negative error.
 * A null dst only queries eligibility; the single owner retains live state. */
[[nodiscard]] int gk_replay_key_ref(struct gk_replay *replay, AVPacket *dst);
/* One export at a time. End is limited to the common completed audio watermark;
 * the last video image holds through that endpoint. Capture never waits for disk. */
[[nodiscard]] int gk_replay_save(struct gk_replay *replay, const char *path, int64_t endpoint_ns);
bool gk_replay_poll(struct gk_replay *replay, struct gk_replay_result *result);
int gk_replay_fd(const struct gk_replay *replay);
void gk_replay_close(struct gk_replay **replay, struct gk_replay_stats *stats);

#endif

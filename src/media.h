#ifndef GPUKEY_MEDIA_H
#define GPUKEY_MEDIA_H

#include "audio.h"
#include "gpu.h"
#include "replay.h"
#include "sink.h"

enum gk_audio_codec { GK_AUDIO_CODEC_AUTO, GK_AUDIO_CODEC_OPUS, GK_AUDIO_CODEC_PCM };
enum gk_packet_readout {
    GK_READOUT_AUTO,
    GK_READOUT_BUFFERED,
    GK_READOUT_AVAILABLE,
    GK_READOUT_EVENTS
};
struct gk_media_options {
    const char *output;
    bool matroska, measure, compact_gpu, encode_repeats;
    /* Static refreshes use independent keys. Compact can then discard the
     * redundant raw fallback after a CPU key has been published successfully. */
    bool independent_repeats;
    const struct gk_audio_track *audio;
    unsigned audio_count;
    enum gk_audio_codec audio_codec;
    unsigned bits, surfaces; /* surfaces == 0 selects the backend's bounded input pool. */
    unsigned encode_depth;   /* 0 selects 2; VAAPI/Vulkan only. */
    enum gk_packet_readout packet_readout;
    int qp;
    uint32_t fps_num, fps_den;
    struct gk_sink_options sink;
    struct gk_replay_options replay;
};
struct gk_media_stats {
    unsigned encoder_inputs, encode_depth;
    bool available_readout;
    bool completion_events;
    uint64_t completion_wakeups, completion_fallbacks;
    uint64_t submitted, packets, reused_packets, audio_samples, audio_silence, audio_trimmed;
    uint64_t video_boundary_omitted;
    struct gk_histogram preparation, submission;
    struct gk_sink_stats sink;
    struct gk_replay_stats replay;
};
struct gk_media;
[[nodiscard]] int gk_media_open(struct gk_media **out, struct gk_gpu *gpu,
                                const struct gk_video_format *source,
                                const struct gk_media_options *options);
/* After successful prepare, the source lease can be returned before encode.
 * The returned AVFrame is media-owned and valid through its submission. Later
 * media operations may recycle it; callers must not free it and must clone a
 * reference before retaining its image beyond that borrowing interval. */
[[nodiscard]] int gk_media_prepare(struct gk_media *media, const struct gk_image *image,
                                   VASurfaceID imported, AVFrame **frame);
/* Split preparation permits serialization of submission with buffer removal,
 * while keeping the GPU completion wait outside the PipeWire loop lock. */
[[nodiscard]] int gk_media_prepare_begin(struct gk_media *media, const struct gk_image *image,
                                         VASurfaceID imported, AVFrame **frame);
[[nodiscard]] int gk_media_prepare_finish(struct gk_media *media, AVFrame *frame);
[[nodiscard]] int gk_media_submit(struct gk_media *media, AVFrame *frame, int64_t pts_ns);
/* Direct RGB borrows the persistent import and retains the last image until
 * replaced or compact independent-key standby releases it. Capture may return
 * its lease only once its cache solely owns the import; codec completion and
 * last-image ownership both count. */
[[nodiscard]] int gk_media_submit_imported(struct gk_media *media, AVFrame *frame, int64_t pts_ns);
[[nodiscard]] int gk_media_repeat(struct gk_media *media, int64_t pts_ns);
/* One owner. Completion descriptors are borrowed. Compact mode may close idle
 * descriptors during codec calls: invalidate cached watches when generation
 * changes, before entering an event loop. Drain before querying a pending FD. */
int gk_media_completion_fd(const struct gk_media *media);
uint64_t gk_media_completion_generation(const struct gk_media *media);
[[nodiscard]] int gk_media_collect(struct gk_media *media, bool notified);
int gk_media_error(const struct gk_media *media);
[[nodiscard]] int gk_media_audio(struct gk_media *media, unsigned track, const float *samples,
                                 unsigned frames, int64_t pts_ns);
[[nodiscard]] int gk_media_save(struct gk_media *media, const char *path, int64_t endpoint_ns);
bool gk_media_saved(struct gk_media *media, struct gk_replay_result *result);
int gk_media_replay_fd(const struct gk_media *media);
/* Last displayed frame extends to endpoint. Healthy encoders drain before sink
 * completion; a fatal codec error closes without another encode/drain call and
 * preserves the collected prefix while returning the original error. */
[[nodiscard]] int gk_media_close(struct gk_media **media, int64_t endpoint_ns,
                                 struct gk_media_stats *stats);

#endif

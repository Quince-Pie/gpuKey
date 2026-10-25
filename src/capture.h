#ifndef GPUKEY_CAPTURE_H
#define GPUKEY_CAPTURE_H

#include "audio.h"
#include "gpu.h"
#include "portal.h"

enum gk_audio_kind { GK_AUDIO_AUTO, GK_AUDIO_SOURCE, GK_AUDIO_MONITOR };
enum gk_video_clock : uint8_t {
    GK_VIDEO_CLOCK_AUTO,
    GK_VIDEO_CLOCK_SOURCE,
    GK_VIDEO_CLOCK_CYCLE,
    GK_VIDEO_CLOCK_RECEIVE
};
struct gk_audio_selection {
    const char *target; /* Exact object serial or node name; resolved on the capture connection. */
    enum gk_audio_kind kind;
};
struct gk_capture_options {
    unsigned buffers;
    unsigned preferred_buffers; /* 0 uses the capacity; negotiation may require more. */
    size_t cpu_budget;
    uint32_t fps_num, fps_den;
    enum gk_video_clock video_clock;
    bool allow_shm, force_shm;
    bool stop_on_overload;
    const struct gk_audio_selection *audio;
    unsigned audio_count;
};
struct gk_capture_frame {
    struct gk_image image;
    int64_t pts, arrival;
    uint64_t token;
};
struct gk_audio_frame {
    const float *samples;
    unsigned frames, track;
    uint64_t token; /* track:slot; both bounded, stable for the capture lifetime. */
    int64_t pts;
};
struct gk_capture_stats {
    uint64_t offered, admitted, skipped, corrupted, rate_limited;
    enum gk_video_clock video_clock;
    unsigned peak_source_buffers;
    alignas(64) uint64_t audio_blocks, audio_dropped, audio_silenced_samples;
};
struct gk_capture;
/* Takes ownership of source.fd, including on failure. */
[[nodiscard]] int gk_capture_open(struct gk_capture **capture, struct gk_portal_source source,
                                  const struct gk_gpu *gpu,
                                  const struct gk_capture_options *options);
int gk_capture_fd(const struct gk_capture *capture);
int gk_capture_error(const struct gk_capture *capture);
bool gk_capture_format(const struct gk_capture *capture, struct gk_video_format *format);
/* Ready only once every selected audio source has an immutable negotiated format. */
bool gk_capture_audio_formats(const struct gk_capture *capture, struct gk_audio_track *tracks);
bool gk_capture_next(struct gk_capture *capture, struct gk_capture_frame *frame);
/* Success holds the video lifecycle lock through GPU submission only.
 * A failed claim leaves the token owned by the caller for ordinary release. */
bool gk_capture_begin_submission(struct gk_capture *capture, uint64_t token);
void gk_capture_end_submission(struct gk_capture *capture);
void gk_capture_release(struct gk_capture *capture, uint64_t token);
bool gk_capture_audio_next(struct gk_capture *capture, struct gk_audio_frame *frame);
void gk_capture_audio_release(struct gk_capture *capture, uint64_t token);
void gk_capture_stop(struct gk_capture *capture);
void gk_capture_close(struct gk_capture **capture, struct gk_capture_stats *stats);

#endif

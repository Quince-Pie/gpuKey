#ifndef GPUKEY_AUDIO_H
#define GPUKEY_AUDIO_H

#include <libavutil/channel_layout.h>
#include <stdint.h>

/* SPA's public maximum. These are cold per-source attributes, never queue cells. */
enum { GK_AUDIO_CHANNELS = 64, GK_AUDIO_FRAMES = 4096 };
struct gk_audio_format {
    unsigned rate, channels;
    bool discrete;
    union {
        enum AVChannel position[GK_AUDIO_CHANNELS];
        uint8_t auxiliary[GK_AUDIO_CHANNELS];
    };
};
struct gk_audio_track {
    struct gk_audio_format format;
    const char *title;
};

#define GK_AUDIO_STEREO                                                                            \
    {                                                                                              \
        .rate = 48000, .channels = 2, .position = { AV_CHAN_FRONT_LEFT, AV_CHAN_FRONT_RIGHT }      \
    }

/* Preserve channel identities; canonicalize only their ordering. No downmix.
 * The caller owns both layouts, including on failure. */
[[nodiscard]] int gk_audio_layout(const struct gk_audio_format *format, AVChannelLayout *input,
                                  AVChannelLayout *output);
/* PipeWire capture cycle time minus graph and converter delay. Queued-buffer
 * waiting is already represented by the individual buffer's cycle timestamp. */
[[nodiscard]] bool gk_audio_cycle_pts(uint64_t cycle_ns, int64_t delay, uint32_t rate_num,
                                      uint32_t rate_den, uint64_t buffered, uint32_t sample_rate,
                                      int64_t *pts);

#endif

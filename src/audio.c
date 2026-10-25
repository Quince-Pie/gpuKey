#include "audio.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>

bool gk_audio_cycle_pts(uint64_t cycle_ns, int64_t delay, uint32_t num, uint32_t den,
                        uint64_t buffered, uint32_t sample_rate, int64_t *pts)
{
    if (!cycle_ns || cycle_ns > INT64_MAX || !num || !den || !sample_rate)
        return false;
    signed _BitInt(128) graph = (signed _BitInt(128))delay * num * 1000000000 / den;
    signed _BitInt(128) converter = (signed _BitInt(128))buffered * 1000000000 / sample_rate;
    signed _BitInt(128) result = (signed _BitInt(128))cycle_ns - graph - converter;
    if (result < 0 || result > INT64_MAX)
        return false;
    *pts = (int64_t)result;
    return true;
}

int gk_audio_layout(const struct gk_audio_format *f, AVChannelLayout *input,
                    AVChannelLayout *output)
{
    if (f->rate < 8000 || f->rate > 384000 || !f->channels || f->channels > GK_AUDIO_CHANNELS)
        return -ENOTSUP;
    if (f->discrete) {
        uint64_t seen = 0;
        for (unsigned i = 0; i < f->channels; ++i) {
            unsigned id = f->auxiliary[i];
            if (id >= GK_AUDIO_CHANNELS || (seen & (UINT64_C(1) << id)))
                return -EINVAL;
            seen |= UINT64_C(1) << id;
        }
        int ret = av_channel_layout_custom_init(output, (int)f->channels);
        if (ret < 0)
            return ret;
        for (unsigned i = 0; i < f->channels; ++i) {
            output->u.map[i].id = AV_CHAN_UNKNOWN;
            snprintf(output->u.map[i].name, sizeof(output->u.map[i].name), "AUX%u",
                     f->auxiliary[i]);
        }
        return av_channel_layout_copy(input, output);
    }
    uint64_t mask = 0;
    for (unsigned i = 0; i < f->channels; ++i) {
        if (f->position[i] < 0 || f->position[i] >= 64)
            return -ENOTSUP;
        uint64_t bit = UINT64_C(1) << f->position[i];
        if (mask & bit)
            return -EINVAL;
        mask |= bit;
    }
    int ret = av_channel_layout_custom_init(input, (int)f->channels);
    if (ret < 0)
        return ret;
    for (unsigned i = 0; i < f->channels; ++i)
        input->u.map[i].id = f->position[i];
    return av_channel_layout_from_mask(output, mask);
}

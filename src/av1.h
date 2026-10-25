#ifndef GPUKEY_AV1_H
#define GPUKEY_AV1_H

#include <libavcodec/codec_par.h>
#include <libavcodec/packet.h>

/* One-shot parser flags. Caller retains the immutable packet through parser
 * close; borrowing is selected only when owned zero padding is verified. */
[[nodiscard]] int gk_av1_parser_flags(const AVPacket *packet);

/* A single displayed independent key, single base layer, no coded timing or
 * still-picture restriction. The caller must separately prove that no real
 * input has changed the encoder's state since this key was produced. */
bool gk_av1_reusable_key(const AVCodecParameters *parameters, const AVPacket *packet);

/* Inspect the first independent picture before publishing stream metadata.
 * Check the planned matrix/range/transfer against the bitstream. Retain coded
 * geometry; present the requested top-left image via standard
 * container crop metadata. The packet remains owned and unchanged by caller. */
[[nodiscard]] int gk_av1_configuration(AVCodecParameters *parameters, const AVPacket *first,
                                       AVRational time_base, int width, int height, unsigned bits);

#endif

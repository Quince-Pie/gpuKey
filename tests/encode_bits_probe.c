/* nix/encode-bits-check.nix supplies the generated headers for this probe.
 * The driver bodies are extracted from pinned original and patched sources.
 * The oracle writes individual bits and observes each firmware-consumed count. */
#include <assert.h>
#include <stdbit.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

[[maybe_unused]] static inline unsigned util_last_bit(unsigned value)
{
    return stdc_bit_width(value);
}
#include "encode_bits_candidate.h"
#include "encode_bits_control.h"

enum { PUT, ALIGN, FLUSH, RESET, OPS = 512, CAPACITY = 2 * OPS + 4 };
struct operation {
    uint32_t value;
    uint8_t bits, kind;
    uint16_t padding;
};
static_assert(sizeof(struct operation) == 8);
struct result {
    unsigned words;
};

static unsigned control_flush(struct control_radv_cmd_buffer *c, unsigned *copied)
{
    control_radv_enc_flush_headers(c);
    unsigned total = c->video.enc.bits_output;
    unsigned bits = total - *copied;
    *copied = total;
    return bits;
}

static unsigned candidate_flush(struct candidate_radv_cmd_buffer *c, unsigned *copied)
{
#ifdef GK_BITS_CUMULATIVE
    candidate_radv_enc_flush_headers(c);
    unsigned total = c->video.enc.bits_output;
    unsigned bits = total - *copied;
    *copied = total;
    return bits;
#else
    (void)copied;
    return candidate_radv_enc_flush_headers(c);
#endif
}

#define RUNNER(name, initialize, align)                                                            \
    static struct result name(const struct operation *ops, unsigned n, uint32_t *out)              \
    {                                                                                              \
        struct name##_ac_cmdbuf b = {.buf = out};                                                  \
        struct name##_radv_cmd_stream cs = {.b = &b};                                              \
        struct name##_radv_cmd_buffer c = {.cs = &cs};                                             \
        unsigned pending = 0, copied = 0;                                                          \
        name##_radv_enc_reset(&c);                                                                 \
        initialize;                                                                                \
        for (unsigned i = 0; i < n; ++i) {                                                         \
            switch (ops[i].kind) {                                                                 \
            case PUT:                                                                              \
                name##_radv_enc_code_fixed_bits(&c, ops[i].value, ops[i].bits);                    \
                pending = (pending + ops[i].bits) % 32;                                            \
                break;                                                                             \
            case ALIGN: {                                                                          \
                unsigned padding = (8 - pending % 8) % 8;                                          \
                align;                                                                             \
                pending = (pending + padding) % 32;                                                \
                break;                                                                             \
            }                                                                                      \
            case FLUSH: {                                                                          \
                unsigned bits = name##_flush(&c, &copied);                                         \
                out[b.cdw++] = bits;                                                               \
                pending = 0;                                                                       \
                break;                                                                             \
            }                                                                                      \
            case RESET:                                                                            \
                name##_radv_enc_reset(&c);                                                         \
                pending = 0;                                                                       \
                copied = 0;                                                                        \
                break;                                                                             \
            default:                                                                               \
                abort();                                                                           \
            }                                                                                      \
        }                                                                                          \
        unsigned bits = name##_flush(&c, &copied);                                                 \
        out[b.cdw++] = bits;                                                                       \
        return (struct result){b.cdw};                                                             \
    }

RUNNER(control, control_radv_enc_set_emulation_prevention(&c, false),
       control_radv_enc_byte_align(&c))
RUNNER(candidate, (void)0, candidate_radv_enc_code_fixed_bits(&c, 0, padding))

static struct result oracle(const struct operation *ops, unsigned n, uint32_t *out)
{
    unsigned at = 0, used = 0, total = 0;
    uint32_t current = 0;
    for (unsigned i = 0; i <= n; ++i) {
        unsigned kind = i == n ? FLUSH : ops[i].kind;
        if (kind == PUT || kind == ALIGN) {
            unsigned width = kind == ALIGN ? (8 - used % 8) % 8 : ops[i].bits;
            uint32_t value = kind == ALIGN ? 0 : ops[i].value;
            for (unsigned bit = width; bit > 0; --bit) {
                current |= ((value >> (bit - 1)) & 1U) << (31 - used);
                ++used;
                ++total;
                if (used == 32) {
                    out[at++] = current;
                    current = 0;
                    used = 0;
                }
            }
        } else {
            if (kind == FLUSH) {
                if (used)
                    out[at++] = current;
                out[at++] = total;
                total = 0;
            }
            current = 0;
            used = 0;
            if (kind == RESET)
                total = 0;
        }
    }
    return (struct result){at};
}

static uint32_t random32(uint32_t *state)
{
    *state ^= *state << 13;
    *state ^= *state >> 17;
    *state ^= *state << 5;
    return *state;
}

static void check(const struct operation *ops, unsigned count)
{
    uint32_t expected[CAPACITY], actual[CAPACITY];
    struct result reference = oracle(ops, count, expected);
    assert(reference.words < CAPACITY);
    struct result (*const runners[])(const struct operation *, unsigned, uint32_t *) = {control,
                                                                                        candidate};
    for (unsigned i = 0; i < 2; ++i) {
        memset(actual, 0xa5, sizeof(actual));
        struct result result = runners[i](ops, count, actual);
        assert(result.words == reference.words);
        assert(!memcmp(expected, actual, (size_t)reference.words * sizeof(*actual)));
        assert(actual[CAPACITY - 1] == UINT32_C(0xa5a5a5a5));
    }
}

int main(void)
{
    uint32_t seed = UINT32_C(0x9d52ab17);
    struct operation ops[OPS];
    for (unsigned prefix = 0; prefix < 32; ++prefix)
        for (unsigned width = 0; width <= 32; ++width)
            for (unsigned value = 0; value < 4; ++value) {
                ops[0] = (struct operation){.value = random32(&seed), .bits = (uint8_t)prefix};
                ops[1] = (struct operation){.value = value == 0   ? 0
                                                     : value == 1 ? UINT32_MAX
                                                     : value == 2 ? 1
                                                                  : UINT32_C(0xaaaaaaaa),
                                            .bits = (uint8_t)width};
                ops[2] = (struct operation){.kind = FLUSH};
                ops[3] = (struct operation){.value = UINT32_MAX, .bits = 32};
                check(ops, 4);
            }
    for (unsigned trial = 0; trial < 10000; ++trial) {
        unsigned count = 1 + random32(&seed) % OPS;
        for (unsigned i = 0; i < count; ++i) {
            uint32_t value = random32(&seed);
            ops[i] = (struct operation){.value = random32(&seed),
                                        .bits = (uint8_t)(value % 33),
                                        .kind = (uint8_t)(value % 19 == 0   ? RESET
                                                          : value % 17 == 0 ? FLUSH
                                                          : value % 11 == 0 ? ALIGN
                                                                            : PUT)};
        }
        check(ops, count);
    }
    printf("encode bits: 4,224 boundary cases and 10,000 stateful sequences pass; state %zu -> %zu "
           "bytes\n",
           sizeof(struct control_radv_enc_state), sizeof(struct candidate_radv_enc_state));
    return 0;
}

#ifndef GPUKEY_CORE_H
#define GPUKEY_CORE_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#define GK_VERSION "0.1.0"
#define GK_NS INT64_C(1000000000)

/* Linux cache-line isolation is a layout choice, not a C portability claim.
 * Cells hold ownership tokens, never pointers to borrowed PipeWire metadata.
 * One producer and one consumer; destroy only after both have stopped. */
struct gk_ring {
    alignas(64) _Atomic uint32_t published;
    uint32_t write, read_cache;
    alignas(64) _Atomic uint32_t released;
    uint32_t read, write_cache;
    alignas(64) uint64_t *cells;
    uint32_t capacity;
};

[[nodiscard]] int gk_ring_init(struct gk_ring *q, uint32_t capacity);
void gk_ring_destroy(struct gk_ring *q);
[[nodiscard]] bool gk_ring_push(struct gk_ring *q, uint64_t token);
[[nodiscard]] bool gk_ring_pop(struct gk_ring *q, uint64_t *token);
int64_t gk_now_ns(void);
[[nodiscard]] bool gk_timestamp(uint64_t index, uint32_t num, uint32_t den, int64_t *ns);
/* Nearest fixed capture slot; PTS remains the actual source time. */
[[nodiscard]] bool gk_rate_slot(int64_t ns, uint32_t num, uint32_t den, uint64_t *slot);
[[nodiscard]] int gk_notify(int fd);
void gk_drain_event(int fd);
[[nodiscard]] int gk_wait(int fd, int timeout_ms);

struct gk_histogram {
    uint64_t bins[64], count, total, maximum;
};
void gk_hist_add(struct gk_histogram *h, uint64_t ns);
uint64_t gk_hist_percentile(const struct gk_histogram *h, unsigned percent);

#endif

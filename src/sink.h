#ifndef GPUKEY_SINK_H
#define GPUKEY_SINK_H

#include "core.h"

enum gk_sink_kind { GK_SINK_BUFFERED, GK_SINK_URING };
struct gk_sink_options {
    enum gk_sink_kind kind;
    size_t slab_size;
    uint32_t slabs;
    int stall_ms;
    bool durable, measure;
    /* Fault injection; zero disables. Used by the same production write path. */
    size_t max_write;
    uint64_t fail_after;
};
struct gk_sink_stats {
    uint64_t accepted, written, durable, peak_pending;
    struct gk_histogram completion;
};
struct gk_sink;
[[nodiscard]] int gk_sink_open(struct gk_sink **out, const char *path,
                               const struct gk_sink_options *options);
/* AVIO requires complete acceptance or a negative error, never a short success. */
int gk_sink_write(void *opaque, const uint8_t *data, int size);
/* Joins the writer and retires all kernel accesses before releasing payloads. */
[[nodiscard]] int gk_sink_close(struct gk_sink **sink, struct gk_sink_stats *stats);
int gk_sink_error(const struct gk_sink *sink);

#endif

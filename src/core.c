#include "core.h"

#include <errno.h>
#include <poll.h>
#include <stdbit.h>
#include <stdckdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

int gk_ring_init(struct gk_ring *q, uint32_t capacity)
{
    if (!stdc_has_single_bit(capacity) || capacity > UINT32_MAX / 2)
        return -EINVAL;
    *q = (struct gk_ring){};
    atomic_init(&q->published, 0);
    atomic_init(&q->released, 0);
    size_t bytes = ((size_t)capacity * sizeof(*q->cells) + 63U) & ~(size_t)63U;
    q->cells = aligned_alloc(64, bytes);
    if (!q->cells)
        return -ENOMEM;
    memset(q->cells, 0, bytes);
    q->capacity = capacity;
    return 0;
}

void gk_ring_destroy(struct gk_ring *q)
{
    free(q->cells);
    q->cells = nullptr;
}

bool gk_ring_push(struct gk_ring *q, uint64_t token)
{
    if (q->write - q->read_cache == q->capacity) {
        q->read_cache = atomic_load_explicit(&q->released, memory_order_acquire);
        if (q->write - q->read_cache == q->capacity)
            return false;
    }
    q->cells[q->write & (q->capacity - 1)] = token;
    atomic_store_explicit(&q->published, ++q->write, memory_order_release);
    return true;
}

bool gk_ring_pop(struct gk_ring *q, uint64_t *token)
{
    if (q->read == q->write_cache) {
        q->write_cache = atomic_load_explicit(&q->published, memory_order_acquire);
        if (q->read == q->write_cache)
            return false;
    }
    *token = q->cells[q->read & (q->capacity - 1)];
    atomic_store_explicit(&q->released, ++q->read, memory_order_release);
    return true;
}

int64_t gk_now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
        abort();
    return ts.tv_sec * GK_NS + ts.tv_nsec;
}

bool gk_timestamp(uint64_t index, uint32_t num, uint32_t den, int64_t *ns)
{
    if (!num || !den)
        return false;
    /* At most 64 + 32 + 30 bits before division, for the entire input domain. */
    unsigned _BitInt(128) value = (unsigned _BitInt(128))index * den * (uint64_t)GK_NS;
    value = (value + num / 2U) / num;
    if (value > INT64_MAX)
        return false;
    *ns = (int64_t)value;
    return true;
}

bool gk_rate_slot(int64_t ns, uint32_t num, uint32_t den, uint64_t *slot)
{
    if (ns < 0 || !num || !den)
        return false;
    uint64_t scale = (uint64_t)den * (uint64_t)GK_NS;
    unsigned _BitInt(128) value = ((unsigned _BitInt(128))(uint64_t)ns * num + scale / 2) / scale;
    if (value > UINT64_MAX)
        return false;
    *slot = (uint64_t)value;
    return true;
}

int gk_notify(int fd)
{
    uint64_t one = 1;
    ssize_t n;
    do {
        n = write(fd, &one, sizeof(one));
    } while (n < 0 && errno == EINTR);
    return n == sizeof(one) || (n < 0 && errno == EAGAIN) ? 0 : -errno;
}

void gk_drain_event(int fd)
{
    uint64_t value;
    while (read(fd, &value, sizeof(value)) < 0 && errno == EINTR) {
    }
}

int gk_wait(int fd, int timeout_ms)
{
    struct pollfd p = {.fd = fd, .events = POLLIN};
    int ret;
    do {
        ret = poll(&p, 1, timeout_ms);
    } while (ret < 0 && errno == EINTR);
    if (ret < 0)
        return -errno;
    if (p.revents & (POLLERR | POLLHUP | POLLNVAL))
        return -EIO;
    return ret;
}

void gk_hist_add(struct gk_histogram *h, uint64_t ns)
{
    unsigned bin = ns ? stdc_bit_width(ns) - 1 : 0;
    h->bins[bin]++;
    h->count++;
    h->total += ns;
    if (ns > h->maximum)
        h->maximum = ns;
}

uint64_t gk_hist_percentile(const struct gk_histogram *h, unsigned percent)
{
    if (!h->count || percent > 100)
        return 0;
    uint64_t goal = h->count / 100 * percent + (h->count % 100 * percent + 99) / 100;
    uint64_t count = 0;
    for (unsigned i = 0; i < 64; ++i) {
        count += h->bins[i];
        if (count >= goal)
            return i == 63 ? UINT64_MAX : (UINT64_C(1) << (i + 1)) - 1;
    }
    return h->maximum;
}

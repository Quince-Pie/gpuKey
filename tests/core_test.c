#include "core.h"
#include "sink.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static void *produce(void *opaque)
{
    struct gk_ring *q = opaque;
    for (uint64_t i = 0; i < 1000000; ++i)
        while (!gk_ring_push(q, i ^ UINT64_C(0xd38acb73ff1098a0)))
            sched_yield();
    return nullptr;
}

static void rings(void)
{
    struct gk_ring q;
    CHECK(gk_ring_init(&q, 3) == -EINVAL);
    CHECK(gk_ring_init(&q, 8) == 0);
    q.write = q.read = q.read_cache = q.write_cache = UINT32_MAX - 3;
    atomic_store(&q.published, q.write);
    atomic_store(&q.released, q.read);
    pthread_t thread;
    CHECK(pthread_create(&thread, nullptr, produce, &q) == 0);
    for (uint64_t i = 0; i < 1000000; ++i) {
        uint64_t token;
        while (!gk_ring_pop(&q, &token))
            sched_yield();
        CHECK(token == (i ^ UINT64_C(0xd38acb73ff1098a0)));
    }
    CHECK(pthread_join(thread, nullptr) == 0);
    uint64_t token;
    CHECK(!gk_ring_pop(&q, &token));
    gk_ring_destroy(&q);
}

static void timeline(void)
{
    int64_t ns;
    CHECK(gk_timestamp(240 * 86400ULL, 240, 1, &ns) && ns == 86400 * GK_NS);
    CHECK(gk_timestamp(60000, 60000, 1001, &ns) && ns == 1001 * GK_NS);
    CHECK(gk_timestamp(1, 240, 1, &ns) && ns == 4166667);
    CHECK(gk_timestamp(2, 240, 1, &ns) && ns == 8333333);
    CHECK(gk_timestamp(UINT32_MAX, UINT32_MAX, UINT32_MAX, &ns) &&
          ns == (int64_t)UINT32_MAX * GK_NS);
    CHECK(!gk_timestamp(UINT64_MAX, 1, 1, &ns));
    CHECK(!gk_timestamp(1, 0, 1, &ns));
    CHECK(!gk_timestamp(1, 1, 0, &ns));
    uint64_t slot;
    CHECK(!gk_rate_slot(-1, 60, 1, &slot));
    CHECK(!gk_rate_slot(0, 0, 1, &slot));
    CHECK(!gk_rate_slot(0, 1, 0, &slot));
    CHECK(!gk_rate_slot(INT64_MAX, UINT32_MAX, 1, &slot));
    CHECK(gk_rate_slot(INT64_MAX, 1000000, 1, &slot));
    for (uint64_t i = 0; i < 60000; ++i) {
        CHECK(gk_timestamp(i, 60000, 1001, &ns));
        CHECK(gk_rate_slot(ns, 60000, 1001, &slot) && slot == i);
        if (i % 2 == 0)
            CHECK(gk_rate_slot(ns, 30000, 1001, &slot) && slot == i / 2);
    }
}

static void output(enum gk_sink_kind kind, bool failure)
{
    char path[] = "/tmp/gpukey-output-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    CHECK(close(fd) == 0);
    struct gk_sink *sink;
    struct gk_sink_options options = {
        .kind = kind,
        .slab_size = 113,
        .slabs = 8,
        .stall_ms = 5000,
        .durable = true,
        .max_write = 7,
        .fail_after = failure ? 101 : 0,
    };
    CHECK(gk_sink_open(&sink, path, &options) == -EEXIST);
    CHECK(unlink(path) == 0);
    CHECK(gk_sink_open(&sink, path, &options) == 0);
    uint8_t expected[4097];
    for (size_t i = 0; i < sizeof(expected); ++i)
        expected[i] = (uint8_t)(i * 73 + (i >> 7));
    int ret = gk_sink_write(sink, expected, sizeof(expected));
    struct gk_sink_stats stats;
    int closed = gk_sink_close(&sink, &stats);
    if (kind == GK_SINK_URING && (closed == -EPERM || closed == -ENOSYS || closed == -EINVAL)) {
        puts("SKIP io_uring: unavailable in this kernel/sandbox");
        CHECK(unlink(path) == 0);
        return;
    }
    if (failure) {
        CHECK(ret < 0 || ret == sizeof(expected));
        CHECK(closed == -ENOSPC);
        CHECK(stats.written > 0 && stats.written <= 101 && stats.durable == 0);
        if (kind == GK_SINK_BUFFERED)
            CHECK(stats.written == 101);
    } else {
        CHECK(ret == sizeof(expected));
        CHECK(closed == 0);
        CHECK(stats.accepted == sizeof(expected));
        CHECK(stats.written == stats.accepted && stats.durable == stats.written);
    }
    fd = open(path, O_RDONLY);
    CHECK(fd >= 0);
    uint8_t actual[sizeof(expected)] = {};
    ssize_t size = read(fd, actual, sizeof(actual));
    CHECK(size >= 0 && (uint64_t)size >= stats.written);
    CHECK(memcmp(actual, expected, (size_t)stats.written) == 0);
    CHECK(close(fd) == 0);
    CHECK(unlink(path) == 0);
}

int main(void)
{
    timeline();
    rings();
    output(GK_SINK_BUFFERED, false);
    output(GK_SINK_BUFFERED, true);
    output(GK_SINK_URING, false);
    output(GK_SINK_URING, true);
    puts("core: timeline, concurrent wrap, ordered output, short writes and ENOSPC passed");
    return 0;
}

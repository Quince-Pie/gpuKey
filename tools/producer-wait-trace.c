#define _GNU_SOURCE
/* Private regression control: defer readiness of the first DMA-BUF for 30 ms.
 * Real writes already completed; this tests owner/event-loop behavior only.
 * Other descriptors continue to wake poll, including audio and cancellation. */
#include <dlfcn.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static _Atomic int producer = -1;
static _Atomic int64_t ready_at;

static int64_t now(void)
{
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time))
        abort();
    return (int64_t)time.tv_sec * 1000000000 + time.tv_nsec;
}

int poll(struct pollfd *fds, nfds_t count, int timeout)
{
    typeof(&poll) real_poll;
    void *symbol = dlsym(RTLD_NEXT, "poll");
    static_assert(sizeof(real_poll) == sizeof(symbol));
    memcpy(&real_poll, &symbol, sizeof(real_poll));
    if (!real_poll)
        abort();
    if (atomic_load_explicit(&producer, memory_order_relaxed) == -1 && count == 1 && timeout == 0 &&
        fds[0].fd >= 0 && (fds[0].events & POLLIN)) {
        char path[64], target[128];
        snprintf(path, sizeof(path), "/proc/self/fd/%d", fds[0].fd);
        ssize_t n = readlink(path, target, sizeof(target) - 1);
        if (n > 0) {
            target[n] = '\0';
            int expected = -1;
            if (strstr(target, "dmabuf") &&
                atomic_compare_exchange_strong(&producer, &expected, -2)) {
                atomic_store_explicit(&ready_at, now() + 30000000, memory_order_relaxed);
                atomic_store_explicit(&producer, fds[0].fd, memory_order_release);
                fputs("PRODUCER_WAIT first DMA-BUF readiness deferred for 30 ms\n", stderr);
            }
        }
    }
    int fd = atomic_load_explicit(&producer, memory_order_acquire);
    if (fd >= 0) {
        int64_t remaining = atomic_load_explicit(&ready_at, memory_order_relaxed) - now();
        if (remaining > 0)
            for (nfds_t i = 0; i < count; ++i)
                if (fds[i].fd == fd) {
                    int until_ready = (int)((remaining + 999999) / 1000000);
                    if (timeout < 0 || timeout > until_ready)
                        timeout = until_ready;
                    fds[i].fd = -1;
                    int result = real_poll(fds, count, timeout);
                    fds[i].fd = fd;
                    return result;
                }
    }
    return real_poll(fds, count, timeout);
}

#include "sink.h"

#include <errno.h>
#include <fcntl.h>
#include <liburing.h>
#include <pthread.h>
#include <stdbit.h>
#include <stdckdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/uio.h>
#include <tracy/TracyC.h>
#include <unistd.h>

struct slab {
    uint64_t offset;
    size_t size, completed;
    bool terminal;
};

struct gk_sink {
    struct gk_sink_options options;
    int fd, directory, ready, space;
    pthread_t thread;
    bool started;
    bool quarantine, fixed_io;
    uint8_t *bytes;
    struct slab *slabs;
    int64_t *accepted_at; /* Optional cold timing data, absent in ordinary recordings. */
    alignas(64) _Atomic uint32_t published;
    uint32_t next, cached_released;
    uint64_t logical;
    _Atomic bool waiting_space;
    alignas(64) _Atomic uint32_t released;
    _Atomic bool waiting_ready;
    _Atomic int error;
    _Atomic bool stop;
    struct gk_sink_stats stats;
};

static void latch(struct gk_sink *s, int error)
{
    int expected = 0;
    (void)atomic_compare_exchange_strong(&s->error, &expected, error);
    int ignored = gk_notify(s->space);
    (void)ignored;
}

static size_t write_size(struct gk_sink *s, const struct slab *b)
{
    size_t n = b->size - b->completed;
    if (s->options.max_write && n > s->options.max_write)
        n = s->options.max_write;
    if (s->options.fail_after) {
        uint64_t at = b->offset + b->completed;
        if (at >= s->options.fail_after)
            return 0;
        if (n > s->options.fail_after - at)
            n = (size_t)(s->options.fail_after - at);
    }
    return n;
}

static void release_prefix(struct gk_sink *s, uint32_t *read, uint32_t issued)
{
    uint32_t before = *read;
    while (*read != issued) {
        struct slab *b = &s->slabs[*read & (s->options.slabs - 1)];
        if (!b->terminal)
            break;
        /* Once a hole fails, later successes cannot advance W over it. */
        if (b->offset == s->stats.written)
            s->stats.written += b->completed;
        if (b->completed == b->size && s->options.measure)
            gk_hist_add(&s->stats.completion,
                        (uint64_t)(gk_now_ns() - s->accepted_at[*read & (s->options.slabs - 1)]));
        ++*read;
    }
    if (*read != before) {
        atomic_store_explicit(&s->released, *read, memory_order_seq_cst);
        if (atomic_load_explicit(&s->waiting_space, memory_order_seq_cst) &&
            atomic_exchange_explicit(&s->waiting_space, false, memory_order_relaxed)) {
            int ret = gk_notify(s->space);
            if (ret < 0)
                latch(s, ret);
        }
    }
}

static void buffered_write(struct gk_sink *s, uint32_t sequence)
{
    uint32_t i = sequence & (s->options.slabs - 1);
    struct slab *b = &s->slabs[i];
    TracyCZoneN(zone, "sink.pwrite", true);
    while (b->completed < b->size && !atomic_load(&s->error)) {
        size_t amount = write_size(s, b);
        if (!amount) {
            latch(s, -ENOSPC);
            break;
        }
        ssize_t n = pwrite(s->fd, s->bytes + (size_t)i * s->options.slab_size + b->completed,
                           amount, (off_t)(b->offset + b->completed));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            latch(s, n < 0 ? -errno : -EIO);
            break;
        }
        b->completed += (size_t)n;
    }
    b->terminal = true;
    TracyCZoneEnd(zone);
}

static int submit_slab(struct gk_sink *s, struct io_uring *ring, uint32_t sequence)
{
    uint32_t i = sequence & (s->options.slabs - 1);
    struct slab *b = &s->slabs[i];
    size_t n = write_size(s, b);
    if (!n)
        return -ENOSPC;
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (!sqe)
        return -ENOBUFS;
    uint8_t *buf = s->bytes + (size_t)i * s->options.slab_size + b->completed;
    if (s->fixed_io) {
        io_uring_prep_write_fixed(sqe, 0, buf, (unsigned)n, b->offset + b->completed, 0);
        sqe->flags |= IOSQE_FIXED_FILE;
    } else {
        io_uring_prep_write(sqe, s->fd, buf, (unsigned)n, b->offset + b->completed);
    }
    io_uring_sqe_set_data64(sqe, sequence);
    return 0;
}

static void *writer(void *opaque)
{
    struct gk_sink *s = opaque;
    (void)pthread_setname_np(pthread_self(), "gpukey-output");
    struct io_uring ring;
    bool uring = s->options.kind == GK_SINK_URING;
    if (uring) {
        /* The ring is created and driven by its actual single issuer. */
        struct io_uring_params params = {
            .flags =
                IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_NO_SQARRAY,
        };
        int ret = io_uring_queue_init_params(s->options.slabs, &ring, &params);
        if (ret < 0) {
            latch(s, ret);
            return nullptr;
        }
        struct iovec iov = {.iov_base = s->bytes,
                            .iov_len = s->options.slab_size * s->options.slabs};
        if (io_uring_register_files(&ring, &s->fd, 1) == 0 &&
            io_uring_register_buffers(&ring, &iov, 1) == 0)
            s->fixed_io = true;
    }
    uint32_t read = 0, issued = 0, pending = 0;
    for (;;) {
        uint32_t published = atomic_load_explicit(&s->published, memory_order_acquire);
        while (issued != published) {
            struct slab *b = &s->slabs[issued & (s->options.slabs - 1)];
            if (atomic_load(&s->error)) {
                b->terminal = true;
            } else if (uring) {
                int ret = submit_slab(s, &ring, issued);
                if (ret < 0) {
                    latch(s, ret);
                    b->terminal = true;
                } else {
                    ++pending;
                }
            } else {
                buffered_write(s, issued);
            }
            ++issued;
        }
        if (uring && pending) {
            int ret;
            do {
                ret = io_uring_submit_and_wait(&ring, 1);
            } while (ret == -EINTR);
            if (ret < 0) {
                latch(s, ret);
                /* A failed enter is not a completion. Synchronously retire every
                 * consumed request before unmapping the ring. Unconsumed SQEs
                 * cannot execute without this issuer (no SQPOLL). */
                struct io_uring_sync_cancel_reg cancel = {
                    .flags = IORING_ASYNC_CANCEL_ANY | IORING_ASYNC_CANCEL_ALL,
                    .timeout = {.tv_sec = -1, .tv_nsec = -1},
                };
                int cancelled;
                do {
                    cancelled = io_uring_register_sync_cancel(&ring, &cancel);
                } while (cancelled == -EINTR);
                /* ENOENT means no live matching request. If the runtime cannot
                 * establish quiescence, keep payload mappings until process exit;
                 * a bounded leak on a fatal kernel path beats unsafe reuse. */
                if (cancelled < 0 && cancelled != -ENOENT)
                    s->quarantine = true;
                break;
            }
            struct io_uring_cqe *cqe;
            while (io_uring_peek_cqe(&ring, &cqe) == 0) {
                uint32_t sequence = (uint32_t)io_uring_cqe_get_data64(cqe);
                struct slab *b = &s->slabs[sequence & (s->options.slabs - 1)];
                int result = cqe->res;
                io_uring_cqe_seen(&ring, cqe);
                --pending;
                if (result > 0 && (size_t)result <= b->size - b->completed)
                    b->completed += (size_t)result;
                else
                    latch(s, result < 0 ? result : -EIO);
                if (b->completed < b->size && !atomic_load(&s->error)) {
                    ret = submit_slab(s, &ring, sequence);
                    if (ret < 0)
                        latch(s, ret);
                    else
                        ++pending;
                }
                b->terminal = b->completed == b->size || atomic_load(&s->error);
            }
        }
        release_prefix(s, &read, issued);
        if (!pending && issued == atomic_load_explicit(&s->published, memory_order_acquire)) {
            if (atomic_load_explicit(&s->stop, memory_order_acquire))
                break;
            atomic_store_explicit(&s->waiting_ready, true, memory_order_seq_cst);
            if (issued == atomic_load_explicit(&s->published, memory_order_seq_cst) &&
                !atomic_load_explicit(&s->stop, memory_order_seq_cst)) {
                int ret = gk_wait(s->ready, -1);
                gk_drain_event(s->ready);
                if (ret < 0) {
                    latch(s, ret);
                    break;
                }
            } else {
                atomic_store_explicit(&s->waiting_ready, false, memory_order_relaxed);
            }
        }
    }
    if (uring)
        io_uring_queue_exit(&ring);
    if (!atomic_load(&s->error) && s->options.durable) {
        if (fsync(s->fd) < 0 || fsync(s->directory) < 0)
            latch(s, -errno);
        else
            s->stats.durable = s->stats.written;
    }
    return nullptr;
}

int gk_sink_open(struct gk_sink **out, const char *path, const struct gk_sink_options *options)
{
    *out = nullptr;
    size_t bytes;
    if (!stdc_has_single_bit(options->slabs) || options->slabs > 32768 || !options->slab_size ||
        options->slab_size > INT32_MAX || options->stall_ms < 1 ||
        ckd_mul(&bytes, options->slab_size, options->slabs))
        return -EINVAL;
    /* aligned_alloc's size must be a multiple of its requested alignment. */
    size_t aligned_size = (sizeof(struct gk_sink) + 63) & ~(size_t)63;
    struct gk_sink *s = aligned_alloc(64, aligned_size);
    if (!s)
        return -ENOMEM;
    *s = (struct gk_sink){.options = *options, .fd = -1, .directory = -1, .ready = -1, .space = -1};
    atomic_init(&s->published, 0);
    atomic_init(&s->released, 0);
    atomic_init(&s->error, 0);
    atomic_init(&s->stop, false);
    s->bytes = malloc(bytes);
    s->slabs = calloc(options->slabs, sizeof(*s->slabs));
    if (options->measure)
        s->accepted_at = calloc(options->slabs, sizeof(*s->accepted_at));
    int ret = -ENOMEM;
    if (!s->bytes || !s->slabs || (options->measure && !s->accepted_at))
        goto fail;
    s->ready = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    s->space = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (s->ready < 0 || s->space < 0) {
        ret = -errno;
        goto fail;
    }
    if (options->durable) {
        char *parent = strdup(path);
        if (!parent)
            goto fail;
        char *slash = strrchr(parent, '/');
        if (slash)
            slash[slash == parent ? 1 : 0] = '\0';
        s->directory = open(slash ? parent : ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        free(parent);
        if (s->directory < 0) {
            ret = -errno;
            goto fail;
        }
    }
    s->fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (s->fd < 0) {
        ret = -errno;
        goto fail;
    }
    ret = pthread_create(&s->thread, nullptr, writer, s);
    if (ret) {
        ret = -ret;
        goto fail;
    }
    s->started = true;
    *out = s;
    return 0;
fail: {
    int ignored = gk_sink_close(&s, nullptr);
    (void)ignored;
}
    return ret;
}

int gk_sink_write(void *opaque, const uint8_t *data, int size)
{
    struct gk_sink *s = opaque;
    if (size < 0 || (size && !data))
        return -EINVAL;
    int64_t deadline = gk_now_ns() + (int64_t)s->options.stall_ms * 1000000;
    size_t remaining = (size_t)size;
    while (remaining) {
        int error = atomic_load(&s->error);
        if (error)
            return error;
        while (s->next - s->cached_released == s->options.slabs) {
            s->cached_released = atomic_load_explicit(&s->released, memory_order_acquire);
            if (s->next - s->cached_released != s->options.slabs)
                break;
            atomic_store_explicit(&s->waiting_space, true, memory_order_seq_cst);
            s->cached_released = atomic_load_explicit(&s->released, memory_order_seq_cst);
            if (s->next - s->cached_released != s->options.slabs) {
                atomic_store_explicit(&s->waiting_space, false, memory_order_relaxed);
                break;
            }
            if ((error = atomic_load(&s->error)))
                return error;
            int64_t left = deadline - gk_now_ns();
            if (left <= 0) {
                latch(s, -ETIMEDOUT);
                return -ETIMEDOUT;
            }
            error = gk_wait(s->space, (int)((left + 999999) / 1000000));
            gk_drain_event(s->space);
            if (error < 0) {
                latch(s, error);
                return error;
            }
        }
        size_t n = remaining < s->options.slab_size ? remaining : s->options.slab_size;
        if (s->logical > INT64_MAX - n)
            return -EOVERFLOW;
        uint32_t i = s->next & (s->options.slabs - 1);
        memcpy(s->bytes + (size_t)i * s->options.slab_size, data, n);
        s->slabs[i] = (struct slab){.offset = s->logical, .size = n};
        if (s->options.measure)
            s->accepted_at[i] = gk_now_ns();
        s->logical += n;
        data += n;
        remaining -= n;
        uint64_t occupancy = (uint64_t)(s->next + 1 - s->cached_released) * s->options.slab_size;
        if (occupancy > s->stats.peak_pending)
            s->stats.peak_pending = occupancy;
        atomic_store_explicit(&s->published, ++s->next, memory_order_seq_cst);
        if (atomic_load_explicit(&s->waiting_ready, memory_order_seq_cst) &&
            atomic_exchange_explicit(&s->waiting_ready, false, memory_order_relaxed)) {
            error = gk_notify(s->ready);
            if (error < 0) {
                latch(s, error);
                return error;
            }
        }
    }
    return size;
}

int gk_sink_error(const struct gk_sink *s)
{
    return s ? atomic_load(&s->error) : 0;
}

int gk_sink_close(struct gk_sink **sink, struct gk_sink_stats *stats)
{
    struct gk_sink *s = *sink;
    if (!s)
        return 0;
    atomic_store_explicit(&s->stop, true, memory_order_release);
    if (s->started) {
        int ignored = gk_notify(s->ready);
        (void)ignored;
        (void)pthread_join(s->thread, nullptr);
    }
    int ret = atomic_load(&s->error);
    if (s->fd >= 0 && close(s->fd) < 0 && !ret)
        ret = -errno;
    if (s->directory >= 0)
        close(s->directory);
    if (s->ready >= 0)
        close(s->ready);
    if (s->space >= 0)
        close(s->space);
    s->stats.accepted = s->logical;
    if (stats)
        *stats = s->stats;
    free(s->slabs);
    free(s->accepted_at);
    if (!s->quarantine)
        free(s->bytes);
    free(s);
    *sink = nullptr;
    return ret;
}

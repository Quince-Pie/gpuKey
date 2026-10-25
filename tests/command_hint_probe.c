/* Core lookup/growth functions are extracted verbatim from each Mesa candidate.
 * The model supplies owned buffer lists and a deterministic allocator binding.
 * It is not a GPU benchmark; native driver integration is checked separately. */
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef GK_HINT_MODE
#error Select the extracted implementation.
#endif

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
#define BUFFER_HASHLIST_SIZE 32768
#define NUM_BO_LIST_TYPES 3
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define MAX2(a, b) ((a) > (b) ? (a) : (b))
#define MIN2(a, b) ((a) < (b) ? (a) : (b))
#define unlikely(x) __builtin_expect(!!(x), 0)
#define mesa_loge(...) fprintf(stderr, __VA_ARGS__)
#define p_atomic_inc(p) ((void)atomic_fetch_add_explicit((p), 1, memory_order_acq_rel))

struct amdgpu_winsys_bo {
    alignas(64) struct {
        struct {
            atomic_uint count;
        } reference;
    } base;
    uint32_t unique_id;
    unsigned type;
};
struct amdgpu_cs_buffer {
    struct amdgpu_winsys_bo *bo;
    unsigned usage;
};
static_assert(sizeof(struct amdgpu_winsys_bo) == 64);
static_assert(sizeof(struct amdgpu_cs_buffer) == 16);
struct amdgpu_buffer_list {
    unsigned max_buffers, num_buffers;
    struct amdgpu_cs_buffer *buffers;
#if GK_HINT_MODE == 3
    uint32_t *buffer_map;
    size_t map_capacity;
#endif
};
struct amdgpu_cs_context {
    struct amdgpu_buffer_list buffer_lists[3];
#if GK_HINT_MODE != 3
    int16_t *buffer_indices_hashlist;
#endif
#if GK_HINT_MODE == 2
    unsigned buffer_hash_mask;
#endif
#if GK_HINT_MODE == 4
    bool hash_negative_valid;
#endif
};

static thread_local unsigned fail_hint;
static thread_local unsigned denied;
static void *resize(void *old, size_t old_size, size_t new_size, bool hint)
{
    (void)old_size;
    CHECK(new_size);
    if (hint && fail_hint) {
        --fail_hint;
        ++denied;
        return nullptr;
    }
    return realloc(old, new_size);
}
#define REALLOC(p, o, n) resize((p), (o), (n), false)
#define HINT_REALLOC(p, o, n) resize((p), (o), (n), true)
static unsigned get_buf_list_idx(struct amdgpu_winsys_bo *bo)
{
    return MIN2(bo->type, 2U);
}

#include "command_hint_core.h"

struct model {
    struct amdgpu_cs_context c[2];
};

static void init(struct model *m)
{
    *m = (struct model){};
#if GK_HINT_MODE == 0 || GK_HINT_MODE == 1 || GK_HINT_MODE == 4
    m->c[0].buffer_indices_hashlist = malloc(65536);
    CHECK(m->c[0].buffer_indices_hashlist);
    memset(m->c[0].buffer_indices_hashlist, -1, 65536);
#if GK_HINT_MODE == 0 || GK_HINT_MODE == 4
    m->c[1].buffer_indices_hashlist = m->c[0].buffer_indices_hashlist;
#else
    m->c[1].buffer_indices_hashlist = malloc(65536);
    CHECK(m->c[1].buffer_indices_hashlist);
    memset(m->c[1].buffer_indices_hashlist, -1, 65536);
#endif
#endif
#if GK_HINT_MODE == 4
    m->c[0].hash_negative_valid = m->c[1].hash_negative_valid = true;
#endif
}

static void drop_lists(struct amdgpu_cs_context *c)
{
    for (unsigned kind = 0; kind < 3; ++kind) {
        struct amdgpu_buffer_list *list = &c->buffer_lists[kind];
        for (unsigned i = 0; i < list->num_buffers; ++i)
            CHECK(atomic_fetch_sub_explicit(&list->buffers[i].bo->base.reference.count, 1,
                                            memory_order_acq_rel) > 1);
        list->num_buffers = 0;
    }
}

static void clear_cache(struct amdgpu_cs_context *c)
{
#if GK_HINT_MODE == 3
    for (unsigned kind = 0; kind < 3; ++kind) {
        struct amdgpu_buffer_list *list = &c->buffer_lists[kind];
        list->map_capacity &= SIZE_MAX >> 1;
        if (list->buffer_map)
            memset(list->buffer_map, 0, list->map_capacity * sizeof(uint32_t));
    }
#endif
#if GK_HINT_MODE == 0 || GK_HINT_MODE == 1
    memset(c->buffer_indices_hashlist, -1, 65536);
#elif GK_HINT_MODE == 4
    size_t count = 0;
    for (unsigned kind = 0; kind < 3; ++kind)
        count += c->buffer_lists[kind].num_buffers;
    if (count > 256) {
        memset(c->buffer_indices_hashlist, -1, 65536);
    } else {
        for (unsigned kind = 0; kind < 3; ++kind) {
            struct amdgpu_buffer_list *list = &c->buffer_lists[kind];
            for (unsigned i = 0; i < list->num_buffers; ++i)
                c->buffer_indices_hashlist[list->buffers[i].bo->unique_id & 32767] = -1;
        }
    }
#elif GK_HINT_MODE == 2
    c->buffer_hash_mask &= BUFFER_HASHLIST_SIZE - 1;
    if (c->buffer_indices_hashlist)
        memset(c->buffer_indices_hashlist, -1, (c->buffer_hash_mask + 1) * sizeof(int16_t));
#endif
}

static void reset(struct amdgpu_cs_context *c)
{
#if GK_HINT_MODE == 4
    clear_cache(c);
    drop_lists(c);
#else
    drop_lists(c);
    clear_cache(c);
#endif
}

static void destroy(struct model *m)
{
    for (unsigned j = 0; j < 2; ++j) {
        reset(&m->c[j]);
        for (unsigned kind = 0; kind < 3; ++kind) {
            free(m->c[j].buffer_lists[kind].buffers);
#if GK_HINT_MODE == 3
            free(m->c[j].buffer_lists[kind].buffer_map);
#endif
        }
    }
#if GK_HINT_MODE != 3
    free(m->c[0].buffer_indices_hashlist);
#if GK_HINT_MODE != 0 && GK_HINT_MODE != 4
    free(m->c[1].buffer_indices_hashlist);
#endif
#endif
}

static struct amdgpu_winsys_bo *objects(unsigned n, bool collide, bool single)
{
    struct amdgpu_winsys_bo *bo = aligned_alloc(64, (size_t)n * sizeof(*bo));
    CHECK(bo);
    for (unsigned i = 0; i < n; ++i) {
        bo[i].unique_id = collide ? i * 32768U : i;
        bo[i].type = single ? 2 : i % 5;
        atomic_init(&bo[i].base.reference.count, 1);
    }
    return bo;
}

static uint32_t random32(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *state = x;
}

static void check_case(unsigned n, bool collide, bool single, bool fail)
{
    struct model m;
    init(&m);
    struct amdgpu_winsys_bo *bo = objects(n, collide, single);
    unsigned *order = malloc((size_t)n * sizeof(*order));
    unsigned *expected = malloc((size_t)n * sizeof(*expected));
    CHECK(order && expected);
    uint32_t rng = 0x51c0deda;
    unsigned failures_before = denied;
    for (unsigned epoch = 0; epoch < (n > 4096 ? 1U : 5U); ++epoch) {
        struct amdgpu_cs_context *c = &m.c[epoch & 1];
        reset(c);
        unsigned counts[3] = {};
        for (unsigned i = 0; i < n; ++i) {
            order[i] = i;
            expected[i] = UINT32_MAX;
        }
        for (unsigned i = n; i > 1; --i) {
            unsigned j = random32(&rng) % i, tmp = order[i - 1];
            order[i - 1] = order[j];
            order[j] = tmp;
        }
        fail_hint = fail && epoch < 2 ? 1 : 0;
        for (unsigned at = 0; at < n; ++at) {
            unsigned id = order[at], kind = get_buf_list_idx(&bo[id]);
            struct amdgpu_buffer_list *list = &c->buffer_lists[kind];
            struct amdgpu_cs_buffer *entry = amdgpu_lookup_or_add_buffer(c, &bo[id], list, true);
            CHECK(entry && entry->bo == &bo[id]);
            expected[id] = counts[kind]++;
            CHECK(entry == &list->buffers[expected[id]] && list->num_buffers == counts[kind]);
            entry->usage = id ^ epoch;
            CHECK(amdgpu_lookup_buffer_any_type(c, &bo[id]) == entry);
        }
        /* Independent object-index oracle; neither UID hashing nor hint indices
         * determine expected membership. Duplicate UIDs are legal collisions. */
        for (unsigned k = 0; k < MIN2(n, n > 4096 ? 256U : 4096U); ++k) {
            unsigned id = random32(&rng) % n, kind = get_buf_list_idx(&bo[id]);
            struct amdgpu_cs_buffer *entry = amdgpu_lookup_buffer_any_type(c, &bo[id]);
            CHECK(entry == &c->buffer_lists[kind].buffers[expected[id]]);
            CHECK(entry->usage == (id ^ epoch));
        }
        struct amdgpu_winsys_bo absent = {.unique_id = bo[0].unique_id, .type = bo[0].type};
        CHECK(!amdgpu_lookup_buffer_any_type(c, &absent));
    }
#if GK_HINT_MODE == 2 || GK_HINT_MODE == 3
    if (fail && n >= 64)
        CHECK(denied > failures_before);
#else
    (void)failures_before;
#endif
    fail_hint = 0;
    destroy(&m);
    for (unsigned i = 0; i < n; ++i)
        CHECK(atomic_load_explicit(&bo[i].base.reference.count, memory_order_relaxed) == 1);
    free(bo);
    free(expected);
    free(order);
}

struct handoff {
    struct model m;
    struct amdgpu_winsys_bo *bo;
    atomic_uint stage;
};
static void *worker(void *opaque)
{
    struct handoff *h = opaque;
    struct amdgpu_cs_context *c = &h->m.c[0];
#if GK_HINT_MODE == 4
    c->hash_negative_valid = false;
#endif
    for (unsigned i = 0; i < 64; ++i)
        CHECK(amdgpu_lookup_or_add_buffer(c, &h->bo[i], &c->buffer_lists[2], true));
    atomic_store_explicit(&h->stage, 1, memory_order_release);
    while (atomic_load_explicit(&h->stage, memory_order_acquire) != 2)
        sched_yield();
    /* Legal interleaving: frontend clears the other recording context while
     * submission resumes resolving an already-added backing buffer. */
    CHECK(amdgpu_lookup_buffer_any_type(c, h->bo) == &c->buffer_lists[2].buffers[0]);
    return nullptr;
}

static void handoff(void)
{
    struct handoff h = {.bo = objects(64, false, true)};
    init(&h.m);
    atomic_init(&h.stage, 0);
    pthread_t thread;
    CHECK(pthread_create(&thread, nullptr, worker, &h) == 0);
    while (atomic_load_explicit(&h.stage, memory_order_acquire) != 1)
        sched_yield();
    for (unsigned i = 0; i < 64; ++i)
        CHECK(amdgpu_lookup_or_add_buffer(&h.m.c[1], &h.bo[i], &h.m.c[1].buffer_lists[2], true));
    reset(&h.m.c[1]);
    atomic_store_explicit(&h.stage, 2, memory_order_release);
    void *result;
    CHECK(pthread_join(thread, &result) == 0 && result == nullptr);
    destroy(&h.m);
    free(h.bo);
}

struct queue_model {
    struct model m;
    struct amdgpu_winsys_bo *bo;
    atomic_uint published, started, finished;
    unsigned worker_failures;
};

static void *submit_worker(void *opaque)
{
    struct queue_model *q = opaque;
    for (unsigned phase = 1; phase <= 200; ++phase) {
        while (atomic_load_explicit(&q->published, memory_order_acquire) < phase)
            sched_yield();
        unsigned slot = (phase - 1) & 1;
        struct amdgpu_cs_context *c = &q->m.c[slot];
        atomic_store_explicit(&q->started, phase, memory_order_release);
        fail_hint = phase <= 2 ? 1 : 0;
        /* Submission may add backing buffers and grow its own cache while the
         * frontend records the other context. Include real growth failures. */
        for (unsigned i = 0; i < 48; ++i) {
            struct amdgpu_winsys_bo *bo = &q->bo[128 + slot * 48 + i];
            CHECK(amdgpu_lookup_or_add_buffer(c, bo, &c->buffer_lists[2], true));
        }
        for (unsigned repeat = 0; repeat < 32; ++repeat) {
            for (unsigned i = 0; i < 64; ++i) {
                struct amdgpu_winsys_bo *bo = &q->bo[slot * 64 + i];
                struct amdgpu_cs_buffer *entry = amdgpu_lookup_buffer_any_type(c, bo);
                CHECK(entry && entry->bo == bo);
            }
        }
        drop_lists(c);
        atomic_store_explicit(&q->finished, phase, memory_order_release);
    }
    q->worker_failures = denied;
    return nullptr;
}

static void fill_slot(struct queue_model *q, unsigned slot)
{
    struct amdgpu_cs_context *c = &q->m.c[slot];
    CHECK(c->buffer_lists[2].num_buffers == 0);
#if GK_HINT_MODE == 4
    c->hash_negative_valid = true;
#else
    clear_cache(c);
#endif
    for (unsigned at = 0; at < 64; ++at) {
        unsigned i = slot ? 63 - at : at;
        struct amdgpu_winsys_bo *bo = &q->bo[slot * 64 + i];
        CHECK(amdgpu_lookup_or_add_buffer(c, bo, &c->buffer_lists[2], true));
    }
}

static void race(void)
{
    struct queue_model q = {.bo = objects(224, false, true)};
    init(&q.m);
    for (unsigned i = 64; i < 128; ++i)
        q.bo[i].unique_id = i - 64 + 65536;
    for (unsigned i = 128; i < 224; ++i)
        q.bo[i].unique_id = i - 128 + 131072;
    atomic_init(&q.published, 0);
    atomic_init(&q.started, 0);
    atomic_init(&q.finished, 0);
    fill_slot(&q, 0);
    pthread_t worker_thread;
    CHECK(pthread_create(&worker_thread, nullptr, submit_worker, &q) == 0);
    for (unsigned phase = 1; phase <= 200; ++phase) {
        unsigned slot = (phase - 1) & 1;
#if GK_HINT_MODE == 4
        /* Exactly the shared candidate's safe reset interval: the preceding
         * job has finished and the next job has not been published. */
        clear_cache(&q.m.c[slot]);
        q.m.c[slot].hash_negative_valid = false;
#endif
        atomic_store_explicit(&q.published, phase, memory_order_release);
        while (atomic_load_explicit(&q.started, memory_order_acquire) < phase)
            sched_yield();
        fill_slot(&q, !slot);
        while (atomic_load_explicit(&q.finished, memory_order_acquire) < phase)
            sched_yield();
    }
    void *result;
    CHECK(pthread_join(worker_thread, &result) == 0 && result == nullptr);
#if GK_HINT_MODE == 2 || GK_HINT_MODE == 3
    CHECK(q.worker_failures >= 2);
#endif
    destroy(&q.m);
    for (unsigned i = 0; i < 224; ++i)
        CHECK(atomic_load_explicit(&q.bo[i].base.reference.count, memory_order_relaxed) == 1);
    free(q.bo);
    puts("concurrent recording/submission and paused-context reuse pass");
}

static uint64_t now_ns(void)
{
    struct timespec time;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &time) == 0);
    return (uint64_t)time.tv_sec * 1'000'000'000 + (uint64_t)time.tv_nsec;
}

static size_t cache_bytes(const struct model *m)
{
#if GK_HINT_MODE == 0 || GK_HINT_MODE == 4
    (void)m;
    return 65536;
#elif GK_HINT_MODE == 1
    (void)m;
    return 131072;
#else
    size_t bytes = 0;
    for (unsigned c = 0; c < 2; ++c) {
#if GK_HINT_MODE == 2
        if (m->c[c].buffer_indices_hashlist)
            bytes += (m->c[c].buffer_hash_mask + 1) * sizeof(int16_t);
#else
        for (unsigned kind = 0; kind < 3; ++kind)
            bytes += m->c[c].buffer_lists[kind].map_capacity * sizeof(uint32_t);
#endif
    }
    return bytes;
#endif
}

/* Complete sequential batches, including insertion, two independently ordered
 * lookups per object, refcount release and reset. This isolates table policy;
 * queue contention, real BO layout and GPU work require native measurements. */
static uint64_t bench_batch(struct amdgpu_cs_context *c, struct amdgpu_winsys_bo *bo,
                            const unsigned *order, unsigned n)
{
    uint64_t sum = 0;
    for (unsigned at = 0; at < n; ++at) {
        unsigned id = order[at];
        struct amdgpu_buffer_list *list = &c->buffer_lists[get_buf_list_idx(&bo[id])];
        struct amdgpu_cs_buffer *entry = amdgpu_lookup_or_add_buffer(c, &bo[id], list, true);
        CHECK(entry);
        entry->usage = id;
    }
    for (unsigned at = 0; at < n; ++at) {
        struct amdgpu_cs_buffer *a = amdgpu_lookup_buffer_any_type(c, &bo[at]);
        struct amdgpu_cs_buffer *b = amdgpu_lookup_buffer_any_type(c, &bo[order[n - at - 1]]);
        CHECK(a && b);
        sum += a->usage;
        sum += b->usage;
    }
    reset(c);
    return sum;
}

static void benchmark(void)
{
    puts("count,colliding,single_list,batches,cache_bytes,cold_pair_ns,ns_per_batch");
    const unsigned counts[] = {8, 32, 256, 4096, 33024};
    for (unsigned ci = 0; ci < ARRAY_SIZE(counts); ++ci) {
        unsigned n = counts[ci];
        for (unsigned collide = 0; collide < (n <= 4096 ? 2U : 1U); ++collide) {
            for (unsigned single = 0; single < 2; ++single) {
                struct amdgpu_winsys_bo *bo = objects(n, collide != 0, single != 0);
                unsigned *order = malloc((size_t)n * sizeof(*order));
                CHECK(order);
                uint32_t rng = 0x9e3779b9;
                for (unsigned i = 0; i < n; ++i)
                    order[i] = i;
                for (unsigned i = n; i > 1; --i) {
                    unsigned j = random32(&rng) % i, tmp = order[i - 1];
                    order[i - 1] = order[j];
                    order[j] = tmp;
                }
                struct model m;
                uint64_t start = now_ns();
                init(&m);
                uint64_t sum = bench_batch(&m.c[0], bo, order, n);
                sum += bench_batch(&m.c[1], bo, order, n);
                uint64_t cold_ns = now_ns() - start;
                unsigned batches = MAX2(16U, (collide ? 65536U : 1048576U) / n);
                start = now_ns();
                for (unsigned batch = 0; batch < batches; ++batch)
                    sum += bench_batch(&m.c[batch & 1], bo, order, n);
                uint64_t elapsed = now_ns() - start;
                CHECK(sum == (uint64_t)(batches + 2) * n * (n - 1));
                printf("%u,%u,%u,%u,%zu,%llu,%.3f\n", n, collide, single, batches, cache_bytes(&m),
                       (unsigned long long)cold_ns, (double)elapsed / batches);
                destroy(&m);
                for (unsigned i = 0; i < n; ++i)
                    CHECK(atomic_load_explicit(&bo[i].base.reference.count, memory_order_relaxed) ==
                          1);
                free(order);
                free(bo);
            }
        }
    }
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "bench")) {
        benchmark();
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "race")) {
        race();
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "handoff")) {
        handoff();
        puts("handoff pass");
        return 0;
    }
    CHECK(argc == 1);
    const unsigned counts[] = {1, 8, 9, 16, 64, 257, 4096, 33024, 66048};
    for (unsigned i = 0; i < ARRAY_SIZE(counts); ++i) {
        check_case(counts[i], false, false, false);
        check_case(counts[i], false, true, false);
        if (counts[i] <= 4096)
            check_case(counts[i], true, false, false);
        if (counts[i] <= 257)
            check_case(counts[i], false, false, true);
    }
    puts("hint core: independent membership, growth, collision, large-index, allocation-failure "
         "and teardown checks pass");
}

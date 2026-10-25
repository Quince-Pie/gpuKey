/* Independent lifetime oracle for the actual FFmpeg pool implementations. */
#include <errno.h>
#include <libavutil/buffer.h>
#include <libavutil/error.h>
#include <libavutil/refstruct.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef AV_HAVE_BUFFER_POOL_TRIM
#error This probe requires the pool-trim candidate headers.
#endif
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                \
            abort();                                                                               \
        }                                                                                          \
    } while (false)
constexpr unsigned workers = 4;
constexpr unsigned rounds = 1000;
constexpr unsigned oracle_count = workers * rounds + 32;
struct element {
    unsigned id, value;
};
struct arena {
    bool refstruct;
    union {
        AVBufferPool *buffer;
        AVRefStructPool *ref;
    } pool;
    atomic_uint created, destroyed, closed;
    atomic_bool fail_next, destroy_on_free;
    atomic_uchar live[oracle_count];
};
struct lease {
    AVBufferRef *buffer;
    struct element *value;
};

static void uninit(struct arena *a)
{
    if (a->refstruct)
        av_refstruct_pool_uninit(&a->pool.ref);
    else
        av_buffer_pool_uninit(&a->pool.buffer);
}

static void create_element(struct arena *a, struct element *p)
{
    p->id = atomic_fetch_add(&a->created, 1);
    CHECK(p->id < oracle_count);
    CHECK(atomic_exchange(&a->live[p->id], 1) == 0);
    p->value = p->id ^ 0x5a5a5a5aU;
}

static void destroy_element(struct arena *a, struct element *p)
{
    CHECK(p->value == (p->id ^ 0x5a5a5a5aU));
    CHECK(atomic_exchange(&a->live[p->id], 0) == 1);
    atomic_fetch_add(&a->destroyed, 1);
    if (atomic_exchange(&a->destroy_on_free, false))
        uninit(a);
}

static void buffer_free(void *opaque, uint8_t *data)
{
    destroy_element(opaque, (struct element *)data);
    free_sized(data, sizeof(struct element));
}

static AVBufferRef *buffer_alloc(void *opaque, size_t size)
{
    struct arena *a = opaque;
    CHECK(size == sizeof(struct element));
    if (atomic_exchange(&a->fail_next, false))
        return nullptr;
    struct element *p = malloc(size);
    CHECK(p);
    create_element(a, p);
    AVBufferRef *ref = av_buffer_create((uint8_t *)p, size, buffer_free, a, 0);
    if (!ref)
        buffer_free(a, (uint8_t *)p);
    return ref;
}

static void buffer_closed(void *opaque)
{
    struct arena *a = opaque;
    CHECK(atomic_load(&a->destroyed) == atomic_load(&a->created));
    CHECK(atomic_fetch_add(&a->closed, 1) == 0);
}

static int ref_init(AVRefStructOpaque opaque, void *obj)
{
    struct arena *a = opaque.nc;
    if (atomic_exchange(&a->fail_next, false))
        return AVERROR(ENOMEM);
    create_element(a, obj);
    return 0;
}
static void ref_free(AVRefStructOpaque opaque, void *obj)
{
    destroy_element(opaque.nc, obj);
}
static void ref_closed(AVRefStructOpaque opaque)
{
    buffer_closed(opaque.nc);
}

static void init_flags(struct arena *a, bool refstruct, unsigned flags)
{
    *a = (struct arena){.refstruct = refstruct};
    atomic_init(&a->created, 0);
    atomic_init(&a->destroyed, 0);
    atomic_init(&a->closed, 0);
    atomic_init(&a->fail_next, false);
    atomic_init(&a->destroy_on_free, false);
    for (unsigned i = 0; i < oracle_count; ++i)
        atomic_init(&a->live[i], 0);
    if (refstruct) {
        a->pool.ref = av_refstruct_pool_alloc_ext(sizeof(struct element), flags, a, ref_init,
                                                  nullptr, ref_free, ref_closed);
        CHECK(a->pool.ref);
    } else {
        CHECK(flags == 0);
        a->pool.buffer =
            av_buffer_pool_init2(sizeof(struct element), a, buffer_alloc, buffer_closed);
        CHECK(a->pool.buffer);
    }
}

static void init(struct arena *a, bool refstruct)
{
    init_flags(a, refstruct, 0);
}

static struct lease get(struct arena *a)
{
    if (a->refstruct)
        return (struct lease){.value = av_refstruct_pool_get(a->pool.ref)};
    AVBufferRef *ref = av_buffer_pool_get(a->pool.buffer);
    return (struct lease){ref, ref ? (void *)ref->data : nullptr};
}
static void put(struct arena *a, struct lease *lease)
{
    if (a->refstruct)
        av_refstruct_unref(&lease->value);
    else
        av_buffer_unref(&lease->buffer);
    *lease = (struct lease){};
}
static size_t trim(struct arena *a, size_t keep)
{
    return a->refstruct ? av_refstruct_pool_trim(a->pool.ref, keep)
                        : av_buffer_pool_trim(a->pool.buffer, keep);
}
static void verify(const struct arena *a)
{
    CHECK(atomic_load(&a->created) == atomic_load(&a->destroyed));
    CHECK(atomic_load(&a->closed) == 1);
    for (unsigned i = 0; i < oracle_count; ++i)
        CHECK(!atomic_load(&a->live[i]));
}

static void serial(bool kind)
{
    struct arena a;
    init(&a, kind);
    CHECK(trim(&a, 0) == 0);
    struct lease x[6];
    for (unsigned i = 0; i < 6; ++i) {
        x[i] = get(&a);
        CHECK(x[i].value);
    }
    CHECK(trim(&a, 0) == 0);
    struct lease clone = kind ? (struct lease){.value = av_refstruct_ref(x[0].value)}
                              : (struct lease){av_buffer_ref(x[0].buffer), x[0].value};
    CHECK(kind || clone.buffer);
    for (unsigned i = 0; i < 3; ++i)
        put(&a, &x[i]);
    CHECK(trim(&a, 1) == 1 && atomic_load(&a.destroyed) == 1);
    CHECK(clone.value->value == (clone.value->id ^ 0x5a5a5a5aU));
    put(&a, &clone);
    CHECK(trim(&a, 0) == 2);
    atomic_store(&a.fail_next, true);
    CHECK(!get(&a).value);
    struct lease retry = get(&a);
    CHECK(retry.value);
    put(&a, &retry);
    uninit(&a);
    CHECK(!atomic_load(&a.closed));
    for (unsigned i = 3; i < 6; ++i)
        put(&a, &x[i]);
    verify(&a);
}

static void reentrant(bool kind, bool held)
{
    struct arena a;
    init(&a, kind);
    struct lease x = get(&a), y = get(&a);
    CHECK(x.value && y.value);
    put(&a, &x);
    if (!held)
        put(&a, &y);
    atomic_store(&a.destroy_on_free, true);
    CHECK(trim(&a, 0) == (held ? 1U : 2U));
    if (held) {
        CHECK(!atomic_load(&a.closed));
        put(&a, &y);
    }
    verify(&a);
}

static void *run_worker(void *opaque)
{
    struct arena *a = opaque;
    for (unsigned i = 0; i < rounds; ++i) {
        struct lease x = get(a);
        CHECK(x.value);
        CHECK(x.value->value == (x.value->id ^ 0x5a5a5a5aU));
        x.value->value = x.value->id ^ 0x5a5a5a5aU;
        put(a, &x);
    }
    return nullptr;
}
static void concurrent(bool kind, unsigned flags)
{
    struct arena a;
    init_flags(&a, kind, flags);
    pthread_t threads[workers];
    for (unsigned i = 0; i < workers; ++i)
        CHECK(!pthread_create(&threads[i], nullptr, run_worker, &a));
    for (unsigned i = 0; i < rounds; ++i)
        (void)trim(&a, i % 2);
    for (unsigned i = 0; i < workers; ++i)
        CHECK(!pthread_join(threads[i], nullptr));
    uninit(&a);
    verify(&a);
}

#ifdef AV_REFSTRUCT_POOL_FLAG_SINGLE_IDLE
static void single_idle(void)
{
    struct arena a;
    init_flags(&a, true, AV_REFSTRUCT_POOL_FLAG_SINGLE_IDLE);
    struct lease x[4];
    for (unsigned i = 0; i < 4; ++i) {
        x[i] = get(&a);
        CHECK(x[i].value);
    }
    unsigned newest = x[1].value->id;
    put(&a, &x[0]);
    CHECK(atomic_load(&a.destroyed) == 0);
    put(&a, &x[1]);
    CHECK(atomic_load(&a.destroyed) == 1);
    struct lease clone = {.value = av_refstruct_ref(x[2].value)};
    put(&a, &x[2]);
    CHECK(atomic_load(&a.destroyed) == 1);
    struct lease reused = get(&a);
    CHECK(reused.value && reused.value->id == newest && atomic_load(&a.created) == 4);
    CHECK(trim(&a, 0) == 0);
    put(&a, &reused);
    put(&a, &clone);
    CHECK(atomic_load(&a.destroyed) == 2);
    /* Evicting the old cached object destroys the pool owner. Its callback
     * destroys the just-returned object's storage in place, while the return
     * routine is still executing. The existing lease must pin the pool. */
    atomic_store(&a.destroy_on_free, true);
    put(&a, &x[3]);
    CHECK(a.pool.ref == nullptr);
    verify(&a);
    concurrent(true, AV_REFSTRUCT_POOL_FLAG_SINGLE_IDLE);
}
#endif

int main(int argc, char **argv)
{
    unsigned first = 0, end = 2;
    if (argc == 2) {
        CHECK(!strcmp(argv[1], "buffer") || !strcmp(argv[1], "refstruct"));
        first = !strcmp(argv[1], "refstruct");
        end = first + 1;
    } else {
        CHECK(argc == 1);
    }
    for (unsigned kind = first; kind < end; ++kind) {
        serial(kind != 0);
        reentrant(kind != 0, false);
        reentrant(kind != 0, true);
        concurrent(kind != 0, 0);
    }
#ifdef AV_REFSTRUCT_POOL_FLAG_SINGLE_IDLE
    if (end == 2)
        single_idle();
#endif
    puts("Both pool trims: held references, keep/empty, allocation failure, concurrent reuse, "
         "reentrant owner destruction and final teardown pass.");
}

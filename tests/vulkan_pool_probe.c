/* Exercise the exact lazy-pool helper extracted from the delivered FFmpeg patch.
 * The mock supplies completed/failed Vulkan construction; it does not model
 * command execution or claim GPU/concurrent-transfer coverage. */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AVERROR(value) (-(value))
typedef struct FFVkExecPool {
    struct FFVkExecPool *parent;
    uint64_t initialized;
    int contexts;
} FFVkExecPool;
typedef struct AVVulkanDeviceQueueFamily {
    int num, kind;
} AVVulkanDeviceQueueFamily;
typedef struct VulkanDevicePriv {
    int vkctx;
    AVVulkanDeviceQueueFamily *compute_qf, *transfer_qf;
} VulkanDevicePriv;
typedef struct VulkanFramesPriv {
    _Atomic(FFVkExecPool *) execution[2];
    pthread_mutex_t execution_mutex;
} VulkanFramesPriv;
typedef struct AVHWDeviceContext {
    void *hwctx;
} AVHWDeviceContext;
typedef struct AVHWFramesContext {
    void *hwctx;
    AVHWDeviceContext *device_ctx;
} AVHWFramesContext;

static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
static bool pause_compute, entered, resume;
static atomic_bool fail_next;
static atomic_uint calls[2];

static void *av_mallocz(size_t size)
{
    return calloc(1, size);
}

static void av_freep(void *address)
{
    void *pointer;
    memcpy(&pointer, address, sizeof(pointer));
    free(pointer);
    pointer = nullptr;
    memcpy(address, &pointer, sizeof(pointer));
}

static int ff_vk_exec_pool_init(int *context, AVVulkanDeviceQueueFamily *queue, FFVkExecPool *pool,
                                int contexts, int queries, int query_type, int query_64bit,
                                const void *next)
{
    (void)context;
    assert(!queries && !query_type && !query_64bit && !next);
    atomic_fetch_add(&calls[queue->kind], 1);
    if (atomic_exchange(&fail_next, false))
        return AVERROR(ENOMEM);
    if (queue->kind == 0 && pause_compute) {
        assert(!pthread_mutex_lock(&gate));
        entered = true;
        assert(!pthread_cond_broadcast(&condition));
        while (!resume)
            assert(!pthread_cond_wait(&condition, &gate));
        assert(!pthread_mutex_unlock(&gate));
    }
    pool->parent = pool;
    pool->contexts = contexts;
    pool->initialized = UINT64_C(0x600d12345678);
    return 0;
}

#include "vulkan-frame-execution.inc"

struct worker {
    AVHWFramesContext *frames;
    FFVkExecPool *result;
    bool await_publication;
};

static void *acquire_compute(void *opaque)
{
    struct worker *worker = opaque;
    VulkanFramesPriv *frames = worker->frames->hwctx;
    if (worker->await_publication)
        while (!atomic_load_explicit(&frames->execution[FRAME_COMPUTE], memory_order_relaxed))
            sched_yield();
    assert(!vulkan_frame_execution(worker->frames, FRAME_COMPUTE, &worker->result));
    assert(worker->result->initialized == UINT64_C(0x600d12345678));
    assert(worker->result->parent == worker->result && worker->result->contexts == 1);
    return nullptr;
}

int main(void)
{
    AVVulkanDeviceQueueFamily compute = {.num = 1, .kind = 0};
    AVVulkanDeviceQueueFamily transfer = {.num = 1, .kind = 1};
    VulkanDevicePriv device = {.compute_qf = &compute, .transfer_qf = &transfer};
    AVHWDeviceContext device_context = {.hwctx = &device};
    VulkanFramesPriv frames = {};
    atomic_init(&frames.execution[0], nullptr);
    atomic_init(&frames.execution[1], nullptr);
    assert(!pthread_mutex_init(&frames.execution_mutex, nullptr));
    AVHWFramesContext context = {.hwctx = &frames, .device_ctx = &device_context};
    FFVkExecPool *pool = nullptr, *ready = nullptr;
    atomic_store(&fail_next, true);
    assert(vulkan_frame_execution(&context, FRAME_COMPUTE, &pool) == AVERROR(ENOMEM));
    assert(!pool && !atomic_load(&frames.execution[FRAME_COMPUTE]));
    assert(!vulkan_frame_execution(&context, FRAME_TRANSFER, &ready));
    assert(ready->contexts == 2);

    pause_compute = true;
    struct worker workers[4] = {};
    pthread_t threads[4];
    for (unsigned i = 0; i < 4; ++i) {
        workers[i] = (struct worker){.frames = &context, .await_publication = i > 1};
        assert(!pthread_create(&threads[i], nullptr, acquire_compute, &workers[i]));
    }
    assert(!pthread_mutex_lock(&gate));
    while (!entered)
        assert(!pthread_cond_wait(&condition, &gate));
    assert(!pthread_mutex_unlock(&gate));
    /* A paused constructor must not block a different, already-ready pool. */
    assert(!vulkan_frame_execution(&context, FRAME_TRANSFER, &pool));
    assert(pool == ready && atomic_load(&calls[0]) == 2 && atomic_load(&calls[1]) == 1);
    assert(!pthread_mutex_lock(&gate));
    resume = true;
    assert(!pthread_cond_broadcast(&condition));
    assert(!pthread_mutex_unlock(&gate));
    for (unsigned i = 0; i < 4; ++i) {
        assert(!pthread_join(threads[i], nullptr));
        assert(workers[i].result == workers[0].result);
    }
    assert(atomic_load(&calls[0]) == 2 && atomic_load(&calls[1]) == 1);
    free(atomic_load(&frames.execution[0]));
    free(atomic_load(&frames.execution[1]));
    assert(!pthread_mutex_destroy(&frames.execution_mutex));
    puts("lazy execution pools: retry, single construction, publication and paused-owner checks "
         "pass");
    return 0;
}

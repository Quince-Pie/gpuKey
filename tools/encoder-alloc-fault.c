#define _GNU_SOURCE
/* Private-test fault injector. Match real BO size/domain/flags and optionally
 * skip earlier matches; the test also checks the encoder's specific error. */
#include <amdgpu.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#define REAL(name)                                                                                 \
    typeof(&name) call;                                                                            \
    do {                                                                                           \
        void *symbol = dlsym(RTLD_NEXT, #name);                                                    \
        static_assert(sizeof(call) == sizeof(symbol));                                             \
        memcpy(&call, &symbol, sizeof(call));                                                      \
        if (!call)                                                                                 \
            abort();                                                                               \
    } while (0)

static once_flag initialized = ONCE_FLAG_INIT;
static uint64_t bytes, heap, flags, skip;
static bool match_heap, match_flags, map_failure;
static _Atomic uint64_t matches;
static _Atomic(amdgpu_bo_handle) map_target;
static _Atomic bool armed = true;

/* Test callers change the phase only after closing the prior GPU context. */
void gpukey_fault_arm(void);
void gpukey_fault_disarm(void);

void gpukey_fault_arm(void)
{
    atomic_store_explicit(&matches, 0, memory_order_relaxed);
    atomic_store_explicit(&map_target, nullptr, memory_order_relaxed);
    atomic_store_explicit(&armed, true, memory_order_release);
}

void gpukey_fault_disarm(void)
{
    atomic_store_explicit(&armed, false, memory_order_release);
    atomic_store_explicit(&map_target, nullptr, memory_order_relaxed);
}

static uint64_t option(const char *name, bool *present)
{
    const char *text = getenv(name);
    if (present)
        *present = text != nullptr;
    if (!text)
        return 0;
    char *end;
    errno = 0;
    unsigned long long value = strtoull(text, &end, 0);
    if (errno || !*text || *end)
        abort();
    return (uint64_t)value;
}

static void initialize(void)
{
    bytes = option("GPUKEY_FAIL_BYTES", nullptr);
    heap = option("GPUKEY_FAIL_HEAP", &match_heap);
    flags = option("GPUKEY_FAIL_FLAGS", &match_flags);
    skip = option("GPUKEY_FAIL_SKIP", nullptr);
    map_failure = option("GPUKEY_FAIL_MAP", nullptr) != 0;
}

int amdgpu_bo_alloc(amdgpu_device_handle device, struct amdgpu_bo_alloc_request *request,
                    amdgpu_bo_handle *buffer)
{
    REAL(amdgpu_bo_alloc);
    call_once(&initialized, initialize);
    bool target = atomic_load_explicit(&armed, memory_order_acquire) && bytes &&
                  request->alloc_size == bytes &&
                  (!match_heap || request->preferred_heap == heap) &&
                  (!match_flags || request->flags == flags);
    if (target)
        target = atomic_fetch_add_explicit(&matches, 1, memory_order_relaxed) >= skip;
    if (target && !map_failure) {
        fprintf(stderr, "INJECTED_BO_FAILURE bytes=%llu heap=%u flags=%llu\n",
                (unsigned long long)request->alloc_size, request->preferred_heap,
                (unsigned long long)request->flags);
        return -ENOMEM;
    }
    int ret = call(device, request, buffer);
    if (!ret && target)
        atomic_store_explicit(&map_target, *buffer, memory_order_relaxed);
    return ret;
}

int amdgpu_bo_cpu_map(amdgpu_bo_handle buffer, void **address)
{
    REAL(amdgpu_bo_cpu_map);
    call_once(&initialized, initialize);
    amdgpu_bo_handle target = atomic_load_explicit(&map_target, memory_order_relaxed);
    if (atomic_load_explicit(&armed, memory_order_acquire) && map_failure && target &&
        buffer == target) {
        fputs("INJECTED_MAP_FAILURE\n", stderr);
        return -ENOMEM;
    }
    return call(buffer, address);
}

int amdgpu_bo_free(amdgpu_bo_handle buffer)
{
    REAL(amdgpu_bo_free);
    amdgpu_bo_handle expected = buffer;
    atomic_compare_exchange_strong_explicit(&map_target, &expected, nullptr, memory_order_relaxed,
                                            memory_order_relaxed);
    return call(buffer);
}

#define _GNU_SOURCE
/* Diagnostic interposer only; never link into the recorder. BO_RELEASE can
 * drop one handle reference without freeing backing storage. Use DRM fdinfo
 * for memory totals. GPUKEY_FAIL_BO_BYTES injects an allocation error for the
 * private constructor-failure probe; other allocations retain native behavior. */
#include <amdgpu.h>
#include <dlfcn.h>
#include <errno.h>
#include <execinfo.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int amdgpu_bo_alloc(amdgpu_device_handle device, struct amdgpu_bo_alloc_request *request,
                    amdgpu_bo_handle *buffer)
{
    void *symbol = dlsym(RTLD_NEXT, "amdgpu_bo_alloc");
    typeof(&amdgpu_bo_alloc) allocate;
    static_assert(sizeof(allocate) == sizeof(symbol));
    memcpy(&allocate, &symbol, sizeof(allocate));
    if (!allocate)
        abort();
    const char *failure = getenv("GPUKEY_FAIL_BO_BYTES");
    if (failure && strtoull(failure, nullptr, 10) == request->alloc_size) {
        static atomic_uint matches;
        const char *after = getenv("GPUKEY_FAIL_BO_AFTER");
        if (after && atomic_fetch_add(&matches, 1) != strtoull(after, nullptr, 10))
            return allocate(device, request, buffer);
        static constexpr char message[] = "INJECTED GPU ALLOCATION FAILURE\n";
        ssize_t ignored = write(STDERR_FILENO, message, sizeof(message) - 1);
        (void)ignored;
        return -ENOMEM;
    }
    int result = allocate(device, request, buffer);
    if (!result) {
        char line[256];
        int size = snprintf(
            line, sizeof(line), "BO_ALLOC pid=%ld handle=%p bytes=%llu heap=%u flags=%llu\n",
            (long)getpid(), (void *)*buffer, (unsigned long long)request->alloc_size,
            request->preferred_heap, (unsigned long long)request->flags);
        if (size > 0 && (size_t)size < sizeof(line)) {
            ssize_t ignored = write(STDERR_FILENO, line, (size_t)size);
            (void)ignored;
        }
        if (request->alloc_size >= 1024 * 1024) {
            void *stack[12];
            int count = backtrace(stack, 12);
            backtrace_symbols_fd(stack, count, STDERR_FILENO);
        }
    }
    return result;
}

int amdgpu_bo_free(amdgpu_bo_handle buffer)
{
    void *symbol = dlsym(RTLD_NEXT, "amdgpu_bo_free");
    typeof(&amdgpu_bo_free) release;
    static_assert(sizeof(release) == sizeof(symbol));
    memcpy(&release, &symbol, sizeof(release));
    if (!release)
        abort();
    char line[128];
    int size = snprintf(line, sizeof(line), "BO_RELEASE pid=%ld handle=%p\n", (long)getpid(),
                        (void *)buffer);
    if (size > 0 && (size_t)size < sizeof(line)) {
        ssize_t ignored = write(STDERR_FILENO, line, (size_t)size);
        (void)ignored;
    }
    return release(buffer);
}

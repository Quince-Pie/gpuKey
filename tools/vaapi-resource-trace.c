#define _GNU_SOURCE
/* Diagnostic-only VA resource/lifetime trace. Never linked into the recorder.
 * Apply LD_PRELOAD only to the private fixture's recorder child. Timings from
 * this trace are observations, not uninstrumented performance measurements. */
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdckdint.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <va/va.h>
#include <va/va_enc_av1.h>

static pthread_once_t once = PTHREAD_ONCE_INIT;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static FILE *trace;
static typeof(&vaCreateConfig) create_config;
static typeof(&vaCreateContext) create_context;
static typeof(&vaDestroyContext) destroy_context;
static typeof(&vaCreateSurfaces) create_surfaces;
static typeof(&vaDestroySurfaces) destroy_surfaces;
static typeof(&vaCreateBuffer) create_buffer;
static typeof(&vaDestroyBuffer) destroy_buffer;
static typeof(&vaBeginPicture) begin_picture;
static typeof(&vaSyncBuffer) sync_buffer;

struct context {
    VADisplay display;
    VAContextID id;
    VASurfaceID last;
    uint64_t begins;
};
struct coded {
    VADisplay display;
    VABufferID id;
};
static struct context *contexts;
static struct coded *buffers;
static size_t context_count, context_capacity, buffer_count, buffer_capacity;
static uint64_t completed, stall_after;
static bool repeat_stall, fail_restore, failed_restore;

static void finish(void)
{
    if (fclose(trace))
        abort();
    free(contexts);
    free(buffers);
}

static void initialize(void)
{
#define RESOLVE(target, name)                                                                      \
    do {                                                                                           \
        void *symbol = dlsym(RTLD_NEXT, name);                                                     \
        static_assert(sizeof(target) == sizeof(symbol));                                           \
        if (!symbol)                                                                               \
            abort();                                                                               \
        memcpy(&target, &symbol, sizeof(target));                                                  \
    } while (false)
    RESOLVE(create_config, "vaCreateConfig");
    RESOLVE(create_context, "vaCreateContext");
    RESOLVE(destroy_context, "vaDestroyContext");
    RESOLVE(create_surfaces, "vaCreateSurfaces");
    RESOLVE(destroy_surfaces, "vaDestroySurfaces");
    RESOLVE(create_buffer, "vaCreateBuffer");
    RESOLVE(destroy_buffer, "vaDestroyBuffer");
    RESOLVE(begin_picture, "vaBeginPicture");
    RESOLVE(sync_buffer, "vaSyncBuffer");
#undef RESOLVE
    const char *path = getenv("GPUKEY_RESOURCE_TRACE");
    if (!path || !(trace = fopen(path, "wx")) || atexit(finish))
        abort();
    const char *stall = getenv("GPUKEY_RESOURCE_STALL_AFTER");
    if (stall) {
        char *end;
        errno = 0;
        unsigned long long value = strtoull(stall, &end, 10);
        if (errno || end == stall || *end || !value || value > UINT64_MAX || *stall == '-')
            abort();
        stall_after = (uint64_t)value;
    }
    const char *repeat = getenv("GPUKEY_RESOURCE_STALL_REPEAT");
    const char *fail = getenv("GPUKEY_RESOURCE_FAIL_RESTORE");
    if ((repeat && strcmp(repeat, "1")) || (fail && strcmp(fail, "1")) ||
        ((repeat || fail) && !stall_after))
        abort();
    repeat_stall = repeat != nullptr;
    fail_restore = fail != nullptr;
}

static void ready(void)
{
    if (pthread_once(&once, initialize))
        abort();
}

static void lock(void)
{
    if (pthread_mutex_lock(&mutex))
        abort();
}

static void unlock(void)
{
    if (ferror(trace) || pthread_mutex_unlock(&mutex))
        abort();
}

static void prefix(const char *event, VADisplay display)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t))
        abort();
    uint64_t ns = (uint64_t)t.tv_sec * 1'000'000'000 + (uint64_t)t.tv_nsec;
    fprintf(trace, "%llu %p %s", (unsigned long long)ns, display, event);
}

static void *grow(void *array, size_t count, size_t *capacity, size_t element_size)
{
    if (count < *capacity)
        return array;
    size_t next, bytes;
    if (ckd_add(&next, *capacity, *capacity ? *capacity : 8U) ||
        ckd_mul(&bytes, next, element_size))
        abort();
    void *result = realloc(array, bytes);
    if (!result)
        abort();
    *capacity = next;
    return result;
}

VAStatus vaCreateConfig(VADisplay d, VAProfile profile, VAEntrypoint entry,
                        VAConfigAttrib *attributes, int count, VAConfigID *id)
{
    ready();
    VAStatus result = create_config(d, profile, entry, attributes, count, id);
    lock();
    prefix("config", d);
    fprintf(trace, " %u %d %d %d\n", result == VA_STATUS_SUCCESS ? *id : VA_INVALID_ID, profile,
            entry, result);
    unlock();
    return result;
}

VAStatus vaCreateContext(VADisplay d, VAConfigID config, int width, int height, int flags,
                         VASurfaceID *targets, int count, VAContextID *id)
{
    ready();
    VAStatus result = create_context(d, config, width, height, flags, targets, count, id);
    lock();
    prefix("context", d);
    fprintf(trace, " %u %u %d %d %d\n", result == VA_STATUS_SUCCESS ? *id : VA_INVALID_ID, config,
            width, height, result);
    if (result == VA_STATUS_SUCCESS) {
        contexts = grow(contexts, context_count, &context_capacity, sizeof(*contexts));
        contexts[context_count++] = (struct context){d, *id, VA_INVALID_ID, 0};
    }
    unlock();
    return result;
}

VAStatus vaDestroyContext(VADisplay d, VAContextID id)
{
    ready();
    VAStatus result = destroy_context(d, id);
    lock();
    prefix("context-free", d);
    fprintf(trace, " %u %d\n", id, result);
    if (result == VA_STATUS_SUCCESS)
        for (size_t i = 0; i < context_count; ++i)
            if (contexts[i].display == d && contexts[i].id == id) {
                contexts[i] = contexts[--context_count];
                break;
            }
    unlock();
    return result;
}

VAStatus vaCreateSurfaces(VADisplay d, unsigned format, unsigned width, unsigned height,
                          VASurfaceID *ids, unsigned count, VASurfaceAttrib *attributes,
                          unsigned attribute_count)
{
    ready();
    lock();
    bool fail = fail_restore && !failed_restore && completed >= stall_after &&
                (format == VA_RT_FORMAT_YUV420 || format == VA_RT_FORMAT_YUV420_10BPP);
    if (fail) {
        failed_restore = true;
        prefix("surface-fail", d);
        fprintf(trace, " %u %u %u %u\n", format, width, height, count);
    }
    unlock();
    if (fail) {
        for (unsigned i = 0; i < count; ++i)
            ids[i] = VA_INVALID_ID;
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    VAStatus result =
        create_surfaces(d, format, width, height, ids, count, attributes, attribute_count);
    lock();
    if (result == VA_STATUS_SUCCESS)
        for (unsigned i = 0; i < count; ++i) {
            prefix("surface", d);
            fprintf(trace, " %u %u %u %u\n", ids[i], format, width, height);
        }
    unlock();
    return result;
}

VAStatus vaDestroySurfaces(VADisplay d, VASurfaceID *ids, int count)
{
    ready();
    VAStatus result = destroy_surfaces(d, ids, count);
    lock();
    for (int i = 0; i < count; ++i) {
        prefix("surface-free", d);
        fprintf(trace, " %u %d\n", ids[i], result);
    }
    unlock();
    return result;
}

VAStatus vaCreateBuffer(VADisplay d, VAContextID context, VABufferType type, unsigned size,
                        unsigned count, void *data, VABufferID *id)
{
    ready();
    VAStatus result = create_buffer(d, context, type, size, count, data, id);
    if (type == VAEncCodedBufferType) {
        lock();
        prefix("coded", d);
        fprintf(trace, " %u %u %u %u %d\n", context,
                result == VA_STATUS_SUCCESS ? *id : VA_INVALID_ID, size, count, result);
        if (result == VA_STATUS_SUCCESS) {
            buffers = grow(buffers, buffer_count, &buffer_capacity, sizeof(*buffers));
            buffers[buffer_count++] = (struct coded){d, *id};
        }
        unlock();
    } else if (data && type == VAEncPictureParameterBufferType && count == 1 &&
               size == sizeof(VAEncPictureParameterBufferAV1)) {
        const VAEncPictureParameterBufferAV1 *p = data;
        lock();
        prefix("encode", d);
        fprintf(trace, " %u %u %u %d\n", context, p->coded_buf, p->reconstructed_frame, result);
        unlock();
    }
    return result;
}

VAStatus vaDestroyBuffer(VADisplay d, VABufferID id)
{
    ready();
    VAStatus result = destroy_buffer(d, id);
    lock();
    for (size_t i = 0; i < buffer_count; ++i)
        if (buffers[i].display == d && buffers[i].id == id) {
            prefix("coded-free", d);
            fprintf(trace, " %u %d\n", id, result);
            if (result == VA_STATUS_SUCCESS)
                buffers[i] = buffers[--buffer_count];
            break;
        }
    unlock();
    return result;
}

VAStatus vaBeginPicture(VADisplay d, VAContextID id, VASurfaceID target)
{
    ready();
    VAStatus result = begin_picture(d, id, target);
    lock();
    for (size_t i = 0; i < context_count; ++i) {
        struct context *c = &contexts[i];
        if (c->display != d || c->id != id)
            continue;
        ++c->begins;
        if (target != c->last || result != VA_STATUS_SUCCESS) {
            prefix("begin-change", d);
            fprintf(trace, " %u %u %llu %d\n", id, target, (unsigned long long)c->begins, result);
            c->last = target;
        }
        break;
    }
    unlock();
    return result;
}

VAStatus vaSyncBuffer(VADisplay d, VABufferID id, uint64_t timeout)
{
    ready();
    VAStatus result = sync_buffer(d, id, timeout);
    lock();
    prefix("sync", d);
    fprintf(trace, " %u %llu %d\n", id, (unsigned long long)timeout, result);
    bool stall = false;
    if (stall_after && result == VA_STATUS_SUCCESS) {
        ++completed;
        stall = repeat_stall ? completed % stall_after == 0 : completed == stall_after;
    }
    if (stall) {
        prefix("stall-start", d);
        fprintf(trace, " %u 50000000\n", id);
    }
    unlock();
    if (stall) {
        /* Explicit fault injection, only when requested: pause the media owner
         * after a real completion while the private source continues producing.
         * Fifty milliseconds is a test stimulus, never a production policy. */
        struct timespec delay = {.tv_nsec = 50'000'000};
        while (nanosleep(&delay, &delay) < 0)
            if (errno != EINTR)
                abort();
        lock();
        prefix("stall-end", d);
        fprintf(trace, " %u\n", id);
        unlock();
    }
    return result;
}

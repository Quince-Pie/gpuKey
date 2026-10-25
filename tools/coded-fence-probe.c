#define _GNU_SOURCE
/* Diagnostic for packet_ready_probe only: duplicate a coded-buffer dma-buf
 * before submission, then release the VA export lock before ordinary mapping. */
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <va/va.h>
#include <va/va_drmcommon.h>

extern char *program_invocation_short_name;
int gpukey_packet_fd = -1;
VABufferID gpukey_packet_buffer = VA_INVALID_ID;

#define REAL(name)                                                                                 \
    typeof(&name) call;                                                                            \
    do {                                                                                           \
        void *symbol = dlsym(RTLD_NEXT, #name);                                                    \
        static_assert(sizeof(call) == sizeof(symbol));                                             \
        memcpy(&call, &symbol, sizeof(call));                                                      \
        if (!call || strcmp(program_invocation_short_name, "packet_ready_probe"))                  \
            abort();                                                                               \
    } while (0)

VAStatus vaCreateBuffer(VADisplay display, VAContextID context, VABufferType type, unsigned size,
                        unsigned count, void *data, VABufferID *id)
{
    REAL(vaCreateBuffer);
    VAStatus status = call(display, context, type, size, count, data, id);
    if (status != VA_STATUS_SUCCESS || type != VAEncCodedBufferType)
        return status;
    if (gpukey_packet_fd >= 0)
        abort();
    VABufferInfo info = {.mem_type = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME};
    status = vaAcquireBufferHandle(display, *id, &info);
    if (status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "coded export: %s\n", vaErrorStr(status));
        vaDestroyBuffer(display, *id);
        return status;
    }
    if (info.mem_type != VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME || info.handle > INT32_MAX)
        abort();
    gpukey_packet_fd = fcntl((int)info.handle, F_DUPFD_CLOEXEC, 0);
    VAStatus released = vaReleaseBufferHandle(display, *id);
    if (gpukey_packet_fd < 0 || released != VA_STATUS_SUCCESS)
        abort();
    gpukey_packet_buffer = *id;
    return status;
}

VAStatus vaDestroyBuffer(VADisplay display, VABufferID id)
{
    REAL(vaDestroyBuffer);
    if (id == gpukey_packet_buffer) {
        close(gpukey_packet_fd);
        gpukey_packet_fd = -1;
        gpukey_packet_buffer = VA_INVALID_ID;
    }
    return call(display, id);
}

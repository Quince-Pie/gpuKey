#define _GNU_SOURCE
/* Diagnostic interposer only. Canonical packed bytes and rate-control offsets,
 * excluding nondeterministic surface/buffer handles and structure padding. */
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <va/va.h>
#include <va/va_enc_av1.h>

static pthread_once_t once = PTHREAD_ONCE_INIT;
static typeof(&vaCreateBuffer) create_buffer;
static FILE *trace;

static void close_trace(void)
{
    if (trace && fclose(trace))
        abort();
}

static void initialize(void)
{
    void *symbol = dlsym(RTLD_NEXT, "vaCreateBuffer");
    static_assert(sizeof(create_buffer) == sizeof(symbol));
    memcpy(&create_buffer, &symbol, sizeof(create_buffer));
    const char *path = getenv("GPUKEY_HEADER_TRACE");
    if (!create_buffer || !path || !(trace = fopen(path, "wx")) || atexit(close_trace))
        abort();
}

VAStatus vaCreateBuffer(VADisplay display, VAContextID context, VABufferType type, unsigned size,
                        unsigned count, void *data, VABufferID *buffer)
{
    if (pthread_once(&once, initialize))
        abort();
    if (data) {
        flockfile(trace);
        if (type == VAEncPackedHeaderParameterBufferType) {
            if (size != sizeof(VAEncPackedHeaderParameterBuffer) || count != 1)
                abort();
            const VAEncPackedHeaderParameterBuffer *p = data;
            fprintf(trace, "parameter %u %u %u\n", p->type, p->bit_length,
                    (unsigned)p->has_emulation_bytes);
        } else if (type == VAEncPackedHeaderDataBufferType) {
            if (count != 1)
                abort();
            const unsigned char *bytes = data;
            fprintf(trace, "data %u ", size);
            for (unsigned i = 0; i < size; ++i)
                fprintf(trace, "%02x", bytes[i]);
            fputc('\n', trace);
        } else if (type == VAEncPictureParameterBufferType &&
                   size == sizeof(VAEncPictureParameterBufferAV1) && count == 1) {
            const VAEncPictureParameterBufferAV1 *p = data;
            fprintf(trace, "offsets %u %u %u %u %u %u %u %u\n", (unsigned)p->base_qindex,
                    p->bit_offset_qindex, p->bit_offset_loopfilter_params,
                    p->bit_offset_cdef_params, p->size_in_bits_cdef_params,
                    p->size_in_bits_frame_hdr_obu, p->byte_offset_frame_hdr_obu_size,
                    (unsigned)p->picture_flags.bits.frame_type);
        }
        if (ferror(trace))
            abort();
        funlockfile(trace);
    }
    return create_buffer(display, context, type, size, count, data, buffer);
}

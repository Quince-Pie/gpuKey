#define _GNU_SOURCE
/* Diagnostic only: redirect this probe's VPP/encode pair into Mesa's combined
 * encode-processing path. gpu_fused_probe keeps the RGB image unchanged until
 * encoder drain; this interposer MUST NOT be used with live capture, whose
 * ordinary prepare completion permits immediate source reuse. */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <va/va.h>
#include <va/va_vpp.h>

#define REAL(name)                                                                                 \
    typeof(&name) call;                                                                            \
    do {                                                                                           \
        void *symbol = dlsym(RTLD_NEXT, #name);                                                    \
        static_assert(sizeof(call) == sizeof(symbol));                                             \
        memcpy(&call, &symbol, sizeof(call));                                                      \
        if (!call || strcmp(program_invocation_short_name, "gpu_fused_probe"))                     \
            abort();                                                                               \
    } while (0)

extern char *program_invocation_short_name;
/* The probe uses this marker to forbid a pre-encode YUV readback oracle: the
 * combined path has intentionally not materialized that image. */
const char gpukey_fused_probe_active = 1;
static VAConfigID vpp_config = VA_INVALID_ID, encode_config = VA_INVALID_ID;
static VAContextID vpp_context = VA_INVALID_ID, encode_context = VA_INVALID_ID;
static VASurfaceID output = VA_INVALID_ID;
static VAProcPipelineParameterBuffer pending;

VAStatus vaCreateConfig(VADisplay display, VAProfile profile, VAEntrypoint entry,
                        VAConfigAttrib *attributes, int count, VAConfigID *config)
{
    REAL(vaCreateConfig);
    VAStatus status = call(display, profile, entry, attributes, count, config);
    if (status == VA_STATUS_SUCCESS) {
        if (entry == VAEntrypointVideoProc)
            vpp_config = *config;
        else if (profile == VAProfileAV1Profile0)
            encode_config = *config;
    }
    return status;
}

VAStatus vaCreateContext(VADisplay display, VAConfigID config, int width, int height, int flags,
                         VASurfaceID *targets, int count, VAContextID *context)
{
    REAL(vaCreateContext);
    VAStatus status = call(display, config, width, height, flags, targets, count, context);
    if (status == VA_STATUS_SUCCESS) {
        if (config == vpp_config)
            vpp_context = *context;
        else if (config == encode_config)
            encode_context = *context;
    }
    return status;
}

VAStatus vaRenderPicture(VADisplay display, VAContextID context, VABufferID *buffers, int count)
{
    REAL(vaRenderPicture);
    if (context != vpp_context)
        return call(display, context, buffers, count);
    if (count != 1)
        abort();
    void *mapped;
    VAStatus status = vaMapBuffer(display, buffers[0], &mapped);
    if (status != VA_STATUS_SUCCESS)
        return status;
    pending = *(const VAProcPipelineParameterBuffer *)mapped;
    if (pending.surface_region || pending.output_region || pending.filters || pending.num_filters)
        abort();
    return vaUnmapBuffer(display, buffers[0]);
}

VAStatus vaBeginPicture(VADisplay display, VAContextID context, VASurfaceID target)
{
    REAL(vaBeginPicture);
    if (context == vpp_context) {
        output = target;
        return VA_STATUS_SUCCESS;
    }
    if (context != encode_context || target != output || output == VA_INVALID_ID)
        return call(display, context, target);
    VAStatus status = call(display, context, pending.surface);
    if (status != VA_STATUS_SUCCESS)
        return status;
    VAProcPipelineParameterBuffer params = pending;
    params.additional_outputs = &output;
    params.num_additional_outputs = 1;
    VABufferID buffer;
    status = vaCreateBuffer(display, context, VAProcPipelineParameterBufferType, sizeof(params), 1,
                            &params, &buffer);
    if (status != VA_STATUS_SUCCESS)
        return status;
    status = vaRenderPicture(display, context, &buffer, 1);
    VAStatus destroyed = vaDestroyBuffer(display, buffer);
    fprintf(stderr, "probe: combined encode processing: %s\n", vaErrorStr(status));
    return status == VA_STATUS_SUCCESS ? destroyed : status;
}

VAStatus vaEndPicture(VADisplay display, VAContextID context)
{
    REAL(vaEndPicture);
    return context == vpp_context ? VA_STATUS_SUCCESS : call(display, context);
}

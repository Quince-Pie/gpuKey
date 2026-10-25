#define _GNU_SOURCE
/* Diagnostic only. gpuKey serializes encoder API calls on one media owner.
 * Measure accepted video submissions through public receive, not GPU execution
 * time or mux/disk completion. Keep profiling out of CPU-overhead comparisons. */
#include <dlfcn.h>
#include <libavcodec/avcodec.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct submission {
    AVCodecContext *context;
    int64_t pts, started;
};
static struct submission *pending;
static size_t count, capacity;

static int64_t now(void)
{
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time))
        abort();
    return (int64_t)time.tv_sec * INT64_C(1000000000) + time.tv_nsec;
}

#define REAL(name)                                                                                 \
    typeof(&name) call;                                                                            \
    do {                                                                                           \
        void *symbol = dlsym(RTLD_NEXT, #name);                                                    \
        static_assert(sizeof(call) == sizeof(symbol));                                             \
        memcpy(&call, &symbol, sizeof(call));                                                      \
        if (!call)                                                                                 \
            abort();                                                                               \
    } while (0)

int avcodec_send_frame(AVCodecContext *context, const AVFrame *frame)
{
    REAL(avcodec_send_frame);
    bool video = frame && context->codec_type == AVMEDIA_TYPE_VIDEO;
    int64_t started = video ? now() : 0;
    int result = call(context, frame);
    if (result >= 0 && video) {
        if (count == capacity) {
            if (capacity > SIZE_MAX / 2 / sizeof(*pending))
                abort();
            size_t next = capacity ? capacity * 2 : 8;
            void *grown = realloc(pending, next * sizeof(*pending));
            if (!grown)
                abort();
            pending = grown;
            capacity = next;
        }
        pending[count++] = (struct submission){context, frame->pts, started};
    }
    return result;
}

int avcodec_receive_packet(AVCodecContext *context, AVPacket *packet)
{
    REAL(avcodec_receive_packet);
    int result = call(context, packet);
    if (result >= 0 && packet->size && context->codec_type == AVMEDIA_TYPE_VIDEO) {
        size_t at;
        for (at = 0; at < count; ++at)
            if (pending[at].context == context && pending[at].pts == packet->pts)
                break;
        if (at == count) {
            fprintf(stderr, "PACKET_LATENCY_UNMATCHED pts=%lld\n", (long long)packet->pts);
        } else {
            int64_t completed = now();
            int64_t elapsed = completed - pending[at].started;
            fprintf(stderr, "PACKET_LATENCY pts=%lld ns=%lld bytes=%d pending=%zu\n",
                    (long long)packet->pts, (long long)elapsed, packet->size, count);
            if (getenv("GPUKEY_PACKET_ABSOLUTE"))
                fprintf(stderr, "PACKET_TIMING pts=%lld started_ns=%lld completed_ns=%lld\n",
                        (long long)packet->pts, (long long)pending[at].started,
                        (long long)completed);
            pending[at] = pending[--count];
        }
    }
    return result;
}

void avcodec_free_context(AVCodecContext **context)
{
    REAL(avcodec_free_context);
    typeof(&av_codec_is_encoder) is_encoder;
    void *symbol = dlsym(RTLD_NEXT, "av_codec_is_encoder");
    static_assert(sizeof(is_encoder) == sizeof(symbol));
    memcpy(&is_encoder, &symbol, sizeof(is_encoder));
    if (!is_encoder)
        abort();
    bool video = context && *context && (*context)->codec &&
                 (*context)->codec_type == AVMEDIA_TYPE_VIDEO && is_encoder((*context)->codec);
    if (video) {
        for (size_t at = 0; at < count;) {
            if (pending[at].context == *context) {
                fprintf(stderr, "PACKET_LATENCY_ABANDONED pts=%lld\n", (long long)pending[at].pts);
                pending[at] = pending[--count];
            } else
                ++at;
        }
    }
    call(context);
    if (video && !count) {
        free_sized(pending, capacity * sizeof(*pending));
        pending = nullptr;
        capacity = 0;
    }
}

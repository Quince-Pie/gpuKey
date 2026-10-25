/* Native-driver lifecycle probe. This executable alone interposes VA export
 * to exercise a real descriptor-allocation failure and capability fallback. */
#include "media.h"

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <libavcodec/avcodec.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

#define REAL(name)                                                                                 \
    typeof(&name) call;                                                                            \
    do {                                                                                           \
        void *symbol = dlsym(RTLD_NEXT, #name);                                                    \
        static_assert(sizeof(call) == sizeof(symbol));                                             \
        memcpy(&call, &symbol, sizeof(call));                                                      \
        CHECK(call);                                                                               \
    } while (0)

static enum { NONE, UNSUPPORTED, NO_FDS } fault;
static unsigned fault_hits;
static bool limited;
static struct rlimit saved_limit;

VAStatus vaAcquireBufferHandle(VADisplay display, VABufferID id, VABufferInfo *info)
{
    REAL(vaAcquireBufferHandle);
    if (fault == UNSUPPORTED) {
        ++fault_hits;
        return VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE;
    }
    VAStatus status = call(display, id, info);
    if (status == VA_STATUS_SUCCESS && fault == NO_FDS) {
        /* The next actual F_DUPFD_CLOEXEC fails with EMFILE. Release needs no
         * new descriptor; restore the limit before any other application work. */
        CHECK(!limited && getrlimit(RLIMIT_NOFILE, &saved_limit) == 0);
        struct rlimit limit = saved_limit;
        limit.rlim_cur = 0;
        CHECK(setrlimit(RLIMIT_NOFILE, &limit) == 0);
        limited = true;
        ++fault_hits;
    }
    return status;
}

VAStatus vaReleaseBufferHandle(VADisplay display, VABufferID id)
{
    REAL(vaReleaseBufferHandle);
    VAStatus status = call(display, id);
    if (limited) {
        CHECK(setrlimit(RLIMIT_NOFILE, &saved_limit) == 0);
        limited = false;
    }
    return status;
}

static unsigned fd_count(void)
{
    DIR *directory = opendir("/proc/self/fd");
    CHECK(directory);
    unsigned count = 0;
    struct dirent *entry;
    while ((entry = readdir(directory)))
        if (entry->d_name[0] != '.')
            ++count;
    CHECK(closedir(directory) == 0);
    return count;
}

static void cycle(unsigned mode)
{
    struct gk_gpu gpu;
    CHECK(gk_gpu_open(&gpu, GK_VAAPI, nullptr) == 0);
    struct gk_video_format format = {.width = 320,
                                     .height = 240,
                                     .pixel_format = AV_PIX_FMT_BGRA,
                                     .transfer = AVCOL_TRC_IEC61966_2_1};
    struct gk_media_options options = {.bits = 8,
                                       .encode_depth = 2,
                                       .qp = 32,
                                       .fps_num = 60,
                                       .fps_den = 1,
                                       .packet_readout =
                                           mode == 1 ? GK_READOUT_AUTO : GK_READOUT_EVENTS,
                                       .matroska = true,
                                       .replay = {.duration_ns = 30 * GK_NS, .bytes = 1048576},
                                       .sink = {.slabs = 8, .slab_size = 65536, .stall_ms = 5000}};
    fault = mode == 1 || mode == 2 ? UNSUPPORTED : mode == 3 ? NO_FDS : NONE;
    unsigned before = fault_hits;
    struct gk_media *media = nullptr;
    int ret = gk_media_open(&media, &gpu, &format, &options);
    CHECK(!limited);
    CHECK(fault == NONE || fault_hits == before + 1);
    fault = NONE;
    if (mode == 2 || mode == 3) {
        CHECK(ret < 0 && !media);
    } else {
        CHECK(ret == 0 && media && gk_media_completion_fd(media) == -1);
        struct gk_media_stats stats;
        if (mode == 0) {
            CHECK(gk_media_close(&media, 0, &stats) == -ENODATA);
        } else {
            uint8_t pixels[320 * 240 * 4];
            memset(pixels, 255, sizeof(pixels));
            struct gk_image image = {.data = pixels, .size = sizeof(pixels), .stride = 320 * 4};
            AVFrame *frame;
            CHECK(gk_media_prepare(media, &image, VA_INVALID_ID, &frame) == 0);
            CHECK(gk_media_submit(media, frame, 0) == 0);
            int fd = gk_media_completion_fd(media);
            if (mode == 5 && fd >= 0) {
                int flags = fcntl(fd, F_GETFD);
                CHECK(flags >= 0 && (flags & FD_CLOEXEC));
                struct pollfd ready = {.fd = fd, .events = POLLIN};
                CHECK(poll(&ready, 1, 5000) == 1 && ready.revents == POLLIN);
                CHECK(gk_media_collect(media, true) == 0);
            }
            CHECK(gk_media_close(&media, 10000000, &stats) == 0 && !media);
            CHECK(stats.submitted == 1 && stats.packets == 1);
            CHECK(stats.completion_events == (mode != 1));
            if (fd >= 0)
                CHECK(fcntl(fd, F_GETFD) == -1 && errno == EBADF);
        }
    }
    gk_gpu_close(&gpu);
}

int main(void)
{
    cycle(5); /* Warm loader/runtime caches before the leak comparison. */
    unsigned baseline = fd_count();
    for (unsigned repeat = 0; repeat < 8; ++repeat)
        for (unsigned mode = 0; mode < 6; ++mode) {
            cycle(mode);
            CHECK(fd_count() == baseline);
        }
    puts("completion: 48 empty/pending/collected/fallback/EMFILE lifecycles, no descriptor leaks");
}

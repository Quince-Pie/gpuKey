/* Compare the public replay consumer after GPU completion, without a second
 * input or encoder drain. No private FFmpeg structure is inspected. */
#include "media.h"

#include <dlfcn.h>
#include <errno.h>
#include <libavcodec/avcodec.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

int main(int argc, char **argv)
{
    CHECK((argc == 3 || (argc == 4 && (!strcmp(argv[3], "buffered") || !strcmp(argv[3], "poll") ||
                                       !strcmp(argv[3], "events")))) &&
          (!strcmp(argv[2], "1") || !strcmp(argv[2], "2")));
    bool poll_completion = argc == 4 && !strcmp(argv[3], "poll");
    bool events = argc == 4 && !strcmp(argv[3], "events");
    unsigned depth = argv[2][0] == '1' ? 1U : 2U;
    struct gk_gpu gpu;
    CHECK(gk_gpu_open(&gpu, GK_VAAPI, nullptr) == 0);
    struct gk_video_format format = {.width = 320,
                                     .height = 240,
                                     .pixel_format = AV_PIX_FMT_BGRA,
                                     .transfer = AVCOL_TRC_IEC61966_2_1};
    struct gk_media_options options = {.bits = 8,
                                       .encode_depth = depth,
                                       .packet_readout = events ? GK_READOUT_EVENTS
                                                         : argc == 4 && !poll_completion
                                                             ? GK_READOUT_BUFFERED
                                                             : GK_READOUT_AUTO,
                                       .qp = 32,
                                       .fps_num = 60,
                                       .fps_den = 1,
                                       .matroska = true,
                                       .replay = {.duration_ns = 30 * GK_NS, .bytes = 1048576},
                                       .sink = {.slabs = 8, .slab_size = 65536, .stall_ms = 5000}};
    struct gk_media *media;
    CHECK(gk_media_open(&media, &gpu, &format, &options) == 0);
    CHECK(gk_media_completion_fd(media) == -1);
    uint8_t *pixels = malloc(320 * 240 * 4);
    CHECK(pixels);
    memset(pixels, 255, 320 * 240 * 4);
    struct gk_image image = {.data = pixels, .size = 320 * 240 * 4, .stride = 320 * 4};
    AVFrame *frame;
    int64_t start = gk_now_ns();
    CHECK(gk_media_prepare(media, &image, VA_INVALID_ID, &frame) == 0);
    CHECK(gk_media_submit(media, frame, 0) == 0);
    /* Mesa associates this input with the encoding fence. Wait explicitly so
     * an early EAGAIN cannot be explained by unfinished hardware work. */
    if (events) {
        int fd = gk_media_completion_fd(media);
        if (fd >= 0) {
            struct pollfd ready = {.fd = fd, .events = POLLIN};
            CHECK(poll(&ready, 1, 5000) == 1 && ready.revents == POLLIN);
            CHECK(gk_media_collect(media, true) == 0);
        }
        CHECK(gk_media_completion_fd(media) == -1);
    } else if (poll_completion) {
        const int *fd = dlsym(RTLD_DEFAULT, "gpukey_packet_fd");
        const VABufferID *buffer = dlsym(RTLD_DEFAULT, "gpukey_packet_buffer");
        CHECK(fd && buffer && *fd >= 0);
        struct pollfd ready = {.fd = *fd, .events = POLLIN};
        CHECK(poll(&ready, 1, 5000) == 1 && ready.revents == POLLIN);
        CHECK(vaSyncBuffer(gpu.display, *buffer, 0) == VA_STATUS_SUCCESS);
    } else
        CHECK(vaSyncSurface(gpu.display, (VASurfaceID)(uintptr_t)frame->data[3]) ==
              VA_STATUS_SUCCESS);
    int64_t complete = gk_now_ns();
    if (complete - start < 2000000) {
        struct timespec until = {.tv_sec = (start + 2000000) / GK_NS,
                                 .tv_nsec = (start + 2000000) % GK_NS};
        CHECK(clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &until, nullptr) == 0);
    }
    int64_t endpoint = gk_now_ns() - start; /* Representable on Matroska's 1 ms grid. */
    int status = gk_media_save(media, argv[1], endpoint);
    CHECK(status == 0 || status == -EAGAIN);
    if (!status) {
        CHECK(gk_wait(gk_media_replay_fd(media), 5000) == 1);
        struct gk_replay_result result;
        CHECK(gk_media_saved(media, &result) && !result.error);
    }
    if (events && depth == 2) {
        /* Completion permits in-place reuse, but an independent owner still
         * pins the old image. The second slot remains real parallel capacity. */
        AVFrame *pin = av_frame_clone(frame), *other = nullptr;
        CHECK(pin && gk_media_prepare(media, &image, VA_INVALID_ID, &other) == 0);
        CHECK(other != frame);
        av_frame_free(&pin);
        AVFrame *reused = nullptr;
        CHECK(gk_media_prepare(media, &image, VA_INVALID_ID, &reused) == 0 && reused == frame);
    }
    struct gk_media_stats stats;
    CHECK(gk_media_close(&media, endpoint, &stats) == 0);
    free_sized(pixels, 320 * 240 * 4);
    gk_gpu_close(&gpu);
    printf("{\"depth\":%u,\"gpu_complete_ns\":%lld,\"early_save_status\":%d,\"saved\":%llu}\n",
           depth, (long long)(complete - start), status, (unsigned long long)stats.replay.saved);
    return 0;
}

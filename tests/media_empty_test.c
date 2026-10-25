#include "media.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static void empty(enum gk_backend backend, bool replay, bool prepared)
{
    char path[] = "/tmp/gpukey-empty-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0 && close(fd) == 0 && unlink(path) == 0);
    struct gk_gpu gpu;
    CHECK(gk_gpu_open(&gpu, backend, nullptr) == 0);
    struct gk_video_format format = {.width = 320,
                                     .height = 240,
                                     .pixel_format = AV_PIX_FMT_BGRA,
                                     .transfer = AVCOL_TRC_IEC61966_2_1};
    const struct gk_audio_track stereo = {.format = GK_AUDIO_STEREO, .title = "Unused audio"};
    struct gk_media_options options = {
        .output = path,
        .matroska = true,
        .bits = 8,
        .surfaces = 4,
        .qp = 32,
        .fps_num = 60,
        .fps_den = 1,
        .audio_count = 1,
        .audio = &stereo,
        .sink = {.slab_size = 4096, .slabs = 8, .stall_ms = 5000},
        .replay = {.duration_ns = replay ? GK_NS : 0, .bytes = 1024 * 1024},
    };
    struct gk_media *media = nullptr;
    CHECK(gk_media_open(&media, &gpu, &format, &options) == 0);
    if (prepared) {
        uint8_t *pixels = calloc(320 * 240, 4);
        CHECK(pixels);
        struct gk_image image = {.data = pixels, .size = 320 * 240 * 4, .stride = 320 * 4};
        AVFrame *frame;
        CHECK(gk_media_prepare(media, &image, VA_INVALID_ID, &frame) == 0);
        /* A failed first submission must preserve its error through teardown,
         * even though conversion has already allocated/prepared GPU resources. */
        CHECK(gk_media_submit(media, frame, -1) == -ERANGE);
        free(pixels);
    }
    struct gk_media_stats stats;
    CHECK(gk_media_close(&media, 0, &stats) == (prepared ? -ERANGE : -ENODATA));
    CHECK(!media && stats.submitted == 0 && stats.packets == 0 && stats.replay.saved == 0);
    CHECK(gk_media_close(&media, 0, nullptr) == 0);
    gk_gpu_close(&gpu);
    if (replay)
        CHECK(access(path, F_OK) < 0 && errno == ENOENT);
    else
        CHECK(unlink(path) == 0);
}

int main(int argc, char **argv)
{
    enum gk_backend backend = GK_SOFTWARE;
    if (argc == 2 && !strcmp(argv[1], "vaapi"))
        backend = GK_VAAPI;
    else if (argc == 2 && !strcmp(argv[1], "vulkan"))
        backend = GK_VULKAN;
    else if (argc != 1)
        return 2;
    for (unsigned mode = 0; mode < 4; ++mode)
        empty(backend, (mode & 1) != 0, (mode & 2) != 0);
    puts("media: empty file/replay and rejected first-frame teardown passed");
    return 0;
}

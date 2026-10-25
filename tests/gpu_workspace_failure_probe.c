/* Repeat a real driver allocation/map failure and a healthy encoder lifetime
 * in one process. The optional interposer supplies arm/disarm, not media logic. */
#include "media.h"

#include <dirent.h>
#include <dlfcn.h>
#include <libavcodec/avcodec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static unsigned descriptors(void)
{
    DIR *directory = opendir("/proc/self/fd");
    CHECK(directory);
    unsigned count = 0;
    struct dirent *entry;
    while ((entry = readdir(directory)))
        count += entry->d_name[0] != '.';
    CHECK(closedir(directory) == 0);
    return count;
}

static void cycle(bool fail)
{
    struct gk_gpu gpu;
    CHECK(gk_gpu_open(&gpu, GK_VAAPI, nullptr) == 0);
    struct gk_video_format format = {.width = 320,
                                     .height = 240,
                                     .pixel_format = AV_PIX_FMT_BGRA,
                                     .transfer = AVCOL_TRC_IEC61966_2_1};
    struct gk_media_options options = {.bits = 8,
                                       .qp = 32,
                                       .fps_num = 60,
                                       .fps_den = 1,
                                       .matroska = true,
                                       .replay = {.duration_ns = 30 * GK_NS, .bytes = 1048576},
                                       .sink = {.slabs = 8, .slab_size = 65536, .stall_ms = 5000}};
    struct gk_media *media;
    CHECK(gk_media_open(&media, &gpu, &format, &options) == 0);
    uint8_t pixels[320 * 240 * 4];
    memset(pixels, 255, sizeof(pixels));
    struct gk_image image = {.data = pixels, .size = sizeof(pixels), .stride = 320 * 4};
    AVFrame *frame;
    CHECK(gk_media_prepare(media, &image, VA_INVALID_ID, &frame) == 0);
    int ret = gk_media_submit(media, frame, 0);
    CHECK(fail ? ret < 0 : ret == 0);
    struct gk_media_stats stats;
    ret = gk_media_close(&media, 20000000, &stats);
    CHECK(fail ? ret < 0 : ret == 0);
    if (!fail)
        CHECK(stats.submitted == 1 && stats.packets == 1);
    gk_gpu_close(&gpu);
}

int main(void)
{
    void *symbol = dlsym(RTLD_DEFAULT, "gpukey_fault_arm");
    void (*arm)(void), (*disarm)(void);
    static_assert(sizeof(arm) == sizeof(symbol));
    memcpy(&arm, &symbol, sizeof(arm));
    symbol = dlsym(RTLD_DEFAULT, "gpukey_fault_disarm");
    memcpy(&disarm, &symbol, sizeof(disarm));
    CHECK(arm && disarm);
    disarm();
    cycle(false);
    unsigned baseline = descriptors();
    for (unsigned i = 0; i < 8; ++i) {
        arm();
        cycle(true);
        CHECK(descriptors() == baseline);
        disarm();
        cycle(false);
        CHECK(descriptors() == baseline);
    }
    puts("workspace: eight failed and healthy lifetimes, balanced descriptors");
}

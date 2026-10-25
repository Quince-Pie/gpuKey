#include "control.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

struct client {
    const char *path;
    _Atomic bool done;
    int result;
};
static void *save_client(void *opaque)
{
    struct client *client = opaque;
    client->result = gk_control_save("test", client->path);
    atomic_store_explicit(&client->done, true, memory_order_release);
    return nullptr;
}

int main(void)
{
    av_log_set_level(AV_LOG_ERROR);
    char root[] = "/tmp/gpukey-control-XXXXXX", subdir[PATH_MAX], clip[PATH_MAX], name[PATH_MAX];
    CHECK(mkdtemp(root));
    const char *previous = getenv("XDG_RUNTIME_DIR");
    char *old = previous ? strdup(previous) : nullptr;
    CHECK(!previous || old);
    CHECK(setenv("XDG_RUNTIME_DIR", root, 1) == 0);
    snprintf(subdir, sizeof(subdir), "%s/gpukey", root);
    CHECK(mkdir(subdir, 0700) == 0);
    snprintf(clip, sizeof(clip), "%s/client.mkv", root);
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    CHECK(snprintf(address.sun_path, sizeof(address.sun_path), "%s/gpukey/test.sock", root) > 0);
    int peer = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    CHECK(peer >= 0 && bind(peer, (struct sockaddr *)&address, sizeof(address)) == 0);
    CHECK(close(peer) == 0); /* Orphaned socket from an abruptly stopped recorder. */

    struct gk_gpu gpu;
    CHECK(gk_gpu_open(&gpu, GK_SOFTWARE, nullptr) == 0);
    struct gk_video_format format = {.width = 64,
                                     .height = 48,
                                     .pixel_format = AV_PIX_FMT_BGRA,
                                     .transfer = AVCOL_TRC_IEC61966_2_1};
    struct gk_media_options options = {.matroska = true,
                                       .bits = 8,
                                       .qp = 32,
                                       .surfaces = 4,
                                       .fps_num = 60,
                                       .fps_den = 1,
                                       .sink = {.slabs = 8, .slab_size = 4096, .stall_ms = 5000},
                                       .replay = {.duration_ns = GK_NS, .bytes = 1048576}};
    struct gk_media *media;
    CHECK(gk_media_open(&media, &gpu, &format, &options) == 0);
    int input[2];
    CHECK(pipe(input) == 0);
    struct gk_control *control, *other;
    int wake = gk_media_replay_fd(media);
    CHECK(gk_control_open(&other, "../test", root, true, wake, -1) == -EINVAL);
    CHECK(gk_control_open(&control, "test", root, true, wake, input[0]) == 0);
    CHECK(gk_control_open(&other, "test", root, true, wake, -1) == -EWOULDBLOCK);
    CHECK(chmod(subdir, 0777) == 0);
    CHECK(gk_control_open(&other, "unsafe", root, true, wake, -1) == -EACCES);
    CHECK(chmod(subdir, 0700) == 0);

    peer = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    CHECK(peer >= 0 && connect(peer, (struct sockaddr *)&address, sizeof(address)) == 0);
    CHECK(send(peer, "bad", 3, 0) == 3);
    gk_control_service(control, media, GK_NS);
    gk_control_service(control, media, GK_NS);
    struct pollfd readable = {.fd = peer, .events = POLLIN};
    CHECK(poll(&readable, 1, 1000) == 1 && (readable.revents & POLLIN));
    struct {
        uint32_t version;
        int error;
        int64_t duration;
        char path[PATH_MAX];
    } reply;
    CHECK(recv(peer, &reply, sizeof(reply), MSG_TRUNC) == sizeof(reply));
    CHECK(reply.error == -EPROTO);
    CHECK(close(peer) == 0);

    uint8_t pixels[64 * 48 * 4] = {};
    struct gk_image image = {.data = pixels, .size = sizeof(pixels), .stride = 64 * 4};
    for (unsigned i = 0; i < 180; ++i) {
        AVFrame *frame;
        int64_t pts;
        CHECK(gk_timestamp(i, 60, 1, &pts));
        CHECK(gk_media_prepare(media, &image, VA_INVALID_ID, &frame) == 0);
        CHECK(gk_media_submit(media, frame, pts) == 0);
    }
    CHECK(write(input[1], "\n", 1) == 1);
    gk_control_service(control, media, 3 * GK_NS);
    CHECK(gk_wait(wake, 5000) == 1);
    gk_control_service(control, media, 3 * GK_NS);
    unsigned dated = 0;
    DIR *dir = opendir(root);
    CHECK(dir);
    struct dirent *entry;
    while ((entry = readdir(dir)))
        if (!strncmp(entry->d_name, "gpukey-replay-", 14)) {
            snprintf(name, sizeof(name), "%s/%s", root, entry->d_name);
            AVFormatContext *file = nullptr;
            CHECK(avformat_open_input(&file, name, nullptr, nullptr) == 0);
            CHECK(avformat_find_stream_info(file, nullptr) == 0 && file->nb_streams == 1);
            avformat_close_input(&file);
            CHECK(unlink(name) == 0);
            ++dated;
        }
    CHECK(closedir(dir) == 0 && dated == 1);
    struct client client = {.path = clip};
    pthread_t thread;
    CHECK(pthread_create(&thread, nullptr, save_client, &client) == 0);
    int64_t deadline = gk_now_ns() + 5 * GK_NS;
    while (!atomic_load_explicit(&client.done, memory_order_acquire)) {
        CHECK(gk_now_ns() < deadline);
        gk_control_service(control, media, 3 * GK_NS);
        CHECK(gk_wait(gk_control_fd(control), 10) >= 0);
    }
    CHECK(pthread_join(thread, nullptr) == 0 && client.result == 0 && access(clip, F_OK) == 0);
    struct gk_media_stats stats;
    CHECK(gk_media_close(&media, 3 * GK_NS, &stats) == 0);
    CHECK(stats.replay.saved == 2);
    gk_control_finish(control, &stats.replay.last);
    gk_control_close(&control);
    gk_gpu_close(&gpu);
    CHECK(close(input[0]) == 0 && close(input[1]) == 0);
    CHECK(access(address.sun_path, F_OK) < 0 && errno == ENOENT);
    CHECK(gk_control_save("test", nullptr) == -ENOTCONN);
    snprintf(name, sizeof(name), "%s/gpukey/test.lock", root);
    CHECK(unlink(name) == 0 && unlink(clip) == 0 && rmdir(subdir) == 0 && rmdir(root) == 0);
    CHECK(old ? setenv("XDG_RUNTIME_DIR", old, 1) == 0 : unsetenv("XDG_RUNTIME_DIR") == 0);
    free(old);
    puts("control: Enter, client completion, stale socket, collision, permissions and malformed "
         "request pass");
    return 0;
}

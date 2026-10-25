#include "control.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static constexpr uint32_t protocol = 0x47524b31;
struct request {
    uint32_t version;
    char path[PATH_MAX];
};
struct reply {
    uint32_t version;
    int error;
    int64_t duration;
    char path[PATH_MAX];
};
struct gk_control {
    int directory, lock, listener, events, client, input, replay;
    char socket_name[64];
    char *output_directory;
    char output[PATH_MAX];
    ino_t socket_inode;
    bool matroska, saving;
    int64_t client_deadline;
};

bool gk_control_valid_session(const char *name)
{
    size_t n = name ? strlen(name) : 0;
    if (!n || n > 32)
        return false;
    for (size_t i = 0; i < n; ++i)
        if (!((name[i] >= 'a' && name[i] <= 'z') || (name[i] >= 'A' && name[i] <= 'Z') ||
              (name[i] >= '0' && name[i] <= '9') || name[i] == '-' || name[i] == '_'))
            return false;
    return true;
}

static int private_directory(bool create)
{
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (!runtime || !*runtime)
        return -ENOENT;
    int parent = open(runtime, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (parent < 0)
        return -errno;
    struct stat status;
    int ret = 0;
    if (fstat(parent, &status) < 0)
        ret = -errno;
    else if (status.st_uid != geteuid() || (status.st_mode & 0077))
        ret = -EACCES;
    if (ret >= 0 && create && mkdirat(parent, "gpukey", 0700) < 0 && errno != EEXIST)
        ret = -errno;
    int fd = -1;
    if (ret >= 0) {
        fd = openat(parent, "gpukey", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0)
            ret = -errno;
        else if (fstat(fd, &status) < 0)
            ret = -errno;
        else if (status.st_uid != geteuid() || (status.st_mode & 0077))
            ret = -EACCES;
    }
    close(parent);
    if (ret < 0) {
        if (fd >= 0)
            close(fd);
        return ret;
    }
    return fd;
}

static int address(struct sockaddr_un *out, int directory, const char *name)
{
    *out = (struct sockaddr_un){.sun_family = AF_UNIX};
    /* Stable directory FD avoids cwd mutation and the short pathname limit of
     * Unix sockets even when XDG_RUNTIME_DIR is a long test/sandbox path. */
    int n = snprintf(out->sun_path, sizeof(out->sun_path), "/proc/self/fd/%d/%s", directory, name);
    return n < 0 || (size_t)n >= sizeof(out->sun_path) ? -ENAMETOOLONG : 0;
}
static bool same_user(int fd)
{
    struct ucred credentials;
    socklen_t size = sizeof(credentials);
    return getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &size) == 0 &&
           size == sizeof(credentials) && credentials.uid == geteuid();
}
static int watch(struct gk_control *c, int fd, uint32_t events)
{
    struct epoll_event event = {.events = events, .data.fd = fd};
    return epoll_ctl(c->events, EPOLL_CTL_ADD, fd, &event) < 0 ? -errno : 0;
}

static void respond(int fd, int error, const char *path, int64_t duration)
{
    if (fd < 0)
        return;
    struct reply reply = {.version = protocol, .error = error, .duration = duration};
    if (path)
        snprintf(reply.path, sizeof(reply.path), "%s", path);
    (void)send(fd, &reply, sizeof(reply), MSG_NOSIGNAL | MSG_DONTWAIT);
}
static void drop_client(struct gk_control *c)
{
    if (c->client >= 0) {
        epoll_ctl(c->events, EPOLL_CTL_DEL, c->client, nullptr);
        close(c->client);
        c->client = -1;
    }
}

int gk_control_open(struct gk_control **out, const char *session, const char *directory,
                    bool matroska, int replay_fd, int input_fd)
{
    *out = nullptr;
    if (!gk_control_valid_session(session) || !directory || replay_fd < 0)
        return -EINVAL;
    struct gk_control *c = calloc(1, sizeof(*c));
    if (!c)
        return -ENOMEM;
    *c = (struct gk_control){.directory = -1,
                             .lock = -1,
                             .listener = -1,
                             .events = -1,
                             .client = -1,
                             .input = input_fd,
                             .replay = replay_fd,
                             .matroska = matroska};
    int ret;
    c->directory = private_directory(true);
    if (c->directory < 0) {
        ret = c->directory;
        fprintf(stderr, "Replay control needs an owned, private XDG_RUNTIME_DIR (mode 0700): %s\n",
                strerror(-ret));
        goto fail;
    }
    char lock_name[64];
    snprintf(lock_name, sizeof(lock_name), "%s.lock", session);
    snprintf(c->socket_name, sizeof(c->socket_name), "%s.sock", session);
    c->lock = openat(c->directory, lock_name, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (c->lock < 0) {
        ret = -errno;
        goto fail;
    }
    struct stat status;
    if (fstat(c->lock, &status) < 0) {
        ret = -errno;
        goto fail;
    }
    if (!S_ISREG(status.st_mode) || status.st_uid != geteuid() || status.st_nlink != 1) {
        ret = -EACCES;
        goto fail;
    }
    if (flock(c->lock, LOCK_EX | LOCK_NB) < 0) {
        ret = -errno;
        if (ret == -EWOULDBLOCK)
            fprintf(stderr,
                    "Replay session '%s' is already running. Use --session NAME for another.\n",
                    session);
        goto fail;
    }
    if (fstatat(c->directory, c->socket_name, &status, AT_SYMLINK_NOFOLLOW) == 0) {
        if (!S_ISSOCK(status.st_mode) || status.st_uid != geteuid()) {
            ret = -EEXIST;
            goto fail;
        }
        if (unlinkat(c->directory, c->socket_name, 0) < 0) {
            ret = -errno;
            goto fail;
        }
    } else if (errno != ENOENT) {
        ret = -errno;
        goto fail;
    }
    if (mkdir(directory, 0700) < 0 && errno != EEXIST) {
        ret = -errno;
        goto fail;
    }
    c->output_directory = realpath(directory, nullptr);
    if (!c->output_directory) {
        ret = -errno;
        goto fail;
    }
    if (stat(c->output_directory, &status) < 0) {
        ret = -errno;
        goto fail;
    }
    if (!S_ISDIR(status.st_mode)) {
        ret = -ENOTDIR;
        goto fail;
    }
    c->listener = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (c->listener < 0) {
        ret = -errno;
        goto fail;
    }
    struct sockaddr_un location;
    ret = address(&location, c->directory, c->socket_name);
    if (ret < 0)
        goto fail;
    if (bind(c->listener, (struct sockaddr *)&location, sizeof(location)) < 0) {
        ret = -errno;
        goto fail;
    }
    if (fstatat(c->directory, c->socket_name, &status, AT_SYMLINK_NOFOLLOW) < 0) {
        ret = -errno;
        goto fail;
    }
    c->socket_inode = status.st_ino;
    if (fchmodat(c->directory, c->socket_name, 0600, 0) < 0 || listen(c->listener, 8) < 0) {
        ret = -errno;
        goto fail;
    }
    c->events = epoll_create1(EPOLL_CLOEXEC);
    if (c->events < 0) {
        ret = -errno;
        goto fail;
    }
    if ((ret = watch(c, c->listener, EPOLLIN)) < 0 || (ret = watch(c, replay_fd, EPOLLIN)) < 0 ||
        (input_fd >= 0 && (ret = watch(c, input_fd, EPOLLIN)) < 0))
        goto fail;
    fprintf(stderr, "Replay '%s' ready. %sRun 'gpukey save%s%s' to save; Ctrl-C stops.\n", session,
            input_fd >= 0 ? "Enter saves. " : "", strcmp(session, "default") ? " --session " : "",
            strcmp(session, "default") ? session : "");
    *out = c;
    return 0;
fail:
    gk_control_close(&c);
    return ret;
}

static int destination(struct gk_control *c, const char *requested)
{
    const char *extension = c->matroska ? ".mkv" : ".mp4";
    int n;
    if (*requested) {
        const char *base = strrchr(requested, '/');
        const char *suffix = strrchr(base ? base + 1 : requested, '.');
        if (suffix && strcasecmp(suffix, extension))
            return -EINVAL;
        n = snprintf(c->output, sizeof(c->output), "%s%s", requested, suffix ? "" : extension);
    } else {
        struct timespec now;
        struct tm local;
        char stamp[64];
        if (clock_gettime(CLOCK_REALTIME, &now) < 0 || !localtime_r(&now.tv_sec, &local))
            return -errno;
        if (!strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local))
            return -ERANGE;
        n = snprintf(c->output, sizeof(c->output), "%s/gpukey-replay-%s-%09ld%s",
                     c->output_directory, stamp, now.tv_nsec, extension);
    }
    return n < 0 || (size_t)n >= sizeof(c->output) ? -ENAMETOOLONG : 0;
}

static void request_save(struct gk_control *c, struct gk_media *media, int64_t endpoint_ns,
                         const char *path)
{
    if (c->saving) {
        fputs("A replay save is already in progress.\n", stderr);
        return;
    }
    int ret = destination(c, path);
    if (ret >= 0)
        ret = gk_media_save(media, c->output, endpoint_ns);
    if (ret < 0) {
        fprintf(stderr, "Cannot save replay: %s%s\n", strerror(-ret),
                ret == -EAGAIN   ? " (buffer is warming up)"
                : ret == -EINVAL ? " (filename must match the replay container)"
                                 : "");
        respond(c->client, ret, c->output, 0);
        drop_client(c);
    } else {
        c->saving = true;
        fprintf(stderr, "Saving replay to %s\n", c->output);
    }
}

void gk_control_finish(struct gk_control *c, const struct gk_replay_result *last)
{
    if (!c || !c->saving)
        return;
    respond(c->client, last->error, c->output, last->end_ns - last->start_ns);
    drop_client(c);
    if (last->error < 0)
        fprintf(stderr, "Replay save failed for %s: %s. Any partial file is retained.\n", c->output,
                strerror(-last->error));
    else
        fprintf(stderr, "Saved %.3f seconds to %s\n",
                (double)(last->end_ns - last->start_ns) / (double)GK_NS, c->output);
    if (last->error == -ERANGE && c->matroska)
        fputs("Matroska uses 1 ms timestamps. Use 'gpukey replay --container mp4' for finer "
              "video timing.\n",
              stderr);
    c->saving = false;
}

void gk_control_service(struct gk_control *c, struct gk_media *media, int64_t endpoint_ns)
{
    if (!c)
        return;
    if (!c->saving && c->client >= 0 && gk_now_ns() > c->client_deadline) {
        respond(c->client, -ETIMEDOUT, nullptr, 0);
        drop_client(c);
    }
    struct epoll_event events[8];
    int count = epoll_wait(c->events, events, 8, 0);
    for (int i = 0; i < count; ++i) {
        int fd = events[i].data.fd;
        if (fd == c->listener) {
            /* Bounded control work per owner turn, including accidental floods. */
            for (unsigned accepted = 0; accepted < 8; ++accepted) {
                int peer = accept4(c->listener, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
                if (peer < 0)
                    break;
                if (!same_user(peer) || c->client >= 0 || c->saving) {
                    respond(peer, -EBUSY, nullptr, 0);
                    close(peer);
                    continue;
                }
                if (watch(c, peer, EPOLLIN | EPOLLRDHUP) < 0) {
                    close(peer);
                    continue;
                }
                c->client = peer;
                c->client_deadline = gk_now_ns() + 2 * GK_NS;
            }
        } else if (fd == c->input) {
            char buffer[256];
            ssize_t n = read(fd, buffer, sizeof(buffer));
            if (n <= 0) {
                epoll_ctl(c->events, EPOLL_CTL_DEL, fd, nullptr);
                c->input = -1;
            } else if (memchr(buffer, '\n', (size_t)n)) {
                if (!c->saving && c->client >= 0) {
                    respond(c->client, -EBUSY, nullptr, 0);
                    drop_client(c);
                }
                request_save(c, media, endpoint_ns, "");
            }
        } else if (fd == c->replay) {
            struct gk_replay_result result;
            if (gk_media_saved(media, &result))
                gk_control_finish(c, &result);
        } else if (fd == c->client) {
            if (events[i].events & (EPOLLHUP | EPOLLRDHUP | EPOLLERR)) {
                drop_client(c);
                continue;
            }
            if (c->saving) {
                drop_client(c); /* A second command on one connection is not a new save. */
                continue;
            }
            struct request request;
            ssize_t n = recv(fd, &request, sizeof(request), MSG_DONTWAIT | MSG_TRUNC);
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                continue;
            if (n != sizeof(request) || request.version != protocol ||
                !memchr(request.path, 0, sizeof(request.path)) ||
                (request.path[0] && request.path[0] != '/')) {
                respond(fd, -EPROTO, nullptr, 0);
                drop_client(c);
                continue;
            }
            request_save(c, media, endpoint_ns, request.path);
        }
    }
}

int gk_control_fd(const struct gk_control *c)
{
    return c ? c->events : -1;
}

void gk_control_close(struct gk_control **control)
{
    struct gk_control *c = *control;
    if (!c)
        return;
    drop_client(c);
    if (c->events >= 0)
        close(c->events);
    if (c->listener >= 0)
        close(c->listener);
    if (c->directory >= 0 && c->socket_inode) {
        struct stat status;
        if (fstatat(c->directory, c->socket_name, &status, AT_SYMLINK_NOFOLLOW) == 0 &&
            S_ISSOCK(status.st_mode) && status.st_ino == c->socket_inode)
            unlinkat(c->directory, c->socket_name, 0);
    }
    if (c->lock >= 0)
        close(c->lock);
    if (c->directory >= 0)
        close(c->directory);
    free(c->output_directory);
    free(c);
    *control = nullptr;
}

int gk_control_save(const char *session, const char *path)
{
    if (!gk_control_valid_session(session))
        return -EINVAL;
    int directory = private_directory(false);
    if (directory < 0)
        return directory == -ENOENT ? -ENOTCONN : directory;
    char name[64];
    snprintf(name, sizeof(name), "%s.sock", session);
    struct sockaddr_un location;
    int ret = address(&location, directory, name);
    int fd = -1;
    if (ret < 0)
        goto cleanup;
    fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        ret = -errno;
        goto cleanup;
    }
    struct timeval timeout = {.tv_sec = 5};
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0 ||
        connect(fd, (struct sockaddr *)&location, sizeof(location)) < 0) {
        ret = errno == ENOENT || errno == ECONNREFUSED ? -ENOTCONN : -errno;
        goto cleanup;
    }
    if (!same_user(fd)) {
        ret = -EACCES;
        goto cleanup;
    }
    struct request request = {.version = protocol};
    if (path && *path) {
        char *cwd = path[0] == '/' ? nullptr : getcwd(nullptr, 0);
        if (path[0] != '/' && !cwd) {
            ret = -errno;
            goto cleanup;
        }
        int n = cwd ? snprintf(request.path, sizeof(request.path), "%s/%s", cwd, path)
                    : snprintf(request.path, sizeof(request.path), "%s", path);
        free(cwd);
        if (n < 0 || (size_t)n >= sizeof(request.path)) {
            ret = -ENAMETOOLONG;
            goto cleanup;
        }
    }
    ssize_t sent = send(fd, &request, sizeof(request), MSG_NOSIGNAL);
    if (sent != sizeof(request)) {
        ret = sent < 0 ? -errno : -EIO;
        goto cleanup;
    }
    struct reply reply;
    ssize_t n;
    do {
        n = recv(fd, &reply, sizeof(reply), MSG_TRUNC);
    } while (n < 0 && errno == EINTR);
    if (n != sizeof(reply) || reply.version != protocol ||
        !memchr(reply.path, 0, sizeof(reply.path))) {
        ret = n < 0 ? -errno : -ECONNRESET;
        goto cleanup;
    }
    ret = reply.error > 0 ? -EPROTO : reply.error;
    if (ret >= 0)
        printf("Saved %.3f seconds to %s\n", (double)reply.duration / (double)GK_NS, reply.path);
cleanup:
    if (fd >= 0)
        close(fd);
    close(directory);
    return ret;
}

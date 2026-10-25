#include "portal.h"
#include "portal_mock.h"

#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
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
    g_test_init(&argc, &argv, nullptr);
    struct gk_portal_mock mock;
    gk_portal_mock_start(&mock, nullptr);
    int stop = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    CHECK(stop >= 0);
    struct gk_portal *portal;
    struct gk_portal_source source;
    CHECK(gk_portal_open(&portal, &source, 1, true, stop) == 0);
    CHECK(atomic_load(&mock.selected_types) == 1);
    CHECK(source.node == 42 && source.serial == 123456789 && source.fd >= 0);
    int capture = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    int control = eventfd(1, EFD_CLOEXEC | EFD_NONBLOCK);
    CHECK(capture >= 0 && control >= 0);
    /* Replay control can be attached after the initial capture-only wait.
     * Keep it readable across iterations: each must wake without waiting for
     * a video frame or the two-second fallback deadline. */
    gk_portal_wait(portal, capture, -1, 0);
    gint64 before = g_get_monotonic_time();
    for (unsigned i = 0; i < 4; ++i)
        gk_portal_wait(portal, capture, control, 2000);
    CHECK(g_get_monotonic_time() - before < 1000000);
    uint64_t notifications;
    CHECK(read(control, &notifications, sizeof(notifications)) == sizeof(notifications));
    int complete = eventfd(1, EFD_CLOEXEC | EFD_NONBLOCK);
    int pending = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    CHECK(complete >= 0 && pending >= 0);
    CHECK(gk_portal_wait_completion(portal, capture, control, complete, 2000) == 1);
    /* A readiness check alone hid a self-waking wait. With both FDs drained,
     * wait must honor the timeout and consume little owner-thread CPU. */
    for (unsigned i = 0; i < 3; ++i) {
        struct timespec cpu_before, cpu_after;
        CHECK(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu_before) == 0);
        before = g_get_monotonic_time();
        /* The other registered completion fd stays readable but inactive. */
        CHECK(gk_portal_wait_completion(portal, capture, control, pending, 30) == 0);
        gint64 elapsed = g_get_monotonic_time() - before;
        CHECK(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu_after) == 0);
        int64_t cpu_ns = (cpu_after.tv_sec - cpu_before.tv_sec) * INT64_C(1000000000) +
                         cpu_after.tv_nsec - cpu_before.tv_nsec;
        CHECK(elapsed >= 30000);
        CHECK(cpu_ns < INT64_C(15000000));
    }
    uint64_t signal = 1;
    CHECK(write(pending, &signal, sizeof(signal)) == sizeof(signal));
    CHECK(gk_portal_wait_completion(portal, capture, control, pending, 2000) == 1);
    gk_portal_clear_completion(portal);
    /* Replace an owned descriptor number after retiring the old watches. */
    int replacement = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    CHECK(replacement >= 0 && dup3(replacement, complete, O_CLOEXEC) == complete);
    close(replacement);
    before = g_get_monotonic_time();
    CHECK(gk_portal_wait_completion(portal, capture, control, complete, 30) == 0);
    CHECK(g_get_monotonic_time() - before >= 30000);
    gk_portal_clear_completion(portal);
    int broken[2];
    CHECK(pipe2(broken, O_CLOEXEC | O_NONBLOCK) == 0);
    CHECK(gk_portal_wait_completion(portal, capture, control, broken[0], 0) == 0);
    close(broken[1]);
    /* poll reports HUP even with events=0. An inactive failed watch must
     * report the failure instead of spinning until another input arrives. */
    CHECK(gk_portal_wait_completion(portal, capture, control, -1, 2000) == -EIO);
    gk_portal_clear_completion(portal);
    close(broken[0]);
    close(complete);
    close(pending);
    close(source.fd);
    CHECK(g_dbus_connection_emit_signal(mock.bus, nullptr, pm_session,
                                        "org.freedesktop.portal.Session", "Closed",
                                        g_variant_new("(a{sv})", nullptr), nullptr));
    for (unsigned i = 0; i < 100 && !gk_portal_closed(portal); ++i)
        g_usleep(1000);
    CHECK(gk_portal_closed(portal));
    gk_portal_close(&portal);
    close(capture);
    close(control);
    CHECK(atomic_load(&mock.closes) == 1);
    CHECK(gk_portal_open(&portal, &source, 2, true, stop) == 0);
    CHECK(atomic_load(&mock.selected_types) == 2);
    close(source.fd);
    gk_portal_close(&portal);
    CHECK(atomic_load(&mock.closes) == 2);
    atomic_store(&mock.deny, true);
    CHECK(gk_portal_open(&portal, &source, 3, true, stop) == -ECANCELED);
    CHECK(portal == nullptr && source.fd == -1);
    CHECK(atomic_load(&mock.closes) == 3);
    atomic_store(&mock.deny, false);
    atomic_store(&mock.bad_fd, true);
    CHECK(gk_portal_open(&portal, &source, 3, true, stop) == -EPROTO);
    CHECK(portal == nullptr && source.fd == -1 && atomic_load(&mock.closes) == 4);
    uint64_t one = 1;
    CHECK(write(stop, &one, sizeof(one)) == sizeof(one));
    CHECK(gk_portal_open(&portal, &source, 3, true, stop) == -ECANCELED);
    close(stop);
    gk_portal_mock_stop(&mock);
    puts("portal: immediate response, stable serial, owned FD, revocation, denial and cancellation "
         "passed");
    return 0;
}

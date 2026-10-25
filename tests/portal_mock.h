#ifndef GPUKEY_TEST_PORTAL_MOCK_H
#define GPUKEY_TEST_PORTAL_MOCK_H

/* Private test bus only. Never claims a portal name on the user's session bus. */
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define PM_CHECK(x)                                                                                \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

static const char pm_name[] = "org.freedesktop.portal.Desktop";
static const char pm_path[] = "/org/freedesktop/portal/desktop";
static const char pm_session[] = "/org/freedesktop/portal/desktop/session/mock";
static const char pm_xml[] =
    "<node><interface name='org.freedesktop.portal.ScreenCast'>"
    "<property name='version' type='u' access='read'/>"
    "<property name='AvailableSourceTypes' type='u' access='read'/>"
    "<property name='AvailableCursorModes' type='u' access='read'/>"
    "<method name='CreateSession'><arg type='a{sv}' direction='in'/><arg type='o' "
    "direction='out'/></method>"
    "<method name='SelectSources'><arg type='o' direction='in'/><arg type='a{sv}' "
    "direction='in'/><arg type='o' direction='out'/></method>"
    "<method name='Start'><arg type='o' direction='in'/><arg type='s' direction='in'/><arg "
    "type='a{sv}' direction='in'/><arg type='o' direction='out'/></method>"
    "<method name='OpenPipeWireRemote'><arg type='o' direction='in'/><arg type='a{sv}' "
    "direction='in'/><arg type='h' direction='out'/></method>"
    "</interface><interface name='org.freedesktop.portal.Session'><method "
    "name='Close'/></interface></node>";

struct gk_portal_mock {
    GMainContext *context;
    GMainLoop *loop;
    GDBusConnection *bus;
    _Atomic bool ready, deny, bad_fd;
    _Atomic unsigned closes, selected_types;
    unsigned request;
    pthread_t thread;
    GTestDBus *test_bus;
    const char *remote_path;
    _Atomic uint32_t node;
    uint64_t serial;
};

static GVariant *pm_property(GDBusConnection *connection, const gchar *sender, const gchar *object,
                             const gchar *interface, const gchar *key, GError **error,
                             gpointer opaque)
{
    (void)connection;
    (void)sender;
    (void)object;
    (void)interface;
    (void)error;
    (void)opaque;
    return g_variant_new_uint32(!strcmp(key, "version") ? 6U : 3U);
}

static void pm_call(GDBusConnection *bus, const gchar *sender, const gchar *object,
                    const gchar *interface, const gchar *method, GVariant *parameters,
                    GDBusMethodInvocation *invocation, gpointer opaque)
{
    (void)object;
    (void)interface;
    struct gk_portal_mock *m = opaque;
    if (!strcmp(method, "SelectSources")) {
        GVariant *options = g_variant_get_child_value(parameters, 1);
        guint32 types;
        gboolean multiple;
        PM_CHECK(g_variant_lookup(options, "types", "u", &types));
        PM_CHECK(g_variant_lookup(options, "multiple", "b", &multiple) && !multiple);
        atomic_store(&m->selected_types, types);
        g_variant_unref(options);
    }
    if (!strcmp(method, "Close")) {
        atomic_fetch_add(&m->closes, 1);
        g_dbus_method_invocation_return_value(invocation, nullptr);
        return;
    }
    if (!strcmp(method, "OpenPipeWireRemote")) {
        int fd;
        if (m->remote_path) {
            fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
            struct sockaddr_un address = {.sun_family = AF_UNIX};
            int n = snprintf(address.sun_path, sizeof(address.sun_path), "%s", m->remote_path);
            PM_CHECK(fd >= 0 && n > 0 && (size_t)n < sizeof(address.sun_path));
            PM_CHECK(connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
        } else {
            int pair[2];
            PM_CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0);
            fd = pair[0];
            close(pair[1]);
        }
        GUnixFDList *fds = g_unix_fd_list_new();
        PM_CHECK(g_unix_fd_list_append(fds, fd, nullptr) == 0);
        g_dbus_method_invocation_return_value_with_unix_fd_list(
            invocation, g_variant_new("(h)", atomic_load(&m->bad_fd) ? 42 : 0), fds);
        g_object_unref(fds);
        close(fd);
        return;
    }
    char request[128];
    snprintf(request, sizeof(request), "/org/freedesktop/portal/desktop/request/mock/%u",
             ++m->request);
    GVariantBuilder result;
    g_variant_builder_init(&result, G_VARIANT_TYPE_VARDICT);
    if (!strcmp(method, "CreateSession"))
        g_variant_builder_add(&result, "{sv}", "session_handle", g_variant_new_string(pm_session));
    if (!strcmp(method, "Start")) {
        GVariantBuilder streams, properties;
        g_variant_builder_init(&streams, G_VARIANT_TYPE("a(ua{sv})"));
        g_variant_builder_init(&properties, G_VARIANT_TYPE_VARDICT);
        if (m->serial)
            g_variant_builder_add(&properties, "{sv}", "pipewire-serial",
                                  g_variant_new_uint64(m->serial));
        g_variant_builder_add(&streams, "(u@a{sv})", atomic_load(&m->node),
                              g_variant_builder_end(&properties));
        g_variant_builder_add(&result, "{sv}", "streams", g_variant_builder_end(&streams));
    }
    uint32_t code = !strcmp(method, "Start") && atomic_load(&m->deny) ? 1U : 0U;
    /* Match the portal's directed response to its caller. Retain the early
     * signal to exercise subscription-before-call and handle reconciliation. */
    PM_CHECK(g_dbus_connection_emit_signal(
        bus, sender, request, "org.freedesktop.portal.Request", "Response",
        g_variant_new("(u@a{sv})", code, g_variant_builder_end(&result)), nullptr));
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(o)", request));
}

static const GDBusInterfaceVTable pm_vtable = {.method_call = pm_call, .get_property = pm_property};

static void *pm_serve(void *opaque)
{
    struct gk_portal_mock *m = opaque;
    m->context = g_main_context_new();
    g_main_context_push_thread_default(m->context);
    m->loop = g_main_loop_new(m->context, false);
    m->bus =
        g_dbus_connection_new_for_address_sync(g_getenv("DBUS_SESSION_BUS_ADDRESS"),
                                               G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                                   G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
                                               nullptr, nullptr, nullptr);
    PM_CHECK(m->bus != nullptr);
    GVariant *reply = g_dbus_connection_call_sync(
        m->bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
        "RequestName", g_variant_new("(su)", pm_name, 0U), G_VARIANT_TYPE("(u)"),
        G_DBUS_CALL_FLAGS_NONE, 1000, nullptr, nullptr);
    PM_CHECK(reply != nullptr);
    g_variant_unref(reply);
    GDBusNodeInfo *info = g_dbus_node_info_new_for_xml(pm_xml, nullptr);
    PM_CHECK(info != nullptr);
    guint portal_id = g_dbus_connection_register_object(m->bus, pm_path, info->interfaces[0],
                                                        &pm_vtable, m, nullptr, nullptr);
    guint session_id = g_dbus_connection_register_object(m->bus, pm_session, info->interfaces[1],
                                                         &pm_vtable, m, nullptr, nullptr);
    PM_CHECK(portal_id && session_id);
    atomic_store_explicit(&m->ready, true, memory_order_release);
    g_main_loop_run(m->loop);
    g_dbus_connection_unregister_object(m->bus, portal_id);
    g_dbus_connection_unregister_object(m->bus, session_id);
    g_dbus_node_info_unref(info);
    g_dbus_connection_close_sync(m->bus, nullptr, nullptr);
    g_object_unref(m->bus);
    g_main_loop_unref(m->loop);
    g_main_context_pop_thread_default(m->context);
    g_main_context_unref(m->context);
    return nullptr;
}

static void gk_portal_mock_start(struct gk_portal_mock *m, const char *remote_path)
{
    *m = (struct gk_portal_mock){.remote_path = remote_path, .serial = remote_path ? 0 : 123456789};
    atomic_init(&m->ready, false);
    atomic_init(&m->deny, false);
    atomic_init(&m->bad_fd, false);
    atomic_init(&m->closes, 0);
    atomic_init(&m->selected_types, 0);
    atomic_init(&m->node, 42);
    /* GTestDBus removes XDG_RUNTIME_DIR to prevent session-bus autolaunch.
     * Keep the caller's private runtime for PipeWire/replay, while retaining
     * GTestDBus's explicit private DBUS_SESSION_BUS_ADDRESS. */
    char *runtime = g_strdup(g_getenv("XDG_RUNTIME_DIR"));
    m->test_bus = g_test_dbus_new(G_TEST_DBUS_NONE);
    g_test_dbus_up(m->test_bus);
    if (runtime) {
        g_setenv("XDG_RUNTIME_DIR", runtime, true);
        g_free(runtime);
    }
    PM_CHECK(pthread_create(&m->thread, nullptr, pm_serve, m) == 0);
    while (!atomic_load_explicit(&m->ready, memory_order_acquire))
        g_usleep(1000);
}

static void gk_portal_mock_stop(struct gk_portal_mock *m)
{
    g_main_loop_quit(m->loop);
    PM_CHECK(pthread_join(m->thread, nullptr) == 0);
    g_test_dbus_down(m->test_bus);
    g_object_unref(m->test_bus);
}

#undef PM_CHECK
#endif

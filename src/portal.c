#include "portal.h"

#include <errno.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <glib-unix.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tracy/TracyC.h>
#include <unistd.h>

static const char bus_name[] = "org.freedesktop.portal.Desktop";
static const char portal_path[] = "/org/freedesktop/portal/desktop";
static const char screencast[] = "org.freedesktop.portal.ScreenCast";

struct deadline_source {
    GSource source;
    gint64 at; /* Owner-only: -1 disabled/fired; G_MAXINT64 infinite. */
};

struct completion_source {
    GSource source;
    GPollFD poll;
    struct gk_portal *portal;
    bool ready;
};

struct gk_portal {
    GDBusConnection *bus;
    GMainContext *context;
    GSource *signal, *input, *control, *deadline;
    struct completion_source **completions;
    size_t completion_count, completion_capacity;
    int completion_error;
    char *session, *request;
    GVariant *response;
    guint response_subscription, closed_subscription;
    uint32_t code;
    bool received, closed, wake;
};

static gboolean completion_check(GSource *source)
{
    struct completion_source *completion = (void *)source;
    return (completion->poll.events && completion->poll.revents) ||
           (completion->poll.revents & (G_IO_ERR | G_IO_HUP | G_IO_NVAL));
}

static gboolean completion_dispatch(GSource *source, GSourceFunc callback, gpointer opaque)
{
    (void)callback;
    (void)opaque;
    struct completion_source *completion = (void *)source;
    completion->ready = true;
    completion->portal->wake = true;
    if (completion->poll.revents & (G_IO_ERR | G_IO_HUP | G_IO_NVAL))
        completion->portal->completion_error = -EIO;
    return G_SOURCE_CONTINUE;
}

static GSourceFuncs completion_functions = {.check = completion_check,
                                            .dispatch = completion_dispatch};

static struct completion_source *completion_watch(struct gk_portal *p, int fd)
{
    for (size_t i = 0; i < p->completion_count; ++i)
        if (p->completions[i]->poll.fd == fd)
            return p->completions[i];
    if (p->completion_count == p->completion_capacity) {
        if (p->completion_capacity > SIZE_MAX / 2 / sizeof(*p->completions))
            return nullptr;
        size_t count = p->completion_capacity ? p->completion_capacity * 2 : 2;
        void *grown = realloc(p->completions, count * sizeof(*p->completions));
        if (!grown)
            return nullptr;
        p->completions = grown;
        p->completion_capacity = count;
    }
    struct completion_source *completion =
        (void *)g_source_new(&completion_functions, sizeof(*completion));
    completion->poll.fd = fd;
    completion->portal = p;
    g_source_add_poll(&completion->source, &completion->poll);
    g_source_attach(&completion->source, p->context);
    p->completions[p->completion_count++] = completion;
    return completion;
}

void gk_portal_clear_completion(struct gk_portal *p)
{
    if (!p)
        return;
    TracyCZoneN(zone, "portal.clear-completions", true);
    for (size_t i = 0; i < p->completion_count; ++i) {
        g_source_destroy(&p->completions[i]->source);
        g_source_unref(&p->completions[i]->source);
    }
    p->completion_count = 0;
    TracyCZoneEnd(zone);
}

static gboolean input_ready(gint fd, GIOCondition condition, gpointer opaque)
{
    (void)fd;
    (void)condition;
    ((struct gk_portal *)opaque)->wake = true;
    return G_SOURCE_CONTINUE;
}

#ifndef GK_PORTAL_READY_TIME
static gboolean deadline_prepare(GSource *source, gint *timeout)
{
    struct deadline_source *deadline = (void *)source;
    *timeout = -1;
    if (deadline->at < 0 || deadline->at == G_MAXINT64)
        return false;
    gint64 remaining = deadline->at - g_get_monotonic_time();
    if (remaining <= 0) {
        *timeout = 0;
        return true;
    }
    *timeout = (gint)((remaining + 999) / 1000);
    return false;
}

static gboolean deadline_check(GSource *source)
{
    struct deadline_source *deadline = (void *)source;
    return deadline->at >= 0 && deadline->at != G_MAXINT64 &&
           g_get_monotonic_time() >= deadline->at;
}
#endif

static gboolean deadline_ready(GSource *source, GSourceFunc callback, gpointer opaque)
{
    (void)callback;
    (void)opaque;
    ((struct deadline_source *)source)->at = -1;
    return G_SOURCE_CONTINUE;
}

static GSourceFuncs deadline_functions = {
#ifndef GK_PORTAL_READY_TIME
    .prepare = deadline_prepare,
    .check = deadline_check,
#endif
    .dispatch = deadline_ready};

static gboolean interrupted(gint fd, GIOCondition condition, gpointer opaque)
{
    (void)fd;
    (void)condition;
    ((struct gk_portal *)opaque)->closed = true;
    /* Leave the signal pending for the main recording loop. */
    return G_SOURCE_REMOVE;
}

static void response(GDBusConnection *connection, const gchar *sender, const gchar *path,
                     const gchar *interface, const gchar *signal, GVariant *parameters,
                     gpointer opaque)
{
    (void)connection;
    (void)sender;
    (void)interface;
    (void)signal;
    struct gk_portal *p = opaque;
    if (!p->request || strcmp(path, p->request) || p->received)
        return;
    if (!g_variant_is_of_type(parameters, G_VARIANT_TYPE("(ua{sv})"))) {
        p->closed = true;
        return;
    }
    g_variant_get(parameters, "(u@a{sv})", &p->code, &p->response);
    p->received = true;
}

static void closed(GDBusConnection *connection, const gchar *sender, const gchar *path,
                   const gchar *interface, const gchar *signal, GVariant *parameters,
                   gpointer opaque)
{
    (void)connection;
    (void)sender;
    (void)interface;
    (void)signal;
    (void)parameters;
    struct gk_portal *p = opaque;
    if (p->session && !strcmp(path, p->session))
        p->closed = true;
}

static int request(struct gk_portal *p, const char *method, GVariant *parameters)
{
    g_clear_pointer(&p->request, g_free);
    g_clear_pointer(&p->response, g_variant_unref);
    p->received = false;
    GError *error = nullptr;
    GVariant *reply = g_dbus_connection_call_sync(p->bus, bus_name, portal_path, screencast, method,
                                                  parameters, G_VARIANT_TYPE("(o)"),
                                                  G_DBUS_CALL_FLAGS_NONE, 30000, nullptr, &error);
    if (!reply) {
        fprintf(stderr, "portal %s: %s\n", method, error->message);
        g_error_free(error);
        return -EIO;
    }
    g_variant_get(reply, "(o)", &p->request);
    g_variant_unref(reply);
    /* Response subscription predates the method call. Its callback is queued
     * on this private context, so even an immediate response is not lost. */
    while (!p->received && !p->closed)
        g_main_context_iteration(p->context, true);
    if (p->closed)
        return -ECANCELED;
    return p->code == 0 ? 0 : p->code == 1 ? -ECANCELED : -EACCES;
}

static GVariant *options(const char *token)
{
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&b, "{sv}", "handle_token", g_variant_new_string(token));
    return g_variant_builder_end(&b);
}

int gk_portal_open(struct gk_portal **out, struct gk_portal_source *source, uint32_t source_types,
                   bool cursor, int signal_fd)
{
    *out = nullptr;
    *source = (struct gk_portal_source){.fd = -1};
    struct gk_portal *p = g_new0(struct gk_portal, 1);
    p->context = g_main_context_new();
    g_main_context_push_thread_default(p->context);
    p->signal = g_unix_fd_source_new(signal_fd, G_IO_IN | G_IO_HUP | G_IO_ERR);
    g_source_set_callback(p->signal, G_SOURCE_FUNC(interrupted), p, nullptr);
    g_source_attach(p->signal, p->context);
    GError *error = nullptr;
    char *address = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (address) {
        p->bus = g_dbus_connection_new_for_address_sync(
            address,
            G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
            nullptr, nullptr, &error);
        g_free(address);
    }
    int ret = -EIO;
    if (!p->bus) {
        fprintf(stderr, "session bus: %s\n", error->message);
        g_error_free(error);
        goto fail;
    }
    p->response_subscription = g_dbus_connection_signal_subscribe(
        p->bus, bus_name, "org.freedesktop.portal.Request", "Response", nullptr, nullptr,
        G_DBUS_SIGNAL_FLAGS_NONE, response, p, nullptr);
    p->closed_subscription = g_dbus_connection_signal_subscribe(
        p->bus, bus_name, "org.freedesktop.portal.Session", "Closed", nullptr, nullptr,
        G_DBUS_SIGNAL_FLAGS_NONE, closed, p, nullptr);
    GVariant *props_reply = g_dbus_connection_call_sync(
        p->bus, bus_name, portal_path, "org.freedesktop.DBus.Properties", "GetAll",
        g_variant_new("(s)", screencast), G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NONE, 30000,
        nullptr, &error);
    if (!props_reply) {
        fprintf(stderr, "ScreenCast portal: %s\n", error->message);
        g_error_free(error);
        goto fail;
    }
    GVariant *props;
    g_variant_get(props_reply, "(@a{sv})", &props);
    uint32_t available = 0, modes = 1, version = 0;
    g_variant_lookup(props, "AvailableSourceTypes", "u", &available);
    g_variant_lookup(props, "AvailableCursorModes", "u", &modes);
    g_variant_lookup(props, "version", "u", &version);
    g_variant_unref(props);
    g_variant_unref(props_reply);
    uint32_t cursor_mode = cursor ? 2U : 1U;
    if (!(available & source_types) || !(modes & cursor_mode)) {
        ret = -ENOTSUP;
        fprintf(stderr, "portal does not offer requested source/cursor mode\n");
        goto fail;
    }
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&b, "{sv}", "handle_token", g_variant_new_string("gpukey_create"));
    char *session_token = g_strdup_printf("gpukey_%u_%u", (unsigned)getpid(), g_random_int());
    g_variant_builder_add(&b, "{sv}", "session_handle_token", g_variant_new_string(session_token));
    g_free(session_token);
    ret = request(p, "CreateSession", g_variant_new("(@a{sv})", g_variant_builder_end(&b)));
    if (ret < 0) {
        /* Cancellation can arrive in the same dispatch as a successful create.
         * Recover the new handle so cleanup revokes that session too. */
        if (p->response && p->code == 0)
            g_variant_lookup(p->response, "session_handle", "s", &p->session);
        goto fail;
    }
    if (!g_variant_lookup(p->response, "session_handle", "s", &p->session) ||
        !g_variant_is_object_path(p->session)) {
        ret = -EPROTO;
        goto fail;
    }
    g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&b, "{sv}", "handle_token", g_variant_new_string("gpukey_select"));
    g_variant_builder_add(&b, "{sv}", "types", g_variant_new_uint32(source_types & available));
    g_variant_builder_add(&b, "{sv}", "multiple", g_variant_new_boolean(false));
    if (version >= 2)
        g_variant_builder_add(&b, "{sv}", "cursor_mode", g_variant_new_uint32(cursor_mode));
    ret = request(p, "SelectSources",
                  g_variant_new("(o@a{sv})", p->session, g_variant_builder_end(&b)));
    if (ret < 0)
        goto fail;
    ret = request(p, "Start", g_variant_new("(os@a{sv})", p->session, "", options("gpukey_start")));
    if (ret < 0)
        goto fail;
    GVariant *streams = g_variant_lookup_value(p->response, "streams", G_VARIANT_TYPE("a(ua{sv})"));
    if (!streams || g_variant_n_children(streams) != 1) {
        g_clear_pointer(&streams, g_variant_unref);
        ret = -EPROTO;
        goto fail;
    }
    GVariant *stream = g_variant_get_child_value(streams, 0);
    g_variant_get(stream, "(u@a{sv})", &source->node, &props);
    if (version >= 6)
        g_variant_lookup(props, "pipewire-serial", "t", &source->serial);
    g_variant_unref(props);
    g_variant_unref(stream);
    g_variant_unref(streams);
    GUnixFDList *fds = nullptr;
    GVariant *reply = g_dbus_connection_call_with_unix_fd_list_sync(
        p->bus, bus_name, portal_path, screencast, "OpenPipeWireRemote",
        g_variant_new("(o@a{sv})", p->session, options("gpukey_remote")), G_VARIANT_TYPE("(h)"),
        G_DBUS_CALL_FLAGS_NONE, 30000, nullptr, &fds, nullptr, &error);
    if (!reply) {
        fprintf(stderr, "portal PipeWire remote: %s\n", error->message);
        g_error_free(error);
        ret = -EIO;
        goto fail;
    }
    int handle;
    g_variant_get(reply, "(h)", &handle);
    if (!fds || handle < 0 || handle >= g_unix_fd_list_get_length(fds)) {
        g_clear_object(&fds);
        g_variant_unref(reply);
        ret = -EPROTO;
        goto fail;
    }
    source->fd = g_unix_fd_list_get(fds, handle, &error); /* Independent owned dup. */
    g_object_unref(fds);
    g_variant_unref(reply);
    if (source->fd < 0) {
        g_clear_error(&error);
        ret = -EBADF;
        goto fail;
    }
    g_main_context_pop_thread_default(p->context);
    *out = p;
    return 0;
fail:
    g_main_context_pop_thread_default(p->context);
    gk_portal_close(&p);
    return ret;
}

bool gk_portal_closed(struct gk_portal *p)
{
    if (!p)
        return false;
    g_main_context_iteration(p->context, false);
    return p->closed || g_dbus_connection_is_closed(p->bus);
}

int gk_portal_wait_gpu(struct gk_portal *p, int capture_fd, int control_fd, int completion_fd,
                       int producer_fd, int timeout_ms)
{
    p->completion_error = 0;
    struct completion_source *completion = nullptr;
    if (completion_fd >= 0) {
        completion = completion_watch(p, completion_fd);
        if (!completion)
            return -ENOMEM;
        /* The fd stays registered and open for the codec's pool lifetime.
         * Only this context owner changes its events between iterations.
         * GLib reads the mask on every query; changing fd numbers in-place
         * would instead violate its sorted poll-record invariant. */
        completion->poll.events = G_IO_IN;
        completion->poll.revents = 0;
        completion->ready = false;
    }
    if (!p->input) {
        p->input = g_unix_fd_source_new(capture_fd, G_IO_IN | G_IO_ERR | G_IO_HUP);
        g_source_set_callback(p->input, G_SOURCE_FUNC(input_ready), p, nullptr);
        g_source_attach(p->input, p->context);
        p->deadline = g_source_new(&deadline_functions, sizeof(struct deadline_source));
        ((struct deadline_source *)p->deadline)->at = -1;
        g_source_attach(p->deadline, p->context);
    }
    if (control_fd >= 0 && !p->control) {
        p->control = g_unix_fd_source_new(control_fd, G_IO_IN | G_IO_ERR | G_IO_HUP);
        g_source_set_callback(p->control, G_SOURCE_FUNC(input_ready), p, nullptr);
        g_source_attach(p->control, p->context);
    }
    GSource *producer = nullptr;
    if (producer_fd >= 0) {
        producer = g_unix_fd_source_new(producer_fd, G_IO_IN | G_IO_ERR | G_IO_HUP);
        g_source_set_callback(producer, G_SOURCE_FUNC(input_ready), p, nullptr);
        g_source_attach(producer, p->context);
    }
    struct deadline_source *deadline = (void *)p->deadline;
    deadline->at = timeout_ms < 0 ? G_MAXINT64 : g_get_monotonic_time() + (gint64)timeout_ms * 1000;
    p->wake = false;
#ifdef GK_PORTAL_READY_TIME
    /* Correctness control: tolerate the wake generated by set_ready_time.
     * The default owner-only source avoids creating that wake in the first place. */
    g_source_set_ready_time(p->deadline, timeout_ms < 0 ? -1 : deadline->at);
#endif
    TracyCZoneN(wait, "portal.wait (wall time, includes sleep)", true);
    while (!p->wake && !p->closed && deadline->at >= 0 && !g_dbus_connection_is_closed(p->bus))
        g_main_context_iteration(p->context, true);
    TracyCZoneEnd(wait);
#ifdef GK_PORTAL_READY_TIME
    g_source_set_ready_time(p->deadline, -1);
#endif
    deadline->at = -1;
    if (completion) {
        completion->poll.events = 0;
        completion->poll.revents = 0;
    }
    if (producer) {
        g_source_destroy(producer);
        g_source_unref(producer);
    }
    return p->completion_error ? p->completion_error : completion && completion->ready;
}

int gk_portal_wait_completion(struct gk_portal *p, int capture_fd, int control_fd,
                              int completion_fd, int timeout_ms)
{
    return gk_portal_wait_gpu(p, capture_fd, control_fd, completion_fd, -1, timeout_ms);
}

void gk_portal_wait(struct gk_portal *p, int capture_fd, int control_fd, int timeout_ms)
{
    int ignored = gk_portal_wait_completion(p, capture_fd, control_fd, -1, timeout_ms);
    (void)ignored;
}

void gk_portal_close(struct gk_portal **portal)
{
    struct gk_portal *p = *portal;
    if (!p)
        return;
    gk_portal_clear_completion(p);
    free_sized(p->completions, p->completion_capacity * sizeof(*p->completions));
    if (p->bus) {
        if (p->request && !p->received) {
            GVariant *reply = g_dbus_connection_call_sync(
                p->bus, bus_name, p->request, "org.freedesktop.portal.Request", "Close", nullptr,
                nullptr, G_DBUS_CALL_FLAGS_NONE, 1000, nullptr, nullptr);
            g_clear_pointer(&reply, g_variant_unref);
        }
        if (p->session && g_variant_is_object_path(p->session)) {
            GVariant *reply = g_dbus_connection_call_sync(
                p->bus, bus_name, p->session, "org.freedesktop.portal.Session", "Close", nullptr,
                nullptr, G_DBUS_CALL_FLAGS_NONE, 1000, nullptr, nullptr);
            g_clear_pointer(&reply, g_variant_unref);
        }
        if (p->response_subscription)
            g_dbus_connection_signal_unsubscribe(p->bus, p->response_subscription);
        if (p->closed_subscription)
            g_dbus_connection_signal_unsubscribe(p->bus, p->closed_subscription);
        g_dbus_connection_close_sync(p->bus, nullptr, nullptr);
        g_object_unref(p->bus);
    }
    g_source_destroy(p->signal);
    g_source_unref(p->signal);
    if (p->input) {
        g_source_destroy(p->input);
        g_source_unref(p->input);
        g_source_destroy(p->deadline);
        g_source_unref(p->deadline);
    }
    if (p->control) {
        g_source_destroy(p->control);
        g_source_unref(p->control);
    }
    /* Unsubscribe on the subscribing thread; drain deferred subscription cleanup. */
    g_main_context_iteration(p->context, false);
    g_main_context_unref(p->context);
    g_clear_pointer(&p->response, g_variant_unref);
    g_free(p->request);
    g_free(p->session);
    g_free(p);
    *portal = nullptr;
}

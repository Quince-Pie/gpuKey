#include "devices.h"

#include <errno.h>
#include <gio/gio.h>
#include <pipewire/pipewire.h>
#include <stdckdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool gk_device_info(const struct spa_dict *props, struct gk_device_info *info)
{
    if (!props)
        return false;
    const char *kind = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
    if (!kind || (strcmp(kind, "Audio/Source") && strcmp(kind, "Audio/Source/Virtual") &&
                  strcmp(kind, "Audio/Sink")))
        return false;
    const char *serial = spa_dict_lookup(props, PW_KEY_OBJECT_SERIAL);
    const char *name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    if (!serial || !name)
        return false;
    const char *description = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
    *info = (struct gk_device_info){.serial = serial,
                                    .name = name,
                                    .description = description ? description : name,
                                    .monitor = !strcmp(kind, "Audio/Sink")};
    return true;
}

struct device {
    char *serial, *name, *description;
    bool monitor;
};
struct discovery {
    struct pw_main_loop *loop;
    struct device *devices;
    size_t count, capacity;
    int sync, error;
};

static void finish(struct discovery *d, int result)
{
    d->error = result;
    pw_main_loop_quit(d->loop);
}

static void global(void *opaque, uint32_t id, uint32_t permissions, const char *type,
                   uint32_t version, const struct spa_dict *props)
{
    (void)id;
    (void)permissions;
    (void)version;
    struct discovery *d = opaque;
    struct gk_device_info info;
    if (d->error || strcmp(type, PW_TYPE_INTERFACE_Node) || !gk_device_info(props, &info))
        return;
    if (d->count == d->capacity) {
        size_t capacity = d->capacity ? d->capacity * 2 : 16, bytes;
        if (capacity < d->capacity || ckd_mul(&bytes, capacity, sizeof(*d->devices))) {
            finish(d, -EOVERFLOW);
            return;
        }
        struct device *next = realloc(d->devices, bytes);
        if (!next) {
            finish(d, -ENOMEM);
            return;
        }
        d->devices = next;
        d->capacity = capacity;
    }
    struct device *device = &d->devices[d->count++];
    *device = (struct device){.serial = strdup(info.serial),
                              .name = strdup(info.name),
                              .description = strdup(info.description),
                              .monitor = info.monitor};
    if (!device->serial || !device->name || !device->description)
        finish(d, -ENOMEM);
}

static void done(void *opaque, uint32_t id, int seq)
{
    struct discovery *d = opaque;
    if (id == PW_ID_CORE && seq == d->sync)
        finish(d, d->error);
}

static void core_error(void *opaque, uint32_t id, int seq, int result, const char *message)
{
    (void)id;
    (void)seq;
    (void)message;
    finish(opaque, result < 0 ? result : -EIO);
}

static void timeout(void *opaque, uint64_t expirations)
{
    (void)expirations;
    finish(opaque, -ETIMEDOUT);
}

static int compare(const void *left, const void *right)
{
    const struct device *a = left, *b = right;
    if (a->monitor != b->monitor)
        return a->monitor ? 1 : -1;
    int order = strcmp(a->description, b->description);
    return order ? order : strcmp(a->name, b->name);
}

static void label(const char *text)
{
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p)
        if (*p < 32 || *p == 127)
            printf("\\x%02x", *p);
        else
            putchar(*p);
}

static void list_displays(void)
{
    char *address = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    if (!address)
        return;
    GDBusConnection *bus =
        g_dbus_connection_new_for_address_sync(address,
                                               G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                                   G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
                                               nullptr, nullptr, nullptr);
    g_free(address);
    if (!bus)
        return;
    GVariant *reply = g_dbus_connection_call_sync(
        bus, "org.gnome.Mutter.DisplayConfig", "/org/gnome/Mutter/DisplayConfig",
        "org.gnome.Mutter.DisplayConfig", "GetCurrentState", nullptr,
        G_VARIANT_TYPE("(ua((ssss)a(siiddada{sv})a{sv})a(iiduba(ssss)a{sv})a{sv})"),
        G_DBUS_CALL_FLAGS_NONE, 1000, nullptr, nullptr);
    if (reply) {
        GVariant *monitors = g_variant_get_child_value(reply, 1);
        if (g_variant_n_children(monitors) > 0) {
            puts("Displays (use -d NAME or --display NAME):");
            puts("  CONNECTOR    CURRENT MODE         DESCRIPTION");
            GVariantIter iter;
            g_variant_iter_init(&iter, monitors);
            GVariant *item;
            while ((item = g_variant_iter_next_value(&iter))) {
                GVariant *ids = g_variant_get_child_value(item, 0);
                GVariant *modes = g_variant_get_child_value(item, 1);
                const char *conn = "", *vendor = "", *product = "", *serial = "";
                g_variant_get(ids, "(&s&s&s&s)", &conn, &vendor, &product, &serial);
                const char *current_mode = "disabled";
                GVariantIter mode_iter;
                g_variant_iter_init(&mode_iter, modes);
                GVariant *mode;
                while ((mode = g_variant_iter_next_value(&mode_iter))) {
                    const char *mode_id = nullptr;
                    GVariant *props = g_variant_get_child_value(mode, 6);
                    gboolean is_current = false;
                    g_variant_get_child(mode, 0, "&s", &mode_id);
                    if (g_variant_lookup(props, "is-current", "b", &is_current) && is_current &&
                        mode_id)
                        current_mode = mode_id;
                    g_variant_unref(props);
                    g_variant_unref(mode);
                }
                printf("  %-12s %-20s ", conn, current_mode);
                if (*vendor) {
                    label(vendor);
                    putchar(' ');
                }
                label(product);
                putchar('\n');
                g_variant_unref(modes);
                g_variant_unref(ids);
                g_variant_unref(item);
            }
            putchar('\n');
        }
        g_variant_unref(monitors);
        g_variant_unref(reply);
    }
    g_dbus_connection_close_sync(bus, nullptr, nullptr);
    g_object_unref(bus);
}

int gk_devices_list(void)
{
    list_displays();
    pw_init(nullptr, nullptr);
    struct discovery d = {};
    struct pw_context *context = nullptr;
    struct pw_core *core = nullptr;
    struct pw_registry *registry = nullptr;
    struct spa_source *timer = nullptr;
    struct spa_hook core_hook, registry_hook;
    static const struct pw_core_events core_events = {
        .version = PW_VERSION_CORE_EVENTS, .done = done, .error = core_error};
    static const struct pw_registry_events registry_events = {.version = PW_VERSION_REGISTRY_EVENTS,
                                                              .global = global};
    int ret = -ENOMEM;
    d.loop = pw_main_loop_new(nullptr);
    if (!d.loop)
        goto cleanup;
    struct pw_loop *loop = pw_main_loop_get_loop(d.loop);
    context = pw_context_new(loop, nullptr, 0);
    if (!context)
        goto cleanup;
    core = pw_context_connect(context, nullptr, 0);
    if (!core) {
        ret = -errno;
        goto cleanup;
    }
    pw_core_add_listener(core, &core_hook, &core_events, &d);
    registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
    if (!registry)
        goto cleanup;
    pw_registry_add_listener(registry, &registry_hook, &registry_events, &d);
    d.sync = pw_core_sync(core, PW_ID_CORE, 0);
    if (d.sync < 0) {
        ret = d.sync;
        goto cleanup;
    }
    timer = pw_loop_add_timer(loop, timeout, &d);
    if (!timer)
        goto cleanup;
    ret = pw_loop_update_timer(loop, timer, &(struct timespec){.tv_sec = 5}, nullptr, false);
    if (ret < 0)
        goto cleanup;
    ret = pw_main_loop_run(d.loop);
    if (ret >= 0)
        ret = d.error;
    if (ret < 0)
        goto cleanup;
    if (d.count)
        qsort(d.devices, d.count, sizeof(*d.devices), compare);
    puts("Audio devices (IDs last until PipeWire restarts; names can be reused):");
    puts("  ID           TYPE      DESCRIPTION / NODE NAME");
    for (size_t i = 0; i < d.count; ++i) {
        const struct device *device = &d.devices[i];
        printf("  %-12s %-9s ", device->serial, device->monitor ? "monitor" : "input");
        label(device->description);
        fputs("\n                         ", stdout);
        label(device->name);
        putchar('\n');
    }
    if (!d.count)
        puts("  No audio devices are visible on this PipeWire connection.");
    puts("\nUse: gpukey clip.mkv --audio ID [--audio ID ...]\n"
         "Each selected device becomes a separate track; no audio is selected by default.\n"
         "Use --display or --window to choose a screen source in the desktop dialog.");
    if (ferror(stdout))
        ret = -EIO;
cleanup:
    if (timer)
        pw_loop_destroy_source(pw_main_loop_get_loop(d.loop), timer);
    if (registry)
        pw_proxy_destroy((struct pw_proxy *)registry);
    if (core)
        pw_core_disconnect(core);
    if (context)
        pw_context_destroy(context);
    if (d.loop)
        pw_main_loop_destroy(d.loop);
    for (size_t i = 0; i < d.count; ++i) {
        free(d.devices[i].serial);
        free(d.devices[i].name);
        free(d.devices[i].description);
    }
    free(d.devices);
    return ret;
}

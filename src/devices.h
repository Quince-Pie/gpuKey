#ifndef GPUKEY_DEVICES_H
#define GPUKEY_DEVICES_H

#include <spa/utils/dict.h>

struct gk_device_info {
    const char *serial, *name, *description;
    bool monitor;
};
/* Borrowed registry strings, valid only during the callback. */
bool gk_device_info(const struct spa_dict *props, struct gk_device_info *info);
[[nodiscard]] int gk_devices_list(void);

#endif

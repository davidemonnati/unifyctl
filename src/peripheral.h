#ifndef UNIFYCTL_PERIPHERAL_H
#define UNIFYCTL_PERIPHERAL_H

#include "devices.h"

enum info_availability { INFO_UNKNOWN, INFO_VALUE, INFO_UNSUPPORTED, INFO_NOT_APPLICABLE, INFO_UNREACHABLE };
enum info_connection { INFO_LINK_UNKNOWN, INFO_LINK_REACHABLE, INFO_LINK_UNREACHABLE };
enum battery_level { BATTERY_UNKNOWN, BATTERY_CRITICAL, BATTERY_LOW, BATTERY_GOOD, BATTERY_FULL };

/* Live observations are separate from receiver-owned identity. Zero is a value
 * only where the protocol defines it; availability never depends on truthiness. */
struct peripheral_info {
    enum info_availability battery, power, channel, dpi, backlight, signal;
    enum info_connection connection;
    bool has_percentage;
    unsigned percentage;
    enum battery_level battery_level;
    const char *charging;
    unsigned host_channel;
    unsigned sensor_count;
    uint16_t sensor_dpi[255];
    bool sensor_has_y[255];
    uint16_t sensor_dpi_y[255];
    bool backlight_enabled;
    bool has_backlight_level;
    unsigned backlight_level;
};

/* Caller supplies a shared hardware phase deadline in h->deadline. */
int peripheral_read(struct hidpp *h, const struct device *device,
                    struct peripheral_info *info, struct error *err);
void peripheral_print(FILE *out, const struct device *device, const struct peripheral_info *info);

#endif

#include "peripheral.h"

#include <inttypes.h>

struct feature {
    uint8_t index, version;
    enum info_availability state;
};

/* These requests and layouts are independently implemented from the references
 * in PROTOCOL_INFO.md. No external implementation code is incorporated. */
static int query(struct hidpp *h, unsigned slot, uint8_t index, unsigned function,
                 uint8_t argument, size_t size, struct report *r, struct error *err) {
    const uint8_t params[] = {argument, 0, 0};
    return hidpp_feature_request(h, slot, index, function, params, size, r, err);
}

static bool unsupported(int status, const struct report *r, const struct error *err) {
    return status == UC_PROTOCOL && r->length == HIDPP_LONG &&
           r->bytes[2] == 0xff && err->protocol_code == 9;
}

static int discover(struct hidpp *h, unsigned slot, uint16_t id, struct feature *f, struct error *err) {
    const uint8_t params[] = {(uint8_t)(id >> 8), (uint8_t)id, 0};
    struct report r;
    int status = hidpp_feature_request(h, slot, 0, 0, params, 3, &r, err);
    if (status) return status;
    if (r.bytes[4] == 0xff) {
        h->poisoned = true;
        return fail(err, UC_PROTOCOL, 0, 0, "invalid feature discovery index");
    }
    *f = (struct feature){.index = r.bytes[4], .version = r.bytes[6],
                          .state = r.bytes[4] ? INFO_VALUE : INFO_UNSUPPORTED};
    /* Hidden/engineering features are not a supported public query interface. */
    if (f->index && (r.bytes[5] & 0x60)) f->state = INFO_UNKNOWN;
    return UC_OK;
}

/* Correctly matched UNSUPPORTED completes a transaction. Every other error is
 * fatal; busy, resource errors and silence must never become "unsupported". */
static int read_feature(struct hidpp *h, unsigned slot, const struct feature *f,
                        unsigned function, size_t size, struct report *r,
                        enum info_availability *state, struct error *err) {
    int status = query(h, slot, f->index, function, 0, size, r, err);
    if (unsupported(status, r, err)) {
        *state = INFO_UNSUPPORTED;
        *err = (struct error){0};
        return UC_OK;
    }
    return status;
}

static int malformed(struct hidpp *h, struct error *err, const char *field) {
    h->poisoned = true;
    return fail(err, UC_PROTOCOL, 0, 0, "invalid peripheral %s reply", field);
}

static int battery_read(struct hidpp *h, unsigned slot, struct peripheral_info *i, struct error *err) {
    struct feature f;
    int status = discover(h, slot, 0x1004, &f, err);
    if (status) return status;
    i->battery = f.state;
    if (f.state == INFO_VALUE) {
        struct report r;
        status = read_feature(h, slot, &f, 0, 2, &r, &i->battery, err);
        if (status || i->battery != INFO_VALUE) return status;
        bool percentage = (r.bytes[5] & 2) != 0;
        uint8_t levels = r.bytes[4];
        status = read_feature(h, slot, &f, 1, 4, &r, &i->battery, err);
        if (status || i->battery != INFO_VALUE) return status;
        if (percentage && r.bytes[4] > 100) return malformed(h, err, "battery percentage");
        i->has_percentage = percentage;
        i->percentage = r.bytes[4]; /* 0% is valid when explicitly supported. */
        uint8_t level = r.bytes[5];
        if (level && (level & levels) == level) {
            switch (level) {
            case 1: i->battery_level = BATTERY_CRITICAL; break;
            case 2: i->battery_level = BATTERY_LOW; break;
            case 4: i->battery_level = BATTERY_GOOD; break;
            case 8: i->battery_level = BATTERY_FULL; break;
            default: return malformed(h, err, "battery level");
            }
        }
        static const char *const charging[] = {"discharging", "charging", "charging slowly", "charge complete", "charging error"};
        if (r.bytes[6] >= sizeof(charging) / sizeof(charging[0])) return malformed(h, err, "charging status");
        i->charging = charging[r.bytes[6]];
        return UC_OK;
    }
    if (f.state != INFO_UNSUPPORTED) return UC_OK;
    status = discover(h, slot, 0x1000, &f, err);
    if (status) return status;
    i->battery = f.state;
    if (f.state == INFO_VALUE) {
        struct report r;
        status = read_feature(h, slot, &f, 1, 2, &r, &i->battery, err);
        if (status || i->battery != INFO_VALUE) return status;
        unsigned levels = r.bytes[4];
        if (levels < 2 || levels > 100) return malformed(h, err, "battery capabilities");
        bool percentage = levels >= 10 && (r.bytes[5] & 2) != 0;
        status = read_feature(h, slot, &f, 0, 3, &r, &i->battery, err);
        if (status || i->battery != INFO_VALUE) return status;
        unsigned value = r.bytes[4];
        if (value > 100 || r.bytes[5] > value || r.bytes[6] > 7) return malformed(h, err, "battery status");
        i->has_percentage = percentage && value != 0; /* 1000: zero means unknown. */
        i->percentage = value;
        if (!percentage && value) i->battery_level = value <= 10 ? BATTERY_CRITICAL : value <= 30 ? BATTERY_LOW : value <= 80 ? BATTERY_GOOD : BATTERY_FULL;
        static const char *const charging[] = {"discharging", "charging", "charge in final stage", "charge complete", "charging slowly", "invalid battery type", "thermal error", "charging error"};
        i->charging = charging[r.bytes[6]];
        return UC_OK;
    }
    if (f.state != INFO_UNSUPPORTED) return UC_OK;
    /* Voltage-only firmware does not establish a percentage or coarse level. */
    status = discover(h, slot, 0x1001, &f, err);
    if (!status && f.state != INFO_UNSUPPORTED) i->battery = INFO_UNKNOWN;
    return status;
}

static int channel_read(struct hidpp *h, unsigned slot, struct peripheral_info *i, struct error *err) {
    struct feature f;
    int status = discover(h, slot, 0x1814, &f, err);
    if (status) return status;
    i->channel = f.state;
    if (f.state != INFO_VALUE) return UC_OK;
    struct report r;
    status = read_feature(h, slot, &f, 0, 2, &r, &i->channel, err);
    if (status || i->channel != INFO_VALUE) return status;
    if (!r.bytes[4] || r.bytes[5] >= r.bytes[4]) return malformed(h, err, "host channel");
    i->host_channel = (unsigned)r.bytes[5] + 1;
    return UC_OK;
}

static int dpi_read(struct hidpp *h, unsigned slot, struct peripheral_info *i, struct error *err) {
    if (i->dpi == INFO_NOT_APPLICABLE) return UC_OK;
    struct feature f;
    int status = discover(h, slot, 0x2201, &f, err);
    if (status) return status;
    bool extended = false;
    if (f.state == INFO_UNSUPPORTED) {
        status = discover(h, slot, 0x2202, &f, err);
        if (status) return status;
        extended = true;
    }
    i->dpi = f.state;
    if (f.state == INFO_VALUE) {
        struct report r;
        status = read_feature(h, slot, &f, 0, 1, &r, &i->dpi, err);
        if (status || i->dpi != INFO_VALUE) return status;
        i->sensor_count = r.bytes[4];
        if (!i->sensor_count) return malformed(h, err, "sensor count");
        for (unsigned sensor = 0; sensor < i->sensor_count; sensor++) {
            if (extended) {
                status = query(h, slot, f.index, 1, (uint8_t)sensor, 3, &r, err);
                if (unsupported(status, &r, err)) {
                    i->dpi = INFO_UNSUPPORTED;
                    *err = (struct error){0};
                    return UC_OK;
                }
                if (status) return status;
                if (r.bytes[4] != sensor) return malformed(h, err, "sensor capability index");
                i->sensor_has_y[sensor] = (r.bytes[6] & 1) != 0;
            }
            status = query(h, slot, f.index, extended ? 5u : 2u, (uint8_t)sensor, extended ? 10u : 3u, &r, err);
            if (unsupported(status, &r, err)) {
                i->dpi = INFO_UNSUPPORTED;
                *err = (struct error){0};
                return UC_OK;
            }
            if (status) return status;
            if (r.bytes[4] != sensor) return malformed(h, err, "sensor index");
            i->sensor_dpi[sensor] = (uint16_t)((uint16_t)r.bytes[5] << 8 | r.bytes[6]);
            if (extended && i->sensor_has_y[sensor]) {
                i->sensor_dpi_y[sensor] = (uint16_t)((uint16_t)r.bytes[9] << 8 | r.bytes[10]);
                if (!i->sensor_dpi_y[sensor]) i->dpi = INFO_UNKNOWN;
            }
            /* Do not substitute the default field for an unreported current DPI. */
            if (!i->sensor_dpi[sensor]) i->dpi = INFO_UNKNOWN;
        }
        return UC_OK;
    }
    return UC_OK;
}

static int backlight_read(struct hidpp *h, unsigned slot, struct peripheral_info *i, struct error *err) {
    if (i->backlight == INFO_NOT_APPLICABLE) return UC_OK;
    struct feature f;
    int status = discover(h, slot, 0x1982, &f, err);
    if (status) return status;
    i->backlight = f.state;
    if (f.state == INFO_VALUE) {
        struct report r;
        status = read_feature(h, slot, &f, 0, 6, &r, &i->backlight, err);
        if (status || i->backlight != INFO_VALUE) return status;
        if (r.bytes[4] > 1) return malformed(h, err, "backlight enablement");
        i->backlight_enabled = r.bytes[4] != 0;
        /* The documented v3 layout labels this a manual configuration level,
         * not a measurement of whether the LEDs are currently illuminated. */
        if (f.version >= 3) {
            if (r.bytes[9] > 7) return malformed(h, err, "backlight level");
            i->has_backlight_level = true;
            i->backlight_level = r.bytes[9];
        }
        return UC_OK;
    }
    if (f.state != INFO_UNSUPPORTED) return UC_OK;
    const uint16_t alternatives[] = {0x1981, 0x1983};
    for (size_t n = 0; n < sizeof(alternatives) / sizeof(alternatives[0]); n++) {
        status = discover(h, slot, alternatives[n], &f, err);
        if (status) return status;
        if (f.state != INFO_UNSUPPORTED) i->backlight = INFO_UNKNOWN;
    }
    return UC_OK;
}

static int read_live(struct hidpp *h, const struct device *d, struct peripheral_info *i, struct error *err) {
    *i = (struct peripheral_info){0};
    if (d->type == 1 || d->type == 3) i->dpi = INFO_NOT_APPLICABLE;
    if (d->type == 2 || d->type == 8) i->backlight = INFO_NOT_APPLICABLE;
    const uint8_t ping[] = {0, 0, 0x5a};
    struct report r;
    int status = hidpp_feature_request(h, d->slot, 0, 1, ping, 3, &r, err);
    if (status) {
        if (status != UC_PROTOCOL || h->poisoned || !r.length || r.bytes[2] != 0x8f || err->protocol_code != 1) return status;
        /* INVALID_SUBID establishes legacy HID++ 1.0, not absence of battery
         * hardware. Keep unsupported protocol paths unknown, preserving identity. */
        i->connection = INFO_LINK_REACHABLE;
        *err = (struct error){0};
        return UC_OK;
    }
    if (r.bytes[6] != ping[2]) return malformed(h, err, "ping echo");
    i->connection = INFO_LINK_REACHABLE;
    if (r.bytes[4] != 2 && r.bytes[4] != 4) return UC_OK;
    status = battery_read(h, d->slot, i, err);
    if (!status) status = channel_read(h, d->slot, i, err);
    if (!status) status = dpi_read(h, d->slot, i, err);
    if (!status) status = backlight_read(h, d->slot, i, err);
    if (!status) {
        struct feature f;
        status = discover(h, d->slot, 0x0080, &f, err);
        if (!status) i->signal = f.state == INFO_UNSUPPORTED ? INFO_UNSUPPORTED : INFO_UNKNOWN;
    }
    return status;
}

int peripheral_read(struct hidpp *h, const struct device *d, struct peripheral_info *i, struct error *err) {
    int status = read_live(h, d, i, err);
    if (status == UC_PROTOCOL && !h->poisoned && h->peripheral_unreachable) {
        enum info_availability dpi = i->dpi == INFO_NOT_APPLICABLE ? INFO_NOT_APPLICABLE : INFO_UNREACHABLE;
        enum info_availability backlight = i->backlight == INFO_NOT_APPLICABLE ? INFO_NOT_APPLICABLE : INFO_UNREACHABLE;
        *i = (struct peripheral_info){.connection = INFO_LINK_UNREACHABLE,
                                     .battery = INFO_UNREACHABLE, .channel = INFO_UNREACHABLE,
                                     .dpi = dpi, .backlight = backlight, .signal = INFO_UNREACHABLE};
        *err = (struct error){0};
        return UC_OK;
    }
    return status;
}

static const char *availability(enum info_availability state) {
    switch (state) {
    case INFO_UNSUPPORTED: return "unsupported";
    case INFO_NOT_APPLICABLE: return "not applicable";
    case INFO_UNREACHABLE: return "unavailable (device unreachable)";
    default: return "unknown";
    }
}

void peripheral_print(FILE *out, const struct device *d, const struct peripheral_info *i) {
    fprintf(out, "Slot:           %u\nName:           %s\nType:           %s\nWPID:           %04x\nSerial:         ",
            d->slot, d->name[0] ? d->name : "unknown", device_type_name(d->type), (unsigned)d->wpid);
    if (d->has_serial) fprintf(out, "%08" PRIx32 "\n", d->serial);
    else fputs("unknown\n", out);
    fputs("Battery:        ", out);
    if (i->battery == INFO_VALUE) {
        static const char *const levels[] = {"unknown", "critical", "low", "good", "full"};
        if (i->has_percentage) fprintf(out, "%u%%", i->percentage);
        else fputs(levels[i->battery_level], out);
        if (i->charging) fprintf(out, " (%s)", i->charging);
    } else fputs(availability(i->battery), out);
    fprintf(out, "\nConnection:     %s\nPower:          %s\nActive channel: ",
            i->connection == INFO_LINK_REACHABLE ? "reachable" : i->connection == INFO_LINK_UNREACHABLE ? "unreachable" : "unknown", availability(i->power));
    if (i->channel == INFO_VALUE) fprintf(out, "%u (host channel, 1-based)", i->host_channel);
    else fputs(availability(i->channel), out);
    fputs("\nDPI:            ", out);
    if (i->dpi == INFO_VALUE) {
        for (unsigned sensor = 0; sensor < i->sensor_count; sensor++) {
            if (sensor) fputs(", ", out);
            if (i->sensor_count > 1) fprintf(out, "sensor %u: ", sensor);
            if (i->sensor_has_y[sensor]) fprintf(out, "X %u DPI / Y %u DPI", (unsigned)i->sensor_dpi[sensor], (unsigned)i->sensor_dpi_y[sensor]);
            else fprintf(out, "%u DPI", (unsigned)i->sensor_dpi[sensor]);
        }
    } else fputs(availability(i->dpi), out);
    fputs("\nBacklight:      ", out);
    if (i->backlight == INFO_VALUE) {
        fprintf(out, "%s (configured enablement)", i->backlight_enabled ? "enabled" : "disabled");
        if (i->has_backlight_level) fprintf(out, "; configured manual level %u (device steps 0–7)", i->backlight_level);
        else fputs("; level unknown", out);
    } else fputs(availability(i->backlight), out);
    fprintf(out, "\nSignal quality: %s\n", availability(i->signal));
}

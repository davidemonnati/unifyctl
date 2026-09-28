#ifndef UNIFYCTL_DEVICES_H
#define UNIFYCTL_DEVICES_H

#include "hidpp.h"

enum connectivity { LINK_UNKNOWN, LINK_CONNECTED, LINK_DISCONNECTED };
struct device {
    unsigned slot;
    bool paired;
    uint8_t type;
    uint16_t wpid;
    bool has_serial;
    uint32_t serial;
    char name[15];
    enum connectivity link;
};

int device_read(struct hidpp *h, unsigned slot, struct device *d, bool metadata, struct error *err);
int devices_read(struct hidpp *h, struct device devices[SLOT_COUNT], bool metadata, struct error *err);
void device_print(FILE *out, const struct device *d);
void devices_print(FILE *out, const struct device devices[SLOT_COUNT]);
bool device_same(const struct device *a, const struct device *b);

#endif

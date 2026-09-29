#include "discovery.h"

#include <errno.h>
#include <fcntl.h>
#include <libudev.h>
#include <linux/hidraw.h>
#include <linux/input.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

static bool hex_attr(struct udev_device *device, const char *name, unsigned *value) {
    const char *text = udev_device_get_sysattr_value(device, name);
    if (!text || !*text) return false;
    char *end;
    errno = 0;
    unsigned long n = strtoul(text, &end, 16);
    if (errno || *end || n > 0xffff) return false;
    *value = (unsigned)n;
    return true;
}

static bool identify(struct udev_device *device, struct receiver *receiver) {
    struct udev_device *usb = udev_device_get_parent_with_subsystem_devtype(device, "usb", "usb_device");
    struct udev_device *interface = udev_device_get_parent_with_subsystem_devtype(device, "usb", "usb_interface");
    struct udev_device *hid = udev_device_get_parent_with_subsystem_devtype(device, "hid", NULL);
    unsigned vid, pid, number, bus, hid_vid, hid_pid;
    if (!usb || !interface || !hid) return false;
    const char *id = udev_device_get_property_value(hid, "HID_ID");
    if (!id || sscanf(id, "%x:%x:%x", &bus, &hid_vid, &hid_pid) != 3) return false;
    if (!hex_attr(usb, "idVendor", &vid) || !hex_attr(usb, "idProduct", &pid) ||
        !hex_attr(interface, "bInterfaceNumber", &number)) return false;
    if (!receiver_supported(vid, pid, number) || bus != BUS_USB || hid_vid != vid || hid_pid != pid) return false;
    const char *path = udev_device_get_devnode(device);
    const char *physical = udev_device_get_syspath(usb);
    if (!path || !physical || strlen(path) >= sizeof(receiver->path) || strlen(physical) >= sizeof(receiver->physical)) return false;
    strcpy(receiver->path, path);
    strcpy(receiver->physical, physical);
    receiver->product = (uint16_t)pid;
    return true;
}

static int enumerate(struct udev *udev, struct receivers *receivers, struct error *err) {
    struct udev_enumerate *scan = udev_enumerate_new(udev);
    if (!scan) return fail(err, UC_INTERNAL, ENOMEM, 0, "cannot allocate udev enumeration");
    int result = udev_enumerate_add_match_subsystem(scan, "hidraw");
    if (result >= 0) result = udev_enumerate_scan_devices(scan);
    if (result < 0) {
        udev_enumerate_unref(scan);
        return fail(err, UC_IO, -result, 0, "cannot enumerate hidraw devices");
    }
    struct udev_list_entry *entry;
    int status = UC_OK;
    udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(scan)) {
        struct udev_device *device = udev_device_new_from_syspath(udev, udev_list_entry_get_name(entry));
        struct receiver candidate;
        bool valid = device && identify(device, &candidate);
        if (device) udev_device_unref(device);
        if (!valid) continue;
        bool duplicate = false;
        for (size_t i = 0; i < receivers->count; i++) {
            if (!strcmp(receivers->entries[i].physical, candidate.physical)) duplicate = true;
        }
        if (duplicate) continue;
        if (receivers->count == MAX_RECEIVERS) {
            status = fail(err, UC_RECEIVER, 0, 0, "too many receivers; use --receiver PATH");
            break;
        }
        receivers->entries[receivers->count++] = candidate;
    }
    udev_enumerate_unref(scan);
    return status;
}

static int validated_open(struct udev *udev, const char *path, struct error *err) {
    struct stat before, after;
    if (stat(path, &before) < 0) {
        fail(err, UC_ACCESS, errno, 0, "cannot access receiver path %s", path);
        return -1;
    }
    if (!S_ISCHR(before.st_mode)) {
        fail(err, UC_RECEIVER, 0, 0, "receiver path is not a character device");
        return -1;
    }
    struct udev_device *device = udev_device_new_from_devnum(udev, 'c', before.st_rdev);
    struct receiver identity;
    const char *subsystem = device ? udev_device_get_subsystem(device) : NULL;
    bool valid = subsystem && !strcmp(subsystem, "hidraw") && identify(device, &identity);
    if (device) udev_device_unref(device);
    if (!valid) {
        fail(err, UC_RECEIVER, 0, 0, "path is not a supported Unifying management interface");
        return -1;
    }
    int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC | O_NOCTTY);
    if (fd < 0) {
        fail(err, UC_ACCESS, errno, 0, "cannot open %s; check scoped udev permissions", path);
        return -1;
    }
    if (fstat(fd, &after) < 0 || !S_ISCHR(after.st_mode) || before.st_rdev != after.st_rdev ||
        before.st_ino != after.st_ino || before.st_dev != after.st_dev) {
        fail(err, UC_RECEIVER, 0, 0, "receiver path changed during open");
        goto failed;
    }
    struct hidraw_devinfo info = {0};
    if (ioctl(fd, HIDIOCGRAWINFO, &info) < 0) {
        fail(err, UC_IO, errno, 0, "cannot read hidraw identity");
        goto failed;
    }
    if (info.bustype != BUS_USB || (uint16_t)info.vendor != 0x046d || (uint16_t)info.product != identity.product) {
        fail(err, UC_RECEIVER, 0, 0, "opened hidraw identity does not match receiver");
        goto failed;
    }
    int size = 0;
    struct hidraw_report_descriptor descriptor = {0};
    if (ioctl(fd, HIDIOCGRDESCSIZE, &size) < 0 || size <= 0 || (size_t)size > sizeof(descriptor.value)) {
        fail(err, UC_RECEIVER, 0, 0, "invalid or unavailable HID descriptor size");
        goto failed;
    }
    descriptor.size = (uint32_t)size;
    if (ioctl(fd, HIDIOCGRDESC, &descriptor) < 0) {
        fail(err, UC_IO, errno, 0, "cannot read HID descriptor");
        goto failed;
    }
    if (descriptor.size > sizeof(descriptor.value) || !receiver_descriptor(descriptor.value, descriptor.size)) {
        fail(err, UC_RECEIVER, 0, 0, "management interface lacks supported HID++ input/output reports");
        goto failed;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
        fail(err, UC_ACCESS, errno, 0, "receiver is locked by another unifyctl process");
        goto failed;
    }
    return fd;
failed:
    close(fd);
    return -1;
}

static int open_receiver(const char *explicit_path, FILE *diagnostics, struct error *err) {
    struct udev *udev = udev_new();
    if (!udev) {
        fail(err, UC_INTERNAL, ENOMEM, 0, "cannot initialize libudev");
        return -1;
    }
    int fd = -1;
    if (explicit_path) fd = validated_open(udev, explicit_path, err);
    else {
        struct receivers *receivers = calloc(1, sizeof(*receivers));
        if (!receivers) fail(err, UC_INTERNAL, ENOMEM, 0, "cannot allocate receiver list");
        else {
            size_t index = 0;
            if (!enumerate(udev, receivers, err)) {
                if (!receiver_choose(receivers, &index, err)) fd = validated_open(udev, receivers->entries[index].path, err);
                else {
                    for (size_t i = 0; i < receivers->count; i++) fprintf(diagnostics, "  %s  Logitech Unifying 046d:%04x\n", receivers->entries[i].path, (unsigned)receivers->entries[i].product);
                }
            }
            free(receivers);
        }
    }
    udev_unref(udev);
    return fd;
}

struct receiver_session {
    struct raw_transport raw;
};

int receiver_connect(const char *explicit_path, FILE *diagnostics,
                     struct receiver_session **session, struct transport *io,
                     struct error *err) {
    *session = NULL;
    int fd = open_receiver(explicit_path, diagnostics, err);
    if (fd < 0) return (int)err->status;
    struct receiver_session *s = calloc(1, sizeof(*s));
    if (!s) {
        close(fd);
        return fail(err, UC_INTERNAL, ENOMEM, 0, "cannot allocate receiver session");
    }
    int status = raw_init(&s->raw, fd, io, err);
    if (status) {
        raw_close(&s->raw);
        free(s);
        return status;
    }
    *session = s;
    return UC_OK;
}

void receiver_disconnect(struct receiver_session *session) {
    if (!session) return;
    raw_close(&session->raw);
    free(session);
}

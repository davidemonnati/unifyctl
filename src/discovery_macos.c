#include "discovery.h"
#include "transport_macos.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

/* HID_MAX_DESCRIPTOR_SIZE on Linux; bounds the IOKit descriptor as well. */
enum { MAX_DESCRIPTOR = 4096 };

struct receiver_session {
    struct iokit_transport transport;
    int lock_fd;
};

/* Reads a nonnegative number from the entry itself or, when parents is set,
 * from the nearest ancestor in the IOService plane (USB interface/device). */
static bool number_property(io_service_t service, CFStringRef key, bool parents, uint64_t *value) {
    CFTypeRef ref = parents
        ? IORegistryEntrySearchCFProperty(service, kIOServicePlane, key, kCFAllocatorDefault,
                                          kIORegistryIterateRecursively | kIORegistryIterateParents)
        : IORegistryEntryCreateCFProperty(service, key, kCFAllocatorDefault, 0);
    if (!ref) return false;
    long long n = -1;
    bool valid = CFGetTypeID(ref) == CFNumberGetTypeID() &&
                 CFNumberGetValue((CFNumberRef)ref, kCFNumberLongLongType, &n) && n >= 0;
    CFRelease(ref);
    if (!valid) return false;
    *value = (uint64_t)n;
    return true;
}

static bool string_property_is(io_service_t service, CFStringRef key, CFStringRef expected) {
    CFTypeRef ref = IORegistryEntryCreateCFProperty(service, key, kCFAllocatorDefault, 0);
    if (!ref) return false;
    bool equal = CFGetTypeID(ref) == CFStringGetTypeID() &&
                 CFStringCompare((CFStringRef)ref, expected, 0) == kCFCompareEqualTo;
    CFRelease(ref);
    return equal;
}

static bool descriptor_valid(io_service_t service) {
    CFTypeRef ref = IORegistryEntryCreateCFProperty(service, CFSTR(kIOHIDReportDescriptorKey), kCFAllocatorDefault, 0);
    if (!ref) return false;
    bool valid = false;
    if (CFGetTypeID(ref) == CFDataGetTypeID()) {
        CFIndex length = CFDataGetLength((CFDataRef)ref);
        const uint8_t *bytes = CFDataGetBytePtr((CFDataRef)ref);
        valid = bytes && length > 0 && length <= MAX_DESCRIPTOR && receiver_descriptor(bytes, (size_t)length);
    }
    CFRelease(ref);
    return valid;
}

/* Mirrors the Linux checks: USB transport, allowlisted VID/PID on the HID
 * device matching its USB ancestor, management interface number, and a
 * report descriptor with the expected HID++ short/long reports. */
static bool identify(io_service_t service, struct receiver *receiver) {
    uint64_t vid, pid, number, usb_vid, usb_pid, id, location;
    if (!IOObjectConformsTo(service, kIOHIDDeviceKey)) return false;
    if (!string_property_is(service, CFSTR(kIOHIDTransportKey), CFSTR(kIOHIDTransportUSBValue))) return false;
    if (!number_property(service, CFSTR(kIOHIDVendorIDKey), false, &vid) ||
        !number_property(service, CFSTR(kIOHIDProductIDKey), false, &pid) ||
        !number_property(service, CFSTR("bInterfaceNumber"), true, &number) ||
        !number_property(service, CFSTR("idVendor"), true, &usb_vid) ||
        !number_property(service, CFSTR("idProduct"), true, &usb_pid)) return false;
    if (vid > 0xffff || pid > 0xffff || number > 0xff) return false;
    if (!receiver_supported((unsigned)vid, (unsigned)pid, (unsigned)number) || usb_vid != vid || usb_pid != pid) return false;
    if (!descriptor_valid(service)) return false;
    if (IORegistryEntryGetRegistryEntryID(service, &id) != KERN_SUCCESS || !id) return false;
    snprintf(receiver->path, sizeof(receiver->path), "DevSrvsID:%" PRIu64, id);
    if (number_property(service, CFSTR(kIOHIDLocationIDKey), false, &location)) snprintf(receiver->physical, sizeof(receiver->physical), "location:0x%08" PRIx64, location);
    else snprintf(receiver->physical, sizeof(receiver->physical), "entry:%" PRIu64, id);
    receiver->product = (uint16_t)pid;
    return true;
}

static int enumerate(struct receivers *receivers, struct error *err) {
    CFMutableDictionaryRef match = IOServiceMatching(kIOHIDDeviceKey);
    if (!match) return fail(err, UC_INTERNAL, ENOMEM, 0, "cannot allocate IOKit matching dictionary");
    io_iterator_t iterator = IO_OBJECT_NULL;
    /* Consumes match. MACH_PORT_NULL selects the default main port. */
    kern_return_t kr = IOServiceGetMatchingServices(MACH_PORT_NULL, match, &iterator);
    if (kr != KERN_SUCCESS) return fail(err, UC_IO, 0, 0, "cannot enumerate IOKit HID devices (0x%08x)", (unsigned)kr);
    int status = UC_OK;
    io_service_t service;
    while ((service = IOIteratorNext(iterator)) != IO_OBJECT_NULL) {
        struct receiver candidate;
        bool valid = identify(service, &candidate);
        IOObjectRelease(service);
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
    IOObjectRelease(iterator);
    return status;
}

static int lookup(const char *path, io_service_t *service, struct error *err) {
    uint64_t id;
    if (!receiver_registry_id(path, &id)) return fail(err, UC_RECEIVER, 0, 0, "on macOS --receiver takes an IOKit ID such as DevSrvsID:4294968397, not %s", path);
    CFMutableDictionaryRef match = IORegistryEntryIDMatching(id);
    if (!match) return fail(err, UC_INTERNAL, ENOMEM, 0, "cannot allocate IOKit matching dictionary");
    *service = IOServiceGetMatchingService(MACH_PORT_NULL, match); /* Consumes match. */
    if (*service == IO_OBJECT_NULL) return fail(err, UC_RECEIVER, 0, 0, "no IOKit device %s; it may have been unplugged", path);
    return UC_OK;
}

/* flock() on a per-user lock file keyed by the IORegistry entry ID. It
 * coordinates only cooperating unifyctl processes of the same user. */
static int lock_receiver(const char *path, struct error *err) {
    char directory[PATH_MAX], name[PATH_MAX];
    size_t size = confstr(_CS_DARWIN_USER_TEMP_DIR, directory, sizeof(directory));
    if (size < 2 || size > sizeof(directory)) {
        fail(err, UC_ACCESS, errno, 0, "cannot locate per-user temporary directory for the receiver lock");
        return -1;
    }
    const char *separator = directory[strlen(directory) - 1] == '/' ? "" : "/";
    int length = snprintf(name, sizeof(name), "%s%sunifyctl-%s.lock", directory, separator, path);
    if (length < 0 || (size_t)length >= sizeof(name)) {
        fail(err, UC_INTERNAL, 0, 0, "receiver lock path is too long");
        return -1;
    }
    int fd = open(name, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        fail(err, UC_ACCESS, errno, 0, "cannot open receiver lock %s", name);
        return -1;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
        fail(err, UC_ACCESS, errno, 0, "receiver is locked by another unifyctl process");
        close(fd);
        return -1;
    }
    return fd;
}

static int open_failure(IOReturn result, struct error *err) {
    if (result == kIOReturnNotPermitted || result == kIOReturnNotPrivileged) return fail(err, UC_ACCESS, 0, 0, "macOS denied access to the receiver (IOReturn 0x%08x); check Privacy & Security settings for this terminal", (unsigned)result);
    if (result == kIOReturnExclusiveAccess) return fail(err, UC_ACCESS, 0, 0, "receiver interface is held exclusively by another process");
    if (result == kIOReturnNoDevice || result == kIOReturnNotAttached) return fail(err, UC_IO, 0, 0, "receiver disconnected");
    return fail(err, UC_IO, 0, 0, "cannot open receiver (IOReturn 0x%08x)", (unsigned)result);
}

static int open_service(io_service_t service, struct receiver_session **session, struct transport *io, struct error *err) {
    struct receiver identity;
    if (!identify(service, &identity)) return fail(err, UC_RECEIVER, 0, 0, "device is not a supported Unifying management interface");
    int lock_fd = lock_receiver(identity.path, err);
    if (lock_fd < 0) return (int)err->status;
    struct receiver_session *s = calloc(1, sizeof(*s));
    if (!s) {
        close(lock_fd);
        return fail(err, UC_INTERNAL, ENOMEM, 0, "cannot allocate receiver session");
    }
    s->lock_fd = lock_fd;
    IOHIDDeviceRef device = IOHIDDeviceCreate(kCFAllocatorDefault, service);
    if (!device) {
        close(lock_fd);
        free(s);
        return fail(err, UC_IO, 0, 0, "cannot create HID device reference; receiver may have been unplugged");
    }
    /* Shared, non-seizing open keeps normal keyboard and mouse input intact. */
    IOReturn result = IOHIDDeviceOpen(device, kIOHIDOptionsTypeNone);
    if (result != kIOReturnSuccess) {
        CFRelease(device);
        close(lock_fd);
        free(s);
        return open_failure(result, err);
    }
    int status = iokit_init(&s->transport, device, io, err);
    if (status) {
        receiver_disconnect(s);
        return status;
    }
    *session = s;
    return UC_OK;
}

int receiver_connect(const char *explicit_path, FILE *diagnostics,
                     struct receiver_session **session, struct transport *io,
                     struct error *err) {
    *session = NULL;
    io_service_t service = IO_OBJECT_NULL;
    int status;
    if (explicit_path) status = lookup(explicit_path, &service, err);
    else {
        struct receivers *receivers = calloc(1, sizeof(*receivers));
        if (!receivers) return fail(err, UC_INTERNAL, ENOMEM, 0, "cannot allocate receiver list");
        size_t index = 0;
        status = enumerate(receivers, err);
        if (!status) status = receiver_choose(receivers, &index, err);
        if (!status) status = lookup(receivers->entries[index].path, &service, err);
        else {
            for (size_t i = 0; i < receivers->count; i++) fprintf(diagnostics, "  %s  Logitech Unifying 046d:%04x (%s)\n", receivers->entries[i].path, (unsigned)receivers->entries[i].product, receivers->entries[i].physical);
        }
        free(receivers);
    }
    if (status) return status;
    status = open_service(service, session, io, err);
    IOObjectRelease(service);
    return status;
}

void receiver_disconnect(struct receiver_session *session) {
    if (!session) return;
    iokit_close(&session->transport);
    if (session->lock_fd >= 0) close(session->lock_fd);
    free(session);
}

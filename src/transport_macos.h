#ifndef UNIFYCTL_TRANSPORT_MACOS_H
#define UNIFYCTL_TRANSPORT_MACOS_H

#include "report_queue.h"
#include "transport.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDDevice.h>

/* IOKit HID report transport. Input reports are delivered by IOKit callbacks
 * only while receive() runs a private run-loop mode on the calling thread,
 * so the single-reader model is preserved without worker threads. */
struct iokit_transport {
    IOHIDDeviceRef device;
    CFRunLoopRef run_loop;
    CFFileDescriptorRef wake;
    CFRunLoopSourceRef wake_source;
    uint8_t input[REPORT_QUEUE_BYTES];
    struct report_queue queue;
    IOReturn input_failure;
    bool removed;
    bool scheduled;
    bool signals;
    bool cleanup;
};

/* Adopts a device already opened with IOHIDDeviceOpen, even on failure;
 * the caller always invokes iokit_close. */
int iokit_init(struct iokit_transport *t, IOHIDDeviceRef device, struct transport *io, struct error *err);
void iokit_close(struct iokit_transport *t);

#endif

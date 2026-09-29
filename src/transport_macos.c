#include "transport_macos.h"
#include "signals.h"

#include <errno.h>
#include <string.h>

/* A private mode ensures only this session's sources run while waiting. */
#define RUN_LOOP_MODE CFSTR("unifyctl.receiver")

static void on_input(void *context, IOReturn result, void *sender, IOHIDReportType type,
                     uint32_t report_id, uint8_t *report, CFIndex length) {
    struct iokit_transport *t = context;
    (void)sender;
    (void)type;
    (void)report_id;
    if (result != kIOReturnSuccess) t->input_failure = result;
    else if (length <= 0) t->queue.lost = true;
    else report_queue_push(&t->queue, report, (size_t)length);
}

static void on_removed(void *context, IOReturn result, void *sender) {
    (void)result;
    (void)sender;
    ((struct iokit_transport *)context)->removed = true;
}

/* Wakes CFRunLoopRunInMode after a signal; state lives in signals.c. */
static void on_wake(CFFileDescriptorRef descriptor, CFOptionFlags types, void *info) {
    (void)types;
    (void)info;
    signals_drain();
    CFFileDescriptorEnableCallBacks(descriptor, kCFFileDescriptorReadCallBack);
}

static int64_t iokit_now(void *context) {
    (void)context;
    return monotonic_ms();
}

static int iokit_cancelled(void *context) {
    struct iokit_transport *t = context;
    if (t->cleanup) return 0;
    return signals_pending();
}

static void iokit_set_cleanup(void *context, bool cleanup) {
    ((struct iokit_transport *)context)->cleanup = cleanup;
}

static bool detached(IOReturn result) {
    return result == kIOReturnNoDevice || result == kIOReturnNotAttached ||
           result == kIOReturnNotOpen || result == kIOReturnOffline;
}

static int iokit_send(void *context, const uint8_t *bytes, size_t size, int64_t deadline, struct error *err) {
    struct iokit_transport *t = context;
    int cancelled = iokit_cancelled(t);
    if (cancelled) return fail(err, (enum status)cancelled, 0, 0, "operation cancelled");
    if (t->removed) return fail(err, UC_IO, 0, 0, "receiver disconnected");
    if (deadline - monotonic_ms() <= 0) return fail(err, UC_TIMEOUT, 0, 0, "receiver I/O timed out");
    if (!size || size > REPORT_QUEUE_BYTES) return fail(err, UC_INTERNAL, 0, 0, "invalid output report size");
    /* Numbered reports: the ID is passed separately and remains byte 0 of the
     * buffer, matching the hidraw write layout. IOKit applies its own transfer
     * timeout to this synchronous call; it is never retried here. */
    IOReturn result = IOHIDDeviceSetReport(t->device, kIOHIDReportTypeOutput, (CFIndex)bytes[0], bytes, (CFIndex)size);
    if (result == kIOReturnSuccess) return UC_OK;
    if (detached(result)) {
        t->removed = true;
        return fail(err, UC_IO, 0, 0, "receiver disconnected");
    }
    if (result == kIOReturnTimeout) return fail(err, UC_TIMEOUT, 0, 0, "HID report write timed out; outcome uncertain");
    return fail(err, UC_IO, 0, 0, "cannot write HID report (IOReturn 0x%08x); outcome uncertain", (unsigned)result);
}

static int iokit_receive(void *context, uint8_t *bytes, size_t *size, int64_t deadline, struct error *err) {
    struct iokit_transport *t = context;
    for (;;) {
        int cancelled = iokit_cancelled(t);
        if (cancelled) return fail(err, (enum status)cancelled, 0, 0, "operation cancelled");
        if (t->removed) return fail(err, UC_IO, 0, 0, "receiver disconnected");
        if (t->input_failure != kIOReturnSuccess) {
            if (detached(t->input_failure) || t->input_failure == kIOReturnAborted) return fail(err, UC_IO, 0, 0, "receiver disconnected");
            return fail(err, UC_IO, 0, 0, "HID input failed (IOReturn 0x%08x)", (unsigned)t->input_failure);
        }
        if (t->queue.lost) return fail(err, UC_IO, 0, 0, "HID input reports were lost; outcome uncertain");
        if (!report_queue_empty(&t->queue)) return report_queue_pop(&t->queue, bytes, size, err);
        int64_t remaining = deadline - monotonic_ms();
        if (remaining <= 0) return fail(err, UC_TIMEOUT, 0, 0, "receiver I/O timed out");
        SInt32 result = CFRunLoopRunInMode(RUN_LOOP_MODE, (CFTimeInterval)remaining / 1000.0, true);
        if (result == kCFRunLoopRunFinished) return fail(err, UC_IO, 0, 0, "receiver event source unavailable");
    }
}

int iokit_init(struct iokit_transport *t, IOHIDDeviceRef device, struct transport *io, struct error *err) {
    memset(t, 0, sizeof(*t));
    t->device = device;
    t->run_loop = CFRunLoopGetCurrent();
    int status = signals_install(err);
    if (status) return status;
    t->signals = true;
    CFFileDescriptorContext context = {0, t, NULL, NULL, NULL};
    t->wake = CFFileDescriptorCreate(kCFAllocatorDefault, signals_wake_fd(), false, on_wake, &context);
    if (!t->wake) return fail(err, UC_INTERNAL, ENOMEM, 0, "cannot monitor signal pipe");
    t->wake_source = CFFileDescriptorCreateRunLoopSource(kCFAllocatorDefault, t->wake, 0);
    if (!t->wake_source) return fail(err, UC_INTERNAL, ENOMEM, 0, "cannot create signal run-loop source");
    CFFileDescriptorEnableCallBacks(t->wake, kCFFileDescriptorReadCallBack);
    CFRunLoopAddSource(t->run_loop, t->wake_source, RUN_LOOP_MODE);
    IOHIDDeviceRegisterInputReportCallback(device, t->input, (CFIndex)sizeof(t->input), on_input, t);
    IOHIDDeviceRegisterRemovalCallback(device, on_removed, t);
    IOHIDDeviceScheduleWithRunLoop(device, t->run_loop, RUN_LOOP_MODE);
    t->scheduled = true;
    *io = (struct transport){t, iokit_send, iokit_receive, iokit_now, iokit_cancelled, iokit_set_cleanup};
    return UC_OK;
}

void iokit_close(struct iokit_transport *t) {
    if (t->device) {
        if (t->scheduled) {
            IOHIDDeviceUnscheduleFromRunLoop(t->device, t->run_loop, RUN_LOOP_MODE);
            IOHIDDeviceRegisterInputReportCallback(t->device, t->input, (CFIndex)sizeof(t->input), NULL, NULL);
            IOHIDDeviceRegisterRemovalCallback(t->device, NULL, NULL);
        }
        IOHIDDeviceClose(t->device, kIOHIDOptionsTypeNone);
        CFRelease(t->device);
    }
    if (t->wake_source) {
        CFRunLoopRemoveSource(t->run_loop, t->wake_source, RUN_LOOP_MODE);
        CFRelease(t->wake_source);
    }
    if (t->wake) {
        CFFileDescriptorInvalidate(t->wake);
        CFRelease(t->wake);
    }
    if (t->signals) signals_restore();
    t->device = NULL;
    t->wake = NULL;
    t->wake_source = NULL;
    t->scheduled = t->signals = false;
}

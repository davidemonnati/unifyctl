#include "hidpp.h"

#include <string.h>

const char *hidpp_error_name(uint8_t code) {
    static const char *const names[] = {
        "undefined error", "invalid command", "invalid address", "invalid value",
        "pairing failed", "receiver full", "already exists", "receiver busy",
        "unknown device", "resource unavailable", "request unavailable",
        "invalid parameter", "wrong PIN"
    };
    if (code < sizeof(names) / sizeof(names[0])) return names[code];
    return "unknown protocol error";
}

static void trace(struct hidpp *h, const char *direction, const uint8_t *p, size_t n) {
    if (!h->debug) return;
    fprintf(stderr, "%s", direction);
    for (size_t i = 0; i < n; i++) fprintf(stderr, " %02x", p[i]);
    fputc('\n', stderr);
}

static int receive(struct hidpp *h, struct report *r, int64_t deadline, struct error *err) {
    /* Read more than 20 bytes so oversized reports cannot be silently truncated. */
    uint8_t buffer[256];
    size_t size = sizeof(buffer);
    int status = h->io.receive(h->io.context, buffer, &size, deadline, err);
    if (status) return status;
    if (size > sizeof(buffer)) return fail(err, UC_PROTOCOL, 0, 0, "transport returned an oversized report");
    trace(h, "RX", buffer, size);
    r->length = 0;
    if (!size) return fail(err, UC_IO, 0, 0, "receiver disconnected");
    /* The management interface can also carry DJ reports. Ignore other IDs. */
    if (buffer[0] != 0x10 && buffer[0] != 0x11) return UC_OK;
    size_t expected = buffer[0] == 0x10 ? HIDPP_SHORT : HIDPP_LONG;
    if (size != expected) return fail(err, UC_PROTOCOL, 0, 0, "invalid HID++ report length: %zu", size);
    if (buffer[1] != 0xff && (buffer[1] < 1 || buffer[1] > SLOT_COUNT)) return fail(err, UC_PROTOCOL, 0, 0, "invalid HID++ device index: %u", buffer[1]);
    memcpy(r->bytes, buffer, size);
    r->length = size;
    return UC_OK;
}

static int dispatch(struct hidpp *h, const struct report *r, struct error *err) {
    if (!r->length || r->bytes[2] >= 0x80) return UC_OK;
    const uint8_t *p = r->bytes;
    if (p[2] == 0x40 || p[2] == 0x41 || p[2] == 0x4a) {
        if (r->length != HIDPP_SHORT) return fail(err, UC_PROTOCOL, 0, 0, "invalid notification length");
        if (p[2] == 0x4a && (p[1] != 0xff || p[3] > 1)) return fail(err, UC_PROTOCOL, 0, 0, "invalid receiver lock notification");
        if (p[2] != 0x4a && p[1] == 0xff) return fail(err, UC_PROTOCOL, 0, 0, "invalid device notification index");
        if (p[2] == 0x41 && p[3] != 0x04) return fail(err, UC_PROTOCOL, 0, 0, "unsupported connection notification protocol");
    }
    if (h->notification) h->notification(h->notification_context, r);
    return UC_OK;
}

int hidpp_pump(struct hidpp *h, int64_t deadline, struct error *err) {
    struct report r;
    int status = receive(h, &r, deadline, err);
    if (status) return status;
    return dispatch(h, &r, err);
}

int hidpp_request(struct hidpp *h, uint8_t op, uint8_t reg,
                  const uint8_t params[3], int selector,
                  struct report *reply, struct error *err) {
    if (h->pending || h->poisoned) return fail(err, UC_PROTOCOL, 0, 0, "HID++ session unavailable after an incomplete transaction");
    if (op != 0x80 && op != 0x81 && op != 0x83) return fail(err, UC_INTERNAL, 0, 0, "unsupported internal request");
    uint8_t request[HIDPP_SHORT] = {0x10, 0xff, op, reg, params[0], params[1], params[2]};
    int64_t deadline = h->io.now(h->io.context) + 2000;
    if (h->deadline && deadline > h->deadline) deadline = h->deadline;
    h->pending = true;
    trace(h, "TX", request, sizeof(request));
    int status = h->io.send(h->io.context, request, sizeof(request), deadline, err);
    while (!status) {
        if (h->io.now(h->io.context) >= deadline) {
            status = fail(err, UC_TIMEOUT, 0, 0, "HID++ reply timed out");
            break;
        }
        status = receive(h, reply, deadline, err);
        if (status) break;
        if (!reply->length) continue;
        const uint8_t *p = reply->bytes;
        if (p[1] == 0xff && p[2] == 0x8f && p[3] == op && p[4] == reg) {
            if (reply->length != HIDPP_SHORT) {
                status = fail(err, UC_PROTOCOL, 0, 0, "invalid error response length");
                break;
            }
            h->pending = false;
            return fail(err, UC_PROTOCOL, 0, p[5], "%s", hidpp_error_name(p[5]));
        }
        if (p[1] == 0xff && p[2] == op && p[3] == reg) {
            size_t expected = op == 0x83 ? HIDPP_LONG : HIDPP_SHORT;
            if (reply->length != expected) {
                status = fail(err, UC_PROTOCOL, 0, 0, "invalid reply length for operation 0x%02x", op);
                break;
            }
            if (selector >= 0 && p[4] != (uint8_t)selector) continue;
            h->pending = false;
            return UC_OK;
        }
        status = dispatch(h, reply, err);
    }
    h->pending = false;
    h->poisoned = true;
    return status;
}

int hidpp_cleanup_write(struct hidpp *h, uint8_t reg, const uint8_t params[3],
                        int64_t deadline, struct error *err) {
    uint8_t request[HIDPP_SHORT] = {0x10, 0xff, 0x80, reg, params[0], params[1], params[2]};
    trace(h, "TX cleanup", request, sizeof(request));
    return h->io.send(h->io.context, request, sizeof(request), deadline, err);
}

#include "hidpp.h"

#include <string.h>

/*
 * Return a readable name for a HID++ error code.
 *
 * Returns a static string for known HID++ 1.0 codes, or a fallback for unknown
 * codes. No allocation or receiver access is performed.
 */
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

/*
 * Log report bytes to stderr when debug logging is enabled.
 *
 * Prints the direction label and n hexadecimal bytes as one line. Leaves the
 * report and session unchanged.
 */
static void trace(struct hidpp *h, const char *direction, const uint8_t *p, size_t n) {
    if (!h->debug) return;
    fprintf(stderr, "%s", direction);
    for (size_t i = 0; i < n; i++) fprintf(stderr, " %02x", p[i]);
    fputc('\n', stderr);
}

/*
 * Read and validate one HID++ report, ignoring unrelated report IDs.
 *
 * Uses an oversized temporary buffer to detect invalid lengths. Non-HID++
 * reports succeed with r->length=0; malformed HID++ lengths or device indices
 * produce UC_PROTOCOL.
 */
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

/*
 * Validate notifications and forward them to the registered callback.
 *
 * Ignores empty reports and register replies. Checks known notification
 * layouts before invoking the optional callback synchronously with a borrowed
 * report pointer.
 */
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

/*
 * Receive and dispatch one report within the supplied deadline.
 *
 * The deadline is absolute monotonic milliseconds. Returns receive/validation
 * failures directly and dispatches notifications without issuing a request.
 */
int hidpp_pump(struct hidpp *h, int64_t deadline, struct error *err) {
    struct report r;
    int status = receive(h, &r, deadline, err);
    if (status) return status;
    return dispatch(h, &r, err);
}

/*
 * Match a register reply while dispatching notifications; poison incomplete
 * transactions.
 *
 * Allows one outstanding request and matches receiver, operation, register,
 * and optional selector. Each attempt is limited to two seconds and the
 * operation deadline; opted-in pairing reads may retry once, but writes never
 * do. Incomplete transactions make the session unusable for further normal
 * requests.
 */
int hidpp_request(struct hidpp *h, uint8_t op, uint8_t reg,
                  const uint8_t params[3], int selector,
                  struct report *reply, struct error *err) {
    if (h->pending || h->poisoned) return fail(err, UC_PROTOCOL, 0, 0, "HID++ session unavailable after an incomplete transaction");
    if (op != 0x80 && op != 0x81 && op != 0x83) return fail(err, UC_INTERNAL, 0, 0, "unsupported internal request");
    uint8_t request[HIDPP_SHORT] = {0x10, 0xff, op, reg, params[0], params[1], params[2]};
    unsigned attempts = h->retry_pairing_reads && op == 0x83 && reg == 0xb5 &&
                        selector >= 0 && selector == params[0] ? 2u : 1u;
    unsigned attempt = 0;
    int64_t deadline;
    int status;
    bool sent;
retry:
    deadline = h->io.now(h->io.context) + 2000;
    if (h->deadline && deadline > h->deadline) deadline = h->deadline;
    h->pending = true;
    trace(h, "TX", request, sizeof(request));
    status = h->io.send(h->io.context, request, sizeof(request), deadline, err);
    sent = !status;
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
            if (selector >= 0 && p[4] != (uint8_t)selector) {
                if (h->debug) fprintf(stderr, "Ignoring B5 selector 0x%02x; waiting for 0x%02x\n", p[4], (unsigned)selector);
                continue;
            }
            h->pending = false;
            return UC_OK;
        }
        status = dispatch(h, reply, err);
    }
    /* Keep the same logical request pending. A late matching reply still
     * answers this identical read; never clear a poisoned session externally.
     * Other selectors remain mismatches. Writes and send failures never retry. */
    if (sent && status == UC_TIMEOUT && ++attempt < attempts &&
        (!h->deadline || h->io.now(h->io.context) < h->deadline)) {
        if (h->debug) fprintf(stderr, "Retrying stored-record read for selector 0x%02x after timeout\n", (unsigned)selector);
        *err = (struct error){0};
        goto retry;
    }
    h->pending = false;
    h->poisoned = true;
    return status;
}

/*
 * Send a cleanup write without waiting for an ambiguous acknowledgement.
 *
 * Bypasses normal session matching and sends once within the absolute
 * deadline. Success confirms transport submission only, not receiver execution
 * or pairing-window closure.
 */
int hidpp_cleanup_write(struct hidpp *h, uint8_t reg, const uint8_t params[3],
                        int64_t deadline, struct error *err) {
    uint8_t request[HIDPP_SHORT] = {0x10, 0xff, 0x80, reg, params[0], params[1], params[2]};
    trace(h, "TX cleanup", request, sizeof(request));
    return h->io.send(h->io.context, request, sizeof(request), deadline, err);
}

/* Independent FAP transaction path. A feature index may look like a receiver
 * notification sub-ID; passing these packets to dispatch() would misdecode it.
 * Software ID 0xe distinguishes responses from broadcast notifications. */
int hidpp_feature_request(struct hidpp *h, unsigned slot, uint8_t feature,
                          unsigned function, const uint8_t params[3],
                          size_t payload_size, struct report *reply, struct error *err) {
    *reply = (struct report){0};
    h->peripheral_unreachable = false;
    if (h->pending || h->poisoned) return fail(err, UC_PROTOCOL, 0, 0, "HID++ session unavailable after an incomplete transaction");
    if (slot < 1 || slot > SLOT_COUNT || function > 15 || payload_size > 16) return fail(err, UC_INTERNAL, 0, 0, "invalid peripheral query");
    uint8_t request[] = {0x10, (uint8_t)slot, feature, (uint8_t)((function << 4) | 0x0e), params[0], params[1], params[2]};
    int64_t deadline = h->io.now(h->io.context) + 2000;
    if (h->deadline && deadline > h->deadline) deadline = h->deadline;
    int status = h->io.cancelled(h->io.context);
    if (status) return fail(err, (enum status)status, 0, 0, "peripheral query cancelled");
    if (h->io.now(h->io.context) >= deadline) return fail(err, UC_TIMEOUT, 0, 0, "information query deadline expired");
    h->pending = true;
    bool link_lost = false;
    trace(h, "TX", request, sizeof(request));
    status = h->io.send(h->io.context, request, sizeof(request), deadline, err);
    while (!status) {
        status = h->io.cancelled(h->io.context);
        if (status) {
            status = fail(err, (enum status)status, 0, 0, "peripheral query cancelled");
            break;
        }
        if (h->io.now(h->io.context) >= deadline) {
            status = fail(err, UC_TIMEOUT, 0, 0, "peripheral reply timed out");
            break;
        }
        status = receive(h, reply, deadline, err);
        if (status) break;
        if (!reply->length || reply->bytes[1] != slot) continue;
        const uint8_t *p = reply->bytes;
        /* This notification alone never completes a request. A subsequent
         * matching RAP resource error can be explained by explicit link loss;
         * a resource error on its own remains an operational failure. */
        if (reply->length == HIDPP_SHORT && p[2] == 0x41 && p[3] == 4) link_lost = (p[4] & 0x40) != 0;
        if ((p[2] == 0x8f || p[2] == 0xff) && p[3] == feature && p[4] == request[3]) {
            size_t expected = p[2] == 0x8f ? HIDPP_SHORT : HIDPP_LONG;
            if (reply->length != expected || !p[5]) {
                status = fail(err, UC_PROTOCOL, 0, 0, "malformed peripheral error reply");
                break;
            }
            h->pending = false;
            h->peripheral_unreachable = link_lost && p[2] == 0x8f && p[5] == 9;
            return fail(err, UC_PROTOCOL, 0, p[5], "peripheral slot %u rejected feature 0x%02x function %u", slot, feature, function);
        }
        if (p[2] != feature || p[3] != request[3]) continue;
        if (reply->length < 4 + payload_size) {
            status = fail(err, UC_PROTOCOL, 0, 0, "truncated peripheral reply payload");
            break;
        }
        h->pending = false;
        return UC_OK;
    }
    h->pending = false;
    h->poisoned = true;
    return status;
}

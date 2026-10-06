#include "operations.h"
#include "cli.h"

#include <string.h>

struct pairing_events {
    bool closed;
    uint8_t error;
    bool connected[SLOT_COUNT];
    uint16_t wpid[SLOT_COUNT];
};

enum { PAIRING_VERIFY_MS = 5000 };

/*
 * Track pairing-window state and device connection notifications.
 *
 * Context must point to pairing_events and reports must already be validated
 * by the dispatcher. Updates slot connectivity/WPID or the receiver's closure
 * result without issuing I/O.
 */
static void pairing_notification(void *context, const struct report *r) {
    struct pairing_events *events = context;
    const uint8_t *p = r->bytes;
    if (p[2] == 0x4a && p[3] == 0) {
        events->closed = true;
        events->error = p[4];
    } else if (p[2] == 0x41) {
        unsigned index = (unsigned)p[1] - 1;
        events->connected[index] = (p[4] & 0x40) == 0;
        events->wpid[index] = (uint16_t)((uint16_t)p[6] << 8 | p[5]);
    } else if (p[2] == 0x40 && p[3] == 2) events->connected[p[1] - 1] = false;
}

/*
 * Translate a pairing-window error code into an operation failure.
 *
 * Maps receiver timeout codes to UC_TIMEOUT and rejection codes to
 * UC_PROTOCOL. Unknown codes are preserved in the diagnostic text.
 */
static int pairing_failure(uint8_t code, struct error *err) {
    switch (code) {
    case 1: return fail(err, UC_TIMEOUT, 0, 0, "pairing window timed out");
    case 2: return fail(err, UC_PROTOCOL, 0, 0, "receiver rejected an unsupported device");
    case 3: return fail(err, UC_PROTOCOL, 0, 0, "receiver is full");
    case 6: return fail(err, UC_TIMEOUT, 0, 0, "device connection sequence timed out");
    default: return fail(err, UC_PROTOCOL, 0, 0, "receiver reported pairing error 0x%02x", code);
    }
}

/*
 * Write a receiver register and wait for its acknowledgement.
 *
 * Sends exactly three parameter bytes with operation 0x80. Returns the
 * transaction status; an acknowledgement alone does not verify a pairing-state
 * change.
 */
static int write_register(struct hidpp *h, uint8_t reg, const uint8_t params[3], struct error *err) {
    struct report reply;
    return hidpp_request(h, 0x80, reg, params, -1, &reply, err);
}

/*
 * Process notifications until the pause expires or an error occurs.
 *
 * The until argument is an absolute monotonic deadline. Treats expiry as a
 * successful pause and clears its timeout error, while propagating other
 * failures.
 */
static int pause_events(struct hidpp *h, int64_t until, struct error *err) {
    while (h->io.now(h->io.context) < until) {
        int status = hidpp_pump(h, until, err);
        if (status == UC_TIMEOUT) {
            *err = (struct error){0};
            return UC_OK;
        }
        if (status) return status;
    }
    return UC_OK;
}

/*
 * Find and validate a newly stored pairing within the verification deadline.
 *
 * Compares snapshots against before, rejecting changes to existing slots or
 * multiple additions. Fills added only after rereading the new device; narrows
 * the session deadline to at most five seconds and checks notifications where
 * required.
 */
static int verify_new(struct hidpp *h, const struct device before[SLOT_COUNT],
                      struct pairing_events *events, struct device *added, struct error *err) {
    int64_t deadline = h->io.now(h->io.context) + PAIRING_VERIFY_MS;
    if (h->deadline && deadline > h->deadline) deadline = h->deadline;
    h->deadline = deadline;
    while (h->io.now(h->io.context) < deadline) {
        struct device after[SLOT_COUNT];
        int status = devices_read(h, after, false, err);
        if (status) return status;
        if (events->closed && events->error) return pairing_failure(events->error, err);
        unsigned count = 0, slot = 0;
        for (unsigned i = 0; i < SLOT_COUNT; i++) {
            if (before[i].paired && (!after[i].paired || after[i].wpid != before[i].wpid || after[i].type != before[i].type)) return fail(err, UC_PROTOCOL, 0, 0, "existing pairing changed concurrently; outcome uncertain; run list");
            if (!before[i].paired && after[i].paired) {
                count++;
                slot = i + 1;
            }
        }
        if (count > 1) return fail(err, UC_PROTOCOL, 0, 0, "multiple new pairings observed; outcome uncertain; run list");
        if (count == 1) {
            unsigned i = slot - 1;
            if (!events->closed && (!events->connected[i] || events->wpid[i] != after[i].wpid)) return fail(err, UC_PROTOCOL, 0, 0, "new pairing does not match observed notification; run list");
            status = device_read(h, slot, added, true, err);
            if (status) return status;
            if (!added->paired || added->wpid != after[i].wpid) return fail(err, UC_PROTOCOL, 0, 0, "pairing changed during verification; run list");
            if (events->connected[i] && events->wpid[i] == added->wpid) added->link = LINK_CONNECTED;
            return UC_OK;
        }
        int64_t until = h->io.now(h->io.context) + 100;
        if (until > deadline) until = deadline;
        status = pause_events(h, until, err);
        if (status) return status;
    }
    return fail(err, UC_PROTOCOL, 0, 0, "no new stored pairing verified; outcome uncertain; run list");
}

/*
 * Attempt bounded receiver cleanup without using it to determine pairing
 * success.
 *
 * Suppresses cancellation during a two-second cleanup budget, then clears the
 * callback and operation deadline. For a poisoned session, sends an unverified
 * close only if closure is not already known and skips flag restoration;
 * reported cleanup status is separate from pairing success.
 */
static int cleanup_pairing(struct hidpp *h, bool opened, bool closed, bool flags_changed,
                           const uint8_t original[3], FILE *diagnostics) {
    struct error cleanup_error = {0};
    bool uncertain = h->poisoned;
    int status = UC_OK;
    h->notification = NULL;
    h->notification_context = NULL;
    h->io.set_cleanup(h->io.context, true);
    h->deadline = h->io.now(h->io.context) + 2000;
    if (uncertain) {
        if (opened && !closed) {
            const uint8_t close_window[3] = {2, 0, 0};
            status = hidpp_cleanup_write(h, 0xb2, close_window, h->deadline, &cleanup_error);
            fputs("unifyctl: pairing-window closure attempted but cannot be verified after an incomplete transaction\n", diagnostics);
        }
        if (flags_changed) fputs("unifyctl: notification flags could not be safely restored; reconnect receiver after checking pairing state\n", diagnostics);
    } else {
        if (opened) {
            const uint8_t close_window[3] = {2, 0, 0};
            status = write_register(h, 0xb2, close_window, &cleanup_error);
        }
        if (!status && flags_changed) status = write_register(h, 0x00, original, &cleanup_error);
    }
    if (status) {
        fputs("unifyctl: cleanup incomplete: ", diagnostics);
        print_error(diagnostics, &cleanup_error);
    }
    h->io.set_cleanup(h->io.context, false);
    h->deadline = 0;
    return status;
}

/*
 * Open a pairing window, verify a new device, and clean up receiver state.
 *
 * Requires a timeout of 1-255 seconds and a free slot. Temporarily enables
 * notifications, tracks pairing events, and verifies stored state before
 * printing success; cleanup failures still return an error after a device has
 * paired.
 */
int operation_add(struct hidpp *h, unsigned timeout, const struct operation_ui *ui, struct error *err) {
    if (timeout < 1 || timeout > 255) return fail(err, UC_USAGE, 0, 0, "pairing timeout must be 1–255 seconds");
    struct device before[SLOT_COUNT], added = {0};
    int status = devices_read(h, before, false, err);
    if (status) return status;
    unsigned occupied = 0;
    for (unsigned i = 0; i < SLOT_COUNT; i++) occupied += before[i].paired ? 1u : 0u;
    if (occupied == SLOT_COUNT) return fail(err, UC_PROTOCOL, 0, 0, "receiver is full (six stored pairings)");

    struct report reply;
    const uint8_t zero[3] = {0};
    status = hidpp_request(h, 0x81, 0x00, zero, -1, &reply, err);
    if (status) return status;
    uint8_t original[3], enabled[3];
    memcpy(original, reply.bytes + 4, sizeof(original));
    memcpy(enabled, original, sizeof(enabled));
    /* Solaar enables both wireless reports and software presence while it
     * manages a receiver. Preserve the original flags and restore them. */
    enabled[1] |= 0x09;
    bool flags_changed = memcmp(enabled, original, sizeof(enabled)) != 0;
    bool opened = false;
    if (flags_changed) status = write_register(h, 0x00, enabled, err);

    struct pairing_events events = {0};
    bool verification_started = false;
    if (!status) {
        h->notification = pairing_notification;
        h->notification_context = &events;
        fprintf(ui->diagnostics, "Pairing for %u seconds. Put a Unifying-compatible peripheral into pairing mode\n"
                "(usually power it off/on; for multi-host devices choose a channel and hold its pairing button).\n"
                "Press Ctrl-C to cancel.\n", timeout);
        int64_t window_deadline = h->io.now(h->io.context) + (int64_t)timeout * 1000;
        h->deadline = window_deadline;
        uint8_t open_window[3] = {1, 0, (uint8_t)timeout};
        opened = true; /* Even a failed acknowledgement can follow a successful write. */
        status = write_register(h, 0xb2, open_window, err);
        h->deadline = window_deadline + PAIRING_VERIFY_MS;
        while (!status) {
            if (events.closed && events.error) {
                status = pairing_failure(events.error, err);
                break;
            }
            bool candidate = false;
            for (unsigned i = 0; i < SLOT_COUNT; i++) {
                if (!before[i].paired && events.connected[i]) candidate = true;
            }
            if (candidate || events.closed) {
                verification_started = true;
                bool previous_retry = h->retry_pairing_reads;
                h->retry_pairing_reads = true;
                status = verify_new(h, before, &events, &added, err);
                h->retry_pairing_reads = previous_retry;
                break;
            }
            if (h->io.now(h->io.context) >= window_deadline) {
                status = fail(err, UC_TIMEOUT, 0, 0, "pairing window timed out without a verified new device");
                break;
            }
            status = hidpp_pump(h, window_deadline, err);
            if (status == UC_TIMEOUT) fail(err, UC_TIMEOUT, 0, 0, "pairing window timed out without a verified new device");
        }
    }
    bool reported_success = events.closed && !events.error;
    if (status && verification_started && reported_success) fputs("unifyctl: receiver reported successful pairing, but stored-device verification failed; run list before pairing again\n", ui->diagnostics);
    int cleanup = cleanup_pairing(h, opened, events.closed, flags_changed, original, ui->diagnostics);
    if (!status && added.paired) {
        fputs("New stored pairing:\n", ui->output);
        device_print(ui->output, &added);
        if (cleanup) status = fail(err, UC_PROTOCOL, 0, 0, "device paired, but receiver cleanup was incomplete");
    }
    if (status && opened && !reported_success && (status == UC_IO || h->poisoned)) fputs("unifyctl: pairing outcome may be uncertain; run list before trying again\n", ui->diagnostics);
    return status;
}

/* The caller has already obtained confirmation (or ui->yes authorization) for
 * before. Recheck its identity, send one unpair request, and verify absence with
 * a 2-second polling budget after acknowledgement. Never retry the write. Report
 * success only after verification; failed writes/verification may have changed
 * receiver state. Shared by single-slot and bulk removal; does not prompt. */
static int remove_confirmed(struct hidpp *h, const struct device *before,
                            const struct operation_ui *ui, struct error *err) {
    unsigned slot = before->slot;
    struct device current;
    int cancelled = h->io.cancelled(h->io.context);
    if (cancelled) return fail(err, (enum status)cancelled, 0, 0, "removal cancelled");
    int status = device_read(h, slot, &current, true, err);
    if (status) return status;
    if (!device_same(before, &current)) return fail(err, UC_PROTOCOL, 0, 0, "slot changed during confirmation; removal aborted");
    uint8_t unpair[3] = {3, (uint8_t)slot, 0};
    status = write_register(h, 0xb2, unpair, err);
    if (!status) {
        h->deadline = h->io.now(h->io.context) + 2000;
        while (!status && h->io.now(h->io.context) < h->deadline) {
            status = device_read(h, slot, &current, false, err);
            if (status || !current.paired) break;
            int64_t until = h->io.now(h->io.context) + 100;
            if (until > h->deadline) until = h->deadline;
            status = pause_events(h, until, err);
        }
        h->deadline = 0;
        if (!status && current.paired) status = fail(err, UC_PROTOCOL, 0, 0, "slot remains paired after removal request");
    }
    if (status) {
        fputs("unifyctl: removal not verified; outcome may be uncertain; run list before retrying\n", ui->diagnostics);
        return status;
    }
    fprintf(ui->output, "Removed stored pairing from slot %u.\n", slot);
    return UC_OK;
}

int operation_remove(struct hidpp *h, unsigned slot, const struct operation_ui *ui, struct error *err) {
    struct device before;
    int status = device_read(h, slot, &before, true, err);
    if (status) return status;
    if (!before.paired) return fail(err, UC_PROTOCOL, 0, 0, "slot %u has no stored pairing", slot);
    fputs("Removing this pairing disconnects the device from this receiver:\n", ui->diagnostics);
    device_print(ui->diagnostics, &before);
    if (!ui->yes) status = cli_confirm(ui->input, ui->diagnostics, ui->interactive, false, err);
    int cancelled = h->io.cancelled(h->io.context);
    if (cancelled) return fail(err, (enum status)cancelled, 0, 0, "removal cancelled");
    if (status) return status;
    return remove_confirmed(h, &before, ui, err);
}

int operation_remove_all(struct hidpp *h, const struct operation_ui *ui, struct error *err) {
    struct device before[SLOT_COUNT];
    int status = devices_read(h, before, true, err);
    if (status) return status;
    unsigned count = 0, removed = 0;
    for (unsigned i = 0; i < SLOT_COUNT; i++) count += before[i].paired ? 1u : 0u;
    if (!count) {
        fputs("No paired devices.\n", ui->output);
        return UC_OK;
    }
    fputs("Removing all listed pairings disconnects these devices from this receiver:\n", ui->diagnostics);
    devices_print(ui->diagnostics, before);
    if (!ui->yes) status = cli_confirm(ui->input, ui->diagnostics, ui->interactive, true, err);
    int cancelled = h->io.cancelled(h->io.context);
    if (cancelled) return fail(err, (enum status)cancelled, 0, 0, "removal cancelled");
    if (status) return status;
    for (unsigned i = 0; i < SLOT_COUNT; i++) {
        if (!before[i].paired) continue;
        status = remove_confirmed(h, &before[i], ui, err);
        if (status) {
            fprintf(ui->diagnostics, "unifyctl: bulk removal stopped at slot %u; %u of %u pairings verified removed; run list before retrying\n",
                    before[i].slot, removed, count);
            return status;
        }
        removed++;
    }
    return UC_OK;
}

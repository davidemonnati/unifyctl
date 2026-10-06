#include "devices.h"

#include <inttypes.h>
#include <string.h>

/*
 * Read a stored receiver record matching the requested selector.
 *
 * Uses the long-read operation on register 0xb5. Returns the HID++ transaction
 * status; the reply is valid for the requested selector only on success.
 */
static int info(struct hidpp *h, uint8_t selector, struct report *r, struct error *err) {
    uint8_t params[3] = {selector, 0, 0};
    return hidpp_request(h, 0x83, 0xb5, params, selector, r, err);
}

/*
 * INVALID_VALUE on a valid B5 slot selector is the observed missing-record
 * convention (see docs/protocol.md). INVALID_ADDRESS is NOT an empty slot: it
 * can mean an unsupported register. UNKNOWN_DEVICE explicitly means absent.
 *
 * Accepts INVALID_VALUE and UNKNOWN_DEVICE only when the status is
 * UC_PROTOCOL. Other failures must not be interpreted as an empty slot.
 */
static bool missing(const struct error *err) {
    return err->status == UC_PROTOCOL && (err->protocol_code == 0x03 || err->protocol_code == 0x08);
}

/*
 * Recognize absent or unsupported optional metadata records.
 *
 * Also accepts INVALID_ADDRESS for optional records. Transport failures remain
 * fatal rather than being hidden as missing metadata.
 */
static bool unavailable_metadata(const struct error *err) {
    return missing(err) || (err->status == UC_PROTOCOL && err->protocol_code == 0x02);
}

/*
 * Return a display name for the device type, or unknown.
 *
 * Maps known receiver device-type codes to static strings. The caller must not
 * free the returned pointer.
 */
static const char *type_name(uint8_t type) {
    switch (type) {
    case 1: return "keyboard";
    case 2: return "mouse";
    case 3: return "numpad";
    case 4: return "presenter";
    case 8: return "trackball";
    case 9: return "touchpad";
    default: return "unknown";
    }
}

/*
 * Read one pairing slot and optionally its serial number and name.
 *
 * Requires a slot from 1 through 6 and initializes d before reading. An empty
 * slot succeeds with paired=false; optional missing metadata stays unknown,
 * while transport and malformed-data errors propagate.
 */
int device_read(struct hidpp *h, unsigned slot, struct device *d, bool metadata, struct error *err) {
    if (slot < 1 || slot > SLOT_COUNT) return fail(err, UC_USAGE, 0, 0, "slot must be 1–6");
    *d = (struct device){.slot = slot};
    struct report r;
    int status = info(h, (uint8_t)(0x20 + slot - 1), &r, err);
    if (status) {
        if (!missing(err)) return status;
        *err = (struct error){0};
        return UC_OK;
    }
    d->paired = true;
    d->wpid = (uint16_t)((uint16_t)r.bytes[7] << 8 | r.bytes[8]);
    d->type = r.bytes[11];
    if (!metadata) return UC_OK;
    status = info(h, (uint8_t)(0x30 + slot - 1), &r, err);
    if (!status) {
        d->serial = (uint32_t)r.bytes[5] << 24 | (uint32_t)r.bytes[6] << 16 |
                    (uint32_t)r.bytes[7] << 8 | r.bytes[8];
        d->has_serial = true;
    } else if (!unavailable_metadata(err)) return status;
    *err = (struct error){0};
    status = info(h, (uint8_t)(0x40 + slot - 1), &r, err);
    if (!status) {
        size_t length = r.bytes[5];
        if (length > 14) return fail(err, UC_PROTOCOL, 0, 0, "invalid name length for slot %u", slot);
        /* Strip ASCII terminal controls; preserve UTF-8 bytes from firmware. */
        for (size_t i = 0; i < length; i++) {
            uint8_t c = r.bytes[6 + i];
            d->name[i] = c < 0x20 || c == 0x7f ? '?' : (char)c;
        }
        d->name[length] = '\0';
    } else if (!unavailable_metadata(err)) return status;
    *err = (struct error){0};
    return UC_OK;
}

/*
 * Read all pairing slots, stopping at the first error.
 *
 * Fills the array in slot order and optionally reads metadata. On failure,
 * earlier entries remain populated and later entries are not guaranteed to be
 * initialized.
 */
int devices_read(struct hidpp *h, struct device devices[SLOT_COUNT], bool metadata, struct error *err) {
    for (unsigned slot = 1; slot <= SLOT_COUNT; slot++) {
        int status = device_read(h, slot, &devices[slot - 1], metadata, err);
        if (status) return status;
    }
    return UC_OK;
}

/*
 * Print one device row, marking unavailable fields as unknown.
 *
 * Writes a row to out using the stored identity and observed link state. Does
 * not query the receiver or filter out unpaired entries.
 */
void device_print(FILE *out, const struct device *d) {
    fprintf(out, "%u  %-14s %-9s %04x ", d->slot,
            d->name[0] ? d->name : "unknown", type_name(d->type), (unsigned)d->wpid);
    if (d->has_serial) fprintf(out, "%08" PRIx32, d->serial);
    else fputs("unknown ", out);
    const char *link = d->link == LINK_CONNECTED ? "connected" :
                       d->link == LINK_DISCONNECTED ? "disconnected" : "unknown";
    fprintf(out, " %s\n", link);
}

/*
 * Print stored pairings with a heading, or an empty-list message.
 *
 * Skips unpaired slots and prints the heading once. Uses only the supplied
 * snapshot and performs no receiver I/O.
 */
void devices_print(FILE *out, const struct device devices[SLOT_COUNT]) {
    unsigned count = 0;
    for (unsigned i = 0; i < SLOT_COUNT; i++) {
        if (!devices[i].paired) continue;
        if (!count) fputs("SLOT NAME           TYPE      WPID SERIAL   STATUS\n", out);
        device_print(out, &devices[i]);
        count++;
    }
    if (!count) fputs("No paired devices.\n", out);
}

/*
 * Compare stored device identities, excluding transient link state.
 *
 * Compares slot, occupancy, WPID, type, serial availability/value, and name.
 * Link state is intentionally ignored when rechecking removal identity.
 */
bool device_same(const struct device *a, const struct device *b) {
    return a->slot == b->slot && a->paired == b->paired && a->wpid == b->wpid &&
           a->type == b->type && a->has_serial == b->has_serial &&
           (!a->has_serial || a->serial == b->serial) && !strcmp(a->name, b->name);
}

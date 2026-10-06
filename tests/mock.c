#include "mock.h"

#include <assert.h>
#include <string.h>

/*
 * Record a mock write unless an injected error or deadline prevents it.
 *
 * Checks injected send failures, cancellation, and the absolute fake deadline
 * before copying bytes. Successful writes append to sent; fixture capacity
 * violations assert.
 */
static int send_report(void *context, const uint8_t *bytes, size_t length, int64_t deadline, struct error *err) {
    struct mock *m = context;
    if (m->send_error) return fail(err, (enum status)m->send_error, 0, 0, "mock send failure");
    if (m->cancel && !m->cleanup) return fail(err, (enum status)m->cancel, 0, 0, "mock cancellation");
    if (m->now >= deadline) return fail(err, UC_TIMEOUT, 0, 0, "mock write timeout");
    assert(m->sends < MOCK_MAX && length <= HIDPP_LONG);
    struct report *r = &m->sent[m->sends++];
    memcpy(r->bytes, bytes, length);
    r->length = length;
    return UC_OK;
}

/*
 * Deliver the next mock event, advancing the simulated clock.
 *
 * Consumes events in insertion order and advances time to their scheduled
 * timestamp. No available event before the deadline produces UC_TIMEOUT;
 * injected signal events also latch cancellation.
 */
static int receive_report(void *context, uint8_t *bytes, size_t *length, int64_t deadline, struct error *err) {
    struct mock *m = context;
    if (m->cancel && !m->cleanup) return fail(err, (enum status)m->cancel, 0, 0, "mock cancellation");
    if (m->position == m->count || m->events[m->position].at >= deadline) {
        m->now = deadline;
        return fail(err, UC_TIMEOUT, 0, 0, "mock read timeout");
    }
    struct mock_event *event = &m->events[m->position++];
    if (event->at > m->now) m->now = event->at;
    if (event->status) {
        if (event->status == UC_INTERRUPT || event->status == UC_TERMINATE) m->cancel = event->status;
        return fail(err, (enum status)event->status, 0, 0, "mock read failure");
    }
    assert(*length >= event->report.length);
    *length = event->report.length;
    memcpy(bytes, event->report.bytes, *length);
    return UC_OK;
}

/*
 * Return the simulated transport time.
 *
 * Reads the mock clock without advancing it. Tests schedule receive events to
 * control deadline behavior deterministically.
 */
static int64_t now(void *context) {
    return ((struct mock *)context)->now;
}

/*
 * Return injected cancellation unless mock cleanup mode is active.
 *
 * Returns the mock's latched cancellation status without consuming it. Cleanup
 * mode temporarily masks that status.
 */
static int cancelled(void *context) {
    struct mock *m = context;
    return m->cleanup ? 0 : m->cancel;
}

/*
 * Toggle cancellation suppression in the mock transport.
 *
 * Changes the cleanup flag only; it does not clear injected cancellation or
 * queued events.
 */
static void set_cleanup(void *context, bool cleanup) {
    ((struct mock *)context)->cleanup = cleanup;
}

/*
 * Create a HID++ session backed by the supplied mock state.
 *
 * The returned session borrows mock as its transport context. The caller must
 * initialize mock and keep it alive throughout session use.
 */
struct hidpp mock_session(struct mock *mock) {
    return (struct hidpp){.io = {mock, send_report, receive_report, now, cancelled, set_cleanup}};
}

/*
 * Append a report to the mock input sequence.
 *
 * Copies bytes into the next event slot and asserts fixture bounds. Timing and
 * injected status use the caller-initialized event fields.
 */
void mock_report(struct mock *mock, const uint8_t *bytes, size_t length) {
    assert(mock->count < MOCK_MAX && length <= HIDPP_LONG);
    struct mock_event *event = &mock->events[mock->count++];
    event->report.length = length;
    memcpy(event->report.bytes, bytes, length);
}

/*
 * Queue a receiver error reply for the specified request.
 *
 * Encodes a short HID++ error with the supplied operation, register, and
 * protocol code. Appends it without advancing the mock clock.
 */
void mock_error(struct mock *mock, uint8_t op, uint8_t reg, uint8_t code) {
    uint8_t bytes[7] = {0x10, 0xff, 0x8f, op, reg, code, 0};
    mock_report(mock, bytes, sizeof(bytes));
}

/*
 * Queue a successful register-write acknowledgement.
 *
 * Encodes a short successful 0x80 reply for reg. Appends the reply without
 * validating a previously sent request.
 */
void mock_ack(struct mock *mock, uint8_t reg) {
    uint8_t bytes[7] = {0x10, 0xff, 0x80, reg, 0, 0, 0};
    mock_report(mock, bytes, sizeof(bytes));
}

/*
 * Synthetic bytes independently laid out from Logitech HID++ 1.0 sections
 * 4.5.1–4.5.3, not captured traffic or copied implementation fixtures.
 *
 * Queues either a missing-record error or a synthetic occupied-slot record.
 * With metadata enabled, appends matching serial and name records for the one-
 * based slot.
 */
void mock_slot(struct mock *mock, unsigned slot, bool paired, bool metadata) {
    if (!paired) {
        mock_error(mock, 0x83, 0xb5, 0x03);
        return;
    }
    uint8_t pairing[20] = {0x11, 0xff, 0x83, 0xb5, 0, 0, 8, 0x40, 0x01, 0, 0, 2};
    pairing[4] = (uint8_t)(0x20 + slot - 1);
    mock_report(mock, pairing, sizeof(pairing));
    if (!metadata) return;
    uint8_t serial[20] = {0x11, 0xff, 0x83, 0xb5, 0, 0x12, 0x34, 0x56, 0x78};
    serial[4] = (uint8_t)(0x30 + slot - 1);
    mock_report(mock, serial, sizeof(serial));
    uint8_t name[20] = {0x11, 0xff, 0x83, 0xb5, 0, 5, 'M', 'o', 'u', 's', 'e'};
    name[4] = (uint8_t)(0x40 + slot - 1);
    mock_report(mock, name, sizeof(name));
}

/*
 * Queue slot records according to the supplied occupancy mask.
 *
 * Bit zero represents slot 1, through bit five for slot 6. Appends records in
 * slot order and optionally includes metadata for occupied slots.
 */
void mock_snapshot(struct mock *mock, unsigned occupied_mask, bool metadata) {
    for (unsigned slot = 1; slot <= SLOT_COUNT; slot++) mock_slot(mock, slot, (occupied_mask & (1u << (slot - 1))) != 0, metadata);
}

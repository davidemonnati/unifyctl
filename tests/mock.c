#include "mock.h"

#include <assert.h>
#include <string.h>

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

static int64_t now(void *context) {
    return ((struct mock *)context)->now;
}

static int cancelled(void *context) {
    struct mock *m = context;
    return m->cleanup ? 0 : m->cancel;
}

static void set_cleanup(void *context, bool cleanup) {
    ((struct mock *)context)->cleanup = cleanup;
}

struct hidpp mock_session(struct mock *mock) {
    return (struct hidpp){.io = {mock, send_report, receive_report, now, cancelled, set_cleanup}};
}

void mock_report(struct mock *mock, const uint8_t *bytes, size_t length) {
    assert(mock->count < MOCK_MAX && length <= HIDPP_LONG);
    struct mock_event *event = &mock->events[mock->count++];
    event->report.length = length;
    memcpy(event->report.bytes, bytes, length);
}

void mock_error(struct mock *mock, uint8_t op, uint8_t reg, uint8_t code) {
    uint8_t bytes[7] = {0x10, 0xff, 0x8f, op, reg, code, 0};
    mock_report(mock, bytes, sizeof(bytes));
}

void mock_ack(struct mock *mock, uint8_t reg) {
    uint8_t bytes[7] = {0x10, 0xff, 0x80, reg, 0, 0, 0};
    mock_report(mock, bytes, sizeof(bytes));
}

/* Synthetic bytes independently laid out from Logitech HID++ 1.0 sections
 * 4.5.1–4.5.3, not captured traffic or copied implementation fixtures. */
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

void mock_snapshot(struct mock *mock, unsigned occupied_mask, bool metadata) {
    for (unsigned slot = 1; slot <= SLOT_COUNT; slot++) mock_slot(mock, slot, (occupied_mask & (1u << (slot - 1))) != 0, metadata);
}

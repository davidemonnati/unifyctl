#include "cli.h"
#include "devices.h"
#include "discovery.h"
#include "mock.h"
#include "report_queue.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

/*
 * Verify slot decoding, optional metadata, and listing output.
 *
 * Covers empty and occupied records, serial/name decoding, unknown link
 * state, and display behavior using synthetic replies.
 */
static void test_listing(void) {
    unsigned masks[] = {0, 1, 0x21, 0x15, 0x3f};
    for (size_t i = 0; i < sizeof(masks) / sizeof(masks[0]); i++) {
        struct mock m = {0};
        mock_snapshot(&m, masks[i], true);
        struct hidpp h = mock_session(&m);
        struct device devices[SLOT_COUNT];
        struct error err = {0};
        assert(devices_read(&h, devices, true, &err) == UC_OK);
        for (unsigned j = 0; j < SLOT_COUNT; j++) {
            assert(devices[j].paired == ((masks[i] & (1u << j)) != 0));
            assert(devices[j].slot == j + 1);
            assert(devices[j].link == LINK_UNKNOWN);
            if (!devices[j].paired) continue;
            assert(devices[j].wpid == 0x4001 && devices[j].type == 2);
            assert(devices[j].serial == 0x12345678 && devices[j].has_serial);
            assert(!strcmp(devices[j].name, "Mouse"));
        }
        for (size_t j = 0; j < m.sends; j++) {
            assert(m.sent[j].length == 7);
            assert(m.sent[j].bytes[0] == 0x10 && m.sent[j].bytes[1] == 0xff);
            assert(m.sent[j].bytes[2] == 0x83 && m.sent[j].bytes[3] == 0xb5);
            assert(m.sent[j].bytes[5] == 0 && m.sent[j].bytes[6] == 0);
        }
        assert(m.position == m.count);
        FILE *out = tmpfile();
        assert(out);
        devices_print(out, devices);
        rewind(out);
        char text[1024] = {0};
        assert(fread(text, 1, sizeof(text) - 1, out) > 0);
        assert(strstr(text, masks[i] ? "STATUS" : "No paired devices."));
        fclose(out);
    }
}

/*
 * Count notifications delivered to the test callback.
 *
 * Asserts that the report is a connection notification, then increments the
 * caller-owned counter. Checks that request matching preserves notifications.
 */
static void count_notification(void *context, const struct report *report) {
    assert(report->bytes[2] == 0x41);
    (*(unsigned *)context)++;
}

/*
 * Verify reply matching while unrelated notifications are dispatched.
 *
 * Injects unrelated traffic around a matching reply and checks callback
 * delivery. Ensures requests accept only the expected receiver record.
 */
static void test_matching(void) {
    struct mock m = {0};
    uint8_t notification[] = {0x10, 1, 0x41, 4, 2, 1, 0x40};
    mock_report(&m, notification, sizeof(notification));
    mock_error(&m, 0x81, 0xb5, 2); /* Wrong operation. */
    mock_error(&m, 0x83, 0x00, 2); /* Wrong register. */
    mock_slot(&m, 2, true, false); /* Wrong echoed slot. */
    mock_slot(&m, 1, true, false);
    struct hidpp h = mock_session(&m);
    unsigned notifications = 0;
    h.notification = count_notification;
    h.notification_context = &notifications;
    struct device d;
    struct error err = {0};
    assert(device_read(&h, 1, &d, false, &err) == UC_OK);
    assert(notifications == 1 && m.position == m.count && m.sends == 1);
    assert(d.paired && d.slot == 1);
}

/*
 * Verify malformed reports, protocol errors, and incomplete transactions.
 *
 * Exercises invalid report shapes and transport/protocol failure paths. Checks
 * that incomplete transactions prevent unsafe reuse of the session.
 */
static void test_failures(void) {
    for (size_t n = 0; n <= 20; n++) {
        if (n == 7) continue;
        struct mock m = {0};
        uint8_t bytes[20] = {0x10, 0xff, 0x83, 0xb5, 0x20};
        mock_report(&m, bytes, n);
        struct hidpp h = mock_session(&m);
        struct device d;
        struct error err = {0};
        assert(device_read(&h, 1, &d, false, &err) != UC_OK);
        assert(h.poisoned);
    }
    uint8_t codes[] = {1, 2, 4, 5, 6, 7, 9, 10, 11, 12, 255};
    for (size_t i = 0; i < sizeof(codes); i++) {
        struct mock m = {0};
        mock_error(&m, 0x83, 0xb5, codes[i]);
        struct hidpp h = mock_session(&m);
        struct device d;
        struct error err = {0};
        assert(device_read(&h, 1, &d, false, &err) == UC_PROTOCOL);
        assert(err.protocol_code == codes[i] && !h.poisoned);
    }
    struct mock m = {0};
    struct hidpp h = mock_session(&m);
    struct device d;
    struct error err = {0};
    assert(device_read(&h, 1, &d, false, &err) == UC_TIMEOUT);
    assert(h.poisoned && m.now == 2000);
    mock_slot(&m, 1, true, false);
    assert(device_read(&h, 1, &d, false, &err) == UC_PROTOCOL);
    assert(m.sends == 1); /* A late reply cannot resume a poisoned session. */

    m = (struct mock){0};
    mock_slot(&m, 1, true, true);
    m.events[2].report.bytes[5] = 15;
    h = mock_session(&m);
    assert(device_read(&h, 1, &d, true, &err) == UC_PROTOCOL);

    m = (struct mock){0};
    mock_slot(&m, 1, true, false);
    mock_error(&m, 0x83, 0xb5, 2);
    mock_error(&m, 0x83, 0xb5, 2);
    h = mock_session(&m);
    assert(device_read(&h, 1, &d, true, &err) == UC_OK);
    assert(d.paired && !d.has_serial && !d.name[0]);
}

/*
 * Verify read retry deadlines and that writes and terminal failures are not
 * retried.
 *
 * Uses fake deadlines to bound retries and injected failures to stop them.
 * Confirms B2 state-changing writes are sent at most once.
 */
static void test_read_retry_limits(void) {
    struct mock m = {0};
    struct hidpp h = mock_session(&m);
    struct error err = {0};
    struct device d;
    h.retry_pairing_reads = true;
    h.deadline = 2500;
    assert(device_read(&h, 1, &d, false, &err) == UC_TIMEOUT);
    assert(m.now == 2500 && m.sends == 2 && h.poisoned);

    m = (struct mock){0};
    h = mock_session(&m);
    h.retry_pairing_reads = true;
    h.deadline = 1000;
    assert(device_read(&h, 1, &d, false, &err) == UC_TIMEOUT);
    assert(m.now == 1000 && m.sends == 1);

    for (unsigned action = 1; action <= 3; action++) {
        m = (struct mock){0};
        h = mock_session(&m);
        h.retry_pairing_reads = true; /* Even explicit opt-in cannot retry SET. */
        struct report reply;
        const uint8_t params[] = {(uint8_t)action, 0, 0};
        assert(hidpp_request(&h, 0x80, 0xb2, params, -1, &reply, &err) == UC_TIMEOUT);
        assert(m.sends == 1 && m.now == 2000);
    }

    int failures[] = {UC_IO, UC_INTERRUPT, UC_TERMINATE};
    for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); i++) {
        m = (struct mock){0};
        m.events[m.count++] = (struct mock_event){.status = failures[i], .at = 2100};
        h = mock_session(&m);
        h.retry_pairing_reads = true;
        assert(device_read(&h, 1, &d, false, &err) == failures[i]);
        assert(m.sends == 2 && m.now == 2100 && h.poisoned);
    }

    m = (struct mock){.send_error = UC_IO};
    h = mock_session(&m);
    h.retry_pairing_reads = true;
    assert(device_read(&h, 1, &d, false, &err) == UC_IO);
    assert(m.now == 0 && h.poisoned);
}

/*
 * Verify receiver allowlisting and unambiguous selection.
 *
 * Checks accepted USB identities and failure when no receiver or multiple
 * receivers are present. Does not enumerate real hardware.
 */
static void test_selection(void) {
    struct receivers *receivers = calloc(1, sizeof(*receivers));
    assert(receivers);
    size_t index = 99;
    struct error err = {0};
    assert(receiver_choose(receivers, &index, &err) == UC_RECEIVER);
    receivers->count = 1;
    assert(receiver_choose(receivers, &index, &err) == UC_OK && index == 0);
    receivers->count = 2;
    assert(receiver_choose(receivers, &index, &err) == UC_RECEIVER);
    free(receivers);
    assert(receiver_supported(0x046d, 0xc52b, 2));
    assert(receiver_supported(0x046d, 0xc532, 2));
    assert(!receiver_supported(0x046d, 0xc548, 2));
    assert(!receiver_supported(0x046d, 0xc52b, 1));
    assert(!receiver_supported(0x1234, 0xc52b, 2));
}

/*
 * Verify receiver ID syntax and integer bounds.
 *
 * Covers valid IDs, malformed prefixes, zero, and integer overflow. Parsing
 * must not depend on a macOS receiver being present.
 */
static void test_registry_id(void) {
    uint64_t id = 0;
    assert(receiver_registry_id("DevSrvsID:4294968397", &id) && id == UINT64_C(4294968397));
    assert(receiver_registry_id("DevSrvsID:18446744073709551615", &id) && id == UINT64_MAX);
    assert(receiver_registry_id("DevSrvsID:1", &id) && id == 1);
    const char *invalid[] = {
        "", "DevSrvsID:", "DevSrvsID:0", "DevSrvsID:-1", "DevSrvsID:+1", "DevSrvsID: 1",
        "DevSrvsID:1x", "DevSrvsID:0x10", "DevSrvsID:18446744073709551616",
        "devsrvsid:1", "/dev/hidraw2", "IOService:/AppleACPIPlatformExpert"
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        id = 42;
        assert(!receiver_registry_id(invalid[i], &id));
        assert(id == 42);
    }
}

/*
 * Verify FIFO ordering, capacity limits, and loss reporting.
 *
 * Exercises report ordering and bounded storage with synthetic bytes. Confirms
 * undersized output buffers do not consume queued reports.
 */
static void test_report_queue(void) {
    struct report_queue *q = calloc(1, sizeof(*q));
    assert(q);
    struct error err = {0};
    uint8_t out[REPORT_QUEUE_BYTES];
    size_t size = sizeof(out);
    assert(report_queue_empty(q));
    assert(report_queue_pop(q, out, &size, &err) == UC_INTERNAL);

    /* FIFO order across wraparound, preserving exact lengths. */
    for (unsigned round = 0; round < 3; round++) {
        for (unsigned i = 0; i < REPORT_QUEUE_DEPTH; i++) {
            uint8_t report[HIDPP_LONG] = {0x10, (uint8_t)i, (uint8_t)round};
            size_t length = (size_t)(i % 2 ? HIDPP_LONG : HIDPP_SHORT);
            assert(report_queue_push(q, report, length));
        }
        for (unsigned i = 0; i < REPORT_QUEUE_DEPTH; i++) {
            size = sizeof(out);
            assert(report_queue_pop(q, out, &size, &err) == UC_OK);
            assert(size == (size_t)(i % 2 ? HIDPP_LONG : HIDPP_SHORT));
            assert(out[0] == 0x10 && out[1] == (uint8_t)i && out[2] == (uint8_t)round);
        }
        assert(report_queue_empty(q) && !q->lost);
    }

    /* A caller buffer that is too small fails without consuming the report. */
    const uint8_t long_report[HIDPP_LONG] = {0x11, 0xff, 0x83, 0xb5};
    assert(report_queue_push(q, long_report, sizeof(long_report)));
    size = HIDPP_SHORT;
    assert(report_queue_pop(q, out, &size, &err) == UC_PROTOCOL);
    size = sizeof(out);
    assert(report_queue_pop(q, out, &size, &err) == UC_OK && size == sizeof(long_report));
    assert(!memcmp(out, long_report, size));

    /* Overflow, empty and oversized reports latch loss instead of dropping silently. */
    uint8_t big[REPORT_QUEUE_BYTES + 1] = {0x20};
    for (unsigned i = 0; i < REPORT_QUEUE_DEPTH; i++) assert(report_queue_push(q, big, 7));
    assert(!q->lost && !report_queue_push(q, big, 7) && q->lost);
    memset(q, 0, sizeof(*q));
    assert(!report_queue_push(q, big, 0) && q->lost && report_queue_empty(q));
    memset(q, 0, sizeof(*q));
    assert(report_queue_push(q, big, REPORT_QUEUE_BYTES));
    assert(!report_queue_push(q, big, sizeof(big)) && q->lost);
    free(q);
}

/*
 * Verify accepted HID++ descriptors and reject invalid layouts.
 *
 * Uses synthetic HID item sequences to exercise report-size validation and
 * malformed input. No platform HID APIs are required.
 */
static void test_descriptor(void) {
    const uint8_t bytes[] = {
        0x06, 0x00, 0xff, 0x09, 1, 0xa1, 1, 0x85, 0x10,
        0x75, 8, 0x95, 6, 0x81, 0, 0x91, 0, 0xc0,
        0x06, 0x00, 0xff, 0x09, 2, 0xa1, 1, 0x85, 0x11,
        0x75, 8, 0x95, 19, 0x81, 0, 0x91, 0, 0xc0
    };
    assert(receiver_descriptor(bytes, sizeof(bytes)));
    assert(!receiver_descriptor(bytes, sizeof(bytes) - 3));
    assert(!receiver_descriptor(bytes, 1));
    uint8_t invalid[sizeof(bytes)];
    memcpy(invalid, bytes, sizeof(bytes));
    invalid[12] = 7;
    assert(!receiver_descriptor(invalid, sizeof(invalid)));
    invalid[0] = 0xb4; /* Global pop with empty stack. */
    assert(!receiver_descriptor(invalid, sizeof(invalid)));
}

/*
 * Run the test suite and report successful completion.
 *
 * Executes each assertion-based test in this suite. Returns zero after
 * printing the completion message; assertion failures abort the process.
 */
int main(void) {
    test_listing();
    test_matching();
    test_failures();
    test_read_retry_limits();
    test_selection();
    test_registry_id();
    test_report_queue();
    test_descriptor();
    puts("Core: listing, matching, malformed input, metadata, discovery, receiver ID, report queue and descriptor tests passed");
    return 0;
}

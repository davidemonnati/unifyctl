#include "cli.h"
#include "devices.h"
#include "discovery.h"
#include "mock.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

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

static void count_notification(void *context, const struct report *report) {
    assert(report->bytes[2] == 0x41);
    (*(unsigned *)context)++;
}

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

int main(void) {
    test_listing();
    test_matching();
    test_failures();
    test_selection();
    test_descriptor();
    puts("Core: listing, matching, malformed input, metadata, discovery and descriptor tests passed");
    return 0;
}

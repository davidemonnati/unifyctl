#include "mock.h"
#include "operations.h"
#include "peripheral.h"
#include "signals.h"

#include <assert.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* Synthetic fixtures independently laid out from the references in
 * PROTOCOL_INFO.md. No hardware and no production encoder/decoder is used. */
static void reply(struct mock *m, unsigned slot, uint8_t feature, uint8_t function,
                  const uint8_t *payload, size_t size) {
    uint8_t bytes[20] = {0x11, (uint8_t)slot, feature, function};
    assert(size <= 16);
    if (size) memcpy(bytes + 4, payload, size);
    mock_report(m, bytes, sizeof(bytes));
}

static void found(struct mock *m, unsigned slot, uint8_t index, uint8_t flags, uint8_t version) {
    uint8_t bytes[] = {0x10, (uint8_t)slot, 0, 0x0e, index, flags, version};
    mock_report(m, bytes, sizeof(bytes));
}

static void ping(struct mock *m, unsigned slot, uint8_t major) {
    uint8_t bytes[] = {0x10, (uint8_t)slot, 0, 0x1e, major, 0, 0x5a};
    mock_report(m, bytes, sizeof(bytes));
}

static void rap_error(struct mock *m, unsigned slot, uint8_t feature, uint8_t function, uint8_t code) {
    uint8_t bytes[] = {0x10, (uint8_t)slot, 0x8f, feature, function, code, 0};
    mock_report(m, bytes, sizeof(bytes));
}

static struct operation_ui ui_open(bool interactive, const char *answer) {
    struct operation_ui ui = {tmpfile(), tmpfile(), tmpfile(), interactive, false};
    assert(ui.input && ui.output && ui.diagnostics);
    if (answer) fputs(answer, ui.input);
    rewind(ui.input);
    return ui;
}

static void ui_close(struct operation_ui *ui) {
    fclose(ui->input);
    fclose(ui->output);
    fclose(ui->diagnostics);
}

static bool contains(FILE *file, const char *needle) {
    char text[8192] = {0};
    fflush(file);
    rewind(file);
    size_t size = fread(text, 1, sizeof(text) - 1, file);
    text[size] = '\0';
    return strstr(text, needle) != NULL;
}

static void request_is(const struct mock *m, size_t index, const uint8_t expected[7]) {
    assert(index < m->sends);
    assert(m->sent[index].length == 7);
    assert(!memcmp(m->sent[index].bytes, expected, 7));
}

static void mouse_fixture(struct mock *m, unsigned slot, bool percentage) {
    mock_slot(m, slot, true, true);
    ping(m, slot, 2);
    found(m, slot, 0, 0, 0); /* 1004 absent. */
    found(m, slot, 0x41, 0, 0); /* 1000 at a notification-like index. */
    const uint8_t caps[] = {percentage ? 100 : 4, percentage ? 2 : 0};
    reply(m, slot, 0x41, 0x1e, caps, sizeof(caps));
    const uint8_t battery[] = {percentage ? 75 : 25, 0, 0};
    reply(m, slot, 0x41, 0x0e, battery, sizeof(battery));
    found(m, slot, 9, 0, 0); /* 1814 */
    const uint8_t host[] = {3, 1};
    reply(m, slot, 9, 0x0e, host, sizeof(host));
    found(m, slot, 0x42, 0, 0); /* 2201 */
    const uint8_t sensors[] = {1};
    reply(m, slot, 0x42, 0x0e, sensors, sizeof(sensors));
    const uint8_t dpi[] = {0, 0x03, 0xe8, 0x06, 0x40}; /* Current 1000, default 1600. */
    reply(m, slot, 0x42, 0x2e, dpi, sizeof(dpi));
    found(m, slot, 0, 0, 0); /* 0080 absent. */
}

static void test_mouse(void) {
    for (unsigned percentage = 0; percentage < 2; percentage++) {
        struct mock m = {0};
        mouse_fixture(&m, 3, percentage != 0);
        struct hidpp h = mock_session(&m);
        struct operation_ui ui = ui_open(false, "not consumed\n");
        struct error err = {0};
        assert(operation_info(&h, 3, &ui, &err) == UC_OK);
        assert(contains(ui.output, "Slot:           3\n"));
        assert(contains(ui.output, percentage ? "Battery:        75% (discharging)" : "Battery:        low (discharging)"));
        assert(contains(ui.output, "Connection:     reachable\nPower:          unknown\n"));
        assert(contains(ui.output, "Active channel: 2 (host channel, 1-based)"));
        assert(contains(ui.output, "DPI:            1000 DPI\nBacklight:      not applicable\nSignal quality: unsupported"));
        assert(!contains(ui.output, "1600 DPI") && !contains(ui.output, "SLOT NAME"));
        assert(ftell(ui.input) == 0 && ftell(ui.diagnostics) == 0);
        assert(!h.poisoned && !h.pending && !h.deadline);
        assert(m.sends == 14 && m.position == m.count);
        /* Explicit slot reads only B5 selectors for slot 3, then address 3. */
        const uint8_t expected[][7] = {
            {0x10, 0xff, 0x83, 0xb5, 0x22, 0, 0},
            {0x10, 0xff, 0x83, 0xb5, 0x32, 0, 0},
            {0x10, 0xff, 0x83, 0xb5, 0x42, 0, 0},
            {0x10, 3, 0, 0x1e, 0, 0, 0x5a},
            {0x10, 3, 0, 0x0e, 0x10, 0x04, 0},
            {0x10, 3, 0, 0x0e, 0x10, 0, 0},
            {0x10, 3, 0x41, 0x1e, 0, 0, 0},
            {0x10, 3, 0x41, 0x0e, 0, 0, 0},
            {0x10, 3, 0, 0x0e, 0x18, 0x14, 0},
            {0x10, 3, 9, 0x0e, 0, 0, 0},
            {0x10, 3, 0, 0x0e, 0x22, 1, 0},
            {0x10, 3, 0x42, 0x0e, 0, 0, 0},
            {0x10, 3, 0x42, 0x2e, 0, 0, 0},
            {0x10, 3, 0, 0x0e, 0, 0x80, 0}
        };
        for (size_t n = 0; n < sizeof(expected) / sizeof(expected[0]); n++) request_is(&m, n, expected[n]);
        ui_close(&ui);
    }
}

static void keyboard_fixture(struct mock *m, unsigned version, bool percentage) {
    mock_slot(m, 6, true, true);
    m->events[0].report.bytes[11] = 1;
    ping(m, 6, 4);
    found(m, 6, 5, 0, 0); /* 1004 */
    const uint8_t caps[] = {15, percentage ? 3 : 1};
    reply(m, 6, 5, 0x0e, caps, sizeof(caps));
    const uint8_t battery[] = {0, 4, 1, 0};
    reply(m, 6, 5, 0x1e, battery, sizeof(battery));
    found(m, 6, 0, 0, 0); /* 1814 absent */
    found(m, 6, 0x4a, 0, (uint8_t)version); /* 1982 */
    const uint8_t backlight[] = {1, 8, 0, 1, 0, 0};
    reply(m, 6, 0x4a, 0x0e, backlight, sizeof(backlight));
    found(m, 6, 7, 0, 0); /* 0080 exists, units unknown */
}

static void test_keyboard(void) {
    for (unsigned version = 0; version <= 3; version++) {
        struct mock m = {0};
        keyboard_fixture(&m, version, version == 3);
        struct hidpp h = mock_session(&m);
        struct operation_ui ui = ui_open(false, NULL);
        struct error err = {0};
        assert(operation_info(&h, 6, &ui, &err) == UC_OK);
        assert(contains(ui.output, version == 3 ? "Battery:        0% (charging)" : "Battery:        good (charging)"));
        assert(contains(ui.output, "DPI:            not applicable"));
        assert(contains(ui.output, "Backlight:      enabled (configured enablement)"));
        assert(contains(ui.output, version == 3 ? "configured manual level 0 (device steps 0–7)" : "level unknown"));
        assert(contains(ui.output, "Signal quality: unknown"));
        assert(m.sends == 11 && m.position == m.count);
        for (size_t n = 3; n < m.sends; n++) {
            const uint8_t *p = m.sent[n].bytes;
            assert(p[1] == 6);
            assert(p[2] == 0 || p[2] == 5 || p[2] == 0x4a);
            if (p[2] == 0x4a) assert(p[3] == 0x0e); /* No setter. */
        }
        ui_close(&ui);
    }
}

static void test_interactive(void) {
    const char *answers[] = {" \t05 \r\n", "", "2\n", "0\n", "7\n", "-1\n", "1x\n", "5 1\n", "\n", "5", "000000000000000000000000000000000000000000000000000000000000000005\n"};
    int expected[] = {UC_OK, UC_REFUSED, UC_USAGE, UC_USAGE, UC_USAGE, UC_USAGE, UC_USAGE, UC_USAGE, UC_USAGE, UC_REFUSED, UC_USAGE};
    for (size_t n = 0; n < sizeof(expected) / sizeof(expected[0]); n++) {
        struct mock m = {0};
        mock_snapshot(&m, 1u << 4, true);
        if (!n) {
            mock_slot(&m, 5, true, true);
            rap_error(&m, 5, 0, 0x1e, 1); /* Legacy version fallback. */
        }
        struct hidpp h = mock_session(&m);
        struct operation_ui ui = ui_open(true, answers[n]);
        struct error err = {0};
        assert(operation_info(&h, 0, &ui, &err) == expected[n]);
        assert(contains(ui.output, "SLOT NAME") && contains(ui.output, "5  Mouse"));
        assert(contains(ui.diagnostics, "Select a device slot (1–6): "));
        assert(contains(ui.output, "Slot:") == (n == 0));
        assert(m.sends == (n == 0 ? 12 : 8));
        assert(!h.deadline);
        ui_close(&ui);
    }
    for (unsigned occupied = 0; occupied < 2; occupied++) {
        struct mock m = {0};
        mock_snapshot(&m, occupied, true);
        struct hidpp h = mock_session(&m);
        struct operation_ui ui = ui_open(false, "1\n");
        struct error err = {0};
        assert(operation_info(&h, 0, &ui, &err) == (occupied ? UC_REFUSED : UC_OK));
        assert(ftell(ui.input) == 0 && ftell(ui.diagnostics) == 0);
        if (occupied) assert(strstr(err.message, "info SLOT"));
        else assert(contains(ui.output, "No paired devices."));
        ui_close(&ui);
    }
    struct mock m = {0};
    mock_snapshot(&m, 4, true);
    mock_slot(&m, 3, true, true);
    m.events[m.count - 2].report.bytes[5] ^= 1; /* Serial changed. */
    struct hidpp h = mock_session(&m);
    struct operation_ui ui = ui_open(true, "3\n");
    struct error err = {0};
    assert(operation_info(&h, 0, &ui, &err) == UC_PROTOCOL);
    assert(strstr(err.message, "rerun info") && !contains(ui.output, "Slot:"));
    assert(m.sends == 11);
    ui_close(&ui);
}

static void test_matching(void) {
    struct mock m = {0};
    uint8_t bytes[] = {0x10, 2, 0x41, 0x2e, 0, 3, 0xe8};
    mock_report(&m, bytes, 7); /* Wrong device. */
    bytes[1] = 1;
    bytes[2] = 0x42;
    mock_report(&m, bytes, 7); /* Wrong feature. */
    bytes[2] = 0x41;
    bytes[3] = 0x1e;
    mock_report(&m, bytes, 7); /* Wrong function. */
    bytes[3] = 0x2d;
    mock_report(&m, bytes, 7); /* Wrong software. */
    bytes[3] = 0x20;
    mock_report(&m, bytes, 7); /* Broadcast. */
    const uint8_t connection[] = {0x10, 1, 0x41, 4, 2, 1, 0x40};
    mock_report(&m, connection, sizeof(connection));
    rap_error(&m, 2, 0x41, 0x2e, 9);
    rap_error(&m, 1, 0x42, 0x2e, 9);
    const uint8_t wrong[] = {0x41, 0x2d, 9};
    reply(&m, 1, 0xff, 0x41, wrong + 1, 2); /* Wrong request identifier. */
    bytes[3] = 0x2e;
    mock_report(&m, bytes, 7);
    struct hidpp h = mock_session(&m);
    struct report r;
    struct error err = {0};
    const uint8_t params[] = {0, 0, 0};
    assert(hidpp_feature_request(&h, 1, 0x41, 2, params, 3, &r, &err) == UC_OK);
    assert(r.bytes[6] == 0xe8 && m.sends == 1 && m.position == m.count && !h.poisoned);
}

static void test_failures(void) {
    for (unsigned test = 0; test < 9; test++) {
        struct mock m = {0};
        mock_slot(&m, 1, test != 0, true);
        int expected = UC_PROTOCOL;
        size_t sends = test ? 4 : 1;
        if (test == 1) expected = UC_TIMEOUT; /* No ping reply. */
        if (test == 2) {
            ping(&m, 1, 2);
            m.events[m.count - 1].report.bytes[6] = 0; /* Wrong ping echo. */
        }
        if (test == 3) {
            const uint8_t truncated[] = {0x10, 1, 0, 0x1e, 2, 0};
            mock_report(&m, truncated, sizeof(truncated));
        }
        if (test == 4) rap_error(&m, 1, 0, 0x1e, 9); /* Resource error alone is not offline. */
        if (test == 5) rap_error(&m, 1, 0, 0x1e, 8); /* Unknown routing address. */
        if (test >= 6) {
            m.events[m.count++].status = test == 6 ? UC_IO : test == 7 ? UC_INTERRUPT : UC_TERMINATE;
            expected = test == 6 ? UC_IO : test == 7 ? UC_INTERRUPT : UC_TERMINATE;
        }
        struct hidpp h = mock_session(&m);
        h.retry_pairing_reads = true; /* Must still never retry info. */
        struct operation_ui ui = ui_open(false, NULL);
        struct error err = {0};
        assert(operation_info(&h, 1, &ui, &err) == expected);
        assert(!contains(ui.output, "Slot:") && m.sends == sends && !h.deadline);
        assert(h.retry_pairing_reads);
        if (test == 1 || test == 2 || test == 3 || test >= 6) {
            size_t before = m.sends;
            struct report r;
            const uint8_t params[] = {0, 0, 0};
            assert(h.poisoned);
            assert(hidpp_feature_request(&h, 1, 0, 1, params, 3, &r, &err) == UC_PROTOCOL);
            assert(m.sends == before);
        }
        ui_close(&ui);
    }
    /* Explicit receiver link-loss followed by a matched resource error. */
    struct mock m = {0};
    mock_slot(&m, 1, true, true);
    const uint8_t offline[] = {0x10, 1, 0x41, 4, 0x42, 1, 0x40};
    mock_report(&m, offline, sizeof(offline));
    rap_error(&m, 1, 0, 0x1e, 9);
    struct hidpp h = mock_session(&m);
    struct operation_ui ui = ui_open(false, NULL);
    struct error err = {0};
    assert(operation_info(&h, 1, &ui, &err) == UC_OK);
    assert(contains(ui.output, "Connection:     unreachable\nPower:          unknown"));
    assert(contains(ui.output, "Battery:        unavailable (device unreachable)"));
    assert(m.sends == 4 && !h.poisoned);
    ui_close(&ui);
}

static void test_deadlines(void) {
    /* Each matching reply arrives just inside its request deadline, but the
     * cumulative scan must stop at the shared 30-second phase budget. */
    struct mock m = {0};
    mock_snapshot(&m, 0x3f, true);
    for (size_t n = 0; n < m.count; n++) m.events[n].at = (int64_t)(n + 1) * 1900;
    struct hidpp h = mock_session(&m);
    struct operation_ui ui = ui_open(false, NULL);
    struct error err = {0};
    assert(operation_info(&h, 0, &ui, &err) == UC_TIMEOUT);
    assert(m.now == 30000 && m.sends == 16 && h.poisoned && !h.deadline);
    assert(!contains(ui.output, "SLOT NAME"));
    ui_close(&ui);

    /* A caller's tighter deadline remains in force and is restored on exit. */
    m = (struct mock){0};
    h = mock_session(&m);
    h.deadline = 100;
    ui = ui_open(false, NULL);
    assert(operation_info(&h, 1, &ui, &err) == UC_TIMEOUT);
    assert(m.now == 100 && h.deadline == 100 && m.sends == 1);
    ui_close(&ui);
}

static void test_extended_dpi(void) {
    for (unsigned sensors = 1; sensors <= 2; sensors++) {
        struct mock m = {0};
        mock_slot(&m, 1, true, true);
        ping(&m, 1, 2);
        found(&m, 1, 0, 0, 0); /* 1004 */
        found(&m, 1, 0, 0, 0); /* 1000 */
        found(&m, 1, 0, 0, 0); /* 1001 */
        found(&m, 1, 0, 0, 0); /* 1814 */
        found(&m, 1, 0, 0, 0); /* 2201 */
        found(&m, 1, 8, 0, 0); /* 2202 */
        const uint8_t count[] = {(uint8_t)sensors};
        reply(&m, 1, 8, 0x0e, count, sizeof(count));
        for (unsigned sensor = 0; sensor < sensors; sensor++) {
            const uint8_t caps[] = {(uint8_t)sensor, 0, 1};
            reply(&m, 1, 8, 0x1e, caps, sizeof(caps));
            const uint8_t dpi[] = {(uint8_t)sensor, 0x03, 0xe8, 0x06, 0x40, 0x07, 0xd0, 0x0c, 0x80, 0};
            reply(&m, 1, 8, 0x5e, dpi, sizeof(dpi));
        }
        found(&m, 1, 0, 0, 0); /* signal */
        struct hidpp h = mock_session(&m);
        struct operation_ui ui = ui_open(false, NULL);
        struct error err = {0};
        assert(operation_info(&h, 1, &ui, &err) == UC_OK);
        assert(contains(ui.output, "X 1000 DPI / Y 2000 DPI"));
        if (sensors == 2) assert(contains(ui.output, "sensor 1: X 1000 DPI / Y 2000 DPI"));
        assert(!contains(ui.output, "1600") && !contains(ui.output, "3200"));
        assert(m.position == m.count);
        const uint8_t request[] = {0x10, 1, 8, 0x5e, 0, 0, 0};
        request_is(&m, 12, request);
        ui_close(&ui);
    }
}

static void test_availability_and_validation(void) {
    /* 1000's zero sentinel, hidden features, malformed metrics, correctly
     * matched unsupported responses, unknown protocol versions and legacy RAP. */
    for (unsigned test = 0; test < 10; test++) {
        struct mock m = {0};
        mouse_fixture(&m, 3, true);
        int expected = UC_OK;
        if (test == 0) m.events[7].report.bytes[4] = 0; /* 1000 unknown percentage. */
        if (test == 1) {
            m.events[7].report.bytes[4] = 101;
            expected = UC_PROTOCOL;
        }
        if (test == 2) {
            m.events[9].report.bytes[5] = 3; /* host index == host count */
            expected = UC_PROTOCOL;
        }
        if (test == 3) {
            m.events[12].report.bytes[4] = 1; /* wrong echoed sensor */
            expected = UC_PROTOCOL;
        }
        if (test == 4) m.events[12].report.bytes[5] = m.events[12].report.bytes[6] = 0;
        if (test == 5) {
            /* Hidden battery capability: do not query the hidden index. */
            m.events[5].report.bytes[5] = 0x40;
            memmove(&m.events[6], &m.events[8], (m.count - 8) * sizeof(m.events[0]));
            m.count -= 2;
        }
        if (test == 6) {
            m.events[3].report.bytes[4] = 9; /* Future protocol: identity remains. */
            m.count = 4;
        }
        if (test == 7) {
            uint8_t bytes[] = {0x11, 3, 0xff, 9, 0x0e, 9}; /* 1814 unsupported. */
            memcpy(m.events[9].report.bytes, bytes, sizeof(bytes));
        }
        if (test == 8) {
            uint8_t bytes[] = {0x11, 3, 0xff, 9, 0x0e, 8}; /* Busy is fatal. */
            memcpy(m.events[9].report.bytes, bytes, sizeof(bytes));
            expected = UC_PROTOCOL;
        }
        if (test == 9) {
            m.events[10].report.bytes[4] = 0xff; /* Reserved feature index. */
            expected = UC_PROTOCOL;
        }
        struct hidpp h = mock_session(&m);
        struct operation_ui ui = ui_open(false, NULL);
        struct error err = {0};
        assert(operation_info(&h, 3, &ui, &err) == expected);
        if (expected) assert(!contains(ui.output, "Slot:"));
        else {
            assert(m.position == m.count);
            if (test == 0) assert(contains(ui.output, "Battery:        unknown (discharging)"));
            if (test == 4) assert(contains(ui.output, "DPI:            unknown"));
            if (test == 5 || test == 6) assert(contains(ui.output, "Battery:        unknown"));
            if (test == 7) assert(contains(ui.output, "Active channel: unsupported"));
        }
        ui_close(&ui);
    }
    /* A short matching report cannot supply a four-byte 1004 payload. */
    struct mock m = {0};
    const uint8_t bytes[] = {0x10, 1, 5, 0x1e, 50, 4, 0};
    mock_report(&m, bytes, sizeof(bytes));
    struct hidpp h = mock_session(&m);
    struct report r;
    struct error err = {0};
    const uint8_t params[] = {0, 0, 0};
    assert(hidpp_feature_request(&h, 1, 5, 1, params, 4, &r, &err) == UC_PROTOCOL);
    assert(h.poisoned && m.sends == 1);
}

/* Real signal delivery while input remains open and idle. The parent waits for
 * the actual prompt before signalling; no sleeps or physical receiver needed. */
static void test_prompt_signals(void) {
    const int signals[] = {SIGINT, SIGTERM};
    for (size_t n = 0; n < sizeof(signals) / sizeof(signals[0]); n++) {
        int input[2], prompt[2];
        assert(pipe(input) == 0 && pipe(prompt) == 0);
        pid_t child = fork();
        assert(child >= 0);
        if (!child) {
            close(input[1]);
            close(prompt[0]);
            alarm(5);
            struct mock m = {0};
            mock_snapshot(&m, 1, true);
            struct hidpp h = mock_session(&m);
            struct operation_ui ui = {fdopen(input[0], "r"), tmpfile(), fdopen(prompt[1], "w"), true, false};
            struct error err = {0};
            assert(ui.input && ui.output && ui.diagnostics);
            assert(signals_install(&err) == UC_OK);
            int status = operation_info(&h, 0, &ui, &err);
            assert(!h.deadline && m.sends == 8);
            signals_restore();
            assert(signals_wake_fd() == -1);
            ui_close(&ui);
            _exit(status);
        }
        close(input[0]);
        close(prompt[1]);
        struct pollfd ready = {prompt[0], POLLIN, 0};
        assert(poll(&ready, 1, 3000) == 1);
        char buffer[128];
        assert(read(prompt[0], buffer, sizeof(buffer)) > 0);
        assert(kill(child, signals[n]) == 0);
        int status;
        assert(waitpid(child, &status, 0) == child);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == (signals[n] == SIGINT ? UC_INTERRUPT : UC_TERMINATE));
        close(input[1]);
        close(prompt[0]);
    }
}

int main(void) {
    test_mouse();
    test_keyboard();
    test_interactive();
    test_matching();
    test_failures();
    test_deadlines();
    test_extended_dpi();
    test_availability_and_validation();
    test_prompt_signals();
    puts("Info: selection, metrics, wire requests, matching, failures, deadlines and real prompt signals passed");
    return 0;
}

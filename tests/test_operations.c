#include "operations.h"
#include "mock.h"

#include <assert.h>
#include <string.h>

static struct operation_ui ui_open(bool interactive, bool yes, const char *answer) {
    struct operation_ui ui = {tmpfile(), tmpfile(), tmpfile(), interactive, yes};
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
    char text[4096] = {0};
    fflush(file);
    rewind(file);
    size_t n = fread(text, 1, sizeof(text) - 1, file);
    text[n] = '\0';
    return strstr(text, needle) != NULL;
}

static size_t writes(const struct mock *m, uint8_t reg, uint8_t first) {
    size_t count = 0;
    for (size_t i = 0; i < m->sends; i++) {
        const uint8_t *p = m->sent[i].bytes;
        if (p[2] == 0x80 && p[3] == reg && p[4] == first) count++;
    }
    return count;
}

static void prepare_add(struct mock *m) {
    mock_snapshot(m, 1, false);
    const uint8_t flags[] = {0x10, 0xff, 0x81, 0x00, 0x10, 0x08, 0x00};
    mock_report(m, flags, sizeof(flags));
    mock_ack(m, 0x00);
}

static void close_notification(struct mock *m, uint8_t code) {
    uint8_t closed[] = {0x10, 0xff, 0x4a, 0, code, 0, 0};
    mock_report(m, closed, sizeof(closed));
}

static void connection_notification(struct mock *m, uint8_t slot) {
    uint8_t connected[] = {0x10, slot, 0x41, 4, 2, 1, 0x40};
    mock_report(m, connected, sizeof(connected));
}

static void cleanup_replies(struct mock *m) {
    mock_ack(m, 0xb2);
    mock_ack(m, 0x00);
}

static void test_pair_success(void) {
    for (unsigned before_ack = 0; before_ack < 2; before_ack++) {
        struct mock m = {0};
        prepare_add(&m);
        if (!before_ack) mock_ack(&m, 0xb2);
        connection_notification(&m, 3);
        close_notification(&m, 0);
        if (before_ack) mock_ack(&m, 0xb2);
        mock_snapshot(&m, 5, false);
        mock_slot(&m, 3, true, true);
        cleanup_replies(&m);
        struct hidpp h = mock_session(&m);
        struct operation_ui ui = ui_open(false, false, NULL);
        struct error err = {0};
        assert(operation_add(&h, 30, &ui, &err) == UC_OK);
        assert(contains(ui.output, "New stored pairing"));
        assert(m.position == m.count);
        assert(writes(&m, 0xb2, 1) == 1 && writes(&m, 0xb2, 2) == 1);
        assert(m.sent[7].bytes[4] == 0x10 && m.sent[7].bytes[5] == 0x09);
        assert(m.sent[m.sends - 1].bytes[4] == 0x10 && m.sent[m.sends - 1].bytes[5] == 0x08);
        ui_close(&ui);
    }
}

static void test_pair_failures(void) {
    int expected[] = {UC_TIMEOUT, UC_PROTOCOL, UC_PROTOCOL, UC_TIMEOUT};
    uint8_t codes[] = {1, 2, 3, 6};
    for (size_t i = 0; i < sizeof(codes); i++) {
        struct mock m = {0};
        prepare_add(&m);
        mock_ack(&m, 0xb2);
        close_notification(&m, codes[i]);
        cleanup_replies(&m);
        struct hidpp h = mock_session(&m);
        struct operation_ui ui = ui_open(false, false, NULL);
        struct error err = {0};
        assert(operation_add(&h, 1, &ui, &err) == expected[i]);
        assert(!contains(ui.output, "New stored pairing"));
        assert(writes(&m, 0xb2, 2) == 1);
        ui_close(&ui);
    }
    for (unsigned reconnect = 0; reconnect < 2; reconnect++) {
        struct mock m = {0};
        prepare_add(&m);
        mock_ack(&m, 0xb2);
        if (reconnect) connection_notification(&m, 1);
        size_t cleanup_start = m.count;
        cleanup_replies(&m);
        m.events[cleanup_start].at = 1000;
        m.events[cleanup_start + 1].at = 1000;
        struct hidpp h = mock_session(&m);
        struct operation_ui ui = ui_open(false, false, NULL);
        struct error err = {0};
        assert(operation_add(&h, 1, &ui, &err) == UC_TIMEOUT);
        assert(m.now == 1000 && !h.poisoned);
        assert(writes(&m, 0xb2, 1) == 1 && writes(&m, 0xb2, 2) == 1);
        ui_close(&ui);
    }
    struct mock m = {0};
    mock_snapshot(&m, 0x3f, false);
    struct hidpp h = mock_session(&m);
    struct operation_ui ui = ui_open(false, false, NULL);
    struct error err = {0};
    assert(operation_add(&h, 30, &ui, &err) == UC_PROTOCOL);
    assert(writes(&m, 0xb2, 1) == 0 && strstr(err.message, "full"));
    ui_close(&ui);
}

static void test_pair_interruption(void) {
    int statuses[] = {UC_INTERRUPT, UC_TERMINATE, UC_IO};
    for (size_t i = 0; i < sizeof(statuses) / sizeof(statuses[0]); i++) {
        struct mock m = {0};
        prepare_add(&m);
        mock_ack(&m, 0xb2);
        m.events[m.count++].status = statuses[i];
        cleanup_replies(&m);
        struct hidpp h = mock_session(&m);
        struct operation_ui ui = ui_open(false, false, NULL);
        struct error err = {0};
        assert(operation_add(&h, 30, &ui, &err) == statuses[i]);
        assert(writes(&m, 0xb2, 2) == 1);
        assert(!contains(ui.output, "New stored pairing"));
        ui_close(&ui);
    }
    struct mock m = {0};
    prepare_add(&m);
    /* The pairing command is sent, but its acknowledgement never arrives. */
    struct hidpp h = mock_session(&m);
    struct operation_ui ui = ui_open(false, false, NULL);
    struct error err = {0};
    assert(operation_add(&h, 30, &ui, &err) == UC_TIMEOUT);
    assert(writes(&m, 0xb2, 1) == 1 && writes(&m, 0xb2, 2) == 1);
    assert(contains(ui.diagnostics, "cannot be verified"));
    ui_close(&ui);
}

static void test_pair_unverified_and_cleanup_failure(void) {
    struct mock m = {0};
    prepare_add(&m);
    mock_ack(&m, 0xb2);
    close_notification(&m, 0);
    mock_snapshot(&m, 1, false); /* Closed successfully, but no new stored slot. */
    struct hidpp h = mock_session(&m);
    struct operation_ui ui = ui_open(false, false, NULL);
    struct error err = {0};
    assert(operation_add(&h, 30, &ui, &err) != UC_OK);
    assert(!contains(ui.output, "New stored pairing"));
    ui_close(&ui);

    m = (struct mock){0};
    prepare_add(&m);
    mock_ack(&m, 0xb2);
    connection_notification(&m, 3);
    close_notification(&m, 0);
    mock_snapshot(&m, 5, false);
    mock_slot(&m, 3, true, true);
    mock_error(&m, 0x80, 0xb2, 7); /* Cleanup close rejected. */
    h = mock_session(&m);
    ui = ui_open(false, false, NULL);
    assert(operation_add(&h, 30, &ui, &err) == UC_PROTOCOL);
    assert(contains(ui.output, "New stored pairing"));
    assert(strstr(err.message, "device paired"));
    assert(contains(ui.diagnostics, "cleanup incomplete"));
    ui_close(&ui);
}

static void test_remove_confirmation(void) {
    const char *answers[] = {"n\n", "\n", "YES\n", "", "y\n"};
    for (size_t i = 0; i < sizeof(answers) / sizeof(answers[0]); i++) {
        struct mock m = {0};
        mock_slot(&m, 2, true, true);
        struct hidpp h = mock_session(&m);
        struct operation_ui ui = ui_open(i != 4, false, answers[i]);
        struct error err = {0};
        assert(operation_remove(&h, 2, &ui, &err) == UC_REFUSED);
        assert(writes(&m, 0xb2, 3) == 0);
        assert(contains(ui.diagnostics, "disconnects"));
        ui_close(&ui);
    }
    for (unsigned yes = 0; yes < 2; yes++) {
        struct mock m = {0};
        mock_slot(&m, 2, true, true);
        mock_slot(&m, 2, true, true);
        mock_ack(&m, 0xb2);
        mock_slot(&m, 2, false, false);
        struct hidpp h = mock_session(&m);
        struct operation_ui ui = ui_open(!yes, yes != 0, "yes\n");
        struct error err = {0};
        assert(operation_remove(&h, 2, &ui, &err) == UC_OK);
        assert(writes(&m, 0xb2, 3) == 1);
        assert(m.sent[6].bytes[5] == 2 && m.sent[6].bytes[6] == 0);
        assert(contains(ui.output, "Removed stored pairing from slot 2"));
        ui_close(&ui);
    }
}

static void test_remove_failures(void) {
    struct error err = {0};
    struct mock m = {0};
    mock_slot(&m, 2, false, false);
    struct hidpp h = mock_session(&m);
    struct operation_ui ui = ui_open(false, true, NULL);
    assert(operation_remove(&h, 2, &ui, &err) == UC_PROTOCOL);
    assert(writes(&m, 0xb2, 3) == 0);
    ui_close(&ui);

    m = (struct mock){0};
    mock_slot(&m, 2, true, true);
    mock_slot(&m, 2, true, true);
    m.events[4].report.bytes[5] ^= 1; /* Same model, different serial. */
    h = mock_session(&m);
    ui = ui_open(true, false, "y\n");
    assert(operation_remove(&h, 2, &ui, &err) == UC_PROTOCOL);
    assert(writes(&m, 0xb2, 3) == 0 && strstr(err.message, "changed"));
    ui_close(&ui);

    m = (struct mock){0};
    mock_slot(&m, 2, true, true);
    mock_slot(&m, 2, true, true);
    h = mock_session(&m);
    ui = ui_open(false, true, NULL);
    assert(operation_remove(&h, 2, &ui, &err) == UC_TIMEOUT);
    assert(writes(&m, 0xb2, 3) == 1);
    assert(contains(ui.diagnostics, "outcome may be uncertain"));
    ui_close(&ui);

    m = (struct mock){0};
    mock_slot(&m, 2, true, true);
    mock_slot(&m, 2, true, true);
    mock_ack(&m, 0xb2);
    m.events[m.count++].status = UC_IO;
    h = mock_session(&m);
    ui = ui_open(false, true, NULL);
    assert(operation_remove(&h, 2, &ui, &err) == UC_IO);
    assert(writes(&m, 0xb2, 3) == 1 && !contains(ui.output, "Removed"));
    ui_close(&ui);
}

int main(void) {
    test_pair_success();
    test_pair_failures();
    test_pair_interruption();
    test_pair_unverified_and_cleanup_failure();
    test_remove_confirmation();
    test_remove_failures();
    puts("Operations: pairing, cancellation, cleanup, confirmation and removal tests passed");
    return 0;
}

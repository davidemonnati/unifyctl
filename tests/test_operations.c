#include "operations.h"
#include "mock.h"

#include <assert.h>
#include <string.h>

/*
 * Create temporary streams and confirmation input for an operation test.
 *
 * Preloads answer when non-NULL and rewinds the input stream. Asserts stream
 * creation; the caller must release all three streams with ui_close.
 */
static struct operation_ui ui_open(bool interactive, bool yes, const char *answer) {
    struct operation_ui ui = {tmpfile(), tmpfile(), tmpfile(), interactive, yes};
    assert(ui.input && ui.output && ui.diagnostics);
    if (answer) fputs(answer, ui.input);
    rewind(ui.input);
    return ui;
}

/*
 * Close the temporary operation UI streams.
 *
 * Requires streams created by ui_open. Does not reset the pointers, so the
 * same UI must not be closed twice.
 */
static void ui_close(struct operation_ui *ui) {
    fclose(ui->input);
    fclose(ui->output);
    fclose(ui->diagnostics);
}

/*
 * Check whether captured output contains the expected text.
 *
 * Flushes and rewinds file, then searches at most 4095 bytes. Changes the
 * stream position and returns false if the text is outside that captured
 * prefix.
 */
static bool contains(FILE *file, const char *needle) {
    char text[4096] = {0};
    fflush(file);
    rewind(file);
    size_t n = fread(text, 1, sizeof(text) - 1, file);
    text[n] = '\0';
    return strstr(text, needle) != NULL;
}

/*
 * Count mock register writes with the specified first parameter.
 *
 * Filters recorded sends by operation 0x80, register, and first parameter.
 * Reads the fixture without modifying counters or reports.
 */
static size_t writes(const struct mock *m, uint8_t reg, uint8_t first) {
    size_t count = 0;
    for (size_t i = 0; i < m->sends; i++) {
        const uint8_t *p = m->sent[i].bytes;
        if (p[2] == 0x80 && p[3] == reg && p[4] == first) count++;
    }
    return count;
}

/*
 * Queue a receiver snapshot with slot 1 occupied and notification-flag setup.
 *
 * Starts with slot 1 occupied, then queues wireless-only flags and an
 * acknowledgement for enabling software-presence notifications. The caller
 * appends pairing and cleanup events.
 */
static void prepare_add(struct mock *m) {
    mock_snapshot(m, 1, false);
    /* Receiver reported by the user: wireless on, software present off. */
    const uint8_t flags[] = {0x10, 0xff, 0x81, 0x00, 0x00, 0x01, 0x00};
    mock_report(m, flags, sizeof(flags));
    mock_ack(m, 0x00);
}

/*
 * Queue a pairing-window closure notification with the given result.
 *
 * A zero code represents successful closure; nonzero codes exercise receiver-
 * reported failures. Appends the notification without advancing time.
 */
static void close_notification(struct mock *m, uint8_t code) {
    uint8_t closed[] = {0x10, 0xff, 0x4a, 0, code, 0, 0};
    mock_report(m, closed, sizeof(closed));
}

/*
 * Queue a connection notification for the selected slot.
 *
 * Encodes an established link using WPID 0x4001 and a one-based device slot.
 * Used with matching synthetic stored records.
 */
static void connection_notification(struct mock *m, uint8_t slot) {
    uint8_t connected[] = {0x10, slot, 0x41, 4, 2, 1, 0x40};
    mock_report(m, connected, sizeof(connected));
}

/*
 * Queue acknowledgements for pairing cleanup and flag restoration.
 *
 * Appends the B2 close acknowledgement before the notification-register
 * acknowledgement. Tests can adjust event times afterward to exercise
 * deadlines.
 */
static void cleanup_replies(struct mock *m) {
    mock_ack(m, 0xb2);
    mock_ack(m, 0x00);
}

/*
 * Verify successful pairing and receiver-state cleanup.
 *
 * Exercises notifications both before and after the open acknowledgement.
 * Checks one open/close sequence, verified output, and restoration of the
 * original notification flags.
 */
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
        assert(m.sent[7].bytes[4] == 0x00 && m.sent[7].bytes[5] == 0x09);
        assert(m.sent[m.sends - 1].bytes[4] == 0x00 && m.sent[m.sends - 1].bytes[5] == 0x01);
        ui_close(&ui);
    }
}

/*
 * Verify pairing rejection, receiver errors, and timeouts.
 *
 * Covers receiver error codes, silence, reconnection of an existing device,
 * and a full receiver. Checks that these cases cannot produce a new-pairing
 * success.
 */
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

/*
 * Verify cancellation handling and bounded pairing cleanup.
 *
 * Injects SIGINT, SIGTERM, I/O failure, and a missing open acknowledgement.
 * Verifies cleanup attempts without falsely reporting a paired device.
 */
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

/*
 * Verify diagnostics for uncertain pairing and cleanup failures.
 *
 * Distinguishes successful closure without a verified slot from verified
 * pairing followed by cleanup rejection. Checks both exit status and
 * diagnostic output.
 */
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

/*
 * Hardware trace supplied by the user: pairing completes, but the first
 * occupancy query sees an extended-record response (selector 0x30) instead.
 * Serial bytes are anonymized. Timings and later verification are synthetic.
 *
 * Appends an initially empty snapshot and the supplied notification sequence
 * without a matching occupancy reply. Individual tests add delayed recovery
 * records or let verification expire.
 */
static void paired_trace(struct mock *m) {
    mock_snapshot(m, 0, false);
    const uint8_t flags[] = {0x10, 0xff, 0x81, 0, 0, 9, 0};
    const uint8_t open[] = {0x10, 0xff, 0x4a, 1, 0, 0, 0};
    const uint8_t offline[] = {0x10, 1, 0x41, 4, 0x72, 0x82, 0x40};
    const uint8_t dj_link[15] = {0x20, 1, 0x42, 1};
    const uint8_t dj_pair[15] = {0x20, 1, 0x41, 0, 0x82, 0x40, 6};
    const uint8_t extended[20] = {0x11, 0xff, 0x83, 0xb5, 0x30, 0x12, 0x34, 0x56, 0x78, 6, 0, 0, 0, 1};
    const uint8_t online[] = {0x10, 1, 0x41, 4, 0xb2, 0x82, 0x40};
    const uint8_t dj_online[15] = {0x20, 1, 0x42, 0};
    const uint8_t feature[20] = {0x11, 1, 4, 0, 1, 1, 1};
    const uint8_t battery[20] = {0x11, 1, 8, 0, 0x64, 0x32};
    mock_report(m, flags, sizeof(flags));
    mock_report(m, open, sizeof(open));
    mock_ack(m, 0xb2);
    mock_report(m, offline, sizeof(offline));
    mock_report(m, dj_link, sizeof(dj_link));
    mock_report(m, dj_pair, sizeof(dj_pair));
    close_notification(m, 0);
    mock_report(m, extended, sizeof(extended));
    mock_report(m, online, sizeof(online));
    mock_report(m, dj_online, sizeof(dj_online));
    mock_report(m, feature, sizeof(feature));
    mock_report(m, battery, sizeof(battery));
    mock_report(m, battery, sizeof(battery));
}

/*
 * Verify recovery from a mismatched record followed by a delayed valid reply.
 *
 * Replays the anonymized trace and supplies recovery records at 2100 ms.
 * Asserts an identical read retry, a connected device, restored retry policy,
 * and no repeated pairing write.
 */
static void test_pair_verification_retry(void) {
    struct mock m = {0};
    paired_trace(&m);
    size_t start = m.count;
    mock_snapshot(&m, 1, false);
    m.events[start].report.bytes[8] = 0x82;
    size_t metadata = m.count;
    mock_slot(&m, 1, true, true);
    m.events[metadata].report.bytes[8] = 0x82;
    mock_ack(&m, 0xb2);
    for (size_t i = start; i < m.count; i++) m.events[i].at = 2100;
    struct hidpp h = mock_session(&m);
    struct operation_ui ui = ui_open(false, false, NULL);
    struct error err = {0};
    assert(operation_add(&h, 30, &ui, &err) == UC_OK);
    assert(contains(ui.output, "New stored pairing"));
    assert(contains(ui.output, "4082") && contains(ui.output, "connected"));
    assert(!contains(ui.diagnostics, "verification failed"));
    assert(writes(&m, 0xb2, 1) == 1 && writes(&m, 0xb2, 2) == 1);
    assert(m.position == m.count && m.now == 2100);
    assert(!h.pending && !h.poisoned && !h.retry_pairing_reads);
    const uint8_t read_slot_one[] = {0x10, 0xff, 0x83, 0xb5, 0x20, 0, 0};
    assert(m.sent[8].length == sizeof(read_slot_one));
    assert(!memcmp(m.sent[8].bytes, read_slot_one, sizeof(read_slot_one)));
    assert(!memcmp(m.sent[8].bytes, m.sent[9].bytes, sizeof(read_slot_one)));
    ui_close(&ui);
}

/*
 * Verify bounded failure when no matching verification reply arrives.
 *
 * Provides no recovery reply after the trace and expects two reads to expire
 * at 4000 ms. Checks receiver-success diagnostics and avoids closing an
 * already closed window.
 */
static void test_pair_verification_exhausted(void) {
    struct mock m = {0};
    paired_trace(&m); /* No matching stored record ever arrives. */
    struct hidpp h = mock_session(&m);
    struct operation_ui ui = ui_open(false, false, NULL);
    struct error err = {0};
    assert(operation_add(&h, 30, &ui, &err) == UC_TIMEOUT);
    assert(!contains(ui.output, "New stored pairing"));
    assert(contains(ui.diagnostics, "receiver reported successful pairing"));
    assert(!contains(ui.diagnostics, "closure attempted"));
    assert(writes(&m, 0xb2, 1) == 1 && writes(&m, 0xb2, 2) == 0);
    assert(m.sends == 10 && m.now == 4000 && h.poisoned);
    assert(!h.retry_pairing_reads);
    ui_close(&ui);
}

/*
 * Verify removal confirmation and refusal paths.
 *
 * Covers declined, empty, uppercase, and noninteractive answers plus explicit
 * confirmation and --yes. Asserts that refused operations never send an unpair
 * command.
 */
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

/*
 * Verify removal aborts or reports failure when safety checks fail.
 *
 * Covers an empty slot, identity changes during confirmation, missing
 * acknowledgement, and verification I/O failure. Checks write counts and
 * uncertain-outcome diagnostics.
 */
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

/*
 * Run the test suite and report successful completion.
 *
 * Executes each assertion-based test in this suite. Returns zero after
 * printing the completion message; assertion failures abort the process.
 */
int main(void) {
    test_pair_success();
    test_pair_failures();
    test_pair_interruption();
    test_pair_unverified_and_cleanup_failure();
    test_pair_verification_retry();
    test_pair_verification_exhausted();
    test_remove_confirmation();
    test_remove_failures();
    puts("Operations: pairing, cancellation, cleanup, confirmation and removal tests passed");
    return 0;
}

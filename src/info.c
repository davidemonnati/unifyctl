#include "operations.h"
#include "peripheral.h"
#include "signals.h"

#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <unistd.h>

/* Poll input together with the process signal pipe. Using descriptor reads
 * avoids stdio's possible read-ahead/blocking after readiness. This operation
 * owns input for the duration of selection; there is no prompt deadline. */
static int select_slot(struct hidpp *h, const struct operation_ui *ui, unsigned *slot, struct error *err) {
    if (!ui->interactive) return fail(err, UC_REFUSED, 0, 0, "interactive selection unavailable; use info SLOT");
    if (fputs("Select a device slot (1–6): ", ui->diagnostics) < 0 || fflush(ui->diagnostics)) return fail(err, UC_IO, errno, 0, "cannot write selection prompt");
    char line[64];
    size_t length = 0;
    bool overlong = false, invalid = false;
    for (;;) {
        int cancelled = h->io.cancelled(h->io.context);
        if (!cancelled && signals_wake_fd() >= 0) cancelled = signals_pending();
        if (cancelled) return fail(err, (enum status)cancelled, 0, 0, "device selection cancelled");
        struct pollfd fds[] = {{fileno(ui->input), POLLIN, 0}, {signals_wake_fd(), POLLIN, 0}};
        if (fds[0].fd < 0) return fail(err, UC_IO, 0, 0, "selection input has no file descriptor");
        int ready = poll(fds, 2, -1);
        if (ready < 0) {
            if (errno == EINTR) continue;
            return fail(err, UC_IO, errno, 0, "cannot wait for device selection");
        }
        if (fds[1].revents) continue;
        if (fds[0].revents & (POLLERR | POLLNVAL)) return fail(err, UC_IO, 0, 0, "cannot read device selection");
        if (!(fds[0].revents & (POLLIN | POLLHUP))) continue;
        char byte;
        ssize_t count = read(fds[0].fd, &byte, 1);
        if (count < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            return fail(err, UC_IO, errno, 0, "cannot read device selection");
        }
        if (!count) return fail(err, UC_REFUSED, 0, 0, "device selection ended; use info SLOT");
        if (byte == '\n') break;
        if (!byte) invalid = true;
        if (length < sizeof(line) - 1) line[length++] = byte;
        else overlong = true;
    }
    if (overlong || invalid) return fail(err, UC_USAGE, 0, 0, "invalid device slot selection");
    line[length] = '\0';
    char *p = line;
    while (isspace((unsigned char)*p)) p++;
    unsigned value = 0;
    bool digits = false;
    while (*p >= '0' && *p <= '9') {
        digits = true;
        unsigned digit = (unsigned)(*p++ - '0');
        if (digit > SLOT_COUNT || value > (SLOT_COUNT - digit) / 10) return fail(err, UC_USAGE, 0, 0, "slot must be an integer from 1 to 6");
        value = value * 10 + digit;
    }
    while (isspace((unsigned char)*p)) p++;
    if (!digits || *p || !value) return fail(err, UC_USAGE, 0, 0, "slot must be an integer from 1 to 6");
    *slot = value;
    return UC_OK;
}

static void phase_begin(struct hidpp *h, int64_t outer_deadline) {
    h->deadline = h->io.now(h->io.context) + 30000;
    if (outer_deadline && h->deadline > outer_deadline) h->deadline = outer_deadline;
}

/* Each hardware phase has a separate budget. Restore the caller's deadline on
 * every path without resetting pending/poisoned state or enabling read retries. */
int operation_info(struct hidpp *h, unsigned slot, const struct operation_ui *ui, struct error *err) {
    int64_t outer_deadline = h->deadline;
    bool retries = h->retry_pairing_reads;
    h->retry_pairing_reads = false;
    struct device inventory[SLOT_COUNT], selected;
    int status = UC_OK;
    bool interactive = !slot;
    if (interactive) {
        phase_begin(h, outer_deadline);
        status = devices_read(h, inventory, true, err);
        h->deadline = outer_deadline;
        if (status) goto done;
        devices_print(ui->output, inventory);
        if (fflush(ui->output)) {
            status = fail(err, UC_IO, errno, 0, "cannot write device inventory");
            goto done;
        }
        bool occupied = false;
        for (unsigned n = 0; n < SLOT_COUNT; n++) occupied |= inventory[n].paired;
        if (!occupied) goto done;
        status = select_slot(h, ui, &slot, err);
        if (status) goto done;
        if (!inventory[slot - 1].paired) {
            status = fail(err, UC_USAGE, 0, 0, "selected slot %u was not in the displayed inventory", slot);
            goto done;
        }
    }
    phase_begin(h, outer_deadline);
    status = device_read(h, slot, &selected, true, err);
    if (status) goto done;
    if (interactive && !device_same(&selected, &inventory[slot - 1])) {
        status = fail(err, UC_PROTOCOL, 0, 0, "device identity changed; rerun info");
        goto done;
    }
    if (!selected.paired) {
        status = fail(err, UC_PROTOCOL, 0, 0, "slot %u has no stored pairing", slot);
        goto done;
    }
    struct peripheral_info info;
    status = peripheral_read(h, &selected, &info, err);
    if (!status) {
        int cancelled = h->io.cancelled(h->io.context);
        if (cancelled) status = fail(err, (enum status)cancelled, 0, 0, "information query cancelled");
        else {
            peripheral_print(ui->output, &selected, &info);
            if (fflush(ui->output)) status = fail(err, UC_IO, errno, 0, "cannot write device information");
        }
    }
done:
    h->deadline = outer_deadline;
    h->retry_pairing_reads = retries;
    return status;
}

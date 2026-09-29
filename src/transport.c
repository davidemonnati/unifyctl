#include "transport.h"
#include "signals.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <unistd.h>

static int64_t raw_now(void *context) {
    (void)context;
    return monotonic_ms();
}

static int raw_cancelled(void *context) {
    struct raw_transport *raw = context;
    if (raw->cleanup) return 0;
    return signals_pending();
}

static void raw_set_cleanup(void *context, bool cleanup) {
    ((struct raw_transport *)context)->cleanup = cleanup;
}

static int ready(struct raw_transport *raw, short events, int64_t deadline, struct error *err) {
    for (;;) {
        int cancelled = raw_cancelled(raw);
        if (cancelled) return fail(err, (enum status)cancelled, 0, 0, "operation cancelled");
        int64_t remaining = deadline - raw_now(raw);
        if (remaining <= 0) return fail(err, UC_TIMEOUT, 0, 0, "receiver I/O timed out");
        int timeout = remaining > INT_MAX ? INT_MAX : (int)remaining;
        struct pollfd fds[2] = {{raw->fd, events, 0}, {signals_wake_fd(), POLLIN, 0}};
        int result = poll(fds, 2, timeout);
        if (result < 0) {
            if (errno == EINTR) continue;
            return fail(err, UC_IO, errno, 0, "poll failed");
        }
        if (!result) continue;
        if (fds[1].revents & POLLIN) {
            signals_drain();
            if (!raw->cleanup) continue;
        }
        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) return fail(err, UC_IO, 0, 0, "receiver disconnected or descriptor unavailable");
        if (fds[0].revents & events) return UC_OK;
    }
}

static int raw_send(void *context, const uint8_t *bytes, size_t size, int64_t deadline, struct error *err) {
    struct raw_transport *raw = context;
    for (;;) {
        int status = ready(raw, POLLOUT, deadline, err);
        if (status) return status;
        ssize_t written = write(raw->fd, bytes, size);
        if (written < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return fail(err, UC_IO, errno, 0, "cannot write HID report");
        }
        /* Never continue a partial HID report or resend a state-changing one. */
        if ((size_t)written != size) return fail(err, UC_IO, 0, 0, "short HID report write; outcome uncertain");
        return UC_OK;
    }
}

static int raw_receive(void *context, uint8_t *bytes, size_t *size, int64_t deadline, struct error *err) {
    struct raw_transport *raw = context;
    for (;;) {
        int status = ready(raw, POLLIN, deadline, err);
        if (status) return status;
        ssize_t count = read(raw->fd, bytes, *size);
        if (count < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return fail(err, UC_IO, errno, 0, "cannot read HID report");
        }
        if (!count) return fail(err, UC_IO, 0, 0, "receiver disconnected");
        *size = (size_t)count;
        return UC_OK;
    }
}

static int nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return -1;
    return fcntl(fd, F_SETFD, FD_CLOEXEC);
}

int raw_init(struct raw_transport *raw, int fd, struct transport *transport, struct error *err) {
    *raw = (struct raw_transport){.fd = fd};
    if (nonblocking(fd) < 0) return fail(err, UC_IO, errno, 0, "cannot configure nonblocking I/O");
    int status = signals_install(err);
    if (status) return status;
    raw->signals = true;
    *transport = (struct transport){raw, raw_send, raw_receive, raw_now, raw_cancelled, raw_set_cleanup};
    return UC_OK;
}

void raw_close(struct raw_transport *raw) {
    if (raw->signals) signals_restore();
    if (raw->fd >= 0) close(raw->fd);
    raw->fd = -1;
    raw->signals = false;
}

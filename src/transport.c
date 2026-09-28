#include "transport.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* There is exactly one receiver session and one reader per process. */
static volatile sig_atomic_t received_signal;
static volatile sig_atomic_t signal_fd = -1;
static struct sigaction previous_int, previous_term;
static bool signals_installed;

static void on_signal(int sig) {
    int saved = errno;
    received_signal = sig;
    if (signal_fd >= 0) {
        uint8_t byte = 1;
        ssize_t ignored = write((int)signal_fd, &byte, 1);
        (void)ignored;
    }
    errno = saved;
}

static int64_t raw_now(void *context) {
    (void)context;
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int raw_cancelled(void *context) {
    struct raw_transport *raw = context;
    if (raw->cleanup || !received_signal) return 0;
    return received_signal == SIGINT ? UC_INTERRUPT : UC_TERMINATE;
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
        struct pollfd fds[2] = {{raw->fd, events, 0}, {raw->wake_read, POLLIN, 0}};
        int result = poll(fds, 2, timeout);
        if (result < 0) {
            if (errno == EINTR) continue;
            return fail(err, UC_IO, errno, 0, "poll failed");
        }
        if (!result) continue;
        if (fds[1].revents & POLLIN) {
            uint8_t bytes[64];
            while (read(raw->wake_read, bytes, sizeof(bytes)) > 0) {}
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
    *raw = (struct raw_transport){.fd = fd, .wake_read = -1, .wake_write = -1};
    int pipes[2];
    if (pipe(pipes) < 0) return fail(err, UC_IO, errno, 0, "cannot create signal pipe");
    raw->wake_read = pipes[0];
    raw->wake_write = pipes[1];
    if (nonblocking(pipes[0]) < 0 || nonblocking(pipes[1]) < 0 || nonblocking(fd) < 0) return fail(err, UC_IO, errno, 0, "cannot configure nonblocking I/O");
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    received_signal = 0;
    signal_fd = pipes[1];
    if (sigaction(SIGINT, &action, &previous_int) < 0) return fail(err, UC_IO, errno, 0, "cannot install SIGINT handler");
    if (sigaction(SIGTERM, &action, &previous_term) < 0) {
        int saved = errno;
        sigaction(SIGINT, &previous_int, NULL);
        return fail(err, UC_IO, saved, 0, "cannot install SIGTERM handler");
    }
    signals_installed = true;
    *transport = (struct transport){raw, raw_send, raw_receive, raw_now, raw_cancelled, raw_set_cleanup};
    return UC_OK;
}

void raw_close(struct raw_transport *raw) {
    signal_fd = -1;
    if (signals_installed) {
        sigaction(SIGINT, &previous_int, NULL);
        sigaction(SIGTERM, &previous_term, NULL);
        signals_installed = false;
    }
    if (raw->wake_read >= 0) close(raw->wake_read);
    if (raw->wake_write >= 0) close(raw->wake_write);
    if (raw->fd >= 0) close(raw->fd);
    raw->fd = raw->wake_read = raw->wake_write = -1;
}

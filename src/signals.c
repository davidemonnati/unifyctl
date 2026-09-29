#include "signals.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t received_signal;
static volatile sig_atomic_t signal_fd = -1;
static int wake_read = -1, wake_write = -1;
static struct sigaction previous_int, previous_term;
static bool handlers_installed;

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

static int nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return -1;
    return fcntl(fd, F_SETFD, FD_CLOEXEC);
}

int signals_install(struct error *err) {
    if (wake_read >= 0) return fail(err, UC_INTERNAL, 0, 0, "signal handling is already installed");
    int pipes[2];
    if (pipe(pipes) < 0) return fail(err, UC_IO, errno, 0, "cannot create signal pipe");
    wake_read = pipes[0];
    wake_write = pipes[1];
    if (nonblocking(wake_read) < 0 || nonblocking(wake_write) < 0) {
        int saved = errno;
        signals_restore();
        return fail(err, UC_IO, saved, 0, "cannot configure nonblocking I/O");
    }
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    received_signal = 0;
    signal_fd = wake_write;
    if (sigaction(SIGINT, &action, &previous_int) < 0) {
        int saved = errno;
        signals_restore();
        return fail(err, UC_IO, saved, 0, "cannot install SIGINT handler");
    }
    if (sigaction(SIGTERM, &action, &previous_term) < 0) {
        int saved = errno;
        sigaction(SIGINT, &previous_int, NULL);
        signals_restore();
        return fail(err, UC_IO, saved, 0, "cannot install SIGTERM handler");
    }
    handlers_installed = true;
    return UC_OK;
}

void signals_restore(void) {
    signal_fd = -1;
    if (handlers_installed) {
        sigaction(SIGINT, &previous_int, NULL);
        sigaction(SIGTERM, &previous_term, NULL);
        handlers_installed = false;
    }
    if (wake_read >= 0) close(wake_read);
    if (wake_write >= 0) close(wake_write);
    wake_read = wake_write = -1;
}

int signals_wake_fd(void) {
    return wake_read;
}

int signals_pending(void) {
    if (!received_signal) return 0;
    return received_signal == SIGINT ? UC_INTERRUPT : UC_TERMINATE;
}

void signals_drain(void) {
    uint8_t bytes[64];
    if (wake_read < 0) return;
    while (read(wake_read, bytes, sizeof(bytes)) > 0) {}
}

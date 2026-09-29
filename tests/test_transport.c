#include "signals.h"
#include "transport.h"

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void test_io(void) {
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_DGRAM, 0, sockets) == 0);
    struct raw_transport raw;
    struct transport io;
    struct error err = {0};
    assert(raw_init(&raw, sockets[0], &io, &err) == UC_OK);
    const uint8_t report[] = {0x10, 0xff, 0x81, 0, 0, 0, 0};
    uint8_t buffer[256];
    assert(io.send(io.context, report, sizeof(report), io.now(io.context) + 100, &err) == UC_OK);
    assert(read(sockets[1], buffer, sizeof(buffer)) == (ssize_t)sizeof(report));
    assert(!memcmp(buffer, report, sizeof(report)));
    assert(write(sockets[1], report, sizeof(report)) == (ssize_t)sizeof(report));
    size_t size = sizeof(buffer);
    assert(io.receive(io.context, buffer, &size, io.now(io.context) + 100, &err) == UC_OK);
    assert(size == sizeof(report) && !memcmp(buffer, report, size));
    size = sizeof(buffer);
    assert(io.receive(io.context, buffer, &size, io.now(io.context) + 10, &err) == UC_TIMEOUT);
    assert(io.send(io.context, report, sizeof(report), io.now(io.context), &err) == UC_TIMEOUT);

    assert(raise(SIGINT) == 0);
    assert(io.receive(io.context, buffer, &size, io.now(io.context) + 100, &err) == UC_INTERRUPT);
    io.set_cleanup(io.context, true);
    assert(io.send(io.context, report, sizeof(report), io.now(io.context) + 100, &err) == UC_OK);
    io.set_cleanup(io.context, false);
    assert(raise(SIGTERM) == 0);
    assert(io.send(io.context, report, sizeof(report), io.now(io.context) + 100, &err) == UC_TERMINATE);
    raw_close(&raw);
    close(sockets[1]);
}

static void test_disconnect_and_backpressure(void) {
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    struct raw_transport raw;
    struct transport io;
    struct error err = {0};
    assert(raw_init(&raw, sockets[0], &io, &err) == UC_OK);
    uint8_t bytes[4096] = {0};
    while (write(sockets[0], bytes, sizeof(bytes)) > 0) {}
    assert(errno == EAGAIN || errno == EWOULDBLOCK);
    assert(io.send(io.context, bytes, 7, io.now(io.context) + 10, &err) == UC_TIMEOUT);
    close(sockets[1]);
    size_t size = sizeof(bytes);
    assert(io.receive(io.context, bytes, &size, io.now(io.context) + 100, &err) == UC_IO);
    raw_close(&raw);
}

static void test_signal_lifecycle(void) {
    struct error err = {0};
    struct sigaction before_int, before_term, current;
    assert(sigaction(SIGINT, NULL, &before_int) == 0);
    assert(sigaction(SIGTERM, NULL, &before_term) == 0);
    assert(signals_install(&err) == UC_OK);
    assert(signals_wake_fd() >= 0 && signals_pending() == 0);
    assert(signals_install(&err) == UC_INTERNAL); /* One session per process. */
    assert(raise(SIGTERM) == 0);
    assert(signals_pending() == UC_TERMINATE);
    signals_drain();
    assert(signals_pending() == UC_TERMINATE); /* Draining only resets the wakeup. */
    signals_restore();
    assert(signals_wake_fd() == -1);
    assert(sigaction(SIGINT, NULL, &current) == 0 && current.sa_handler == before_int.sa_handler);
    assert(sigaction(SIGTERM, NULL, &current) == 0 && current.sa_handler == before_term.sa_handler);
    assert(signals_install(&err) == UC_OK && signals_pending() == 0);
    signals_restore();
}

int main(void) {
    test_io();
    test_disconnect_and_backpressure();
    test_signal_lifecycle();
    puts("Transport: report I/O, deadlines, cancellation, cleanup, backpressure, disconnect and signal lifecycle tests passed");
    return 0;
}

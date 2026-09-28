#ifndef UNIFYCTL_TRANSPORT_H
#define UNIFYCTL_TRANSPORT_H

#include "common.h"

/* Deadlines are absolute monotonic milliseconds. Receive returns one report. */
struct transport {
    void *context;
    int (*send)(void *, const uint8_t *, size_t, int64_t, struct error *);
    int (*receive)(void *, uint8_t *, size_t *, int64_t, struct error *);
    int64_t (*now)(void *);
    int (*cancelled)(void *);
    void (*set_cleanup)(void *, bool);
};

struct raw_transport {
    int fd;
    int wake_read;
    int wake_write;
    bool cleanup;
};

int raw_init(struct raw_transport *raw, int fd, struct transport *transport, struct error *err);
void raw_close(struct raw_transport *raw);

#endif

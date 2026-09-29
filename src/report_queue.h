#ifndef UNIFYCTL_REPORT_QUEUE_H
#define UNIFYCTL_REPORT_QUEUE_H

#include "common.h"

/* Bounded FIFO for transports whose OS delivers input reports through
 * callbacks (macOS IOKit). Reports are never dropped silently: a full queue,
 * an empty report or an oversized report latches `lost`, after which the
 * transport must fail rather than risk misattributing replies. */
enum { REPORT_QUEUE_DEPTH = 16, REPORT_QUEUE_BYTES = 256 };
struct report_queue {
    uint8_t bytes[REPORT_QUEUE_DEPTH][REPORT_QUEUE_BYTES];
    size_t lengths[REPORT_QUEUE_DEPTH];
    size_t head;
    size_t count;
    bool lost;
};

bool report_queue_push(struct report_queue *queue, const uint8_t *bytes, size_t length);
bool report_queue_empty(const struct report_queue *queue);
/* Copies the oldest report into bytes (capacity *size) and consumes it.
 * Fails without consuming it if the caller's buffer is too small. */
int report_queue_pop(struct report_queue *queue, uint8_t *bytes, size_t *size, struct error *err);

#endif

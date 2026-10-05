#include "report_queue.h"

#include <string.h>

/*
 * Append a report, latching loss if its size or queue capacity is invalid.
 *
 * Copies bytes into the bounded FIFO and returns true on success. Invalid
 * lengths or a full queue return false and latch lost without overwriting
 * queued reports.
 */
bool report_queue_push(struct report_queue *queue, const uint8_t *bytes, size_t length) {
    if (!length || length > REPORT_QUEUE_BYTES || queue->count == REPORT_QUEUE_DEPTH) {
        queue->lost = true;
        return false;
    }
    size_t tail = (queue->head + queue->count) % REPORT_QUEUE_DEPTH;
    memcpy(queue->bytes[tail], bytes, length);
    queue->lengths[tail] = length;
    queue->count++;
    return true;
}

/*
 * Return whether the report queue contains no entries.
 *
 * Checks count only and does not inspect the latched lost flag. Callers must
 * check loss separately before trusting queued input.
 */
bool report_queue_empty(const struct report_queue *queue) {
    return queue->count == 0;
}

/*
 * Copy and consume the oldest report only if the output buffer fits.
 *
 * Treats *size as capacity and replaces it with the copied length on success.
 * An empty queue or undersized buffer returns an error without consuming the
 * oldest report.
 */
int report_queue_pop(struct report_queue *queue, uint8_t *bytes, size_t *size, struct error *err) {
    if (!queue->count) return fail(err, UC_INTERNAL, 0, 0, "input report queue is empty");
    size_t length = queue->lengths[queue->head];
    if (length > *size) return fail(err, UC_PROTOCOL, 0, 0, "input report exceeds buffer: %zu bytes", length);
    memcpy(bytes, queue->bytes[queue->head], length);
    *size = length;
    queue->head = (queue->head + 1) % REPORT_QUEUE_DEPTH;
    queue->count--;
    return UC_OK;
}

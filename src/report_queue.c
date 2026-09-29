#include "report_queue.h"

#include <string.h>

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

bool report_queue_empty(const struct report_queue *queue) {
    return queue->count == 0;
}

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

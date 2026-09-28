#ifndef UNIFYCTL_MOCK_H
#define UNIFYCTL_MOCK_H

#include "hidpp.h"

enum { MOCK_MAX = 256 };
struct mock_event {
    struct report report;
    int status;
    int64_t at;
};
struct mock {
    struct mock_event events[MOCK_MAX];
    size_t count, position;
    struct report sent[MOCK_MAX];
    size_t sends;
    int64_t now;
    int cancel;
    int send_error;
    bool cleanup;
};

struct hidpp mock_session(struct mock *mock);
void mock_report(struct mock *mock, const uint8_t *bytes, size_t length);
void mock_error(struct mock *mock, uint8_t op, uint8_t reg, uint8_t code);
void mock_slot(struct mock *mock, unsigned slot, bool paired, bool metadata);
void mock_snapshot(struct mock *mock, unsigned occupied_mask, bool metadata);
void mock_ack(struct mock *mock, uint8_t reg);

#endif

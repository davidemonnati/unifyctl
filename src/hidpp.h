#ifndef UNIFYCTL_HIDPP_H
#define UNIFYCTL_HIDPP_H

#include "transport.h"

enum { HIDPP_SHORT = 7, HIDPP_LONG = 20, SLOT_COUNT = 6 };
struct report {
    uint8_t bytes[HIDPP_LONG];
    size_t length;
};
typedef void (*notification_fn)(void *, const struct report *);
struct hidpp {
    struct transport io;
    bool debug;
    bool poisoned;
    bool pending;
    bool retry_pairing_reads; /* One identical B5 read retry during verification only. */
    bool peripheral_unreachable; /* Last FAP query: explicit link-loss + RAP resource error. */
    int64_t deadline; /* Optional operation-wide limit, zero means none. */
    notification_fn notification;
    void *notification_context;
};

int hidpp_request(struct hidpp *h, uint8_t op, uint8_t reg,
                  const uint8_t params[3], int selector,
                  struct report *reply, struct error *err);
int hidpp_pump(struct hidpp *h, int64_t deadline, struct error *err);
/* Cleanup sends only: acknowledgements may be ambiguous after a timeout. */
int hidpp_cleanup_write(struct hidpp *h, uint8_t reg, const uint8_t params[3],
                        int64_t deadline, struct error *err);
const char *hidpp_error_name(uint8_t code);
/* Peripheral FAP queries: full header matching, no receiver notification dispatch
 * and no retries. Matched error replies remain in reply (8f RAP / ff FAP).
 * payload_size is the minimum successful response payload, excluding header. */
int hidpp_feature_request(struct hidpp *h, unsigned slot, uint8_t feature,
                          unsigned function, const uint8_t params[3],
                          size_t payload_size, struct report *reply, struct error *err);

#endif

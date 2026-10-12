#ifndef UNIFYCTL_DISCOVERY_H
#define UNIFYCTL_DISCOVERY_H

#include "transport.h"

enum { MAX_RECEIVERS = 64, RECEIVER_PATH_SIZE = 4096 };
struct receiver {
    char path[RECEIVER_PATH_SIZE];     /* User-visible --receiver value. */
    char physical[RECEIVER_PATH_SIZE]; /* Physical USB identity for deduplication. */
    uint16_t product;
};
struct receivers {
    struct receiver entries[MAX_RECEIVERS];
    size_t count;
};

/* Portable identification helpers (discovery.c). */
bool receiver_supported(unsigned vendor, unsigned product, unsigned interface);
int receiver_choose(const struct receivers *receivers, size_t *index, struct error *err);
bool receiver_descriptor(const uint8_t *bytes, size_t size);
/* macOS receiver IDs use hidapi's "DevSrvsID:<IORegistry entry ID>" form. */
bool receiver_registry_id(const char *text, uint64_t *id);

/* Platform backend (discovery_linux.c, discovery_macos.c, discovery_stub.c).
 * receiver_connect selects and validates one receiver, opens it and
 * initializes *io. On failure nothing remains open and *session is NULL.
 * On multiple candidates they are listed on diagnostics. */
struct receiver_session;
int receiver_connect(const char *explicit_path, FILE *diagnostics,
                     struct receiver_session **session, struct transport *io,
                     struct error *err);
/* Validated identity borrowed from a live session; no receiver I/O. */
const struct receiver *receiver_identity(const struct receiver_session *session);
void receiver_disconnect(struct receiver_session *session);

#endif

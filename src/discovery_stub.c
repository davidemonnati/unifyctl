#include "discovery.h"

struct receiver_session {
    int unused;
};

int receiver_connect(const char *explicit_path, FILE *diagnostics,
                     struct receiver_session **session, struct transport *io,
                     struct error *err) {
    (void)explicit_path;
    (void)diagnostics;
    (void)io;
    *session = NULL;
    return fail(err, UC_RECEIVER, 0, 0, "hardware access requires Linux or macOS; help and mock tests work on this platform");
}

void receiver_disconnect(struct receiver_session *session) {
    (void)session;
}

#include "discovery.h"

struct receiver_session {
    int unused;
};

/*
 * Report that receiver access is unsupported on this platform.
 *
 * Sets *session to NULL and returns UC_RECEIVER with a platform-support
 * diagnostic; no resources are acquired.
 */
int receiver_connect(const char *explicit_path, FILE *diagnostics,
                     struct receiver_session **session, struct transport *io,
                     struct error *err) {
    (void)explicit_path;
    (void)diagnostics;
    (void)io;
    *session = NULL;
    return fail(err, UC_RECEIVER, 0, 0, "hardware access requires Linux or macOS; help and mock tests work on this platform");
}

/*
 * Provide the no-op disconnect hook for unsupported platforms.
 *
 * Accepts the unused session pointer without taking ownership or accessing
 * hardware.
 */
void receiver_disconnect(struct receiver_session *session) {
    (void)session;
}

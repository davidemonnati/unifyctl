#include "discovery.h"

int receiver_open(const char *explicit_path, FILE *diagnostics, struct error *err) {
    (void)explicit_path;
    (void)diagnostics;
    fail(err, UC_RECEIVER, 0, 0, "hardware access requires Linux; help and mock tests work on this platform");
    return -1;
}

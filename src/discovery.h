#ifndef UNIFYCTL_DISCOVERY_H
#define UNIFYCTL_DISCOVERY_H

#include "common.h"

enum { MAX_RECEIVERS = 64, RECEIVER_PATH_SIZE = 4096 };
struct receiver {
    char path[RECEIVER_PATH_SIZE];
    char physical[RECEIVER_PATH_SIZE];
    uint16_t product;
};
struct receivers {
    struct receiver entries[MAX_RECEIVERS];
    size_t count;
};

bool receiver_supported(unsigned vendor, unsigned product, unsigned interface);
int receiver_choose(const struct receivers *receivers, size_t *index, struct error *err);
bool receiver_descriptor(const uint8_t *bytes, size_t size);
int receiver_open(const char *explicit_path, FILE *diagnostics, struct error *err);

#endif

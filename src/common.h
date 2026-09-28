#ifndef UNIFYCTL_COMMON_H
#define UNIFYCTL_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

enum status {
    UC_OK = 0, UC_INTERNAL = 1, UC_USAGE = 2, UC_RECEIVER = 3,
    UC_ACCESS = 4, UC_IO = 5, UC_TIMEOUT = 6, UC_PROTOCOL = 7,
    UC_REFUSED = 8, UC_INTERRUPT = 130, UC_TERMINATE = 143
};

struct error {
    enum status status;
    int system_errno;
    uint8_t protocol_code;
    char message[256];
};

int fail(struct error *err, enum status status, int system_errno,
         uint8_t protocol_code, const char *format, ...);
void print_error(FILE *out, const struct error *err);

#endif

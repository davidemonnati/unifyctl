#include "common.h"

#include <stdarg.h>
#include <string.h>

int fail(struct error *err, enum status status, int system_errno,
         uint8_t protocol_code, const char *format, ...) {
    va_list args;
    err->status = status;
    err->system_errno = system_errno;
    err->protocol_code = protocol_code;
    va_start(args, format);
    vsnprintf(err->message, sizeof(err->message), format, args);
    va_end(args);
    return (int)status;
}

void print_error(FILE *out, const struct error *err) {
    fprintf(out, "unifyctl: %s", err->message);
    if (err->system_errno) fprintf(out, ": %s", strerror(err->system_errno));
    if (err->protocol_code) fprintf(out, " (HID++ 0x%02x)", err->protocol_code);
    fputc('\n', out);
}

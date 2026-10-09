#include "export.h"

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Return the length of a valid UTF-8 scalar, or zero for an invalid byte.
 * Reject overlong encodings, surrogates, and values above U+10FFFF. */
static size_t utf8_length(const unsigned char *s, size_t remaining) {
    unsigned c = s[0];
    if (c < 0x80) return 1;
    size_t n = c >= 0xc2 && c <= 0xdf ? 2 :
               c >= 0xe0 && c <= 0xef ? 3 :
               c >= 0xf0 && c <= 0xf4 ? 4 : 0;
    if (!n || remaining < n) return 0;
    for (size_t i = 1; i < n; i++) {
        if ((s[i] & 0xc0) != 0x80) return 0;
    }
    if ((c == 0xe0 && s[1] < 0xa0) || (c == 0xed && s[1] >= 0xa0) ||
        (c == 0xf0 && s[1] < 0x90) || (c == 0xf4 && s[1] >= 0x90)) return 0;
    return n;
}

/* Emit valid JSON text. Invalid input bytes become U+FFFD individually.
 * Stream errors are checked by the caller before publishing the file. */
static bool json_string(FILE *out, const char *text) {
    const unsigned char *s = (const unsigned char *)text;
    size_t remaining = strlen(text);
    fputc('"', out);
    while (remaining) {
        size_t n = utf8_length(s, remaining);
        if (!n) {
            fputs("\\ufffd", out);
            n = 1;
        } else if (*s == '"' || *s == '\\') {
            fputc('\\', out);
            fputc(*s, out);
        } else if (*s < 0x20) fprintf(out, "\\u%04x", (unsigned)*s);
        else if (fwrite(s, 1, n, out) != n) return false;
        s += n;
        remaining -= n;
    }
    fputc('"', out);
    return !ferror(out);
}

static bool inventory_json(FILE *out, const struct receiver *identity,
                           const struct device devices[SLOT_COUNT]) {
    fputs("{\n  \"schema_version\": 1,\n  \"kind\": \"unifyctl-inventory\",\n"
          "  \"restorable_pairings\": false,\n  \"receiver\": {\n"
          "    \"family\": \"unifying\",\n    \"usb_vendor_id\": \"046d\",\n", out);
    fprintf(out, "    \"usb_product_id\": \"%04x\",\n    \"path\": ", (unsigned)identity->product);
    if (!json_string(out, identity->path)) return false;
    fputs(",\n    \"slot_capacity\": 6\n  },\n  \"devices\": [", out);
    bool first = true;
    for (unsigned i = 0; i < SLOT_COUNT; i++) {
        const struct device *d = &devices[i];
        if (!d->paired) continue;
        fputs(first ? "\n" : ",\n", out);
        first = false;
        fprintf(out, "    {\"slot\": %u, \"wpid\": \"%04x\", \"type\": %u, \"serial\": ",
                d->slot, (unsigned)d->wpid, (unsigned)d->type);
        if (d->has_serial) fprintf(out, "\"%08" PRIx32 "\"", d->serial);
        else fputs("null", out);
        fputs(", \"name\": ", out);
        if (d->name[0]) {
            if (!json_string(out, d->name)) return false;
        } else fputs("null", out);
        fputs(", \"connectivity\": \"unknown\"}", out);
    }
    fputs(first ? "]\n}\n" : "\n  ]\n}\n", out);
    return !ferror(out);
}

int operation_export(struct hidpp *h, const struct receiver *identity,
                     const char *path, struct error *err) {
    struct device devices[SLOT_COUNT];
    int status = devices_read(h, devices, true, err);
    if (status) return status;
    int cancelled = h->io.cancelled(h->io.context);
    if (cancelled) return fail(err, (enum status)cancelled, 0, 0, "export cancelled");

    /* Create beside the destination so link() publishes on the same filesystem.
     * mkstemp uses mode 0600. link() atomically refuses any existing destination,
     * including symlinks; rename() would silently replace one. */
    const char *slash = strrchr(path, '/');
    size_t prefix = slash ? (size_t)(slash - path) + 1 : 0;
    const char suffix[] = ".unifyctl-export-XXXXXX";
    char *temporary = malloc(prefix + sizeof(suffix));
    if (!temporary) return fail(err, UC_INTERNAL, ENOMEM, 0, "cannot allocate export path");
    memcpy(temporary, path, prefix);
    memcpy(temporary + prefix, suffix, sizeof(suffix));
    int fd = mkstemp(temporary);
    if (fd < 0) {
        int saved = errno;
        free(temporary);
        return fail(err, UC_IO, saved, 0, "cannot create export temporary file");
    }
    FILE *out = fdopen(fd, "w");
    if (!out) {
        status = fail(err, UC_IO, errno, 0, "cannot open export stream");
        close(fd);
    } else {
        errno = 0;
        if (!inventory_json(out, identity, devices) || fflush(out) == EOF) status = fail(err, UC_IO, errno ? errno : EIO, 0, "cannot write export file");
        if (!status && fsync(fd) < 0) status = fail(err, UC_IO, errno, 0, "cannot synchronize export file");
        if (fclose(out) == EOF && !status) status = fail(err, UC_IO, errno ? errno : EIO, 0, "cannot close export file");
    }
    cancelled = h->io.cancelled(h->io.context);
    if (!status && cancelled) status = fail(err, (enum status)cancelled, 0, 0, "export cancelled before publication");
    if (!status) {
        if (link(temporary, path) < 0) status = fail(err, UC_IO, errno, 0, "cannot publish export to %s; destination must not exist", path);
    }
    if (unlink(temporary) < 0) {
        if (!status) status = fail(err, UC_IO, errno, 0, "export published to %s, but temporary file remains: %s", path, temporary);
        else {
            /* Retain the original status/errno and make incomplete cleanup visible. */
            size_t used = strlen(err->message);
            snprintf(err->message + used, sizeof(err->message) - used, "; temporary cleanup failed: %s", temporary);
        }
    }
    free(temporary);
    return status;
}

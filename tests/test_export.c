#include "mock.h"
#include "export.h"

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Inject filesystem failures without adding hooks to the production API.
 * Always perform the real fclose so the failure case does not leak a stream. */
static int fault;
static struct mock *cancel_on_flush;
static int export_flush(FILE *out) {
    if (cancel_on_flush) cancel_on_flush->cancel = UC_INTERRUPT;
    if (fault == 1) {
        errno = ENOSPC;
        return EOF;
    }
    return fflush(out);
}
static int export_sync(int fd) {
    if (fault == 2) {
        errno = EIO;
        return -1;
    }
    return fsync(fd);
}
static int export_close(FILE *out) {
    int result = fclose(out);
    if (fault == 3) {
        errno = EIO;
        return EOF;
    }
    return result;
}
static size_t export_write(const void *data, size_t size, size_t count, FILE *out) {
    if (fault == 4) {
        errno = ENOSPC;
        return count ? fwrite(data, size, count - 1, out) : 0;
    }
    return fwrite(data, size, count, out);
}
#define fwrite export_write
#define fflush export_flush
#define fsync export_sync
#define fclose export_close
#include "../src/export.c"
#undef fwrite
#undef fflush
#undef fsync
#undef fclose

static const struct receiver identity = {.path = "test/receiver", .product = 0xc52b};

static int run(struct mock *m, const char *path, struct error *err) {
    struct hidpp h = mock_session(m);
    int status = operation_export(&h, &identity, path, err);
    /* Independent wire assertions: every outbound report must be a B5 GET. */
    for (size_t i = 0; i < m->sends; i++) {
        const struct report *r = &m->sent[i];
        uint8_t expected[7] = {0x10, 0xff, 0x83, 0xb5, r->bytes[4], 0, 0};
        assert(r->length == sizeof(expected));
        assert(!memcmp(r->bytes, expected, sizeof(expected)));
        unsigned selector = r->bytes[4];
        assert((selector >= 0x20 && selector <= 0x25) ||
               (selector >= 0x30 && selector <= 0x35) ||
               (selector >= 0x40 && selector <= 0x45));
    }
    return status;
}

static void success(unsigned mask, const char *path) {
    struct mock m = {0};
    struct error err = {0};
    mock_snapshot(&m, mask, true);
    assert(run(&m, path, &err) == UC_OK);
    size_t at = 0;
    for (unsigned slot = 0; slot < 6; slot++) {
        assert(m.sent[at++].bytes[4] == 0x20 + slot);
        if (mask & (1u << slot)) {
            assert(m.sent[at++].bytes[4] == 0x30 + slot);
            assert(m.sent[at++].bytes[4] == 0x40 + slot);
        }
    }
    assert(at == m.sends && m.position == m.count);
    struct stat st;
    assert(!stat(path, &st) && (st.st_mode & 0777) == 0600);
}

int main(int argc, char **argv) {
    assert(argc == 2 && !chdir(argv[1]));
    success(0, "empty.json");
    success(0x21, "sparse.json");
    success(0x3f, "full.json");
    struct mock m = {0};
    struct error err = {0};
    /* Optional records absent/unsupported; they do not hide transport errors. */
    mock_slot(&m, 1, true, false);
    mock_error(&m, 0x83, 0xb5, 0x02);
    mock_error(&m, 0x83, 0xb5, 0x03);
    for (unsigned slot = 2; slot <= 6; slot++) mock_slot(&m, slot, false, true);
    assert(run(&m, "optional.json", &err) == UC_OK);

    m = (struct mock){0};
    mock_snapshot(&m, 1, true);
    uint8_t *name = m.events[2].report.bytes;
    const uint8_t text[14] = {'"', '\\', '\n', 0xc3, 0xa9, 0xf0, 0x9f, 0x90, 0xad, 0xff, 0xed, 0xa0, 0x80, 'X'};
    name[5] = 14;
    memcpy(name + 6, text, sizeof(text));
    m.events[0].report.bytes[7] = 0xff;
    m.events[0].report.bytes[8] = 0xff;
    m.events[0].report.bytes[11] = 0xff;
    memset(m.events[1].report.bytes + 5, 0xff, 4);
    assert(run(&m, "encoding.json", &err) == UC_OK);

    /* Notification and wrong-selector reply must not replace the pending GET. */
    m = (struct mock){0};
    uint8_t notification[7] = {0x10, 1, 0x41, 4, 2, 1, 0x40};
    mock_report(&m, notification, sizeof(notification));
    mock_slot(&m, 2, true, false);
    mock_snapshot(&m, 1, true);
    assert(run(&m, "ordering.json", &err) == UC_OK);

    const int failures[] = {UC_TIMEOUT, UC_IO, UC_INTERRUPT, UC_TERMINATE};
    for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); i++) {
        m = (struct mock){0};
        mock_slot(&m, 1, true, true);
        m.events[m.count++].status = failures[i];
        assert(run(&m, "failed.json", &err) == failures[i]);
        assert(access("failed.json", F_OK) < 0);
    }
    m = (struct mock){0};
    mock_snapshot(&m, 1, true);
    m.events[2].report.bytes[5] = 15;
    assert(run(&m, "failed.json", &err) == UC_PROTOCOL);
    assert(access("failed.json", F_OK) < 0);
    m = (struct mock){0};
    mock_snapshot(&m, 1, true);
    m.events[1].report.length = 7;
    assert(run(&m, "failed.json", &err) == UC_PROTOCOL);
    m = (struct mock){0};
    mock_error(&m, 0x83, 0xb5, 0x02);
    assert(run(&m, "failed.json", &err) == UC_PROTOCOL);

    for (int i = 1; i <= 4; i++) {
        m = (struct mock){0};
        mock_snapshot(&m, 0x3f, true);
        fault = i;
        assert(run(&m, "failed.json", &err) == UC_IO);
        assert(access("failed.json", F_OK) < 0);
    }
    fault = 0;
    m = (struct mock){0};
    mock_snapshot(&m, 1, true);
    cancel_on_flush = &m;
    assert(run(&m, "failed.json", &err) == UC_INTERRUPT);
    assert(access("failed.json", F_OK) < 0);
    cancel_on_flush = NULL;

    /* Never overwrite regular files, symlinks, or directories. */
    assert(!symlink("empty.json", "symlink.json"));
    assert(!mkdir("directory", 0700));
    const char *paths[] = {"empty.json", "symlink.json", "directory", "missing/output.json"};
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        m = (struct mock){0};
        mock_snapshot(&m, 0x3f, true);
        assert(run(&m, paths[i], &err) == UC_IO);
    }
    puts("Export: inventory, read-only wire requests, failures, and publication checks passed");
    return 0;
}

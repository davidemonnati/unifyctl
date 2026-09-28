#include "cli.h"
#include "devices.h"
#include "discovery.h"
#include "operations.h"

#include <unistd.h>

int main(int argc, char **argv) {
    struct options options;
    struct error err = {0};
    int status = cli_parse(argc, argv, &options, &err);
    if (status) {
        print_error(stderr, &err);
        cli_help(stderr, options.command);
        return status;
    }
    if (options.help) {
        cli_help(stdout, options.command);
        return UC_OK;
    }
    int fd = receiver_open(options.receiver, stderr, &err);
    if (fd < 0) {
        print_error(stderr, &err);
        return (int)err.status;
    }
    struct raw_transport raw;
    struct hidpp h = {.debug = options.debug};
    struct operation_ui ui = {stdin, stdout, stderr,
                              isatty(STDIN_FILENO) && isatty(STDERR_FILENO), options.yes};
    status = raw_init(&raw, fd, &h.io, &err);
    if (!status && options.command == CMD_LIST) {
        struct device devices[SLOT_COUNT];
        status = devices_read(&h, devices, true, &err);
        if (!status) devices_print(stdout, devices);
    } else if (!status && options.command == CMD_ADD) status = operation_add(&h, options.timeout, &ui, &err);
    else if (!status && options.command == CMD_REMOVE) status = operation_remove(&h, options.slot, &ui, &err);
    raw_close(&raw);
    if (status) print_error(stderr, &err);
    return status;
}

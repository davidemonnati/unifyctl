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
    struct receiver_session *session = NULL;
    struct hidpp h = {.debug = options.debug};
    status = receiver_connect(options.receiver, stderr, &session, &h.io, &err);
    if (status) {
        print_error(stderr, &err);
        return status;
    }
    struct operation_ui ui = {stdin, stdout, stderr,
                              isatty(STDIN_FILENO) && isatty(STDERR_FILENO), options.yes};
    if (options.command == CMD_LIST) {
        struct device devices[SLOT_COUNT];
        status = devices_read(&h, devices, true, &err);
        if (!status) devices_print(stdout, devices);
    } else if (options.command == CMD_ADD) status = operation_add(&h, options.timeout, &ui, &err);
    else if (options.command == CMD_REMOVE) status = options.all
        ? operation_remove_all(&h, &ui, &err)
        : operation_remove(&h, options.slot, &ui, &err);
    receiver_disconnect(session);
    if (status) print_error(stderr, &err);
    return status;
}

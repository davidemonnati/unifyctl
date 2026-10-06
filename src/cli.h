#ifndef UNIFYCTL_CLI_H
#define UNIFYCTL_CLI_H

#include "common.h"

enum command { CMD_NONE, CMD_HELP, CMD_LIST, CMD_ADD, CMD_REMOVE };
struct options {
    enum command command;
    const char *receiver;
    bool help;
    bool debug;
    bool yes;
    bool all; /* remove --all; mutually exclusive with slot. */
    unsigned timeout;
    unsigned slot;
};

int cli_parse(int argc, char **argv, struct options *options, struct error *err);
void cli_help(FILE *out, enum command command);
/* Prompt for one pairing or, when all is true, all previously listed pairings.
 * The caller establishes terminal availability through interactive. Accept only
 * "y\n" or "yes\n"; return UC_REFUSED with err for noninteractive input, EOF,
 * or any other answer. This function never changes receiver state. */
int cli_confirm(FILE *in, FILE *out, bool interactive, bool all, struct error *err);

#endif

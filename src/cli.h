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
    unsigned timeout;
    unsigned slot;
};

int cli_parse(int argc, char **argv, struct options *options, struct error *err);
void cli_help(FILE *out, enum command command);
int cli_confirm(FILE *in, FILE *out, bool interactive, struct error *err);

#endif

#include "cli.h"

#include <string.h>

/*
 * Parse a positive decimal integer without exceeding the given limit.
 *
 * Writes value only on success; rejects zero, nondecimal characters, and
 * values above max.
 */
static bool number(const char *text, unsigned max, unsigned *value) {
    unsigned n = 0;
    if (!*text) return false;
    for (; *text; text++) {
        if (*text < '0' || *text > '9') return false;
        unsigned digit = (unsigned)(*text - '0');
        if (digit > max || n > (max - digit) / 10) return false;
        n = n * 10 + digit;
    }
    if (n == 0 || n > max) return false;
    *value = n;
    return true;
}

/*
 * Parse arguments and reject invalid command and option combinations.
 *
 * Initializes options with a 30-second timeout. Returns UC_USAGE with an
 * explanation for invalid input; receiver paths borrow storage from argv.
 */
int cli_parse(int argc, char **argv, struct options *o, struct error *err) {
    *o = (struct options){.timeout = 30};
    bool timeout_seen = false;
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (!strcmp(arg, "--help")) o->help = true;
        else if (!strcmp(arg, "--debug")) o->debug = true;
        else if (!strcmp(arg, "--yes")) o->yes = true;
        else if (!strcmp(arg, "--all")) o->all = true;
        else if (!strcmp(arg, "--receiver")) {
            if (o->receiver || ++i == argc || !*argv[i] || argv[i][0] == '-') return fail(err, UC_USAGE, 0, 0, "--receiver requires one path");
            o->receiver = argv[i];
        } else if (!strcmp(arg, "--timeout")) {
            if (timeout_seen || ++i == argc || !number(argv[i], 255, &o->timeout)) return fail(err, UC_USAGE, 0, 0, "--timeout requires an integer from 1 to 255 seconds");
            timeout_seen = true;
        } else if (arg[0] == '-') return fail(err, UC_USAGE, 0, 0, "unknown option: %s", arg);
        else if (o->command == CMD_NONE) {
            if (!strcmp(arg, "help")) o->command = CMD_HELP;
            else if (!strcmp(arg, "list")) o->command = CMD_LIST;
            else if (!strcmp(arg, "add")) o->command = CMD_ADD;
            else if (!strcmp(arg, "remove")) o->command = CMD_REMOVE;
            else return fail(err, UC_USAGE, 0, 0, "unknown command: %s", arg);
        } else if (o->command == CMD_REMOVE && !o->slot) {
            if (!number(arg, 6, &o->slot)) return fail(err, UC_USAGE, 0, 0, "slot must be an integer from 1 to 6");
        } else return fail(err, UC_USAGE, 0, 0, "unexpected argument: %s", arg);
    }
    if (timeout_seen && o->command != CMD_ADD) return fail(err, UC_USAGE, 0, 0, "--timeout is only valid with add");
    if (o->yes && o->command != CMD_REMOVE) return fail(err, UC_USAGE, 0, 0, "--yes is only valid with remove");
    if (o->all && o->command != CMD_REMOVE) return fail(err, UC_USAGE, 0, 0, "--all is only valid with remove");
    if (o->all && o->slot) return fail(err, UC_USAGE, 0, 0, "remove accepts either a slot or --all, not both");
    if (o->command == CMD_HELP) o->help = true;
    if (o->command == CMD_NONE && !o->help) return fail(err, UC_USAGE, 0, 0, "a command is required");
    if (o->command == CMD_REMOVE && !o->slot && !o->all && !o->help) return fail(err, UC_USAGE, 0, 0, "remove requires a slot (1–6) or --all");
    return UC_OK;
}

/*
 * Print general or command-specific usage information.
 *
 * Writes to the supplied stream without accessing hardware. Unrecognized
 * command values select the general help text.
 */
void cli_help(FILE *out, enum command command) {
    fputs("unifyctl — manage classic Logitech Unifying receiver pairings\n\n", out);
    switch (command) {
    case CMD_LIST:
        fputs("Usage: unifyctl [OPTIONS] list\n\n"
              "List all stored paired devices, including sleeping or powered-off devices.\n"
              "Columns: SLOT NAME TYPE WPID SERIAL STATUS. Missing data is unknown.\n"
              "No arguments or command-specific flags. Does not change receiver state.\n"
              "Example: unifyctl --receiver /dev/hidraw2 list\n"
              "         unifyctl --receiver DevSrvsID:4294968397 list   (macOS)\n", out);
        break;
    case CMD_ADD:
        fputs("Usage: unifyctl [OPTIONS] add [--timeout SECONDS]\n\n"
              "Open a pairing window and wait for a newly stored device.\n"
              "  --timeout SECONDS  Pairing window: 1–255 seconds, default 30.\n"
              "Activate a Unifying-compatible peripheral's pairing mode when prompted.\n"
              "Ctrl-C cancels and attempts to close the window.\n"
              "Example: unifyctl add --timeout 30\n", out);
        break;
    case CMD_REMOVE:
        fputs("Usage: unifyctl [OPTIONS] remove (SLOT | --all) [--yes]\n\n"
              "Remove stored pairings; devices disconnect from this receiver.\n"
              "  SLOT   Receiver slot, integer 1–6; mutually exclusive with --all.\n"
              "  --all  Remove all stored pairings on the selected receiver.\n"
              "  --yes  Skip confirmation for intentional noninteractive use.\n"
              "Default: require interactive confirmation; otherwise refuse.\n"
              "With --all, confirm once; stop on the first failure. Empty receivers succeed.\n"
              "Examples: unifyctl remove 2\n"
              "          unifyctl remove --all\n"
              "          unifyctl --receiver /dev/hidraw2 remove 2 --yes\n", out);
        break;
    default:
        fputs("Usage: unifyctl [OPTIONS] COMMAND [ARGUMENTS]\n\n"
              "Commands:\n"
              "  list                 List stored pairings (including offline devices).\n"
              "  add [--timeout N]    Pair a device; default 30 seconds, range 1–255.\n"
              "  remove SLOT [--yes]  Unpair slot 1–6; confirms unless --yes is given.\n"
              "  remove --all [--yes] Unpair all devices on the selected receiver.\n"
              "  help                 Show this help.\n\n"
              "Examples: unifyctl list\n"
              "          unifyctl add --timeout 30\n"
              "          unifyctl remove 2 --yes\n"
              "          unifyctl add --help\n", out);
        break;
    }
    fputs("\nGlobal options (before or after COMMAND):\n"
          "  --receiver PATH  Select the supported receiver's management interface:\n"
          "                   Linux: hidraw node, e.g. /dev/hidraw2\n"
          "                   macOS: IOKit ID, e.g. DevSrvsID:4294968397\n"
          "                   Default: auto-select only if exactly one is found.\n"
          "                   Multiple receivers require explicit selection.\n"
          "  --debug          Hexadecimal HID++ logging to stderr (default off).\n"
          "  --help           Global or command-specific help; no hardware needed.\n\n"
          "Supported: classic Unifying 046d:c52b/c532 on Linux (hidraw + libudev)\n"
          "and macOS (IOKit HID). Bolt, Lightspeed, and other receiver families\n"
          "are not supported.\n"
          "Exit: 0 success; 1 internal; 2 usage; 3 selection; 4 access; 5 I/O;\n"
          "      6 timeout; 7 protocol/operation; 8 confirmation refused;\n"
          "      130 interrupted; 143 terminated.\n", out);
}

/*
 * Require an explicit interactive confirmation before removal.
 *
 * Accepts only lowercase y or yes followed by a newline. Noninteractive input,
 * EOF, and all other answers return UC_REFUSED.
 */
int cli_confirm(FILE *in, FILE *out, bool interactive, bool all, struct error *err) {
    char line[16];
    if (!interactive) return fail(err, UC_REFUSED, 0, 0, "interactive confirmation unavailable; use --yes intentionally");
    fputs(all ? "Remove all listed pairings? [y/N] " : "Remove this pairing? [y/N] ", out);
    fflush(out);
    if (!fgets(line, sizeof(line), in)) return fail(err, UC_REFUSED, 0, 0, "removal declined");
    if (!strcmp(line, "y\n") || !strcmp(line, "yes\n")) return UC_OK;
    return fail(err, UC_REFUSED, 0, 0, "removal declined");
}

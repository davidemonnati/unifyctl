#ifndef UNIFYCTL_OPERATIONS_H
#define UNIFYCTL_OPERATIONS_H

#include "devices.h"

struct operation_ui {
    FILE *input;
    FILE *output;
    FILE *diagnostics;
    bool interactive;
    bool yes;
};

int operation_add(struct hidpp *h, unsigned timeout, const struct operation_ui *ui, struct error *err);
int operation_remove(struct hidpp *h, unsigned slot, const struct operation_ui *ui, struct error *err);
int operation_remove_all(struct hidpp *h, const struct operation_ui *ui, struct error *err);

#endif

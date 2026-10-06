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
/* Display and confirm a stored pairing unless ui->yes is set, then recheck
 * identity and unpair once. Return UC_OK only after verifying the slot is empty;
 * an initially empty slot is UC_PROTOCOL. Failures populate err. */
int operation_remove(struct hidpp *h, unsigned slot, const struct operation_ui *ui, struct error *err);
/* Snapshot all stored pairings on h's receiver, display them, and confirm once
 * unless ui->yes is set. Remove occupied slots in ascending order using the same
 * identity and empty-slot checks as operation_remove. An empty snapshot succeeds
 * without confirmation; newly paired devices outside the snapshot are excluded.
 * Return the first failure in err and stop, reporting verified progress through
 * ui. Earlier removals are not rolled back; UC_OK means every listed removal was
 * verified, not that the receiver is still empty under concurrent changes. */
int operation_remove_all(struct hidpp *h, const struct operation_ui *ui, struct error *err);

#endif

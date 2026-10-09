#ifndef UNIFYCTL_EXPORT_H
#define UNIFYCTL_EXPORT_H

#include "devices.h"
#include "discovery.h"

/* Read all stored slots, then publish a version-1 JSON inventory at path.
 * The destination must not exist. No receiver state is changed. Identity is
 * borrowed from the validated session. Failures populate err; after publication
 * a temporary-file cleanup error explicitly reports that the output exists. */
int operation_export(struct hidpp *h, const struct receiver *identity,
                     const char *path, struct error *err);

#endif

#ifndef UNIFYCTL_SIGNALS_H
#define UNIFYCTL_SIGNALS_H

#include "common.h"

/* Process-wide SIGINT/SIGTERM capture shared by all report transports.
 * Handlers only record the signal and write one byte to a nonblocking
 * self-pipe; transports wait on signals_wake_fd() and check
 * signals_pending() in ordinary control flow. There is exactly one receiver
 * session per process, so at most one installation is active. */
int signals_install(struct error *err);
void signals_restore(void);
int signals_wake_fd(void);
/* Returns 0, UC_INTERRUPT (SIGINT) or UC_TERMINATE (SIGTERM). */
int signals_pending(void);
void signals_drain(void);

#endif

/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_UPDATE_H
#define SNAJPAGENT_UPDATE_H

#include "wake.h"
#include <stdbool.h>

struct snag_update;
/* A single launch attempt. NULL means unavailable; ordinary work continues. */
struct snag_update *snag_update_start(const char *program, const char *url,
                                      snag_wake_fd wake);
/* Caller-owned UI thread: returns the success banner only once. */
const char *snag_update_take(struct snag_update *update);
/* Cancel, join and free; caller frees any final, previously unseen banner. */
char *snag_update_stop(struct snag_update *update);

#endif

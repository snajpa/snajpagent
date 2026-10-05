/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_TMUX_H
#define SNAJPAGENT_TMUX_H

#include "base.h"

/* Protocol output uses the sole writable client displaying this pane. Normal
 * UI output stays in the pane. The caller owns the returned nonblocking fd.
 * Optional terminal receives SNAG_TERMINAL_NAME_BYTES bytes for later refresh. */
int snag_tmux_output_open(const struct snag_terminal_profile *, char *terminal);
void snag_tmux_refresh(const struct snag_terminal_profile *, const char *terminal);

#endif

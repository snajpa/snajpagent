/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_RULES_COMMAND_H
#define SNAJPAGENT_RULES_COMMAND_H

#include "config.h"
#include "json.h"

#include <stddef.h>

/* Run a trusted host-user helper with the canonical envelope on stdin.
 * A trusted helper is not a sandbox. Returns 0 with *reply unset for an empty
 * successful stdout (pass), 0 with *reply owned for one strict JSON effect, or
 * -1 with *error set on any failure: spawn, nonzero exit, signal, timeout,
 * oversized stdout/stderr or invalid/oversized JSON. */
int snag_rule_command_run(const struct snag_config *config, const char *command,
                          const char *workdir, unsigned int timeout_ms,
                          const char *envelope, size_t envelope_len,
                          json_t **reply, char *error, size_t error_size);

#endif

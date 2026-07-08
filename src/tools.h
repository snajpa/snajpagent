/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_TOOLS_H
#define SNAJPAGENT_TOOLS_H

#include "config.h"
#include "credential.h"
#include "turn.h"

#include "snj_jansson.h"
#include <stdbool.h>
#include <stddef.h>

/* Return 0 for a completed tool result, 2 for user cancellation with *result set,
 * and -1 only when the host adapter itself failed before a factual result could
 * be constructed. */
typedef int (*snj_tool_pump_fn)(void *opaque, unsigned int timeout_ms);

int snj_tools_run(const struct snj_response_item *call,
                  const struct snj_config *config,
                  const struct snj_credential *credential,
                  const char *session_workspace,
                  snj_tool_pump_fn pump, void *pump_opaque,
                  json_t **result,
                  char *error, size_t error_size);

int snj_tools_close_managed(const char *handle, bool user_interrupt,
                            snj_tool_pump_fn pump, void *pump_opaque,
                            json_t **result,
                            char *error, size_t error_size);

#endif

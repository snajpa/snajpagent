/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_TOOLS_H
#define SNAJPAGENT_TOOLS_H

#include "config.h"
#include "credential.h"
#include "turn.h"

#include "snag_jansson.h"
#include <stdbool.h>
#include <stddef.h>

/* Return 0 for a completed tool result, 2 for user cancellation with *result set,
 * and -1 only when the host adapter itself failed before a factual result could
 * be constructed. */
typedef int (*snag_tool_pump_fn)(void *opaque, unsigned int timeout_ms);

int snag_tools_read_only(const struct snag_response_item *call,
                        const char *workspace, snag_tool_pump_fn pump,
                        void *opaque, json_t **result);

int snag_tools_run(const struct snag_response_item *call,
                  const struct snag_config *config,
                  const struct snag_credential *credential,
                  const char *session_workspace,
                  snag_tool_pump_fn pump, void *pump_opaque, int wake_fd,
                  json_t **result,
                  char *error, size_t error_size);

int snag_tools_attach_output_limit(const struct snag_response_item *call,
                                  const struct snag_config *config,
                                  json_t *result);

int snag_tools_close_managed(const char *handle, bool user_interrupt,
                            snag_tool_pump_fn pump, void *pump_opaque, int wake_fd,
                            json_t **result,
                            char *error, size_t error_size);

#endif

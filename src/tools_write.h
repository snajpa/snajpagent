/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_TOOLS_WRITE_H
#define SNAJPAGENT_TOOLS_WRITE_H

#include "turn.h"

#include "snag_jansson.h"
#include <stddef.h>

/* Whole-file create or replace (workspace-relative, atomic). */
int snag_tools_write_file(const struct snag_response_item *call, const char *session_workspace,
                          json_t **result, char *error, size_t error_size);

/* Targeted exact replacement in one file; ambiguous or missing matches fail
 * without changing the file. */
int snag_tools_edit_file(const struct snag_response_item *call, const char *session_workspace,
                         json_t **result, char *error, size_t error_size);

#endif

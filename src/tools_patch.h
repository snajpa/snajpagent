/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_TOOLS_PATCH_H
#define SNAJPAGENT_TOOLS_PATCH_H

#include "turn.h"

#include "snag_jansson.h"
#include <stddef.h>

int snag_tools_apply_patch(const struct snag_response_item *call,
                          const char *session_workspace,
                          json_t **result,
                          char *error, size_t error_size);

#endif

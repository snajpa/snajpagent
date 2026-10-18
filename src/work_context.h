/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_WORK_CONTEXT_H
#define SNAJPAGENT_WORK_CONTEXT_H

#include "snag_jansson.h"
#include <stdbool.h>

/* A single temporary visit; entering another project or harness keeps the original return point. */
bool snag_work_context_valid(const json_t *visit);

#endif

/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_HISTORY_VIEW_H
#define SNAJPAGENT_HISTORY_VIEW_H

#include "snag_jansson.h"
#include "wire.h"

#include <stdint.h>

/* Owned JSON text with private provider state omitted and secrets redacted. */
char *snag_history_event_data(uint64_t, const char *, const json_t *,
    const struct snag_wire_secrets *, char *, size_t);

#endif

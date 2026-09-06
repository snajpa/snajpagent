/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_TEST_CHECKED_JSON_H
#define SNAJPAGENT_TEST_CHECKED_JSON_H

#include "snag_jansson.h"
#include <assert.h>

static inline json_t *
checked_json(json_t *value)
{
    assert(value);
    return value;
}

#endif

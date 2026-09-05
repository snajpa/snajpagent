/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_SECRET_H
#define SNAJPAGENT_SECRET_H

#include "config.h"
#include "credential.h"
#include "wire.h"

#define SNAG_SECRET_VALUES_MAX \
    (1u + SNAG_CONFIG_PROVIDER_MAX + SNAG_CONFIG_SECRET_ENV_MAX)

_Static_assert(SNAG_SECRET_VALUES_MAX <= SNAG_WIRE_SECRET_COUNT_MAX,
               "wire secret limit must cover configured secrets");

struct snag_secret_set {
    const char *values[SNAG_SECRET_VALUES_MAX];
    struct snag_wire_secrets wire;
};

void snag_secret_set_build(struct snag_secret_set *set,
                          const struct snag_config *config,
                          const struct snag_credential *credential);

#endif

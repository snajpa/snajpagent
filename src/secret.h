/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_SECRET_H
#define SNAJPAGENT_SECRET_H

#include "config.h"
#include "credential.h"
#include "wire.h"

#define SNJ_SECRET_VALUES_MAX \
    (1u + SNJ_CONFIG_PROVIDER_MAX + SNJ_CONFIG_SECRET_ENV_MAX)

_Static_assert(SNJ_SECRET_VALUES_MAX <= SNJ_WIRE_SECRET_COUNT_MAX,
               "wire secret limit must cover configured secrets");

struct snj_secret_set {
    const char *values[SNJ_SECRET_VALUES_MAX];
    struct snj_wire_secrets wire;
};

void snj_secret_set_build(struct snj_secret_set *set,
                          const struct snj_config *config,
                          const struct snj_credential *credential);

#endif

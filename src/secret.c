/* SPDX-License-Identifier: GPL-2.0-only */
#include "secret.h"

#include <stdlib.h>
#include <string.h>

void
snj_secret_set_build(struct snj_secret_set *set,
                     const struct snj_config *config,
                     const struct snj_credential *credential)
{
    size_t count = 0u;

    memset(set, 0, sizeof(*set));
    if (credential && credential->len)
        set->values[count++] = credential->value;
    if (config) {
        for (size_t i = 0; i < config->secret_env_count &&
             count < SNJ_SECRET_VALUES_MAX; ++i) {
            const char *value = getenv(config->secret_env[i]);
            size_t len;
            if (!value)
                continue;
            len = strnlen(value, SNJ_WIRE_SECRET_MAX + 1u);
            if (!len || len > SNJ_WIRE_SECRET_MAX)
                continue;
            set->values[count++] = value;
        }
    }
    set->wire.values = set->values;
    set->wire.count = count;
}

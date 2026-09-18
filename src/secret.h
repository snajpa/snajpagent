/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_SECRET_H
#define SNAJPAGENT_SECRET_H

#include "config.h"
#include "credential.h"
#include "wire.h"
#include "snag_jansson.h"

struct snag_secret_set {
    const char **values;
    size_t capacity;
    struct snag_wire_secrets wire;
};

/* Zero-initialize once; subsequent builds retain old values through rotation. */
int snag_secret_set_build(struct snag_secret_set *set, const struct snag_config *config,
                          const struct snag_credential *credential, char *error, size_t error_size);
void snag_secret_set_free(struct snag_secret_set *set);
int snag_secret_result(const struct snag_secret_set *set, json_t *result, char *error, size_t error_size);

#endif

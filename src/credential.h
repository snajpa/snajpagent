/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_CREDENTIAL_H
#define SNAJPAGENT_CREDENTIAL_H

#include <stddef.h>

#define SNJ_CREDENTIAL_MAX 4096u

struct snj_credential {
    char value[SNJ_CREDENTIAL_MAX + 1u];
    size_t len;
};

int snj_credential_read(struct snj_credential *credential,
                        const char *env_name,
                        char *error, size_t error_size);
void snj_credential_clear(struct snj_credential *credential);

#endif

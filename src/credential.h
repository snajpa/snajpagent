/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_CREDENTIAL_H
#define SNAJPAGENT_CREDENTIAL_H

#include <stddef.h>

#define SNJ_CREDENTIAL_MAX 16384u
#define SNJ_ACCOUNT_ID_MAX 128u

struct snj_credential {
    char value[SNJ_CREDENTIAL_MAX + 1u];
    size_t len;
    char account_id[SNJ_ACCOUNT_ID_MAX + 1u];
    /* Borrowed dotdir fd for provider-scoped reacquisition, never closed here. */
    int root_fd;
};

int snj_credential_read(struct snj_credential *credential,
                        const char *env_name,
                        char *error, size_t error_size);
void snj_credential_clear(struct snj_credential *credential);

#endif

/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_CREDENTIAL_H
#define SNAJPAGENT_CREDENTIAL_H

#include <stddef.h>
#include "secret_source.h"

#define SNAG_CREDENTIAL_MAX 16384u
#define SNAG_ACCOUNT_ID_MAX 128u

struct snag_credential {
    char value[SNAG_CREDENTIAL_MAX + 1u];
    size_t len;
    char account_id[SNAG_ACCOUNT_ID_MAX + 1u];
    /* Borrowed dotdir fd for provider-scoped reacquisition, never closed here. */
    int root_fd;
};

void snag_credential_clear(struct snag_credential *credential);
int snag_credential_resolve(struct snag_credential *credential,
                            const struct snag_secret_source *source,
                            char *error, size_t error_size);

#endif

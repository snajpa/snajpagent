/* SPDX-License-Identifier: GPL-2.0-only */
#include "credential.h"
#include "base.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void
snag_credential_clear(struct snag_credential *credential)
{
    volatile unsigned char *p;

    if (!credential)
        return;
    p = (volatile unsigned char *)credential;
    for (size_t i = 0; i < sizeof(*credential); ++i)
        p[i] = 0u;
    credential->root_fd = -1;
}

int
snag_credential_resolve(struct snag_credential *credential,
                       const struct snag_secret_source *source,
                       char *error, size_t error_size)
{
    char *value = NULL;
    int rc = -1;

    snag_credential_clear(credential);
    if (snag_secret_source_resolve(source, &value, error, error_size) < 0)
        return -1;
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p)
        if (*p < 0x21u || *p > 0x7eu) {
            snag_errorf(error, error_size, "API key must contain printable ASCII without whitespace");
            goto done;
        }
    credential->len = strlen(value);
    memcpy(credential->value, value, credential->len + 1u);
    rc = 0;
done:
    snag_secret_bytes_free(value);
    return rc;
}

int
snag_credential_read(struct snag_credential *credential,
                    const char *env_name,
                    char *error, size_t error_size)
{
    const char *effective_env = (env_name && *env_name) ? env_name :
                                "OPENAI_API_KEY";
    const char *value;
    size_t len;

    if (!credential) {
        errno = EINVAL;
        snag_errorf(error, error_size, "invalid credential destination");
        return -1;
    }
    snag_credential_clear(credential);
    value = getenv(effective_env);
    if (!value) {
        errno = ENOENT;
        (void)snprintf(error, error_size,
                       "%s is required for provider work", effective_env);
        return -1;
    }
    len = strnlen(value, SNAG_CREDENTIAL_MAX + 1u);
    if (!len || len > SNAG_CREDENTIAL_MAX) {
        errno = EINVAL;
        (void)snprintf(error, error_size,
                       "%s must contain 1..16384 printable ASCII bytes",
                       effective_env);
        return -1;
    }
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)value[i];
        if (c < 0x21u || c > 0x7eu) {
            errno = EINVAL;
            (void)snprintf(error, error_size,
                           "%s must contain no whitespace or control bytes",
                           effective_env);
            return -1;
        }
    }
    memcpy(credential->value, value, len);
    credential->value[len] = '\0';
    credential->len = len;
    return 0;
}

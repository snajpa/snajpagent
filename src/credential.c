/* SPDX-License-Identifier: GPL-2.0-only */
#include "credential.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
set_error(char *error, size_t size, const char *message)
{
    if (size)
        (void)snprintf(error, size, "%s", message);
}

void
snj_credential_clear(struct snj_credential *credential)
{
    volatile unsigned char *p;

    if (!credential)
        return;
    p = (volatile unsigned char *)credential;
    for (size_t i = 0; i < sizeof(*credential); ++i)
        p[i] = 0u;
}

int
snj_credential_read(struct snj_credential *credential,
                    const char *env_name,
                    char *error, size_t error_size)
{
    const char *effective_env = (env_name && *env_name) ? env_name :
                                "OPENAI_API_KEY";
    const char *value;
    size_t len;

    if (!credential) {
        errno = EINVAL;
        set_error(error, error_size, "invalid credential destination");
        return -1;
    }
    snj_credential_clear(credential);
    value = getenv(effective_env);
    if (!value) {
        errno = ENOENT;
        (void)snprintf(error, error_size,
                       "%s is required for provider work", effective_env);
        return -1;
    }
    len = strnlen(value, SNJ_CREDENTIAL_MAX + 1u);
    if (!len || len > SNJ_CREDENTIAL_MAX) {
        errno = EINVAL;
        (void)snprintf(error, error_size,
                       "%s must contain 1..4096 printable ASCII bytes",
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

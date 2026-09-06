/* SPDX-License-Identifier: GPL-2.0-only */
#include "credential.h"
#include "base.h"

#include <errno.h>
#include <string.h>

void
snag_credential_clear(struct snag_credential *credential)
{
    if (!credential)
        return;
    snag_secret_clear(credential, sizeof(*credential));
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
            errno = EINVAL;
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

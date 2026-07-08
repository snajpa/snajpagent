/* SPDX-License-Identifier: GPL-2.0-only */
#include "credential.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int
main(void)
{
    struct snj_credential credential;
    char error[256];
    char *large = malloc(SNJ_CREDENTIAL_MAX + 2u);

    assert(large);
    assert(unsetenv("OPENAI_API_KEY") == 0);
    assert(snj_credential_read(&credential, NULL, error, sizeof(error)) < 0);
    assert(errno == ENOENT);
    assert(credential.len == 0u);

    assert(setenv("OPENAI_API_KEY", "sk-test", 1) == 0);
    assert(snj_credential_read(&credential, NULL, error, sizeof(error)) == 0);
    assert(credential.len == 7u);
    assert(strcmp(credential.value, "sk-test") == 0);
    snj_credential_clear(&credential);
    for (size_t i = 0; i < sizeof(credential); ++i)
        assert(((unsigned char *)&credential)[i] == 0u);

    assert(setenv("OPENAI_API_KEY", "bad key", 1) == 0);
    assert(snj_credential_read(&credential, NULL, error, sizeof(error)) < 0);
    assert(errno == EINVAL);

    assert(setenv("CUSTOM_API_KEY", "custom-secret", 1) == 0);
    assert(snj_credential_read(&credential, "CUSTOM_API_KEY",
                               error, sizeof(error)) == 0);
    assert(credential.len == strlen("custom-secret"));
    assert(strcmp(credential.value, "custom-secret") == 0);
    snj_credential_clear(&credential);

    memset(large, 'x', SNJ_CREDENTIAL_MAX);
    large[SNJ_CREDENTIAL_MAX] = '\0';
    assert(setenv("OPENAI_API_KEY", large, 1) == 0);
    assert(snj_credential_read(&credential, NULL, error, sizeof(error)) == 0);
    snj_credential_clear(&credential);

    large[SNJ_CREDENTIAL_MAX] = 'x';
    large[SNJ_CREDENTIAL_MAX + 1u] = '\0';
    assert(setenv("OPENAI_API_KEY", large, 1) == 0);
    assert(snj_credential_read(&credential, NULL, error, sizeof(error)) < 0);
    assert(errno == EINVAL);

    free(large);
    assert(unsetenv("OPENAI_API_KEY") == 0);
    assert(unsetenv("CUSTOM_API_KEY") == 0);
    puts("test_credential: ok");
    return 0;
}

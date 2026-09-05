/* SPDX-License-Identifier: GPL-2.0-only */
#include "credential.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void
write_secret(const char *path, const void *value, size_t len)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    assert(fd >= 0);
    assert(write(fd, value, len) == (ssize_t)len);
    assert(close(fd) == 0);
}

static void
test_sources(void)
{
    char dir[] = "/tmp/snajpagent-secret-XXXXXX";
    char path[512], config[512], link[512], error[256] = {0};
    struct snag_secret_source source = {0};
    struct snag_credential credential;
    char *value = NULL;
    static const char *const invalid[] = {
        "", "${}", "${1BAD}", "${VALID:-other}", "${VALID}suffix",
        "\"unterminated", "\"secret\" trailing", "\"\"", "\"nul\\u0000byte\""
    };

    assert(mkdtemp(dir));
    (void)snprintf(path, sizeof(path), "%s/key file#1", dir);
    (void)snprintf(config, sizeof(config), "%s/config.ini", dir);
    (void)snprintf(link, sizeof(link), "%s/key-link", dir);
    assert(setenv("SNAG_SECRET_TEST", "env-key", 1) == 0);
    assert(snag_secret_source_parse(&source, "${SNAG_SECRET_TEST}", config, error, sizeof(error)) == 0);
    assert(source.kind == SNAG_SECRET_ENV);
    assert(snag_credential_resolve(&credential, &source, error, sizeof(error)) == 0);
    assert(strcmp(credential.value, "env-key") == 0);
    assert(credential.root_fd == -1);
    assert(unsetenv("SNAG_SECRET_TEST") == 0);
    assert(snag_credential_resolve(&credential, &source, error, sizeof(error)) < 0);
    assert(credential.len == 0u);

    assert(snag_secret_source_parse(&source, "\"${SNAG_SECRET_TEST}\"", config, error, sizeof(error)) == 0);
    assert(snag_credential_resolve(&credential, &source, error, sizeof(error)) == 0);
    assert(strcmp(credential.value, "${SNAG_SECRET_TEST}") == 0);
    assert(snag_secret_source_parse(&source, "\"line\\nvalue\"", config, error, sizeof(error)) == 0);
    assert(snag_secret_source_resolve(&source, &value, error, sizeof(error)) == 0);
    assert(strcmp(value, "line\nvalue") == 0);
    snag_secret_bytes_free(value);
    assert(snag_credential_resolve(&credential, &source, error, sizeof(error)) < 0);

    write_secret(path, "file-key\r\n", 10u);
    assert(snag_secret_source_parse(&source, "key file#1", config, error, sizeof(error)) == 0);
    assert(source.kind == SNAG_SECRET_FILE && strcmp(source.path, path) == 0);
    assert(snag_credential_resolve(&credential, &source, error, sizeof(error)) == 0);
    assert(strcmp(credential.value, "file-key") == 0);
    assert(symlink(path, link) == 0);
    assert(snag_secret_source_parse(&source, link, config, error, sizeof(error)) == 0);
    write_secret(path, "rotated-key\n", 12u);
    assert(snag_credential_resolve(&credential, &source, error, sizeof(error)) == 0);
    assert(strcmp(credential.value, "rotated-key") == 0);
    write_secret(path, "nul\0key", 7u);
    assert(snag_credential_resolve(&credential, &source, error, sizeof(error)) < 0);
    write_secret(path, "", 0u);
    assert(snag_credential_resolve(&credential, &source, error, sizeof(error)) < 0);
    assert(unlink(path) == 0);
    assert(mkfifo(path, 0600) == 0);
    assert(snag_credential_resolve(&credential, &source, error, sizeof(error)) < 0);
    assert(unlink(path) == 0);
    assert(snag_secret_source_parse(&source, dir, config, error, sizeof(error)) == 0);
    assert(snag_credential_resolve(&credential, &source, error, sizeof(error)) < 0);
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        assert(snag_secret_source_parse(&source, invalid[i], config, error, sizeof(error)) < 0);
        assert(!strstr(error, "unterminated") && !strstr(error, "nul\\u0000byte"));
    }
    assert(snag_secret_source_parse(&source, "SNAG_SECRET_TEST", config, error, sizeof(error)) == 0);
    assert(source.kind == SNAG_SECRET_FILE);
    snag_secret_source_free(&source);
    snag_credential_clear(&credential);
    assert(unlink(link) == 0);
    assert(rmdir(dir) == 0);
}

int
main(void)
{
    struct snag_credential credential;
    char error[256];
    char *large = malloc(SNAG_CREDENTIAL_MAX + 2u);

    assert(large);
    assert(unsetenv("OPENAI_API_KEY") == 0);
    assert(snag_credential_read(&credential, NULL, error, sizeof(error)) < 0);
    assert(errno == ENOENT);
    assert(credential.len == 0u);

    assert(setenv("OPENAI_API_KEY", "sk-test", 1) == 0);
    assert(snag_credential_read(&credential, NULL, error, sizeof(error)) == 0);
    assert(credential.len == 7u);
    assert(strcmp(credential.value, "sk-test") == 0);
    snag_credential_clear(&credential);
    for (size_t i = 0; i < sizeof(credential.value); ++i)
        assert(credential.value[i] == 0);
    assert(credential.len == 0u && credential.root_fd == -1);

    assert(setenv("OPENAI_API_KEY", "bad key", 1) == 0);
    assert(snag_credential_read(&credential, NULL, error, sizeof(error)) < 0);
    assert(errno == EINVAL);

    assert(setenv("CUSTOM_API_KEY", "custom-secret", 1) == 0);
    assert(snag_credential_read(&credential, "CUSTOM_API_KEY",
                               error, sizeof(error)) == 0);
    assert(credential.len == strlen("custom-secret"));
    assert(strcmp(credential.value, "custom-secret") == 0);
    snag_credential_clear(&credential);

    memset(large, 'x', SNAG_CREDENTIAL_MAX);
    large[SNAG_CREDENTIAL_MAX] = '\0';
    assert(setenv("OPENAI_API_KEY", large, 1) == 0);
    assert(snag_credential_read(&credential, NULL, error, sizeof(error)) == 0);
    snag_credential_clear(&credential);

    large[SNAG_CREDENTIAL_MAX] = 'x';
    large[SNAG_CREDENTIAL_MAX + 1u] = '\0';
    assert(setenv("OPENAI_API_KEY", large, 1) == 0);
    assert(snag_credential_read(&credential, NULL, error, sizeof(error)) < 0);
    assert(errno == EINVAL);

    free(large);
    assert(unsetenv("OPENAI_API_KEY") == 0);
    assert(unsetenv("CUSTOM_API_KEY") == 0);
    test_sources();
    puts("test_credential: ok");
    return 0;
}

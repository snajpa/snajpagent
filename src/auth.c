/* SPDX-License-Identifier: GPL-2.0-only */
#include "auth.h"
#include "base.h"
#include "json.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define AUTH_FILE_MAX (96u * 1024u)

void
snj_auth_clear(struct snj_auth_tokens *tokens)
{
    volatile unsigned char *p = (volatile unsigned char *)tokens;
    for (size_t i = 0; i < sizeof(*tokens); ++i)
        p[i] = 0;
}

void
snj_auth_json_free(json_t *value)
{
    if (json_is_object(value)) {
        for (void *iter = json_object_iter(value); iter;
             iter = json_object_iter_next(value, iter)) {
            json_t *item = json_object_iter_value(iter);
            if (json_is_string(item)) {
                volatile char *p = (volatile char *)json_string_value(item);
                for (size_t i = 0; i < json_string_length(item); ++i)
                    p[i] = 0;
            }
        }
    }
    json_decref(value);
}

const char *
snj_auth_kind_name(enum snj_auth_kind kind)
{
    switch (kind) {
    case SNJ_AUTH_ENV: return "env";
    case SNJ_AUTH_API_KEY: return "api_key";
    case SNJ_AUTH_CHATGPT: return "chatgpt";
    }
    return "invalid";
}

static bool
token_copy(char *out, size_t size, const char *value, bool empty)
{
    if (!value || (!empty && !*value) || strlen(value) >= size)
        return false;
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p)
        if (*p < 0x21u || *p > 0x7eu)
            return false;
    return snj_strcpy(out, size, value);
}

int
snj_auth_key(struct snj_auth_tokens *tokens, const char *key,
              char *error, size_t error_size)
{
    snj_auth_clear(tokens);
    if (!token_copy(tokens->credential.value,
                    sizeof(tokens->credential.value), key, false)) {
        snj_errorf(error, error_size, "API key must contain 1..16384 non-whitespace ASCII bytes");
        errno = EINVAL;
        return -1;
    }
    tokens->credential.len = strlen(key);
    return 0;
}

static int
private_fd(int fd, bool directory)
{
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_uid != geteuid() || (st.st_mode & 077u) ||
        (directory ? !S_ISDIR(st.st_mode) :
                     (!S_ISREG(st.st_mode) || st.st_nlink != 1u))) {
        errno = EACCES;
        return -1;
    }
    return 0;
}

static bool
provider_valid(const struct snj_provider_config *provider)
{
    if (!provider || !provider->name[0] ||
        (provider->auth != SNJ_AUTH_API_KEY && provider->auth != SNJ_AUTH_CHATGPT))
        return false;
    for (const unsigned char *p = (const unsigned char *)provider->name; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '.' || *p == '_' || *p == '-'))
            return false;
    return provider->auth != SNJ_AUTH_CHATGPT ||
           strcmp(provider->base_url, SNJ_CHATGPT_BASE) == 0;
}

static int
auth_dir(int root_fd, bool create)
{
    int fd;
    if (private_fd(root_fd, true) < 0)
        return -1;
    if (create) {
        if (mkdirat(root_fd, "auth", 0700) == 0) {
            if (snj_sync_dir(root_fd) < 0)
                return -1;
        } else if (errno != EEXIST) {
            return -1;
        }
    }
    fd = openat(root_fd, "auth", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd >= 0 && private_fd(fd, true) < 0) {
        (void)close(fd);
        return -1;
    }
    return fd;
}

static int
lock_provider(int dir, const char *name, snj_auth_pump_fn pump, void *opaque)
{
    char path[SNJ_CONFIG_PROVIDER_NAME_MAX + 8u];
    struct flock lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET};
    int fd;
    uint64_t deadline = snj_time_ms() + 30000u;

    (void)snprintf(path, sizeof(path), "%s.lock", name);
    fd = openat(dir, path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0)
        return -1;
    if (private_fd(fd, false) < 0)
        goto fail;
    while (fcntl(fd, F_SETLK, &lock) < 0) {
        if (errno != EACCES && errno != EAGAIN && errno != EINTR)
            goto fail;
        if (snj_time_ms() >= deadline) {
            errno = ETIMEDOUT;
            goto fail;
        }
        if (pump ? pump(opaque, 50u) != 0 : poll(NULL, 0, 50) < 0) {
            errno = ECANCELED;
            goto fail;
        }
    }
    return fd;
fail:
    (void)close(fd);
    return -1;
}

static int
read_tokens(int dir, const struct snj_provider_config *provider,
             struct snj_auth_tokens *tokens)
{
    static const char *const keys[] = {
        "kind", "base_url", "access_token", "refresh_token", "account_id", "expires_at_ms"
    };
    char path[SNJ_CONFIG_PROVIDER_NAME_MAX + 8u], error[128];
    struct snj_buf text;
    json_t *value = NULL;
    struct stat st;
    int fd, rc = -1;

    snj_auth_clear(tokens);
    (void)snprintf(path, sizeof(path), "%s.json", provider->name);
    fd = openat(dir, path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return errno == ENOENT ? 1 : -1;
    snj_buf_init(&text, AUTH_FILE_MAX);
    if (private_fd(fd, false) < 0 || fstat(fd, &st) < 0 ||
        st.st_size < 1 || (uint64_t)st.st_size > AUTH_FILE_MAX)
        goto out;
    for (;;) {
        char chunk[4096];
        ssize_t n = read(fd, chunk, sizeof(chunk));
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 || (n > 0 && snj_buf_append(&text, chunk, (size_t)n) < 0))
            goto out;
        if (!n)
            break;
    }
    value = snj_json_load_strict(text.data, text.len, AUTH_FILE_MAX,
                                error, sizeof(error));
    if (!snj_json_exact_keys(value, keys, sizeof(keys) / sizeof(keys[0])) ||
        strcmp(snj_json_string(value, "kind"), snj_auth_kind_name(provider->auth)) ||
        strcmp(snj_json_string(value, "base_url"), provider->base_url) ||
        !token_copy(tokens->credential.value, sizeof(tokens->credential.value),
                     snj_json_string(value, "access_token"), false) ||
        !token_copy(tokens->refresh_token, sizeof(tokens->refresh_token),
                     snj_json_string(value, "refresh_token"),
                     provider->auth != SNJ_AUTH_CHATGPT) ||
        !token_copy(tokens->credential.account_id,
                     sizeof(tokens->credential.account_id),
                     snj_json_string(value, "account_id"),
                     provider->auth != SNJ_AUTH_CHATGPT) ||
        snj_json_integer_u64(value, "expires_at_ms", &tokens->expires_at_ms) < 0 ||
        (provider->auth == SNJ_AUTH_CHATGPT && !tokens->expires_at_ms) ||
        (provider->auth == SNJ_AUTH_API_KEY && (tokens->expires_at_ms ||
            tokens->refresh_token[0] || tokens->credential.account_id[0])))
        goto out;
    tokens->credential.len = strlen(tokens->credential.value);
    rc = 0;
out:
    snj_auth_json_free(value);
    if (text.data)
        memset(text.data, 0, text.len);
    snj_buf_free(&text);
    (void)close(fd);
    if (rc < 0) {
        snj_auth_clear(tokens);
        errno = EACCES;
    }
    return rc;
}

static int
write_tokens(int dir, const struct snj_provider_config *provider,
              const struct snj_auth_tokens *tokens)
{
    char path[SNJ_CONFIG_PROVIDER_NAME_MAX + 8u];
    char temp[SNJ_ID_HEX_LEN + 8u], id[SNJ_ID_HEX_LEN + 1u];
    json_t *value = json_object();
    struct snj_buf text;
    int fd = -1, rc = -1;

    temp[0] = '\0';
    snj_buf_init(&text, AUTH_FILE_MAX);
    if (!value ||
        snj_json_set_new(value, "kind", json_string(snj_auth_kind_name(provider->auth))) < 0 ||
        snj_json_set_new(value, "base_url", json_string(provider->base_url)) < 0 ||
        snj_json_set_new(value, "access_token", json_string(tokens->credential.value)) < 0 ||
        snj_json_set_new(value, "refresh_token", json_string(tokens->refresh_token)) < 0 ||
        snj_json_set_new(value, "account_id", json_string(tokens->credential.account_id)) < 0 ||
        snj_json_set_new(value, "expires_at_ms", json_integer((json_int_t)tokens->expires_at_ms)) < 0 ||
        snj_json_canonical(value, &text) < 0 || snj_random_id(id) < 0)
        goto out;
    (void)snprintf(path, sizeof(path), "%s.json", provider->name);
    (void)snprintf(temp, sizeof(temp), "%s.tmp", id);
    fd = openat(dir, temp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0 || snj_write_full(fd, text.data, text.len) < 0 || fsync(fd) < 0 ||
        renameat(dir, temp, dir, path) < 0)
        goto out;
    temp[0] = '\0';
    if (fsync(dir) < 0)
        goto out;
    rc = 0;
out:
    if (fd >= 0)
        (void)close(fd);
    if (temp[0])
        (void)unlinkat(dir, temp, 0);
    snj_auth_json_free(value);
    if (text.data)
        memset(text.data, 0, text.len);
    snj_buf_free(&text);
    return rc;
}

int
snj_auth_load(int root_fd, const struct snj_provider_config *provider,
              struct snj_auth_tokens *tokens, char *error, size_t error_size)
{
    int dir, rc;
    snj_auth_clear(tokens);
    if (!provider_valid(provider)) {
        snj_errorf(error, error_size, "invalid stored credential provider");
        return -1;
    }
    dir = auth_dir(root_fd, false);
    if (dir < 0) {
        rc = errno == ENOENT ? 1 : -1;
    } else {
        rc = read_tokens(dir, provider, tokens);
        (void)close(dir);
    }
    if (rc != 0)
        snj_errorf(error, error_size, rc == 1 ?
            "provider %s is not logged in; use snajpagent login %s" :
            "provider %s credentials are unsafe, invalid, or bound to another endpoint; use snajpagent login %s",
            provider->name, provider->name);
    if (rc == 0)
        tokens->credential.root_fd = root_fd;
    return rc;
}

int
snj_auth_save(int root_fd, const struct snj_provider_config *provider,
              const struct snj_auth_tokens *tokens,
              struct snj_auth_tokens *previous,
              snj_auth_pump_fn pump, void *opaque, char *error, size_t error_size)
{
    struct snj_auth_tokens local_previous;
    int dir = -1, lock = -1, rc = -1;
    if (!provider_valid(provider) || !tokens->credential.len)
        goto out;
    dir = auth_dir(root_fd, true);
    if (dir < 0 || (lock = lock_provider(dir, provider->name, pump, opaque)) < 0)
        goto out;
    if (read_tokens(dir, provider, previous ? previous : &local_previous) < 0)
        goto out;
    rc = write_tokens(dir, provider, tokens);
out:
    snj_auth_clear(&local_previous);
    if (lock >= 0)
        (void)close(lock);
    if (dir >= 0)
        (void)close(dir);
    if (rc < 0)
        snj_errorf(error, error_size, "cannot save provider credentials safely");
    return rc;
}

int
snj_auth_restore(int root_fd, const struct snj_provider_config *provider,
                 const struct snj_auth_tokens *expected,
                 const struct snj_auth_tokens *previous,
                 char *error, size_t error_size)
{
    struct snj_auth_tokens current;
    char path[SNJ_CONFIG_PROVIDER_NAME_MAX + 8u];
    int dir = auth_dir(root_fd, false), lock = -1, rc = -1;
    if (dir < 0 || (lock = lock_provider(dir, provider->name, NULL, NULL)) < 0 ||
        read_tokens(dir, provider, &current) != 0)
        goto out;
    if (strcmp(current.credential.value, expected->credential.value) ||
        strcmp(current.refresh_token, expected->refresh_token) ||
        current.expires_at_ms != expected->expires_at_ms) {
        snj_errorf(error, error_size, "credentials changed concurrently; rollback left the newer login intact");
        goto out;
    }
    if (previous->credential.len) {
        rc = write_tokens(dir, provider, previous);
    } else {
        (void)snprintf(path, sizeof(path), "%s.json", provider->name);
        if (unlinkat(dir, path, 0) == 0)
            rc = fsync(dir);
    }
out:
    snj_auth_clear(&current);
    if (lock >= 0)
        (void)close(lock);
    if (dir >= 0)
        (void)close(dir);
    if (rc < 0 && !error[0])
        snj_errorf(error, error_size, "credential rollback failed; login state is retained for recovery");
    return rc;
}

int
snj_auth_logout(int root_fd, const struct snj_provider_config *provider,
                snj_auth_pump_fn pump, void *opaque, char *error, size_t error_size)
{
    char path[SNJ_CONFIG_PROVIDER_NAME_MAX + 8u];
    struct snj_auth_tokens tokens;
    int dir = -1, lock = -1, rc = -1;
    if (!provider_valid(provider))
        goto out;
    dir = auth_dir(root_fd, false);
    if (dir < 0) {
        if (errno == ENOENT)
            rc = 0;
        goto out;
    }
    lock = lock_provider(dir, provider->name, pump, opaque);
    if (lock < 0 || read_tokens(dir, provider, &tokens) < 0)
        goto out;
    (void)snprintf(path, sizeof(path), "%s.json", provider->name);
    if (unlinkat(dir, path, 0) < 0 && errno != ENOENT)
        goto out;
    rc = fsync(dir);
out:
    snj_auth_clear(&tokens);
    if (lock >= 0)
        (void)close(lock);
    if (dir >= 0)
        (void)close(dir);
    if (rc < 0)
        snj_errorf(error, error_size, "cannot remove provider credentials safely");
    return rc;
}

int
snj_auth_read(int root_fd, const struct snj_provider_config *provider,
              bool force, const char *stale, struct snj_credential *out,
              snj_auth_pump_fn pump, void *opaque, char *error, size_t error_size)
{
    struct snj_auth_tokens tokens;
    int dir = -1, lock = -1, rc;
    snj_credential_clear(out);
    if (provider->auth == SNJ_AUTH_ENV)
        return snj_credential_read(out, provider->api_key_env, error, error_size);
    rc = snj_auth_load(root_fd, provider, &tokens, error, error_size);
    if (rc != 0) {
        rc = -1;
        goto done;
    }
    if (provider->auth == SNJ_AUTH_CHATGPT &&
        (force || tokens.expires_at_ms <= snj_time_ms() + 60000u)) {
        rc = -1;
        dir = auth_dir(root_fd, false);
        if (dir < 0 || (lock = lock_provider(dir, provider->name, pump, opaque)) < 0 ||
            read_tokens(dir, provider, &tokens) != 0)
            goto done;
        if ((force && stale && strcmp(stale, tokens.credential.value) == 0) ||
            tokens.expires_at_ms <= snj_time_ms() + 60000u) {
            if (snj_auth_refresh(&tokens, pump, opaque, error, error_size) < 0 ||
                write_tokens(dir, provider, &tokens) < 0)
                goto done;
        }
        rc = 0;
    }
    *out = tokens.credential;
    out->root_fd = root_fd;
done:
    snj_auth_clear(&tokens);
    if (lock >= 0)
        (void)close(lock);
    if (dir >= 0)
        (void)close(dir);
    if (rc < 0 && !error[0])
        snj_errorf(error, error_size, "cannot acquire or refresh provider credentials");
    return rc;
}

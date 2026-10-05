/* SPDX-License-Identifier: GPL-2.0-only */
#include "auth.h"
#include "fs.h"
#include "base.h"
#include "json.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define AUTH_FILE_MAX (96u * 1024u)
#define AUTH_PATH_MAX (SNAG_CONFIG_PROVIDER_NAME_MAX + 32u)

struct snag_auth_state {
    pthread_mutex_t mutex;
    struct snag_auth_tokens tokens;
    int result;
    char error[256];
};

void
snag_auth_clear(struct snag_auth_tokens *tokens)
{
    snag_secret_clear(tokens, sizeof(*tokens));
    tokens->credential.root_fd = -1;
}

void
snag_auth_json_free(json_t *value)
{
    if (json_is_object(value)) {
        for (void *iter = json_object_iter(value); iter;
             iter = json_object_iter_next(value, iter)) {
            json_t *item = json_object_iter_value(iter);
            if (json_is_string(item))
                snag_secret_clear((void *)json_string_value(item), json_string_length(item));
        }
    }
    json_decref(value);
}

const char *
snag_auth_kind_name(enum snag_auth_kind kind)
{
    switch (kind) {
    case SNAG_AUTH_API_KEY: return "api_key";
    case SNAG_AUTH_CHATGPT: return "chatgpt";
    case SNAG_AUTH_CODEX_TOKEN: return "codex_token";
    case SNAG_AUTH_META: return "meta";
    }
    return "invalid";
}

static bool
token_copy(char *out, size_t size, const char *value, bool empty)
{
    if (!value || (!empty && !*value) || strlen(value) >= size) return false;
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p)
        if (*p < 0x21u || *p > 0x7eu) return false;
    return snag_strcpy(out, size, value);
}

int
snag_auth_key(struct snag_auth_tokens *tokens, const char *key, char *error, size_t error_size)
{
    snag_auth_clear(tokens);
    if (!token_copy(tokens->credential.value, sizeof(tokens->credential.value), key, false))
        return snag_fail(error, error_size, EINVAL, "API key must contain 1..16384 non-whitespace ASCII bytes");
    tokens->credential.len = strlen(key);
    return 0;
}

static int
private_fd(int fd, bool directory)
{
    snag_file_info st;
    struct snag_file_privacy privacy;
    if (snag_fstat(fd, &st) < 0 || snag_fd_privacy(fd, &privacy) < 0 ||
        !privacy.effective_owner || !privacy.private_access || (directory ? !S_ISDIR(st.st_mode) :
                     (!S_ISREG(st.st_mode) || st.st_nlink != 1u))) return snag_errno(EACCES);
    return 0;
}

static bool
provider_valid(const struct snag_provider_config *provider)
{
    if (!provider || !snag_config_name_valid(provider->name) ||
        (provider->auth != SNAG_AUTH_API_KEY && !snag_auth_uses_codex(provider->auth) &&
         provider->auth != SNAG_AUTH_META)) return false;
    if (snag_auth_uses_codex(provider->auth))
        return strcmp(provider->base_url, SNAG_CHATGPT_BASE) == 0;
    if (provider->auth == SNAG_AUTH_META)
        return snag_is_meta_base(provider->base_url);
    return true;
}

static int
auth_dir(int root_fd, bool create)
{
    int fd;
    if (private_fd(root_fd, true) < 0) return -1;
    if (create) {
        if (snag_mkdir_private_at(root_fd, "auth") == 0) {
            if (snag_sync_dir(root_fd) < 0) return -1;
        } else if (errno != EEXIST) {
            return -1;
        }
    }
    fd = snag_open_read_security_at(root_fd, "auth", true);
    if (fd >= 0 && private_fd(fd, true) < 0) {
        (void)close(fd);
        return -1;
    }
    return fd;
}

static int
lock_provider(int dir, const char *name, snag_auth_pump_fn pump, void *opaque)
{
    char path[SNAG_CONFIG_PROVIDER_NAME_MAX + 8u];
    int fd;
    uint64_t deadline = snag_monotonic_ms() + 30000u;

    (void)snprintf(path, sizeof(path), "%s.lock", name);
    fd = snag_create_private_at(dir, path, false);
    if (fd < 0) return -1;
    if (private_fd(fd, false) < 0) goto fail;
    while (snag_lock_file(fd, false) < 0) {
        if (errno != EACCES && errno != EAGAIN && errno != EINTR) goto fail;
        if (snag_monotonic_ms() >= deadline) {
            errno = ETIMEDOUT;
            goto fail;
        }
        if (pump ? pump(opaque, 50u) != 0 : snag_sleep_ms(50u) < 0) {
            errno = ECANCELED;
            goto fail;
        }
    }
    return fd;
fail: (void)close(fd);
    return -1;
}

/* A method-specific login can coexist with the legacy file still read by an
 * older running binary. Keep legacy paths for existing stores and writers. */
static int
auth_path(int dir, const struct snag_provider_config *provider, char path[AUTH_PATH_MAX])
{
    snag_file_info st;

    (void)snprintf(path, AUTH_PATH_MAX, "%s.%s.json", provider->name,
        snag_auth_kind_name(provider->auth));
    if (snag_lstat_at(dir, path, &st) == 0) return 0;
    if (errno != ENOENT) return -1;
    (void)snprintf(path, AUTH_PATH_MAX, "%s.json", provider->name);
    return 0;
}

static int
read_tokens(int dir, const struct snag_provider_config *provider, struct snag_auth_tokens *tokens)
{
    char path[AUTH_PATH_MAX], error[128];
    json_t *value = NULL;
    const char *kind, *base;
    snag_file_info st;
    int fd, rc = -1;

    snag_auth_clear(tokens);
    if (auth_path(dir, provider, path) < 0) return -1;
    fd = snag_open_read_security_at(dir, path, false);
    if (fd < 0) return errno == ENOENT ? 1 : -1;
    struct snag_buf text = {.max = AUTH_FILE_MAX};
    if (private_fd(fd, false) < 0 || snag_fstat(fd, &st) < 0 ||
        st.st_size < 1 || (uint64_t)st.st_size > AUTH_FILE_MAX) goto out;
    if (snag_buf_read(&text, fd) < 0) goto out;
    value = snag_json_load_strict(text.data, text.len, AUTH_FILE_MAX, error, sizeof(error));
    kind = snag_json_string(value, "kind");
    base = snag_json_string(value, "base_url");
    if (!snag_json_exact_keys(value, "kind base_url access_token refresh_token account_id expires_at_ms") ||
        !kind || !base || strcmp(kind, snag_auth_kind_name(provider->auth)) ||
        strcmp(base, provider->base_url) ||
        !token_copy(tokens->credential.value, sizeof(tokens->credential.value),
                     snag_json_string(value, "access_token"), false) ||
        !token_copy(tokens->refresh_token, sizeof(tokens->refresh_token),
                     snag_json_string(value, "refresh_token"),
                     provider->auth == SNAG_AUTH_CODEX_TOKEN ||
                     provider->auth == SNAG_AUTH_API_KEY ||
                     provider->auth == SNAG_AUTH_META) ||
        !token_copy(tokens->credential.account_id, sizeof(tokens->credential.account_id),
                     snag_json_string(value, "account_id"),
                     provider->auth == SNAG_AUTH_API_KEY ||
                     provider->auth == SNAG_AUTH_META) ||
        snag_json_integer_u64(value, "expires_at_ms", &tokens->expires_at_ms) < 0 ||
        ((provider->auth == SNAG_AUTH_CHATGPT || provider->auth == SNAG_AUTH_META) &&
         !tokens->expires_at_ms) ||
        (provider->auth == SNAG_AUTH_CODEX_TOKEN &&
         (tokens->expires_at_ms || tokens->refresh_token[0])) ||
        (provider->auth == SNAG_AUTH_API_KEY && (tokens->expires_at_ms ||
            tokens->refresh_token[0] || tokens->credential.account_id[0]))) goto out;
    tokens->credential.len = strlen(tokens->credential.value);
    rc = 0;
out: snag_auth_json_free(value);
    if (text.data) memset(text.data, 0, text.len);
    snag_buf_free(&text);
    (void)close(fd);
    if (rc < 0) {
        snag_auth_clear(tokens);
        errno = EACCES;
    }
    return rc;
}

static int
write_tokens(int dir, const struct snag_provider_config *provider, const struct snag_auth_tokens *tokens)
{
    char path[AUTH_PATH_MAX];
    char temp[SNAG_ID_HEX_LEN + 8u], id[SNAG_ID_HEX_LEN + 1u];
    json_t *value = json_object();
    int fd = -1, rc = -1;

    temp[0] = '\0';
    struct snag_buf text = {.max = AUTH_FILE_MAX};
    if (!value || snag_json_set_new(value, "kind", json_string(snag_auth_kind_name(provider->auth))) < 0 ||
        snag_json_set_new(value, "base_url", json_string(provider->base_url)) < 0 ||
        snag_json_set_new(value, "access_token", json_string(tokens->credential.value)) < 0 ||
        snag_json_set_new(value, "refresh_token", json_string(tokens->refresh_token)) < 0 ||
        snag_json_set_new(value, "account_id", json_string(tokens->credential.account_id)) < 0 ||
        snag_json_set_new(value, "expires_at_ms", json_integer((json_int_t)tokens->expires_at_ms)) < 0 ||
        snag_json_canonical(value, &text) < 0 || snag_random_id(id) < 0) goto out;
    if (auth_path(dir, provider, path) < 0) goto out;
    (void)snprintf(temp, sizeof(temp), "%s.tmp", id);
    fd = snag_create_private_at(dir, temp, true);
    if (fd < 0 || snag_write_full(fd, text.data, text.len) < 0 || snag_fsync(fd) < 0 ||
        snag_rename_at(dir, temp, dir, path) < 0) goto out;
    temp[0] = '\0';
    if (snag_fsync(dir) < 0) goto out;
    rc = 0;
out:
    if (fd >= 0) (void)close(fd);
    if (temp[0]) (void)snag_unlink_at(dir, temp, false);
    snag_auth_json_free(value);
    if (text.data) memset(text.data, 0, text.len);
    snag_buf_free(&text);
    return rc;
}

int
snag_auth_load(int root_fd, const struct snag_provider_config *provider,
              struct snag_auth_tokens *tokens, char *error, size_t error_size)
{
    int dir, rc;
    snag_auth_clear(tokens);
    if (!provider_valid(provider))
        return snag_errorf(error, error_size, "invalid stored credential provider");
    dir = auth_dir(root_fd, false);
    if (dir < 0) {
        rc = errno == ENOENT ? 1 : -1;
    } else {
        rc = read_tokens(dir, provider, tokens);
        (void)close(dir);
    }
    if (rc != 0) snag_errorf(error, error_size, rc == 1 ?
            "provider %s is not logged in; use snajpagent login %s" :
            "provider %s credentials are unsafe, invalid, or bound to another endpoint; use snajpagent login %s",
            provider->name, provider->name);
    if (rc == 0) tokens->credential.root_fd = root_fd;
    return rc;
}

int
snag_auth_save(int root_fd, const struct snag_provider_config *provider,
              const struct snag_auth_tokens *tokens, struct snag_auth_tokens *previous,
              snag_auth_pump_fn pump, void *opaque, char *error, size_t error_size)
{
    struct snag_auth_tokens local_previous;
    int dir = -1, lock = -1, rc = -1;
    if (!provider_valid(provider) || !tokens->credential.len) goto out;
    dir = auth_dir(root_fd, true);
    if (dir < 0 || (lock = lock_provider(dir, provider->name, pump, opaque)) < 0) goto out;
    if (read_tokens(dir, provider, previous ? previous : &local_previous) < 0) goto out;
    rc = write_tokens(dir, provider, tokens);
out: snag_auth_clear(&local_previous);
    if (lock >= 0) (void)close(lock);
    if (dir >= 0) (void)close(dir);
    if (rc < 0) snag_errorf(error, error_size, "cannot save provider credentials safely");
    return rc;
}

int
snag_auth_restore(int root_fd, const struct snag_provider_config *provider,
                 const struct snag_auth_tokens *expected, const struct snag_auth_tokens *previous,
                 char *error, size_t error_size)
{
    struct snag_auth_tokens current;
    char path[AUTH_PATH_MAX];
    int dir = auth_dir(root_fd, false), lock = -1, rc = -1;
    if (dir < 0 || (lock = lock_provider(dir, provider->name, NULL, NULL)) < 0 ||
        read_tokens(dir, provider, &current) != 0) goto out;
    if (strcmp(current.credential.value, expected->credential.value) ||
        strcmp(current.refresh_token, expected->refresh_token) ||
        current.expires_at_ms != expected->expires_at_ms) {
        snag_errorf(error, error_size, "credentials changed concurrently; rollback left the newer login intact");
        goto out;
    }
    if (previous->credential.len) {
        rc = write_tokens(dir, provider, previous);
    } else {
        if (auth_path(dir, provider, path) < 0) goto out;
        if (snag_unlink_at(dir, path, false) == 0) rc = snag_fsync(dir);
    }
out: snag_auth_clear(&current);
    if (lock >= 0) (void)close(lock);
    if (dir >= 0) (void)close(dir);
    if (rc < 0 && !error[0])
        snag_errorf(error, error_size, "credential rollback failed; login state is retained for recovery");
    return rc;
}

int
snag_auth_logout(int root_fd, const struct snag_provider_config *provider,
                snag_auth_pump_fn pump, void *opaque, char *error, size_t error_size)
{
    char path[AUTH_PATH_MAX];
    struct snag_auth_tokens tokens;
    int dir = -1, lock = -1, rc = -1;
    if (!provider_valid(provider)) goto out;
    dir = auth_dir(root_fd, false);
    if (dir < 0) {
        if (errno == ENOENT) rc = 0;
        goto out;
    }
    lock = lock_provider(dir, provider->name, pump, opaque);
    if (lock < 0 || read_tokens(dir, provider, &tokens) < 0) goto out;
    if (auth_path(dir, provider, path) < 0) goto out;
    if (snag_unlink_at(dir, path, false) < 0 && errno != ENOENT) goto out;
    rc = snag_fsync(dir);
out: snag_auth_clear(&tokens);
    if (lock >= 0) (void)close(lock);
    if (dir >= 0) (void)close(dir);
    if (rc < 0) snag_errorf(error, error_size, "cannot remove provider credentials safely");
    return rc;
}

void
snag_auth_config_close(struct snag_config *config)
{
    for (size_t i = 0; i < config->provider_count; ++i) {
        struct snag_auth_state *state = config->providers[i].auth_state;
        if (!state) continue;
        pthread_mutex_destroy(&state->mutex);
        snag_auth_clear(&state->tokens);
        free(state);
        config->providers[i].auth_state = NULL;
    }
}

int
snag_auth_config_open(int root_fd, struct snag_config *config, char *error, size_t error_size)
{
    for (size_t i = 0; i < config->provider_count; ++i) {
        struct snag_provider_config *provider = &config->providers[i];
        struct snag_auth_state *state = calloc(1u, sizeof(*state));
        if (!state) goto fail;
        if (pthread_mutex_init(&state->mutex, NULL)) {
            free(state);
            goto fail;
        }
        provider->auth_state = state;
        snag_auth_clear(&state->tokens);
        if (provider->api_key.kind != SNAG_SECRET_NONE) {
            state->result = snag_credential_resolve(&state->tokens.credential,
                &provider->api_key, state->error, sizeof(state->error));
        } else {
            state->result = snag_auth_load(root_fd, provider, &state->tokens,
                state->error, sizeof(state->error));
        }
        state->tokens.credential.root_fd = root_fd;
    }
    return 0;
fail:
    snag_auth_config_close(config);
    return snag_errorf(error, error_size, "cannot snapshot provider credentials");
}

int
snag_auth_config_check(const struct snag_provider_config *provider,
                       char *error, size_t error_size)
{
    if (!provider || !provider->auth_state) {
        return snag_errorf(error, error_size, "provider credential snapshot is unavailable");
    }
    if (provider->auth_state->result != 0) {
        return snag_errorf(error, error_size, "%s; use /configure after correcting the login",
            provider->auth_state->error);
    }
    return 0;
}

/* Renewal may consult the shared store to reuse another owner's renewal for
 * this account. A different login never replaces this owner's credentials. */
static int
refresh_snapshot(int root_fd, const struct snag_provider_config *provider,
                 struct snag_auth_tokens *tokens, bool force, const char *stale,
                 snag_auth_pump_fn pump, void *opaque, char *error, size_t error_size)
{
    struct snag_auth_tokens current;
    struct snag_auth_tokens next = *tokens;
    int dir = auth_dir(root_fd, false);
    int lock = -1;
    int rc = -1;
    bool same_login = false;

    snag_auth_clear(&current);
    if (dir >= 0) {
        lock = lock_provider(dir, provider->name, pump, opaque);
        if (lock < 0) goto out;
        if (read_tokens(dir, provider, &current) == 0) {
            same_login = tokens->credential.account_id[0] ||
                !strcmp(current.credential.value, tokens->credential.value) ||
                (tokens->refresh_token[0] &&
                 !strcmp(current.refresh_token, tokens->refresh_token));
            if (strcmp(current.credential.account_id, tokens->credential.account_id)) {
                same_login = false;
            }
            if (same_login) next = current;
        }
    }
    if ((force && stale && !strcmp(stale, next.credential.value)) ||
        next.expires_at_ms <= snag_time_ms() + 60000u) {
        int (*refresh)(struct snag_auth_tokens *, snag_auth_pump_fn,
            void *, char *, size_t) = provider->auth == SNAG_AUTH_META ?
            snag_auth_refresh_meta : snag_auth_refresh;
        if (refresh(&next, pump, opaque, error, error_size) < 0) goto out;
        /* Leave a newer login or logout untouched. */
        if (same_login && write_tokens(dir, provider, &next) < 0) {
            (void)snag_errorf(error, error_size, "cannot save renewed provider credentials");
            goto out;
        }
    }
    *tokens = next;
    tokens->credential.root_fd = root_fd;
    rc = 0;
out:
    snag_auth_clear(&current);
    snag_auth_clear(&next);
    if (lock >= 0) (void)close(lock);
    if (dir >= 0) (void)close(dir);
    return rc;
}

static int
read_snapshot(int root_fd, const struct snag_provider_config *provider,
              bool force, const char *stale, struct snag_credential *out,
              snag_auth_pump_fn pump, void *opaque, char *error, size_t error_size)
{
    struct snag_auth_state *state = provider->auth_state;
    uint64_t deadline = snag_monotonic_ms() + 30000u;
    int rc;

    while ((rc = pthread_mutex_trylock(&state->mutex)) != 0) {
        if (rc != EBUSY || snag_monotonic_ms() >= deadline) {
            return snag_errorf(error, error_size, "provider credential renewal is busy");
        }
        if (pump ? pump(opaque, 50u) != 0 : snag_sleep_ms(50u) < 0) {
            return snag_fail(error, error_size, ECANCELED, "credential renewal interrupted");
        }
    }
    rc = snag_auth_config_check(provider, error, error_size);
    if (!rc && (provider->auth == SNAG_AUTH_CHATGPT || provider->auth == SNAG_AUTH_META) &&
        (force || state->tokens.expires_at_ms <= snag_time_ms() + 60000u)) {
        rc = refresh_snapshot(root_fd, provider, &state->tokens, force, stale,
            pump, opaque, error, error_size);
    }
    if (!rc) *out = state->tokens.credential;
    pthread_mutex_unlock(&state->mutex);
    if (rc < 0 && error && error_size && !error[0]) {
        (void)snag_errorf(error, error_size, "cannot renew provider credentials");
    }
    return rc;
}

int
snag_auth_read(int root_fd, const struct snag_provider_config *provider,
              bool force, const char *stale, struct snag_credential *out,
              snag_auth_pump_fn pump, void *opaque, char *error, size_t error_size)
{
    struct snag_auth_tokens tokens;
    int dir = -1, lock = -1, rc;
    snag_credential_clear(out);
    if (provider->auth_state) {
        return read_snapshot(root_fd, provider, force, stale, out,
            pump, opaque, error, error_size);
    }
    if (provider->api_key.kind != SNAG_SECRET_NONE) {
        rc = snag_credential_resolve(out, &provider->api_key, error, error_size);
        if (rc == 0) out->root_fd = root_fd;
        return rc;
    }
    rc = snag_auth_load(root_fd, provider, &tokens, error, error_size);
    if (rc != 0) {
        rc = -1;
        goto done;
    }
    if ((provider->auth == SNAG_AUTH_CHATGPT || provider->auth == SNAG_AUTH_META) &&
        (force || tokens.expires_at_ms <= snag_time_ms() + 60000u)) {
        rc = -1;
        dir = auth_dir(root_fd, false);
        if (dir < 0 || (lock = lock_provider(dir, provider->name, pump, opaque)) < 0 ||
            read_tokens(dir, provider, &tokens) != 0) goto done;
        if ((force && stale && strcmp(stale, tokens.credential.value) == 0) ||
            tokens.expires_at_ms <= snag_time_ms() + 60000u) {
            int (*refresh)(struct snag_auth_tokens *, snag_auth_pump_fn, void *, char *, size_t) =
                provider->auth == SNAG_AUTH_META ? snag_auth_refresh_meta : snag_auth_refresh;
            if (refresh(&tokens, pump, opaque, error, error_size) < 0 ||
                write_tokens(dir, provider, &tokens) < 0) goto done;
        }
        rc = 0;
    }
    *out = tokens.credential;
    out->root_fd = root_fd;
done: snag_auth_clear(&tokens);
    if (lock >= 0) (void)close(lock);
    if (dir >= 0) (void)close(dir);
    if (rc < 0 && !error[0]) snag_errorf(error, error_size, "cannot acquire or refresh provider credentials");
    return rc;
}

/* SPDX-License-Identifier: GPL-2.0-only */
#include "model_cache.h"

#include "base.h"
#include "json.h"
#include "provider.h"
#include "wire.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

#define SNJ_MODEL_CACHE_FILE_MAX (8u * 1024u * 1024u)
#define SNJ_MODEL_CACHE_MODELS_MAX 4096u
#define SNJ_MODEL_CACHE_EFFORTS_MAX 32u
#define SNJ_MODEL_CACHE_ENTRIES_MAX 32768u

static void
set_error(char *error, size_t size, const char *fmt, ...)
{
    va_list ap;
    if (!size)
        return;
    va_start(ap, fmt);
    (void)vsnprintf(error, size, fmt, ap);
    va_end(ap);
}

void
snj_model_cache_init(struct snj_model_cache *cache)
{
    memset(cache, 0, sizeof(*cache));
}

void
snj_model_cache_free(struct snj_model_cache *cache)
{
    if (cache->providers)
        json_decref(cache->providers);
    snj_model_cache_init(cache);
}

static bool
cache_string(const json_t *value, size_t max)
{
    const char *text;
    size_t len;
    return json_is_string(value) &&
           (text = json_string_value(value)) != NULL &&
           (len = json_string_length(value)) != 0u && len <= max &&
           strlen(text) == len &&
           snj_utf8_valid((const unsigned char *)text, len, true);
}

static bool
model_valid(const json_t *model)
{
    static const char *const keys[] = {"default_effort", "efforts", "id"};
    json_t *fallback;
    json_t *efforts;

    if (!json_is_object(model) || !snj_json_exact_keys((json_t *)model, keys, 3u) ||
        !cache_string(json_object_get(model, "id"), SNJ_CONFIG_MODEL_MAX - 1u))
        return false;
    fallback = json_object_get(model, "default_effort");
    if (!json_is_null(fallback) &&
        !cache_string(fallback, SNJ_CONFIG_EFFORT_MAX - 1u))
        return false;
    efforts = json_object_get(model, "efforts");
    if (!json_is_array(efforts) ||
        json_array_size(efforts) > SNJ_MODEL_CACHE_EFFORTS_MAX)
        return false;
    for (size_t i = 0; i < json_array_size(efforts); ++i) {
        json_t *effort = json_array_get(efforts, i);
        if (!cache_string(effort, SNJ_CONFIG_EFFORT_MAX - 1u))
            return false;
        for (size_t j = 0; j < i; ++j)
            if (strcmp(json_string_value(json_array_get(efforts, j)),
                       json_string_value(effort)) == 0)
                return false;
    }
    return true;
}

static bool
providers_valid(const json_t *providers)
{
    static const char *const keys[] = {"models", "name"};
    size_t total_models = 0u;
    size_t total_entries = 0u;

    if (!json_is_array(providers) || json_array_size(providers) == 0u ||
        json_array_size(providers) > SNJ_CONFIG_PROVIDER_MAX)
        return false;
    for (size_t i = 0; i < json_array_size(providers); ++i) {
        json_t *provider = json_array_get(providers, i);
        json_t *models;
        const char *name;
        if (!json_is_object(provider) ||
            !snj_json_exact_keys(provider, keys, 2u) ||
            !cache_string(json_object_get(provider, "name"),
                          SNJ_CONFIG_PROVIDER_NAME_MAX) ||
            !(name = snj_json_string(provider, "name")) ||
            !json_is_array((models = json_object_get(provider, "models"))) ||
            json_array_size(models) >
                SNJ_MODEL_CACHE_MODELS_MAX - total_models)
            return false;
        for (size_t j = 0; j < i; ++j)
            if (strcmp(snj_json_string(json_array_get(providers, j), "name"),
                       name) == 0)
                return false;
        total_models += json_array_size(models);
        for (size_t j = 0; j < json_array_size(models); ++j) {
            json_t *model = json_array_get(models, j);
            json_t *efforts;
            size_t variants;
            const char *id;
            if (!model_valid(model) ||
                !(id = snj_json_string(model, "id")))
                return false;
            efforts = json_object_get(model, "efforts");
            variants = json_array_size(efforts);
            if (variants == 0u)
                variants = 1u;
            if (variants > SNJ_MODEL_CACHE_ENTRIES_MAX - total_entries)
                return false;
            total_entries += variants;
            for (size_t k = 0; k < j; ++k)
                if (strcmp(snj_json_string(json_array_get(models, k), "id"),
                           id) == 0)
                    return false;
        }
    }
    return true;
}

static int
decode_cache(const unsigned char *data, size_t len,
             struct snj_model_cache *cache,
             char *error, size_t error_size)
{
    static const char *const keys[] = {"format", "providers", "updated_at_ms"};
    json_t *root;
    json_t *providers;
    json_t *copy;
    uint64_t format;
    uint64_t updated;
    char json_error[160] = {0};

    root = snj_json_load_strict(data, len, SNJ_MODEL_CACHE_FILE_MAX,
                                json_error, sizeof(json_error));
    if (!root || !json_is_object(root) ||
        !snj_json_exact_keys(root, keys, 3u) ||
        snj_json_integer_u64(root, "format", &format) < 0 || format != 1u ||
        snj_json_integer_u64(root, "updated_at_ms", &updated) < 0 ||
        updated == 0u ||
        !providers_valid((providers = json_object_get(root, "providers")))) {
        set_error(error, error_size, "invalid model cache%s%s",
                  json_error[0] ? ": " : "",
                  json_error[0] ? json_error : "");
        if (root)
            json_decref(root);
        errno = EINVAL;
        return -1;
    }
    copy = json_deep_copy(providers);
    json_decref(root);
    if (!copy) {
        errno = ENOMEM;
        return -1;
    }
    snj_model_cache_free(cache);
    cache->providers = copy;
    cache->updated_at_ms = updated;
    return 0;
}

int
snj_model_cache_load(struct snj_store *store, struct snj_model_cache *cache,
                     char *error, size_t error_size)
{
    struct stat st;
    struct snj_buf data;
    int fd;
    int rc = -1;

    if (!store || store->root_fd < 0 || !cache) {
        errno = EINVAL;
        return -1;
    }
    fd = openat(store->root_fd, "models.json", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        if (errno == ENOENT)
            return 1;
        set_error(error, error_size, "cannot open model cache: %s", strerror(errno));
        return -1;
    }
    snj_buf_init(&data, SNJ_MODEL_CACHE_FILE_MAX);
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_uid != getuid() ||
        (st.st_mode & 077u) != 0 || st.st_size <= 0 ||
        (uintmax_t)st.st_size > SNJ_MODEL_CACHE_FILE_MAX) {
        set_error(error, error_size,
                  "model cache must be a private user-owned regular file no larger than 8 MiB");
        errno = EACCES;
        goto out;
    }
    for (;;) {
        unsigned char chunk[8192];
        ssize_t got = read(fd, chunk, sizeof(chunk));
        if (got < 0) {
            if (errno == EINTR)
                continue;
            set_error(error, error_size, "cannot read model cache: %s", strerror(errno));
            goto out;
        }
        if (got == 0)
            break;
        if (snj_buf_append(&data, chunk, (size_t)got) < 0) {
            set_error(error, error_size, "model cache exceeds 8 MiB");
            goto out;
        }
    }
    rc = decode_cache(data.data, data.len, cache, error, error_size);
out:
    {
        int saved = errno;
        snj_buf_free(&data);
        (void)close(fd);
        errno = saved;
    }
    return rc;
}

int
snj_model_cache_replace(struct snj_store *store, const json_t *providers,
                        uint64_t updated_at_ms, struct snj_model_cache *cache,
                        char *error, size_t error_size)
{
    struct snj_buf data;
    json_t *root = NULL;
    json_t *installed = NULL;
    char id[SNJ_ID_HEX_LEN + 1u];
    char tmp_name[64];
    int fd = -1;
    int rc = -1;

    if (!store || store->root_fd < 0 || !cache || !updated_at_ms ||
        updated_at_ms > (uint64_t)INT64_MAX ||
        !providers_valid(providers)) {
        set_error(error, error_size, "refusing to write an invalid model cache");
        errno = EINVAL;
        return -1;
    }
    root = json_object();
    installed = json_deep_copy(providers);
    snj_buf_init(&data, SNJ_MODEL_CACHE_FILE_MAX);
    if (!root || !installed ||
        snj_json_set_new(root, "format", json_integer(1)) < 0 ||
        snj_json_set_new(root, "providers", json_deep_copy(providers)) < 0 ||
        snj_json_set_new(root, "updated_at_ms",
                         json_integer((json_int_t)updated_at_ms)) < 0 ||
        snj_json_canonical(root, &data) < 0 || snj_buf_putc(&data, '\n') < 0 ||
        snj_random_id(id) < 0) {
        set_error(error, error_size, "cannot encode model cache");
        goto out;
    }
    (void)snprintf(tmp_name, sizeof(tmp_name), "models.json.tmp.%s", id);
    fd = openat(store->root_fd, tmp_name,
                O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        set_error(error, error_size, "cannot create model cache: %s",
                  strerror(errno));
        goto out;
    }
    if (snj_write_full(fd, data.data, data.len) < 0 ||
        snj_sync_file(fd) < 0) {
        int saved = errno;
        (void)close(fd);
        fd = -1;
        (void)unlinkat(store->root_fd, tmp_name, 0);
        set_error(error, error_size, "cannot write model cache: %s",
                  strerror(saved));
        errno = saved;
        goto out;
    }
    if (close(fd) < 0) {
        int saved = errno;
        fd = -1;
        (void)unlinkat(store->root_fd, tmp_name, 0);
        set_error(error, error_size, "cannot close model cache: %s",
                  strerror(saved));
        errno = saved;
        goto out;
    }
    fd = -1;
    if (renameat(store->root_fd, tmp_name, store->root_fd, "models.json") < 0 ||
        snj_sync_dir(store->root_fd) < 0) {
        int saved = errno;
        (void)unlinkat(store->root_fd, tmp_name, 0);
        set_error(error, error_size, "cannot install model cache: %s",
                  strerror(saved));
        errno = saved;
        goto out;
    }
    snj_model_cache_free(cache);
    cache->providers = installed;
    installed = NULL;
    cache->updated_at_ms = updated_at_ms;
    rc = 0;
out:
    if (fd >= 0)
        (void)close(fd);
    if (root)
        json_decref(root);
    if (installed)
        json_decref(installed);
    snj_buf_free(&data);
    return rc;
}

int
snj_model_cache_import_codex(struct snj_store *store, const char *path,
                             const char *provider_name,
                             uint64_t updated_at_ms,
                             struct snj_model_cache *cache,
                             char *error, size_t error_size)
{
    struct stat st;
    struct snj_buf data;
    json_t *models = NULL;
    json_t *provider = NULL;
    json_t *providers = NULL;
    int fd = -1;
    int rc = -1;

    if (!store || store->root_fd < 0 || !path || path[0] != '/' ||
        !provider_name || !provider_name[0] || !updated_at_ms || !cache) {
        errno = EINVAL;
        return -1;
    }
    fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        if (errno == ENOENT)
            return 1;
        set_error(error, error_size, "cannot open local Codex model cache: %s",
                  strerror(errno));
        return -1;
    }
    snj_buf_init(&data, SNJ_WIRE_BODY_MAX);
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_uid != getuid() ||
        st.st_size <= 0 || (uintmax_t)st.st_size > SNJ_WIRE_BODY_MAX) {
        set_error(error, error_size,
                  "local Codex model cache must be a user-owned regular file no larger than 2 MiB");
        errno = EACCES;
        goto out;
    }
    for (;;) {
        unsigned char chunk[8192];
        ssize_t got = read(fd, chunk, sizeof(chunk));
        if (got < 0) {
            if (errno == EINTR)
                continue;
            set_error(error, error_size,
                      "cannot read local Codex model cache: %s",
                      strerror(errno));
            goto out;
        }
        if (got == 0)
            break;
        if (snj_buf_append(&data, chunk, (size_t)got) < 0) {
            set_error(error, error_size,
                      "local Codex model cache exceeds 2 MiB");
            goto out;
        }
    }
    if (snj_provider_models_decode(data.data, data.len, &models,
                                   error, error_size) < 0)
        goto out;
    if (json_array_size(models) == 0u) {
        set_error(error, error_size, "local Codex model cache is empty");
        errno = EINVAL;
        goto out;
    }
    provider = json_object();
    providers = json_array();
    if (!provider || !providers ||
        json_object_set(provider, "models", models) < 0 ||
        snj_json_set_new(provider, "name", json_string(provider_name)) < 0 ||
        json_array_append(providers, provider) < 0) {
        set_error(error, error_size,
                  "cannot assemble imported Codex model cache");
        errno = ENOMEM;
        goto out;
    }
    json_decref(models);
    models = NULL;
    json_decref(provider);
    provider = NULL;
    rc = snj_model_cache_replace(store, providers, updated_at_ms, cache,
                                 error, error_size);
out:
    {
        int saved = errno;
        if (fd >= 0)
            (void)close(fd);
        if (models)
            json_decref(models);
        if (provider)
            json_decref(provider);
        if (providers)
            json_decref(providers);
        snj_buf_free(&data);
        errno = saved;
    }
    return rc;
}

const json_t *
snj_model_cache_find(const struct snj_model_cache *cache,
                     const char *provider, const char *model)
{
    if (!cache || !cache->providers || !provider || !model)
        return NULL;
    for (size_t i = 0; i < json_array_size(cache->providers); ++i) {
        json_t *entry = json_array_get(cache->providers, i);
        json_t *models;
        if (strcmp(snj_json_string(entry, "name"), provider) != 0)
            continue;
        models = json_object_get(entry, "models");
        for (size_t j = 0; j < json_array_size(models); ++j) {
            json_t *candidate = json_array_get(models, j);
            if (strcmp(snj_json_string(candidate, "id"), model) == 0)
                return candidate;
        }
    }
    return NULL;
}

static int
effort_rank(const char *effort)
{
    static const char *const ordered[] = {
        "none", "minimal", "low", "medium", "high", "xhigh", "max", "ultra"
    };
    for (size_t i = 0; i < sizeof(ordered) / sizeof(ordered[0]); ++i)
        if (strcmp(effort, ordered[i]) == 0)
            return (int)i;
    return -1;
}

const char *
snj_model_cache_best_effort(const json_t *model, const char *fallback)
{
    json_t *efforts;
    const char *best = NULL;
    int best_rank = -1;

    if (!model_valid(model))
        return fallback;
    efforts = json_object_get(model, "efforts");
    for (size_t i = 0; i < json_array_size(efforts); ++i) {
        const char *effort = json_string_value(json_array_get(efforts, i));
        int rank = effort_rank(effort);
        if (!best)
            best = effort;
        if (rank > best_rank) {
            best = effort;
            best_rank = rank;
        }
    }
    if (best)
        return best;
    {
        json_t *value = json_object_get(model, "default_effort");
        if (json_is_string(value))
            return json_string_value(value);
    }
    return fallback;
}

size_t
snj_model_cache_entry_count(const struct snj_model_cache *cache)
{
    size_t count = 0u;
    if (!cache || !cache->providers)
        return 0u;
    for (size_t i = 0; i < json_array_size(cache->providers); ++i) {
        json_t *models = json_object_get(json_array_get(cache->providers, i),
                                         "models");
        for (size_t j = 0; j < json_array_size(models); ++j) {
            json_t *efforts = json_object_get(json_array_get(models, j), "efforts");
            size_t variants = json_array_size(efforts);
            count += variants ? variants : 1u;
        }
    }
    return count;
}

int
snj_model_cache_entry(const struct snj_model_cache *cache, size_t index,
                      const char *fallback_effort,
                      const char **provider, const char **model,
                      const char **effort)
{
    size_t current = 1u;
    if (!cache || !cache->providers || !index || !provider || !model || !effort)
        return -1;
    for (size_t i = 0; i < json_array_size(cache->providers); ++i) {
        json_t *provider_entry = json_array_get(cache->providers, i);
        json_t *models = json_object_get(provider_entry, "models");
        for (size_t j = 0; j < json_array_size(models); ++j) {
            json_t *model_entry = json_array_get(models, j);
            json_t *efforts = json_object_get(model_entry, "efforts");
            size_t variants = json_array_size(efforts);
            if (!variants)
                variants = 1u;
            for (size_t k = 0; k < variants; ++k, ++current) {
                if (current != index)
                    continue;
                *provider = snj_json_string(provider_entry, "name");
                *model = snj_json_string(model_entry, "id");
                if (json_array_size(efforts))
                    *effort = json_string_value(json_array_get(efforts, k));
                else
                    *effort = snj_model_cache_best_effort(model_entry,
                                                          fallback_effort);
                return *provider && *model && *effort ? 0 : -1;
            }
        }
    }
    return 1;
}

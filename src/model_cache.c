/* SPDX-License-Identifier: GPL-2.0-only */
#include "model_cache.h"

#include "base.h"
#include "json.h"
#include "store_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
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
#define SNJ_MODEL_CACHE_INPUT_MAX (32u * 1024u * 1024u)
#define SNJ_MODEL_CACHE_SCHEMA 1u

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
nullable_limit(const json_t *object, const char *key, uint64_t max,
               uint64_t *value, bool *known)
{
    json_t *entry = json_object_get(object, key);
    json_int_t integer;

    *known = false;
    *value = 0u;
    if (json_is_null(entry))
        return true;
    if (!json_is_integer(entry) || (integer = json_integer_value(entry)) <= 0 ||
        (uint64_t)integer > max)
        return false;
    *known = true;
    *value = (uint64_t)integer;
    return true;
}

static bool
limits_valid(const json_t *limits)
{
    static const char *const keys[] = {
        "auto_compact_input_tokens", "context_window_tokens",
        "effective_context_window_percent", "input_context_window_tokens",
        "max_context_window_tokens", "max_input_tokens", "max_output_tokens"
    };
    uint64_t context, max_context, input_context, max_input, max_output;
    uint64_t auto_compact, effective;
    bool context_known, max_context_known, input_context_known;
    bool max_input_known, max_output_known, auto_compact_known, effective_known;

    if (!json_is_object(limits) ||
        !snj_json_exact_keys((json_t *)limits, keys,
                             sizeof(keys) / sizeof(keys[0])) ||
        !nullable_limit(limits, "context_window_tokens",
                        SNJ_CONFIG_TOKEN_LIMIT_MAX, &context,
                        &context_known) ||
        !nullable_limit(limits, "max_context_window_tokens",
                        SNJ_CONFIG_TOKEN_LIMIT_MAX, &max_context,
                        &max_context_known) ||
        !nullable_limit(limits, "input_context_window_tokens",
                        SNJ_CONFIG_TOKEN_LIMIT_MAX, &input_context,
                        &input_context_known) ||
        !nullable_limit(limits, "max_input_tokens",
                        SNJ_CONFIG_TOKEN_LIMIT_MAX, &max_input,
                        &max_input_known) ||
        !nullable_limit(limits, "max_output_tokens",
                        SNJ_CONFIG_TOKEN_LIMIT_MAX, &max_output,
                        &max_output_known) ||
        !nullable_limit(limits, "auto_compact_input_tokens",
                        SNJ_CONFIG_TOKEN_LIMIT_MAX, &auto_compact,
                        &auto_compact_known) ||
        !nullable_limit(limits, "effective_context_window_percent", 100u,
                        &effective, &effective_known))
        return false;
    return !(context_known && max_context_known && context > max_context) &&
           !(context_known && input_context_known && input_context > context) &&
           !(context_known && max_input_known && max_input > context) &&
           !(context_known && max_output_known && max_output > context) &&
           !(context_known && max_input_known && max_output_known &&
             max_input > context - max_output) &&
           !(context_known && auto_compact_known && auto_compact > context);
}

static bool
accounting_valid(const json_t *model)
{
    uint64_t hard;
    uint64_t tokens;
    uint64_t bytes;
    const char *state = snj_json_string(model, "count_capability");

    return state &&
           (strcmp(state, "unknown") == 0 ||
            strcmp(state, "supported") == 0 ||
            strcmp(state, "unsupported") == 0) &&
           snj_json_integer_u64(model, "observed_hard_input_tokens",
                                &hard) == 0 &&
           snj_json_integer_u64(model, "observed_input_tokens",
                                &tokens) == 0 &&
           snj_json_integer_u64(model, "observed_model_input_bytes",
                                &bytes) == 0 &&
           hard <= SNJ_CONFIG_TOKEN_LIMIT_MAX &&
           tokens <= SNJ_CONFIG_TOKEN_LIMIT_MAX &&
           bytes <= SNJ_MODEL_CACHE_INPUT_MAX && !!tokens == !!bytes;
}

static bool
model_valid(const json_t *model, bool cached)
{
    static const char *const catalog_keys[] = {
        "default_effort", "efforts", "id", "limits"
    };
    static const char *const cache_keys[] = {"count_capability", "default_effort",
        "efforts", "id", "limits", "observed_hard_input_tokens",
        "observed_input_tokens", "observed_model_input_bytes"};
    json_t *fallback;
    json_t *efforts;

    if (!json_is_object(model) || !snj_json_exact_keys((json_t *)model,
            cached ? cache_keys : catalog_keys, cached ? 8u : 4u) ||
        !cache_string(json_object_get(model, "id"), SNJ_CONFIG_MODEL_MAX - 1u) ||
        !limits_valid(json_object_get(model, "limits")))
        return false;
    if (cached && !accounting_valid(model))
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
providers_valid(const json_t *providers, bool cached)
{
    static const char *const keys[] = {
        "base_url", "models", "name", "protocol"
    };
    size_t total_models = 0u;
    size_t total_entries = 0u;

    if (!json_is_array(providers) || json_array_size(providers) == 0u ||
        json_array_size(providers) > SNJ_CONFIG_PROVIDER_MAX)
        return false;
    for (size_t i = 0; i < json_array_size(providers); ++i) {
        json_t *provider = json_array_get(providers, i);
        json_t *models;
        const char *name;
        const char *protocol;

        if (!json_is_object(provider) ||
            !snj_json_exact_keys(provider, keys, 4u) ||
            !cache_string(json_object_get(provider, "name"),
                          SNJ_CONFIG_PROVIDER_NAME_MAX) ||
            !cache_string(json_object_get(provider, "base_url"),
                          SNJ_CONFIG_URL_MAX) ||
            !cache_string(json_object_get(provider, "protocol"), 6u) ||
            !(protocol = snj_json_string(provider, "protocol")) ||
            (strcmp(protocol, "codex") != 0 &&
             strcmp(protocol, "openai") != 0) ||
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
            if (!model_valid(model, cached) ||
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

static const json_t *
provider_entry(const json_t *providers, const char *name)
{
    for (size_t i = 0; providers && i < json_array_size(providers); ++i) {
        json_t *entry = json_array_get(providers, i);
        if (strcmp(snj_json_string(entry, "name"), name) == 0)
            return entry;
    }
    return NULL;
}

static int
decode_cache(const unsigned char *data, size_t len,
             struct snj_model_cache *cache, char *error, size_t error_size)
{
    static const char *const keys[] = {"providers", "schema_version", "updated_at_ms"};
    json_t *root;
    json_t *providers;
    json_t *copy;
    uint64_t updated;
    uint64_t schema;

    root = snj_json_load_strict(data, len, SNJ_MODEL_CACHE_FILE_MAX,
                                error, error_size);
    if (!root || !json_is_object(root) ||
        !snj_json_exact_keys(root, keys, 3u) ||
        snj_json_integer_u64(root, "schema_version", &schema) < 0 ||
        schema != SNJ_MODEL_CACHE_SCHEMA ||
        snj_json_integer_u64(root, "updated_at_ms", &updated) < 0 ||
        updated == 0u ||
        !providers_valid((providers = json_object_get(root, "providers")), true)) {
        snj_errorf(error, error_size,
                  "model cache is unusable; use /model cache while idle");
        if (root)
            json_decref(root);
        errno = EINVAL;
        return -1;
    }
    copy = json_incref(providers);
    json_decref(root);
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
        snj_errorf(error, error_size, "cannot open model cache: %s", strerror(errno));
        return -1;
    }
    snj_buf_init(&data, SNJ_MODEL_CACHE_FILE_MAX);
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_uid != getuid() ||
        (st.st_mode & 077u) != 0 || st.st_size <= 0 ||
        (uintmax_t)st.st_size > SNJ_MODEL_CACHE_FILE_MAX) {
        snj_errorf(error, error_size,
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
            snj_errorf(error, error_size, "cannot read model cache: %s", strerror(errno));
            goto out;
        }
        if (got == 0)
            break;
        if (snj_buf_append(&data, chunk, (size_t)got) < 0) {
            snj_errorf(error, error_size, "model cache exceeds 8 MiB");
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

static int
lock_cache(struct snj_store *store, char *error, size_t error_size)
{
    struct flock lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET};
    int fd;
    int saved;

    fd = openat(store->root_fd, "models.lock",
                O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        snj_errorf(error, error_size, "cannot open model cache lock: %s",
                  strerror(errno));
        return -1;
    }
    if (snj_store_verify_private_fd(fd, false, "model cache lock",
                                    error, error_size) < 0)
        goto fail;
    if (fcntl(fd, F_SETLKW, &lock) < 0) {
        snj_errorf(error, error_size, "cannot lock model cache: %s",
                  strerror(errno));
        goto fail;
    }
    return fd;
fail:
    saved = errno;
    (void)close(fd);
    errno = saved;
    return -1;
}

static int
write_cache(struct snj_store *store, const json_t *providers,
            uint64_t updated_at_ms, struct snj_model_cache *cache,
            char *error, size_t error_size)
{
    struct snj_buf data;
    json_t *root = NULL;
    char id[SNJ_ID_HEX_LEN + 1u];
    char tmp_name[64] = {0};
    int fd = -1;
    int rc = -1;
    int saved;

    if (!store || store->root_fd < 0 || !cache || !updated_at_ms ||
        updated_at_ms > (uint64_t)INT64_MAX ||
        !providers_valid(providers, true)) {
        snj_errorf(error, error_size, "refusing to write an invalid model cache");
        errno = EINVAL;
        return -1;
    }
    root = json_object();
    snj_buf_init(&data, SNJ_MODEL_CACHE_FILE_MAX);
    if (!root ||
        snj_json_set_new(root, "providers",
                         json_incref((json_t *)providers)) < 0 ||
        snj_json_set_new(root, "schema_version",
                         json_integer(SNJ_MODEL_CACHE_SCHEMA)) < 0 ||
        snj_json_set_new(root, "updated_at_ms",
                         json_integer((json_int_t)updated_at_ms)) < 0 ||
        snj_json_canonical(root, &data) < 0 || snj_buf_putc(&data, '\n') < 0 ||
        snj_random_id(id) < 0) {
        snj_errorf(error, error_size, "cannot encode model cache");
        goto out;
    }
    (void)snprintf(tmp_name, sizeof(tmp_name), "models.json.tmp.%s", id);
    fd = openat(store->root_fd, tmp_name,
                O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        snj_errorf(error, error_size, "cannot create model cache: %s",
                  strerror(errno));
        goto out;
    }
    if (snj_write_full(fd, data.data, data.len) < 0 ||
        snj_sync_file(fd) < 0) {
        snj_errorf(error, error_size, "cannot write model cache: %s",
                  strerror(errno));
        goto out;
    }
    if (close(fd) < 0) {
        fd = -1;
        snj_errorf(error, error_size, "cannot close model cache: %s",
                  strerror(errno));
        goto out;
    }
    fd = -1;
    if (renameat(store->root_fd, tmp_name, store->root_fd, "models.json") < 0) {
        snj_errorf(error, error_size, "cannot install model cache: %s",
                  strerror(errno));
        goto out;
    }
    tmp_name[0] = '\0';
    if (snj_sync_dir(store->root_fd) < 0) {
        snj_errorf(error, error_size, "cannot sync model cache directory: %s",
                  strerror(errno));
        goto out;
    }
    /* Keep one reference after the root object releases its reference. */
    json_incref((json_t *)providers);
    snj_model_cache_free(cache);
    cache->providers = (json_t *)providers;
    cache->updated_at_ms = updated_at_ms;
    rc = 0;
out:
    saved = errno;
    if (fd >= 0)
        (void)close(fd);
    if (rc < 0 && tmp_name[0])
        (void)unlinkat(store->root_fd, tmp_name, 0);
    if (root)
        json_decref(root);
    snj_buf_free(&data);
    errno = saved;
    return rc;
}

static int
prepare_accounting(json_t *model, const json_t *old)
{
    static const char *const keys[] = {
        "observed_hard_input_tokens", "observed_input_tokens",
        "observed_model_input_bytes"
    };

    if (json_object_set_new(model, "count_capability",
                            json_string("unknown")) < 0)
        return -1;
    if (old) {
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i)
            if (json_object_set(model, keys[i],
                                json_object_get(old, keys[i])) < 0)
                return -1;
        return 0;
    }
    if (json_object_set_new(model, "observed_hard_input_tokens",
                            json_integer(0)) < 0 ||
        json_object_set_new(model, "observed_input_tokens",
                            json_integer(0)) < 0 ||
        json_object_set_new(model, "observed_model_input_bytes",
                            json_integer(0)) < 0)
        return -1;
    return 0;
}

int
snj_model_cache_replace(struct snj_store *store, const json_t *providers,
                        uint64_t updated_at_ms, struct snj_model_cache *cache,
                        char *error, size_t error_size)
{
    json_t *prepared = NULL;
    int lock_fd;
    int rc = -1;

    if (!store || store->root_fd < 0 || !cache || !updated_at_ms ||
        updated_at_ms > (uint64_t)INT64_MAX ||
        !providers_valid(providers, false)) {
        snj_errorf(error, error_size, "invalid model cache replacement");
        errno = EINVAL;
        return -1;
    }
    lock_fd = lock_cache(store, error, error_size);
    if (lock_fd < 0)
        return -1;
    snj_model_cache_free(cache);
    if (snj_model_cache_load(store, cache, error, error_size) < 0 &&
        errno != EINVAL)
        goto out;
    prepared = json_deep_copy(providers);
    if (!prepared) {
        snj_errorf(error, error_size, "cannot copy model catalog");
        errno = ENOMEM;
        goto out;
    }
    for (size_t i = 0; i < json_array_size(prepared); ++i) {
        json_t *after = json_array_get(prepared, i);
        const char *name = snj_json_string(after, "name");
        const json_t *before = provider_entry(cache->providers, name);
        bool bound = before &&
            strcmp(snj_json_string(before, "base_url"),
                   snj_json_string(after, "base_url")) == 0 &&
            strcmp(snj_json_string(before, "protocol"),
                   snj_json_string(after, "protocol")) == 0;
        json_t *models = json_object_get(after, "models");

        for (size_t j = 0; j < json_array_size(models); ++j) {
            json_t *model = json_array_get(models, j);
            const json_t *old_model = bound ?
                snj_model_cache_find(cache, name,
                                     snj_json_string(model, "id")) : NULL;

            if (prepare_accounting(model, old_model) < 0) {
                snj_errorf(error, error_size,
                          "cannot preserve model accounting");
                errno = ENOMEM;
                goto out;
            }
        }
    }
    if (error_size)
        error[0] = '\0';
    rc = write_cache(store, prepared, updated_at_ms, cache, error, error_size);
out:
    if (prepared)
        json_decref(prepared);
    (void)close(lock_fd);
    return rc;
}

int
snj_model_cache_record(struct snj_store *store, struct snj_model_cache *cache,
                       const struct snj_provider_config *provider,
                       const char *protocol, const char *model,
                       enum snj_count_capability capability,
                       uint64_t model_input_bytes, uint64_t input_tokens,
                       uint64_t hard_input_tokens,
                       char *error, size_t error_size)
{
    struct snj_model_cache lookup = {0};
    const json_t *source;
    const json_t *item;
    const char *current_state;
    const char *next_state = NULL;
    json_t *updated = NULL;
    uint64_t value = 0u;
    uint64_t largest_bytes = 0u;
    uint64_t largest_tokens = 0u;
    bool capability_changed;
    bool sample_changed;
    bool hard_limit_changed;
    int lock_fd;
    int rc = 1;

    if (!store || !cache || !provider || !protocol || !model || !*model ||
        capability > SNJ_COUNT_UNSUPPORTED ||
        ((model_input_bytes == 0u) != (input_tokens == 0u)) ||
        input_tokens > SNJ_CONFIG_TOKEN_LIMIT_MAX ||
        model_input_bytes > SNJ_MODEL_CACHE_INPUT_MAX ||
        hard_input_tokens > SNJ_CONFIG_TOKEN_LIMIT_MAX ||
        (capability == SNJ_COUNT_UNKNOWN && !input_tokens && !hard_input_tokens)) {
        errno = EINVAL;
        return -1;
    }
    lock_fd = lock_cache(store, error, error_size);
    if (lock_fd < 0)
        return -1;
    rc = snj_model_cache_load(store, cache, error, error_size);
    if (rc != 0)
        goto out;
    rc = 1;
    source = provider_entry(cache->providers, provider->name);
    item = snj_model_cache_find(cache, provider->name, model);
    if (!source ||
        strcmp(snj_json_string(source, "base_url"), provider->base_url) ||
        strcmp(snj_json_string(source, "protocol"), protocol) || !item)
        goto out;
    current_state = snj_json_string(item, "count_capability");
    if (capability != SNJ_COUNT_UNKNOWN)
        next_state = capability == SNJ_COUNT_SUPPORTED ?
            "supported" : "unsupported";
    capability_changed = next_state && strcmp(current_state, next_state) != 0;
    (void)snj_json_integer_u64(item, "observed_model_input_bytes",
                               &largest_bytes);
    (void)snj_json_integer_u64(item, "observed_input_tokens",
                               &largest_tokens);
    (void)snj_json_integer_u64(item, "observed_hard_input_tokens", &value);
    sample_changed = input_tokens &&
        (model_input_bytes > largest_bytes ||
         (model_input_bytes == largest_bytes && input_tokens > largest_tokens));
    hard_limit_changed = hard_input_tokens &&
        (!value || hard_input_tokens < value);
    if (!capability_changed && !sample_changed && !hard_limit_changed) {
        rc = 0;
        goto out;
    }
    updated = json_deep_copy(cache->providers);
    if (!updated) {
        snj_errorf(error, error_size, "cannot copy model cache observation");
        errno = ENOMEM;
        rc = -1;
        goto out;
    }
    lookup.providers = updated;
    item = snj_model_cache_find(&lookup, provider->name, model);
    if (!item) {
        snj_errorf(error, error_size, "cannot find copied model cache entry");
        errno = EINVAL;
        rc = -1;
        goto out;
    }
    if (capability_changed &&
        json_object_set_new((json_t *)item, "count_capability",
                            json_string(next_state)) < 0)
        goto write_error;
    if (sample_changed &&
        (json_object_set_new((json_t *)item, "observed_model_input_bytes",
                             json_integer((json_int_t)model_input_bytes)) < 0 ||
         json_object_set_new((json_t *)item, "observed_input_tokens",
                             json_integer((json_int_t)input_tokens)) < 0))
        goto write_error;
    if (hard_limit_changed &&
        json_object_set_new((json_t *)item, "observed_hard_input_tokens",
                            json_integer((json_int_t)hard_input_tokens)) < 0)
        goto write_error;
    rc = write_cache(store, updated, cache->updated_at_ms, cache,
                     error, error_size);
    goto out;
write_error:
    snj_errorf(error, error_size, "cannot update model cache observation");
    errno = ENOMEM;
    rc = -1;
out:
    if (updated)
        json_decref(updated);
    (void)close(lock_fd);
    return rc;
}

const json_t *
snj_model_cache_find(const struct snj_model_cache *cache,
                     const char *provider, const char *model)
{
    const json_t *entry;
    json_t *models;

    if (!cache || !cache->providers || !provider || !model)
        return NULL;
    entry = provider_entry(cache->providers, provider);
    if (!entry)
        return NULL;
    models = json_object_get(entry, "models");
    for (size_t i = 0; i < json_array_size(models); ++i)
        if (strcmp(snj_json_string(json_array_get(models, i), "id"),
                   model) == 0)
            return json_array_get(models, i);
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
    json_t *fallback_value;
    const char *best = NULL;
    int best_rank = -1;

    if (!model_valid(model, model &&
                     json_object_get(model, "count_capability") != NULL))
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
    fallback_value = json_object_get(model, "default_effort");
    if (json_is_string(fallback_value))
        return json_string_value(fallback_value);
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
            json_t *efforts = json_object_get(json_array_get(models, j),
                                              "efforts");
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

static void
read_capacity_limit(const json_t *limits, const char *key, uint64_t max,
                    uint64_t *value, bool *known)
{
    if (!nullable_limit(limits, key, max, value, known)) {
        *value = 0u;
        *known = false;
    }
}

static void
minimum_budget(uint64_t value, uint64_t *budget, bool *known)
{
    if (!*known || value < *budget) {
        *budget = value;
        *known = true;
    }
}

uint64_t
snj_model_compact_threshold(const struct snj_provider_config *provider,
                            const struct snj_model_capacity *capacity)
{
    uint64_t threshold;

    if (provider->auto_compact_input_tokens != SNJ_CONFIG_COMPACT_AUTO)
        return provider->auto_compact_input_tokens;
    if (!capacity->hard_input_known)
        return 120000u;
    /* Floor 90% without overflowing; only explicit zero disables policy. */
    threshold = capacity->hard_input_tokens / 10u * 9u +
                capacity->hard_input_tokens % 10u * 9u / 10u;
    return threshold ? threshold : 1u;
}

const char *
snj_capacity_source_name(enum snj_capacity_source source)
{
    switch (source) {
    case SNJ_CAPACITY_UNKNOWN: return "unknown";
    case SNJ_CAPACITY_CATALOG: return "advertised";
    case SNJ_CAPACITY_CONFIG: return "configured";
    case SNJ_CAPACITY_OBSERVED: return "observed";
    case SNJ_CAPACITY_STALE_CATALOG: return "stale-catalog-ignored";
    }
    return "unknown";
}

int
snj_model_capacity_resolve(const struct snj_model_cache *cache,
                           const struct snj_config *config,
                           const struct snj_provider_config *provider,
                           const char *model, const char *protocol,
                           struct snj_model_capacity *capacity,
                           char *error, size_t error_size)
{
    const struct snj_model_limit_config *override;
    const json_t *cached_provider;
    const json_t *cached_model = NULL;
    const json_t *limits = NULL;
    bool catalog_used = false;

    if (!config || !provider || !model || !*model || !protocol || !capacity ||
        (strcmp(protocol, "codex") != 0 && strcmp(protocol, "openai") != 0)) {
        snj_errorf(error, error_size, "invalid model capacity selection");
        errno = EINVAL;
        return -1;
    }
    memset(capacity, 0, sizeof(*capacity));
    capacity->codex_protocol = strcmp(protocol, "codex") == 0;
    cached_provider = provider_entry(cache ? cache->providers : NULL,
                                     provider->name);
    if (cached_provider) {
        capacity->source_bound =
            strcmp(snj_json_string(cached_provider, "base_url"),
                   provider->base_url) == 0 &&
            strcmp(snj_json_string(cached_provider, "protocol"), protocol) == 0;
        if (capacity->source_bound) {
            cached_model = snj_model_cache_find(cache, provider->name, model);
            if (cached_model)
                limits = json_object_get(cached_model, "limits");
        } else {
            capacity->source = SNJ_CAPACITY_STALE_CATALOG;
            capacity->cache_source_mismatch = true;
        }
    }
    override = snj_config_model_limit(config, provider->name, model);
    if (override) {
        capacity->context_window_tokens = override->context_window_tokens;
        capacity->context_window_known = override->context_window_known;
        capacity->max_input_tokens = override->max_input_tokens;
        capacity->max_input_known = override->max_input_known;
        capacity->max_output_tokens = override->max_output_tokens;
        capacity->max_output_known = override->max_output_known;
        capacity->source = SNJ_CAPACITY_CONFIG;
    } else if (limits) {
        read_capacity_limit(limits, "context_window_tokens",
                            SNJ_CONFIG_TOKEN_LIMIT_MAX,
                            &capacity->context_window_tokens,
                            &capacity->context_window_known);
        read_capacity_limit(limits, "max_context_window_tokens",
                            SNJ_CONFIG_TOKEN_LIMIT_MAX,
                            &capacity->max_context_window_tokens,
                            &capacity->max_context_window_known);
        read_capacity_limit(limits, "input_context_window_tokens",
                            SNJ_CONFIG_TOKEN_LIMIT_MAX,
                            &capacity->input_context_window_tokens,
                            &capacity->input_context_window_known);
        read_capacity_limit(limits, "max_input_tokens",
                            SNJ_CONFIG_TOKEN_LIMIT_MAX,
                            &capacity->max_input_tokens,
                            &capacity->max_input_known);
        read_capacity_limit(limits, "max_output_tokens",
                            SNJ_CONFIG_TOKEN_LIMIT_MAX,
                            &capacity->max_output_tokens,
                            &capacity->max_output_known);
        read_capacity_limit(limits, "auto_compact_input_tokens",
                            SNJ_CONFIG_TOKEN_LIMIT_MAX,
                            &capacity->auto_compact_input_tokens,
                            &capacity->auto_compact_input_known);
        {
            uint64_t percent = 0u;

            read_capacity_limit(limits, "effective_context_window_percent",
                                100u, &percent,
                                &capacity->effective_context_window_known);
            capacity->effective_context_window_percent = (unsigned int)percent;
        }
        catalog_used = capacity->context_window_known ||
            capacity->max_context_window_known ||
            capacity->input_context_window_known ||
            capacity->max_input_known || capacity->max_output_known ||
            capacity->auto_compact_input_known ||
            capacity->effective_context_window_known;
    }
    if ((capacity->context_window_known &&
         capacity->max_context_window_known &&
         capacity->context_window_tokens >
             capacity->max_context_window_tokens) ||
        (capacity->context_window_known &&
         capacity->input_context_window_known &&
         capacity->input_context_window_tokens >
             capacity->context_window_tokens) ||
        (capacity->context_window_known && capacity->max_input_known &&
         capacity->max_input_tokens > capacity->context_window_tokens) ||
        (capacity->context_window_known && capacity->max_output_known &&
         capacity->max_output_tokens > capacity->context_window_tokens) ||
        (capacity->context_window_known && capacity->max_input_known &&
         capacity->max_output_known &&
         capacity->max_input_tokens > capacity->context_window_tokens -
                                      capacity->max_output_tokens)) {
        snj_errorf(error, error_size,
                  "contradictory capacity limits for %s/%s",
                  provider->name, model);
        errno = EINVAL;
        return -1;
    }
    if (!override && catalog_used)
        capacity->source = SNJ_CAPACITY_CATALOG;
    if (capacity->max_input_known)
        minimum_budget(capacity->max_input_tokens, &capacity->hard_input_tokens,
                       &capacity->hard_input_known);
    if (capacity->input_context_window_known)
        minimum_budget(capacity->input_context_window_tokens,
                       &capacity->hard_input_tokens,
                       &capacity->hard_input_known);
    if (capacity->context_window_known) {
        uint64_t context_budget = capacity->context_window_tokens;

        if (capacity->max_output_known)
            context_budget -= capacity->max_output_tokens;
        minimum_budget(context_budget, &capacity->hard_input_tokens,
                       &capacity->hard_input_known);
        if (!capacity->effective_context_window_known) {
            if (capacity->codex_protocol) {
                capacity->effective_context_window_percent = 95u;
                capacity->effective_context_window_known = true;
                capacity->effective_context_window_derived = true;
            } else if (!capacity->max_output_known) {
                capacity->effective_context_window_percent = 90u;
                capacity->effective_context_window_known = true;
                capacity->effective_context_window_derived = true;
            }
        }
        if (capacity->effective_context_window_known) {
            uint64_t effective_budget = capacity->context_window_tokens *
                capacity->effective_context_window_percent / 100u;

            minimum_budget(effective_budget, &capacity->hard_input_tokens,
                           &capacity->hard_input_known);
        }
    }
    if (cached_model) {
        const char *state = snj_json_string(cached_model, "count_capability");
        uint64_t observed_hard;
        uint64_t observed_bytes;
        uint64_t observed_tokens;
        if (strcmp(state, "supported") == 0)
            capacity->count_capability = SNJ_COUNT_SUPPORTED;
        else if (strcmp(state, "unsupported") == 0)
            capacity->count_capability = SNJ_COUNT_UNSUPPORTED;
        else
            capacity->count_capability = SNJ_COUNT_UNKNOWN;
        observed_bytes = (uint64_t)json_integer_value(
            json_object_get(cached_model, "observed_model_input_bytes"));
        observed_tokens = (uint64_t)json_integer_value(
            json_object_get(cached_model, "observed_input_tokens"));
        if (observed_bytes) {
            uint64_t whole = observed_tokens / observed_bytes;
            uint64_t remainder = observed_tokens % observed_bytes;

            capacity->observed_tokens_per_million_bytes =
                whole * UINT64_C(1000000) +
                (remainder * UINT64_C(1000000) + observed_bytes - 1u) /
                    observed_bytes;
        }
        observed_hard = (uint64_t)json_integer_value(
            json_object_get(cached_model, "observed_hard_input_tokens"));
        if (observed_hard && (!capacity->hard_input_known ||
                             observed_hard < capacity->hard_input_tokens)) {
            capacity->hard_input_tokens = observed_hard;
            capacity->hard_input_known = true;
            capacity->source = SNJ_CAPACITY_OBSERVED;
        }
    }
    return 0;
}

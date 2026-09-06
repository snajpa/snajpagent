/* SPDX-License-Identifier: GPL-2.0-only */
#include "model_cache.h"
#include "fs.h"

#include "base.h"
#include "json.h"
#include "store_internal.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SNAG_MODEL_CACHE_FILE_MAX (8u * 1024u * 1024u)
#define SNAG_MODEL_CACHE_MODELS_MAX 4096u
#define SNAG_MODEL_CACHE_EFFORTS_MAX 32u
#define SNAG_MODEL_CACHE_ENTRIES_MAX 32768u
#define SNAG_MODEL_CACHE_INPUT_MAX (32u * 1024u * 1024u)
#define SNAG_MODEL_CACHE_SCHEMA 1u

void
snag_model_cache_init(struct snag_model_cache *cache)
{
    memset(cache, 0, sizeof(*cache));
}

void
snag_model_cache_free(struct snag_model_cache *cache)
{
    if (cache->providers)
        json_decref(cache->providers);
    snag_model_cache_init(cache);
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
           snag_utf8_valid((const unsigned char *)text, len, true);
}

/* Advertised/configured limits are positive; zero is internal absence only. */
static bool
nullable_limit(const json_t *object, const char *key, uint64_t max,
               uint64_t *value)
{
    json_t *entry = json_object_get(object, key);
    json_int_t integer;

    *value = 0u;
    if (json_is_null(entry))
        return true;
    if (!json_is_integer(entry) || (integer = json_integer_value(entry)) <= 0 ||
        (uint64_t)integer > max)
        return false;
    *value = (uint64_t)integer;
    return true;
}

static bool
capacity_limits_valid(const struct snag_model_capacity *c)
{
    return !c->context_window_tokens ||
        ((!c->max_context_window_tokens ||
          c->context_window_tokens <= c->max_context_window_tokens) &&
         c->input_context_window_tokens <= c->context_window_tokens &&
         c->max_input_tokens <= c->context_window_tokens &&
         c->max_output_tokens <= c->context_window_tokens &&
         c->max_input_tokens <= c->context_window_tokens - c->max_output_tokens &&
         c->auto_compact_input_tokens <= c->context_window_tokens);
}

static bool
read_limits(const json_t *limits, struct snag_model_capacity *c)
{
    static const char *const keys[] = {
        "auto_compact_input_tokens", "context_window_tokens",
        "effective_context_window_percent", "input_context_window_tokens",
        "max_context_window_tokens", "max_input_tokens", "max_output_tokens"
    };
    uint64_t percent = 0u;

    if (!snag_json_exact_keys(limits, keys, sizeof(keys) / sizeof(keys[0])) ||
        !nullable_limit(limits, "context_window_tokens",
                        SNAG_CONFIG_TOKEN_LIMIT_MAX, &c->context_window_tokens) ||
        !nullable_limit(limits, "max_context_window_tokens",
                        SNAG_CONFIG_TOKEN_LIMIT_MAX, &c->max_context_window_tokens) ||
        !nullable_limit(limits, "input_context_window_tokens",
                        SNAG_CONFIG_TOKEN_LIMIT_MAX, &c->input_context_window_tokens) ||
        !nullable_limit(limits, "max_input_tokens",
                        SNAG_CONFIG_TOKEN_LIMIT_MAX, &c->max_input_tokens) ||
        !nullable_limit(limits, "max_output_tokens",
                        SNAG_CONFIG_TOKEN_LIMIT_MAX, &c->max_output_tokens) ||
        !nullable_limit(limits, "auto_compact_input_tokens",
                        SNAG_CONFIG_TOKEN_LIMIT_MAX, &c->auto_compact_input_tokens) ||
        !nullable_limit(limits, "effective_context_window_percent", 100u, &percent))
        return false;
    c->effective_context_window_percent = (unsigned int)percent;
    return capacity_limits_valid(c);
}

static bool
limits_valid(const json_t *limits)
{
    struct snag_model_capacity capacity = {0};
    return read_limits(limits, &capacity);
}

static bool
accounting_valid(const json_t *model)
{
    uint64_t hard;
    uint64_t tokens;
    uint64_t bytes;
    const char *state = snag_json_string(model, "count_capability");

    return state &&
           (strcmp(state, "unknown") == 0 ||
            strcmp(state, "supported") == 0 ||
            strcmp(state, "unsupported") == 0) &&
           snag_json_integer_u64(model, "observed_hard_input_tokens",
                                &hard) == 0 &&
           snag_json_integer_u64(model, "observed_input_tokens",
                                &tokens) == 0 &&
           snag_json_integer_u64(model, "observed_input_bytes",
                                &bytes) == 0 &&
           hard <= SNAG_CONFIG_TOKEN_LIMIT_MAX &&
           tokens <= SNAG_CONFIG_TOKEN_LIMIT_MAX &&
           bytes <= SNAG_MODEL_CACHE_INPUT_MAX && !!tokens == !!bytes;
}

static bool
model_valid(const json_t *model, bool cached)
{
    static const char *const catalog_keys[] = {
        "default_effort", "efforts", "id", "limits"
    };
    static const char *const cache_keys[] = {"count_capability", "default_effort",
        "efforts", "id", "limits", "observed_hard_input_tokens",
        "observed_input_tokens", "observed_input_bytes"};
    json_t *fallback;
    json_t *efforts;

    if (!json_is_object(model) || !snag_json_exact_keys((json_t *)model,
            cached ? cache_keys : catalog_keys, cached ? 8u : 4u) ||
        !cache_string(json_object_get(model, "id"), SNAG_CONFIG_MODEL_MAX - 1u) ||
        !limits_valid(json_object_get(model, "limits")))
        return false;
    if (cached && !accounting_valid(model))
        return false;
    fallback = json_object_get(model, "default_effort");
    if (!json_is_null(fallback) &&
        !cache_string(fallback, SNAG_CONFIG_EFFORT_MAX - 1u))
        return false;
    efforts = json_object_get(model, "efforts");
    if (!json_is_array(efforts) ||
        json_array_size(efforts) > SNAG_MODEL_CACHE_EFFORTS_MAX)
        return false;
    for (size_t i = 0; i < json_array_size(efforts); ++i) {
        json_t *effort = json_array_get(efforts, i);
        if (!cache_string(effort, SNAG_CONFIG_EFFORT_MAX - 1u))
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
        json_array_size(providers) > SNAG_CONFIG_PROVIDER_MAX)
        return false;
    for (size_t i = 0; i < json_array_size(providers); ++i) {
        json_t *provider = json_array_get(providers, i);
        json_t *models;
        const char *name;
        const char *protocol;

        if (!json_is_object(provider) ||
            !snag_json_exact_keys(provider, keys, 4u) ||
            !cache_string(json_object_get(provider, "name"),
                          SNAG_CONFIG_PROVIDER_NAME_MAX) ||
            !cache_string(json_object_get(provider, "base_url"),
                          SNAG_CONFIG_URL_MAX) ||
            !cache_string(json_object_get(provider, "protocol"), 6u) ||
            !(protocol = snag_json_string(provider, "protocol")) ||
            (strcmp(protocol, "codex") != 0 &&
             strcmp(protocol, "openai") != 0) ||
            !(name = snag_json_string(provider, "name")) ||
            !json_is_array((models = json_object_get(provider, "models"))) ||
            json_array_size(models) >
                SNAG_MODEL_CACHE_MODELS_MAX - total_models)
            return false;
        for (size_t j = 0; j < i; ++j)
            if (strcmp(snag_json_string(json_array_get(providers, j), "name"),
                       name) == 0)
                return false;
        total_models += json_array_size(models);
        for (size_t j = 0; j < json_array_size(models); ++j) {
            json_t *model = json_array_get(models, j);
            json_t *efforts;
            size_t variants;
            const char *id;
            if (!model_valid(model, cached) ||
                !(id = snag_json_string(model, "id")))
                return false;
            efforts = json_object_get(model, "efforts");
            variants = json_array_size(efforts);
            if (variants == 0u)
                variants = 1u;
            if (variants > SNAG_MODEL_CACHE_ENTRIES_MAX - total_entries)
                return false;
            total_entries += variants;
            for (size_t k = 0; k < j; ++k)
                if (strcmp(snag_json_string(json_array_get(models, k), "id"),
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
        if (strcmp(snag_json_string(entry, "name"), name) == 0)
            return entry;
    }
    return NULL;
}

static int
decode_cache(const unsigned char *data, size_t len,
             struct snag_model_cache *cache, char *error, size_t error_size)
{
    static const char *const keys[] = {"providers", "schema_version", "updated_at_ms"};
    json_t *root;
    json_t *providers;
    json_t *copy;
    uint64_t updated;
    uint64_t schema;

    root = snag_json_load_strict(data, len, SNAG_MODEL_CACHE_FILE_MAX,
                                error, error_size);
    if (!root || !json_is_object(root) ||
        !snag_json_exact_keys(root, keys, 3u) ||
        snag_json_integer_u64(root, "schema_version", &schema) < 0 ||
        schema != SNAG_MODEL_CACHE_SCHEMA ||
        snag_json_integer_u64(root, "updated_at_ms", &updated) < 0 ||
        updated == 0u ||
        !providers_valid((providers = json_object_get(root, "providers")), true)) {
        snag_errorf(error, error_size,
                  "model cache is unusable; use /model cache while idle");
        if (root)
            json_decref(root);
        errno = EINVAL;
        return -1;
    }
    copy = json_incref(providers);
    json_decref(root);
    snag_model_cache_free(cache);
    cache->providers = copy;
    cache->updated_at_ms = updated;
    return 0;
}

int
snag_model_cache_load(struct snag_store *store, struct snag_model_cache *cache,
                     char *error, size_t error_size)
{
    snag_file_info st;
    struct snag_buf data;
    int fd;
    int rc = -1;

    if (!store || store->root_fd < 0 || !cache) {
        errno = EINVAL;
        return -1;
    }
    fd = snag_open_read_security_at(store->root_fd, "models.json", false);
    if (fd < 0) {
        if (errno == ENOENT)
            return 1;
        snag_errorf(error, error_size, "cannot open model cache: %s", strerror(errno));
        return -1;
    }
    snag_buf_init(&data, SNAG_MODEL_CACHE_FILE_MAX);
    struct snag_file_privacy privacy;
    if (snag_fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || snag_fd_privacy(fd, &privacy) < 0 ||
        !privacy.real_owner || !privacy.private_access || st.st_size <= 0 ||
        (uintmax_t)st.st_size > SNAG_MODEL_CACHE_FILE_MAX) {
        snag_errorf(error, error_size,
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
            snag_errorf(error, error_size, "cannot read model cache: %s", strerror(errno));
            goto out;
        }
        if (got == 0)
            break;
        if (snag_buf_append(&data, chunk, (size_t)got) < 0) {
            snag_errorf(error, error_size, "model cache exceeds 8 MiB");
            goto out;
        }
    }
    rc = decode_cache(data.data, data.len, cache, error, error_size);
out:
    {
        int saved = errno;
        snag_buf_free(&data);
        (void)close(fd);
        errno = saved;
    }
    return rc;
}

static int
lock_cache(struct snag_store *store, char *error, size_t error_size)
{
    int fd;
    int saved;

    fd = snag_create_private_at(store->root_fd, "models.lock", false);
    if (fd < 0) {
        snag_errorf(error, error_size, "cannot open model cache lock: %s",
                  strerror(errno));
        return -1;
    }
    if (snag_store_verify_private_fd(fd, false, "model cache lock",
                                    error, error_size) < 0)
        goto fail;
    if (snag_lock_file(fd, true) < 0) {
        snag_errorf(error, error_size, "cannot lock model cache: %s",
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
write_cache(struct snag_store *store, const json_t *providers,
            uint64_t updated_at_ms, struct snag_model_cache *cache,
            char *error, size_t error_size)
{
    struct snag_buf data;
    json_t *root = NULL;
    char id[SNAG_ID_HEX_LEN + 1u];
    char tmp_name[64] = {0};
    int fd = -1;
    int rc = -1;
    int saved;

    if (!store || store->root_fd < 0 || !cache || !updated_at_ms ||
        updated_at_ms > (uint64_t)INT64_MAX ||
        !providers_valid(providers, true)) {
        snag_errorf(error, error_size, "refusing to write an invalid model cache");
        errno = EINVAL;
        return -1;
    }
    root = json_pack("{s:O,s:i,s:I}", "providers", providers,
                     "schema_version", SNAG_MODEL_CACHE_SCHEMA,
                     "updated_at_ms", (json_int_t)updated_at_ms);
    snag_buf_init(&data, SNAG_MODEL_CACHE_FILE_MAX);
    if (!root ||
        snag_json_canonical(root, &data) < 0 || snag_buf_putc(&data, '\n') < 0 ||
        snag_random_id(id) < 0) {
        snag_errorf(error, error_size, "cannot encode model cache");
        goto out;
    }
    (void)snprintf(tmp_name, sizeof(tmp_name), "models.json.tmp.%s", id);
    fd = snag_create_private_at(store->root_fd, tmp_name, true);
    if (fd < 0) {
        snag_errorf(error, error_size, "cannot create model cache: %s",
                  strerror(errno));
        goto out;
    }
    if (snag_write_full(fd, data.data, data.len) < 0 ||
        snag_sync_file(fd) < 0) {
        snag_errorf(error, error_size, "cannot write model cache: %s",
                  strerror(errno));
        goto out;
    }
    if (close(fd) < 0) {
        fd = -1;
        snag_errorf(error, error_size, "cannot close model cache: %s",
                  strerror(errno));
        goto out;
    }
    fd = -1;
    if (snag_rename_at(store->root_fd, tmp_name, store->root_fd, "models.json") < 0) {
        snag_errorf(error, error_size, "cannot install model cache: %s",
                  strerror(errno));
        goto out;
    }
    tmp_name[0] = '\0';
    if (snag_sync_dir(store->root_fd) < 0) {
        snag_errorf(error, error_size, "cannot sync model cache directory: %s",
                  strerror(errno));
        goto out;
    }
    /* Keep one reference after the root object releases its reference. */
    json_incref((json_t *)providers);
    snag_model_cache_free(cache);
    cache->providers = (json_t *)providers;
    cache->updated_at_ms = updated_at_ms;
    rc = 0;
out:
    saved = errno;
    if (fd >= 0)
        (void)close(fd);
    if (rc < 0 && tmp_name[0])
        (void)snag_unlink_at(store->root_fd, tmp_name, false);
    if (root)
        json_decref(root);
    snag_buf_free(&data);
    errno = saved;
    return rc;
}

static int
prepare_accounting(json_t *model, const json_t *old)
{
    static const char *const keys[] = {
        "observed_hard_input_tokens", "observed_input_tokens",
        "observed_input_bytes"
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
        json_object_set_new(model, "observed_input_bytes",
                            json_integer(0)) < 0)
        return -1;
    return 0;
}

int
snag_model_cache_replace(struct snag_store *store, const json_t *providers,
                        uint64_t updated_at_ms, struct snag_model_cache *cache,
                        char *error, size_t error_size)
{
    struct snag_model_cache previous = {0};
    json_t *prepared = NULL;
    int lock_fd;
    int rc = -1;

    if (!store || store->root_fd < 0 || !cache || !updated_at_ms ||
        updated_at_ms > (uint64_t)INT64_MAX ||
        !providers_valid(providers, false)) {
        snag_errorf(error, error_size, "invalid model cache replacement");
        errno = EINVAL;
        return -1;
    }
    lock_fd = lock_cache(store, error, error_size);
    if (lock_fd < 0)
        return -1;
    if (snag_model_cache_load(store, &previous, error, error_size) < 0 &&
        errno != EINVAL)
        goto out;
    prepared = json_deep_copy(providers);
    if (!prepared) {
        snag_errorf(error, error_size, "cannot copy model catalog");
        errno = ENOMEM;
        goto out;
    }
    for (size_t i = 0; i < json_array_size(prepared); ++i) {
        json_t *after = json_array_get(prepared, i);
        const char *name = snag_json_string(after, "name");
        const json_t *before = provider_entry(previous.providers, name);
        bool bound = before &&
            strcmp(snag_json_string(before, "base_url"),
                   snag_json_string(after, "base_url")) == 0 &&
            strcmp(snag_json_string(before, "protocol"),
                   snag_json_string(after, "protocol")) == 0;
        json_t *models = json_object_get(after, "models");

        for (size_t j = 0; j < json_array_size(models); ++j) {
            json_t *model = json_array_get(models, j);
            const json_t *old_model = bound ?
                snag_model_cache_find(&previous, name,
                                     snag_json_string(model, "id")) : NULL;

            if (prepare_accounting(model, old_model) < 0) {
                snag_errorf(error, error_size,
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
    snag_model_cache_free(&previous);
    (void)close(lock_fd);
    return rc;
}

int
snag_model_cache_record(struct snag_store *store, struct snag_model_cache *cache,
                       const struct snag_provider_config *provider,
                       const char *protocol, const char *model,
                       enum snag_count_capability capability,
                       uint64_t model_input_bytes, uint64_t input_tokens,
                       uint64_t hard_input_tokens,
                       char *error, size_t error_size)
{
    struct snag_model_cache staged = {0};
    const json_t *source;
    const json_t *item;
    const char *current_state;
    const char *next_state = NULL;
    uint64_t value = 0u;
    uint64_t largest_bytes = 0u;
    uint64_t largest_tokens = 0u;
    bool capability_changed;
    bool sample_changed;
    bool hard_limit_changed;
    int lock_fd;
    int rc = 1;

    if (!store || !cache || !provider || !protocol || !model || !*model ||
        capability > SNAG_COUNT_UNSUPPORTED ||
        ((model_input_bytes == 0u) != (input_tokens == 0u)) ||
        input_tokens > SNAG_CONFIG_TOKEN_LIMIT_MAX ||
        model_input_bytes > SNAG_MODEL_CACHE_INPUT_MAX ||
        hard_input_tokens > SNAG_CONFIG_TOKEN_LIMIT_MAX ||
        (capability == SNAG_COUNT_UNKNOWN && !input_tokens && !hard_input_tokens)) {
        errno = EINVAL;
        return -1;
    }
    model = snag_config_model_upstream(provider, model);
    lock_fd = lock_cache(store, error, error_size);
    if (lock_fd < 0)
        return -1;
    rc = snag_model_cache_load(store, &staged, error, error_size);
    if (rc != 0)
        goto out;
    rc = 1;
    source = provider_entry(staged.providers, provider->name);
    item = snag_model_cache_find(&staged, provider->name, model);
    if (!source ||
        strcmp(snag_json_string(source, "base_url"), provider->base_url) ||
        strcmp(snag_json_string(source, "protocol"), protocol) || !item)
        goto adopt;
    current_state = snag_json_string(item, "count_capability");
    if (capability != SNAG_COUNT_UNKNOWN)
        next_state = capability == SNAG_COUNT_SUPPORTED ?
            "supported" : "unsupported";
    capability_changed = next_state && strcmp(current_state, next_state) != 0;
    (void)snag_json_integer_u64(item, "observed_input_bytes",
                               &largest_bytes);
    (void)snag_json_integer_u64(item, "observed_input_tokens",
                               &largest_tokens);
    (void)snag_json_integer_u64(item, "observed_hard_input_tokens", &value);
    sample_changed = input_tokens &&
        (model_input_bytes > largest_bytes ||
         (model_input_bytes == largest_bytes && input_tokens > largest_tokens));
    hard_limit_changed = hard_input_tokens &&
        (!value || hard_input_tokens < value);
    if (!capability_changed && !sample_changed && !hard_limit_changed) {
        rc = 0;
        goto adopt;
    }
    if (capability_changed &&
        json_object_set_new((json_t *)item, "count_capability",
                            json_string(next_state)) < 0)
        goto write_error;
    if (sample_changed &&
        (json_object_set_new((json_t *)item, "observed_input_bytes",
                             json_integer((json_int_t)model_input_bytes)) < 0 ||
         json_object_set_new((json_t *)item, "observed_input_tokens",
                             json_integer((json_int_t)input_tokens)) < 0))
        goto write_error;
    if (hard_limit_changed &&
        json_object_set_new((json_t *)item, "observed_hard_input_tokens",
                            json_integer((json_int_t)hard_input_tokens)) < 0)
        goto write_error;
    rc = write_cache(store, staged.providers, staged.updated_at_ms, cache,
                     error, error_size);
    goto out;
adopt:
    snag_model_cache_free(cache);
    *cache = staged;
    snag_model_cache_init(&staged);
    goto out;
write_error:
    snag_errorf(error, error_size, "cannot update model cache observation");
    errno = ENOMEM;
    rc = -1;
out:
    snag_model_cache_free(&staged);
    (void)close(lock_fd);
    return rc;
}

const json_t *
snag_model_cache_find(const struct snag_model_cache *cache,
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
        if (strcmp(snag_json_string(json_array_get(models, i), "id"),
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
snag_model_cache_best_effort(const json_t *model, const char *fallback)
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


const json_t *
snag_model_metadata(const struct snag_model_cache *cache,
                     const struct snag_provider_config *provider, const char *model)
{
    const json_t *entry = provider ? provider_entry(cache ? cache->providers : NULL, provider->name) : NULL;
    if (!entry || strcmp(snag_json_string(entry, "base_url"), provider->base_url))
        return NULL;
    return snag_model_cache_find(cache, provider->name, snag_config_model_upstream(provider, model));
}

static int
visit_model(const json_t *metadata, const char *provider, const char *model,
             const char *fallback, size_t *index, snag_model_entry_fn visit, void *opaque)
{
    const json_t *efforts = metadata ? json_object_get(metadata, "efforts") : NULL;
    size_t variants = json_array_size(efforts);
    for (size_t i = 0; i < (variants ? variants : 1u); ++i) {
        const char *effort = variants ? json_string_value(json_array_get(efforts, i)) :
                            snag_model_cache_best_effort(metadata, fallback);
        int rc = visit(opaque, ++*index, provider, model, effort, metadata);
        if (rc)
            return rc;
    }
    return 0;
}

int
snag_model_each(const struct snag_model_cache *cache, const struct snag_config *config,
                const char *fallback_effort, snag_model_entry_fn visit, void *opaque)
{
    size_t index = 0u;
    for (size_t i = 0; i < json_array_size(cache->providers); ++i) {
        const json_t *entry = json_array_get(cache->providers, i);
        const char *name = snag_json_string(entry, "name");
        const struct snag_provider_config *provider = snag_config_provider(config, name);
        const json_t *models = json_object_get(entry, "models");
        if (!provider)
            continue;
        for (size_t j = 0; j < json_array_size(models); ++j) {
            const json_t *metadata = json_array_get(models, j);
            const char *model = snag_json_string(metadata, "id");
            bool defined = false;
            for (size_t k = 0; k < provider->model_count; ++k)
                if (!strcmp(provider->models[k].name, model))
                    defined = true;
            if (!defined) {
                int rc = visit_model(metadata, name, model, fallback_effort, &index, visit, opaque);
                if (rc)
                    return rc;
            }
        }
    }
    for (size_t i = 0; i < config->provider_count; ++i) {
        const struct snag_provider_config *provider = &config->providers[i];
        for (size_t j = 0; j < provider->model_count; ++j) {
            const char *model = provider->models[j].name;
            int rc = visit_model(snag_model_metadata(cache, provider, model), provider->name,
                                  model, fallback_effort, &index, visit, opaque);
            if (rc)
                return rc;
        }
    }
    return 0;
}

struct model_selection {
    size_t index;
    const char **provider;
    const char **model;
    const char **effort;
};

static int
select_model(void *opaque, size_t index, const char *provider, const char *model,
              const char *effort, const json_t *metadata)
{
    struct model_selection *selection = opaque;
    (void)metadata;
    if (index != selection->index)
        return 0;
    *selection->provider = provider;
    *selection->model = model;
    *selection->effort = effort;
    return 1;
}

int
snag_model_entry(const struct snag_model_cache *cache, const struct snag_config *config,
                  size_t index, const char *fallback_effort,
                  const char **provider, const char **model, const char **effort)
{
    struct model_selection selection = {index, provider, model, effort};
    int rc = snag_model_each(cache, config, fallback_effort, select_model, &selection);
    return rc > 0 ? 0 : rc < 0 ? -1 : 1;
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
snag_model_compact_threshold(const struct snag_provider_config *provider,
                            const struct snag_model_capacity *capacity)
{
    uint64_t threshold;

    if (provider->auto_compact_input_tokens != SNAG_CONFIG_COMPACT_AUTO)
        return provider->auto_compact_input_tokens;
    if (!capacity->hard_input_known)
        return 120000u;
    /* Floor 90% without overflowing; only explicit zero disables policy. */
    threshold = capacity->hard_input_tokens / 10u * 9u +
                capacity->hard_input_tokens % 10u * 9u / 10u;
    return threshold ? threshold : 1u;
}

const char *
snag_capacity_source_name(enum snag_capacity_source source)
{
    switch (source) {
    case SNAG_CAPACITY_UNKNOWN: return "unknown";
    case SNAG_CAPACITY_CATALOG: return "advertised";
    case SNAG_CAPACITY_CONFIG: return "configured";
    case SNAG_CAPACITY_OBSERVED: return "observed";
    case SNAG_CAPACITY_STALE_CATALOG: return "stale-catalog-ignored";
    }
    return "unknown";
}

int
snag_model_capacity_resolve(const struct snag_model_cache *cache,
                           const struct snag_config *config,
                           const struct snag_provider_config *provider,
                           const char *model, const char *protocol,
                           struct snag_model_capacity *capacity,
                           char *error, size_t error_size)
{
    struct snag_model_limit_config configured;
    const struct snag_model_limit_config *sources[3];
    bool override;
    const json_t *cached_provider;
    const json_t *cached_model = NULL;
    const json_t *limits = NULL;
    bool catalog_used = false;

    if (!config || !provider || !model || !*model || !protocol || !capacity ||
        (strcmp(protocol, "codex") != 0 && strcmp(protocol, "openai") != 0)) {
        snag_errorf(error, error_size, "invalid model capacity selection");
        errno = EINVAL;
        return -1;
    }
    memset(capacity, 0, sizeof(*capacity));
    cached_provider = provider_entry(cache ? cache->providers : NULL,
                                     provider->name);
    if (cached_provider) {
        capacity->source_bound =
            strcmp(snag_json_string(cached_provider, "base_url"),
                   provider->base_url) == 0 &&
            strcmp(snag_json_string(cached_provider, "protocol"), protocol) == 0;
        if (capacity->source_bound) {
            cached_model = snag_model_cache_find(cache, provider->name,
                                                 snag_config_model_upstream(provider, model));
            if (cached_model)
                limits = json_object_get(cached_model, "limits");
        } else {
            capacity->source = SNAG_CAPACITY_STALE_CATALOG;
            capacity->cache_source_mismatch = true;
        }
    }
    override = snag_config_resolve_limits(config, provider->name, model, &configured, sources);
    if (limits) {
        if (!read_limits(limits, capacity)) {
            snag_errorf(error, error_size, "invalid cached capacity limits");
            errno = EINVAL;
            return -1;
        }
        catalog_used = capacity->context_window_tokens ||
            capacity->max_context_window_tokens ||
            capacity->input_context_window_tokens || capacity->max_input_tokens ||
            capacity->max_output_tokens || capacity->auto_compact_input_tokens ||
            capacity->effective_context_window_percent;
    }
    if (configured.context_window_tokens)
        capacity->context_window_tokens = configured.context_window_tokens;
    if (configured.max_input_tokens)
        capacity->max_input_tokens = configured.max_input_tokens;
    if (capacity->max_input_tokens)
        capacity->input_context_window_tokens = 0u;
    if (configured.max_output_tokens)
        capacity->max_output_tokens = configured.max_output_tokens;
    if (override)
        capacity->source = SNAG_CAPACITY_CONFIG;
    if (capacity->context_window_tokens && capacity->max_output_tokens >= capacity->context_window_tokens) {
        snag_errorf(error, error_size,
                  "output reservation %llu (rule %s) leaves no input in context %llu (rule %s) for %s/%s",
                  (unsigned long long)capacity->max_output_tokens,
                  sources[2] ? (sources[2]->model[0] ? sources[2]->model : "provider-wide") : "catalog",
                  (unsigned long long)capacity->context_window_tokens,
                  sources[0] ? (sources[0]->model[0] ? sources[0]->model : "provider-wide") : "catalog",
                  provider->name, model);
        errno = EINVAL;
        return -1;
    }
    if (!override && catalog_used)
        capacity->source = SNAG_CAPACITY_CATALOG;
    if (capacity->max_input_tokens)
        minimum_budget(capacity->max_input_tokens, &capacity->hard_input_tokens,
                       &capacity->hard_input_known);
    if (capacity->input_context_window_tokens)
        minimum_budget(capacity->input_context_window_tokens,
                       &capacity->hard_input_tokens,
                       &capacity->hard_input_known);
    if (capacity->context_window_tokens) {
        uint64_t context_budget = capacity->context_window_tokens;

        if (capacity->max_output_tokens)
            context_budget -= capacity->max_output_tokens;
        minimum_budget(context_budget, &capacity->hard_input_tokens,
                       &capacity->hard_input_known);
        if (!capacity->effective_context_window_percent) {
            if (strcmp(protocol, "codex") == 0) {
                capacity->effective_context_window_percent = 95u;
                capacity->effective_context_window_derived = true;
            } else if (!capacity->max_output_tokens) {
                capacity->effective_context_window_percent = 90u;
                capacity->effective_context_window_derived = true;
            }
        }
        if (capacity->effective_context_window_percent) {
            uint64_t effective_budget = capacity->context_window_tokens *
                capacity->effective_context_window_percent / 100u;

            minimum_budget(effective_budget, &capacity->hard_input_tokens,
                           &capacity->hard_input_known);
        }
    }
    if (!configured.context_window_tokens && capacity->max_context_window_tokens)
        minimum_budget(capacity->max_context_window_tokens,
                       &capacity->hard_input_tokens, &capacity->hard_input_known);
    if (cached_model) {
        const char *state = snag_json_string(cached_model, "count_capability");
        uint64_t observed_hard;
        uint64_t observed_bytes;
        uint64_t observed_tokens;
        if (strcmp(state, "supported") == 0)
            capacity->count_capability = SNAG_COUNT_SUPPORTED;
        else if (strcmp(state, "unsupported") == 0)
            capacity->count_capability = SNAG_COUNT_UNSUPPORTED;
        else
            capacity->count_capability = SNAG_COUNT_UNKNOWN;
        observed_bytes = (uint64_t)json_integer_value(
            json_object_get(cached_model, "observed_input_bytes"));
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
            capacity->source = SNAG_CAPACITY_OBSERVED;
        }
    }
    return 0;
}

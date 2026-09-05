/* SPDX-License-Identifier: GPL-2.0-only */
#include "config.h"
#include "json.h"
#include "model_cache.h"
#include "store.h"

#include <assert.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <unistd.h>

static json_t *
load_json(const char *text)
{
    char error[128] = {0};
    json_t *value = snag_json_load_strict((const unsigned char *)text,
                                         strlen(text), 65536u,
                                         error, sizeof(error));
    assert(value);
    return value;
}

static void
write_file_at(int dirfd, const char *name, const char *text)
{
    int fd = openat(dirfd, name, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    assert(fd >= 0);
    assert(write(fd, text, strlen(text)) == (ssize_t)strlen(text));
    assert(close(fd) == 0);
}

static void
test_local_models(struct snag_store *store, struct snag_model_cache *cache)
{
    struct snag_config config;
    struct snag_provider_config *provider;
    struct snag_model_capacity capacity;
    const char *name, *model, *effort;
    char error[256] = {0};
    bool saw_small = false, saw_large = false;

    snag_config_init(&config);
    provider = &config.providers[0];
    strcpy(provider->name, "codex");
    strcpy(provider->base_url, "https://chat.example.test/backend-api/codex");
    provider->models = calloc(2u, sizeof(*provider->models));
    assert(provider->models);
    provider->model_count = 2u;
    strcpy(provider->models[0].name, "small");
    strcpy(provider->models[0].upstream, "codex-context-only");
    strcpy(provider->models[1].name, "large");
    strcpy(provider->models[1].upstream, "codex-context-only");
    config.model_limit_count = 3u;
    for (size_t i = 0; i < config.model_limit_count; ++i)
        strcpy(config.model_limits[i].provider, "codex");
    config.model_limits[0].context_window_tokens = 500000u;
    strcpy(config.model_limits[1].model, "small");
    config.model_limits[1].context_window_tokens = 128000u;
    strcpy(config.model_limits[2].model, "codex-context-only");
    config.model_limits[2].max_output_tokens = 4000u;
    assert(snag_model_capacity_resolve(cache, &config, provider, "small", "codex",
                                      &capacity, error, sizeof(error)) == 0);
    assert(capacity.context_window_tokens == 128000u && capacity.hard_input_tokens == 121600u);
    assert(!capacity.max_output_tokens); /* No inheritance from a different local name. */
    config.model_limits[1].max_output_tokens = 128000u;
    assert(snag_model_capacity_resolve(cache, &config, provider, "small", "codex",
                                      &capacity, error, sizeof(error)) < 0);
    assert(strstr(error, "rule small"));
    config.model_limits[1].max_output_tokens = 0u;
    assert(snag_model_capacity_resolve(cache, &config, provider, "large", "codex",
                                      &capacity, error, sizeof(error)) == 0);
    assert(capacity.context_window_tokens == 500000u && capacity.hard_input_tokens == 475000u);
    assert(snag_model_compact_threshold(provider, &capacity) == 427500u);
    for (size_t i = 1u; snag_model_entry(cache, &config, i, "medium", &name, &model, &effort) == 0; ++i) {
        saw_small |= strcmp(model, "small") == 0;
        saw_large |= strcmp(model, "large") == 0;
        assert(strcmp(name, "codex") == 0);
    }
    assert(saw_small && saw_large);
    assert(snag_model_cache_record(store, cache, provider, "codex", "large",
                                  SNAG_COUNT_UNKNOWN, 0u, 0u, 400000u, error, sizeof(error)) == 0);
    assert(snag_model_cache_find(cache, "codex", "large") == NULL);
    assert(snag_model_capacity_resolve(cache, &config, provider, "large", "codex",
                                      &capacity, error, sizeof(error)) == 0);
    assert(capacity.hard_input_tokens == 400000u);
    strcpy(provider->models[1].upstream, "unknown");
    assert(snag_model_capacity_resolve(cache, &config, provider, "large", "codex",
                                      &capacity, error, sizeof(error)) == 0);
    assert(capacity.hard_input_tokens == 475000u);
    snag_config_free(&config);
}

int
main(void)
{
    static const char providers_text[] =
        "[{\"base_url\":\"https://api.example.test/v1\","
        "\"models\":[{\"default_effort\":\"high\","
        "\"efforts\":[\"low\",\"high\"],\"id\":\"org/model\","
        "\"limits\":{\"auto_compact_input_tokens\":null,"
        "\"context_window_tokens\":1050000,"
        "\"effective_context_window_percent\":null,"
        "\"input_context_window_tokens\":null,"
        "\"max_context_window_tokens\":null,"
        "\"max_input_tokens\":922000,\"max_output_tokens\":128000}},"
        "{\"default_effort\":null,\"efforts\":[],"
        "\"id\":\"context-only\",\"limits\":{"
        "\"auto_compact_input_tokens\":null,"
        "\"context_window_tokens\":100000,"
        "\"effective_context_window_percent\":null,"
        "\"input_context_window_tokens\":null,"
        "\"max_context_window_tokens\":null,"
        "\"max_input_tokens\":null,\"max_output_tokens\":null}},"
        "{\"default_effort\":null,\"efforts\":[],"
        "\"id\":\"unknown\",\"limits\":{"
        "\"auto_compact_input_tokens\":null,"
        "\"context_window_tokens\":null,"
        "\"effective_context_window_percent\":null,"
        "\"input_context_window_tokens\":null,"
        "\"max_context_window_tokens\":null,"
        "\"max_input_tokens\":null,\"max_output_tokens\":null}}],"
        "\"name\":\"paid\",\"protocol\":\"openai\"},"
        "{\"base_url\":\"https://chat.example.test/backend-api/codex\","
        "\"models\":[{\"default_effort\":\"medium\","
        "\"efforts\":[\"medium\"],\"id\":\"codex-context-only\","
        "\"limits\":{\"auto_compact_input_tokens\":null,"
        "\"context_window_tokens\":272000,"
        "\"effective_context_window_percent\":null,"
        "\"input_context_window_tokens\":null,"
        "\"max_context_window_tokens\":872000,"
        "\"max_input_tokens\":null,\"max_output_tokens\":null}}],"
        "\"name\":\"codex\",\"protocol\":\"codex\"}]";
    static const char old_cache[] =
        "{\"format\":1,\"providers\":[],\"updated_at_ms\":1}\n";
    char temp[] = "/tmp/snajpagent-model-cache-XXXXXX";
    char error[256] = {0};
    char encoded[8192];
    struct snag_store store;
    struct snag_model_cache cache;
    struct snag_model_capacity capacity;
    struct snag_config config;
    struct stat before;
    struct stat after;
    json_t *providers;
    int fd;
    ssize_t got;

    assert(mkdtemp(temp));
    snag_store_init(&store);
    assert(snag_store_open(&store, temp, error, sizeof(error)) == 0);
    snag_model_cache_init(&cache);
    write_file_at(store.root_fd, "models.json", old_cache);
    assert(snag_model_cache_load(&store, &cache, error, sizeof(error)) < 0);
    assert(strstr(error, "use /model cache while idle") != NULL);
    assert(cache.providers == NULL);

    providers = load_json(providers_text);
    {
        json_t *limits = json_object_get(json_array_get(json_object_get(
            json_array_get(providers, 0), "models"), 2), "limits");
        static const char *const keys[] = {
            "context_window_tokens", "max_context_window_tokens",
            "input_context_window_tokens", "max_input_tokens",
            "max_output_tokens", "auto_compact_input_tokens",
            "effective_context_window_percent"
        };

        /* Zero is internal absence, not an accepted external limit. */
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
            assert(json_object_set_new(limits, keys[i], json_integer(0)) == 0);
            assert(snag_model_cache_replace(&store, providers, 123456u, &cache,
                                           error, sizeof(error)) < 0);
            assert(json_object_del(limits, keys[i]) == 0);
            assert(snag_model_cache_replace(&store, providers, 123456u, &cache,
                                           error, sizeof(error)) < 0);
            assert(json_object_set_new(limits, keys[i], json_null()) == 0);
        }
    }
    assert(snag_model_cache_replace(&store, providers, 123456u, &cache,
                                   error, sizeof(error)) == 0);
    assert(cache.updated_at_ms == 123456u);
    {
        const char *name, *model, *effort;
        static const char *const expected[][3] = {
            {"paid", "org/model", "low"},
            {"paid", "org/model", "high"},
            {"paid", "context-only", "fallback"},
            {"paid", "unknown", "fallback"},
            {"codex", "codex-context-only", "medium"}
        };

        for (size_t i = 0; i < 5u; ++i) {
            assert(snag_model_cache_entry(&cache, i + 1u, "fallback",
                                         &name, &model, &effort) == 0);
            assert(strcmp(name, expected[i][0]) == 0);
            assert(strcmp(model, expected[i][1]) == 0);
            assert(strcmp(effort, expected[i][2]) == 0);
        }
        assert(snag_model_cache_entry(&cache, 0u, "fallback",
                                     &name, &model, &effort) < 0);
        assert(snag_model_cache_entry(&cache, 6u, "fallback",
                                     &name, &model, &effort) == 1);
    }
    fd = openat(store.root_fd, "models.json", O_RDONLY);
    assert(fd >= 0);
    got = read(fd, encoded, sizeof(encoded) - 1u);
    assert(got > 0);
    encoded[got] = '\0';
    assert(close(fd) == 0);
    assert(strstr(encoded, "\"format\"") == NULL);
    assert(strstr(encoded, "\"schema_version\":1") != NULL);
    assert(strstr(encoded, "\"count_capability\":\"unknown\"") != NULL);
    assert(strstr(encoded, "\"observed_input_tokens\":0") != NULL);
    assert(strstr(encoded, "\"limits\"") != NULL);
    snag_model_cache_free(&cache);
    assert(snag_model_cache_load(&store, &cache, error, sizeof(error)) == 0);

    snag_config_init(&config);
    assert(snprintf(config.providers[0].name,
                    sizeof(config.providers[0].name), "%s", "paid") > 0);
    assert(snprintf(config.providers[0].base_url,
                    sizeof(config.providers[0].base_url), "%s",
                    "https://api.example.test/v1") > 0);
    assert(snag_model_capacity_resolve(&cache, &config, &config.providers[0],
               "org/model", "openai", &capacity,
               error, sizeof(error)) == 0);
    assert(capacity.source == SNAG_CAPACITY_CATALOG);
    assert(capacity.source_bound);
    assert(capacity.context_window_tokens == 1050000u);
    assert(capacity.max_input_tokens == 922000u);
    assert(capacity.max_output_tokens == 128000u);
    assert(capacity.hard_input_known);
    assert(capacity.hard_input_tokens == 922000u);
    assert(snag_model_compact_threshold(&config.providers[0], &capacity) ==
           829800u);
    assert(!capacity.effective_context_window_percent);

    assert(snag_model_capacity_resolve(&cache, &config, &config.providers[0],
               "context-only", "openai", &capacity,
               error, sizeof(error)) == 0);
    assert(capacity.source == SNAG_CAPACITY_CATALOG);
    assert(capacity.context_window_tokens);
    assert(capacity.effective_context_window_derived);
    assert(capacity.effective_context_window_percent == 90u);
    assert(capacity.hard_input_tokens == 90000u);
    assert(snag_model_compact_threshold(&config.providers[0], &capacity) ==
           81000u);

    assert(snag_model_capacity_resolve(&cache, &config, &config.providers[0],
               "unknown", "openai", &capacity,
               error, sizeof(error)) == 0);
    assert(capacity.source == SNAG_CAPACITY_UNKNOWN);
    assert(capacity.source_bound);
    assert(!capacity.hard_input_known);
    assert(snag_model_compact_threshold(&config.providers[0], &capacity) ==
           120000u);

    assert(snprintf(config.providers[1].name,
                    sizeof(config.providers[1].name), "%s", "codex") > 0);
    assert(snprintf(config.providers[1].base_url,
                    sizeof(config.providers[1].base_url), "%s",
                    "https://chat.example.test/backend-api/codex") > 0);
    assert(snag_model_capacity_resolve(&cache, &config, &config.providers[1],
               "codex-context-only", "codex", &capacity,
               error, sizeof(error)) == 0);
    assert(capacity.source == SNAG_CAPACITY_CATALOG);
    assert(capacity.context_window_tokens == 272000u);
    assert(capacity.max_context_window_tokens == 872000u);
    assert(capacity.effective_context_window_derived);
    assert(capacity.effective_context_window_percent == 95u);
    assert(capacity.hard_input_tokens == 258400u);
    config.providers[1].auto_compact_input_tokens = SNAG_CONFIG_COMPACT_AUTO;
    assert(snag_model_compact_threshold(&config.providers[1], &capacity) ==
           232560u);
    {
        struct snag_model_capacity bigger;
        struct snag_model_limit_config *limit = &config.model_limits[0];

        config.model_limit_count = 1u;
        strcpy(limit->provider, "codex");
        strcpy(limit->model, "codex-context-only");
        limit->context_window_tokens = 872000u;
        assert(snag_model_capacity_resolve(&cache, &config, &config.providers[1],
                   "codex-context-only", "codex", &bigger,
                   error, sizeof(error)) == 0);
        assert(bigger.hard_input_tokens == 828400u);
        assert(snag_model_compact_threshold(&config.providers[1], &bigger) ==
               745560u);
        config.providers[1].auto_compact_input_tokens = 120000u;
        assert(snag_model_compact_threshold(&config.providers[1], &bigger) ==
               120000u);
        config.providers[1].auto_compact_input_tokens = 0u;
        assert(snag_model_compact_threshold(&config.providers[1], &bigger) == 0u);
        config.providers[1].auto_compact_input_tokens = SNAG_CONFIG_COMPACT_AUTO;
        bigger.hard_input_tokens = 11u;
        assert(snag_model_compact_threshold(&config.providers[1], &bigger) == 9u);
        bigger.hard_input_tokens = 1u;
        assert(snag_model_compact_threshold(&config.providers[1], &bigger) == 1u);
        bigger.hard_input_tokens = UINT64_MAX;
        assert(snag_model_compact_threshold(&config.providers[1], &bigger) ==
               UINT64_C(16602069666338596453));
        memset(limit, 0, sizeof(*limit));
    }

    config.model_limit_count = 1u;
    assert(snprintf(config.model_limits[0].provider,
                    sizeof(config.model_limits[0].provider), "%s", "paid") > 0);
    assert(snprintf(config.model_limits[0].model,
                    sizeof(config.model_limits[0].model), "%s", "org/model") > 0);
    config.model_limits[0].max_input_tokens = 900000u;
    assert(snag_model_capacity_resolve(&cache, &config, &config.providers[0],
               "org/model", "openai", &capacity,
               error, sizeof(error)) == 0);
    assert(capacity.source == SNAG_CAPACITY_CONFIG);
    assert(capacity.source_bound);
    assert(capacity.context_window_tokens == 1050000u);
    assert(capacity.max_output_tokens == 128000u);
    assert(capacity.hard_input_tokens == 900000u);
    assert(snag_model_compact_threshold(&config.providers[0], &capacity) ==
           810000u);

    config.model_limits[0].max_input_tokens = 1100000u;
    assert(snag_model_capacity_resolve(&cache, &config, &config.providers[0],
               "org/model", "openai", &capacity,
               error, sizeof(error)) == 0);
    assert(capacity.source == SNAG_CAPACITY_CONFIG);
    assert(capacity.context_window_tokens && capacity.max_output_tokens);
    assert(capacity.hard_input_tokens == 922000u);
    assert(snag_model_compact_threshold(&config.providers[0], &capacity) ==
           829800u);

    config.model_limits[0].max_input_tokens = 0u;
    config.model_limits[0].context_window_tokens = 100u;
    config.model_limits[0].max_output_tokens = 99u;
    assert(snag_model_capacity_resolve(&cache, &config, &config.providers[0],
               "org/model", "openai", &capacity,
               error, sizeof(error)) == 0);
    assert(capacity.hard_input_known && capacity.hard_input_tokens == 1u);
    assert(snag_model_compact_threshold(&config.providers[0], &capacity) == 1u);
    config.model_limits[0].max_output_tokens = 100u;
    assert(snag_model_capacity_resolve(&cache, &config, &config.providers[0],
               "org/model", "openai", &capacity,
               error, sizeof(error)) < 0);

    config.model_limit_count = 0u;
    {
        struct rlimit saved, limited;
        struct snag_model_cache disk = {0};
        json_t *original = cache.providers;
        json_t *snapshot = json_deep_copy(original);
        void (*previous_signal)(int) = signal(SIGXFSZ, SIG_IGN);

        assert(snapshot && previous_signal != SIG_ERR);
        assert(fstatat(store.root_fd, "models.json", &before, 0) == 0);
        assert(getrlimit(RLIMIT_FSIZE, &saved) == 0);
        limited = saved;
        limited.rlim_cur = 1u;
        assert(setrlimit(RLIMIT_FSIZE, &limited) == 0);
        assert(snag_model_cache_record(&store, &cache, &config.providers[0],
                   "openai", "org/model", SNAG_COUNT_SUPPORTED,
                   1000u, 250u, 800000u, error, sizeof(error)) < 0);
        assert(cache.providers == original && json_equal(original, snapshot));
        assert(snag_model_cache_replace(&store, providers, 234567u, &cache,
                                       error, sizeof(error)) < 0);
        assert(cache.providers == original && json_equal(original, snapshot));
        assert(setrlimit(RLIMIT_FSIZE, &saved) == 0);
        assert(signal(SIGXFSZ, previous_signal) != SIG_ERR);
        assert(fstatat(store.root_fd, "models.json", &after, 0) == 0);
        assert(before.st_ino == after.st_ino && before.st_size == after.st_size);
        assert(snag_model_cache_load(&store, &disk, error, sizeof(error)) == 0);
        assert(json_equal(disk.providers, snapshot));
        snag_model_cache_free(&disk);
        json_decref(snapshot);
    }
    {
        struct snag_model_cache stale = {0};
        json_t *retained = json_incref(cache.providers);

        assert(snag_model_cache_load(&store, &stale, error, sizeof(error)) == 0);
        assert(snag_model_cache_record(&store, &cache, &config.providers[0],
                   "openai", "org/model", SNAG_COUNT_SUPPORTED,
                   1000u, 250u, 800000u, error, sizeof(error)) == 0);
        assert(json_equal(retained, stale.providers));
        assert(!json_equal(retained, cache.providers));
        /* A no-op still adopts changes made by another cache owner. */
        assert(snag_model_cache_record(&store, &stale, &config.providers[0],
                   "openai", "org/model", SNAG_COUNT_UNKNOWN,
                   500u, 200u, 850000u, error, sizeof(error)) == 0);
        assert(json_equal(stale.providers, cache.providers));
        snag_model_cache_free(&stale);
        json_decref(retained);
    }
    assert(fstatat(store.root_fd, "models.json", &before,
                   AT_SYMLINK_NOFOLLOW) == 0);
    assert(snag_model_cache_record(&store, &cache, &config.providers[0],
               "openai", "org/model", SNAG_COUNT_UNKNOWN,
               500u, 200u, 850000u, error, sizeof(error)) == 0);
    assert(fstatat(store.root_fd, "models.json", &after,
                   AT_SYMLINK_NOFOLLOW) == 0);
    assert(before.st_dev == after.st_dev && before.st_ino == after.st_ino);
    assert(snag_model_capacity_resolve(&cache, &config, &config.providers[0],
               "org/model", "openai", &capacity,
               error, sizeof(error)) == 0);
    assert(capacity.count_capability == SNAG_COUNT_SUPPORTED);
    assert(capacity.observed_tokens_per_million_bytes == 250000u);
    assert(capacity.hard_input_tokens == 800000u);
    assert(snag_model_compact_threshold(&config.providers[0], &capacity) ==
           720000u);
    assert(capacity.source == SNAG_CAPACITY_OBSERVED);
    assert(snag_model_cache_record(&store, &cache, &config.providers[0],
               "openai", "org/model", SNAG_COUNT_UNKNOWN,
               2000u, 600u, 700000u, error, sizeof(error)) == 0);
    assert(snag_model_capacity_resolve(&cache, &config, &config.providers[0],
               "org/model", "openai", &capacity,
               error, sizeof(error)) == 0);
    assert(capacity.observed_tokens_per_million_bytes == 300000u);
    assert(capacity.hard_input_tokens == 700000u);
    assert(snag_model_compact_threshold(&config.providers[0], &capacity) ==
           630000u);
    assert(snag_model_cache_record(&store, &cache, &config.providers[0],
               "openai", "org/model", SNAG_COUNT_UNKNOWN,
               2000u, 500u, 0u, error, sizeof(error)) == 0);
    assert(snag_model_capacity_resolve(&cache, &config, &config.providers[0],
               "org/model", "openai", &capacity,
               error, sizeof(error)) == 0);
    assert(capacity.observed_tokens_per_million_bytes == 300000u);
    assert(snag_model_cache_record(&store, &cache, &config.providers[0],
               "openai", "org/model", SNAG_COUNT_UNKNOWN,
               2000u, 700u, 0u, error, sizeof(error)) == 0);
    assert(snag_model_capacity_resolve(&cache, &config, &config.providers[0],
               "org/model", "openai", &capacity,
               error, sizeof(error)) == 0);
    assert(capacity.observed_tokens_per_million_bytes == 350000u);
    assert(snag_model_cache_record(&store, &cache, &config.providers[0],
               "openai", "org/model", SNAG_COUNT_UNKNOWN,
               33554432u, 4000000000u, 0u, error, sizeof(error)) == 0);
    assert(snag_model_capacity_resolve(&cache, &config, &config.providers[0],
               "org/model", "openai", &capacity,
               error, sizeof(error)) == 0);
    assert(capacity.observed_tokens_per_million_bytes == 119209290u);
    assert(snag_model_cache_replace(&store, providers, 234567u, &cache,
                                   error, sizeof(error)) == 0);
    assert(snag_model_capacity_resolve(&cache, &config, &config.providers[0],
               "org/model", "openai", &capacity,
               error, sizeof(error)) == 0);
    assert(capacity.count_capability == SNAG_COUNT_UNKNOWN);
    assert(capacity.observed_tokens_per_million_bytes == 119209290u);
    assert(capacity.hard_input_tokens == 700000u);

    assert(snprintf(config.providers[0].base_url,
                    sizeof(config.providers[0].base_url), "%s",
                    "https://changed.example.test/v1") > 0);
    assert(snag_model_capacity_resolve(&cache, &config, &config.providers[0],
               "org/model", "openai", &capacity,
               error, sizeof(error)) == 0);
    assert(capacity.source == SNAG_CAPACITY_STALE_CATALOG);
    assert(!capacity.source_bound);
    assert(!capacity.hard_input_known);
    assert(snag_model_cache_record(&store, &cache, &config.providers[0],
               "openai", "org/model", SNAG_COUNT_UNSUPPORTED,
               0u, 0u, 0u, error, sizeof(error)) == 1);
    assert(json_object_set_new(json_array_get(providers, 0), "base_url",
               json_string("https://changed.example.test/v1")) == 0);
    assert(snag_model_cache_replace(&store, providers, 345678u, &cache,
                                   error, sizeof(error)) == 0);
    assert(snag_model_capacity_resolve(&cache, &config, &config.providers[0],
               "org/model", "openai", &capacity,
               error, sizeof(error)) == 0);
    assert(capacity.count_capability == SNAG_COUNT_UNKNOWN);
    assert(capacity.observed_tokens_per_million_bytes == 0u);
    assert(capacity.hard_input_tokens == 922000u);

    test_local_models(&store, &cache);
    json_decref(providers);
    snag_config_free(&config);
    snag_model_cache_free(&cache);
    snag_store_close(&store);
    puts("test_model_cache: ok");
    return 0;
}

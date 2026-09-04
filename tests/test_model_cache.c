/* SPDX-License-Identifier: GPL-2.0-only */
#include "config.h"
#include "json.h"
#include "model_cache.h"
#include "store.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static json_t *
load_json(const char *text)
{
    char error[128] = {0};
    json_t *value = snj_json_load_strict((const unsigned char *)text,
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
    struct snj_store store;
    struct snj_model_cache cache;
    struct snj_model_capacity capacity;
    struct snj_config config;
    struct stat before;
    struct stat after;
    json_t *providers;
    int fd;
    ssize_t got;

    assert(mkdtemp(temp));
    snj_store_init(&store);
    assert(snj_store_open(&store, temp, error, sizeof(error)) == 0);
    snj_model_cache_init(&cache);
    write_file_at(store.root_fd, "models.json", old_cache);
    assert(snj_model_cache_load(&store, &cache, error, sizeof(error)) < 0);
    assert(strstr(error, "use /model cache while idle") != NULL);
    assert(cache.providers == NULL);

    providers = load_json(providers_text);
    assert(snj_model_cache_replace(&store, providers, 123456u, &cache,
                                   error, sizeof(error)) == 0);
    assert(cache.updated_at_ms == 123456u);
    assert(snj_model_cache_entry_count(&cache) == 5u);
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
    snj_model_cache_free(&cache);
    assert(snj_model_cache_load(&store, &cache, error, sizeof(error)) == 0);

    snj_config_init(&config);
    assert(snprintf(config.providers[0].name,
                    sizeof(config.providers[0].name), "%s", "paid") > 0);
    assert(snprintf(config.providers[0].base_url,
                    sizeof(config.providers[0].base_url), "%s",
                    "https://api.example.test/v1") > 0);
    assert(snj_model_capacity_resolve(&cache, &config, &config.providers[0],
               "org/model", "openai", &capacity,
               error, sizeof(error)) == 0);
    assert(capacity.source == SNJ_CAPACITY_CATALOG);
    assert(capacity.source_bound);
    assert(capacity.context_window_tokens == 1050000u);
    assert(capacity.max_input_tokens == 922000u);
    assert(capacity.max_output_tokens == 128000u);
    assert(capacity.hard_input_known);
    assert(capacity.hard_input_tokens == 922000u);
    assert(!capacity.effective_context_window_known);

    assert(snj_model_capacity_resolve(&cache, &config, &config.providers[0],
               "context-only", "openai", &capacity,
               error, sizeof(error)) == 0);
    assert(capacity.source == SNJ_CAPACITY_CATALOG);
    assert(capacity.context_window_known);
    assert(capacity.effective_context_window_known);
    assert(capacity.effective_context_window_derived);
    assert(capacity.effective_context_window_percent == 90u);
    assert(capacity.hard_input_tokens == 90000u);

    assert(snj_model_capacity_resolve(&cache, &config, &config.providers[0],
               "unknown", "openai", &capacity,
               error, sizeof(error)) == 0);
    assert(capacity.source == SNJ_CAPACITY_UNKNOWN);
    assert(capacity.source_bound);
    assert(!capacity.hard_input_known);

    assert(snprintf(config.providers[1].name,
                    sizeof(config.providers[1].name), "%s", "codex") > 0);
    assert(snprintf(config.providers[1].base_url,
                    sizeof(config.providers[1].base_url), "%s",
                    "https://chat.example.test/backend-api/codex") > 0);
    assert(snj_model_capacity_resolve(&cache, &config, &config.providers[1],
               "codex-context-only", "codex", &capacity,
               error, sizeof(error)) == 0);
    assert(capacity.source == SNJ_CAPACITY_CATALOG);
    assert(capacity.context_window_tokens == 272000u);
    assert(capacity.max_context_window_tokens == 872000u);
    assert(capacity.effective_context_window_known);
    assert(capacity.effective_context_window_derived);
    assert(capacity.effective_context_window_percent == 95u);
    assert(capacity.hard_input_tokens == 258400u);

    config.model_limit_count = 1u;
    assert(snprintf(config.model_limits[0].provider,
                    sizeof(config.model_limits[0].provider), "%s", "paid") > 0);
    assert(snprintf(config.model_limits[0].model,
                    sizeof(config.model_limits[0].model), "%s", "org/model") > 0);
    config.model_limits[0].max_input_known = true;
    config.model_limits[0].max_input_tokens = 900000u;
    assert(snj_model_capacity_resolve(&cache, &config, &config.providers[0],
               "org/model", "openai", &capacity,
               error, sizeof(error)) == 0);
    assert(capacity.source == SNJ_CAPACITY_CONFIG);
    assert(capacity.source_bound);
    assert(!capacity.context_window_known);
    assert(!capacity.max_output_known);
    assert(capacity.hard_input_tokens == 900000u);

    config.model_limits[0].max_input_tokens = 1100000u;
    assert(snj_model_capacity_resolve(&cache, &config, &config.providers[0],
               "org/model", "openai", &capacity,
               error, sizeof(error)) == 0);
    assert(capacity.source == SNJ_CAPACITY_CONFIG);
    assert(!capacity.context_window_known);
    assert(!capacity.max_output_known);
    assert(capacity.hard_input_tokens == 1100000u);

    config.model_limit_count = 0u;
    assert(snj_model_cache_record(&store, &cache, &config.providers[0],
               "openai", "org/model", SNJ_COUNT_SUPPORTED,
               1000u, 250u, 800000u, error, sizeof(error)) == 0);
    assert(fstatat(store.root_fd, "models.json", &before,
                   AT_SYMLINK_NOFOLLOW) == 0);
    assert(snj_model_cache_record(&store, &cache, &config.providers[0],
               "openai", "org/model", SNJ_COUNT_UNKNOWN,
               500u, 200u, 850000u, error, sizeof(error)) == 0);
    assert(fstatat(store.root_fd, "models.json", &after,
                   AT_SYMLINK_NOFOLLOW) == 0);
    assert(before.st_dev == after.st_dev && before.st_ino == after.st_ino);
    assert(snj_model_capacity_resolve(&cache, &config, &config.providers[0],
               "org/model", "openai", &capacity,
               error, sizeof(error)) == 0);
    assert(capacity.count_capability == SNJ_COUNT_SUPPORTED);
    assert(capacity.observed_tokens_per_million_bytes == 250000u);
    assert(capacity.hard_input_tokens == 800000u);
    assert(capacity.source == SNJ_CAPACITY_OBSERVED);
    assert(snj_model_cache_record(&store, &cache, &config.providers[0],
               "openai", "org/model", SNJ_COUNT_UNKNOWN,
               2000u, 600u, 700000u, error, sizeof(error)) == 0);
    assert(snj_model_capacity_resolve(&cache, &config, &config.providers[0],
               "org/model", "openai", &capacity,
               error, sizeof(error)) == 0);
    assert(capacity.observed_tokens_per_million_bytes == 300000u);
    assert(capacity.hard_input_tokens == 700000u);
    assert(snj_model_cache_record(&store, &cache, &config.providers[0],
               "openai", "org/model", SNJ_COUNT_UNKNOWN,
               2000u, 500u, 0u, error, sizeof(error)) == 0);
    assert(snj_model_capacity_resolve(&cache, &config, &config.providers[0],
               "org/model", "openai", &capacity,
               error, sizeof(error)) == 0);
    assert(capacity.observed_tokens_per_million_bytes == 300000u);
    assert(snj_model_cache_record(&store, &cache, &config.providers[0],
               "openai", "org/model", SNJ_COUNT_UNKNOWN,
               2000u, 700u, 0u, error, sizeof(error)) == 0);
    assert(snj_model_capacity_resolve(&cache, &config, &config.providers[0],
               "org/model", "openai", &capacity,
               error, sizeof(error)) == 0);
    assert(capacity.observed_tokens_per_million_bytes == 350000u);
    assert(snj_model_cache_record(&store, &cache, &config.providers[0],
               "openai", "org/model", SNJ_COUNT_UNKNOWN,
               33554432u, 4000000000u, 0u, error, sizeof(error)) == 0);
    assert(snj_model_capacity_resolve(&cache, &config, &config.providers[0],
               "org/model", "openai", &capacity,
               error, sizeof(error)) == 0);
    assert(capacity.observed_tokens_per_million_bytes == 119209290u);
    assert(snj_model_cache_replace(&store, providers, 234567u, &cache,
                                   error, sizeof(error)) == 0);
    assert(snj_model_capacity_resolve(&cache, &config, &config.providers[0],
               "org/model", "openai", &capacity,
               error, sizeof(error)) == 0);
    assert(capacity.count_capability == SNJ_COUNT_UNKNOWN);
    assert(capacity.observed_tokens_per_million_bytes == 119209290u);
    assert(capacity.hard_input_tokens == 700000u);

    assert(snprintf(config.providers[0].base_url,
                    sizeof(config.providers[0].base_url), "%s",
                    "https://changed.example.test/v1") > 0);
    assert(snj_model_capacity_resolve(&cache, &config, &config.providers[0],
               "org/model", "openai", &capacity,
               error, sizeof(error)) == 0);
    assert(capacity.source == SNJ_CAPACITY_STALE_CATALOG);
    assert(!capacity.source_bound);
    assert(!capacity.hard_input_known);
    assert(snj_model_cache_record(&store, &cache, &config.providers[0],
               "openai", "org/model", SNJ_COUNT_UNSUPPORTED,
               0u, 0u, 0u, error, sizeof(error)) == 1);
    assert(json_object_set_new(json_array_get(providers, 0), "base_url",
               json_string("https://changed.example.test/v1")) == 0);
    assert(snj_model_cache_replace(&store, providers, 345678u, &cache,
                                   error, sizeof(error)) == 0);
    assert(snj_model_capacity_resolve(&cache, &config, &config.providers[0],
               "org/model", "openai", &capacity,
               error, sizeof(error)) == 0);
    assert(capacity.count_capability == SNJ_COUNT_UNKNOWN);
    assert(capacity.observed_tokens_per_million_bytes == 0u);
    assert(capacity.hard_input_tokens == 922000u);

    json_decref(providers);
    snj_config_free(&config);
    snj_model_cache_free(&cache);
    snj_store_close(&store);
    puts("test_model_cache: ok");
    return 0;
}

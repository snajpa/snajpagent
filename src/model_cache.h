/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_MODEL_CACHE_H
#define SNAJPAGENT_MODEL_CACHE_H

#include "config.h"
#include "store.h"

#include "snj_jansson.h"
#include <stddef.h>
#include <stdint.h>

struct snj_model_cache {
    json_t *providers;
    uint64_t updated_at_ms;
};

enum snj_capacity_source {
    SNJ_CAPACITY_UNKNOWN,
    SNJ_CAPACITY_CATALOG,
    SNJ_CAPACITY_CONFIG,
    SNJ_CAPACITY_OBSERVED,
    SNJ_CAPACITY_STALE_CATALOG
};

struct snj_model_capacity {
    uint64_t context_window_tokens;
    uint64_t max_context_window_tokens;
    uint64_t input_context_window_tokens;
    uint64_t max_input_tokens;
    uint64_t max_output_tokens;
    uint64_t auto_compact_input_tokens;
    uint64_t hard_input_tokens;
    unsigned int effective_context_window_percent;
    enum snj_capacity_source source;
    bool context_window_known;
    bool max_context_window_known;
    bool input_context_window_known;
    bool max_input_known;
    bool max_output_known;
    bool auto_compact_input_known;
    bool effective_context_window_known;
    bool effective_context_window_derived;
    bool hard_input_known;
    bool source_bound;
    bool cache_source_mismatch;
    bool codex_protocol;
};

void snj_model_cache_init(struct snj_model_cache *cache);
void snj_model_cache_free(struct snj_model_cache *cache);

/* Returns 1 when models.json does not exist, 0 on success, and -1 on error. */
int snj_model_cache_load(struct snj_store *store,
                         struct snj_model_cache *cache,
                         char *error, size_t error_size);
int snj_model_cache_replace(struct snj_store *store,
                            const json_t *providers,
                            uint64_t updated_at_ms,
                            struct snj_model_cache *cache,
                            char *error, size_t error_size);

const json_t *snj_model_cache_find(const struct snj_model_cache *cache,
                                   const char *provider,
                                   const char *model);
const char *snj_model_cache_best_effort(const json_t *model,
                                        const char *fallback);
size_t snj_model_cache_entry_count(const struct snj_model_cache *cache);
int snj_model_cache_entry(const struct snj_model_cache *cache, size_t index,
                          const char *fallback_effort,
                          const char **provider, const char **model,
                          const char **effort);

int snj_model_capacity_resolve(
    const struct snj_model_cache *cache, const struct snj_config *config,
    const struct snj_provider_config *provider, const char *model,
    const char *protocol, struct snj_model_capacity *capacity,
    char *error, size_t error_size);
const char *snj_capacity_source_name(enum snj_capacity_source source);

#endif

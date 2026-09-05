/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_MODEL_CACHE_H
#define SNAJPAGENT_MODEL_CACHE_H

#include "config.h"
#include "store.h"

#include "snag_jansson.h"
#include <stddef.h>
#include <stdint.h>

struct snag_model_cache {
    json_t *providers;
    uint64_t updated_at_ms;
};

enum snag_count_capability {
    SNAG_COUNT_UNKNOWN,
    SNAG_COUNT_SUPPORTED,
    SNAG_COUNT_UNSUPPORTED
};

enum snag_capacity_source {
    SNAG_CAPACITY_UNKNOWN,
    SNAG_CAPACITY_CATALOG,
    SNAG_CAPACITY_CONFIG,
    SNAG_CAPACITY_OBSERVED,
    SNAG_CAPACITY_STALE_CATALOG
};

/* Configured/advertised limits use zero for unknown; positive values are known. */
struct snag_model_capacity {
    uint64_t context_window_tokens;
    uint64_t max_context_window_tokens;
    uint64_t input_context_window_tokens;
    uint64_t max_input_tokens;
    uint64_t max_output_tokens;
    uint64_t auto_compact_input_tokens;
    uint64_t hard_input_tokens;
    uint64_t observed_tokens_per_million_bytes;
    unsigned int effective_context_window_percent;
    enum snag_capacity_source source;
    enum snag_count_capability count_capability;
    bool effective_context_window_derived;
    bool hard_input_known; /* Zero is a known exhausted budget, not absence. */
    bool source_bound;
    bool cache_source_mismatch;
    bool codex_protocol;
};

void snag_model_cache_init(struct snag_model_cache *cache);
void snag_model_cache_free(struct snag_model_cache *cache);

/* Returns 1 when models.json does not exist, 0 on success, and -1 on error. */
int snag_model_cache_load(struct snag_store *store,
                         struct snag_model_cache *cache,
                         char *error, size_t error_size);
int snag_model_cache_replace(struct snag_store *store,
                            const json_t *providers,
                            uint64_t updated_at_ms,
                            struct snag_model_cache *cache,
                            char *error, size_t error_size);
int snag_model_cache_record(struct snag_store *store,
                           struct snag_model_cache *cache,
                           const struct snag_provider_config *provider,
                           const char *protocol, const char *model,
                           enum snag_count_capability capability,
                           uint64_t model_input_bytes, uint64_t input_tokens,
                           uint64_t hard_input_tokens,
                           char *error, size_t error_size);

const json_t *snag_model_cache_find(const struct snag_model_cache *cache,
                                   const char *provider,
                                   const char *model);
const char *snag_model_cache_best_effort(const json_t *model,
                                        const char *fallback);
int snag_model_cache_entry(const struct snag_model_cache *cache, size_t index,
                          const char *fallback_effort,
                          const char **provider, const char **model,
                          const char **effort);

int snag_model_capacity_resolve(
    const struct snag_model_cache *cache, const struct snag_config *config,
    const struct snag_provider_config *provider, const char *model,
    const char *protocol, struct snag_model_capacity *capacity,
    char *error, size_t error_size);
const char *snag_capacity_source_name(enum snag_capacity_source source);
uint64_t snag_model_compact_threshold(
    const struct snag_provider_config *provider,
    const struct snag_model_capacity *capacity);

#endif

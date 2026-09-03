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
/* Returns 1 when no local Codex cache exists, 0 on success, and -1 on error. */
int snj_model_cache_bootstrap_codex(struct snj_store *store,
                                    const struct snj_config *config,
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

#endif

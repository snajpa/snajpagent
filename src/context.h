/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_CONTEXT_H
#define SNAJPAGENT_CONTEXT_H

#include "store.h"
#include "instructions.h"

#include <stddef.h>
#include <stdint.h>

#define SNAG_CONTEXT_MAX_REQUEST (32u * 1024u * 1024u)
#define SNAG_CONTEXT_MAX_COMPACT (12u * 1024u * 1024u)
#define SNAG_CONTEXT_MAX_COMPACT_ITEMS 128u

int snag_context_codex_request(json_t *request);
uint64_t snag_context_input_estimate(uint64_t bytes, uint64_t tokens_per_million_bytes);
/* Bind the ordinary local model once when constructing a provider wire request. */
int snag_context_provider_model(const struct snag_provider_config *provider,
                                const char *model, json_t *request);

struct snag_context_projection {
    json_t *model_input;
    json_t *create_request;
    json_t *count_request;
    size_t model_input_bytes;
    size_t request_input_bytes;
    size_t request_input_count;
    size_t request_controller_count;
    size_t create_request_bytes;
    size_t count_request_bytes;
    uint64_t input_tokens_bound;
    uint64_t irc_seq;
    char model_input_sha256[SNAG_SHA256_HEX_LEN + 1u];
    char request_input_sha256[SNAG_SHA256_HEX_LEN + 1u];
    char request_sha256[SNAG_SHA256_HEX_LEN + 1u];
    char count_request_sha256[SNAG_SHA256_HEX_LEN + 1u];
};

void snag_context_projection_init(struct snag_context_projection *projection);
void snag_context_projection_free(struct snag_context_projection *projection);
int snag_context_usage_anchor_bound(
                      const struct snag_session *session,
                      const char *provider, const char *model,
                      const char *effort, const char *provider_source_sha256,
                      const struct snag_context_projection *projection,
                      uint64_t *input_tokens_bound);
int snag_context_build(struct snag_session *session, const char *model,
                      const char *effort, unsigned int cycle,
                      const json_t *steering,
                      uint64_t max_output_tokens,
                      bool max_output_known,
                      const struct snag_config *config,
                      const struct snag_instruction_set *instructions,
                      struct snag_context_projection *projection,
                      char *error, size_t error_size);
int snag_context_compact_request_build(struct snag_session *session,
                                      const char *model, const char *effort,
                                      bool active_prefix,
                                      uint64_t source_budget,
                                      bool allow_oversized_first,
                                      json_t **request,
                                      json_t **count_request,
                                      char source_hash[SNAG_SHA256_HEX_LEN + 1u],
                                      size_t *source_bytes,
                                      char request_hash[SNAG_SHA256_HEX_LEN + 1u],
                                      size_t *request_bytes,
                                      uint64_t *source_seq,
                                      char *error, size_t error_size);
int snag_context_compact_output_count_request_build(const json_t *output,
                                      const char *model,
                                      json_t **count_request,
                                      char request_hash[SNAG_SHA256_HEX_LEN + 1u],
                                      size_t *request_bytes,
                                      char *error, size_t error_size);
int snag_context_compact_output_valid(const json_t *output,
                                     char output_hash[SNAG_SHA256_HEX_LEN + 1u],
                                     size_t *output_bytes,
                                     char *error, size_t error_size);

#endif

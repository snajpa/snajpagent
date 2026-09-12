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
struct snag_credential;
int snag_context_continuation_scope(const struct snag_provider_config *provider, const char *model,
                                   const struct snag_credential *credential,
                                   char digest[SNAG_SHA256_HEX_LEN + 1u]);
/* Bind the ordinary local model once when constructing a provider wire request. */
int snag_context_provider_model(const struct snag_provider_config *provider,
                                const char *model, json_t *request);

struct snag_context_projection {
    struct snag_json_document model_input, create_request, count_request;
    char continuation_scope[SNAG_SHA256_HEX_LEN + 1u];
    size_t request_input_bytes;
    size_t request_input_count;
    size_t request_controller_count;
    uint64_t input_tokens_bound;
    uint64_t irc_seq;
    char request_input_sha256[SNAG_SHA256_HEX_LEN + 1u];
    uint64_t source_seq; /* Selected complete group for compaction. */
};

void snag_context_projection_free(struct snag_context_projection *projection);
int snag_context_build(struct snag_session *session, const char *model,
                      const char *effort, unsigned int cycle, const json_t *steering,
                      uint64_t max_output_tokens, bool max_output_known,
                      const struct snag_config *config, const char *continuation_scope,
                      const struct snag_instruction_set *instructions, const char *operator_visibility,
                      struct snag_context_projection *projection, char *error, size_t error_size);
int snag_context_compact_request_build(struct snag_session *session, const char *model, const char *effort,
                                      bool active_prefix, uint64_t source_budget,
                                      bool allow_oversized_first, const char *continuation_scope,
                                      struct snag_context_projection *projection,
                                      char *error, size_t error_size);
int snag_context_compact_output_count_request_build(const json_t *output, const char *model,
                                      struct snag_json_document *count_request,
                                      char *error, size_t error_size);
int snag_context_compact_output_valid(const json_t *output, char output_hash[SNAG_SHA256_HEX_LEN + 1u],
                                     size_t *output_bytes, char *error, size_t error_size);
/* Consumes a compact result and retains its validated canonical measurement. */
int snag_context_compact_output_set(struct snag_json_document *document, json_t *value,
                                    char *error, size_t error_size);

#endif

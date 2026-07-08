/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_CONTEXT_H
#define SNAJPAGENT_CONTEXT_H

#include "store.h"
#include "instructions.h"

#include <stddef.h>
#include <stdint.h>

#define SNJ_CONTEXT_MAX_REQUEST (32u * 1024u * 1024u)
#define SNJ_CONTEXT_MAX_COMPACT (12u * 1024u * 1024u)
#define SNJ_CONTEXT_MAX_COMPACT_ITEMS 128u

struct snj_context_projection {
    json_t *model_input;
    json_t *create_request;
    json_t *count_request;
    size_t model_input_bytes;
    size_t create_request_bytes;
    size_t count_request_bytes;
    uint64_t input_tokens_bound;
    char model_input_sha256[SNJ_SHA256_HEX_LEN + 1u];
    char request_sha256[SNJ_SHA256_HEX_LEN + 1u];
    char count_request_sha256[SNJ_SHA256_HEX_LEN + 1u];
};

void snj_context_projection_init(struct snj_context_projection *projection);
void snj_context_projection_free(struct snj_context_projection *projection);
int snj_context_build(struct snj_session *session, const char *model,
                      const char *effort, unsigned int cycle,
                      const json_t *steering,
                      const struct snj_instruction_set *instructions,
                      struct snj_context_projection *projection,
                      char *error, size_t error_size);
int snj_context_compact_request_build(struct snj_session *session,
                                      const char *model, const char *effort,
                                      json_t **request,
                                      json_t **count_request,
                                      char source_hash[SNJ_SHA256_HEX_LEN + 1u],
                                      size_t *source_bytes,
                                      char request_hash[SNJ_SHA256_HEX_LEN + 1u],
                                      size_t *request_bytes,
                                      uint64_t *source_seq,
                                      char *error, size_t error_size);
int snj_context_compact_active_prefix_request_build(struct snj_session *session,
                                      const char *model, const char *effort,
                                      json_t **request,
                                      json_t **count_request,
                                      char source_hash[SNJ_SHA256_HEX_LEN + 1u],
                                      size_t *source_bytes,
                                      char request_hash[SNJ_SHA256_HEX_LEN + 1u],
                                      size_t *request_bytes,
                                      uint64_t *source_seq,
                                      char *error, size_t error_size);
int snj_context_compact_output_count_request_build(const json_t *output,
                                      const char *model,
                                      json_t **count_request,
                                      char request_hash[SNJ_SHA256_HEX_LEN + 1u],
                                      size_t *request_bytes,
                                      char *error, size_t error_size);
int snj_context_compact_output_valid(const json_t *output,
                                     char output_hash[SNJ_SHA256_HEX_LEN + 1u],
                                     size_t *output_bytes,
                                     char *error, size_t error_size);

#endif

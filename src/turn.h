/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_TURN_H
#define SNAJPAGENT_TURN_H

#include "json.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SNJ_MAX_RESPONSE_ITEMS 96u
#define SNJ_MAX_CALLS_PER_RESPONSE 32u
#define SNJ_MAX_RESPONSE_CYCLES 64u
#define SNJ_MAX_TOOL_INVOCATIONS 128u
#define SNJ_MAX_PUBLIC_ITEM (2u * 1024u * 1024u)
#define SNJ_MAX_PROVIDER_ID 512u
#define SNJ_MAX_TOOL_ARGUMENTS (2u * 1024u * 1024u)
#define SNJ_MAX_RESPONSE_GRAPH (8u * 1024u * 1024u)

/* These are semantic response items, not a provider plug-in interface. */
enum snj_item_kind {
    SNJ_ITEM_ASSISTANT,
    SNJ_ITEM_REFUSAL,
    SNJ_ITEM_REASONING_SUMMARY,
    SNJ_ITEM_TOOL_CALL,
    SNJ_ITEM_OPAQUE
};

enum snj_item_phase {
    SNJ_PHASE_NONE,
    SNJ_PHASE_COMMENTARY,
    SNJ_PHASE_FINAL_ANSWER,
    SNJ_PHASE_SUMMARY
};

struct snj_response_item {
    enum snj_item_kind kind;
    enum snj_item_phase phase;
    char local_item_id[SNJ_ID_HEX_LEN + 1u];
    char call_id[SNJ_ID_HEX_LEN + 1u];
    char *provider_item_id;
    char *provider_call_id;
    char *text;
    char *name;
    json_t *arguments;
    char *provider_type;
    json_t *payload;
};


struct snj_response_usage {
    uint64_t input_tokens;
    uint64_t output_tokens;
    uint64_t reasoning_tokens;
    uint64_t total_tokens;
    bool input_known;
    bool output_known;
    bool reasoning_known;
    bool total_known;
};

struct snj_response_graph {
    char *provider_response_id;
    struct snj_response_item *items;
    size_t count;
    size_t cap;
    size_t encoded_bytes;
    struct snj_response_usage usage;
};

enum snj_graph_outcome {
    SNJ_GRAPH_CALLS,
    SNJ_GRAPH_FINAL,
    SNJ_GRAPH_REFUSAL,
    SNJ_GRAPH_NONPRODUCTIVE,
    SNJ_GRAPH_CONFLICT
};

struct snj_graph_decision {
    enum snj_graph_outcome outcome;
    size_t final_index;
    size_t call_count;
    const char *message;
};

void snj_response_graph_init(struct snj_response_graph *graph);
void snj_response_graph_free(struct snj_response_graph *graph);
int snj_response_graph_set_provider_id(struct snj_response_graph *graph,
                                       const char *provider_response_id);
int snj_response_graph_add_public(struct snj_response_graph *graph,
                                  enum snj_item_kind kind,
                                  enum snj_item_phase phase,
                                  const char *provider_item_id,
                                  const char *text);
int snj_response_graph_add_call(struct snj_response_graph *graph,
                                const char *provider_item_id,
                                const char *provider_call_id,
                                const char *name, json_t *arguments);
int snj_response_graph_add_opaque(struct snj_response_graph *graph,
                                  const char *provider_item_id,
                                  const char *provider_type, json_t *payload);
int snj_response_graph_classify(const struct snj_response_graph *graph,
                                struct snj_graph_decision *decision,
                                char *error, size_t error_size);
json_t *snj_response_graph_json(const struct snj_response_graph *graph);
int snj_response_usage_valid(const struct snj_response_usage *usage);
json_t *snj_response_usage_json(const struct snj_response_usage *usage);
int snj_response_usage_from_json(const json_t *value,
                                 struct snj_response_usage *usage);
int snj_response_graph_from_json(struct snj_response_graph *graph,
                                 const json_t *items,
                                 char *error, size_t error_size);
int snj_partial_public_validate(const json_t *items,
                                char *error, size_t error_size);
int snj_tool_action_digest(const struct snj_response_item *call,
                           const char *resolved_workdir,
                           char out[SNJ_SHA256_HEX_LEN + 1u]);

const char *snj_item_kind_name(enum snj_item_kind kind);
const char *snj_item_phase_name(enum snj_item_phase phase);

json_t *snj_tool_result_not_run(const char *reason);
json_t *snj_tool_result_terminal(bool succeeded, const char *model_text);
json_t *snj_tool_result_denied(void);
json_t *snj_tool_result_outcome_unknown(const char *reason);
int snj_tool_result_valid(const json_t *result);

#endif

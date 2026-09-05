/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_TURN_H
#define SNAJPAGENT_TURN_H

#include "json.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SNAG_MAX_RESPONSE_ITEMS 96u
/* Parse only local prompt syntax; never apply this to model or IRC messages. */
const char *snag_prompt_parse(const char *text, bool *read_only);
bool snag_read_only_tool(const char *name);
#define SNAG_MAX_CALLS_PER_RESPONSE 32u
#define SNAG_MAX_PROCESSES 32u

/* Durable identity/output counters plus engine-owned observations, never PIDs. */
struct snag_process_state {
    char handle[SNAG_ID_HEX_LEN + 1u];
    char command[257];
    char workdir[257];
    uint64_t output_bytes[2];
    uint64_t collected_bytes[2];
    uint64_t input_accepted, input_written, input_pending;
    bool ready, draining;
    /* Live journal scan anchor; not part of provider-visible process facts. */
    uint64_t log_offset, log_seq;
    char log_hash[SNAG_SHA256_HEX_LEN + 1u];
};
#define SNAG_MAX_PUBLIC_ITEM (2u * 1024u * 1024u)
#define SNAG_MAX_PROVIDER_ID 512u
#define SNAG_MAX_TOOL_ARGUMENTS (2u * 1024u * 1024u)
#define SNAG_MAX_RESPONSE_GRAPH (8u * 1024u * 1024u)
#define SNAG_EMPTY_OUTPUT_CORRECTION \
    "You tried to send an empty assistant message. Send nonempty text or take another action."
#define SNAG_OVERSIZED_OUTPUT_CORRECTION \
    "You tried to send an oversized assistant message. Send a shorter message or take another action."

enum snag_output_correction {
    SNAG_OUTPUT_CORRECTION_NONE,
    SNAG_OUTPUT_CORRECTION_EMPTY,
    SNAG_OUTPUT_CORRECTION_OVERSIZED
};

/* These are semantic response items, not a provider plug-in interface. */
enum snag_item_kind {
    SNAG_ITEM_ASSISTANT,
    SNAG_ITEM_REFUSAL,
    SNAG_ITEM_REASONING_SUMMARY,
    SNAG_ITEM_TOOL_CALL,
    SNAG_ITEM_OPAQUE
};

enum snag_item_phase {
    SNAG_PHASE_NONE,
    SNAG_PHASE_COMMENTARY,
    SNAG_PHASE_FINAL_ANSWER,
    SNAG_PHASE_SUMMARY
};

struct snag_response_item {
    enum snag_item_kind kind;
    enum snag_item_phase phase;
    char local_item_id[SNAG_ID_HEX_LEN + 1u];
    char call_id[SNAG_ID_HEX_LEN + 1u];
    char *provider_item_id;
    char *provider_call_id;
    char *text;
    char *name;
    json_t *arguments;
    char *provider_type;
    json_t *payload;
};

struct snag_response_usage {
    uint64_t input_tokens;
    uint64_t output_tokens;
    uint64_t reasoning_tokens;
    uint64_t total_tokens;
    bool input_known;
    bool output_known;
    bool reasoning_known;
    bool total_known;
};

struct snag_response_graph {
    char *provider_response_id;
    struct snag_response_item *items;
    size_t count;
    size_t cap;
    size_t encoded_bytes;
    struct snag_response_usage usage;
};

enum snag_graph_outcome {
    SNAG_GRAPH_CALLS,
    SNAG_GRAPH_FINAL,
    SNAG_GRAPH_REFUSAL,
    SNAG_GRAPH_NONPRODUCTIVE,
    SNAG_GRAPH_CONFLICT
};

struct snag_graph_decision {
    enum snag_graph_outcome outcome;
    size_t final_index;
    size_t call_count;
    const char *message;
};

void snag_response_graph_init(struct snag_response_graph *graph);
void snag_response_graph_free(struct snag_response_graph *graph);
int snag_response_graph_set_provider_id(struct snag_response_graph *graph,
                                       const char *provider_response_id);
int snag_response_graph_add_public(struct snag_response_graph *graph,
                                  enum snag_item_kind kind,
                                  enum snag_item_phase phase,
                                  const char *provider_item_id,
                                  const char *text);
int snag_response_graph_add_call(struct snag_response_graph *graph,
                                const char *provider_item_id,
                                const char *provider_call_id,
                                const char *name, json_t *arguments);
int snag_response_graph_add_opaque(struct snag_response_graph *graph,
                                  const char *provider_item_id,
                                  const char *provider_type, json_t *payload);
int snag_response_graph_classify(const struct snag_response_graph *graph,
                                struct snag_graph_decision *decision,
                                char *error, size_t error_size);
json_t *snag_response_graph_json(const struct snag_response_graph *graph);
int snag_response_usage_valid(const struct snag_response_usage *usage);
json_t *snag_response_usage_json(const struct snag_response_usage *usage);
int snag_response_usage_from_json(const json_t *value,
                                 struct snag_response_usage *usage);
int snag_response_graph_from_json(struct snag_response_graph *graph,
                                 const json_t *items,
                                 char *error, size_t error_size);
int snag_partial_public_validate(const json_t *items,
                                char *error, size_t error_size);
int snag_tool_action_digest(const struct snag_response_item *call,
                           const char *resolved_workdir,
                           char out[SNAG_SHA256_HEX_LEN + 1u]);

const char *snag_item_kind_name(enum snag_item_kind kind);
const char *snag_item_phase_name(enum snag_item_phase phase);

json_t *snag_tool_result(const char *status, const char *reason,
                        const char *model_text, int exit_code,
                        uint64_t duration_ms);
json_t *snag_tool_result_not_run(const char *reason);
json_t *snag_tool_result_terminal(bool succeeded, const char *model_text);
json_t *snag_tool_result_outcome_unknown(const char *reason);
int snag_tool_result_valid(const json_t *result);

#endif

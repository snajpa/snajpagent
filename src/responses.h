/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_RESPONSES_H
#define SNAJPAGENT_RESPONSES_H

#include "sse.h"
#include "turn.h"

#include <stdbool.h>
#include <stddef.h>

#define SNAG_MAX_RESPONSE_PARTS 96u
struct snag_provider_failure {
    char code[64];
    char type[64];
    char message[256];
    /* Positive provider facts; zero means absent, never a measured zero. */
    uint64_t context_limit_tokens;
    uint64_t requested_input_tokens;
    enum snag_output_correction output_correction;
};

typedef int (*snag_responses_emit_fn)(void *opaque, size_t output_index,
                                     enum snag_item_kind kind,
                                     enum snag_item_phase phase,
                                     const char *provider_item_id,
                                     const char *text, size_t len);

enum snag_wire_item_kind {
    SNAG_WIRE_ITEM_NONE,
    SNAG_WIRE_ITEM_MESSAGE,
    SNAG_WIRE_ITEM_FUNCTION_CALL,
    SNAG_WIRE_ITEM_INERT
};

enum snag_wire_part_kind {
    SNAG_WIRE_PART_NONE,
    SNAG_WIRE_PART_TEXT,
    SNAG_WIRE_PART_REFUSAL,
    SNAG_WIRE_PART_INERT
};

struct snag_wire_part {
    enum snag_wire_part_kind kind;
    struct snag_buf text;
    bool present;
    bool value_seen;
    bool complete;
};

struct snag_wire_item {
    enum snag_wire_item_kind kind;
    char *id;
    char *phase;
    bool phase_present;
    char *name;
    char *call_id;
    struct snag_buf arguments;
    struct snag_wire_part *parts;
    size_t part_count;
    size_t part_cap;
    bool present;
    bool arguments_seen;
    bool arguments_complete;
    bool complete;
};

struct snag_responses_stream {
    struct snag_wire_item items[SNAG_MAX_RESPONSE_ITEMS];
    size_t item_count;
    size_t part_count;
    size_t aggregate_bytes;
    char *response_id;
    struct snag_response_usage usage;
    snag_responses_emit_fn emit;
    void *opaque;
    bool created;
    bool terminal;
    bool failed;
    bool retry_unsafe;
    enum snag_output_correction output_correction;
    struct snag_provider_failure provider_failure;
    char error[256];
};

void snag_responses_stream_init(struct snag_responses_stream *stream,
                               snag_responses_emit_fn emit, void *opaque);
void snag_responses_stream_free(struct snag_responses_stream *stream);
int snag_responses_sse_record(void *opaque,
                             const struct snag_sse_record *record);
int snag_responses_stream_finish(struct snag_responses_stream *stream,
                                struct snag_response_graph *graph,
                                char *error, size_t error_size);
const char *snag_responses_stream_error(const struct snag_responses_stream *stream);
bool snag_provider_failure_is_capacity(
    const struct snag_provider_failure *failure);
int snag_provider_failure_from_json(const json_t *root,
                                   struct snag_provider_failure *failure);

#endif

/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_RESPONSES_H
#define SNAJPAGENT_RESPONSES_H

#include "sse.h"
#include "turn.h"

#include <stdbool.h>
#include <stddef.h>

#define SNJ_MAX_RESPONSE_PARTS 96u

typedef int (*snj_responses_emit_fn)(void *opaque, size_t output_index,
                                     enum snj_item_kind kind,
                                     enum snj_item_phase phase,
                                     const char *provider_item_id,
                                     const char *text, size_t len);

enum snj_wire_item_kind {
    SNJ_WIRE_ITEM_NONE,
    SNJ_WIRE_ITEM_MESSAGE,
    SNJ_WIRE_ITEM_FUNCTION_CALL,
    SNJ_WIRE_ITEM_REASONING,
    SNJ_WIRE_ITEM_WEB_SEARCH
};

enum snj_wire_part_kind {
    SNJ_WIRE_PART_NONE,
    SNJ_WIRE_PART_TEXT,
    SNJ_WIRE_PART_REFUSAL
};

struct snj_wire_part {
    enum snj_wire_part_kind kind;
    struct snj_buf text;
    bool present;
    bool value_seen;
    bool complete;
};

struct snj_wire_item {
    enum snj_wire_item_kind kind;
    char *id;
    char *phase;
    bool phase_present;
    char *name;
    char *call_id;
    struct snj_buf arguments;
    struct snj_wire_part *parts;
    size_t part_count;
    size_t part_cap;
    bool present;
    bool arguments_seen;
    bool arguments_complete;
    bool complete;
};

struct snj_responses_stream {
    struct snj_wire_item items[SNJ_MAX_RESPONSE_ITEMS];
    size_t item_count;
    size_t part_count;
    size_t aggregate_bytes;
    char *response_id;
    struct snj_response_usage usage;
    snj_responses_emit_fn emit;
    void *opaque;
    bool created;
    bool terminal;
    bool failed;
    char error[256];
};

void snj_responses_stream_init(struct snj_responses_stream *stream,
                               snj_responses_emit_fn emit, void *opaque);
void snj_responses_stream_free(struct snj_responses_stream *stream);
int snj_responses_sse_record(void *opaque,
                             const struct snj_sse_record *record);
int snj_responses_stream_finish(struct snj_responses_stream *stream,
                                struct snj_response_graph *graph,
                                char *error, size_t error_size);
const char *snj_responses_stream_error(const struct snj_responses_stream *stream);

#endif

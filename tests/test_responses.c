/* SPDX-License-Identifier: GPL-2.0-only */
#include "responses.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct parsed_stream {
    struct snag_response_graph graph;
    char error[256];
    struct snag_buf text;
    size_t calls;
    size_t last_index;
    enum snag_item_kind last_kind;
    enum snag_item_phase last_phase;
    char last_provider_id[64];
};

static struct parsed_stream
parsed_new(size_t max)
{
    struct parsed_stream parsed = {0};

    parsed.graph = (struct snag_response_graph){0};
    snag_buf_init(&parsed.text, max);
    return parsed;
}

static void
parsed_free(struct parsed_stream *parsed)
{
    snag_buf_free(&parsed->text);
    snag_response_graph_free(&parsed->graph);
}

static int
capture_emit(void *opaque, size_t output_index, enum snag_item_kind kind,
             enum snag_item_phase phase, const char *provider_item_id,
             const char *text, size_t len)
{
    struct parsed_stream *emitted = opaque;

    ++emitted->calls;
    emitted->last_index = output_index;
    emitted->last_kind = kind;
    emitted->last_phase = phase;
    if (provider_item_id)
        (void)snprintf(emitted->last_provider_id,
                       sizeof(emitted->last_provider_id), "%s",
                       provider_item_id);
    return snag_buf_append(&emitted->text, text, len);
}

static int
parse_stream(const char *wire, size_t chunk, struct parsed_stream *emitted)
{
    struct snag_responses_stream responses;
    struct snag_sse_parser sse;
    size_t len = strlen(wire);
    char *error = emitted->error;
    size_t error_size = sizeof(emitted->error);
    int rc = 0;

    snag_responses_stream_init(&responses, capture_emit, emitted);
    snag_sse_init(&sse, snag_responses_sse_record, &responses);
    for (size_t offset = 0; offset < len && rc == 0;) {
        size_t take = chunk && chunk < len - offset ? chunk : len - offset;
        rc = snag_sse_feed(&sse, wire + offset, take, error, error_size);
        offset += take;
    }
    if (rc == 0)
        rc = snag_sse_finish(&sse, error, error_size);
    if (rc == 0)
        rc = snag_responses_stream_finish(&responses, &emitted->graph,
                                          error, error_size);
    else if (responses.failed)
        (void)snprintf(error, error_size, "%s",
                       snag_responses_stream_error(&responses));
    snag_sse_free(&sse);
    snag_responses_stream_free(&responses);
    return rc;
}

static void
test_deltas_survive_empty_terminal_output(void)
{
    static const char wire[] =
        "event: response.created\n"
        "data: {\"type\":\"response.created\",\"response\":{\"id\":\"resp_ping\",\"status\":\"in_progress\",\"output\":[]}}\n\n"
        "event: response.output_item.added\n"
        "data: {\"type\":\"response.output_item.added\",\"output_index\":0,\"item\":{\"id\":\"msg_ping\",\"type\":\"message\",\"status\":\"in_progress\",\"role\":\"assistant\",\"phase\":\"final_answer\",\"content\":[]}}\n\n"
        "event: response.content_part.added\n"
        "data: {\"type\":\"response.content_part.added\",\"item_id\":\"msg_ping\",\"output_index\":0,\"content_index\":0,\"part\":{\"type\":\"output_text\",\"text\":\"\",\"annotations\":[]}}\n\n"
        "event: response.output_text.delta\n"
        "data: {\"type\":\"response.output_text.delta\",\"item_id\":\"msg_ping\",\"output_index\":0,\"content_index\":0,\"delta\":\"ha\"}\n\n"
        "event: response.output_text.delta\n"
        "data: {\"type\":\"response.output_text.delta\",\"item_id\":\"msg_ping\",\"output_index\":0,\"content_index\":0,\"delta\":\"ha\"}\n\n"
        "event: response.output_text.done\n"
        "data: {\"type\":\"response.output_text.done\",\"item_id\":\"msg_ping\",\"output_index\":0,\"content_index\":0,\"text\":\"haha\"}\n\n"
        "event: response.content_part.done\n"
        "data: {\"type\":\"response.content_part.done\",\"item_id\":\"msg_ping\",\"output_index\":0,\"content_index\":0,\"part\":{\"type\":\"output_text\",\"text\":\"haha\",\"annotations\":[]}}\n\n"
        "event: response.output_item.done\n"
        "data: {\"type\":\"response.output_item.done\",\"output_index\":0,\"item\":{\"id\":\"msg_ping\",\"type\":\"message\",\"status\":\"completed\",\"role\":\"assistant\",\"phase\":\"final_answer\",\"content\":[{\"type\":\"output_text\",\"text\":\"haha\",\"annotations\":[]}]}}\n\n"
        "event: response.completed\n"
        "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_ping\",\"status\":\"completed\",\"usage\":{\"input_tokens\":12,\"output_tokens\":4,\"total_tokens\":16,\"output_tokens_details\":{\"reasoning_tokens\":2}},\"output\":[]}}\n\n"
        "data: [DONE]\n\n";
    struct parsed_stream emitted = parsed_new(SNAG_MAX_PUBLIC_ITEM + 1u);

    assert(parse_stream(wire, 1u, &emitted) == 0);
    assert(emitted.graph.count == 1u);
    assert(strcmp(emitted.graph.provider_response_id, "resp_ping") == 0);
    assert(snag_response_graph_item(&emitted.graph, 0).kind == SNAG_ITEM_ASSISTANT);
    assert(snag_response_graph_item(&emitted.graph, 0).phase == SNAG_PHASE_FINAL_ANSWER);
    assert(strcmp(snag_response_graph_item(&emitted.graph, 0).text, "haha") == 0);
    assert(emitted.calls == 2u);
    assert(emitted.text.len == 4u);
    assert(memcmp(emitted.text.data, "haha", 4u) == 0);
    assert(emitted.graph.usage.input_known && emitted.graph.usage.input_tokens == 12u);
    assert(emitted.graph.usage.output_known && emitted.graph.usage.output_tokens == 4u);
    assert(emitted.graph.usage.reasoning_known && emitted.graph.usage.reasoning_tokens == 2u);
    assert(emitted.graph.usage.total_known && emitted.graph.usage.total_tokens == 16u);
    parsed_free(&emitted);
}

static void
test_terminal_snapshot_can_supply_unseen_items(void)
{
    static const char wire[] =
        "data: {\"type\":\"response.created\",\"response\":{\"id\":\"resp_snapshot\",\"status\":\"in_progress\",\"output\":[]}}\n\n"
        "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_snapshot\",\"status\":\"completed\",\"output\":[{\"id\":\"msg_comment\",\"type\":\"message\",\"status\":\"completed\",\"role\":\"assistant\",\"phase\":\"commentary\",\"content\":[{\"type\":\"output_text\",\"text\":\"Working. \",\"annotations\":[]},{\"type\":\"output_text\",\"text\":\"Done.\",\"annotations\":[]}]},{\"id\":\"msg_final\",\"type\":\"message\",\"status\":\"completed\",\"role\":\"assistant\",\"phase\":\"final_answer\",\"content\":[{\"type\":\"output_text\",\"text\":\"answer\",\"annotations\":[]}]}]}}\n\n";
    struct parsed_stream emitted = parsed_new(1024u);

    if (parse_stream(wire, 17u, &emitted) != 0) {
        fprintf(stderr, "snapshot parse: %s\n", emitted.error);
        assert(0);
    }
    assert(emitted.graph.count == 2u);
    assert(snag_response_graph_item(&emitted.graph, 0).phase == SNAG_PHASE_COMMENTARY);
    assert(strcmp(snag_response_graph_item(&emitted.graph, 0).text, "Working. Done.") == 0);
    assert(snag_response_graph_item(&emitted.graph, 1).phase == SNAG_PHASE_FINAL_ANSWER);
    assert(strcmp(snag_response_graph_item(&emitted.graph, 1).text, "answer") == 0);
    assert(emitted.last_index == 1u);
    assert(emitted.last_phase == SNAG_PHASE_FINAL_ANSWER);
    parsed_free(&emitted);
}

static void
test_failed_snapshot_preserves_only_consistent_text(void)
{
    static const struct {
        const char *id, *status, *text;
        bool eligible;
    } cases[] = {
        {"m", "in_progress", "hello world", true},
        {"m", "completed", "hello world", true},
        {"m", "in_progress", "hello", true},
        {"m", "in_progress", "other", false},
        {"m", "in_progress", "hel", false},
        {"other", "in_progress", "hello world", false}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        struct parsed_stream emitted = parsed_new(1024u);
        struct snag_responses_stream responses;
        struct snag_sse_parser sse;
        struct snag_buf wire = {.max = 8192u};
        char error[256] = {0};
        assert(snag_buf_printf(&wire,
            "data: {\"type\":\"response.created\",\"response\":{\"id\":\"r\",\"status\":\"in_progress\",\"output\":[]}}\n\n"
            "data: {\"type\":\"response.output_item.added\",\"output_index\":0,\"item\":{\"id\":\"m\",\"type\":\"message\",\"status\":\"in_progress\",\"role\":\"assistant\",\"phase\":\"commentary\",\"content\":[]}}\n\n"
            "data: {\"type\":\"response.content_part.added\",\"item_id\":\"m\",\"output_index\":0,\"content_index\":0,\"part\":{\"type\":\"output_text\",\"text\":\"\"}}\n\n"
            "data: {\"type\":\"response.output_text.delta\",\"item_id\":\"m\",\"output_index\":0,\"content_index\":0,\"delta\":\"hello\"}\n\n"
            "data: {\"type\":\"response.failed\",\"response\":{\"error\":{\"code\":\"cyber_policy\",\"message\":\"policy rejected\"},\"output\":[{\"id\":\"%s\",\"type\":\"message\",\"status\":\"%s\",\"role\":\"assistant\",\"phase\":\"commentary\",\"content\":[{\"type\":\"output_text\",\"text\":\"%s\"}]}]}}\n\n",
            cases[i].id, cases[i].status, cases[i].text) == 0);
        snag_responses_stream_init(&responses, capture_emit, &emitted);
        snag_sse_init(&sse, snag_responses_sse_record, &responses);
        assert(snag_sse_feed(&sse, wire.data, wire.len, error, sizeof(error)) < 0);
        assert(responses.failed && !responses.terminal && responses.retry_unsafe);
        assert(snag_provider_failure_is_policy(&responses.provider_failure));
        assert((responses.clarification_skipped[0] == '\0') == cases[i].eligible);
        const char *expected = cases[i].eligible ? cases[i].text : "hello";
        assert(emitted.text.len == strlen(expected));
        assert(!memcmp(emitted.text.data, expected, emitted.text.len));
        snag_sse_free(&sse);
        snag_responses_stream_free(&responses);
        snag_buf_free(&wire);
        parsed_free(&emitted);
    }
}

static void
test_empty_public_items_get_specific_correction(void)
{
    static const char streamed[] =
        "data: {\"type\":\"response.created\",\"response\":{\"id\":\"resp_empty_stream\",\"status\":\"in_progress\",\"output\":[]}}\n\n"
        "data: {\"type\":\"response.output_item.added\",\"output_index\":0,\"item\":{\"id\":\"msg_empty_stream\",\"type\":\"message\",\"status\":\"in_progress\",\"role\":\"assistant\",\"phase\":\"final_answer\",\"content\":[]}}\n\n"
        "data: {\"type\":\"response.content_part.added\",\"item_id\":\"msg_empty_stream\",\"output_index\":0,\"content_index\":0,\"part\":{\"type\":\"output_text\",\"text\":\"\",\"annotations\":[]}}\n\n"
        "data: {\"type\":\"response.output_text.done\",\"item_id\":\"msg_empty_stream\",\"output_index\":0,\"content_index\":0,\"text\":\"\"}\n\n"
        "data: {\"type\":\"response.output_item.done\",\"output_index\":0,\"item\":{\"id\":\"msg_empty_stream\",\"type\":\"message\",\"status\":\"completed\",\"role\":\"assistant\",\"phase\":\"final_answer\",\"content\":[{\"type\":\"output_text\",\"text\":\"\",\"annotations\":[]}]}}\n\n"
        "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_empty_stream\",\"status\":\"completed\",\"output\":[]}}\n\n";
    static const char snapshot[] =
        "data: {\"type\":\"response.created\",\"response\":{\"id\":\"resp_empty_snapshot\",\"status\":\"in_progress\",\"output\":[]}}\n\n"
        "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_empty_snapshot\",\"status\":\"completed\",\"output\":[{\"id\":\"msg_empty_snapshot\",\"type\":\"message\",\"status\":\"completed\",\"role\":\"assistant\",\"phase\":\"final_answer\",\"content\":[{\"type\":\"output_text\",\"text\":\"\",\"annotations\":[]}]}]}}\n\n";
    static const char refusal[] =
        "data: {\"type\":\"response.created\",\"response\":{\"id\":\"resp_empty_refusal\",\"status\":\"in_progress\",\"output\":[]}}\n\n"
        "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_empty_refusal\",\"status\":\"completed\",\"output\":[{\"id\":\"msg_empty_refusal\",\"type\":\"message\",\"status\":\"completed\",\"role\":\"assistant\",\"phase\":\"final_answer\",\"content\":[{\"type\":\"refusal\",\"refusal\":\"\"}]}]}}\n\n";
    static const char no_content[] =
        "data: {\"type\":\"response.created\",\"response\":{\"id\":\"resp_empty_message\",\"status\":\"in_progress\",\"output\":[]}}\n\n"
        "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_empty_message\",\"status\":\"completed\",\"output\":[{\"id\":\"msg_empty\",\"type\":\"message\",\"status\":\"completed\",\"role\":\"assistant\",\"phase\":\"final_answer\",\"content\":[]}]}}\n\n";
    const char *const wires[] = {streamed, snapshot, refusal, no_content};

    for (size_t i = 0; i < sizeof(wires) / sizeof(wires[0]); ++i) {
        struct parsed_stream emitted = parsed_new(1024u);

        assert(parse_stream(wires[i], 1u, &emitted) == 1);
        assert(strcmp(emitted.error, SNAG_EMPTY_OUTPUT_CORRECTION) == 0);
        assert(emitted.graph.count == 0u);
        assert(emitted.graph.provider_response_id == NULL);
        assert(emitted.calls == 0u);
        assert(emitted.text.len == 0u);
        parsed_free(&emitted);
    }
}

static void
test_oversized_public_items_get_specific_correction(void)
{
    static const char *const event_types[] = {
        "response.output_text.delta", "response.refusal.delta"
    };
    static const char *const delta_keys[] = {"delta", "delta"};
    static const char *const part_types[] = {"output_text", "refusal"};
    const size_t delta_len = 700u * 1024u;
    char *delta = malloc(delta_len + 1u);

    assert(delta);
    memset(delta, 'x', delta_len);
    delta[delta_len] = '\0';
    for (size_t kind = 0; kind < 2u; ++kind) {
        struct parsed_stream emitted = parsed_new(SNAG_MAX_PUBLIC_ITEM);

        struct snag_buf wire = {.max = SNAG_MAX_RESPONSE_GRAPH};
        assert(snag_buf_printf(&wire,
            "data: {\"type\":\"response.created\",\"response\":{\"id\":\"resp_large\",\"status\":\"in_progress\",\"output\":[]}}\n\n"
            "data: {\"type\":\"response.output_item.added\",\"output_index\":0,\"item\":{\"id\":\"msg_large\",\"type\":\"message\",\"status\":\"in_progress\",\"role\":\"assistant\",\"phase\":\"final_answer\",\"content\":[]}}\n\n"
            "data: {\"type\":\"response.content_part.added\",\"item_id\":\"msg_large\",\"output_index\":0,\"content_index\":0,\"part\":{\"type\":\"%s\",\"%s\":\"\"}}\n\n",
            part_types[kind],
            kind == 0u ? "text" : "refusal") == 0);
        for (size_t i = 0; i < 3u; ++i)
            assert(snag_buf_printf(&wire,
                "data: {\"type\":\"%s\",\"item_id\":\"msg_large\",\"output_index\":0,\"content_index\":0,\"%s\":\"%s\"}\n\n",
                event_types[kind], delta_keys[kind], delta) == 0);
        assert(snag_buf_terminate(&wire) == 0);
        assert(parse_stream((const char *)wire.data, 8191u, &emitted) < 0);
        assert(strcmp(emitted.error, SNAG_OVERSIZED_OUTPUT_CORRECTION) == 0);
        assert(emitted.graph.count == 0u);
        assert(emitted.calls == 2u);
        assert(emitted.text.len == 2u * delta_len);
        parsed_free(&emitted);
        snag_buf_free(&wire);
    }
    free(delta);
}

static void
test_structured_keepalives_do_not_end_response(void)
{
    static const char wire[] =
        "event: keepalive\n"
        "data: {\"type\":\"keepalive\"}\n\n"
        "event: response.created\n"
        "data: {\"type\":\"response.created\",\"response\":{\"id\":\"resp_keepalive\",\"status\":\"in_progress\",\"output\":[]}}\n\n"
        "event: keepalive\n"
        "data: {\"type\":\"keepalive\",\"time_ms\":123}\n\n"
        "event: response.completed\n"
        "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_keepalive\",\"status\":\"completed\",\"output\":[]}}\n\n";
    struct parsed_stream emitted = parsed_new(1024u);

    assert(parse_stream(wire, 7u, &emitted) == 0);
    assert(strcmp(emitted.graph.provider_response_id, "resp_keepalive") == 0);
    assert(emitted.graph.count == 0u);
    assert(emitted.calls == 0u);
    parsed_free(&emitted);
}

static void
test_public_stream(size_t which)
{
    static const struct {
        const char *name, *wire, *provider_id, *text;
        size_t chunk, index;
        enum snag_item_phase emitted_phase;
    } cases[] = {
        {"unused_response_events_are_ignored",
         "event: response.created\n"
         "data: {\"type\":\"response.created\",\"response\":{\"id\":\"resp_citation\",\"status\":\"in_progress\",\"output\":[]}}\n\n"
         "event: response.output_item.added\n"
         "data: {\"type\":\"response.output_item.added\",\"output_index\":0,\"item\":{\"id\":\"msg_citation\",\"type\":\"message\",\"status\":\"in_progress\",\"role\":\"assistant\",\"phase\":\"final_answer\",\"content\":[]}}\n\n"
         "event: response.content_part.added\n"
         "data: {\"type\":\"response.content_part.added\",\"item_id\":\"msg_citation\",\"output_index\":0,\"content_index\":0,\"part\":{\"type\":\"output_text\",\"text\":\"\",\"annotations\":[]}}\n\n"
         "event: response.output_text.delta\n"
         "data: {\"type\":\"response.output_text.delta\",\"item_id\":\"msg_citation\",\"output_index\":0,\"content_index\":0,\"delta\":\"Source.\"}\n\n"
         "event: response.output_text.annotation.added\n"
         "data: {\"type\":\"response.output_text.annotation.added\",\"item_id\":\"msg_citation\",\"output_index\":0,\"content_index\":0,\"annotation_index\":0,\"annotation\":{\"type\":\"url_citation\",\"start_index\":0,\"end_index\":7,\"title\":\"Example\",\"url\":\"https://example.com/\"},\"sequence_number\":5}\n\n"
         "event: response.in_progress\n"
         "data: {\"type\":\"response.in_progress\",\"payload\":false}\n\n"
         "event: response.image_generation_call.partial_image\n"
         "data: {\"type\":\"response.image_generation_call.partial_image\",\"payload\":null}\n\n"
         "event: response.future.progress\n"
         "data: {\"type\":\"response.future.progress\",\"payload\":{\"anything\":true}}\n\n"
         "event: response.output_text.done\n"
         "data: {\"type\":\"response.output_text.done\",\"item_id\":\"msg_citation\",\"output_index\":0,\"content_index\":0,\"text\":\"Source.\"}\n\n"
         "event: response.content_part.done\n"
         "data: {\"type\":\"response.content_part.done\",\"item_id\":\"msg_citation\",\"output_index\":0,\"content_index\":0,\"part\":{\"type\":\"output_text\",\"text\":\"Source.\",\"annotations\":[{\"type\":\"url_citation\",\"start_index\":0,\"end_index\":7,\"title\":\"Example\",\"url\":\"https://example.com/\"}]}}\n\n"
         "event: response.output_item.done\n"
         "data: {\"type\":\"response.output_item.done\",\"output_index\":0,\"item\":{\"id\":\"msg_citation\",\"type\":\"message\",\"status\":\"completed\",\"role\":\"assistant\",\"phase\":\"final_answer\",\"content\":[{\"type\":\"output_text\",\"text\":\"Source.\",\"annotations\":[{\"type\":\"url_citation\",\"start_index\":0,\"end_index\":7,\"title\":\"Example\",\"url\":\"https://example.com/\"}]}]}}\n\n"
         "event: response.completed\n"
         "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_citation\",\"status\":\"completed\",\"output\":[]}}\n\n",
         "msg_citation", "Source.", 11u, 0u, SNAG_PHASE_FINAL_ANSWER},
        {"terminal_snapshot_ignores_unused_text_metadata",
         "data: {\"type\":\"response.created\",\"response\":{\"id\":\"resp_file\",\"status\":\"in_progress\",\"output\":[]}}\n\n"
         "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_file\",\"status\":\"completed\",\"output\":[{\"id\":\"msg_file\",\"type\":\"message\",\"status\":\"completed\",\"role\":\"assistant\",\"phase\":\"final_answer\",\"content\":[{\"type\":\"output_text\",\"text\":\"See file.\",\"annotations\":{\"unused\":true},\"logprobs\":\"unused\"}]}]}}\n\n",
         "msg_file", "See file.", 13u, 0u, SNAG_PHASE_FINAL_ANSWER},
        {"phase-absent text",
         "event: response.created\n"
         "data: {\"type\":\"response.created\",\"response\":{\"id\":\"resp_pong\",\"status\":\"in_progress\",\"output\":[]}}\n\n"
         "event: response.output_item.added\n"
         "data: {\"type\":\"response.output_item.added\",\"output_index\":0,\"item\":{\"id\":\"msg_pong\",\"type\":\"message\",\"status\":\"in_progress\",\"role\":\"assistant\",\"content\":[]}}\n\n"
         "event: response.content_part.added\n"
         "data: {\"type\":\"response.content_part.added\",\"item_id\":\"msg_pong\",\"output_index\":0,\"content_index\":0,\"part\":{\"type\":\"output_text\",\"text\":\"\",\"annotations\":[]}}\n\n"
         "event: response.output_text.delta\n"
         "data: {\"type\":\"response.output_text.delta\",\"item_id\":\"msg_pong\",\"output_index\":0,\"content_index\":0,\"delta\":\"pong\"}\n\n"
         "event: response.output_text.done\n"
         "data: {\"type\":\"response.output_text.done\",\"item_id\":\"msg_pong\",\"output_index\":0,\"content_index\":0,\"text\":\"pong\"}\n\n"
         "event: response.output_item.done\n"
         "data: {\"type\":\"response.output_item.done\",\"output_index\":0,\"item\":{\"id\":\"msg_pong\",\"type\":\"message\",\"status\":\"completed\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"pong\",\"annotations\":[]}]}}\n\n"
         "event: response.completed\n"
         "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_pong\",\"status\":\"completed\",\"output\":[]}}\n\n",
         "msg_pong", "pong", 9u, 0u, SNAG_PHASE_COMMENTARY},
        {"empty_reasoning_item_is_internal_only",
         "event: response.created\n"
         "data: {\"type\":\"response.created\",\"response\":{\"id\":\"resp_reasoning\",\"status\":\"in_progress\",\"output\":[]}}\n\n"
         "event: response.output_item.added\n"
         "data: {\"type\":\"response.output_item.added\",\"output_index\":0,\"item\":{\"id\":\"rs_1\",\"type\":\"reasoning\",\"content\":[],\"summary\":[]}}\n\n"
         "event: response.output_item.done\n"
         "data: {\"type\":\"response.output_item.done\",\"output_index\":0,\"item\":{\"id\":\"rs_1\",\"type\":\"reasoning\",\"content\":[],\"summary\":[]}}\n\n"
         "event: response.output_item.added\n"
         "data: {\"type\":\"response.output_item.added\",\"output_index\":1,\"item\":{\"id\":\"msg_after_reasoning\",\"type\":\"message\",\"status\":\"in_progress\",\"role\":\"assistant\",\"phase\":\"final_answer\",\"content\":[]}}\n\n"
         "event: response.content_part.added\n"
         "data: {\"type\":\"response.content_part.added\",\"item_id\":\"msg_after_reasoning\",\"output_index\":1,\"content_index\":0,\"part\":{\"type\":\"output_text\",\"text\":\"\",\"annotations\":[]}}\n\n"
         "event: response.output_text.delta\n"
         "data: {\"type\":\"response.output_text.delta\",\"item_id\":\"msg_after_reasoning\",\"output_index\":1,\"content_index\":0,\"delta\":\"ok\"}\n\n"
         "event: response.output_text.done\n"
         "data: {\"type\":\"response.output_text.done\",\"item_id\":\"msg_after_reasoning\",\"output_index\":1,\"content_index\":0,\"text\":\"ok\"}\n\n"
         "event: response.output_item.done\n"
         "data: {\"type\":\"response.output_item.done\",\"output_index\":1,\"item\":{\"id\":\"msg_after_reasoning\",\"type\":\"message\",\"status\":\"completed\",\"role\":\"assistant\",\"phase\":\"final_answer\",\"content\":[{\"type\":\"output_text\",\"text\":\"ok\",\"annotations\":[]}]}}\n\n"
         "event: response.completed\n"
         "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_reasoning\",\"status\":\"completed\",\"output\":[]}}\n\n",
         "msg_after_reasoning", "ok", 19u, 1u, SNAG_PHASE_FINAL_ANSWER},
        {"web_search_item_is_internal_only",
         "event: response.created\n"
         "data: {\"type\":\"response.created\",\"response\":{\"id\":\"resp_web\",\"status\":\"in_progress\",\"output\":[]}}\n\n"
         "event: response.output_item.added\n"
         "data: {\"type\":\"response.output_item.added\",\"output_index\":0,\"item\":{\"id\":\"ws_1\",\"type\":\"web_search_call\",\"status\":\"in_progress\",\"action\":{\"type\":\"search\",\"query\":\"selinux 6.18\"}}}\n\n"
         "event: response.web_search_call.in_progress\n"
         "data: {\"type\":\"response.web_search_call.in_progress\",\"output_index\":0,\"item_id\":\"ws_1\"}\n\n"
         "event: response.web_search_call.searching\n"
         "data: {\"type\":\"response.web_search_call.searching\",\"output_index\":0,\"item_id\":\"ws_1\"}\n\n"
         "event: response.web_search_call.completed\n"
         "data: {\"type\":\"response.web_search_call.completed\",\"output_index\":0,\"item_id\":\"ws_1\"}\n\n"
         "event: response.output_item.done\n"
         "data: {\"type\":\"response.output_item.done\",\"output_index\":0,\"item\":{\"id\":\"ws_1\",\"type\":\"web_search_call\",\"status\":\"completed\",\"action\":{\"type\":\"search\",\"query\":\"selinux 6.18\"}}}\n\n"
         "event: response.output_item.added\n"
         "data: {\"type\":\"response.output_item.added\",\"output_index\":1,\"item\":{\"id\":\"msg_after_web\",\"type\":\"message\",\"status\":\"in_progress\",\"role\":\"assistant\",\"phase\":\"final_answer\",\"content\":[]}}\n\n"
         "event: response.content_part.added\n"
         "data: {\"type\":\"response.content_part.added\",\"item_id\":\"msg_after_web\",\"output_index\":1,\"content_index\":0,\"part\":{\"type\":\"output_text\",\"text\":\"\",\"annotations\":[]}}\n\n"
         "event: response.output_text.delta\n"
         "data: {\"type\":\"response.output_text.delta\",\"item_id\":\"msg_after_web\",\"output_index\":1,\"content_index\":0,\"delta\":\"done\"}\n\n"
         "event: response.output_text.done\n"
         "data: {\"type\":\"response.output_text.done\",\"item_id\":\"msg_after_web\",\"output_index\":1,\"content_index\":0,\"text\":\"done\"}\n\n"
         "event: response.output_item.done\n"
         "data: {\"type\":\"response.output_item.done\",\"output_index\":1,\"item\":{\"id\":\"msg_after_web\",\"type\":\"message\",\"status\":\"completed\",\"role\":\"assistant\",\"phase\":\"final_answer\",\"content\":[{\"type\":\"output_text\",\"text\":\"done\",\"annotations\":[]}]}}\n\n"
         "event: response.completed\n"
         "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_web\",\"status\":\"completed\",\"output\":[]}}\n\n",
         "msg_after_web", "done", 23u, 1u, SNAG_PHASE_FINAL_ANSWER},
        {"future_items_and_content_are_inert",
         "data: {\"type\":\"response.created\",\"response\":{\"id\":\"resp_inert\",\"status\":\"in_progress\",\"output\":[]}}\n\n"
         "data: {\"type\":\"response.output_item.added\",\"output_index\":0,\"item\":{\"type\":\"future_action\",\"name\":\"exec_command\",\"arguments\":\"{\\\"command\\\":\\\"false\\\"}\"}}\n\n"
         "data: {\"type\":\"response.output_item.done\",\"output_index\":0,\"item\":{\"type\":\"future_result\",\"output\":\"unused\"}}\n\n"
         "data: {\"type\":\"response.output_item.added\",\"output_index\":1,\"item\":{\"id\":\"msg_inert\",\"type\":\"message\",\"status\":\"in_progress\",\"role\":\"assistant\",\"phase\":\"final_answer\",\"content\":[]}}\n\n"
         "data: {\"type\":\"response.content_part.added\",\"item_id\":\"msg_inert\",\"output_index\":1,\"content_index\":0,\"part\":{\"type\":\"future_content\",\"text\":\"hidden\"}}\n\n"
         "data: {\"type\":\"response.content_part.done\",\"item_id\":\"msg_inert\",\"output_index\":1,\"content_index\":0,\"part\":{\"type\":\"future_content_result\"}}\n\n"
         "data: {\"type\":\"response.content_part.added\",\"item_id\":\"msg_inert\",\"output_index\":1,\"content_index\":1,\"part\":{\"type\":\"output_text\",\"text\":\"\"}}\n\n"
         "data: {\"type\":\"response.output_text.delta\",\"item_id\":\"msg_inert\",\"output_index\":1,\"content_index\":1,\"delta\":\"visible\"}\n\n"
         "data: {\"type\":\"response.output_text.done\",\"item_id\":\"msg_inert\",\"output_index\":1,\"content_index\":1,\"text\":\"visible\"}\n\n"
         "data: {\"type\":\"response.output_item.done\",\"output_index\":1,\"item\":{\"id\":\"msg_inert\",\"type\":\"message\",\"status\":\"completed\",\"role\":\"assistant\",\"phase\":\"final_answer\",\"content\":[{\"type\":\"future_content_result\"},{\"type\":\"output_text\",\"text\":\"visible\"}]}}\n\n"
         "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_inert\",\"status\":\"completed\",\"output\":[]}}\n\n",
         "msg_inert", "visible", 17u, 1u, SNAG_PHASE_FINAL_ANSWER},
    };
    struct parsed_stream emitted = parsed_new(1024u);

    assert(which < sizeof(cases) / sizeof(cases[0]));
    if (parse_stream(cases[which].wire, cases[which].chunk, &emitted) != 0) {
        fprintf(stderr, "%s parse: %s\n", cases[which].name, emitted.error);
        assert(0);
    }
    assert(emitted.graph.count == 1u);
    struct snag_response_item item = snag_response_graph_item(&emitted.graph, 0u);
    assert(item.kind == SNAG_ITEM_ASSISTANT && item.phase == SNAG_PHASE_FINAL_ANSWER);
    assert(strcmp(item.provider_item_id, cases[which].provider_id) == 0);
    assert(strcmp(item.text, cases[which].text) == 0);
    assert(emitted.calls == 1u && emitted.last_index == cases[which].index);
    assert(emitted.last_phase == cases[which].emitted_phase);
    assert(strcmp(emitted.last_provider_id, cases[which].provider_id) == 0);
    assert(emitted.text.len == strlen(cases[which].text));
    assert(memcmp(emitted.text.data, cases[which].text, emitted.text.len) == 0);
    parsed_free(&emitted);
}

static void
test_unused_annotation_shapes_are_ignored(void)
{
    static const char prefix[] =
        "data: {\"type\":\"response.created\",\"response\":{\"id\":\"r\",\"status\":\"in_progress\",\"output\":[]}}\n\n"
        "data: {\"type\":\"response.output_item.added\",\"output_index\":0,\"item\":{\"id\":\"m\",\"type\":\"message\",\"status\":\"in_progress\",\"role\":\"assistant\",\"phase\":\"final_answer\",\"content\":[]}}\n\n"
        "data: {\"type\":\"response.content_part.added\",\"item_id\":\"m\",\"output_index\":0,\"content_index\":0,\"part\":{\"type\":\"output_text\",\"text\":\"\",\"annotations\":[]}}\n\n";
    static const char *const bad_suffixes[] = {
        "data: {\"type\":\"response.output_text.annotation.added\",\"item_id\":\"m\",\"output_index\":0,\"content_index\":0,\"annotation_index\":1,\"annotation\":{\"type\":\"file_path\",\"file_id\":\"file-a\",\"index\":0}}\n\n",
        "data: {\"type\":\"response.output_text.annotation.added\",\"item_id\":\"m\",\"output_index\":0,\"content_index\":0,\"annotation_index\":0}\n\n",
        "data: {\"type\":\"response.output_text.annotation.added\",\"item_id\":\"m\",\"output_index\":0,\"content_index\":0,\"annotation_index\":0,\"annotation\":{\"type\":\"url_citation\",\"start_index\":0,\"end_index\":1,\"url\":\"https://example.com/\"}}\n\n",
        "data: {\"type\":\"response.output_text.annotation.added\",\"item_id\":\"m\",\"output_index\":0,\"content_index\":0,\"annotation_index\":0,\"annotation\":{\"type\":\"future_citation\"}}\n\n"
    };
    static const char finish[] =
        "data: {\"type\":\"response.output_text.delta\",\"item_id\":\"m\",\"output_index\":0,\"content_index\":0,\"delta\":\"x\"}\n\n"
        "data: {\"type\":\"response.output_text.done\",\"item_id\":\"m\",\"output_index\":0,\"content_index\":0,\"text\":\"x\"}\n\n"
        "data: {\"type\":\"response.output_item.done\",\"output_index\":0,\"item\":{\"id\":\"m\",\"type\":\"message\",\"status\":\"completed\",\"role\":\"assistant\",\"phase\":\"final_answer\",\"content\":[{\"type\":\"output_text\",\"text\":\"x\"}]}}\n\n"
        "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"r\",\"status\":\"completed\",\"output\":[]}}\n\n";

    for (size_t i = 0; i < sizeof(bad_suffixes) / sizeof(bad_suffixes[0]); ++i) {
        struct parsed_stream emitted = parsed_new(1024u);

        struct snag_buf wire = {.max = 8192u};
        assert(snag_buf_append(&wire, prefix, strlen(prefix)) == 0);
        assert(snag_buf_append(&wire, bad_suffixes[i],
                              strlen(bad_suffixes[i])) == 0);
        assert(snag_buf_append(&wire, finish, strlen(finish)) == 0);
        assert(snag_buf_terminate(&wire) == 0);
        assert(parse_stream((char *)wire.data, 7u, &emitted) == 0);
        assert(emitted.graph.count == 1u);
        assert(strcmp(snag_response_graph_item(&emitted.graph, 0).text, "x") == 0);
        parsed_free(&emitted);
        snag_buf_free(&wire);
    }
}

static void
test_phase_absent_text_before_tool_stays_commentary(void)
{
    static const char wire[] =
        "event: response.created\ndata: {\"type\":\"response.created\",\"response\":{\"id\":\"resp_call_text\",\"status\":\"in_progress\",\"output\":[]}}\n\n"
        "event: response.output_item.added\ndata: {\"type\":\"response.output_item.added\",\"output_index\":0,\"item\":{\"id\":\"msg_note\",\"type\":\"message\",\"status\":\"in_progress\",\"role\":\"assistant\",\"content\":[]}}\n\n"
        "event: response.content_part.added\ndata: {\"type\":\"response.content_part.added\",\"item_id\":\"msg_note\",\"output_index\":0,\"content_index\":0,\"part\":{\"type\":\"output_text\",\"text\":\"\",\"annotations\":[]}}\n\n"
        "event: response.output_text.delta\ndata: {\"type\":\"response.output_text.delta\",\"item_id\":\"msg_note\",\"output_index\":0,\"content_index\":0,\"delta\":\"Checking.\"}\n\n"
        "event: response.output_text.done\ndata: {\"type\":\"response.output_text.done\",\"item_id\":\"msg_note\",\"output_index\":0,\"content_index\":0,\"text\":\"Checking.\"}\n\n"
        "event: response.output_item.done\ndata: {\"type\":\"response.output_item.done\",\"output_index\":0,\"item\":{\"id\":\"msg_note\",\"type\":\"message\",\"status\":\"completed\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"Checking.\",\"annotations\":[]}]}}\n\n"
        "event: response.output_item.added\ndata: {\"type\":\"response.output_item.added\",\"output_index\":1,\"item\":{\"id\":\"fc_2\",\"type\":\"function_call\",\"status\":\"in_progress\",\"call_id\":\"call_2\",\"name\":\"exec_command\",\"arguments\":\"\"}}\n\n"
        "event: response.function_call_arguments.delta\ndata: {\"type\":\"response.function_call_arguments.delta\",\"item_id\":\"fc_2\",\"output_index\":1,\"delta\":\"{\\\"command\\\":\\\"true\\\"}\"}\n\n"
        "event: response.function_call_arguments.done\ndata: {\"type\":\"response.function_call_arguments.done\",\"item_id\":\"fc_2\",\"output_index\":1,\"arguments\":\"{\\\"command\\\":\\\"true\\\"}\"}\n\n"
        "event: response.output_item.done\ndata: {\"type\":\"response.output_item.done\",\"output_index\":1,\"item\":{\"id\":\"fc_2\",\"type\":\"function_call\",\"status\":\"completed\",\"call_id\":\"call_2\",\"name\":\"exec_command\",\"arguments\":\"{\\\"command\\\":\\\"true\\\"}\"}}\n\n"
        "event: response.completed\ndata: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_call_text\",\"status\":\"completed\",\"output\":[]}}\n\n";
    struct parsed_stream emitted = parsed_new(1024u);

    if (parse_stream(wire, 23u, &emitted) != 0) {
        fprintf(stderr, "phase-absent text+tool parse: %s\n", emitted.error);
        assert(0);
    }
    assert(emitted.graph.count == 2u);
    assert(snag_response_graph_item(&emitted.graph, 0).kind == SNAG_ITEM_ASSISTANT);
    assert(snag_response_graph_item(&emitted.graph, 0).phase == SNAG_PHASE_COMMENTARY);
    assert(strcmp(snag_response_graph_item(&emitted.graph, 0).text, "Checking.") == 0);
    assert(snag_response_graph_item(&emitted.graph, 1).kind == SNAG_ITEM_TOOL_CALL);
    assert(strcmp(snag_response_graph_item(&emitted.graph, 1).provider_call_id, "call_2") == 0);
    parsed_free(&emitted);
}

static void
test_inert_only_response_has_empty_graph(void)
{
    static const char wire[] =
        "data: {\"type\":\"response.created\",\"response\":{\"id\":\"resp_empty\",\"status\":\"in_progress\",\"output\":[]}}\n\n"
        "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_empty\",\"status\":\"completed\",\"output\":[{\"type\":\"future_action\",\"name\":\"exec_command\",\"arguments\":{}},{\"id\":\"msg_empty\",\"type\":\"message\",\"status\":\"completed\",\"role\":\"assistant\",\"content\":[{\"type\":\"future_content\",\"text\":\"hidden\"}]}]}}\n\n";
    struct parsed_stream emitted = parsed_new(1024u);

    assert(parse_stream(wire, 19u, &emitted) == 0);
    assert(emitted.graph.count == 0u);
    assert(emitted.calls == 0u);
    parsed_free(&emitted);
}

static void
test_function_call_arguments(void)
{
    static const char wire[] =
        "event: response.created\ndata: {\"type\":\"response.created\",\"response\":{\"id\":\"resp_call\",\"status\":\"in_progress\",\"output\":[]}}\n\n"
        "event: response.output_item.added\ndata: {\"type\":\"response.output_item.added\",\"output_index\":0,\"item\":{\"id\":\"fc_1\",\"type\":\"function_call\",\"status\":\"in_progress\",\"call_id\":\"call_1\",\"name\":\"exec_command\",\"arguments\":\"\"}}\n\n"
        "event: response.function_call_arguments.delta\ndata: {\"type\":\"response.function_call_arguments.delta\",\"item_id\":\"fc_1\",\"output_index\":0,\"delta\":\"{\\\"command\\\":\\\"printf hi\\\"\"}\n\n"
        "event: response.function_call_arguments.delta\ndata: {\"type\":\"response.function_call_arguments.delta\",\"item_id\":\"fc_1\",\"output_index\":0,\"delta\":\"}\"}\n\n"
        "event: response.function_call_arguments.done\ndata: {\"type\":\"response.function_call_arguments.done\",\"item_id\":\"fc_1\",\"output_index\":0,\"arguments\":\"{\\\"command\\\":\\\"printf hi\\\"}\"}\n\n"
        "event: response.output_item.done\ndata: {\"type\":\"response.output_item.done\",\"output_index\":0,\"item\":{\"id\":\"fc_1\",\"type\":\"function_call\",\"status\":\"completed\",\"call_id\":\"call_1\",\"name\":\"exec_command\",\"arguments\":\"{\\\"command\\\":\\\"printf hi\\\"}\"}}\n\n"
        "event: response.completed\ndata: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_call\",\"status\":\"completed\",\"output\":[]}}\n\n";
    struct parsed_stream emitted = parsed_new(1024u);

    assert(parse_stream(wire, 31u, &emitted) == 0);
    assert(emitted.graph.count == 1u);
    assert(snag_response_graph_item(&emitted.graph, 0).kind == SNAG_ITEM_TOOL_CALL);
    assert(strcmp(snag_response_graph_item(&emitted.graph, 0).provider_call_id, "call_1") == 0);
    assert(strcmp(snag_response_graph_item(&emitted.graph, 0).name, "exec_command") == 0);
    assert(strcmp(snag_json_string(snag_response_graph_item(&emitted.graph, 0).arguments, "command"),
                  "printf hi") == 0);
    assert(emitted.calls == 0u);
    parsed_free(&emitted);
}

static void
test_refusal(void)
{
    static const char wire[] =
        "data: {\"type\":\"response.created\",\"response\":{\"id\":\"resp_refuse\",\"status\":\"in_progress\",\"output\":[]}}\n\n"
        "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_refuse\",\"status\":\"completed\",\"output\":[{\"id\":\"msg_refuse\",\"type\":\"message\",\"status\":\"completed\",\"role\":\"assistant\",\"phase\":\"final_answer\",\"content\":[{\"type\":\"refusal\",\"refusal\":\"I cannot do that.\"}]}]}}\n\n";
    struct parsed_stream emitted = parsed_new(1024u);

    assert(parse_stream(wire, 0u, &emitted) == 0);
    assert(emitted.graph.count == 1u);
    assert(snag_response_graph_item(&emitted.graph, 0).kind == SNAG_ITEM_REFUSAL);
    assert(strcmp(snag_response_graph_item(&emitted.graph, 0).text, "I cannot do that.") == 0);
    assert(emitted.last_kind == SNAG_ITEM_REFUSAL);
    parsed_free(&emitted);
}

static void
test_invalid_call_after_public_item(void)
{
    static const char *const calls[] = {
        "\"name\":\"unknown\",\"arguments\":\"{}\"",
        "\"name\":\"exec_command\",\"arguments\":\"[]\""
    };

    for (size_t i = 0; i < sizeof(calls) / sizeof(calls[0]); ++i) {
        struct parsed_stream emitted = parsed_new(1024u);

        assert(snag_response_graph_add_public(&emitted.graph, SNAG_ITEM_ASSISTANT,
            SNAG_PHASE_FINAL_ANSWER, "retained", "previous graph") == 0);
        struct snag_buf wire = {.max = 4096u};
        assert(snag_buf_printf(&wire,
            "data: {\"type\":\"response.created\",\"response\":{\"id\":\"r\",\"status\":\"in_progress\",\"output\":[]}}\n\n"
            "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"r\",\"status\":\"completed\",\"output\":["
            "{\"id\":\"m\",\"type\":\"message\",\"status\":\"completed\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"visible\"}]},"
            "{\"id\":\"f\",\"type\":\"function_call\",\"status\":\"completed\",\"call_id\":\"c\",%s}]}}\n\n",
            calls[i]) == 0);
        assert(snag_buf_terminate(&wire) == 0);
        /* Finalization must reject the whole staged graph, even after a
         * successful public item. Already emitted text cannot be withdrawn. */
        assert(parse_stream((char *)wire.data, 7u, &emitted) < 0);
        assert(strstr(emitted.error, "function"));
        assert(emitted.text.len == 7u);
        assert(emitted.graph.count == 1u);
        assert(strcmp(snag_response_graph_item(&emitted.graph, 0).text, "previous graph") == 0);
        snag_buf_free(&wire);
        parsed_free(&emitted);
    }
}

static void
test_protocol_conflicts_fail_closed(void)
{
    static const char *const bad[] = {
        "data: [DONE]\n\n",
        "data: {\"type\":\"response.created\",\"response\":{\"id\":\"r\",\"status\":\"in_progress\",\"output\":[]}}\n\ndata: [DONE]\n\n",
        "data: {\"type\":\"response.created\",\"response\":{\"id\":\"r\",\"status\":\"in_progress\",\"output\":[]}}\n\ndata: {\"type\":\"response.completed\",\"response\":{\"id\":\"r\",\"status\":\"completed\",\"output\":[]}}\n\nevent: response.completed\ndata: [DONE]\n\n",
        "event: wrong\ndata: {\"type\":\"response.created\",\"response\":{\"id\":\"r\",\"status\":\"in_progress\",\"output\":[]}}\n\n",
        "data: {\"type\":\"response.created\",\"response\":{\"id\":\"r\",\"status\":\"in_progress\",\"output\":[]}}\n\ndata: {\"type\":\"response.output_item.added\",\"output_index\":0,\"item\":{\"id\":\"m\",\"type\":\"message\",\"status\":\"in_progress\",\"role\":\"assistant\",\"phase\":\"final_answer\",\"content\":[]}}\n\ndata: {\"type\":\"response.content_part.added\",\"item_id\":\"m\",\"output_index\":0,\"content_index\":0,\"part\":{\"type\":\"output_text\",\"text\":\"\",\"annotations\":[]}}\n\ndata: {\"type\":\"response.output_text.delta\",\"item_id\":\"m\",\"output_index\":0,\"content_index\":0,\"delta\":\"a\"}\n\ndata: {\"type\":\"response.output_text.done\",\"item_id\":\"m\",\"output_index\":0,\"content_index\":0,\"text\":\"b\"}\n\n",
        "data: {\"type\":\"response.created\",\"response\":{\"id\":\"r\",\"status\":\"in_progress\",\"output\":[]}}\n\ndata: {\"type\":\"response.completed\",\"response\":{\"id\":\"r\",\"status\":\"completed\",\"output\":[{\"id\":\"m\",\"type\":\"message\",\"status\":\"completed\",\"role\":\"assistant\",\"phase\":\"final_answer\",\"content\":[{\"type\":\"output_text\",\"text\":\"x\",\"annotations\":[]},{\"type\":\"refusal\",\"refusal\":\"no\"}]}]}}\n\n",
        "data: {\"type\":\"response.created\",\"response\":{\"id\":\"r\",\"status\":\"in_progress\",\"output\":[]}}\n\ndata: {\"type\":\"response.completed\",\"response\":{\"id\":\"r\",\"status\":\"completed\",\"usage\":{\"input_tokens\":4,\"output_tokens\":3,\"total_tokens\":99},\"output\":[]}}\n\n",
        "data: {\"type\":\"response.created\",\"response\":{\"id\":\"r\",\"status\":\"in_progress\",\"output\":[]}}\n\ndata: {\"type\":\"future.event\"}\n\n",
        "data: {\"type\":\"response.created\",\"response\":{\"id\":\"r\",\"status\":\"in_progress\",\"output\":[]}}\n\ndata: {\"type\":\"response.output_item.added\",\"output_index\":0,\"item\":{\"type\":\"future_item\"}}\n\ndata: {\"type\":\"response.output_item.done\",\"output_index\":0,\"item\":{\"id\":\"m\",\"type\":\"message\",\"status\":\"completed\",\"role\":\"assistant\",\"content\":[]}}\n\n",
        "data: {\"type\":\"response.created\",\"response\":{\"id\":\"r\",\"status\":\"in_progress\",\"output\":[]}}\n\ndata: {\"type\":\"response.output_item.added\",\"output_index\":0,\"item\":{\"id\":\"m\",\"type\":\"message\",\"status\":\"in_progress\",\"role\":\"assistant\",\"content\":[]}}\n\ndata: {\"type\":\"response.output_item.done\",\"output_index\":0,\"item\":{\"type\":\"future_item\"}}\n\n",
        "data: {\"type\":\"response.created\",\"response\":{\"id\":\"r\",\"status\":\"in_progress\",\"output\":[]}}\n\ndata: {\"type\":\"response.output_item.added\",\"output_index\":0,\"item\":{\"id\":\"m\",\"type\":\"message\",\"status\":\"in_progress\",\"role\":\"assistant\",\"content\":[]}}\n\ndata: {\"type\":\"response.content_part.added\",\"item_id\":\"m\",\"output_index\":0,\"content_index\":0,\"part\":{\"type\":\"future_part\"}}\n\ndata: {\"type\":\"response.content_part.done\",\"item_id\":\"m\",\"output_index\":0,\"content_index\":0,\"part\":{\"type\":\"output_text\",\"text\":\"x\"}}\n\n",
        "data: {\"type\":\"response.created\",\"response\":{\"id\":\"r\",\"status\":\"in_progress\",\"output\":[]}}\n\ndata: {\"type\":\"response.completed\",\"response\":{\"id\":\"r\",\"status\":\"completed\",\"output\":[]}}\n\ndata: {\"type\":\"response.future.progress\"}\n\n",
        "data: {\"type\":\"response.created\",\"response\":{\"id\":\"r\",\"status\":\"in_progress\",\"output\":[]}}\n\n"
    };

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        struct parsed_stream emitted = parsed_new(1024u);

        assert(parse_stream(bad[i], 7u, &emitted) < 0);
        assert(emitted.error[0]);
        assert(emitted.graph.count == 0u);
        parsed_free(&emitted);
    }
}

static void
test_interleaved_content_bound(void)
{
    for (unsigned int count = 96u; count <= 97u; ++count) {
        struct snag_buf wire = {.max = 32768u};
        struct parsed_stream emitted = parsed_new(128u);
        assert(snag_buf_printf(&wire,
            "data: {\"type\":\"response.created\",\"response\":{\"id\":\"r\",\"status\":\"in_progress\",\"output\":[]}}\n\n") == 0);
        for (unsigned int item = 0; item < 2u; ++item)
            assert(snag_buf_printf(&wire,
                "data: {\"type\":\"response.output_item.added\",\"output_index\":%u,"
                "\"item\":{\"id\":\"m%u\",\"type\":\"message\",\"role\":\"assistant\","
                "\"phase\":\"%s\",\"status\":\"in_progress\",\"content\":[]}}\n\n", item, item,
                item ? "final_answer" : "commentary") == 0);
        for (unsigned int i = 0; i < count; ++i)
            assert(snag_buf_printf(&wire,
                "data: {\"type\":\"response.content_part.done\",\"output_index\":%u,"
                "\"item_id\":\"m%u\",\"content_index\":%u,"
                "\"part\":{\"type\":\"output_text\",\"text\":\"%c\"}}\n\n",
                i % 2u, i % 2u, i / 2u, i % 2u ? 'b' : 'a') == 0);
        assert(snag_buf_printf(&wire,
            "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"r\","
            "\"status\":\"completed\",\"output\":[]}}\n\n") == 0);
        int rc = parse_stream((char *)wire.data, 7u, &emitted);
        assert(emitted.calls == 96u && emitted.text.len == 96u);
        for (size_t i = 0; i < emitted.text.len; ++i)
            assert(emitted.text.data[i] == (i % 2u ? 'b' : 'a'));
        if (count == 96u) {
            assert(rc == 0 && emitted.graph.count == 2u);
            for (size_t item = 0; item < 2u; ++item) {
                const char *text = snag_response_graph_item(&emitted.graph, item).text;
                assert(strlen(text) == 48u && strspn(text, item ? "b" : "a") == 48u);
            }
        } else {
            assert(rc < 0 && emitted.graph.count == 0u);
            assert(!strcmp(emitted.error, "message content part was not announced"));
        }
        snag_buf_free(&wire);
        parsed_free(&emitted);
    }
}

static void
test_structured_capacity_failure(void)
{
    static const char payload[] =
        "{\"type\":\"response.failed\",\"response\":{\"error\":{"
        "\"code\":\"context_length_exceeded\",\"message\":\"too large\","
        "\"context_length\":272000,\"requested_tokens\":300000}}}";
    struct snag_responses_stream stream;
    struct snag_sse_record record;
    json_t *root;
    struct snag_provider_failure failure;

    snag_responses_stream_init(&stream, NULL, NULL);
    memset(&record, 0, sizeof(record));
    record.kind = SNAG_SSE_EVENT;
    record.event = (const unsigned char *)"response.failed";
    record.event_len = strlen("response.failed");
    record.data = (const unsigned char *)payload;
    record.data_len = strlen(payload);
    assert(snag_responses_sse_record(&stream, &record) < 0);
    assert(snag_provider_failure_is_capacity(&stream.provider_failure));
    assert(strcmp(stream.provider_failure.message, "too large") == 0);
    assert(stream.provider_failure.context_limit_tokens == 272000u);
    assert(stream.provider_failure.requested_input_tokens == 300000u);
    assert(snag_capacity_safety_ceiling(
        stream.provider_failure.context_limit_tokens,
        stream.provider_failure.requested_input_tokens, 128000u) == 144000u);
    assert(snag_capacity_safety_ceiling(0u, 0u, 0u) == 0u);
    assert(snag_capacity_safety_ceiling(0u, 1u, 0u) == 0u);
    assert(snag_capacity_safety_ceiling(0u, 2u, 0u) == 1u);
    assert(snag_capacity_safety_ceiling(42u, 12u, 0u) == 11u);
    assert(snag_capacity_safety_ceiling(42u, 0u, 42u) == 1u);
    assert(snag_capacity_safety_ceiling(42u, 0u, 43u) == 1u);
    snag_responses_stream_free(&stream);

    {
        static const char ordinary[] =
            "{\"error\":{\"code\":\"rate_limit_exceeded\","
            "\"message\":\"later\"}}";
        char json_error[128] = {0};
        root = snag_json_load_strict((const unsigned char *)ordinary,
                                    strlen(ordinary), sizeof(ordinary),
                                    json_error, sizeof(json_error));
    }
    assert(root);
    assert(snag_provider_failure_from_json(root, &failure) == 0);
    assert(!snag_provider_failure_is_capacity(&failure));
    assert(strcmp(failure.code, "rate_limit_exceeded") == 0);
    json_decref(root);

    {
        static const char top_level[] =
            "{\"type\":\"error\",\"code\":\"context_length_exceeded\","
            "\"message\":\"top-level failure\",\"max_context_tokens\":42}";
        char json_error[128] = {0};
        root = snag_json_load_strict((const unsigned char *)top_level,
                                    strlen(top_level), sizeof(top_level),
                                    json_error, sizeof(json_error));
    }
    assert(root);
    assert(snag_provider_failure_from_json(root, &failure) == 0);
    assert(snag_provider_failure_is_capacity(&failure));
    assert(strcmp(failure.message, "top-level failure") == 0);
    assert(failure.context_limit_tokens == 42u);
    assert(json_object_set_new(root, "context_length", json_integer(42)) == 0);
    assert(snag_provider_failure_from_json(root, &failure) == 0);
    assert(json_object_set_new(root, "context_length", json_integer(43)) == 0);
    assert(snag_provider_failure_from_json(root, &failure) < 0);
    assert(json_object_set_new(root, "context_length", json_integer(0)) == 0);
    assert(snag_provider_failure_from_json(root, &failure) < 0);
    assert(json_object_set_new(root, "context_length", json_null()) == 0);
    assert(snag_provider_failure_from_json(root, &failure) == 0);
    assert(failure.context_limit_tokens == 42u);
    assert(failure.requested_input_tokens == 0u);
    assert(json_object_set_new(root, "requested_input_tokens", json_integer(0)) == 0);
    assert(snag_provider_failure_from_json(root, &failure) < 0);
    assert(json_object_set_new(root, "requested_input_tokens", json_integer(1)) == 0);
    assert(snag_provider_failure_from_json(root, &failure) == 0);
    assert(failure.requested_input_tokens == 1u);
    json_decref(root);
}

static void
test_provider_context_formats(void)
{
    const struct { const char *json; bool capacity; uint64_t limit, input; } cases[] = {
        {"{\"error\":{\"code\":\"invalid_prompt\"},\"error_type\":\"context_length_exceeded\"}", true, 0, 0},
        {"{\"response\":{\"error\":{\"code\":\"invalid_prompt\"},\"error_type\":\"context_length_exceeded\"}}", true, 0, 0},
        {"{\"error\":{\"code\":400,\"metadata\":{\"error_type\":\"context_length_exceeded\"}}}", true, 0, 0},
        {"{\"error\":{\"code\":400,\"type\":\"exceed_context_size_error\",\"n_ctx\":8192,\"n_prompt_tokens\":9000}}", true, 8192, 9000},
        {"{\"code\":400,\"type\":\"exceed_context_size_error\",\"n_ctx\":8192,\"n_prompt_tokens\":9000}", true, 8192, 9000},
        {"{\"error\":{\"type\":\"invalid_request_error\",\"param\":\"input\",\"message\":\"The engine prompt length 9000 exceeds the max_model_len 8192. Please reduce prompt.\"}}", true, 8191, 9000},
        {"{\"error\":{\"type\":\"invalid_request_error\",\"param\":\"model\",\"message\":\"The engine prompt length 9000 exceeds the max_model_len 8192. Please reduce prompt.\"}}", false, 0, 0},
        {"{\"error\":{\"code\":400,\"message\":\"context error\"}}", false, 0, 0},
        {"{\"error\":{\"code\":\"invalid_prompt\"},\"error_type\":\"max_tokens_exceeded\"}", false, 0, 0},
        {"{\"error\":{\"code\":\"invalid_prompt\"},\"error_type\":\"authentication_error\"}", false, 0, 0},
        {"{\"error\":{\"code\":\"invalid_prompt\"}}", false, 0, 0},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        struct snag_provider_failure failure;
        json_t *root = json_loadb(cases[i].json, strlen(cases[i].json), 0, NULL);
        assert(root);
        assert(snag_provider_failure_from_json(root, &failure) == 0);
        assert(snag_provider_failure_is_capacity(&failure) == cases[i].capacity);
        assert(failure.context_limit_tokens == cases[i].limit);
        assert(failure.requested_input_tokens == cases[i].input);
        json_decref(root);
    }
    struct snag_responses_stream stream;
    const char *created = "{\"type\":\"response.created\",\"response\":{\"id\":\"resp_cap\",\"status\":\"in_progress\"}}";
    const char *completed = "{\"type\":\"response.completed\",\"response\":{\"id\":\"resp_cap\",\"status\":\"completed\",\"error_type\":\"context_length_exceeded\",\"output\":[]}}";
    snag_responses_stream_init(&stream, NULL, NULL);
    struct snag_sse_record record = {0};
    record.kind = SNAG_SSE_EVENT;
    record.data = (const unsigned char *)created;
    record.data_len = strlen(created);
    assert(snag_responses_sse_record(&stream, &record) == 0);
    record.data = (const unsigned char *)completed;
    record.data_len = strlen(completed);
    assert(snag_responses_sse_record(&stream, &record) < 0);
    assert(snag_provider_failure_is_capacity(&stream.provider_failure));
    snag_responses_stream_free(&stream);
}

static void
test_reasoning_content_parts(void)
{
    static const char created[] =
        "data: {\"type\":\"response.created\",\"response\":{\"id\":\"r\",\"status\":\"in_progress\",\"output\":[]}}\n\n"
        "data: {\"type\":\"response.output_item.added\",\"output_index\":0,\"item\":{\"type\":\"reasoning\",\"id\":\"rs\",\"summary\":[]}}\n\n";
    static const char finish[] =
        "data: {\"type\":\"response.output_item.done\",\"output_index\":0,\"item\":{\"type\":\"reasoning\",\"id\":\"rs\",\"content\":[{\"type\":\"reasoning_text\",\"text\":\"hidden\"}]}}\n\n"
        "data: {\"type\":\"response.output_item.added\",\"output_index\":1,\"item\":{\"type\":\"message\",\"id\":\"m\",\"role\":\"assistant\",\"status\":\"in_progress\",\"content\":[]}}\n\n"
        "data: {\"type\":\"response.content_part.added\",\"output_index\":1,\"item_id\":\"m\",\"content_index\":0,\"part\":{\"type\":\"output_text\",\"text\":\"\"}}\n\n"
        "data: {\"type\":\"response.output_text.delta\",\"output_index\":1,\"item_id\":\"m\",\"content_index\":0,\"delta\":\"ok\"}\n\n"
        "data: {\"type\":\"response.content_part.done\",\"output_index\":1,\"item_id\":\"m\",\"content_index\":0,\"part\":{\"type\":\"output_text\",\"text\":\"ok\"}}\n\n"
        "data: {\"type\":\"response.output_item.done\",\"output_index\":1,\"item\":{\"type\":\"message\",\"id\":\"m\",\"role\":\"assistant\",\"status\":\"completed\",\"content\":[{\"type\":\"output_text\",\"text\":\"ok\"}]}}\n\n"
        "data: {\"type\":\"response.output_item.done\",\"output_index\":2,\"item\":{\"type\":\"function_call\",\"id\":\"f\",\"call_id\":\"c\",\"name\":\"exec_command\",\"arguments\":\"{}\",\"status\":\"completed\"}}\n\n"
        "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"r\",\"status\":\"completed\",\"output\":[]}}\n\n";
    /* DeepSeek content parts, vLLM reasoning deltas, and llama.cpp summary
     * events all stay private; public messages and calls keep their indexes. */
    static const char *variants[] = {
        "data: {\"type\":\"response.content_part.added\",\"output_index\":0,\"item_id\":\"rs\",\"content_index\":0,\"part\":{\"type\":\"reasoning_text\",\"text\":\"\"}}\n\n"
        "data: {\"type\":\"response.reasoning_text.delta\",\"output_index\":0,\"item_id\":\"rs\",\"content_index\":0,\"delta\":\"hidden\"}\n\n"
        "data: {\"type\":\"response.content_part.done\",\"output_index\":0,\"item_id\":\"rs\",\"content_index\":0,\"part\":{\"type\":\"reasoning_text\",\"text\":\"hidden\"}}\n\n",
        "data: {\"type\":\"response.reasoning_part.added\",\"output_index\":0,\"item_id\":\"rs\",\"content_index\":0,\"part\":{\"type\":\"reasoning_text\",\"text\":\"\"}}\n\n"
        "data: {\"type\":\"response.reasoning_text.delta\",\"output_index\":0,\"item_id\":\"rs\",\"content_index\":0,\"delta\":\"hidden\"}\n\n"
        "data: {\"type\":\"response.reasoning_text.done\",\"output_index\":0,\"item_id\":\"rs\",\"content_index\":0,\"text\":\"hidden\"}\n\n"
        "data: {\"type\":\"response.reasoning_part.done\",\"output_index\":0,\"item_id\":\"rs\",\"content_index\":0,\"part\":{\"type\":\"reasoning_text\",\"text\":\"hidden\"}}\n\n",
        "data: {\"type\":\"response.reasoning_summary_part.added\",\"item_id\":\"rs\",\"part\":{\"type\":\"summary_text\",\"text\":\"\"}}\n\n"
        "data: {\"type\":\"response.reasoning_summary_text.delta\",\"item_id\":\"rs\",\"delta\":\"hidden\"}\n\n"
        "data: {\"type\":\"response.reasoning_summary_text.done\",\"item_id\":\"rs\",\"text\":\"hidden\"}\n\n"
        "data: {\"type\":\"response.reasoning_summary_part.done\",\"item_id\":\"rs\",\"part\":{\"type\":\"summary_text\",\"text\":\"hidden\"}}\n\n",
        "data: {\"type\":\"response.content_part.done\",\"output_index\":0,\"content_index\":0,\"part\":{\"type\":\"future_content\",\"name\":\"exec_command\",\"arguments\":\"bad\"}}\n\n",
    };
    for (size_t v = 0; v < sizeof(variants) / sizeof(variants[0]); ++v) {
        struct snag_buf wire = {.max = 32768u};
        assert(snag_buf_printf(&wire, "%s%s%s", created, variants[v], finish) == 0);
        for (size_t chunk = 0; chunk < 3u; ++chunk) {
            struct parsed_stream parsed = parsed_new(1024u);
            assert(parse_stream((char *)wire.data, chunk == 2u ? 19u : chunk,
                                &parsed) == 0);
            assert(parsed.text.len == 2u && memcmp(parsed.text.data, "ok", 2u) == 0);
            assert(parsed.graph.count == 2u);
            assert(snag_response_continuation_valid(parsed.graph.continuation, parsed.graph.count));
            assert(json_array_size(parsed.graph.continuation) == 1u);
            json_t *record = json_array_get(parsed.graph.continuation, 0);
            assert(json_integer_value(json_object_get(record, "before")) == 0);
            assert(strcmp(snag_json_string(json_array_get(json_object_get(
                json_object_get(record, "item"), "content"), 0), "text"), "hidden") == 0);
            assert(snag_response_graph_item(&parsed.graph, 0u).kind == SNAG_ITEM_ASSISTANT);
            assert(snag_response_graph_item(&parsed.graph, 1u).kind == SNAG_ITEM_TOOL_CALL);
            assert(strcmp(snag_response_graph_item(&parsed.graph, 1u).name, "exec_command") == 0);
            assert(parsed.last_index == 1u);
            assert(strcmp(parsed.last_provider_id, "m") == 0);
            parsed_free(&parsed);
        }
        snag_buf_free(&wire);
    }

    /* An ignored item cannot swallow public semantics or invent an index. */
    static const char *invalid[] = {
        "{\"type\":\"response.content_part.added\",\"output_index\":0,\"item_id\":\"rs\",\"content_index\":0,\"part\":{\"type\":\"output_text\",\"text\":\"bad\"}}",
        "{\"type\":\"response.content_part.done\",\"output_index\":0,\"item_id\":\"rs\",\"content_index\":0,\"part\":{\"type\":\"refusal\",\"refusal\":\"bad\"}}",
        "{\"type\":\"response.output_text.delta\",\"output_index\":0,\"item_id\":\"rs\",\"content_index\":0,\"delta\":\"bad\"}",
        "{\"type\":\"response.refusal.done\",\"output_index\":0,\"item_id\":\"rs\",\"content_index\":0,\"refusal\":\"bad\"}",
        "{\"type\":\"response.function_call_arguments.delta\",\"output_index\":0,\"item_id\":\"rs\",\"delta\":\"{}\"}",
        "{\"type\":\"response.content_part.added\",\"output_index\":1,\"item_id\":\"rs\",\"content_index\":0,\"part\":{\"type\":\"reasoning_text\"}}",
        "{\"type\":\"response.content_part.added\",\"output_index\":-1,\"content_index\":0,\"part\":{\"type\":\"reasoning_text\"}}",
        "{\"type\":\"response.content_part.added\",\"output_index\":0,\"content_index\":96,\"part\":{\"type\":\"reasoning_text\"}}",
        "{\"type\":\"response.content_part.added\",\"output_index\":0,\"content_index\":0,\"part\":{}}",
        "{\"type\":\"response.content_part.done\",\"output_index\":0,\"content_index\":0,\"part\":null}",
        "{\"type\":\"response.output_item.done\",\"output_index\":0,\"item\":{\"type\":\"function_call\",\"id\":\"f\",\"call_id\":\"c\",\"name\":\"exec_command\",\"arguments\":\"{}\",\"status\":\"completed\"}}",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        struct snag_buf wire = {.max = 32768u};
        struct parsed_stream parsed = parsed_new(1024u);
        assert(snag_buf_printf(&wire, "%sdata: %s\n\n%s", created, invalid[i], finish) == 0);
        assert(parse_stream((char *)wire.data, 1u, &parsed) < 0);
        assert(parsed.calls == 0u && parsed.graph.count == 0u);
        parsed_free(&parsed);
        snag_buf_free(&wire);
    }
}

static void
test_reasoning_part_cannot_complete_response(void)
{
    static const char wire[] =
        "data: {\"type\":\"response.created\",\"response\":{\"id\":\"r\",\"status\":\"in_progress\",\"output\":[]}}\n\n"
        "data: {\"type\":\"response.output_item.added\",\"output_index\":0,\"item\":{\"type\":\"reasoning\",\"id\":\"rs\"}}\n\n"
        "data: {\"type\":\"response.content_part.done\",\"output_index\":0,\"content_index\":0,\"part\":{\"type\":\"reasoning_text\",\"text\":\"hidden\"}}\n\n";
    struct parsed_stream parsed = parsed_new(1024u);
    assert(parse_stream(wire, 7u, &parsed) < 0);
    assert(parsed.calls == 0u && parsed.graph.count == 0u);
    parsed_free(&parsed);
}

static void
test_reasoning_terminal_and_validation(void)
{
    static const char ordered[] =
        "data: {\"type\":\"response.created\",\"response\":{\"id\":\"r\",\"status\":\"in_progress\",\"output\":[]}}\n\n"
        "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"r\",\"status\":\"completed\",\"output\":["
        "{\"type\":\"message\",\"id\":\"m\",\"role\":\"assistant\",\"status\":\"completed\",\"content\":[{\"type\":\"output_text\",\"text\":\"progress\"}]},"
        "{\"type\":\"reasoning\",\"id\":\"rs_a\",\"summary\":[],\"encrypted_content\":\"first\"},"
        "{\"type\":\"function_call\",\"id\":\"f\",\"call_id\":\"c\",\"name\":\"exec_command\",\"arguments\":\"{}\",\"status\":\"completed\"},"
        "{\"type\":\"reasoning\",\"id\":\"rs_b\",\"summary\":[],\"encrypted_content\":\"second\"}]}}\n\n";
    struct parsed_stream ordered_result = parsed_new(1024u);
    assert(parse_stream(ordered, 7u, &ordered_result) == 0);
    assert(ordered_result.graph.count == 2u);
    assert(json_array_size(ordered_result.graph.continuation) == 2u);
    for (size_t i = 0u; i < 2u; ++i)
        assert(json_integer_value(json_object_get(json_array_get(
            ordered_result.graph.continuation, i), "before")) == (json_int_t)i + 1);
    parsed_free(&ordered_result);
    static const char *snapshots[] = {
        "{\"type\":\"reasoning\",\"id\":\"rs\",\"summary\":[],\"encrypted_content\":\"opaque-final\"}",
        "{\"type\":\"reasoning\",\"id\":\"rs\",\"content\":[{\"type\":\"reasoning_text\",\"text\":\"retained\"}],\"summary\":null,\"encrypted_content\":null,\"status\":null}",
        "{\"type\":\"reasoning\",\"summary\":[{\"type\":\"summary_text\",\"text\":\"private summary\"}]}"
    };
    for (size_t i = 0u; i < sizeof(snapshots) / sizeof(snapshots[0]); ++i) {
        struct snag_buf wire = {.max = 8192u};
        struct parsed_stream parsed = parsed_new(1024u);
        assert(snag_buf_printf(&wire,
            "data: {\"type\":\"response.created\",\"response\":{\"id\":\"r\",\"status\":\"in_progress\",\"output\":[]}}\n\n"
            "data: {\"type\":\"response.output_item.done\",\"output_index\":0,\"item\":{\"type\":\"reasoning\",\"summary\":[]}}\n\n"
            "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"r\",\"status\":\"completed\",\"output\":[%s]}}\n\n",
            snapshots[i]) == 0);
        assert(parse_stream((char *)wire.data, 1u, &parsed) == 0);
        assert(parsed.calls == 0u && parsed.graph.count == 0u);
        assert(json_array_size(parsed.graph.continuation) == 1u);
        assert(snag_response_continuation_valid(parsed.graph.continuation, 0u));
        struct snag_graph_decision decision;
        assert(snag_response_graph_classify(&parsed.graph, &decision, parsed.error, sizeof(parsed.error)) == 0);
        assert(decision.outcome == SNAG_GRAPH_NONPRODUCTIVE);
        json_t *item = json_object_get(json_array_get(parsed.graph.continuation, 0), "item");
        assert(i != 0u || strcmp(snag_json_string(item, "encrypted_content"), "opaque-final") == 0);
        parsed_free(&parsed);
        snag_buf_free(&wire);
    }
    json_t *item = json_pack("{s:s,s:[]}", "type", "reasoning", "summary");
    assert(json_object_set_new(item, "signature", json_string("opaque-provider-extension")) == 0);
    assert(json_object_set_new(item, "name", json_string("exec_command")) == 0);
    assert(snag_reasoning_item_valid(item));
    json_t *records = json_pack("[{s:i,s:O}]", "before", 0, "item", item);
    assert(snag_response_continuation_valid(records, 0u));
    assert(json_object_set_new(json_array_get(records, 0), "before", json_integer(1)) == 0);
    assert(!snag_response_continuation_valid(records, 0u));
    assert(json_object_set_new(item, "content", json_pack("[{s:s,s:s}]",
        "type", "output_text", "text", "must not become public")) == 0);
    assert(!snag_reasoning_item_valid(item));
    assert(json_object_set_new(item, "content", json_string("not-an-array")) == 0);
    assert(!snag_reasoning_item_valid(item));
    assert(json_object_del(item, "content") == 0);
    assert(json_object_set_new(item, "encrypted_content", json_integer(42)) == 0);
    assert(!snag_reasoning_item_valid(item));
    assert(json_object_del(item, "encrypted_content") == 0);
    char *huge = malloc(SNAG_MAX_RESPONSE_GRAPH + 2u);
    assert(huge);
    memset(huge, 'x', SNAG_MAX_RESPONSE_GRAPH + 1u);
    huge[SNAG_MAX_RESPONSE_GRAPH + 1u] = '\0';
    assert(json_object_set_new(item, "encrypted_content", json_string(huge)) == 0);
    assert(!snag_reasoning_item_valid(item));
    free(huge);
    json_decref(records);
    json_decref(item);
}

int
main(void)
{
    test_deltas_survive_empty_terminal_output();
    test_reasoning_content_parts();
    test_reasoning_terminal_and_validation();
    test_reasoning_part_cannot_complete_response();
    test_terminal_snapshot_can_supply_unseen_items();
    test_failed_snapshot_preserves_only_consistent_text();
    test_empty_public_items_get_specific_correction();
    test_oversized_public_items_get_specific_correction();
    test_structured_keepalives_do_not_end_response();
    test_public_stream(0u);
    test_public_stream(1u);
    test_unused_annotation_shapes_are_ignored();
    test_public_stream(2u);
    test_phase_absent_text_before_tool_stays_commentary();
    test_public_stream(3u);
    test_public_stream(4u);
    test_public_stream(5u);
    test_inert_only_response_has_empty_graph();
    test_function_call_arguments();
    test_refusal();
    test_invalid_call_after_public_item();
    test_protocol_conflicts_fail_closed();
    test_structured_capacity_failure();
    test_interleaved_content_bound();
    test_provider_context_formats();
    puts("test_responses: ok");
    return 0;
}

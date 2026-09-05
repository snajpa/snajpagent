/* SPDX-License-Identifier: GPL-2.0-only */
#include "responses.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct emitted {
    struct snag_buf text;
    size_t calls;
    size_t last_index;
    enum snag_item_kind last_kind;
    enum snag_item_phase last_phase;
    char last_provider_id[64];
};

static int
capture_emit(void *opaque, size_t output_index, enum snag_item_kind kind,
             enum snag_item_phase phase, const char *provider_item_id,
             const char *text, size_t len)
{
    struct emitted *emitted = opaque;

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
parse_stream(const char *wire, size_t chunk, struct snag_response_graph *graph,
             struct emitted *emitted, char *error, size_t error_size)
{
    struct snag_responses_stream responses;
    struct snag_sse_parser sse;
    size_t len = strlen(wire);
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
        rc = snag_responses_stream_finish(&responses, graph,
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
    struct snag_response_graph graph;
    struct emitted emitted;
    char error[256] = {0};

    snag_response_graph_init(&graph);
    memset(&emitted, 0, sizeof(emitted));
    snag_buf_init(&emitted.text, SNAG_MAX_PUBLIC_ITEM + 1u);
    assert(parse_stream(wire, 1u, &graph, &emitted,
                        error, sizeof(error)) == 0);
    assert(graph.count == 1u);
    assert(strcmp(graph.provider_response_id, "resp_ping") == 0);
    assert(graph.items[0].kind == SNAG_ITEM_ASSISTANT);
    assert(graph.items[0].phase == SNAG_PHASE_FINAL_ANSWER);
    assert(strcmp(graph.items[0].text, "haha") == 0);
    assert(emitted.calls == 2u);
    assert(emitted.text.len == 4u);
    assert(memcmp(emitted.text.data, "haha", 4u) == 0);
    assert(graph.usage.input_known && graph.usage.input_tokens == 12u);
    assert(graph.usage.output_known && graph.usage.output_tokens == 4u);
    assert(graph.usage.reasoning_known && graph.usage.reasoning_tokens == 2u);
    assert(graph.usage.total_known && graph.usage.total_tokens == 16u);
    snag_buf_free(&emitted.text);
    snag_response_graph_free(&graph);
}

static void
test_terminal_snapshot_can_supply_unseen_items(void)
{
    static const char wire[] =
        "data: {\"type\":\"response.created\",\"response\":{\"id\":\"resp_snapshot\",\"status\":\"in_progress\",\"output\":[]}}\n\n"
        "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_snapshot\",\"status\":\"completed\",\"output\":[{\"id\":\"msg_comment\",\"type\":\"message\",\"status\":\"completed\",\"role\":\"assistant\",\"phase\":\"commentary\",\"content\":[{\"type\":\"output_text\",\"text\":\"Working. \",\"annotations\":[]},{\"type\":\"output_text\",\"text\":\"Done.\",\"annotations\":[]}]},{\"id\":\"msg_final\",\"type\":\"message\",\"status\":\"completed\",\"role\":\"assistant\",\"phase\":\"final_answer\",\"content\":[{\"type\":\"output_text\",\"text\":\"answer\",\"annotations\":[]}]}]}}\n\n";
    struct snag_response_graph graph;
    struct emitted emitted;
    char error[256] = {0};

    snag_response_graph_init(&graph);
    memset(&emitted, 0, sizeof(emitted));
    snag_buf_init(&emitted.text, 1024u);
    if (parse_stream(wire, 17u, &graph, &emitted,
                     error, sizeof(error)) != 0) {
        fprintf(stderr, "snapshot parse: %s\n", error);
        assert(0);
    }
    assert(graph.count == 2u);
    assert(graph.items[0].phase == SNAG_PHASE_COMMENTARY);
    assert(strcmp(graph.items[0].text, "Working. Done.") == 0);
    assert(graph.items[1].phase == SNAG_PHASE_FINAL_ANSWER);
    assert(strcmp(graph.items[1].text, "answer") == 0);
    assert(emitted.last_index == 1u);
    assert(emitted.last_phase == SNAG_PHASE_FINAL_ANSWER);
    snag_buf_free(&emitted.text);
    snag_response_graph_free(&graph);
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
        struct snag_response_graph graph;
        struct emitted emitted;
        char error[256] = {0};

        snag_response_graph_init(&graph);
        memset(&emitted, 0, sizeof(emitted));
        snag_buf_init(&emitted.text, 1024u);
        assert(parse_stream(wires[i], 1u, &graph, &emitted,
                            error, sizeof(error)) == 1);
        assert(strcmp(error, SNAG_EMPTY_OUTPUT_CORRECTION) == 0);
        assert(graph.count == 0u);
        assert(graph.provider_response_id == NULL);
        assert(emitted.calls == 0u);
        assert(emitted.text.len == 0u);
        snag_buf_free(&emitted.text);
        snag_response_graph_free(&graph);
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
        struct snag_response_graph graph;
        struct emitted emitted;
        struct snag_buf wire;
        char error[256] = {0};

        snag_buf_init(&wire, SNAG_MAX_RESPONSE_GRAPH);
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
        snag_response_graph_init(&graph);
        memset(&emitted, 0, sizeof(emitted));
        snag_buf_init(&emitted.text, SNAG_MAX_PUBLIC_ITEM);
        assert(parse_stream((const char *)wire.data, 8191u, &graph, &emitted,
                            error, sizeof(error)) < 0);
        assert(strcmp(error, SNAG_OVERSIZED_OUTPUT_CORRECTION) == 0);
        assert(graph.count == 0u);
        assert(emitted.calls == 2u);
        assert(emitted.text.len == 2u * delta_len);
        snag_buf_free(&emitted.text);
        snag_response_graph_free(&graph);
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
    struct snag_response_graph graph;
    struct emitted emitted;
    char error[256] = {0};

    snag_response_graph_init(&graph);
    memset(&emitted, 0, sizeof(emitted));
    snag_buf_init(&emitted.text, 1024u);
    assert(parse_stream(wire, 7u, &graph, &emitted,
                        error, sizeof(error)) == 0);
    assert(strcmp(graph.provider_response_id, "resp_keepalive") == 0);
    assert(graph.count == 0u);
    assert(emitted.calls == 0u);
    snag_buf_free(&emitted.text);
    snag_response_graph_free(&graph);
}

static void
test_unused_response_events_are_ignored(void)
{
    static const char wire[] =
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
        "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_citation\",\"status\":\"completed\",\"output\":[]}}\n\n";
    struct snag_response_graph graph;
    struct emitted emitted;
    char error[256] = {0};

    snag_response_graph_init(&graph);
    memset(&emitted, 0, sizeof(emitted));
    snag_buf_init(&emitted.text, 1024u);
    assert(parse_stream(wire, 11u, &graph, &emitted,
                        error, sizeof(error)) == 0);
    assert(graph.count == 1u);
    assert(graph.items[0].kind == SNAG_ITEM_ASSISTANT);
    assert(strcmp(graph.items[0].text, "Source.") == 0);
    assert(emitted.calls == 1u);
    assert(emitted.text.len == 7u);
    assert(memcmp(emitted.text.data, "Source.", 7u) == 0);
    snag_buf_free(&emitted.text);
    snag_response_graph_free(&graph);
}

static void
test_terminal_snapshot_ignores_unused_text_metadata(void)
{
    static const char wire[] =
        "data: {\"type\":\"response.created\",\"response\":{\"id\":\"resp_file\",\"status\":\"in_progress\",\"output\":[]}}\n\n"
        "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_file\",\"status\":\"completed\",\"output\":[{\"id\":\"msg_file\",\"type\":\"message\",\"status\":\"completed\",\"role\":\"assistant\",\"phase\":\"final_answer\",\"content\":[{\"type\":\"output_text\",\"text\":\"See file.\",\"annotations\":{\"unused\":true},\"logprobs\":\"unused\"}]}]}}\n\n";
    struct snag_response_graph graph;
    struct emitted emitted;
    char error[256] = {0};

    snag_response_graph_init(&graph);
    memset(&emitted, 0, sizeof(emitted));
    snag_buf_init(&emitted.text, 1024u);
    assert(parse_stream(wire, 13u, &graph, &emitted,
                        error, sizeof(error)) == 0);
    assert(graph.count == 1u);
    assert(strcmp(graph.items[0].text, "See file.") == 0);
    assert(emitted.calls == 1u);
    snag_buf_free(&emitted.text);
    snag_response_graph_free(&graph);
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
        struct snag_response_graph graph;
        struct emitted emitted;
        struct snag_buf wire;
        char error[256] = {0};

        snag_buf_init(&wire, 8192u);
        assert(snag_buf_append(&wire, prefix, strlen(prefix)) == 0);
        assert(snag_buf_append(&wire, bad_suffixes[i],
                              strlen(bad_suffixes[i])) == 0);
        assert(snag_buf_append(&wire, finish, strlen(finish)) == 0);
        assert(snag_buf_terminate(&wire) == 0);
        snag_response_graph_init(&graph);
        memset(&emitted, 0, sizeof(emitted));
        snag_buf_init(&emitted.text, 1024u);
        assert(parse_stream((char *)wire.data, 7u, &graph, &emitted,
                            error, sizeof(error)) == 0);
        assert(graph.count == 1u);
        assert(strcmp(graph.items[0].text, "x") == 0);
        snag_buf_free(&emitted.text);
        snag_response_graph_free(&graph);
        snag_buf_free(&wire);
    }
}

static void
test_phase_absent_text_becomes_visible_final(void)
{
    static const char wire[] =
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
        "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_pong\",\"status\":\"completed\",\"output\":[]}}\n\n";
    struct snag_response_graph graph;
    struct emitted emitted;
    char error[256] = {0};

    snag_response_graph_init(&graph);
    memset(&emitted, 0, sizeof(emitted));
    snag_buf_init(&emitted.text, 1024u);
    if (parse_stream(wire, 9u, &graph, &emitted,
                     error, sizeof(error)) != 0) {
        fprintf(stderr, "phase-absent text parse: %s\n", error);
        assert(0);
    }
    assert(graph.count == 1u);
    assert(graph.items[0].kind == SNAG_ITEM_ASSISTANT);
    assert(graph.items[0].phase == SNAG_PHASE_FINAL_ANSWER);
    assert(strcmp(graph.items[0].provider_item_id, "msg_pong") == 0);
    assert(strcmp(graph.items[0].text, "pong") == 0);
    assert(emitted.calls == 1u);
    assert(emitted.last_phase == SNAG_PHASE_COMMENTARY);
    assert(strcmp(emitted.last_provider_id, "msg_pong") == 0);
    assert(emitted.text.len == 4u);
    assert(memcmp(emitted.text.data, "pong", 4u) == 0);
    snag_buf_free(&emitted.text);
    snag_response_graph_free(&graph);
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
    struct snag_response_graph graph;
    struct emitted emitted;
    char error[256] = {0};

    snag_response_graph_init(&graph);
    memset(&emitted, 0, sizeof(emitted));
    snag_buf_init(&emitted.text, 1024u);
    if (parse_stream(wire, 23u, &graph, &emitted,
                     error, sizeof(error)) != 0) {
        fprintf(stderr, "phase-absent text+tool parse: %s\n", error);
        assert(0);
    }
    assert(graph.count == 2u);
    assert(graph.items[0].kind == SNAG_ITEM_ASSISTANT);
    assert(graph.items[0].phase == SNAG_PHASE_COMMENTARY);
    assert(strcmp(graph.items[0].text, "Checking.") == 0);
    assert(graph.items[1].kind == SNAG_ITEM_TOOL_CALL);
    assert(strcmp(graph.items[1].provider_call_id, "call_2") == 0);
    snag_buf_free(&emitted.text);
    snag_response_graph_free(&graph);
}

static void
test_empty_reasoning_item_is_internal_only(void)
{
    static const char wire[] =
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
        "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_reasoning\",\"status\":\"completed\",\"output\":[]}}\n\n";
    struct snag_response_graph graph;
    struct emitted emitted;
    char error[256] = {0};

    snag_response_graph_init(&graph);
    memset(&emitted, 0, sizeof(emitted));
    snag_buf_init(&emitted.text, 1024u);
    assert(parse_stream(wire, 19u, &graph, &emitted,
                        error, sizeof(error)) == 0);
    assert(graph.count == 1u);
    assert(graph.items[0].kind == SNAG_ITEM_ASSISTANT);
    assert(graph.items[0].phase == SNAG_PHASE_FINAL_ANSWER);
    assert(strcmp(graph.items[0].provider_item_id,
                  "msg_after_reasoning") == 0);
    assert(strcmp(graph.items[0].text, "ok") == 0);
    assert(emitted.calls == 1u);
    assert(emitted.last_index == 1u);
    assert(strcmp(emitted.last_provider_id, "msg_after_reasoning") == 0);
    assert(emitted.text.len == 2u);
    assert(memcmp(emitted.text.data, "ok", 2u) == 0);
    snag_buf_free(&emitted.text);
    snag_response_graph_free(&graph);
}

static void
test_web_search_item_is_internal_only(void)
{
    static const char wire[] =
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
        "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_web\",\"status\":\"completed\",\"output\":[]}}\n\n";
    struct snag_response_graph graph;
    struct emitted emitted;
    char error[256] = {0};

    snag_response_graph_init(&graph);
    memset(&emitted, 0, sizeof(emitted));
    snag_buf_init(&emitted.text, 1024u);
    assert(parse_stream(wire, 23u, &graph, &emitted,
                        error, sizeof(error)) == 0);
    assert(graph.count == 1u);
    assert(graph.items[0].kind == SNAG_ITEM_ASSISTANT);
    assert(graph.items[0].phase == SNAG_PHASE_FINAL_ANSWER);
    assert(strcmp(graph.items[0].provider_item_id, "msg_after_web") == 0);
    assert(strcmp(graph.items[0].text, "done") == 0);
    assert(emitted.calls == 1u);
    assert(emitted.last_index == 1u);
    assert(strcmp(emitted.last_provider_id, "msg_after_web") == 0);
    assert(emitted.text.len == 4u);
    assert(memcmp(emitted.text.data, "done", 4u) == 0);
    snag_buf_free(&emitted.text);
    snag_response_graph_free(&graph);
}

static void
test_future_items_and_content_are_inert(void)
{
    static const char wire[] =
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
        "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_inert\",\"status\":\"completed\",\"output\":[]}}\n\n";
    struct snag_response_graph graph;
    struct emitted emitted;
    char error[256] = {0};

    snag_response_graph_init(&graph);
    memset(&emitted, 0, sizeof(emitted));
    snag_buf_init(&emitted.text, 1024u);
    assert(parse_stream(wire, 17u, &graph, &emitted,
                        error, sizeof(error)) == 0);
    assert(graph.count == 1u);
    assert(graph.items[0].kind == SNAG_ITEM_ASSISTANT);
    assert(strcmp(graph.items[0].text, "visible") == 0);
    assert(emitted.calls == 1u);
    assert(emitted.last_index == 1u);
    snag_buf_free(&emitted.text);
    snag_response_graph_free(&graph);
}

static void
test_inert_only_response_has_empty_graph(void)
{
    static const char wire[] =
        "data: {\"type\":\"response.created\",\"response\":{\"id\":\"resp_empty\",\"status\":\"in_progress\",\"output\":[]}}\n\n"
        "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_empty\",\"status\":\"completed\",\"output\":[{\"type\":\"future_action\",\"name\":\"exec_command\",\"arguments\":{}},{\"id\":\"msg_empty\",\"type\":\"message\",\"status\":\"completed\",\"role\":\"assistant\",\"content\":[{\"type\":\"future_content\",\"text\":\"hidden\"}]}]}}\n\n";
    struct snag_response_graph graph;
    struct emitted emitted;
    char error[256] = {0};

    snag_response_graph_init(&graph);
    memset(&emitted, 0, sizeof(emitted));
    snag_buf_init(&emitted.text, 1024u);
    assert(parse_stream(wire, 19u, &graph, &emitted,
                        error, sizeof(error)) == 0);
    assert(graph.count == 0u);
    assert(emitted.calls == 0u);
    snag_buf_free(&emitted.text);
    snag_response_graph_free(&graph);
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
    struct snag_response_graph graph;
    struct emitted emitted;
    char error[256] = {0};

    snag_response_graph_init(&graph);
    memset(&emitted, 0, sizeof(emitted));
    snag_buf_init(&emitted.text, 1024u);
    assert(parse_stream(wire, 31u, &graph, &emitted,
                        error, sizeof(error)) == 0);
    assert(graph.count == 1u);
    assert(graph.items[0].kind == SNAG_ITEM_TOOL_CALL);
    assert(strcmp(graph.items[0].provider_call_id, "call_1") == 0);
    assert(strcmp(graph.items[0].name, "exec_command") == 0);
    assert(strcmp(snag_json_string(graph.items[0].arguments, "command"),
                  "printf hi") == 0);
    assert(emitted.calls == 0u);
    snag_buf_free(&emitted.text);
    snag_response_graph_free(&graph);
}

static void
test_refusal(void)
{
    static const char wire[] =
        "data: {\"type\":\"response.created\",\"response\":{\"id\":\"resp_refuse\",\"status\":\"in_progress\",\"output\":[]}}\n\n"
        "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_refuse\",\"status\":\"completed\",\"output\":[{\"id\":\"msg_refuse\",\"type\":\"message\",\"status\":\"completed\",\"role\":\"assistant\",\"phase\":\"final_answer\",\"content\":[{\"type\":\"refusal\",\"refusal\":\"I cannot do that.\"}]}]}}\n\n";
    struct snag_response_graph graph;
    struct emitted emitted;
    char error[256] = {0};

    snag_response_graph_init(&graph);
    memset(&emitted, 0, sizeof(emitted));
    snag_buf_init(&emitted.text, 1024u);
    assert(parse_stream(wire, 0u, &graph, &emitted,
                        error, sizeof(error)) == 0);
    assert(graph.count == 1u);
    assert(graph.items[0].kind == SNAG_ITEM_REFUSAL);
    assert(strcmp(graph.items[0].text, "I cannot do that.") == 0);
    assert(emitted.last_kind == SNAG_ITEM_REFUSAL);
    snag_buf_free(&emitted.text);
    snag_response_graph_free(&graph);
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
        struct snag_response_graph graph;
        struct emitted emitted;
        char error[256] = {0};

        snag_response_graph_init(&graph);
        memset(&emitted, 0, sizeof(emitted));
        snag_buf_init(&emitted.text, 1024u);
        assert(parse_stream(bad[i], 7u, &graph, &emitted,
                            error, sizeof(error)) < 0);
        assert(error[0]);
        assert(graph.count == 0u);
        snag_buf_free(&emitted.text);
        snag_response_graph_free(&graph);
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

int
main(void)
{
    test_deltas_survive_empty_terminal_output();
    test_terminal_snapshot_can_supply_unseen_items();
    test_empty_public_items_get_specific_correction();
    test_oversized_public_items_get_specific_correction();
    test_structured_keepalives_do_not_end_response();
    test_unused_response_events_are_ignored();
    test_terminal_snapshot_ignores_unused_text_metadata();
    test_unused_annotation_shapes_are_ignored();
    test_phase_absent_text_becomes_visible_final();
    test_phase_absent_text_before_tool_stays_commentary();
    test_empty_reasoning_item_is_internal_only();
    test_web_search_item_is_internal_only();
    test_future_items_and_content_are_inert();
    test_inert_only_response_has_empty_graph();
    test_function_call_arguments();
    test_refusal();
    test_protocol_conflicts_fail_closed();
    test_structured_capacity_failure();
    puts("test_responses: ok");
    return 0;
}

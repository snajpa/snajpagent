/* SPDX-License-Identifier: GPL-2.0-only */
#include "responses.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

struct emitted {
    struct snj_buf text;
    size_t calls;
    size_t last_index;
    enum snj_item_kind last_kind;
    enum snj_item_phase last_phase;
    char last_provider_id[64];
};

static int
capture_emit(void *opaque, size_t output_index, enum snj_item_kind kind,
             enum snj_item_phase phase, const char *provider_item_id,
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
    return snj_buf_append(&emitted->text, text, len);
}

static int
parse_stream(const char *wire, size_t chunk, struct snj_response_graph *graph,
             struct emitted *emitted, char *error, size_t error_size)
{
    struct snj_responses_stream responses;
    struct snj_sse_parser sse;
    size_t len = strlen(wire);
    int rc = 0;

    snj_responses_stream_init(&responses, capture_emit, emitted);
    snj_sse_init(&sse, snj_responses_sse_record, &responses);
    for (size_t offset = 0; offset < len && rc == 0;) {
        size_t take = chunk && chunk < len - offset ? chunk : len - offset;
        rc = snj_sse_feed(&sse, wire + offset, take, error, error_size);
        offset += take;
    }
    if (rc == 0)
        rc = snj_sse_finish(&sse, error, error_size);
    if (rc == 0)
        rc = snj_responses_stream_finish(&responses, graph,
                                          error, error_size);
    else if (responses.failed)
        (void)snprintf(error, error_size, "%s",
                       snj_responses_stream_error(&responses));
    snj_sse_free(&sse);
    snj_responses_stream_free(&responses);
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
        "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_ping\",\"status\":\"completed\",\"usage\":{\"input_tokens\":12,\"output_tokens\":4,\"total_tokens\":16,\"output_tokens_details\":{\"reasoning_tokens\":2}},\"output\":[]}}\n\n";
    struct snj_response_graph graph;
    struct emitted emitted;
    char error[256] = {0};

    snj_response_graph_init(&graph);
    memset(&emitted, 0, sizeof(emitted));
    snj_buf_init(&emitted.text, 1024u);
    assert(parse_stream(wire, 1u, &graph, &emitted,
                        error, sizeof(error)) == 0);
    assert(graph.count == 1u);
    assert(strcmp(graph.provider_response_id, "resp_ping") == 0);
    assert(graph.items[0].kind == SNJ_ITEM_ASSISTANT);
    assert(graph.items[0].phase == SNJ_PHASE_FINAL_ANSWER);
    assert(strcmp(graph.items[0].text, "haha") == 0);
    assert(emitted.calls == 2u);
    assert(emitted.text.len == 4u);
    assert(memcmp(emitted.text.data, "haha", 4u) == 0);
    assert(graph.usage.input_known && graph.usage.input_tokens == 12u);
    assert(graph.usage.output_known && graph.usage.output_tokens == 4u);
    assert(graph.usage.reasoning_known && graph.usage.reasoning_tokens == 2u);
    assert(graph.usage.total_known && graph.usage.total_tokens == 16u);
    snj_buf_free(&emitted.text);
    snj_response_graph_free(&graph);
}

static void
test_terminal_snapshot_can_supply_unseen_items(void)
{
    static const char wire[] =
        "data: {\"type\":\"response.created\",\"response\":{\"id\":\"resp_snapshot\",\"status\":\"in_progress\",\"output\":[]}}\n\n"
        "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_snapshot\",\"status\":\"completed\",\"output\":[{\"id\":\"msg_comment\",\"type\":\"message\",\"status\":\"completed\",\"role\":\"assistant\",\"phase\":\"commentary\",\"content\":[{\"type\":\"output_text\",\"text\":\"Working. \",\"annotations\":[]},{\"type\":\"output_text\",\"text\":\"Done.\",\"annotations\":[]}]},{\"id\":\"msg_final\",\"type\":\"message\",\"status\":\"completed\",\"role\":\"assistant\",\"phase\":\"final_answer\",\"content\":[{\"type\":\"output_text\",\"text\":\"answer\",\"annotations\":[]}]}]}}\n\n";
    struct snj_response_graph graph;
    struct emitted emitted;
    char error[256] = {0};

    snj_response_graph_init(&graph);
    memset(&emitted, 0, sizeof(emitted));
    snj_buf_init(&emitted.text, 1024u);
    if (parse_stream(wire, 17u, &graph, &emitted,
                     error, sizeof(error)) != 0) {
        fprintf(stderr, "snapshot parse: %s\n", error);
        assert(0);
    }
    assert(graph.count == 2u);
    assert(graph.items[0].phase == SNJ_PHASE_COMMENTARY);
    assert(strcmp(graph.items[0].text, "Working. Done.") == 0);
    assert(graph.items[1].phase == SNJ_PHASE_FINAL_ANSWER);
    assert(strcmp(graph.items[1].text, "answer") == 0);
    assert(emitted.last_index == 1u);
    assert(emitted.last_phase == SNJ_PHASE_FINAL_ANSWER);
    snj_buf_free(&emitted.text);
    snj_response_graph_free(&graph);
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
    struct snj_response_graph graph;
    struct emitted emitted;
    char error[256] = {0};

    snj_response_graph_init(&graph);
    memset(&emitted, 0, sizeof(emitted));
    snj_buf_init(&emitted.text, 1024u);
    if (parse_stream(wire, 9u, &graph, &emitted,
                     error, sizeof(error)) != 0) {
        fprintf(stderr, "phase-absent text parse: %s\n", error);
        assert(0);
    }
    assert(graph.count == 1u);
    assert(graph.items[0].kind == SNJ_ITEM_ASSISTANT);
    assert(graph.items[0].phase == SNJ_PHASE_FINAL_ANSWER);
    assert(strcmp(graph.items[0].provider_item_id, "msg_pong") == 0);
    assert(strcmp(graph.items[0].text, "pong") == 0);
    assert(emitted.calls == 1u);
    assert(emitted.last_phase == SNJ_PHASE_COMMENTARY);
    assert(strcmp(emitted.last_provider_id, "msg_pong") == 0);
    assert(emitted.text.len == 4u);
    assert(memcmp(emitted.text.data, "pong", 4u) == 0);
    snj_buf_free(&emitted.text);
    snj_response_graph_free(&graph);
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
    struct snj_response_graph graph;
    struct emitted emitted;
    char error[256] = {0};

    snj_response_graph_init(&graph);
    memset(&emitted, 0, sizeof(emitted));
    snj_buf_init(&emitted.text, 1024u);
    if (parse_stream(wire, 23u, &graph, &emitted,
                     error, sizeof(error)) != 0) {
        fprintf(stderr, "phase-absent text+tool parse: %s\n", error);
        assert(0);
    }
    assert(graph.count == 2u);
    assert(graph.items[0].kind == SNJ_ITEM_ASSISTANT);
    assert(graph.items[0].phase == SNJ_PHASE_COMMENTARY);
    assert(strcmp(graph.items[0].text, "Checking.") == 0);
    assert(graph.items[1].kind == SNJ_ITEM_TOOL_CALL);
    assert(strcmp(graph.items[1].provider_call_id, "call_2") == 0);
    snj_buf_free(&emitted.text);
    snj_response_graph_free(&graph);
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
    struct snj_response_graph graph;
    struct emitted emitted;
    char error[256] = {0};

    snj_response_graph_init(&graph);
    memset(&emitted, 0, sizeof(emitted));
    snj_buf_init(&emitted.text, 1024u);
    assert(parse_stream(wire, 19u, &graph, &emitted,
                        error, sizeof(error)) == 0);
    assert(graph.count == 1u);
    assert(graph.items[0].kind == SNJ_ITEM_ASSISTANT);
    assert(graph.items[0].phase == SNJ_PHASE_FINAL_ANSWER);
    assert(strcmp(graph.items[0].provider_item_id,
                  "msg_after_reasoning") == 0);
    assert(strcmp(graph.items[0].text, "ok") == 0);
    assert(emitted.calls == 1u);
    assert(emitted.last_index == 1u);
    assert(strcmp(emitted.last_provider_id, "msg_after_reasoning") == 0);
    assert(emitted.text.len == 2u);
    assert(memcmp(emitted.text.data, "ok", 2u) == 0);
    snj_buf_free(&emitted.text);
    snj_response_graph_free(&graph);
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
    struct snj_response_graph graph;
    struct emitted emitted;
    char error[256] = {0};

    snj_response_graph_init(&graph);
    memset(&emitted, 0, sizeof(emitted));
    snj_buf_init(&emitted.text, 1024u);
    assert(parse_stream(wire, 23u, &graph, &emitted,
                        error, sizeof(error)) == 0);
    assert(graph.count == 1u);
    assert(graph.items[0].kind == SNJ_ITEM_ASSISTANT);
    assert(graph.items[0].phase == SNJ_PHASE_FINAL_ANSWER);
    assert(strcmp(graph.items[0].provider_item_id, "msg_after_web") == 0);
    assert(strcmp(graph.items[0].text, "done") == 0);
    assert(emitted.calls == 1u);
    assert(emitted.last_index == 1u);
    assert(strcmp(emitted.last_provider_id, "msg_after_web") == 0);
    assert(emitted.text.len == 4u);
    assert(memcmp(emitted.text.data, "done", 4u) == 0);
    snj_buf_free(&emitted.text);
    snj_response_graph_free(&graph);
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
    struct snj_response_graph graph;
    struct emitted emitted;
    char error[256] = {0};

    snj_response_graph_init(&graph);
    memset(&emitted, 0, sizeof(emitted));
    snj_buf_init(&emitted.text, 1024u);
    assert(parse_stream(wire, 31u, &graph, &emitted,
                        error, sizeof(error)) == 0);
    assert(graph.count == 1u);
    assert(graph.items[0].kind == SNJ_ITEM_TOOL_CALL);
    assert(strcmp(graph.items[0].provider_call_id, "call_1") == 0);
    assert(strcmp(graph.items[0].name, "exec_command") == 0);
    assert(strcmp(snj_json_string(graph.items[0].arguments, "command"),
                  "printf hi") == 0);
    assert(emitted.calls == 0u);
    snj_buf_free(&emitted.text);
    snj_response_graph_free(&graph);
}

static void
test_refusal(void)
{
    static const char wire[] =
        "data: {\"type\":\"response.created\",\"response\":{\"id\":\"resp_refuse\",\"status\":\"in_progress\",\"output\":[]}}\n\n"
        "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_refuse\",\"status\":\"completed\",\"output\":[{\"id\":\"msg_refuse\",\"type\":\"message\",\"status\":\"completed\",\"role\":\"assistant\",\"phase\":\"final_answer\",\"content\":[{\"type\":\"refusal\",\"refusal\":\"I cannot do that.\"}]}]}}\n\n";
    struct snj_response_graph graph;
    struct emitted emitted;
    char error[256] = {0};

    snj_response_graph_init(&graph);
    memset(&emitted, 0, sizeof(emitted));
    snj_buf_init(&emitted.text, 1024u);
    assert(parse_stream(wire, 0u, &graph, &emitted,
                        error, sizeof(error)) == 0);
    assert(graph.count == 1u);
    assert(graph.items[0].kind == SNJ_ITEM_REFUSAL);
    assert(strcmp(graph.items[0].text, "I cannot do that.") == 0);
    assert(emitted.last_kind == SNJ_ITEM_REFUSAL);
    snj_buf_free(&emitted.text);
    snj_response_graph_free(&graph);
}

static void
test_protocol_conflicts_fail_closed(void)
{
    static const char *const bad[] = {
        "event: wrong\ndata: {\"type\":\"response.created\",\"response\":{\"id\":\"r\",\"status\":\"in_progress\",\"output\":[]}}\n\n",
        "data: {\"type\":\"response.created\",\"response\":{\"id\":\"r\",\"status\":\"in_progress\",\"output\":[]}}\n\ndata: {\"type\":\"response.output_item.added\",\"output_index\":0,\"item\":{\"id\":\"m\",\"type\":\"message\",\"status\":\"in_progress\",\"role\":\"assistant\",\"phase\":\"final_answer\",\"content\":[]}}\n\ndata: {\"type\":\"response.content_part.added\",\"item_id\":\"m\",\"output_index\":0,\"content_index\":0,\"part\":{\"type\":\"output_text\",\"text\":\"\",\"annotations\":[]}}\n\ndata: {\"type\":\"response.output_text.delta\",\"item_id\":\"m\",\"output_index\":0,\"content_index\":0,\"delta\":\"a\"}\n\ndata: {\"type\":\"response.output_text.done\",\"item_id\":\"m\",\"output_index\":0,\"content_index\":0,\"text\":\"b\"}\n\n",
        "data: {\"type\":\"response.created\",\"response\":{\"id\":\"r\",\"status\":\"in_progress\",\"output\":[]}}\n\ndata: {\"type\":\"response.completed\",\"response\":{\"id\":\"r\",\"status\":\"completed\",\"output\":[{\"id\":\"m\",\"type\":\"message\",\"status\":\"completed\",\"role\":\"assistant\",\"phase\":\"final_answer\",\"content\":[{\"type\":\"output_text\",\"text\":\"x\",\"annotations\":[]},{\"type\":\"refusal\",\"refusal\":\"no\"}]}]}}\n\n",
        "data: {\"type\":\"response.created\",\"response\":{\"id\":\"r\",\"status\":\"in_progress\",\"output\":[]}}\n\ndata: {\"type\":\"response.completed\",\"response\":{\"id\":\"r\",\"status\":\"completed\",\"usage\":{\"input_tokens\":4,\"output_tokens\":3,\"total_tokens\":99},\"output\":[]}}\n\n",
        "data: {\"type\":\"response.created\",\"response\":{\"id\":\"r\",\"status\":\"in_progress\",\"output\":[]}}\n\n"
    };

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        struct snj_response_graph graph;
        struct emitted emitted;
        char error[256] = {0};

        snj_response_graph_init(&graph);
        memset(&emitted, 0, sizeof(emitted));
        snj_buf_init(&emitted.text, 1024u);
        assert(parse_stream(bad[i], 7u, &graph, &emitted,
                            error, sizeof(error)) < 0);
        assert(error[0]);
        assert(graph.count == 0u);
        snj_buf_free(&emitted.text);
        snj_response_graph_free(&graph);
    }
}

int
main(void)
{
    test_deltas_survive_empty_terminal_output();
    test_terminal_snapshot_can_supply_unseen_items();
    test_phase_absent_text_becomes_visible_final();
    test_phase_absent_text_before_tool_stays_commentary();
    test_empty_reasoning_item_is_internal_only();
    test_web_search_item_is_internal_only();
    test_function_call_arguments();
    test_refusal();
    test_protocol_conflicts_fail_closed();
    puts("test_responses: ok");
    return 0;
}

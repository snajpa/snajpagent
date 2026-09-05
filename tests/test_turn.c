/* SPDX-License-Identifier: GPL-2.0-only */
#include "turn.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static json_t *
args(void)
{
    json_t *o = json_object();
    assert(o);
    assert(snag_json_set_new(o, "command", json_string("true")) == 0);
    assert(snag_json_set_new(o, "pty", json_false()) == 0);
    assert(snag_json_set_new(o, "stdin", json_null()) == 0);
    assert(snag_json_set_new(o, "timeout_ms", json_integer(1000)) == 0);
    assert(snag_json_set_new(o, "workdir", json_string("/tmp")) == 0);
    assert(snag_json_set_new(o, "yield_ms", json_integer(1000)) == 0);
    return o;
}

int
main(void)
{
    struct snag_response_graph graph;
    struct snag_response_graph copy;
    struct snag_graph_decision decision;
    json_t *encoded;
    json_t *result;
    char error[256];
    char action_a[SNAG_SHA256_HEX_LEN + 1u];
    char action_b[SNAG_SHA256_HEX_LEN + 1u];
    char *large;

    snag_response_graph_init(&graph);
    assert(snag_response_graph_add_call(&graph, "item_web", "call_web",
                                       "web_search", json_object()) < 0);
    assert(snag_response_graph_add_call(&graph, "item_web", "call_web",
                                       "openrouter:web_search", json_object()) < 0);
    assert(graph.count == 0u);
    assert(snag_response_graph_set_provider_id(&graph, "bad\nresponse") < 0);
    assert(snag_response_graph_set_provider_id(&graph, "resp_final") == 0);
    assert(snag_response_graph_add_public(&graph, SNAG_ITEM_ASSISTANT,
                                         SNAG_PHASE_FINAL_ANSWER,
                                         "msg_final", "pong") == 0);
    assert(snag_response_graph_classify(&graph, &decision,
                                       error, sizeof(error)) == 0);
    assert(decision.outcome == SNAG_GRAPH_FINAL);
    assert(decision.final_index == 0u);
    encoded = snag_response_graph_json(&graph);
    assert(encoded);
    snag_response_graph_init(&copy);
    assert(snag_response_graph_set_provider_id(&copy, "resp_final") == 0);
    assert(snag_response_graph_from_json(&copy, encoded,
                                        error, sizeof(error)) == 0);
    assert(snag_response_graph_classify(&copy, &decision,
                                       error, sizeof(error)) == 0);
    assert(decision.outcome == SNAG_GRAPH_FINAL);
    assert(strcmp(copy.items[0].text, "pong") == 0);
    assert(snag_partial_public_validate(encoded,
                                       error, sizeof(error)) == 0);
    assert(json_array_append_new(encoded,
                                 json_deep_copy(json_array_get(encoded, 0u))) == 0);
    snag_response_graph_free(&copy);
    snag_response_graph_init(&copy);
    assert(snag_response_graph_from_json(&copy, encoded,
                                        error, sizeof(error)) < 0);
    json_decref(encoded);
    snag_response_graph_free(&copy);
    snag_response_graph_free(&graph);

    snag_response_graph_init(&graph);
    assert(snag_response_graph_set_provider_id(&graph, "resp_calls") == 0);
    assert(snag_response_graph_add_public(&graph, SNAG_ITEM_ASSISTANT,
                                         SNAG_PHASE_COMMENTARY,
                                         "msg_commentary", "checking") == 0);
    assert(snag_response_graph_add_call(&graph, "item_call", "provider_call",
                                       "exec_command", args()) == 0);
    {
        json_t *payload = json_object();
        assert(payload);
        assert(snag_json_set_new(payload, "encrypted_content",
                                json_string("opaque")) == 0);
        assert(snag_response_graph_add_opaque(&graph, "item_opaque",
                                             "reasoning", payload) == 0);
    }
    assert(snag_response_graph_classify(&graph, &decision,
                                       error, sizeof(error)) == 0);
    assert(decision.outcome == SNAG_GRAPH_CALLS);
    assert(decision.call_count == 1u);
    encoded = snag_response_graph_json(&graph);
    assert(encoded);
    assert(snag_partial_public_validate(encoded,
                                       error, sizeof(error)) < 0);
    snag_response_graph_init(&copy);
    assert(snag_response_graph_set_provider_id(&copy, "resp_calls") == 0);
    assert(snag_response_graph_from_json(&copy, encoded,
                                        error, sizeof(error)) == 0);
    assert(copy.items[1].local_item_id[0] == '\0');
    assert(copy.items[2].local_item_id[0] == '\0');
    assert(snag_response_graph_classify(&copy, &decision,
                                       error, sizeof(error)) == 0);
    assert(decision.outcome == SNAG_GRAPH_CALLS);
    {
        json_t *roundtrip = snag_response_graph_json(&copy);
        struct snag_buf a;
        struct snag_buf b;
        assert(roundtrip);
        snag_buf_init(&a, SNAG_MAX_RESPONSE_GRAPH);
        snag_buf_init(&b, SNAG_MAX_RESPONSE_GRAPH);
        assert(snag_json_canonical(encoded, &a) == 0);
        assert(snag_json_canonical(roundtrip, &b) == 0);
        assert(a.len == b.len && memcmp(a.data, b.data, a.len) == 0);
        snag_buf_free(&a);
        snag_buf_free(&b);
        json_decref(roundtrip);
    }
    json_decref(encoded);
    snag_response_graph_free(&copy);
    assert(snag_tool_action_digest(&graph.items[1], "/tmp", action_a) == 0);
    assert(snag_tool_action_digest(&graph.items[1], "/var/tmp", action_b) == 0);
    assert(strcmp(action_a, action_b) != 0);
    assert(snag_response_graph_add_public(&graph, SNAG_ITEM_ASSISTANT,
                                         SNAG_PHASE_FINAL_ANSWER,
                                         "msg_conflict", "done") == 0);
    assert(snag_response_graph_classify(&graph, &decision,
                                       error, sizeof(error)) == 0);
    assert(decision.outcome == SNAG_GRAPH_CONFLICT);
    snag_response_graph_free(&graph);

    snag_response_graph_init(&graph);
    assert(snag_response_graph_set_provider_id(&graph, "resp_irc") == 0);
    {
        static const char *const names[] = {
            "irc_send", "irc_state", "irc_topic"
        };

        for (size_t i = 0u; i < sizeof(names) / sizeof(names[0]); ++i) {
            json_t *arguments = json_object();
            char item[32];
            char call[32];

            assert(arguments);
            assert(snprintf(item, sizeof(item), "item_irc_%zu", i) > 0);
            assert(snprintf(call, sizeof(call), "call_irc_%zu", i) > 0);
            assert(snag_response_graph_add_call(&graph, item, call, names[i],
                                                arguments) == 0);
        }
    }
    assert(snag_response_graph_classify(&graph, &decision,
                                       error, sizeof(error)) == 0);
    assert(decision.outcome == SNAG_GRAPH_CALLS && decision.call_count == 3u);
    encoded = snag_response_graph_json(&graph);
    assert(encoded);
    snag_response_graph_init(&copy);
    assert(snag_response_graph_set_provider_id(&copy, "resp_irc") == 0);
    assert(snag_response_graph_from_json(&copy, encoded,
                                        error, sizeof(error)) == 0);
    assert(strcmp(copy.items[0].name, "irc_send") == 0);
    assert(strcmp(copy.items[1].name, "irc_state") == 0);
    assert(strcmp(copy.items[2].name, "irc_topic") == 0);
    json_decref(encoded);
    snag_response_graph_free(&copy);
    snag_response_graph_free(&graph);

    snag_response_graph_init(&graph);
    assert(snag_response_graph_set_provider_id(&graph, "resp_empty") == 0);
    assert(snag_response_graph_classify(&graph, &decision,
                                       error, sizeof(error)) == 0);
    assert(decision.outcome == SNAG_GRAPH_NONPRODUCTIVE);
    snag_response_graph_free(&graph);

    large = malloc(SNAG_MAX_PUBLIC_ITEM + 1u);
    assert(large);
    memset(large, 'x', SNAG_MAX_PUBLIC_ITEM);
    large[SNAG_MAX_PUBLIC_ITEM] = '\0';
    snag_response_graph_init(&graph);
    assert(snag_response_graph_set_provider_id(&graph, "resp_large") == 0);
    assert(snag_response_graph_add_public(&graph, SNAG_ITEM_ASSISTANT,
                                         SNAG_PHASE_COMMENTARY,
                                         "msg_large_1", large) == 0);
    assert(snag_response_graph_add_public(&graph, SNAG_ITEM_ASSISTANT,
                                         SNAG_PHASE_COMMENTARY,
                                         "msg_large_2", large) == 0);
    assert(snag_response_graph_add_public(&graph, SNAG_ITEM_ASSISTANT,
                                         SNAG_PHASE_COMMENTARY,
                                         "msg_large_3", large) == 0);
    assert(snag_response_graph_add_public(&graph, SNAG_ITEM_ASSISTANT,
                                         SNAG_PHASE_COMMENTARY,
                                         "msg_large_4", large) < 0);
    assert(graph.count == 3u);
    snag_response_graph_free(&graph);
    free(large);

    result = snag_tool_result_not_run("protocol_conflict");
    assert(result && snag_tool_result_valid(result) == 0);
    json_decref(result);
    result = snag_tool_result_outcome_unknown("owner_lost");
    assert(result && snag_tool_result_valid(result) == 0);
    json_decref(result);
    result = snag_tool_result_terminal(true, "bounded");
    assert(result);
    assert(snag_json_set_new(result, "max_output_tokens",
                            json_integer(1)) == 0);
    assert(snag_tool_result_valid(result) == 0);
    assert(json_object_set_new(result, "max_output_tokens",
                               json_integer(0)) == 0);
    assert(snag_tool_result_valid(result) < 0);
    assert(json_object_set_new(result, "max_output_tokens",
                               json_integer(4000000001LL)) == 0);
    assert(snag_tool_result_valid(result) < 0);
    json_decref(result);

    {
        struct snag_response_usage usage = {
            .input_tokens = 10u, .output_tokens = 4u,
            .reasoning_tokens = 3u, .total_tokens = 14u,
            .input_known = true, .output_known = true,
            .reasoning_known = true, .total_known = true
        };
        struct snag_response_usage parsed;
        json_t *usage_json = snag_response_usage_json(&usage);
        assert(usage_json);
        assert(snag_response_usage_from_json(usage_json, &parsed) == 0);
        assert(parsed.input_tokens == 10u && parsed.total_tokens == 14u);
        json_decref(usage_json);
        usage.total_tokens = 99u;
        assert(snag_response_usage_valid(&usage) < 0);
    }

    puts("test_turn: ok");
    return 0;
}

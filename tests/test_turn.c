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
    assert(snj_json_set_new(o, "command", json_string("true")) == 0);
    assert(snj_json_set_new(o, "pty", json_false()) == 0);
    assert(snj_json_set_new(o, "stdin", json_null()) == 0);
    assert(snj_json_set_new(o, "timeout_ms", json_integer(1000)) == 0);
    assert(snj_json_set_new(o, "workdir", json_string("/tmp")) == 0);
    assert(snj_json_set_new(o, "yield_ms", json_integer(1000)) == 0);
    return o;
}

int
main(void)
{
    struct snj_response_graph graph;
    struct snj_response_graph copy;
    struct snj_graph_decision decision;
    json_t *encoded;
    json_t *result;
    char error[256];
    char action_a[SNJ_SHA256_HEX_LEN + 1u];
    char action_b[SNJ_SHA256_HEX_LEN + 1u];
    char *large;

    snj_response_graph_init(&graph);
    assert(snj_response_graph_set_provider_id(&graph, "bad\nresponse") < 0);
    assert(snj_response_graph_set_provider_id(&graph, "resp_final") == 0);
    assert(snj_response_graph_add_public(&graph, SNJ_ITEM_ASSISTANT,
                                         SNJ_PHASE_FINAL_ANSWER,
                                         "msg_final", "pong") == 0);
    assert(snj_response_graph_classify(&graph, &decision,
                                       error, sizeof(error)) == 0);
    assert(decision.outcome == SNJ_GRAPH_FINAL);
    assert(decision.final_index == 0u);
    encoded = snj_response_graph_json(&graph);
    assert(encoded);
    snj_response_graph_init(&copy);
    assert(snj_response_graph_set_provider_id(&copy, "resp_final") == 0);
    assert(snj_response_graph_from_json(&copy, encoded,
                                        error, sizeof(error)) == 0);
    assert(snj_response_graph_classify(&copy, &decision,
                                       error, sizeof(error)) == 0);
    assert(decision.outcome == SNJ_GRAPH_FINAL);
    assert(strcmp(copy.items[0].text, "pong") == 0);
    assert(snj_partial_public_validate(encoded,
                                       error, sizeof(error)) == 0);
    assert(json_array_append_new(encoded,
                                 json_deep_copy(json_array_get(encoded, 0u))) == 0);
    snj_response_graph_free(&copy);
    snj_response_graph_init(&copy);
    assert(snj_response_graph_from_json(&copy, encoded,
                                        error, sizeof(error)) < 0);
    json_decref(encoded);
    snj_response_graph_free(&copy);
    snj_response_graph_free(&graph);

    snj_response_graph_init(&graph);
    assert(snj_response_graph_set_provider_id(&graph, "resp_calls") == 0);
    assert(snj_response_graph_add_public(&graph, SNJ_ITEM_ASSISTANT,
                                         SNJ_PHASE_COMMENTARY,
                                         "msg_commentary", "checking") == 0);
    assert(snj_response_graph_add_call(&graph, "item_call", "provider_call",
                                       "exec_command", args()) == 0);
    {
        json_t *payload = json_object();
        assert(payload);
        assert(snj_json_set_new(payload, "encrypted_content",
                                json_string("opaque")) == 0);
        assert(snj_response_graph_add_opaque(&graph, "item_opaque",
                                             "reasoning", payload) == 0);
    }
    assert(snj_response_graph_classify(&graph, &decision,
                                       error, sizeof(error)) == 0);
    assert(decision.outcome == SNJ_GRAPH_CALLS);
    assert(decision.call_count == 1u);
    encoded = snj_response_graph_json(&graph);
    assert(encoded);
    assert(snj_partial_public_validate(encoded,
                                       error, sizeof(error)) < 0);
    snj_response_graph_init(&copy);
    assert(snj_response_graph_set_provider_id(&copy, "resp_calls") == 0);
    assert(snj_response_graph_from_json(&copy, encoded,
                                        error, sizeof(error)) == 0);
    assert(copy.items[1].local_item_id[0] == '\0');
    assert(copy.items[2].local_item_id[0] == '\0');
    assert(snj_response_graph_classify(&copy, &decision,
                                       error, sizeof(error)) == 0);
    assert(decision.outcome == SNJ_GRAPH_CALLS);
    {
        json_t *roundtrip = snj_response_graph_json(&copy);
        struct snj_buf a;
        struct snj_buf b;
        assert(roundtrip);
        snj_buf_init(&a, SNJ_MAX_RESPONSE_GRAPH);
        snj_buf_init(&b, SNJ_MAX_RESPONSE_GRAPH);
        assert(snj_json_canonical(encoded, &a) == 0);
        assert(snj_json_canonical(roundtrip, &b) == 0);
        assert(a.len == b.len && memcmp(a.data, b.data, a.len) == 0);
        snj_buf_free(&a);
        snj_buf_free(&b);
        json_decref(roundtrip);
    }
    json_decref(encoded);
    snj_response_graph_free(&copy);
    assert(snj_tool_action_digest(&graph.items[1], "/tmp", action_a) == 0);
    assert(snj_tool_action_digest(&graph.items[1], "/var/tmp", action_b) == 0);
    assert(strcmp(action_a, action_b) != 0);
    assert(snj_response_graph_add_public(&graph, SNJ_ITEM_ASSISTANT,
                                         SNJ_PHASE_FINAL_ANSWER,
                                         "msg_conflict", "done") == 0);
    assert(snj_response_graph_classify(&graph, &decision,
                                       error, sizeof(error)) == 0);
    assert(decision.outcome == SNJ_GRAPH_CONFLICT);
    snj_response_graph_free(&graph);

    snj_response_graph_init(&graph);
    assert(snj_response_graph_set_provider_id(&graph, "resp_empty") == 0);
    assert(snj_response_graph_classify(&graph, &decision,
                                       error, sizeof(error)) == 0);
    assert(decision.outcome == SNJ_GRAPH_NONPRODUCTIVE);
    snj_response_graph_free(&graph);

    large = malloc(SNJ_MAX_PUBLIC_ITEM + 1u);
    assert(large);
    memset(large, 'x', SNJ_MAX_PUBLIC_ITEM);
    large[SNJ_MAX_PUBLIC_ITEM] = '\0';
    snj_response_graph_init(&graph);
    assert(snj_response_graph_set_provider_id(&graph, "resp_large") == 0);
    assert(snj_response_graph_add_public(&graph, SNJ_ITEM_ASSISTANT,
                                         SNJ_PHASE_COMMENTARY,
                                         "msg_large_1", large) == 0);
    assert(snj_response_graph_add_public(&graph, SNJ_ITEM_ASSISTANT,
                                         SNJ_PHASE_COMMENTARY,
                                         "msg_large_2", large) == 0);
    assert(snj_response_graph_add_public(&graph, SNJ_ITEM_ASSISTANT,
                                         SNJ_PHASE_COMMENTARY,
                                         "msg_large_3", large) == 0);
    assert(snj_response_graph_add_public(&graph, SNJ_ITEM_ASSISTANT,
                                         SNJ_PHASE_COMMENTARY,
                                         "msg_large_4", large) < 0);
    assert(graph.count == 3u);
    snj_response_graph_free(&graph);
    free(large);

    result = snj_tool_result_not_run("protocol_conflict");
    assert(result && snj_tool_result_valid(result) == 0);
    json_decref(result);
    result = snj_tool_result_outcome_unknown("owner_lost");
    assert(result && snj_tool_result_valid(result) == 0);
    json_decref(result);

    {
        struct snj_response_usage usage = {
            .input_tokens = 10u, .output_tokens = 4u,
            .reasoning_tokens = 3u, .total_tokens = 14u,
            .input_known = true, .output_known = true,
            .reasoning_known = true, .total_known = true
        };
        struct snj_response_usage parsed;
        json_t *usage_json = snj_response_usage_json(&usage);
        assert(usage_json);
        assert(snj_response_usage_from_json(usage_json, &parsed) == 0);
        assert(parsed.input_tokens == 10u && parsed.total_tokens == 14u);
        json_decref(usage_json);
        usage.total_tokens = 99u;
        assert(snj_response_usage_valid(&usage) < 0);
    }

    puts("test_turn: ok");
    return 0;
}

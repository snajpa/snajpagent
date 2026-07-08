/* SPDX-License-Identifier: GPL-2.0-only */
#include "context.h"
#include "json.h"
#include "snajpagent.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static void
write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    assert(f);
    assert(fwrite(text, 1u, strlen(text), f) == strlen(text));
    assert(fclose(f) == 0);
}

static json_t *
turn_config(void)
{
    json_t *config = json_object();
    assert(config);
    assert(snj_json_set_new(config, "capability_version",
                            json_string(SNAJPAGENT_CAPABILITY_VERSION)) == 0);
    assert(snj_json_set_new(config, "effort", json_string("medium")) == 0);
    assert(snj_json_set_new(config, "max_output_tokens",
                            json_integer(SNAJPAGENT_MAX_OUTPUT_TOKENS)) == 0);
    assert(snj_json_set_new(config, "model", json_string(SNAJPAGENT_MODEL)) == 0);
    assert(snj_json_set_new(config, "profile_id",
                            json_string(SNAJPAGENT_PROFILE_ID)) == 0);
    assert(snj_json_set_new(config, "prompt_schema", json_integer(1)) == 0);
    assert(snj_json_set_new(config, "replay_schema", json_integer(1)) == 0);
    assert(snj_json_set_new(config, "tool_schema", json_integer(1)) == 0);
    return config;
}

static json_t *
turn_started(const char *turn_id, unsigned int number, const char *text,
             const char *workspace, json_t *instructions)
{
    json_t *data = json_object();
    assert(data);
    assert(snj_json_set_new(data, "config", turn_config()) == 0);
    assert(snj_json_set_new(data, "input_kind", json_string("direct")) == 0);
    assert(snj_json_set_new(data, "instructions",
                            instructions ? instructions : json_array()) == 0);
    assert(snj_json_set_new(data, "queue_id", json_null()) == 0);
    assert(snj_json_set_new(data, "queue_seq", json_null()) == 0);
    assert(snj_json_set_new(data, "text", json_string(text)) == 0);
    assert(snj_json_set_new(data, "turn_id", json_string(turn_id)) == 0);
    assert(snj_json_set_new(data, "turn_number", json_integer(number)) == 0);
    assert(snj_json_set_new(data, "workspace", json_string(workspace)) == 0);
    return data;
}

static json_t *
response_started(const char *turn_id, const char *response_id,
                 const char *compact_id)
{
    json_t *data = json_object();
    json_t *steering = json_array();
    assert(data && steering);
    assert(snj_json_set_new(data, "baseline_sha256", json_null()) == 0);
    assert(snj_json_set_new(data, "capability_version",
                            json_string(SNAJPAGENT_CAPABILITY_VERSION)) == 0);
    assert(snj_json_set_new(data, "compact_id",
                            compact_id ? json_string(compact_id) : json_null()) == 0);
    assert(snj_json_set_new(data, "count_method", json_string("exact")) == 0);
    assert(snj_json_set_new(data, "count_request_sha256",
        json_string("cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc")) == 0);
    assert(snj_json_set_new(data, "cycle", json_integer(1)) == 0);
    assert(snj_json_set_new(data, "input_tokens_bound", json_integer(1000)) == 0);
    assert(snj_json_set_new(data, "model", json_string(SNAJPAGENT_MODEL)) == 0);
    assert(snj_json_set_new(data, "model_input_sha256",
        json_string("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")) == 0);
    assert(snj_json_set_new(data, "profile_id",
                            json_string(SNAJPAGENT_PROFILE_ID)) == 0);
    assert(snj_json_set_new(data, "request_sha256",
        json_string("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb")) == 0);
    assert(snj_json_set_new(data, "response_id", json_string(response_id)) == 0);
    assert(snj_json_set_new(data, "steering_ids", steering) == 0);
    assert(snj_json_set_new(data, "turn_id", json_string(turn_id)) == 0);
    return data;
}

static json_t *
usage(void)
{
    json_t *u = json_object();
    assert(u);
    assert(snj_json_set_new(u, "input_tokens", json_integer(10)) == 0);
    assert(snj_json_set_new(u, "output_tokens", json_integer(1)) == 0);
    assert(snj_json_set_new(u, "reasoning_tokens", json_null()) == 0);
    assert(snj_json_set_new(u, "total_tokens", json_integer(11)) == 0);
    return u;
}

static json_t *
assistant_item(const char *text)
{
    json_t *item = json_object();
    assert(item);
    assert(snj_json_set_new(item, "kind", json_string("assistant")) == 0);
    assert(snj_json_set_new(item, "local_item_id",
        json_string("11111111111111111111111111111111")) == 0);
    assert(snj_json_set_new(item, "phase", json_string("final_answer")) == 0);
    assert(snj_json_set_new(item, "provider_item_id", json_string("msg_1")) == 0);
    assert(snj_json_set_new(item, "text", json_string(text)) == 0);
    return item;
}

static json_t *
response_completed(const char *turn_id, const char *response_id,
                   const char *text)
{
    json_t *data = json_object();
    json_t *items = json_array();
    assert(data && items);
    assert(json_array_append_new(items, assistant_item(text)) == 0);
    assert(snj_json_set_new(data, "cycle", json_integer(1)) == 0);
    assert(snj_json_set_new(data, "items", items) == 0);
    assert(snj_json_set_new(data, "provider_response_id", json_string("resp_1")) == 0);
    assert(snj_json_set_new(data, "response_id", json_string(response_id)) == 0);
    assert(snj_json_set_new(data, "status", json_string("completed")) == 0);
    assert(snj_json_set_new(data, "turn_id", json_string(turn_id)) == 0);
    assert(snj_json_set_new(data, "usage", usage()) == 0);
    return data;
}

static json_t *
turn_completed(const char *turn_id, const char *response_id)
{
    json_t *data = json_object();
    assert(data);
    assert(snj_json_set_new(data, "final_item_id",
        json_string("11111111111111111111111111111111")) == 0);
    assert(snj_json_set_new(data, "final_response_id", json_string(response_id)) == 0);
    assert(snj_json_set_new(data, "turn_id", json_string(turn_id)) == 0);
    return data;
}



static json_t *
compact_output_fixture(void)
{
    json_t *output = json_array();
    json_t *item = json_object();
    assert(output && item);
    assert(snj_json_set_new(item, "encrypted_content",
                            json_string("test-native-compact")) == 0);
    assert(snj_json_set_new(item, "type", json_string("compaction")) == 0);
    assert(json_array_append_new(output, item) == 0);
    return output;
}

static json_t *
compaction_started_data(const struct snj_session *session,
                        const char *compact_id, const char *reason,
                        uint64_t source_seq, const char *source_hash,
                        const char *request_hash,
                        uint64_t input_tokens_bound)
{
    json_t *data = json_object();
    assert(data);
    assert(snj_json_set_new(data, "capability_version",
                            json_string(SNAJPAGENT_CAPABILITY_VERSION)) == 0);
    assert(snj_json_set_new(data, "compact_id", json_string(compact_id)) == 0);
    assert(snj_json_set_new(data, "count_method",
                            json_string("qualified_upper_bound")) == 0);
    assert(snj_json_set_new(data, "count_request_sha256",
                            json_string(request_hash)) == 0);
    assert(snj_json_set_new(data, "input_tokens_bound",
                            json_integer((json_int_t)input_tokens_bound)) == 0);
    assert(snj_json_set_new(data, "model",
                            json_string(session->default_model)) == 0);
    assert(snj_json_set_new(data, "predecessor_compact_id",
                            session->compact_id[0] ?
                            json_string(session->compact_id) : json_null()) == 0);
    assert(snj_json_set_new(data, "profile_id",
                            json_string(SNAJPAGENT_PROFILE_ID)) == 0);
    assert(snj_json_set_new(data, "reason",
                            json_string(reason ? reason : "manual")) == 0);
    assert(snj_json_set_new(data, "request_sha256",
                            json_string(request_hash)) == 0);
    assert(snj_json_set_new(data, "source_seq",
                            json_integer((json_int_t)source_seq)) == 0);
    assert(snj_json_set_new(data, "source_sha256",
                            json_string(source_hash)) == 0);
    return data;
}

static json_t *
compaction_completed_data(const char *compact_id,
                          const char *source_hash,
                          const char *output_hash,
                          const char *output_count_hash,
                          uint64_t input_tokens_bound,
                          uint64_t output_tokens_bound,
                          const json_t *output)
{
    json_t *data = json_object();
    assert(data);
    assert(snj_json_set_new(data, "compact_id", json_string(compact_id)) == 0);
    assert(snj_json_set_new(data, "count_method",
                            json_string("qualified_upper_bound")) == 0);
    assert(snj_json_set_new(data, "input_tokens_bound",
                            json_integer((json_int_t)input_tokens_bound)) == 0);
    assert(snj_json_set_new(data, "output", json_deep_copy(output)) == 0);
    assert(snj_json_set_new(data, "output_count_method",
                            json_string("qualified_upper_bound")) == 0);
    assert(snj_json_set_new(data, "output_count_request_sha256",
                            json_string(output_count_hash)) == 0);
    assert(snj_json_set_new(data, "output_sha256", json_string(output_hash)) == 0);
    assert(snj_json_set_new(data, "output_tokens_bound",
                            json_integer((json_int_t)output_tokens_bound)) == 0);
    assert(snj_json_set_new(data, "source_sha256", json_string(source_hash)) == 0);
    return data;
}

static json_t *
empty_excerpt(void)
{
    json_t *out = json_object();
    assert(out);
    assert(snj_json_set_new(out, "discarded_bytes", json_integer(0)) == 0);
    assert(snj_json_set_new(out, "encoding", json_string("utf8")) == 0);
    assert(snj_json_set_new(out, "original_bytes", json_integer(0)) == 0);
    assert(snj_json_set_new(out, "retained", json_string("")) == 0);
    assert(snj_json_set_new(out, "retained_bytes", json_integer(0)) == 0);
    return out;
}

static json_t *
running_result(const char *handle)
{
    json_t *result = json_object();
    assert(result);
    assert(snj_json_set_new(result, "duration_ms", json_integer(50)) == 0);
    assert(snj_json_set_new(result, "exit_code", json_null()) == 0);
    assert(snj_json_set_new(result, "handle", json_string(handle)) == 0);
    assert(snj_json_set_new(result, "model_text",
                            json_string("managed process is still running")) == 0);
    assert(snj_json_set_new(result, "reason", json_null()) == 0);
    assert(snj_json_set_new(result, "signal", json_null()) == 0);
    assert(snj_json_set_new(result, "status", json_string("running")) == 0);
    assert(snj_json_set_new(result, "stderr", empty_excerpt()) == 0);
    assert(snj_json_set_new(result, "stdout", empty_excerpt()) == 0);
    assert(snj_tool_result_valid(result) == 0);
    return result;
}

static json_t *
tool_call_item(const char *call_id, const char *workspace)
{
    json_t *item = json_object();
    json_t *args = json_object();
    assert(item && args);
    assert(snj_json_set_new(args, "command", json_string("cat")) == 0);
    assert(snj_json_set_new(args, "pty", json_false()) == 0);
    assert(snj_json_set_new(args, "stdin", json_null()) == 0);
    assert(snj_json_set_new(args, "timeout_ms", json_integer(3000)) == 0);
    assert(snj_json_set_new(args, "workdir", json_string(workspace)) == 0);
    assert(snj_json_set_new(args, "yield_ms", json_integer(100)) == 0);
    assert(snj_json_set_new(item, "arguments", args) == 0);
    assert(snj_json_set_new(item, "call_id", json_string(call_id)) == 0);
    assert(snj_json_set_new(item, "kind", json_string("tool_call")) == 0);
    assert(snj_json_set_new(item, "name", json_string("exec_command")) == 0);
    assert(snj_json_set_new(item, "provider_call_id", json_string("call_exec")) == 0);
    assert(snj_json_set_new(item, "provider_item_id", json_string("item_exec")) == 0);
    return item;
}

static json_t *
response_completed_call(const char *turn_id, const char *response_id,
                        const char *call_id, const char *workspace)
{
    json_t *data = json_object();
    json_t *items = json_array();
    assert(data && items);
    assert(json_array_append_new(items, tool_call_item(call_id, workspace)) == 0);
    assert(snj_json_set_new(data, "cycle", json_integer(1)) == 0);
    assert(snj_json_set_new(data, "items", items) == 0);
    assert(snj_json_set_new(data, "provider_response_id", json_string("resp_call")) == 0);
    assert(snj_json_set_new(data, "response_id", json_string(response_id)) == 0);
    assert(snj_json_set_new(data, "status", json_string("completed")) == 0);
    assert(snj_json_set_new(data, "turn_id", json_string(turn_id)) == 0);
    assert(snj_json_set_new(data, "usage", usage()) == 0);
    return data;
}

static json_t *
tool_started_data(const char *turn_id, const char *call_id,
                  const char *action_sha256, const char *workspace)
{
    json_t *data = json_object();
    assert(data);
    assert(snj_json_set_new(data, "action_sha256",
                            json_string(action_sha256)) == 0);
    assert(snj_json_set_new(data, "call_id", json_string(call_id)) == 0);
    assert(snj_json_set_new(data, "resolved_workdir",
                            json_string(workspace)) == 0);
    assert(snj_json_set_new(data, "turn_id", json_string(turn_id)) == 0);
    return data;
}

static json_t *
tool_finished_data(const char *turn_id, const char *call_id, json_t *result)
{
    json_t *data = json_object();
    assert(data);
    assert(snj_json_set_new(data, "call_id", json_string(call_id)) == 0);
    assert(snj_json_set_new(data, "result", result) == 0);
    assert(snj_json_set_new(data, "turn_id", json_string(turn_id)) == 0);
    return data;
}

static json_t *
process_closed_data(const char *turn_id, const char *handle, json_t *result)
{
    json_t *data = json_object();
    assert(data);
    assert(snj_json_set_new(data, "cause", json_string("internal_failure")) == 0);
    assert(snj_json_set_new(data, "handle", json_string(handle)) == 0);
    assert(snj_json_set_new(data, "result", result) == 0);
    assert(snj_json_set_new(data, "turn_id", json_string(turn_id)) == 0);
    return data;
}

static json_t *
turn_interrupted_data(const char *turn_id)
{
    json_t *data = json_object();
    assert(data);
    assert(snj_json_set_new(data, "origin", json_string("recovery")) == 0);
    assert(snj_json_set_new(data, "reason", json_string("session_recovered")) == 0);
    assert(snj_json_set_new(data, "turn_id", json_string(turn_id)) == 0);
    return data;
}

static int
array_has_string(json_t *array, const char *value)
{
    size_t index;
    json_t *item;

    if (!json_is_array(array))
        return 0;
    for (index = 0u; index < json_array_size(array); ++index) {
        item = json_array_get(array, index);
        if (json_is_string(item) && strcmp(json_string_value(item), value) == 0)
            return 1;
    }
    return 0;
}

static void
assert_schema_type(json_t *schema, const char *type, int nullable)
{
    json_t *schema_type;

    assert(json_is_object(schema));
    schema_type = json_object_get(schema, "type");
    if (!nullable) {
        assert(json_is_string(schema_type));
        assert(strcmp(json_string_value(schema_type), type) == 0);
        return;
    }
    assert(json_is_array(schema_type));
    assert(json_array_size(schema_type) == 2u);
    assert(array_has_string(schema_type, type));
    assert(array_has_string(schema_type, "null"));
}

static void
assert_required_contains(json_t *required, const char *name)
{
    assert(array_has_string(required, name));
}

static json_t *
tool_by_name(json_t *tools, const char *name)
{
    size_t index;
    json_t *tool;

    assert(json_is_array(tools));
    for (index = 0u; index < json_array_size(tools); ++index) {
        tool = json_array_get(tools, index);
        const char *tool_name = snj_json_string(tool, "name");
        if (tool_name && strcmp(tool_name, name) == 0)
            return tool;
    }
    return NULL;
}

static json_t *
tool_by_type(json_t *tools, const char *type)
{
    size_t index;
    json_t *tool;

    assert(json_is_array(tools));
    for (index = 0u; index < json_array_size(tools); ++index) {
        tool = json_array_get(tools, index);
        const char *tool_type = snj_json_string(tool, "type");
        if (tool_type && strcmp(tool_type, type) == 0)
            return tool;
    }
    return NULL;
}

static json_t *
assert_strict_tool_contract(json_t *tool)
{
    const char *key;
    json_t *schema;
    json_t *params;
    json_t *properties;
    json_t *required;
    void *iter;

    assert(json_is_object(tool));
    assert(json_is_true(json_object_get(tool, "strict")));
    assert(strcmp(snj_json_string(tool, "type"), "function") == 0);
    params = json_object_get(tool, "parameters");
    assert(json_is_object(params));
    assert(strcmp(snj_json_string(params, "type"), "object") == 0);
    assert(json_is_false(json_object_get(params, "additionalProperties")));
    properties = json_object_get(params, "properties");
    required = json_object_get(params, "required");
    assert(json_is_object(properties));
    assert(json_is_array(required));
    assert(json_object_size(properties) == json_array_size(required));
    for (iter = json_object_iter(properties); iter;
         iter = json_object_iter_next(properties, iter)) {
        key = json_object_iter_key(iter);
        schema = json_object_iter_value(iter);
        assert(schema);
        assert_required_contains(required, key);
    }
    return properties;
}

static void
assert_context_tool_schemas(json_t *tools)
{
    size_t index;
    json_t *tool;
    json_t *properties;

    assert(json_is_array(tools));
    for (index = 0u; index < json_array_size(tools); ++index) {
        tool = json_array_get(tools, index);
        if (strcmp(snj_json_string(tool, "type"), "web_search") == 0) {
            assert(json_object_size(tool) == 1u);
            continue;
        }
        (void)assert_strict_tool_contract(tool);
    }

    tool = tool_by_type(tools, "web_search");
    if (tool)
        assert(json_object_size(tool) == 1u);

    tool = tool_by_name(tools, "exec_command");
    if (tool) {
        properties = assert_strict_tool_contract(tool);
        assert_schema_type(json_object_get(properties, "command"), "string", 0);
        assert_schema_type(json_object_get(properties, "workdir"), "string", 0);
        assert_schema_type(json_object_get(properties, "stdin"), "string", 1);
        assert_schema_type(json_object_get(properties, "pty"), "boolean", 1);
        assert_schema_type(json_object_get(properties, "yield_ms"), "integer", 1);
        assert_schema_type(json_object_get(properties, "timeout_ms"), "integer", 1);
    }

    tool = tool_by_name(tools, "write_stdin");
    assert(tool != NULL);
    properties = assert_strict_tool_contract(tool);
    assert_schema_type(json_object_get(properties, "handle"), "string", 0);
    assert_schema_type(json_object_get(properties, "data"), "string", 0);
    assert_schema_type(json_object_get(properties, "eof"), "boolean", 1);
    assert_schema_type(json_object_get(properties, "yield_ms"), "integer", 1);

    tool = tool_by_name(tools, "apply_patch");
    if (tool) {
        properties = assert_strict_tool_contract(tool);
        assert_schema_type(json_object_get(properties, "patch"), "string", 0);
        assert_schema_type(json_object_get(properties, "workdir"), "string", 0);
    }
}

int
main(void)
{
    char temp[] = "/tmp/snajpagent-context-XXXXXX";
    char state[4096];
    char workspace[4096];
    char agents[4096];
    char error[256];
    const char *turn1 = "01010101010101010101010101010101";
    const char *resp1 = "02020202020202020202020202020202";
    const char *turn2 = "03030303030303030303030303030303";
    const char *resp2 = "04040404040404040404040404040404";
    const char *compact1 = "07070707070707070707070707070707";
    const char *call2 = "05050505050505050505050505050505";
    const char *handle = "06060606060606060606060606060606";
    struct snj_store store;
    struct snj_session session;
    struct snj_context_projection projection;
    struct snj_instruction_set instructions;
    json_t *empty_steering;
    json_t *items;
    json_t *request_input;

    assert(mkdtemp(temp));
    assert(snprintf(state, sizeof(state), "%s/state", temp) > 0);
    assert(snprintf(workspace, sizeof(workspace), "%s/work", temp) > 0);
    assert(mkdir(state, 0700) == 0);
    assert(mkdir(workspace, 0700) == 0);
    assert(snprintf(agents, sizeof(agents), "%s/AGENTS.md", workspace) > 0);
    write_file(agents, "context guidance\n");
    assert(setenv("XDG_STATE_HOME", state, 1) == 0);

    snj_store_init(&store);
    snj_session_init(&session);
    snj_context_projection_init(&projection);
    snj_instructions_init(&instructions);
    assert(snj_store_open(&store, error, sizeof(error)) == 0);
    assert(snj_session_create(&store, &session, workspace, SNAJPAGENT_MODEL,
                              "default", error, sizeof(error)) == 0);
    assert(snj_session_commit(&session, "turn_started",
                              turn_started(turn1, 1, "ping", workspace, NULL),
                              NULL, error, sizeof(error)) == 0);
    assert(snj_session_commit(&session, "response_started",
                              response_started(turn1, resp1, NULL),
                              NULL, error, sizeof(error)) == 0);
    assert(snj_session_commit(&session, "response_completed",
                              response_completed(turn1, resp1, "pong"),
                              NULL, error, sizeof(error)) == 0);
    assert(snj_session_commit(&session, "turn_completed",
                              turn_completed(turn1, resp1),
                              NULL, error, sizeof(error)) == 0);
    {
        json_t *compact_request = NULL;
        json_t *compact_count_request = NULL;
        json_t *compact_output = compact_output_fixture();
        json_t *output_count_request = NULL;
        char source_hash[SNJ_SHA256_HEX_LEN + 1u];
        char request_hash[SNJ_SHA256_HEX_LEN + 1u];
        char output_hash[SNJ_SHA256_HEX_LEN + 1u];
        char output_count_hash[SNJ_SHA256_HEX_LEN + 1u];
        size_t source_bytes = 0u;
        size_t request_bytes = 0u;
        size_t output_bytes = 0u;
        size_t output_count_bytes = 0u;
        uint64_t source_seq = 0u;
        assert(snj_context_compact_request_build(&session,
                                                 session.default_model,
                                                 session.default_effort,
                                                 &compact_request,
                                                 &compact_count_request,
                                                 source_hash, &source_bytes,
                                                 request_hash, &request_bytes,
                                                 &source_seq,
                                                 error, sizeof(error)) == 0);
        assert(compact_request != NULL);
        assert(compact_count_request != NULL);
        assert(source_seq == session.next_seq - 1u);
        assert(source_bytes > 0u && request_bytes > 0u);
        assert(snj_context_compact_output_valid(compact_output, output_hash,
                                                &output_bytes,
                                                error, sizeof(error)) == 0);
        assert(snj_context_compact_output_count_request_build(compact_output,
                   session.default_model, &output_count_request,
                   output_count_hash, &output_count_bytes,
                   error, sizeof(error)) == 0);
        assert(output_count_request != NULL && output_count_bytes > 0u);
        assert(snj_session_commit(&session, "compaction_started",
                                  compaction_started_data(&session, compact1,
                                      "manual", source_seq, source_hash,
                                      request_hash, (uint64_t)source_bytes),
                                  NULL, error, sizeof(error)) == 0);
        assert(snj_session_commit(&session, "compaction_completed",
                                  compaction_completed_data(compact1,
                                      source_hash, output_hash, output_count_hash,
                                      (uint64_t)source_bytes,
                                      (uint64_t)output_bytes, compact_output),
                                  NULL, error, sizeof(error)) == 0);
        assert(strcmp(session.compact_id, compact1) == 0);
        json_decref(compact_request);
        json_decref(compact_count_request);
        json_decref(output_count_request);
        json_decref(compact_output);
    }
    {
        struct snj_session active;
        struct snj_context_projection active_projection;
        struct snj_instruction_set no_instructions;
        json_t *compact_request = NULL;
        json_t *compact_count_request = NULL;
        json_t *compact_output = compact_output_fixture();
        json_t *output_count_request = NULL;
        json_t *active_steering = json_array();
        json_t *input;
        char source_hash[SNJ_SHA256_HEX_LEN + 1u];
        char request_hash[SNJ_SHA256_HEX_LEN + 1u];
        char output_hash[SNJ_SHA256_HEX_LEN + 1u];
        char output_count_hash[SNJ_SHA256_HEX_LEN + 1u];
        size_t source_bytes = 0u;
        size_t request_bytes = 0u;
        size_t output_bytes = 0u;
        size_t output_count_bytes = 0u;
        uint64_t source_seq = 0u;
        const char *active_turn1 = "08080808080808080808080808080808";
        const char *active_resp1 = "09090909090909090909090909090909";
        const char *active_turn2 = "0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a";
        const char *active_compact = "0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b";

        snj_session_init(&active);
        snj_context_projection_init(&active_projection);
        snj_instructions_init(&no_instructions);
        assert(active_steering);
        assert(snj_session_create(&store, &active, workspace, SNAJPAGENT_MODEL,
                                  "default", error, sizeof(error)) == 0);
        assert(snj_session_commit(&active, "turn_started",
                                  turn_started(active_turn1, 1, "old",
                                               workspace, NULL),
                                  NULL, error, sizeof(error)) == 0);
        assert(snj_session_commit(&active, "response_started",
                                  response_started(active_turn1, active_resp1, NULL),
                                  NULL, error, sizeof(error)) == 0);
        assert(snj_session_commit(&active, "response_completed",
                                  response_completed(active_turn1, active_resp1,
                                                     "old answer"),
                                  NULL, error, sizeof(error)) == 0);
        assert(snj_session_commit(&active, "turn_completed",
                                  turn_completed(active_turn1, active_resp1),
                                  NULL, error, sizeof(error)) == 0);
        assert(snj_session_commit(&active, "turn_started",
                                  turn_started(active_turn2, 2, "new",
                                               workspace, NULL),
                                  NULL, error, sizeof(error)) == 0);
        assert(snj_context_compact_active_prefix_request_build(&active,
                   active.default_model, active.default_effort, &compact_request,
                   &compact_count_request, source_hash, &source_bytes,
                   request_hash, &request_bytes, &source_seq,
                   error, sizeof(error)) == 0);
        assert(compact_request != NULL && compact_count_request != NULL);
        assert(source_seq + 1u == active.next_seq - 1u);
        assert(source_bytes > 0u && request_bytes > 0u);
        assert(snj_context_compact_output_valid(compact_output, output_hash,
                                                &output_bytes,
                                                error, sizeof(error)) == 0);
        assert(snj_context_compact_output_count_request_build(compact_output,
                   active.default_model, &output_count_request,
                   output_count_hash, &output_count_bytes,
                   error, sizeof(error)) == 0);
        assert(snj_session_commit(&active, "compaction_started",
                                  compaction_started_data(&active,
                                      active_compact, "automatic", source_seq,
                                      source_hash, request_hash,
                                      (uint64_t)source_bytes),
                                  NULL, error, sizeof(error)) == 0);
        assert(snj_session_commit(&active, "compaction_completed",
                                  compaction_completed_data(active_compact,
                                      source_hash, output_hash, output_count_hash,
                                      (uint64_t)source_bytes,
                                      (uint64_t)output_bytes, compact_output),
                                  NULL, error, sizeof(error)) == 0);
        assert(active.active_turn);
        assert(strcmp(active.compact_id, active_compact) == 0);
        assert(snj_context_build(&active, SNAJPAGENT_MODEL, "medium", 1,
                                 active_steering, &no_instructions,
                                 &active_projection, error, sizeof(error)) == 0);
        input = json_object_get(active_projection.create_request, "input");
        assert(json_is_array(input));
        assert(json_array_size(input) == 3u);
        assert(strcmp(snj_json_string(json_array_get(input, 1), "type"),
                      "compaction") == 0);
        assert(strcmp(snj_json_string(json_array_get(input, 2), "content"),
                      "new") == 0);
        json_decref(compact_request);
        json_decref(compact_count_request);
        json_decref(output_count_request);
        json_decref(compact_output);
        json_decref(active_steering);
        snj_context_projection_free(&active_projection);
        snj_instructions_free(&no_instructions);
        snj_session_close(&active);
    }

    assert(snj_instructions_discover(&instructions, workspace,
                                     error, sizeof(error)) == 0);
    assert(instructions.count == 1u);
    assert(snj_session_commit(&session, "turn_started",
                              turn_started(turn2, 2, "again", workspace, snj_instructions_metadata_json(&instructions)),
                              NULL, error, sizeof(error)) == 0);

    empty_steering = json_array();
    assert(empty_steering);
    assert(snj_context_build(&session, SNAJPAGENT_MODEL, "medium", 1,
                             empty_steering, &instructions, &projection,
                             error, sizeof(error)) == 0);
    assert(projection.model_input_bytes > 0);
    assert(projection.create_request_bytes > 0);
    assert(projection.count_request_bytes > 0);
    assert(strcmp(projection.model_input_sha256,
                  projection.request_sha256) != 0);
    assert(strcmp(projection.count_request_sha256,
                  projection.request_sha256) != 0);
    assert(json_is_object(projection.count_request));
    assert(json_object_get(projection.count_request, "stream") == NULL);
    assert(json_object_get(projection.count_request, "store") == NULL);
    assert(json_object_get(projection.count_request, "max_output_tokens") == NULL);
    assert(strcmp(snj_json_string(projection.count_request, "model"),
                  SNAJPAGENT_MODEL) == 0);
    {
        json_t *tools = json_object_get(projection.create_request, "tools");
        assert(json_array_size(tools) == 4u);
        assert_context_tool_schemas(tools);
    }
    items = json_object_get(projection.model_input, "items");
    request_input = json_object_get(projection.create_request, "input");
    assert(json_is_array(items));
    assert(json_array_size(items) == 4);
    assert(json_is_array(request_input));
    assert(json_array_size(request_input) == 4);
    assert(strcmp(snj_json_string(json_array_get(request_input, 2), "type"),
                  "compaction") == 0);
    request_input = json_object_get(projection.count_request, "input");
    assert(json_is_array(request_input));
    assert(json_array_size(request_input) == 4);
    assert(strcmp(snj_json_string(json_array_get(request_input, 2), "type"),
                  "compaction") == 0);
    assert(strstr(snj_json_string(json_array_get(items, 1), "text"),
                  "context guidance") != NULL);
    assert(strcmp(snj_json_string(json_array_get(items, 2), "kind"),
                  "native_compact_output") == 0);
    assert(strcmp(snj_json_string(json_array_get(items, 2), "compact_id"),
                  compact1) == 0);
    assert(strcmp(snj_json_string(json_array_get(items, 3), "text"), "again") == 0);
    snj_context_projection_free(&projection);

    assert(snj_session_commit(&session, "response_started",
                              response_started(turn2, resp2, compact1),
                              NULL, error, sizeof(error)) == 0);
    assert(snj_session_commit(&session, "response_completed",
                              response_completed_call(turn2, resp2, call2, workspace),
                              NULL, error, sizeof(error)) == 0);
    assert(session.pending_call_count == 1u);
    assert(snj_session_commit(&session, "tool_started",
                              tool_started_data(turn2, call2,
                                                session.pending_calls[0].action_sha256,
                                                workspace),
                              NULL, error, sizeof(error)) == 0);
    assert(snj_session_commit(&session, "tool_finished",
                              tool_finished_data(turn2, call2, running_result(handle)),
                              NULL, error, sizeof(error)) == 0);
    assert(strcmp(session.active_process_handle, handle) == 0);
    assert(snj_context_build(&session, SNAJPAGENT_MODEL, "medium", 2,
                             empty_steering, &instructions, &projection,
                             error, sizeof(error)) == 0);
    {
        json_t *tools = json_object_get(projection.create_request, "tools");
        json_t *input = json_object_get(projection.create_request, "input");
        json_t *gate;
        const char *gate_text;
        assert(json_is_array(tools));
        assert(json_array_size(tools) == 1);
        assert(strcmp(snj_json_string(json_array_get(tools, 0), "name"),
                      "write_stdin") == 0);
        assert_context_tool_schemas(tools);
        assert(json_is_array(input));
        gate = json_array_get(input, json_array_size(input) - 1u);
        gate_text = snj_json_string(gate, "content");
        assert(gate_text != NULL);
        assert(strstr(gate_text, "only permitted tool call is write_stdin") != NULL);
        assert(strstr(gate_text, handle) != NULL);
    }
    assert(snj_session_commit(&session, "process_closed",
                              process_closed_data(turn2, handle,
                                  snj_tool_result_outcome_unknown("owner_lost")),
                              NULL, error, sizeof(error)) == 0);
    assert(session.active_process_handle[0] == '\0');
    assert(snj_session_commit(&session, "turn_interrupted",
                              turn_interrupted_data(turn2),
                              NULL, error, sizeof(error)) == 0);

    json_decref(empty_steering);
    snj_context_projection_free(&projection);
    snj_instructions_free(&instructions);
    snj_session_close(&session);
    snj_store_close(&store);
    puts("test_context: ok");
    return 0;
}

/* SPDX-License-Identifier: GPL-2.0-only */
#include "base.h"
#include "json.h"
#include "provider.h"
#include "store.h"
#include "turn.h"

#include <stdbool.h>
#include <errno.h>
#include <stddef.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static char managed_handle[SNAG_ID_HEX_LEN + 1u];
static const char wrong_managed_handle[] = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

static json_t *
empty_excerpt(void)
{
    json_t *out = json_object();

    if (!out ||
        snag_json_set_new(out, "discarded_bytes", json_integer(0)) < 0 ||
        snag_json_set_new(out, "encoding", json_string("utf8")) < 0 ||
        snag_json_set_new(out, "original_bytes", json_integer(0)) < 0 ||
        snag_json_set_new(out, "retained", json_string("")) < 0 ||
        snag_json_set_new(out, "retained_bytes", json_integer(0)) < 0) {
        if (out)
            json_decref(out);
        return NULL;
    }
    return out;
}

static json_t *
running_result_reason(const char *text, const char *reason)
{
    json_t *result = json_object();

    if (!result ||
        snag_json_set_new(result, "duration_ms", json_integer(0)) < 0 ||
        snag_json_set_new(result, "exit_code", json_null()) < 0 ||
        snag_json_set_new(result, "handle", json_string(managed_handle)) < 0 ||
        snag_json_set_new(result, "model_text", json_string(text)) < 0 ||
        snag_json_set_new(result, "reason",
                         reason ? json_string(reason) : json_null()) < 0 ||
        snag_json_set_new(result, "signal", json_null()) < 0 ||
        snag_json_set_new(result, "status", json_string("running")) < 0 ||
        snag_json_set_new(result, "stderr", empty_excerpt()) < 0 ||
        snag_json_set_new(result, "stdout", empty_excerpt()) < 0) {
        if (result)
            json_decref(result);
        return NULL;
    }
    return result;
}

static json_t *
running_result(const char *text)
{
    return running_result_reason(text, NULL);
}

static int
set_response_id(struct snag_response_graph *graph, unsigned int cycle,
                const char *suffix)
{
    char id[128];
    if (snprintf(id, sizeof(id), "resp_fixture_%u_%s", cycle, suffix) < 0)
        return -1;
    return snag_response_graph_set_provider_id(graph, id);
}

static void
set_usage(struct snag_response_graph *graph, uint64_t input_tokens,
          uint64_t output_tokens)
{
    graph->usage.input_tokens = input_tokens;
    graph->usage.output_tokens = output_tokens;
    graph->usage.total_tokens = input_tokens + output_tokens;
    graph->usage.input_known = true;
    graph->usage.output_known = true;
    graph->usage.total_known = true;
}

static int
emit_public(struct snag_response_graph *graph, snag_responses_emit_fn emit, void *opaque,
            enum snag_item_kind kind, enum snag_item_phase phase,
            const char *provider_id, const char *text, int pattern)
{
    size_t index = graph->count;
    size_t len = strlen(text);
    size_t split = len / 2u;

    if (snag_response_graph_add_public(graph, kind, phase, provider_id, text) < 0)
        return -1;
    if (pattern == 1)
        return emit(opaque, index, kind, phase, provider_id, "ha", 2u) < 0 ||
               emit(opaque, index, kind, phase, provider_id, "ha", 2u) < 0 ? -1 : 0;
    if (pattern == 2)
        return emit(opaque, index, kind, phase, provider_id, text, 1u) < 0 ||
               emit(opaque, index, kind, phase, provider_id, text + 1u, len - 1u) < 0 ? -1 : 0;
    if ((split && emit(opaque, index, kind, phase, provider_id, text, split) < 0) ||
        emit(opaque, index, kind, phase, provider_id, text + split, len - split) < 0)
        return -1;
    return 0;
}

static json_t *
exec_arguments(const char *workspace, const char *command)
{
    json_t *args = json_object();
    if (!args ||
        snag_json_set_new(args, "command", json_string(command)) < 0 ||
        snag_json_set_new(args, "pty", json_false()) < 0 ||
        snag_json_set_new(args, "stdin", json_null()) < 0 ||
        snag_json_set_new(args, "timeout_ms", json_integer(1000)) < 0 ||
        snag_json_set_new(args, "workdir", json_string(workspace)) < 0 ||
        snag_json_set_new(args, "yield_ms", json_integer(1000)) < 0 ||
        snag_json_set_new(args, "max_output_tokens", json_null()) < 0) {
        if (args)
            json_decref(args);
        return NULL;
    }
    return args;
}

static int
add_call(struct snag_response_graph *graph, const char *workspace,
         unsigned int cycle, unsigned int index, const char *command)
{
    char item_id[128];
    char call_id[128];
    json_t *args = exec_arguments(workspace, command);
    if (!args ||
        snprintf(item_id, sizeof(item_id), "item_fixture_%u_%u", cycle, index) < 0 ||
        snprintf(call_id, sizeof(call_id), "call_fixture_%u_%u", cycle, index) < 0) {
        if (args)
            json_decref(args);
        return -1;
    }
    return snag_response_graph_add_call(graph, item_id, call_id,
                                       "exec_command", args);
}

static int
add_stdin_call(struct snag_response_graph *graph, unsigned int cycle,
               unsigned int index, const char *handle, bool malformed)
{
    char item_id[128];
    char call_id[128];
    json_t *args = json_object();

    if (!args ||
        snag_json_set_new(args, "handle", json_string(handle)) < 0 ||
        (!malformed &&
         snag_json_set_new(args, "data", json_string("")) < 0) ||
        snag_json_set_new(args, "eof", json_null()) < 0 ||
        snag_json_set_new(args, "terminate", json_false()) < 0 ||
        snag_json_set_new(args, "yield_ms", json_integer(0)) < 0 ||
        snag_json_set_new(args, "max_output_tokens", json_null()) < 0 ||
        snprintf(item_id, sizeof(item_id), "item_fixture_%u_%u", cycle,
                 index) < 0 ||
        snprintf(call_id, sizeof(call_id), "call_fixture_%u_%u", cycle,
                 index) < 0) {
        if (args)
            json_decref(args);
        return -1;
    }
    return snag_response_graph_add_call(graph, item_id, call_id,
                                       "write_stdin", args);
}

static int
add_terminate_call(struct snag_response_graph *graph, unsigned int cycle,
                   unsigned int index, const char *handle)
{
    char item_id[128];
    char call_id[128];
    json_t *args = json_object();

    if (!args ||
        snag_json_set_new(args, "handle", json_string(handle)) < 0 ||
        snag_json_set_new(args, "data", json_string("")) < 0 ||
        snag_json_set_new(args, "eof", json_null()) < 0 ||
        snag_json_set_new(args, "terminate", json_true()) < 0 ||
        snag_json_set_new(args, "yield_ms", json_integer(0)) < 0 ||
        snag_json_set_new(args, "max_output_tokens", json_null()) < 0 ||
        snprintf(item_id, sizeof(item_id), "item_fixture_%u_%u", cycle,
                 index) < 0 ||
        snprintf(call_id, sizeof(call_id), "call_fixture_%u_%u", cycle,
                 index) < 0) {
        if (args)
            json_decref(args);
        return -1;
    }
    return snag_response_graph_add_call(graph, item_id, call_id,
                                       "write_stdin", args);
}

static int
add_irc_send_call(struct snag_response_graph *graph, unsigned int cycle,
                  unsigned int index, const char *text)
{
    char item_id[128];
    char call_id[128];
    json_t *args = json_object();

    if (!args ||
        snag_json_set_new(args, "notice", json_false()) < 0 ||
        snag_json_set_new(args, "destination", json_null()) < 0 ||
        snag_json_set_new(args, "text", json_string(text)) < 0 ||
        snprintf(item_id, sizeof(item_id), "item_fixture_%u_%u", cycle,
                 index) < 0 ||
        snprintf(call_id, sizeof(call_id), "call_fixture_%u_%u", cycle,
                 index) < 0) {
        if (args)
            json_decref(args);
        return -1;
    }
    return snag_response_graph_add_call(graph, item_id, call_id,
                                       "irc_send", args);
}

static bool
steering_contains(const json_t *steering, const char *needle)
{
    if (!json_is_array(steering))
        return false;
    for (size_t i = 0u; i < json_array_size(steering); ++i) {
        const char *text = snag_json_string(json_array_get(steering, i), "text");

        if (text && strstr(text, needle))
            return true;
    }
    return false;
}

static int
add_goal_call(struct snag_response_graph *graph, unsigned int cycle,
              const char *action, const char *text)
{
    char item_id[128];
    char call_id[128];
    json_t *args = json_object();

    if (!args ||
        snag_json_set_new(args, "action", json_string(action)) < 0 ||
        snag_json_set_new(args, "text",
                         text ? json_string(text) : json_null()) < 0 ||
        snprintf(item_id, sizeof(item_id),
                 "item_fixture_goal_%u", cycle) < 0 ||
        snprintf(call_id, sizeof(call_id),
                 "call_fixture_goal_%u", cycle) < 0) {
        if (args)
            json_decref(args);
        return -1;
    }
    return snag_response_graph_add_call(graph, item_id, call_id,
                                       "update_goal", args);
}

static int
add_create_goal_call(struct snag_response_graph *graph, unsigned int cycle,
                     const char *objective)
{
    char item_id[128];
    char call_id[128];
    json_t *args = json_object();

    if (!args ||
        snag_json_set_new(args, "objective", json_string(objective)) < 0 ||
        snprintf(item_id, sizeof(item_id),
                 "item_fixture_create_goal_%u", cycle) < 0 ||
        snprintf(call_id, sizeof(call_id),
                 "call_fixture_create_goal_%u", cycle) < 0) {
        if (args)
            json_decref(args);
        return -1;
    }
    return snag_response_graph_add_call(graph, item_id, call_id,
                                       "create_goal", args);
}

static bool
managed_prompt(const char *prompt)
{
    return strcmp(prompt, "managed_wrong_handle") == 0 ||
           strcmp(prompt, "managed_malformed") == 0 ||
           strcmp(prompt, "managed_final_violation") == 0 ||
           strcmp(prompt, "managed_wrong_tool_violation") == 0 ||
           strcmp(prompt, "managed_multiple_violation") == 0;
}

int
snag_fixture_response(const char *prompt, const json_t *steering,
                     const char *workspace, unsigned int cycle,
                     const char *goal_prompt, uint64_t goal_turn_count,
                     snag_responses_emit_fn emit, snag_provider_pump_fn pump, void *opaque,
                     struct snag_response_graph *graph,
                     struct snag_provider_failure *failure,
                     char *error, size_t error_size)
{
    if (strcmp(prompt, "crash") == 0 && cycle == 1u)
        _exit(99);
    if (strcmp(prompt, "provider_fail") == 0 && cycle == 1u) {
        if (error_size)
            (void)snprintf(error, error_size, "fixture provider failed");
        return -1;
    }
    if (strcmp(prompt, "empty_message_recovery") == 0 && cycle == 1u) {
        if (failure)
            failure->output_correction = SNAG_OUTPUT_CORRECTION_EMPTY;
        if (error_size)
            (void)snprintf(error, error_size, "%s",
                           SNAG_EMPTY_OUTPUT_CORRECTION);
        return -1;
    }
    if (strcmp(prompt, "oversized_message_recovery") == 0 && cycle == 1u) {
        if (failure)
            failure->output_correction = SNAG_OUTPUT_CORRECTION_OVERSIZED;
        if (error_size)
            (void)snprintf(error, error_size, "%s",
                           SNAG_OVERSIZED_OUTPUT_CORRECTION);
        return -1;
    }
    if (set_response_id(graph, cycle, "complete") < 0)
        goto allocation;
    if (strcmp(prompt, "empty_message_recovery") == 0) {
        if (!steering_contains(steering, SNAG_EMPTY_OUTPUT_CORRECTION)) {
            if (error_size)
                (void)snprintf(error, error_size,
                               "fixture did not receive empty correction");
            return -1;
        }
        return emit_public(graph, emit, opaque, SNAG_ITEM_ASSISTANT,
                           SNAG_PHASE_FINAL_ANSWER,
                           "msg_fixture_empty_recovered",
                           "empty message recovered", 0);
    }
    if (strcmp(prompt, "oversized_message_recovery") == 0) {
        if (!steering_contains(steering, SNAG_OVERSIZED_OUTPUT_CORRECTION)) {
            if (error_size)
                (void)snprintf(error, error_size,
                               "fixture did not receive oversized correction");
            return -1;
        }
        return emit_public(graph, emit, opaque, SNAG_ITEM_ASSISTANT,
                           SNAG_PHASE_FINAL_ANSWER,
                           "msg_fixture_oversized_recovered",
                           "oversized message recovered", 0);
    }
    if (strcmp(prompt, "context_anchor_chain") == 0) {
        set_usage(graph, 8000u + (uint64_t)(cycle - 1u) * 24000u, 100u);
        if (cycle <= 4u)
            return add_call(graph, workspace, cycle, 0u,
                            "fixture context anchor large output");
        return emit_public(graph, emit, opaque, SNAG_ITEM_ASSISTANT,
                           SNAG_PHASE_FINAL_ANSWER,
                           "msg_fixture_context_anchor",
                           "context anchor complete", 0);
    }
    if (strcmp(prompt, SNAG_GOAL_CONTINUATION_TEXT) == 0) {
        if (!goal_prompt)
            goto allocation;
        if (strcmp(goal_prompt, "failing goal") == 0 && cycle == 1u) {
            if (error_size)
                (void)snprintf(error, error_size,
                               "fixture goal provider failed");
            return -1;
        }
        if (strcmp(goal_prompt, "refusing goal") == 0)
            return emit_public(graph, emit, opaque, SNAG_ITEM_REFUSAL,
                               SNAG_PHASE_FINAL_ANSWER,
                               "msg_fixture_goal_refusal",
                               "I cannot continue this goal.", 0);
        if (strcmp(goal_prompt, "slow goal") == 0 &&
            goal_turn_count == 1u) {
            if (cycle == 1u) {
                if (emit_public(graph, emit, opaque, SNAG_ITEM_ASSISTANT,
                                SNAG_PHASE_COMMENTARY,
                                "msg_fixture_goal_slow_commentary",
                                "working on goal\n", 0) < 0)
                    goto allocation;
                for (unsigned int i = 0; i < 100u; ++i) {
                    int pump_rc = pump(opaque, 20u);
                    if (pump_rc != 0)
                        return pump_rc;
                }
            }
            return emit_public(graph, emit, opaque, SNAG_ITEM_ASSISTANT,
                               SNAG_PHASE_FINAL_ANSWER,
                               "msg_fixture_goal_checkpoint",
                               "goal checkpoint", 0);
        }
        if (strcmp(goal_prompt, "automatic goal") == 0 &&
            goal_turn_count == 1u)
            return emit_public(graph, emit, opaque, SNAG_ITEM_ASSISTANT,
                               SNAG_PHASE_FINAL_ANSWER,
                               "msg_fixture_goal_checkpoint",
                               "goal checkpoint", 0);
        if (strcmp(goal_prompt, "rewrite goal") == 0 && cycle == 1u)
            return add_goal_call(graph, cycle, "rewrite", "rewritten goal");
        if (strcmp(goal_prompt, "rewritten goal") == 0 && cycle == 2u)
            return add_goal_call(graph, cycle, "complete", NULL);
        if (strcmp(goal_prompt, "tiny") == 0 && cycle == 1u)
            return add_goal_call(graph, cycle, "rewrite", "too long");
        if (strcmp(goal_prompt, "locked goal") == 0 && cycle == 1u) {
            if (emit_public(graph, emit, opaque, SNAG_ITEM_ASSISTANT,
                            SNAG_PHASE_COMMENTARY,
                            "msg_fixture_goal_lock_commentary",
                            "preparing goal rewrite\n", 0) < 0)
                goto allocation;
            for (unsigned int i = 0; i < 50u; ++i) {
                int pump_rc = pump(opaque, 20u);
                if (pump_rc != 0)
                    return pump_rc;
            }
            return add_goal_call(graph, cycle, "rewrite", "forbidden rewrite");
        }
        if (strcmp(goal_prompt, "blocked goal") == 0 && cycle == 1u)
            return add_goal_call(graph, cycle, "block",
                                 "fixture dependency is unavailable");
        /* The user-control case owns completion even after the slow turn ends. */
        if ((cycle == 1u && strcmp(goal_prompt, "retitled goal") != 0) ||
            ((strcmp(goal_prompt, "locked goal") == 0 ||
              strcmp(goal_prompt, "tiny") == 0) && cycle == 2u))
            return add_goal_call(graph, cycle, "complete", NULL);
        return emit_public(graph, emit, opaque, SNAG_ITEM_ASSISTANT,
                           SNAG_PHASE_FINAL_ANSWER,
                           "msg_fixture_goal_done", "goal done", 0);
    }
    if (strcmp(prompt, "please create a persistent goal") == 0) {
        if (cycle == 1u)
            return add_create_goal_call(graph, cycle, "model-created goal");
        return emit_public(graph, emit, opaque, SNAG_ITEM_ASSISTANT,
                           SNAG_PHASE_FINAL_ANSWER,
                           "msg_fixture_model_goal_checkpoint",
                           "model-created checkpoint", 0);
    }
    if (strcmp(prompt, "ro_native") == 0) {
        static const char *const names[] = {"list_files", "read_file", "grep"};
        static const char *const arguments[] = {
            "{\"path\":\".\",\"recursive\":false,\"offset\":null,\"limit\":null}",
            "{\"path\":\"ro-input.txt\",\"start_line\":null,\"end_line\":null}",
            "{\"path\":\"ro-input.txt\",\"pattern\":\"native\",\"recursive\":false,\"ignore_case\":false,\"literal\":true,\"offset\":null,\"limit\":null}"
        };
        if (cycle <= 3u) {
            const char *text = arguments[cycle - 1u];
            json_t *args = snag_json_load_strict((const unsigned char *)text,
                strlen(text), 4096u, error, error_size);
            if (!args)
                return -1;
            return snag_response_graph_add_call(graph, "item_native", "call_native",
                                               names[cycle - 1u], args);
        }
        return emit_public(graph, emit, opaque, SNAG_ITEM_ASSISTANT,
                           SNAG_PHASE_FINAL_ANSWER, "msg_native", "native complete", 0);
    }
    if (strcmp(prompt, "ro_denied") == 0) {
        static const char *const names[] = {"exec_command", "apply_patch", "write_stdin",
            "create_goal", "update_goal", "irc_send", "irc_topic", "irc_state"};
        if (cycle <= sizeof(names) / sizeof(names[0]))
            return snag_response_graph_add_call(graph, "item_denied", "call_denied",
                                               names[cycle - 1u], json_object());
        return emit_public(graph, emit, opaque, SNAG_ITEM_ASSISTANT,
                           SNAG_PHASE_FINAL_ANSWER, "msg_denied", "denied complete", 0);
    }
    if (strstr(prompt, "network_prompt_catchup")) {
        const char *kind = strstr(prompt, "slash") ? "slash" : "tab";
        const char *item = strstr(prompt, "_two") ? "two" : "one";
        char text[64];
        char item_id[64];

        if (snprintf(text, sizeof(text), "%s-catchup-%s", kind, item) < 0 ||
            snprintf(item_id, sizeof(item_id),
                     "msg_fixture_%s_catchup_%s", kind, item) < 0)
            goto allocation;
        return emit_public(graph, emit, opaque, SNAG_ITEM_ASSISTANT,
                           SNAG_PHASE_FINAL_ANSWER, item_id, text, 0);
    }
    if (strstr(prompt, "network_zero"))
        return emit_public(graph, emit, opaque, SNAG_ITEM_ASSISTANT,
                           SNAG_PHASE_FINAL_ANSWER, "msg_fixture_network_zero",
                           "network zero local only", 0);
    if (strstr(prompt, "network_one")) {
        if (cycle == 1u)
            return add_irc_send_call(graph, cycle, 0u, "network one reply");
        return emit_public(graph, emit, opaque, SNAG_ITEM_ASSISTANT,
                           SNAG_PHASE_FINAL_ANSWER, "msg_fixture_network_one",
                           "network one local completion", 0);
    }
    if (strstr(prompt, "network_view_stream") && cycle == 1u)
        return add_irc_send_call(graph, cycle, 0u,
                                 "network stream acknowledged");
    if (strstr(prompt, "network_commentary")) {
        if (cycle == 1u) {
            if (emit_public(graph, emit, opaque, SNAG_ITEM_ASSISTANT,
                            SNAG_PHASE_COMMENTARY,
                            "msg_fixture_network_commentary_local",
                            "network local planning", 0) < 0)
                return -1;
            return add_irc_send_call(graph, cycle, 0u,
                                     "network commentary reply");
        }
        return emit_public(graph, emit, opaque, SNAG_ITEM_ASSISTANT,
                           SNAG_PHASE_FINAL_ANSWER,
                           "msg_fixture_network_commentary_final",
                           "network commentary local completion", 0);
    }
    if (strstr(prompt, "network_operator")) {
        if (cycle == 1u)
            return add_irc_send_call(graph, cycle, 0u,
                                     "network operator reply");
        return emit_public(graph, emit, opaque, SNAG_ITEM_ASSISTANT,
                           SNAG_PHASE_FINAL_ANSWER,
                           "msg_fixture_network_operator",
                           "network operator local completion", 0);
    }
    if (strstr(prompt, "network_mention")) {
        if (cycle == 1u)
            return add_irc_send_call(graph, cycle, 0u,
                                     "network mention reply");
        return emit_public(graph, emit, opaque, SNAG_ITEM_ASSISTANT,
                           SNAG_PHASE_FINAL_ANSWER,
                           "msg_fixture_network_mention",
                           "network mention local completion", 0);
    }
    if (strstr(prompt, "network_count_wait")) {
        if (cycle == 1u &&
            !steering_contains(steering, "network count mention")) {
            if (error_size)
                (void)snprintf(error, error_size,
                               "fixture count request was not rebuilt for IRC mention");
            return -1;
        }
        if (cycle == 1u)
            return add_irc_send_call(graph, cycle, 0u,
                                     "network count mention reply");
        return emit_public(graph, emit, opaque, SNAG_ITEM_ASSISTANT,
                           SNAG_PHASE_FINAL_ANSWER,
                           "msg_fixture_network_count",
                           "network count local completion", 0);
    }
    if (strstr(prompt, "network_reminder")) {
        if (cycle == 1u)
            return add_irc_send_call(graph, cycle, 0u, "");
        if (cycle == 2u)
            return emit_public(graph, emit, opaque, SNAG_ITEM_ASSISTANT,
                               SNAG_PHASE_FINAL_ANSWER,
                               "msg_fixture_network_reminder_unsent",
                               "network reminder unsent local reply", 0);
        if (cycle == 3u) {
            if (!steering_contains(steering, "Use irc_send")) {
                if (error_size)
                    (void)snprintf(error, error_size,
                                   "fixture did not receive irc_send reminder");
                return -1;
            }
            return add_irc_send_call(graph, cycle, 0u,
                                     "network reminder reply");
        }
        return emit_public(graph, emit, opaque, SNAG_ITEM_ASSISTANT,
                           SNAG_PHASE_FINAL_ANSWER,
                           "msg_fixture_network_reminder",
                           "network reminder local completion", 0);
    }
    if (strstr(prompt, "network_tool")) {
        if (cycle == 1u)
            return add_call(graph, workspace, cycle, 0u, "fixture ok");
        if (cycle == 2u)
            return add_irc_send_call(graph, cycle, 0u,
                                     "network tool complete");
        return emit_public(graph, emit, opaque, SNAG_ITEM_ASSISTANT,
                           SNAG_PHASE_FINAL_ANSWER,
                           "msg_fixture_network_tool",
                           "network tool local completion", 0);
    }
    if (strstr(prompt, "network_managed")) {
        if (cycle == 1u)
            return add_call(graph, workspace, cycle, 0u,
                            "fixture managed start");
        if (cycle == 2u) {
            for (unsigned int i = 0u; i < 100u; ++i) {
                int pump_rc = pump(opaque, 20u);

                if (pump_rc != 0)
                    return pump_rc;
            }
            return add_stdin_call(graph, cycle, 0u, managed_handle, false);
        }
        if (cycle == 3u) {
            if (!steering_contains(steering, "network managed mention")) {
                if (error_size)
                    (void)snprintf(error, error_size,
                                   "fixture did not receive managed IRC mention");
                return -1;
            }
            if (add_irc_send_call(graph, cycle, 0u,
                                  "network managed reaction") < 0 ||
                add_stdin_call(graph, cycle, 1u,
                               managed_handle, false) < 0)
                goto allocation;
            return 0;
        }
        return emit_public(graph, emit, opaque, SNAG_ITEM_ASSISTANT,
                           SNAG_PHASE_FINAL_ANSWER,
                           "msg_fixture_network_managed",
                           "network managed local completion", 0);
    }
    if (strcmp(prompt, "managed_command_steer") == 0) {
        if (cycle == 1u)
            return add_call(graph, workspace, cycle, 0u,
                            "fixture managed steering wait");
        if (cycle == 2u) {
            if (!steering_contains(steering, "terminate it")) {
                if (error_size)
                    (void)snprintf(error, error_size,
                                   "fixture did not receive command steering");
                return -1;
            }
            return add_terminate_call(graph, cycle, 0u, managed_handle);
        }
        return emit_public(graph, emit, opaque, SNAG_ITEM_ASSISTANT,
                           SNAG_PHASE_FINAL_ANSWER,
                           "msg_fixture_managed_steered",
                           "managed command steering complete", 0);
    }
    if (strcmp(prompt, "managed_command_queue") == 0) {
        if (cycle == 1u)
            return add_call(graph, workspace, cycle, 0u,
                            "fixture managed queue wait");
        return emit_public(graph, emit, opaque, SNAG_ITEM_ASSISTANT,
                           SNAG_PHASE_FINAL_ANSWER,
                           "msg_fixture_managed_queued",
                           "managed command queue complete", 0);
    }
    if (managed_prompt(prompt)) {
        if (cycle == 1u)
            return add_call(graph, workspace, cycle, 0u,
                            "fixture managed start");
        if (cycle == 2u) {
            if (strcmp(prompt, "managed_wrong_handle") == 0)
                return add_stdin_call(graph, cycle, 0u,
                                      wrong_managed_handle, false);
            if (strcmp(prompt, "managed_malformed") == 0)
                return add_stdin_call(graph, cycle, 0u,
                                      managed_handle, true);
            if (strcmp(prompt, "managed_final_violation") == 0)
                return emit_public(graph, emit, opaque, SNAG_ITEM_ASSISTANT,
                                   SNAG_PHASE_FINAL_ANSWER,
                                   "msg_fixture_managed_early_final",
                                   "must not complete", 0);
            if (strcmp(prompt, "managed_wrong_tool_violation") == 0)
                return add_call(graph, workspace, cycle, 0u,
                                "fixture forbidden tool");
            if (add_stdin_call(graph, cycle, 0u, managed_handle, false) < 0 ||
                add_stdin_call(graph, cycle, 1u, managed_handle, false) < 0)
                goto allocation;
            return 0;
        }
        if (cycle == 3u && strcmp(prompt, "managed_wrong_handle") == 0)
            return add_stdin_call(graph, cycle, 0u,
                                  wrong_managed_handle, false);
        if (cycle == 3u || cycle == 4u)
            return add_stdin_call(graph, cycle, 0u, managed_handle, false);
        return emit_public(graph, emit, opaque, SNAG_ITEM_ASSISTANT,
                           SNAG_PHASE_FINAL_ANSWER,
                           "msg_fixture_managed_recovered",
                           "managed process recovered", 0);
    }
    if (strcmp(prompt, "empty") == 0)
        return 0;
    if (strcmp(prompt, "terminal_render") == 0) {
        static const char first[] =
            "alpha beta gamma delta-extraordinary ";
        static const char second_prefix[] =
            "zeta eta theta\nexplicit café ";
        static const char euro_first[] = "\xe2";
        static const char second_suffix[] = "\x82\xac line\n";
        static const char third[] =
            "supercalifragilisticexpialidocious0123456789ABCDEFGHIJ "
            "tail control:\x1b[31m";
        static const char full[] =
            "alpha beta gamma delta-extraordinary "
            "zeta eta theta\nexplicit café € line\n"
            "supercalifragilisticexpialidocious0123456789ABCDEFGHIJ "
            "tail control:\x1b[31m";
        size_t index = graph->count;

        if (snag_response_graph_add_public(
                graph, SNAG_ITEM_ASSISTANT, SNAG_PHASE_FINAL_ANSWER,
                "msg_fixture_terminal_render", full) < 0 ||
            emit(opaque, index, SNAG_ITEM_ASSISTANT, SNAG_PHASE_FINAL_ANSWER, graph->items[index].provider_item_id,
                 first, sizeof(first) - 1u) < 0)
            goto allocation;
        for (unsigned int i = 0u; i < 50u; ++i) {
            int pump_rc = pump(opaque, 20u);

            if (pump_rc != 0)
                return pump_rc;
        }
        if (emit(opaque, index, SNAG_ITEM_ASSISTANT, SNAG_PHASE_FINAL_ANSWER, graph->items[index].provider_item_id,
                 second_prefix, sizeof(second_prefix) - 1u) < 0 ||
            emit(opaque, index, SNAG_ITEM_ASSISTANT, SNAG_PHASE_FINAL_ANSWER, graph->items[index].provider_item_id,
                 euro_first, sizeof(euro_first) - 1u) < 0 ||
            emit(opaque, index, SNAG_ITEM_ASSISTANT, SNAG_PHASE_FINAL_ANSWER, graph->items[index].provider_item_id,
                 second_suffix, sizeof(second_suffix) - 1u) < 0)
            goto allocation;
        /* Leave time for the test to begin its second typing pause. */
        for (unsigned int i = 0u; i < 100u; ++i) {
            int pump_rc = pump(opaque, 20u);

            if (pump_rc != 0)
                return pump_rc;
        }
        if (emit(opaque, index, SNAG_ITEM_ASSISTANT, SNAG_PHASE_FINAL_ANSWER, graph->items[index].provider_item_id,
                 third, sizeof(third) - 1u) < 0)
            goto allocation;
        /* Keep the turn active until that paused output becomes visible. */
        for (unsigned int i = 0u; i < 100u; ++i) {
            int pump_rc = pump(opaque, 20u);

            if (pump_rc != 0)
                return pump_rc;
        }
        return 0;
    }
    if (strcmp(prompt, "render_flood") == 0) {
        struct snag_buf text;
        int rc = -1;
        snag_buf_init(&text, 128u * 1024u);
        if (snag_buf_printf(&text, "| row | text |\n| --- | --- |\n") < 0)
            goto flood_done;
        for (unsigned int i = 0u; i < 2048u; ++i)
            if (snag_buf_printf(&text, "| row-%04u | **bold** and `code` |\n", i) < 0)
                goto flood_done;
        if (snag_buf_printf(&text, "\nflood-end\n") == 0 &&
            snag_buf_terminate(&text) == 0)
            rc = emit_public(graph, emit, opaque, SNAG_ITEM_ASSISTANT,
                SNAG_PHASE_FINAL_ANSWER, "msg_flood", (char *)text.data, 0);
flood_done:
        snag_buf_free(&text);
        return rc;
    }
    if (strcmp(prompt, "engine_blocked") == 0) {
        static const char text[] = "engine-block-start engine-block-end";
        size_t index = graph->count;
        struct timespec delay = {2, 500000000};

        if (snag_response_graph_add_public(graph, SNAG_ITEM_ASSISTANT,
                SNAG_PHASE_FINAL_ANSWER, "msg_engine_blocked", text) < 0 ||
            emit(opaque, index, SNAG_ITEM_ASSISTANT, SNAG_PHASE_FINAL_ANSWER, graph->items[index].provider_item_id,
                 text, 19u) < 0)
            goto allocation;
        /* Intentionally no pump: models a sync/lock/DNS/library stall. */
        while (nanosleep(&delay, &delay) < 0 && errno == EINTR)
            ;
        if (emit(opaque, index, SNAG_ITEM_ASSISTANT, SNAG_PHASE_FINAL_ANSWER, graph->items[index].provider_item_id,
                 text + 19u, sizeof(text) - 1u - 19u) < 0)
            goto allocation;
        return 0;
    }
    if (strcmp(prompt, "terminal_status") == 0) {
        static const char full[] =
            "status-first-fragment status-second-fragment";
        static const char first[] = "status-first-fragment ";
        static const char second[] = "status-second-fragment";
        size_t index = graph->count;

        if (snag_response_graph_add_public(
                graph, SNAG_ITEM_ASSISTANT, SNAG_PHASE_FINAL_ANSWER,
                "msg_fixture_terminal_status", full) < 0)
            goto allocation;
        for (unsigned int i = 0u; i < 50u; ++i) {
            int pump_rc = pump(opaque, 20u);

            if (pump_rc != 0)
                return pump_rc;
        }
        if (emit(opaque, index, SNAG_ITEM_ASSISTANT, SNAG_PHASE_FINAL_ANSWER, graph->items[index].provider_item_id,
                 first, sizeof(first) - 1u) < 0)
            goto allocation;
        for (unsigned int i = 0u; i < 60u; ++i) {
            int pump_rc = pump(opaque, 20u);

            if (pump_rc != 0)
                return pump_rc;
        }
        if (emit(opaque, index, SNAG_ITEM_ASSISTANT, SNAG_PHASE_FINAL_ANSWER, graph->items[index].provider_item_id,
                 second, sizeof(second) - 1u) < 0)
            goto allocation;
        return 0;
    }
    if (strcmp(prompt, "public_index_gap") == 0) {
        if (emit_public(graph, emit, opaque, SNAG_ITEM_ASSISTANT,
                        SNAG_PHASE_COMMENTARY,
                        "msg_fixture_gap_commentary",
                        "Checking hidden work.\n", 0) < 0)
            goto allocation;
        if (snag_response_graph_add_public(graph, SNAG_ITEM_ASSISTANT,
                SNAG_PHASE_FINAL_ANSWER, "msg_fixture_gap_final",
                "Gap-safe final.") < 0)
            goto allocation;
        /* Inert wire item 1 is absent from the supported output graph. */
        return emit(opaque, 2u, SNAG_ITEM_ASSISTANT, SNAG_PHASE_FINAL_ANSWER,
                    "msg_fixture_gap_final", "Gap-safe final.", 15u);
    }
    if (strcmp(prompt, "public_index_decrease") == 0) {
        if (snag_response_graph_add_public(
                graph, SNAG_ITEM_ASSISTANT, SNAG_PHASE_COMMENTARY,
                "msg_fixture_index_zero", "index zero") < 0 ||
            snag_response_graph_add_public(
                graph, SNAG_ITEM_ASSISTANT, SNAG_PHASE_FINAL_ANSWER,
                "msg_fixture_index_one", "index one") < 0 ||
            emit(opaque, 1u, SNAG_ITEM_ASSISTANT,
                 SNAG_PHASE_FINAL_ANSWER, graph->items[1u].provider_item_id, "index one", 9u) < 0 ||
            emit(opaque, 0u, SNAG_ITEM_ASSISTANT,
                 SNAG_PHASE_COMMENTARY, graph->items[0u].provider_item_id, "index zero", 10u) < 0)
            goto allocation;
        return 0;
    }
    if (strcmp(prompt, "terminal_paced_decode") == 0 ||
        strcmp(prompt, "terminal_paced_unicode") == 0) {
        bool unicode = strcmp(prompt, "terminal_paced_unicode") == 0;
        const char *full = unicode ?
            "Paced tokens form inter🌙́fragment and finish finalword" :
            "Paced tokens form interfragment and finish finalword";
        const char *fragments[] = {
            "Paced ", "tokens ", unicode ? "form inter🌙" : "form inter",
            unicode ? "́fragment " : "fragment ",
            "and finish ", "finalword"
        };

        if (cycle == 1u) {
            size_t index = graph->count;
            size_t fragment_count = sizeof(fragments) / sizeof(fragments[0]);

            if (snag_response_graph_add_public(
                    graph, SNAG_ITEM_ASSISTANT, SNAG_PHASE_COMMENTARY,
                    "msg_fixture_terminal_paced", full) < 0 ||
                add_call(graph, workspace, cycle, 1u, "fixture paced") < 0)
                goto allocation;
            for (size_t part = 0u; part < fragment_count; ++part) {
                if (emit(opaque, index, SNAG_ITEM_ASSISTANT,
                         SNAG_PHASE_COMMENTARY, graph->items[index].provider_item_id, fragments[part],
                         strlen(fragments[part])) < 0)
                    goto allocation;
                for (unsigned int wait = 0u;
                     wait < (part + 1u == fragment_count ? 60u :
                             part == 2u ? 20u : 4u);
                     ++wait) {
                    int pump_rc = pump(opaque, 20u);

                    if (pump_rc != 0)
                        return pump_rc;
                }
            }
            return 0;
        }
        for (unsigned int wait = 0u; wait < 60u; ++wait) {
            int pump_rc = pump(opaque, 20u);

            if (pump_rc != 0)
                return pump_rc;
        }
        return emit_public(graph, emit, opaque, SNAG_ITEM_ASSISTANT,
                           SNAG_PHASE_FINAL_ANSWER,
                           "msg_fixture_terminal_paced_final",
                           "paced complete", 0);
    }
    if (strcmp(prompt, "terminal_markdown") == 0) {
        static const char full[] =
            "# Stream **ready**\n"
            "- split `code` and [docs](https://example.test)\n"
            "```c\nint value = 1;\n```\n\n"
            "First prose line\ncontinued prose\n\n"
            "| Item | State | Count |\n"
            "| :--- | :---: | ---: |\n"
            "| alpha | `ready` | 7 |\n\n"
            "second paragraph\n\n"
            "> final quoted boundary";
        static const char *const fragments[] = {
            "# Stream **rea", "dy**\n- split `co", "de` and [docs](",
            "https://example.test)\n```", "c\nint value ",
            "= 1;\n```\n\nFirst prose", " line\ncontinued prose\n\n| Item |",
            " State | Count |\n| :--- | :---: |", " ---: |\n| alpha | `rea",
            "dy` | 7 |\n\nsecond paragraph",
            "\n\n> final quoted boundary"
        };
        size_t index = graph->count;

        if (snag_response_graph_add_public(
                graph, SNAG_ITEM_ASSISTANT, SNAG_PHASE_FINAL_ANSWER,
                "msg_fixture_terminal_markdown", full) < 0)
            goto allocation;
        for (size_t part = 0u;
             part < sizeof(fragments) / sizeof(fragments[0]); ++part) {
            if (emit(opaque, index, SNAG_ITEM_ASSISTANT,
                     SNAG_PHASE_FINAL_ANSWER, graph->items[index].provider_item_id, fragments[part],
                     strlen(fragments[part])) < 0)
                goto allocation;
            for (unsigned int wait = 0u; wait < 4u; ++wait) {
                int pump_rc = pump(opaque, 20u);
                if (pump_rc != 0)
                    return pump_rc;
            }
        }
        for (unsigned int wait = 0u; wait < 50u; ++wait) {
            int pump_rc = pump(opaque, 20u);
            if (pump_rc != 0)
                return pump_rc;
        }
        return 0;
    }
    if (strcmp(prompt, "typing_stream") == 0 ||
        strstr(prompt, "network_view_stream")) {
        static const char full[] =
            "model-output-one model-output-two model-output-three";
        static const char first[] = "model-output-one ";
        static const char second[] = "model-output-two ";
        static const char third[] = "model-output-three";
        size_t index = graph->count;

        if (snag_response_graph_add_public(
                graph, SNAG_ITEM_ASSISTANT, SNAG_PHASE_FINAL_ANSWER,
                "msg_fixture_typing_stream", full) < 0 ||
            emit(opaque, index, SNAG_ITEM_ASSISTANT, SNAG_PHASE_FINAL_ANSWER, graph->items[index].provider_item_id,
                 first, sizeof(first) - 1u) < 0)
            goto allocation;
        for (unsigned int part = 0u; part < 2u; ++part) {
            for (unsigned int i = 0u; i < 10u; ++i) {
                int pump_rc = pump(opaque, 20u);

                if (pump_rc != 0)
                    return pump_rc;
            }
            if (emit(opaque, index, SNAG_ITEM_ASSISTANT,
                     SNAG_PHASE_FINAL_ANSWER, graph->items[index].provider_item_id,
                     part == 0u ? second : third,
                     part == 0u ? sizeof(second) - 1u :
                                  sizeof(third) - 1u) < 0)
                goto allocation;
        }
        return 0;
    }
    if (strcmp(prompt, "one_shot_signal_wait") == 0) {
        if (emit_public(graph, emit, opaque, SNAG_ITEM_ASSISTANT,
                        SNAG_PHASE_COMMENTARY,
                        "msg_fixture_one_shot_signal_wait",
                        "waiting for shutdown\n", 0) < 0)
            goto allocation;
        for (unsigned int i = 0u; i < 100u; ++i) {
            int pump_rc;

            (void)poll(NULL, 0u, 20);
            pump_rc = pump(opaque, 0u);
            if (pump_rc != 0)
                return pump_rc;
        }
        return emit_public(graph, emit, opaque, SNAG_ITEM_ASSISTANT,
                           SNAG_PHASE_FINAL_ANSWER,
                           "msg_fixture_one_shot_signal_final",
                           "shutdown was not requested", 0);
    }
    if (strcmp(prompt, "slow") == 0 || strcmp(prompt, "slow_utf8") == 0 ||
        strcmp(prompt, "queue_slow") == 0 ||
        strcmp(prompt, "slow_resteer") == 0) {
        if (cycle == 1u) {
            if (strcmp(prompt, "slow_utf8") == 0) {
                static const char euro[] = "€";
                size_t index = graph->count;
                if (snag_response_graph_add_public(
                        graph, SNAG_ITEM_ASSISTANT, SNAG_PHASE_COMMENTARY,
                        "msg_fixture_slow_utf8_commentary", euro) < 0 ||
                    emit(opaque, index, SNAG_ITEM_ASSISTANT,
                         SNAG_PHASE_COMMENTARY, graph->items[index].provider_item_id, euro, 1u) < 0)
                    goto allocation;
            } else if (emit_public(
                           graph, emit, opaque, SNAG_ITEM_ASSISTANT,
                           SNAG_PHASE_COMMENTARY, "msg_fixture_slow_commentary",
                           "working slowly\n", 0) < 0) {
                goto allocation;
            }
            unsigned int waits = strcmp(prompt, "queue_slow") == 0 ? 500u : 100u;

            for (unsigned int i = 0; i < waits; ++i) {
                int pump_rc = pump(opaque, 20u);
                if (pump_rc != 0)
                    return pump_rc;
            }
            if (strcmp(prompt, "slow_utf8") == 0) {
                static const char euro[] = "€";
                if (emit(opaque, 0u, SNAG_ITEM_ASSISTANT,
                         SNAG_PHASE_COMMENTARY, graph->items[0u].provider_item_id, euro + 1u,
                         sizeof(euro) - 2u) < 0)
                    goto allocation;
            }
            return emit_public(graph, emit, opaque, SNAG_ITEM_ASSISTANT,
                               SNAG_PHASE_FINAL_ANSWER, "msg_fixture_slow_final",
                               "slow complete", 0);
        }
        if (strcmp(prompt, "slow_resteer") == 0) {
            if (cycle == 2u)
                for (unsigned int i = 0u; i < 100u; ++i) {
                    int pump_rc = pump(opaque, 20u);

                    if (pump_rc != 0)
                        return pump_rc;
                }
            return emit_public(graph, emit, opaque, SNAG_ITEM_ASSISTANT,
                               SNAG_PHASE_FINAL_ANSWER,
                               "msg_fixture_resteered_final",
                               "repeated steering complete", 0);
        }
        if (json_is_array(steering) && json_array_size(steering) != 0u) {
            json_t *last = json_array_get(steering,
                                          json_array_size(steering) - 1u);
            const char *text = snag_json_string(last, "text");
            char answer[1024];
            int written = text ? snprintf(answer, sizeof(answer),
                                          "steered: %s", text) : -1;
            if (written < 0 || (size_t)written >= sizeof(answer))
                goto allocation;
            return emit_public(graph, emit, opaque, SNAG_ITEM_ASSISTANT,
                               SNAG_PHASE_FINAL_ANSWER, "msg_fixture_steered_final",
                               answer, 0);
        }
    }
    if (strcmp(prompt, "commentary_only") == 0)
        return emit_public(graph, emit, opaque, SNAG_ITEM_ASSISTANT,
                           SNAG_PHASE_COMMENTARY, "msg_fixture_commentary",
                           "I am still working.", 0);
    if (strcmp(prompt, "final_plus_call") == 0) {
        if (emit_public(graph, emit, opaque, SNAG_ITEM_ASSISTANT,
                        SNAG_PHASE_FINAL_ANSWER, "msg_fixture_conflict",
                        "This must not complete.", 0) < 0 ||
            add_call(graph, workspace, cycle, 1u, "fixture ok") < 0)
            goto allocation;
        return 0;
    }
    if (strcmp(prompt, "refusal_plus_call") == 0) {
        if (emit_public(graph, emit, opaque, SNAG_ITEM_REFUSAL,
                        SNAG_PHASE_FINAL_ANSWER, "msg_fixture_refusal_conflict",
                        "I cannot do that.", 0) < 0 ||
            add_call(graph, workspace, cycle, 1u, "fixture ok") < 0)
            goto allocation;
        return 0;
    }
    if (strcmp(prompt, "tool_crash") == 0) {
        if (cycle == 1u)
            return add_call(graph, workspace, cycle, 0u, "fixture crash");
        return emit_public(graph, emit, opaque, SNAG_ITEM_ASSISTANT,
                           SNAG_PHASE_FINAL_ANSWER, "msg_fixture_crash_final",
                           "unexpected continuation", 0);
    }
    if (strcmp(prompt, "tool_only") == 0) {
        if (cycle == 1u)
            return add_call(graph, workspace, cycle, 0u, "fixture ok");
        return emit_public(graph, emit, opaque, SNAG_ITEM_ASSISTANT,
                           SNAG_PHASE_FINAL_ANSWER, "msg_fixture_tool_final",
                           "tool complete", 0);
    }
    if (strcmp(prompt, "text_tool") == 0) {
        if (cycle == 1u) {
            if (emit_public(graph, emit, opaque, SNAG_ITEM_ASSISTANT,
                            SNAG_PHASE_COMMENTARY, "msg_fixture_tool_commentary",
                            "Checking first.\n", 0) < 0 ||
                add_call(graph, workspace, cycle, 0u, "fixture ok") < 0)
                goto allocation;
            return 0;
        }
        return emit_public(graph, emit, opaque, SNAG_ITEM_ASSISTANT,
                           SNAG_PHASE_FINAL_ANSWER, "msg_fixture_text_tool_final",
                           "done", 0);
    }
    if (strcmp(prompt, "two_tools") == 0) {
        if (cycle == 1u) {
            if (add_call(graph, workspace, cycle, 0u, "fixture first") < 0 ||
                add_call(graph, workspace, cycle, 1u, "fixture second") < 0)
                goto allocation;
            return 0;
        }
        return emit_public(graph, emit, opaque, SNAG_ITEM_ASSISTANT,
                           SNAG_PHASE_FINAL_ANSWER, "msg_fixture_two_tools_final",
                           "two tools complete", 0);
    }
    if (strcmp(prompt, "many_cycles") == 0) {
        if (cycle <= 129u)
            return add_call(graph, workspace, cycle, 0u, "fixture loop");
        return emit_public(graph, emit, opaque, SNAG_ITEM_ASSISTANT,
                           SNAG_PHASE_FINAL_ANSWER, "msg_fixture_many_final",
                           "130th cycle complete", 0);
    }
    if (strcmp(prompt, "multi_item") == 0) {
        if (emit_public(graph, emit, opaque, SNAG_ITEM_ASSISTANT,
                        SNAG_PHASE_COMMENTARY, "msg_fixture_multi_commentary",
                        "Working.\n", 0) < 0 ||
            emit_public(graph, emit, opaque, SNAG_ITEM_ASSISTANT,
                        SNAG_PHASE_FINAL_ANSWER, "msg_fixture_multi_final",
                        "Done.", 0) < 0)
            goto allocation;
        return 0;
    }
    if (strcmp(prompt, "ping") == 0 || strcmp(prompt, "/ping") == 0)
        return emit_public(graph, emit, opaque, SNAG_ITEM_ASSISTANT,
                           SNAG_PHASE_FINAL_ANSWER, "msg_fixture_ping", "pong", 0);
    if (strcmp(prompt, "utf8") == 0)
        return emit_public(graph, emit, opaque, SNAG_ITEM_ASSISTANT,
                           SNAG_PHASE_FINAL_ANSWER, "msg_fixture_utf8", "€", 2);
    if (strcmp(prompt, "repeat") == 0)
        return emit_public(graph, emit, opaque, SNAG_ITEM_ASSISTANT,
                           SNAG_PHASE_FINAL_ANSWER, "msg_fixture_repeat", "haha", 1);
    if (strcmp(prompt, "refuse") == 0)
        return emit_public(graph, emit, opaque, SNAG_ITEM_REFUSAL,
                           SNAG_PHASE_FINAL_ANSWER, "msg_fixture_refusal",
                           "I can’t do that.", 0);
    return emit_public(graph, emit, opaque, SNAG_ITEM_ASSISTANT,
                       SNAG_PHASE_FINAL_ANSWER, "msg_fixture_default",
                       "fixture answer", 0);

allocation:
    if (error_size)
        (void)snprintf(error, error_size, "fixture allocation failed");
    return -1;
}

int
snag_fixture_tool(const struct snag_response_item *call,
                 snag_provider_pump_fn pump, void *pump_opaque,
                 json_t **result, char *error, size_t error_size)
{
    const char *command;
    const char *handle;

    if (!call || call->kind != SNAG_ITEM_TOOL_CALL) {
        if (error_size)
            (void)snprintf(error, error_size, "fixture received an invalid tool call");
        return -1;
    }
    if (strcmp(call->name, "write_stdin") == 0) {
        handle = snag_json_string(call->arguments, "handle");
        if (!handle || strcmp(handle, managed_handle) != 0) {
            *result = snag_tool_result_terminal(false,
                                               "fixture rejected wrong handle");
        } else if (!snag_json_string(call->arguments, "data")) {
            *result = running_result(
                "Process is still running; interaction was rejected.");
        } else {
            *result = snag_tool_result_terminal(true,
                                               "fixture process completed");
        }
        if (!*result)
            goto allocation;
        return 0;
    }
    if (strcmp(call->name, "exec_command") != 0 ||
        !(command = snag_json_string(call->arguments, "command"))) {
        if (error_size)
            (void)snprintf(error, error_size, "fixture received an invalid tool call");
        return -1;
    }
    if (strstr(command, "crash"))
        _exit(98);
    if (strstr(command, "managed steering wait") ||
        strstr(command, "managed queue wait")) {
        memcpy(managed_handle, call->call_id, sizeof(managed_handle));
        for (unsigned int i = 0u; i < 100u; ++i) {
            int pump_rc = pump ? pump(pump_opaque, 20u) : 0;

            if (pump_rc == 1) {
                *result = running_result_reason(
                    "Command is still running because steering arrived.",
                    "steering_handoff");
                return *result ? 0 : -1;
            }
            if (pump_rc != 0)
                return pump_rc;
        }
        *result = snag_tool_result_terminal(true,
                                           "fixture managed wait completed");
        return *result ? 0 : -1;
    }
    if (strstr(command, "managed start")) {
        memcpy(managed_handle, call->call_id, sizeof(managed_handle));
        *result = running_result("fixture process is still running");
        if (!*result)
            goto allocation;
        return 0;
    }
    if (strstr(command, "context anchor large output")) {
        char *text = malloc(96u * 1024u + 1u);

        if (!text)
            goto allocation;
        memset(text, 'x', 96u * 1024u);
        text[96u * 1024u] = '\0';
        *result = snag_tool_result_terminal(true, text);
        free(text);
        if (!*result)
            goto allocation;
        return 0;
    }
    *result = snag_tool_result_terminal(strstr(command, "fail") == NULL,
                                      strstr(command, "fail") == NULL ?
                                      "fixture command succeeded" :
                                      "fixture command failed");
    if (!*result) {
        if (error_size)
            (void)snprintf(error, error_size, "fixture result allocation failed");
        return -1;
    }
    return 0;

allocation:
    if (error_size)
        (void)snprintf(error, error_size, "fixture allocation failed");
    return -1;
}

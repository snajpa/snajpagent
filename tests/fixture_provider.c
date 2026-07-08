/* SPDX-License-Identifier: GPL-2.0-only */
#include "base.h"
#include "json.h"
#include "turn.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef int (*fixture_emit_fn)(void *opaque, size_t item_index,
                               enum snj_item_kind kind,
                               enum snj_item_phase phase,
                               const char *text, size_t len);
typedef int (*fixture_pump_fn)(void *opaque, unsigned int timeout_ms);

static int
set_response_id(struct snj_response_graph *graph, unsigned int cycle,
                const char *suffix)
{
    char id[128];
    if (snprintf(id, sizeof(id), "resp_fixture_%u_%s", cycle, suffix) < 0)
        return -1;
    return snj_response_graph_set_provider_id(graph, id);
}

static int
emit_public(struct snj_response_graph *graph, fixture_emit_fn emit, void *opaque,
            enum snj_item_kind kind, enum snj_item_phase phase,
            const char *provider_id, const char *text, int pattern)
{
    size_t index = graph->count;
    size_t len = strlen(text);
    size_t split = len / 2u;

    if (snj_response_graph_add_public(graph, kind, phase, provider_id, text) < 0)
        return -1;
    if (pattern == 1)
        return emit(opaque, index, kind, phase, "ha", 2u) < 0 ||
               emit(opaque, index, kind, phase, "ha", 2u) < 0 ? -1 : 0;
    if (pattern == 2)
        return emit(opaque, index, kind, phase, text, 1u) < 0 ||
               emit(opaque, index, kind, phase, text + 1u, len - 1u) < 0 ? -1 : 0;
    if ((split && emit(opaque, index, kind, phase, text, split) < 0) ||
        emit(opaque, index, kind, phase, text + split, len - split) < 0)
        return -1;
    return 0;
}

static json_t *
exec_arguments(const char *workspace, const char *command)
{
    json_t *args = json_object();
    if (!args ||
        snj_json_set_new(args, "command", json_string(command)) < 0 ||
        snj_json_set_new(args, "pty", json_false()) < 0 ||
        snj_json_set_new(args, "stdin", json_null()) < 0 ||
        snj_json_set_new(args, "timeout_ms", json_integer(1000)) < 0 ||
        snj_json_set_new(args, "workdir", json_string(workspace)) < 0 ||
        snj_json_set_new(args, "yield_ms", json_integer(1000)) < 0) {
        if (args)
            json_decref(args);
        return NULL;
    }
    return args;
}

static int
add_call(struct snj_response_graph *graph, const char *workspace,
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
    return snj_response_graph_add_call(graph, item_id, call_id,
                                       "exec_command", args);
}

int
snj_fixture_response(const char *prompt, const json_t *steering,
                     const char *workspace, unsigned int cycle,
                     fixture_emit_fn emit, fixture_pump_fn pump, void *opaque,
                     struct snj_response_graph *graph,
                     char *error, size_t error_size)
{
    if (strcmp(prompt, "crash") == 0 && cycle == 1u)
        _exit(99);
    if (strcmp(prompt, "provider_fail") == 0 && cycle == 1u) {
        if (error_size)
            (void)snprintf(error, error_size, "fixture provider failed");
        return -1;
    }
    if (set_response_id(graph, cycle, "complete") < 0)
        goto allocation;
    if (strcmp(prompt, "empty") == 0)
        return 0;
    if (strcmp(prompt, "slow") == 0 || strcmp(prompt, "slow_utf8") == 0) {
        if (cycle == 1u) {
            if (strcmp(prompt, "slow_utf8") == 0) {
                static const char euro[] = "€";
                size_t index = graph->count;
                if (snj_response_graph_add_public(
                        graph, SNJ_ITEM_ASSISTANT, SNJ_PHASE_COMMENTARY,
                        "msg_fixture_slow_utf8_commentary", euro) < 0 ||
                    emit(opaque, index, SNJ_ITEM_ASSISTANT,
                         SNJ_PHASE_COMMENTARY, euro, 1u) < 0)
                    goto allocation;
            } else if (emit_public(
                           graph, emit, opaque, SNJ_ITEM_ASSISTANT,
                           SNJ_PHASE_COMMENTARY, "msg_fixture_slow_commentary",
                           "working slowly\n", 0) < 0) {
                goto allocation;
            }
            for (unsigned int i = 0; i < 100u; ++i) {
                int pump_rc = pump(opaque, 20u);
                if (pump_rc != 0)
                    return pump_rc;
            }
            if (strcmp(prompt, "slow_utf8") == 0) {
                static const char euro[] = "€";
                if (emit(opaque, 0u, SNJ_ITEM_ASSISTANT,
                         SNJ_PHASE_COMMENTARY, euro + 1u,
                         sizeof(euro) - 2u) < 0)
                    goto allocation;
            }
            return emit_public(graph, emit, opaque, SNJ_ITEM_ASSISTANT,
                               SNJ_PHASE_FINAL_ANSWER, "msg_fixture_slow_final",
                               "slow complete", 0);
        }
        if (json_is_array(steering) && json_array_size(steering) != 0u) {
            json_t *last = json_array_get(steering,
                                          json_array_size(steering) - 1u);
            const char *text = snj_json_string(last, "text");
            char answer[1024];
            int written = text ? snprintf(answer, sizeof(answer),
                                          "steered: %s", text) : -1;
            if (written < 0 || (size_t)written >= sizeof(answer))
                goto allocation;
            return emit_public(graph, emit, opaque, SNJ_ITEM_ASSISTANT,
                               SNJ_PHASE_FINAL_ANSWER, "msg_fixture_steered_final",
                               answer, 0);
        }
    }
    if (strcmp(prompt, "commentary_only") == 0)
        return emit_public(graph, emit, opaque, SNJ_ITEM_ASSISTANT,
                           SNJ_PHASE_COMMENTARY, "msg_fixture_commentary",
                           "I am still working.", 0);
    if (strcmp(prompt, "summary_only") == 0)
        return emit_public(graph, emit, opaque, SNJ_ITEM_REASONING_SUMMARY,
                           SNJ_PHASE_SUMMARY, "sum_fixture_only",
                           "Internal progress summary.", 0);
    if (strcmp(prompt, "final_plus_call") == 0) {
        if (emit_public(graph, emit, opaque, SNJ_ITEM_ASSISTANT,
                        SNJ_PHASE_FINAL_ANSWER, "msg_fixture_conflict",
                        "This must not complete.", 0) < 0 ||
            add_call(graph, workspace, cycle, 1u, "fixture ok") < 0)
            goto allocation;
        return 0;
    }
    if (strcmp(prompt, "refusal_plus_call") == 0) {
        if (emit_public(graph, emit, opaque, SNJ_ITEM_REFUSAL,
                        SNJ_PHASE_FINAL_ANSWER, "msg_fixture_refusal_conflict",
                        "I cannot do that.", 0) < 0 ||
            add_call(graph, workspace, cycle, 1u, "fixture ok") < 0)
            goto allocation;
        return 0;
    }
    if (strcmp(prompt, "tool_crash") == 0) {
        if (cycle == 1u)
            return add_call(graph, workspace, cycle, 0u, "fixture crash");
        return emit_public(graph, emit, opaque, SNJ_ITEM_ASSISTANT,
                           SNJ_PHASE_FINAL_ANSWER, "msg_fixture_crash_final",
                           "unexpected continuation", 0);
    }
    if (strcmp(prompt, "tool_only") == 0) {
        if (cycle == 1u)
            return add_call(graph, workspace, cycle, 0u, "fixture ok");
        return emit_public(graph, emit, opaque, SNJ_ITEM_ASSISTANT,
                           SNJ_PHASE_FINAL_ANSWER, "msg_fixture_tool_final",
                           "tool complete", 0);
    }
    if (strcmp(prompt, "text_tool") == 0) {
        if (cycle == 1u) {
            if (emit_public(graph, emit, opaque, SNJ_ITEM_ASSISTANT,
                            SNJ_PHASE_COMMENTARY, "msg_fixture_tool_commentary",
                            "Checking first.\n", 0) < 0 ||
                add_call(graph, workspace, cycle, 0u, "fixture ok") < 0)
                goto allocation;
            return 0;
        }
        return emit_public(graph, emit, opaque, SNJ_ITEM_ASSISTANT,
                           SNJ_PHASE_FINAL_ANSWER, "msg_fixture_text_tool_final",
                           "done", 0);
    }
    if (strcmp(prompt, "two_tools") == 0) {
        if (cycle == 1u) {
            if (add_call(graph, workspace, cycle, 0u, "fixture first") < 0 ||
                add_call(graph, workspace, cycle, 1u, "fixture second") < 0)
                goto allocation;
            return 0;
        }
        return emit_public(graph, emit, opaque, SNJ_ITEM_ASSISTANT,
                           SNJ_PHASE_FINAL_ANSWER, "msg_fixture_two_tools_final",
                           "two tools complete", 0);
    }
    if (strcmp(prompt, "many_cycles") == 0) {
        if (cycle <= 3u)
            return add_call(graph, workspace, cycle, 0u, "fixture loop");
        return emit_public(graph, emit, opaque, SNJ_ITEM_ASSISTANT,
                           SNJ_PHASE_FINAL_ANSWER, "msg_fixture_many_final",
                           "fourth cycle complete", 0);
    }
    if (strcmp(prompt, "multi_item") == 0) {
        if (emit_public(graph, emit, opaque, SNJ_ITEM_ASSISTANT,
                        SNJ_PHASE_COMMENTARY, "msg_fixture_multi_commentary",
                        "Working.\n", 0) < 0 ||
            emit_public(graph, emit, opaque, SNJ_ITEM_REASONING_SUMMARY,
                        SNJ_PHASE_SUMMARY, "sum_fixture_multi",
                        "Checked the fixture.", 0) < 0 ||
            emit_public(graph, emit, opaque, SNJ_ITEM_ASSISTANT,
                        SNJ_PHASE_FINAL_ANSWER, "msg_fixture_multi_final",
                        "Done.", 0) < 0)
            goto allocation;
        return 0;
    }
    if (strcmp(prompt, "ping") == 0 || strcmp(prompt, "/ping") == 0)
        return emit_public(graph, emit, opaque, SNJ_ITEM_ASSISTANT,
                           SNJ_PHASE_FINAL_ANSWER, "msg_fixture_ping", "pong", 0);
    if (strcmp(prompt, "utf8") == 0)
        return emit_public(graph, emit, opaque, SNJ_ITEM_ASSISTANT,
                           SNJ_PHASE_FINAL_ANSWER, "msg_fixture_utf8", "€", 2);
    if (strcmp(prompt, "repeat") == 0)
        return emit_public(graph, emit, opaque, SNJ_ITEM_ASSISTANT,
                           SNJ_PHASE_FINAL_ANSWER, "msg_fixture_repeat", "haha", 1);
    if (strcmp(prompt, "refuse") == 0)
        return emit_public(graph, emit, opaque, SNJ_ITEM_REFUSAL,
                           SNJ_PHASE_FINAL_ANSWER, "msg_fixture_refusal",
                           "I can’t do that.", 0);
    return emit_public(graph, emit, opaque, SNJ_ITEM_ASSISTANT,
                       SNJ_PHASE_FINAL_ANSWER, "msg_fixture_default",
                       "fixture answer", 0);

allocation:
    if (error_size)
        (void)snprintf(error, error_size, "fixture allocation failed");
    return -1;
}

int
snj_fixture_tool(const struct snj_response_item *call, json_t **result,
                 char *error, size_t error_size)
{
    const char *command;
    if (!call || call->kind != SNJ_ITEM_TOOL_CALL ||
        strcmp(call->name, "exec_command") != 0 ||
        !(command = snj_json_string(call->arguments, "command"))) {
        if (error_size)
            (void)snprintf(error, error_size, "fixture received an invalid tool call");
        return -1;
    }
    if (strstr(command, "crash"))
        _exit(98);
    *result = snj_tool_result_terminal(strstr(command, "fail") == NULL,
                                      strstr(command, "fail") == NULL ?
                                      "fixture command succeeded" :
                                      "fixture command failed");
    if (!*result) {
        if (error_size)
            (void)snprintf(error, error_size, "fixture result allocation failed");
        return -1;
    }
    return 0;
}

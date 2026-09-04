/* SPDX-License-Identifier: GPL-2.0-only */
#include "base.h"
#include "json.h"
#include "store.h"
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

static const char managed_handle[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static const char wrong_managed_handle[] = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

static json_t *
empty_excerpt(void)
{
    json_t *out = json_object();

    if (!out ||
        snj_json_set_new(out, "discarded_bytes", json_integer(0)) < 0 ||
        snj_json_set_new(out, "encoding", json_string("utf8")) < 0 ||
        snj_json_set_new(out, "original_bytes", json_integer(0)) < 0 ||
        snj_json_set_new(out, "retained", json_string("")) < 0 ||
        snj_json_set_new(out, "retained_bytes", json_integer(0)) < 0) {
        if (out)
            json_decref(out);
        return NULL;
    }
    return out;
}

static json_t *
running_result(const char *text)
{
    json_t *result = json_object();

    if (!result ||
        snj_json_set_new(result, "duration_ms", json_integer(0)) < 0 ||
        snj_json_set_new(result, "exit_code", json_null()) < 0 ||
        snj_json_set_new(result, "handle", json_string(managed_handle)) < 0 ||
        snj_json_set_new(result, "model_text", json_string(text)) < 0 ||
        snj_json_set_new(result, "reason", json_null()) < 0 ||
        snj_json_set_new(result, "signal", json_null()) < 0 ||
        snj_json_set_new(result, "status", json_string("running")) < 0 ||
        snj_json_set_new(result, "stderr", empty_excerpt()) < 0 ||
        snj_json_set_new(result, "stdout", empty_excerpt()) < 0) {
        if (result)
            json_decref(result);
        return NULL;
    }
    return result;
}

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

static int
add_stdin_call(struct snj_response_graph *graph, unsigned int cycle,
               unsigned int index, const char *handle, bool malformed)
{
    char item_id[128];
    char call_id[128];
    json_t *args = json_object();

    if (!args ||
        snj_json_set_new(args, "handle", json_string(handle)) < 0 ||
        (!malformed &&
         snj_json_set_new(args, "data", json_string("")) < 0) ||
        snj_json_set_new(args, "eof", json_null()) < 0 ||
        snj_json_set_new(args, "yield_ms", json_integer(0)) < 0 ||
        snprintf(item_id, sizeof(item_id), "item_fixture_%u_%u", cycle,
                 index) < 0 ||
        snprintf(call_id, sizeof(call_id), "call_fixture_%u_%u", cycle,
                 index) < 0) {
        if (args)
            json_decref(args);
        return -1;
    }
    return snj_response_graph_add_call(graph, item_id, call_id,
                                       "write_stdin", args);
}

static int
add_goal_call(struct snj_response_graph *graph, unsigned int cycle,
              const char *action, const char *text)
{
    char item_id[128];
    char call_id[128];
    json_t *args = json_object();

    if (!args ||
        snj_json_set_new(args, "action", json_string(action)) < 0 ||
        snj_json_set_new(args, "text",
                         text ? json_string(text) : json_null()) < 0 ||
        snprintf(item_id, sizeof(item_id),
                 "item_fixture_goal_%u", cycle) < 0 ||
        snprintf(call_id, sizeof(call_id),
                 "call_fixture_goal_%u", cycle) < 0) {
        if (args)
            json_decref(args);
        return -1;
    }
    return snj_response_graph_add_call(graph, item_id, call_id,
                                       "update_goal", args);
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
snj_fixture_response(const char *prompt, const json_t *steering,
                     const char *workspace, unsigned int cycle,
                     const char *goal_prompt, uint64_t goal_turn_count,
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
    if (strcmp(prompt, SNJ_GOAL_CONTINUATION_TEXT) == 0) {
        if (!goal_prompt)
            goto allocation;
        if (strcmp(goal_prompt, "failing goal") == 0 && cycle == 1u) {
            if (error_size)
                (void)snprintf(error, error_size,
                               "fixture goal provider failed");
            return -1;
        }
        if (strcmp(goal_prompt, "refusing goal") == 0)
            return emit_public(graph, emit, opaque, SNJ_ITEM_REFUSAL,
                               SNJ_PHASE_FINAL_ANSWER,
                               "msg_fixture_goal_refusal",
                               "I cannot continue this goal.", 0);
        if (strcmp(goal_prompt, "slow goal") == 0 &&
            goal_turn_count == 1u) {
            if (cycle == 1u) {
                if (emit_public(graph, emit, opaque, SNJ_ITEM_ASSISTANT,
                                SNJ_PHASE_COMMENTARY,
                                "msg_fixture_goal_slow_commentary",
                                "working on goal\n", 0) < 0)
                    goto allocation;
                for (unsigned int i = 0; i < 100u; ++i) {
                    int pump_rc = pump(opaque, 20u);
                    if (pump_rc != 0)
                        return pump_rc;
                }
            }
            return emit_public(graph, emit, opaque, SNJ_ITEM_ASSISTANT,
                               SNJ_PHASE_FINAL_ANSWER,
                               "msg_fixture_goal_checkpoint",
                               "goal checkpoint", 0);
        }
        if (strcmp(goal_prompt, "automatic goal") == 0 &&
            goal_turn_count == 1u)
            return emit_public(graph, emit, opaque, SNJ_ITEM_ASSISTANT,
                               SNJ_PHASE_FINAL_ANSWER,
                               "msg_fixture_goal_checkpoint",
                               "goal checkpoint", 0);
        if (strcmp(goal_prompt, "rewrite goal") == 0 && cycle == 1u)
            return add_goal_call(graph, cycle, "rewrite", "rewritten goal");
        if (strcmp(goal_prompt, "rewritten goal") == 0 && cycle == 2u)
            return add_goal_call(graph, cycle, "complete", NULL);
        if (strcmp(goal_prompt, "tiny") == 0 && cycle == 1u)
            return add_goal_call(graph, cycle, "rewrite", "too long");
        if (strcmp(goal_prompt, "locked goal") == 0 && cycle == 1u) {
            if (emit_public(graph, emit, opaque, SNJ_ITEM_ASSISTANT,
                            SNJ_PHASE_COMMENTARY,
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
        if (cycle == 1u ||
            ((strcmp(goal_prompt, "locked goal") == 0 ||
              strcmp(goal_prompt, "tiny") == 0) && cycle == 2u))
            return add_goal_call(graph, cycle, "complete", NULL);
        return emit_public(graph, emit, opaque, SNJ_ITEM_ASSISTANT,
                           SNJ_PHASE_FINAL_ANSWER,
                           "msg_fixture_goal_done", "goal done", 0);
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
                return emit_public(graph, emit, opaque, SNJ_ITEM_ASSISTANT,
                                   SNJ_PHASE_FINAL_ANSWER,
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
        return emit_public(graph, emit, opaque, SNJ_ITEM_ASSISTANT,
                           SNJ_PHASE_FINAL_ANSWER,
                           "msg_fixture_managed_recovered",
                           "managed process recovered", 0);
    }
    if (strcmp(prompt, "empty") == 0)
        return 0;
    if (strcmp(prompt, "terminal_render") == 0) {
        static const char first[] =
            "alpha beta gamma delta epsilon ";
        static const char second_prefix[] =
            "zeta eta theta\nexplicit café ";
        static const char euro_first[] = "\xe2";
        static const char second_suffix[] = "\x82\xac line\n";
        static const char third[] =
            "supercalifragilisticexpialidocious0123456789ABCDEFGHIJ "
            "tail control:\x1b[31m";
        static const char full[] =
            "alpha beta gamma delta epsilon "
            "zeta eta theta\nexplicit café € line\n"
            "supercalifragilisticexpialidocious0123456789ABCDEFGHIJ "
            "tail control:\x1b[31m";
        size_t index = graph->count;

        if (snj_response_graph_add_public(
                graph, SNJ_ITEM_ASSISTANT, SNJ_PHASE_FINAL_ANSWER,
                "msg_fixture_terminal_render", full) < 0 ||
            emit(opaque, index, SNJ_ITEM_ASSISTANT, SNJ_PHASE_FINAL_ANSWER,
                 first, sizeof(first) - 1u) < 0)
            goto allocation;
        for (unsigned int i = 0u; i < 50u; ++i) {
            int pump_rc = pump(opaque, 20u);

            if (pump_rc != 0)
                return pump_rc;
        }
        if (emit(opaque, index, SNJ_ITEM_ASSISTANT, SNJ_PHASE_FINAL_ANSWER,
                 second_prefix, sizeof(second_prefix) - 1u) < 0 ||
            emit(opaque, index, SNJ_ITEM_ASSISTANT, SNJ_PHASE_FINAL_ANSWER,
                 euro_first, sizeof(euro_first) - 1u) < 0 ||
            emit(opaque, index, SNJ_ITEM_ASSISTANT, SNJ_PHASE_FINAL_ANSWER,
                 second_suffix, sizeof(second_suffix) - 1u) < 0)
            goto allocation;
        for (unsigned int i = 0u; i < 25u; ++i) {
            int pump_rc = pump(opaque, 20u);

            if (pump_rc != 0)
                return pump_rc;
        }
        if (emit(opaque, index, SNJ_ITEM_ASSISTANT, SNJ_PHASE_FINAL_ANSWER,
                 third, sizeof(third) - 1u) < 0)
            goto allocation;
        return 0;
    }
    if (strcmp(prompt, "terminal_status") == 0) {
        static const char full[] =
            "status-first-fragment status-second-fragment";
        static const char first[] = "status-first-fragment ";
        static const char second[] = "status-second-fragment";
        size_t index = graph->count;

        if (snj_response_graph_add_public(
                graph, SNJ_ITEM_ASSISTANT, SNJ_PHASE_FINAL_ANSWER,
                "msg_fixture_terminal_status", full) < 0)
            goto allocation;
        for (unsigned int i = 0u; i < 50u; ++i) {
            int pump_rc = pump(opaque, 20u);

            if (pump_rc != 0)
                return pump_rc;
        }
        if (emit(opaque, index, SNJ_ITEM_ASSISTANT, SNJ_PHASE_FINAL_ANSWER,
                 first, sizeof(first) - 1u) < 0)
            goto allocation;
        for (unsigned int i = 0u; i < 60u; ++i) {
            int pump_rc = pump(opaque, 20u);

            if (pump_rc != 0)
                return pump_rc;
        }
        if (emit(opaque, index, SNJ_ITEM_ASSISTANT, SNJ_PHASE_FINAL_ANSWER,
                 second, sizeof(second) - 1u) < 0)
            goto allocation;
        return 0;
    }
    if (strcmp(prompt, "terminal_paced_decode") == 0) {
        static const char full[] =
            "Paced tokens form interfragment and finish finalword";
        static const char *const fragments[] = {
            "Paced ", "tokens ", "form inter", "fragment ",
            "and finish ", "finalword"
        };

        if (cycle == 1u) {
            size_t index = graph->count;
            size_t fragment_count = sizeof(fragments) / sizeof(fragments[0]);

            if (snj_response_graph_add_public(
                    graph, SNJ_ITEM_ASSISTANT, SNJ_PHASE_COMMENTARY,
                    "msg_fixture_terminal_paced", full) < 0 ||
                add_call(graph, workspace, cycle, 1u, "fixture paced") < 0)
                goto allocation;
            for (size_t part = 0u; part < fragment_count; ++part) {
                if (emit(opaque, index, SNJ_ITEM_ASSISTANT,
                         SNJ_PHASE_COMMENTARY, fragments[part],
                         strlen(fragments[part])) < 0)
                    goto allocation;
                for (unsigned int wait = 0u;
                     wait < (part + 1u == fragment_count ? 60u : 4u);
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
        return emit_public(graph, emit, opaque, SNJ_ITEM_ASSISTANT,
                           SNJ_PHASE_FINAL_ANSWER,
                           "msg_fixture_terminal_paced_final",
                           "paced complete", 0);
    }
    if (strcmp(prompt, "typing_stream") == 0) {
        static const char full[] =
            "model-output-one model-output-two model-output-three";
        static const char first[] = "model-output-one ";
        static const char second[] = "model-output-two ";
        static const char third[] = "model-output-three";
        size_t index = graph->count;

        if (snj_response_graph_add_public(
                graph, SNJ_ITEM_ASSISTANT, SNJ_PHASE_FINAL_ANSWER,
                "msg_fixture_typing_stream", full) < 0 ||
            emit(opaque, index, SNJ_ITEM_ASSISTANT, SNJ_PHASE_FINAL_ANSWER,
                 first, sizeof(first) - 1u) < 0)
            goto allocation;
        for (unsigned int part = 0u; part < 2u; ++part) {
            for (unsigned int i = 0u; i < 10u; ++i) {
                int pump_rc = pump(opaque, 20u);

                if (pump_rc != 0)
                    return pump_rc;
            }
            if (emit(opaque, index, SNJ_ITEM_ASSISTANT,
                     SNJ_PHASE_FINAL_ANSWER,
                     part == 0u ? second : third,
                     part == 0u ? sizeof(second) - 1u :
                                  sizeof(third) - 1u) < 0)
                goto allocation;
        }
        return 0;
    }
    if (strcmp(prompt, "slow") == 0 || strcmp(prompt, "slow_utf8") == 0 ||
        strcmp(prompt, "queue_slow") == 0) {
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
            unsigned int waits = strcmp(prompt, "queue_slow") == 0 ? 500u : 100u;

            for (unsigned int i = 0; i < waits; ++i) {
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
        if (cycle <= 129u)
            return add_call(graph, workspace, cycle, 0u, "fixture loop");
        return emit_public(graph, emit, opaque, SNJ_ITEM_ASSISTANT,
                           SNJ_PHASE_FINAL_ANSWER, "msg_fixture_many_final",
                           "130th cycle complete", 0);
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
    const char *handle;

    if (!call || call->kind != SNJ_ITEM_TOOL_CALL) {
        if (error_size)
            (void)snprintf(error, error_size, "fixture received an invalid tool call");
        return -1;
    }
    if (strcmp(call->name, "write_stdin") == 0) {
        handle = snj_json_string(call->arguments, "handle");
        if (!handle || strcmp(handle, managed_handle) != 0) {
            *result = snj_tool_result_terminal(false,
                                               "fixture rejected wrong handle");
        } else if (!snj_json_string(call->arguments, "data")) {
            *result = running_result(
                "Process is still running; interaction was rejected.");
        } else {
            *result = snj_tool_result_terminal(true,
                                               "fixture process completed");
        }
        if (!*result)
            goto allocation;
        return 0;
    }
    if (strcmp(call->name, "exec_command") != 0 ||
        !(command = snj_json_string(call->arguments, "command"))) {
        if (error_size)
            (void)snprintf(error, error_size, "fixture received an invalid tool call");
        return -1;
    }
    if (strstr(command, "crash"))
        _exit(98);
    if (strstr(command, "managed start")) {
        *result = running_result("fixture process is still running");
        if (!*result)
            goto allocation;
        return 0;
    }
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

allocation:
    if (error_size)
        (void)snprintf(error, error_size, "fixture allocation failed");
    return -1;
}

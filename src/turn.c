/* SPDX-License-Identifier: GPL-2.0-only */
#include "turn.h"
#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *
snag_prompt_parse(const char *text, bool *read_only)
{
    *read_only = strncmp(text, "/ro", 3u) == 0 &&
        (!text[3] || isspace((unsigned char)text[3]));
    if (*read_only) {
        text += 3u;
        while (isspace((unsigned char)*text))
            ++text;
    } else if (text[0] == '/' && text[1] == '/') {
        ++text;
    }
    return text;
}

bool
snag_read_only_tool(const char *name)
{
    return name && (strcmp(name, "list_files") == 0 ||
                    strcmp(name, "read_file") == 0 ||
                    strcmp(name, "grep") == 0);
}

const char *
snag_item_kind_name(enum snag_item_kind kind)
{
    switch (kind) {
    case SNAG_ITEM_ASSISTANT: return "assistant";
    case SNAG_ITEM_REFUSAL: return "refusal";
    case SNAG_ITEM_TOOL_CALL: return "tool_call";
    }
    return NULL;
}

uint64_t
snag_capacity_safety_ceiling(uint64_t context_limit_tokens,
                            uint64_t requested_input_tokens,
                            uint64_t requested_output_tokens)
{
    uint64_t ceiling = 0u;

    if (context_limit_tokens)
        ceiling = context_limit_tokens > requested_output_tokens ?
            context_limit_tokens - requested_output_tokens : 1u;
    if (requested_input_tokens > 1u &&
        (!ceiling || requested_input_tokens - 1u < ceiling))
        ceiling = requested_input_tokens - 1u;
    return ceiling;
}

const char *
snag_item_phase_name(enum snag_item_phase phase)
{
    switch (phase) {
    case SNAG_PHASE_COMMENTARY: return "commentary";
    case SNAG_PHASE_FINAL_ANSWER: return "final_answer";
    case SNAG_PHASE_NONE: break;
    }
    return NULL;
}

static bool
provider_id_valid(const char *s)
{
    size_t len;
    if (!s || !*s)
        return false;
    len = strlen(s);
    if (len > SNAG_MAX_PROVIDER_ID ||
        !snag_utf8_valid((const unsigned char *)s, len, true))
        return false;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20u || c == 0x7fu ||
            (c == 0xc2u && i + 1u < len &&
             (unsigned char)s[i + 1u] >= 0x80u &&
             (unsigned char)s[i + 1u] <= 0x9fu))
            return false;
    }
    return true;
}

static bool
text_valid(const char *s, size_t max)
{
    size_t len;
    if (!s || !*s)
        return false;
    len = strlen(s);
    return len <= max && snag_utf8_valid((const unsigned char *)s, len, true);
}

static bool
public_kind(enum snag_item_kind kind)
{
    return kind == SNAG_ITEM_ASSISTANT || kind == SNAG_ITEM_REFUSAL;
}

struct snag_response_item
snag_response_graph_item(const struct snag_response_graph *graph, size_t index)
{
    json_t *value = json_array_get(graph->items, index);
    const char *kind = snag_json_string(value, "kind");
    const char *phase = snag_json_string(value, "phase");
    const char *local = snag_json_string(value, "local_item_id");
    const char *call = snag_json_string(value, "call_id");
    struct snag_response_item item = {
        .kind = kind && !strcmp(kind, "assistant") ? SNAG_ITEM_ASSISTANT :
                kind && !strcmp(kind, "refusal") ? SNAG_ITEM_REFUSAL : SNAG_ITEM_TOOL_CALL,
        .phase = phase && !strcmp(phase, "commentary") ? SNAG_PHASE_COMMENTARY :
                 phase && !strcmp(phase, "final_answer") ? SNAG_PHASE_FINAL_ANSWER : SNAG_PHASE_NONE,
        .provider_item_id = (char *)snag_json_string(value, "provider_item_id"),
        .provider_call_id = (char *)snag_json_string(value, "provider_call_id"),
        .name = (char *)snag_json_string(value, "name"),
        .text = (char *)snag_json_string(value, "text"),
        .arguments = json_object_get(value, "arguments")
    };
    if (local && snag_hex_is_lower(local, SNAG_ID_HEX_LEN))
        memcpy(item.local_item_id, local, sizeof(item.local_item_id));
    if (call && snag_hex_is_lower(call, SNAG_ID_HEX_LEN))
        memcpy(item.call_id, call, sizeof(item.call_id));
    return item;
}

static int
nullable_usage_member(const json_t *object, const char *key,
                      uint64_t *number, bool *known)
{
    json_t *value = json_object_get(object, key);

    if (json_is_null(value)) {
        *number = 0u;
        *known = false;
        return 0;
    }
    if (!json_is_integer(value) || json_integer_value(value) < 0)
        return -1;
    *number = (uint64_t)json_integer_value(value);
    *known = true;
    return 0;
}

int
snag_response_usage_valid(const struct snag_response_usage *usage)
{
    uint64_t sum;

    if (!usage)
        return snag_errno(EINVAL);
    if ((usage->input_known && usage->input_tokens > (uint64_t)LLONG_MAX) ||
        (usage->output_known && usage->output_tokens > (uint64_t)LLONG_MAX) ||
        (usage->reasoning_known && usage->reasoning_tokens > (uint64_t)LLONG_MAX) ||
        (usage->total_known && usage->total_tokens > (uint64_t)LLONG_MAX))
        return snag_errno(EOVERFLOW);
    if (usage->reasoning_known && usage->output_known &&
        usage->reasoning_tokens > usage->output_tokens)
        return snag_errno(EINVAL);
    if (usage->input_known && usage->output_known && usage->total_known) {
        if (usage->input_tokens > UINT64_MAX - usage->output_tokens)
            return snag_errno(EOVERFLOW);
        sum = usage->input_tokens + usage->output_tokens;
        if (usage->total_tokens != sum)
            return snag_errno(EINVAL);
    }
    return 0;
}

json_t *
snag_response_usage_json(const struct snag_response_usage *usage)
{
    if (snag_response_usage_valid(usage) < 0)
        return NULL;
    return json_pack("{s:o,s:o,s:o,s:o}",
        "input_tokens", usage->input_known ?
            json_integer((json_int_t)usage->input_tokens) : json_null(),
        "output_tokens", usage->output_known ?
            json_integer((json_int_t)usage->output_tokens) : json_null(),
        "reasoning_tokens", usage->reasoning_known ?
            json_integer((json_int_t)usage->reasoning_tokens) : json_null(),
        "total_tokens", usage->total_known ?
            json_integer((json_int_t)usage->total_tokens) : json_null());
}

int
snag_response_usage_from_json(const json_t *value,
                             struct snag_response_usage *usage)
{
    static const char *const keys[] = {
        "input_tokens", "output_tokens", "reasoning_tokens", "total_tokens"
    };
    struct snag_response_usage parsed;

    memset(&parsed, 0, sizeof(parsed));
    if (!usage || !snag_json_exact_keys(value, keys, 4u) ||
        nullable_usage_member(value, "input_tokens", &parsed.input_tokens,
                              &parsed.input_known) < 0 ||
        nullable_usage_member(value, "output_tokens", &parsed.output_tokens,
                              &parsed.output_known) < 0 ||
        nullable_usage_member(value, "reasoning_tokens", &parsed.reasoning_tokens,
                              &parsed.reasoning_known) < 0 ||
        nullable_usage_member(value, "total_tokens", &parsed.total_tokens,
                              &parsed.total_known) < 0 ||
        snag_response_usage_valid(&parsed) < 0)
        return snag_errno(EINVAL);
    *usage = parsed;
    return 0;
}

void
snag_response_graph_init(struct snag_response_graph *graph)
{
    memset(graph, 0, sizeof(*graph));
}

void
snag_response_graph_free(struct snag_response_graph *graph)
{
    json_decref(graph->items);
    free(graph->provider_response_id);
    snag_response_graph_init(graph);
}

int
snag_response_graph_set_provider_id(struct snag_response_graph *graph,
                                   const char *provider_response_id)
{
    char *copy;
    if (!provider_id_valid(provider_response_id))
        return snag_errno(EINVAL);
    copy = snag_strdup_checked(provider_response_id, SNAG_MAX_PROVIDER_ID);
    if (!copy)
        return -1;
    free(graph->provider_response_id);
    graph->provider_response_id = copy;
    return 0;
}

static bool
tool_name_valid(const char *name)
{
    return snag_read_only_tool(name) || (name && (strcmp(name, "exec_command") == 0 ||
                    strcmp(name, "write_stdin") == 0 ||
                    strcmp(name, "apply_patch") == 0 ||
                    strcmp(name, "create_goal") == 0 ||
                    strcmp(name, "update_goal") == 0 ||
                    strcmp(name, "irc_send") == 0 ||
                    strcmp(name, "irc_state") == 0 ||
                    strcmp(name, "irc_topic") == 0));
}

static bool
arguments_bounded(const json_t *arguments)
{
    return json_is_object(arguments) &&
           snag_json_digest_bounded(arguments, SNAG_MAX_TOOL_ARGUMENTS, NULL, NULL) == 0;
}

static bool
item_valid(const json_t *value)
{
    static const char *const public_keys[] = {
        "kind", "local_item_id", "phase", "provider_item_id", "text"
    };
    static const char *const call_keys[] = {
        "arguments", "call_id", "kind", "name", "provider_call_id", "provider_item_id"
    };
    const char *kind = snag_json_string(value, "kind");
    const char *phase = snag_json_string(value, "phase");
    const char *id = snag_json_string(value, kind && !strcmp(kind, "tool_call") ?
                                     "call_id" : "local_item_id");

    if (!kind || !id || !snag_hex_is_lower(id, SNAG_ID_HEX_LEN) ||
        !provider_id_valid(snag_json_string(value, "provider_item_id")))
        return false;
    if (!strcmp(kind, "tool_call"))
        return snag_json_exact_keys(value, call_keys, 6u) &&
            provider_id_valid(snag_json_string(value, "provider_call_id")) &&
            tool_name_valid(snag_json_string(value, "name")) &&
            arguments_bounded(json_object_get(value, "arguments"));
    return (!strcmp(kind, "assistant") || !strcmp(kind, "refusal")) &&
        snag_json_exact_keys(value, public_keys, 5u) &&
        phase && (!strcmp(phase, "final_answer") ||
                  (!strcmp(kind, "assistant") && !strcmp(phase, "commentary"))) &&
        text_valid(snag_json_string(value, "text"), SNAG_MAX_PUBLIC_ITEM);
}

/* Takes ownership. Admission never leaves a partially appended item. */
static int
append_item(struct snag_response_graph *graph, json_t *value)
{
    size_t bytes, total, calls = 0u;
    int rc = -1;

    if (!value)
        return -1;
    if (!item_valid(value)) {
        errno = EINVAL;
        goto out;
    }
    if (!strcmp(snag_json_string(value, "kind"), "tool_call")) {
        for (size_t i = 0u; i < graph->count; ++i)
            calls += snag_response_graph_item(graph, i).kind == SNAG_ITEM_TOOL_CALL;
        if (calls >= SNAG_MAX_CALLS_PER_RESPONSE) {
            errno = EINVAL;
            goto out;
        }
    }
    if (graph->count >= SNAG_MAX_RESPONSE_ITEMS ||
        snag_json_digest_bounded(value, SNAG_MAX_RESPONSE_GRAPH, NULL, &bytes) < 0 ||
        !snag_size_add(bytes, graph->count ? 1u : 2u, &total) ||
        (graph->count && !snag_size_add(graph->encoded_bytes, total, &total)) ||
        total > SNAG_MAX_RESPONSE_GRAPH) {
        errno = EOVERFLOW;
        goto out;
    }
    if (!graph->items && !(graph->items = json_array()))
        goto out;
    if (json_array_append(graph->items, value) < 0)
        goto out;
    ++graph->count;
    graph->encoded_bytes = total;
    rc = 0;
out:
    json_decref(value);
    return rc;
}

int
snag_response_graph_add_public(struct snag_response_graph *graph,
                              enum snag_item_kind kind, enum snag_item_phase phase,
                              const char *provider_item_id, const char *text)
{
    char id[SNAG_ID_HEX_LEN + 1u];
    if (!public_kind(kind) ||
        (phase != SNAG_PHASE_FINAL_ANSWER &&
         (kind != SNAG_ITEM_ASSISTANT || phase != SNAG_PHASE_COMMENTARY)) ||
        !provider_id_valid(provider_item_id) || !text_valid(text, SNAG_MAX_PUBLIC_ITEM))
        return snag_errno(EINVAL);
    if (snag_random_id(id) < 0)
        return -1;
    return append_item(graph, json_pack("{s:s,s:s,s:s,s:s,s:s}",
        "kind", snag_item_kind_name(kind), "local_item_id", id,
        "phase", snag_item_phase_name(phase), "provider_item_id", provider_item_id,
        "text", text));
}

int
snag_response_graph_add_call(struct snag_response_graph *graph,
                            const char *provider_item_id, const char *provider_call_id,
                            const char *name, json_t *arguments)
{
    char id[SNAG_ID_HEX_LEN + 1u];
    json_t *value;
    if (!provider_id_valid(provider_item_id) || !provider_id_valid(provider_call_id) ||
        !tool_name_valid(name) || !arguments_bounded(arguments)) {
        json_decref(arguments);
        return snag_errno(EINVAL);
    }
    if (snag_random_id(id) < 0) {
        json_decref(arguments);
        return -1;
    }
    value = json_pack("{s:s,s:s,s:s,s:s,s:s,s:O}",
        "kind", "tool_call", "call_id", id, "provider_item_id", provider_item_id,
        "provider_call_id", provider_call_id, "name", name, "arguments", arguments);
    json_decref(arguments);
    return append_item(graph, value);
}

static int
identifiers_valid(const struct snag_response_graph *graph,
                  char *error, size_t error_size)
{
    for (size_t i = 0; i < graph->count; ++i) {
        struct snag_response_item view = snag_response_graph_item(graph, i);
        const struct snag_response_item *item = &view;

        if (public_kind(item->kind) &&
            !snag_hex_is_lower(item->local_item_id, SNAG_ID_HEX_LEN)) {
            return snag_fail(error, error_size, EINVAL,
                      "response item %zu has an invalid local id", i);
        }
        if (item->kind == SNAG_ITEM_TOOL_CALL &&
            !snag_hex_is_lower(item->call_id, SNAG_ID_HEX_LEN)) {
            return snag_fail(error, error_size, EINVAL,
                      "response item %zu has an invalid call id", i);
        }
        for (size_t j = 0; j < i; ++j) {
            struct snag_response_item previous = snag_response_graph_item(graph, j);
            if (public_kind(item->kind) &&
                public_kind(previous.kind) &&
                strcmp(item->local_item_id,
                       previous.local_item_id) == 0) {
                return snag_fail(error, error_size, EINVAL,
                          "response graph repeats a local item id");
            }
            if (item->kind == SNAG_ITEM_TOOL_CALL &&
                previous.kind == SNAG_ITEM_TOOL_CALL &&
                strcmp(item->call_id, previous.call_id) == 0) {
                return snag_fail(error, error_size, EINVAL,
                          "response graph repeats a call id");
            }
        }
    }
    return 0;
}

int
snag_response_graph_classify(const struct snag_response_graph *graph,
                            struct snag_graph_decision *decision,
                            char *error, size_t error_size)
{
    size_t terminal_count = 0;
    size_t terminal_index = 0;
    size_t last_speech = 0;
    bool have_speech = false;
    size_t calls = 0;
    size_t bad_index = 0;

    memset(decision, 0, sizeof(*decision));
    if (!graph->provider_response_id || graph->count > SNAG_MAX_RESPONSE_ITEMS) {
        return snag_fail(error, error_size, EINVAL, "response graph has no valid response id");
    }
    if (identifiers_valid(graph, error, error_size) < 0)
        return -1;
    for (size_t i = 0; i < graph->count; ++i) {
        struct snag_response_item view = snag_response_graph_item(graph, i);
        const struct snag_response_item *item = &view;
        bad_index = i;
        if (!provider_id_valid(item->provider_item_id)) {
            return snag_fail(error, error_size, EINVAL, "response item %zu has invalid identity", i);
        }
        if (!item_valid(json_array_get(graph->items, i)))
            goto bad_item;
        switch (item->kind) {
        case SNAG_ITEM_ASSISTANT:
            have_speech = true;
            last_speech = i;
            if (item->phase == SNAG_PHASE_FINAL_ANSWER) {
                ++terminal_count;
                terminal_index = i;
            }
            break;
        case SNAG_ITEM_REFUSAL:
            have_speech = true;
            last_speech = i;
            ++terminal_count;
            terminal_index = i;
            break;
        case SNAG_ITEM_TOOL_CALL:
            ++calls;
            break;
        }
    }
    if (calls > SNAG_MAX_CALLS_PER_RESPONSE) {
        return snag_fail(error, error_size, EOVERFLOW, "response graph exceeds 32 tool calls");
    }
    {
        json_t *items = graph->items ? json_incref(graph->items) : json_array();
        int rc = items ? snag_json_digest_bounded(items, SNAG_MAX_RESPONSE_GRAPH, NULL, NULL) : -1;
        if (items)
            json_decref(items);
        if (rc < 0) {
            return snag_fail(error, error_size, EOVERFLOW, "response graph exceeds 8 MiB");
        }
    }
    decision->call_count = calls;
    if (terminal_count > 1u || (terminal_count && calls) ||
        (terminal_count && (!have_speech || terminal_index != last_speech))) {
        decision->outcome = SNAG_GRAPH_CONFLICT;
        decision->message = terminal_count > 1u ?
            "provider response contained multiple terminal answers" :
            calls ? "provider response combined a terminal answer with tool calls" :
            "terminal answer was not the last assistant or refusal item";
        return 0;
    }
    if (terminal_count == 1u) {
        decision->final_index = terminal_index;
        decision->outcome = snag_response_graph_item(graph, terminal_index).kind == SNAG_ITEM_REFUSAL ?
                            SNAG_GRAPH_REFUSAL : SNAG_GRAPH_FINAL;
        return 0;
    }
    if (calls) {
        decision->outcome = SNAG_GRAPH_CALLS;
        return 0;
    }
    decision->outcome = SNAG_GRAPH_NONPRODUCTIVE;
    decision->message = "provider completed without a final answer, refusal, or tool call";
    return 0;

bad_item:
    return snag_fail(error, error_size, EINVAL, "response item %zu has an invalid shape", bad_index);
}

int
snag_tool_action_digest(const struct snag_response_item *call,
                       const char *resolved_workdir,
                       char out[SNAG_SHA256_HEX_LEN + 1u])
{
    json_t *action;
    int rc;

    if (!call || call->kind != SNAG_ITEM_TOOL_CALL || !resolved_workdir)
        return -1;
    action = json_pack("{s:O,s:s,s:s}", "arguments", call->arguments,
                       "name", call->name, "resolved_workdir", resolved_workdir);
    if (!action)
        return -1;
    rc = snag_json_digest(action, out);
    json_decref(action);
    return rc;
}

json_t *
snag_response_graph_json(const struct snag_response_graph *graph)
{
    return graph->items ? json_deep_copy(graph->items) : json_array();
}

int
snag_response_graph_from_json(struct snag_response_graph *graph, const json_t *items,
                             char *error, size_t error_size)
{
    if (!json_is_array(items) || json_array_size(items) > SNAG_MAX_RESPONSE_ITEMS) {
        return snag_fail(error, error_size, EINVAL, "invalid response item array");
    }
    for (size_t i = 0u; i < json_array_size(items); ++i)
        if (append_item(graph, json_deep_copy(json_array_get(items, i))) < 0) {
            return snag_fail(error, error_size, EINVAL, "invalid response item at index %zu", graph->count);
        }
    return identifiers_valid(graph, error, error_size);
}

int
snag_partial_public_validate(const json_t *items,
                            char *error, size_t error_size)
{
    struct snag_response_graph graph;
    int rc = -1;

    snag_response_graph_init(&graph);
    if (snag_response_graph_from_json(&graph, items,
                                     error, error_size) < 0)
        goto out;
    for (size_t i = 0; i < graph.count; ++i) {
        if (!public_kind(snag_response_graph_item(&graph, i).kind)) {
            snag_errorf(error, error_size,
                      "partial public array contains a non-public item");
            errno = EINVAL;
            goto out;
        }
    }
    rc = 0;
out:
    snag_response_graph_free(&graph);
    return rc;
}

json_t *
snag_tool_result(const char *status, const char *reason,
                const char *model_text, int exit_code, uint64_t duration_ms)
{
    json_t *out = json_pack(
        "{s:I,s:n,s:n,s:s,s:s?,s:n,s:s,"
        "s:{s:i,s:s,s:i,s:s,s:i},s:{s:i,s:s,s:i,s:s,s:i}}",
        "duration_ms", (json_int_t)duration_ms, "exit_code", "handle",
        "model_text", model_text, "reason", reason, "signal", "status", status,
        "stderr", "discarded_bytes", 0, "encoding", "utf8", "original_bytes", 0,
            "retained", "", "retained_bytes", 0,
        "stdout", "discarded_bytes", 0, "encoding", "utf8", "original_bytes", 0,
            "retained", "", "retained_bytes", 0);

    if (out && exit_code >= 0 &&
        snag_json_set_new(out, "exit_code", json_integer(exit_code)) < 0) {
        json_decref(out);
        return NULL;
    }
    return out;
}

json_t *
snag_tool_result_not_run(const char *reason)
{
    char text[192];
    (void)snprintf(text, sizeof(text), "Tool was not run: %s", reason);
    return snag_tool_result("not_run", reason, text, -1, 0u);
}

json_t *
snag_tool_result_terminal(bool succeeded, const char *model_text)
{
    return snag_tool_result(succeeded ? "succeeded" : "failed", NULL,
                           model_text, succeeded ? 0 : 1, 0u);
}

json_t *
snag_tool_result_outcome_unknown(const char *reason)
{
    char text[192];
    (void)snprintf(text, sizeof(text), "Tool outcome is unknown: %s", reason);
    return snag_tool_result("outcome_unknown", reason, text, -1, 0u);
}

static int
tool_excerpt_valid(const json_t *excerpt)
{
    static const char *const keys[] = {
        "discarded_bytes", "encoding", "original_bytes", "retained",
        "retained_bytes"
    };
    const char *encoding;
    const char *retained;
    uint64_t discarded;
    uint64_t original;
    uint64_t retained_bytes;

    if (!snag_json_exact_keys((json_t *)excerpt, keys, 5u) ||
        !(encoding = snag_json_string(excerpt, "encoding")) ||
        !(retained = snag_json_string(excerpt, "retained")) ||
        snag_json_integer_u64(excerpt, "discarded_bytes", &discarded) < 0 ||
        snag_json_integer_u64(excerpt, "original_bytes", &original) < 0 ||
        snag_json_integer_u64(excerpt, "retained_bytes", &retained_bytes) < 0)
        return -1;
    if (strcmp(encoding, "utf8") != 0 && strcmp(encoding, "base64") != 0)
        return -1;
    if (strcmp(encoding, "utf8") == 0 && strlen(retained) != retained_bytes)
        return -1;
    if (strcmp(encoding, "base64") == 0 && strlen(retained) % 4u != 0)
        return -1;
    return original >= discarded ? 0 : -1;
}

static bool
reason_is_not_run(const char *reason)
{
    return reason && (strcmp(reason, "protocol_conflict") == 0 ||
                      strcmp(reason, "read_only") == 0 ||
                      strcmp(reason, "process_limit") == 0 ||
                      strcmp(reason, "batch_yield") == 0 ||
                      strcmp(reason, "operator_yield") == 0 ||
                      strcmp(reason, "process_busy") == 0 ||
                      strcmp(reason, "stdin_busy") == 0 ||
                      strcmp(reason, "stdin_closed") == 0 ||
                      strcmp(reason, "invalid_arguments") == 0 ||
                      strcmp(reason, "managed_process_conflict") == 0 ||
                      strcmp(reason, "managed_process_handle_mismatch") == 0 ||
                      strcmp(reason, "recovery_unstarted") == 0 ||
                      strcmp(reason, "superseded_by_steering") == 0 ||
                      strcmp(reason, "turn_cancelled") == 0 ||
                      strcmp(reason, "process_interaction_required") == 0);
}

int
snag_tool_result_valid(const json_t *result)
{
    static const char *const keys[] = {
        "duration_ms", "exit_code", "handle", "model_text", "reason",
        "signal", "status", "stderr", "stdout", "max_output_tokens", "output_ref"
    };
    const char *status;
    const char *reason;
    const char *model_text;
    uint64_t duration;
    json_t *reason_value;
    json_t *handle;
    json_t *exit_value;
    json_t *signal_value;
    json_t *limit_value;

    if ((!snag_json_exact_keys((json_t *)result, keys, 11u) &&
         !snag_json_exact_keys((json_t *)result, keys, 10u) &&
         !snag_json_exact_keys((json_t *)result, keys, 9u)) ||
        snag_json_integer_u64(result, "duration_ms", &duration) < 0 ||
        !(status = snag_json_string(result, "status")) ||
        !(model_text = snag_json_string(result, "model_text")) ||
        tool_excerpt_valid(json_object_get(result, "stdout")) < 0 ||
        tool_excerpt_valid(json_object_get(result, "stderr")) < 0)
        return -1;
    limit_value = json_object_get(result, "max_output_tokens");
    if (limit_value &&
        (!json_is_integer(limit_value) || json_integer_value(limit_value) < 1 ||
         (uint64_t)json_integer_value(limit_value) >
             SNAG_CONFIG_TOKEN_LIMIT_MAX))
        return -1;
    json_t *ref = json_object_get(result, "output_ref");
    if (ref) {
        static const char *const ref_keys[] = {"handle", "stdout_start", "stdout_end",
            "stderr_start", "stderr_end", "stdin_accepted", "stdin_written",
            "stdin_pending", "stdin_open", "log_start", "log_end"};
        const char *h = snag_json_string(ref, "handle");
        const char *const begin[] = {"stdout_start", "stderr_start"};
        const char *const end[] = {"stdout_end", "stderr_end"};
        const char *const streams[] = {"stdout", "stderr"};
        uint64_t accepted, written, pending, log_start, log_end;
        if (!snag_json_exact_keys(ref, ref_keys, 11u) || !h ||
            !snag_hex_is_lower(h, SNAG_ID_HEX_LEN) ||
            snag_json_integer_u64(ref, "log_start", &log_start) < 0 ||
            snag_json_integer_u64(ref, "log_end", &log_end) < 0 || log_start > log_end ||
            !json_is_boolean(json_object_get(ref, "stdin_open")) ||
            snag_json_integer_u64(ref, "stdin_accepted", &accepted) < 0 ||
            snag_json_integer_u64(ref, "stdin_written", &written) < 0 ||
            snag_json_integer_u64(ref, "stdin_pending", &pending) < 0 ||
            written > accepted || pending > accepted - written)
            return -1;
        for (unsigned int s = 0u; s < 2u; ++s) {
            uint64_t from, to, bytes;
            if (snag_json_integer_u64(ref, begin[s], &from) < 0 ||
                snag_json_integer_u64(ref, end[s], &to) < 0 || from > to ||
                snag_json_integer_u64(json_object_get(result, streams[s]), "original_bytes", &bytes) < 0 ||
                bytes != to - from)
                return -1;
        }
    }
    (void)model_text;
    reason_value = json_object_get(result, "reason");
    handle = json_object_get(result, "handle");
    exit_value = json_object_get(result, "exit_code");
    signal_value = json_object_get(result, "signal");
    reason = snag_json_string(result, "reason");
    if (strcmp(status, "not_run") == 0)
        return reason_is_not_run(reason) && json_is_null(handle) ? 0 : -1;
    if (strcmp(status, "outcome_unknown") == 0)
        return reason && (strcmp(reason, "owner_lost") == 0 ||
                          strcmp(reason, "unreaped_after_sigkill") == 0) &&
               json_is_null(handle) ? 0 : -1;
    if (strcmp(status, "denied") == 0)
        return reason && strcmp(reason, "user_denied") == 0 &&
               json_is_null(handle) ? 0 : -1;
    if (strcmp(status, "cancelled") == 0)
        return reason && strcmp(reason, "turn_cancelled") == 0 &&
               json_is_null(handle) ? 0 : -1;
    if (strcmp(status, "running") == 0)
        return json_is_string(handle) &&
               snag_hex_is_lower(json_string_value(handle), SNAG_ID_HEX_LEN) &&
               (json_is_null(reason_value) ||
                (reason && (strcmp(reason, "timeout_handoff") == 0 ||
                            strcmp(reason, "wait_timeout") == 0 ||
                            strcmp(reason, "operator_yield") == 0 ||
                            strcmp(reason, "batch_yield") == 0 ||
                            strcmp(reason, "steering_handoff") == 0))) ? 0 : -1;
    if (!json_is_null(handle) ||
        (!json_is_null(reason_value) &&
         !(reason && strcmp(reason, "output_drain_timeout") == 0 &&
           (!strcmp(status, "succeeded") || !strcmp(status, "failed") || !strcmp(status, "signaled")))))
        return -1;
    if (strcmp(status, "succeeded") == 0 || strcmp(status, "failed") == 0)
        return json_is_integer(exit_value) && json_is_null(signal_value) ? 0 : -1;
    if (strcmp(status, "signaled") == 0)
        return json_is_null(exit_value) && json_is_integer(signal_value) ? 0 : -1;
    if (strcmp(status, "timed_out") == 0 ||
        strcmp(status, "patch_rejected") == 0 ||
        strcmp(status, "io_failed") == 0)
        return 0;
    return -1;
}

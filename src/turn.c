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
    case SNAG_ITEM_REASONING_SUMMARY: return "reasoning_summary";
    case SNAG_ITEM_TOOL_CALL: return "tool_call";
    case SNAG_ITEM_OPAQUE: return "opaque";
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
    case SNAG_PHASE_SUMMARY: return "summary";
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
    return kind == SNAG_ITEM_ASSISTANT || kind == SNAG_ITEM_REFUSAL ||
           kind == SNAG_ITEM_REASONING_SUMMARY;
}

static json_t *item_json(const struct snag_response_item *item);

static void
item_free(struct snag_response_item *item)
{
    free(item->provider_item_id);
    free(item->provider_call_id);
    free(item->text);
    free(item->name);
    free(item->provider_type);
    if (item->arguments)
        json_decref(item->arguments);
    if (item->payload)
        json_decref(item->payload);
    memset(item, 0, sizeof(*item));
}

static int
account_last_item(struct snag_response_graph *graph)
{
    struct snag_buf encoded;
    json_t *value;
    size_t total;
    int rc;

    value = item_json(&graph->items[graph->count - 1u]);
    if (!value)
        return -1;
    snag_buf_init(&encoded, SNAG_MAX_RESPONSE_GRAPH);
    rc = snag_json_canonical(value, &encoded);
    json_decref(value);
    if (rc < 0) {
        snag_buf_free(&encoded);
        errno = EOVERFLOW;
        return -1;
    }
    if (graph->count == 1u) {
        if (!snag_size_add(encoded.len, 2u, &total)) {
            snag_buf_free(&encoded);
            errno = EOVERFLOW;
            return -1;
        }
    } else if (!snag_size_add(graph->encoded_bytes, encoded.len, &total) ||
               !snag_size_add(total, 1u, &total)) {
        snag_buf_free(&encoded);
        errno = EOVERFLOW;
        return -1;
    }
    snag_buf_free(&encoded);
    if (total > SNAG_MAX_RESPONSE_GRAPH) {
        errno = EOVERFLOW;
        return -1;
    }
    graph->encoded_bytes = total;
    return 0;
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

    if (!usage) {
        errno = EINVAL;
        return -1;
    }
    if ((usage->input_known && usage->input_tokens > (uint64_t)LLONG_MAX) ||
        (usage->output_known && usage->output_tokens > (uint64_t)LLONG_MAX) ||
        (usage->reasoning_known && usage->reasoning_tokens > (uint64_t)LLONG_MAX) ||
        (usage->total_known && usage->total_tokens > (uint64_t)LLONG_MAX)) {
        errno = EOVERFLOW;
        return -1;
    }
    if (usage->reasoning_known && usage->output_known &&
        usage->reasoning_tokens > usage->output_tokens) {
        errno = EINVAL;
        return -1;
    }
    if (usage->input_known && usage->output_known && usage->total_known) {
        if (usage->input_tokens > UINT64_MAX - usage->output_tokens) {
            errno = EOVERFLOW;
            return -1;
        }
        sum = usage->input_tokens + usage->output_tokens;
        if (usage->total_tokens != sum) {
            errno = EINVAL;
            return -1;
        }
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
        snag_response_usage_valid(&parsed) < 0) {
        errno = EINVAL;
        return -1;
    }
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
    for (size_t i = 0; i < graph->count; ++i)
        item_free(&graph->items[i]);
    free(graph->items);
    free(graph->provider_response_id);
    snag_response_graph_init(graph);
}

static struct snag_response_item *
append_item(struct snag_response_graph *graph)
{
    struct snag_response_item *items;
    size_t cap;

    if (graph->count >= SNAG_MAX_RESPONSE_ITEMS) {
        errno = EOVERFLOW;
        return NULL;
    }
    if (graph->count == graph->cap) {
        cap = graph->cap ? graph->cap * 2u : 8u;
        if (cap > SNAG_MAX_RESPONSE_ITEMS)
            cap = SNAG_MAX_RESPONSE_ITEMS;
        items = realloc(graph->items, cap * sizeof(*items));
        if (!items)
            return NULL;
        memset(items + graph->cap, 0,
               (cap - graph->cap) * sizeof(*items));
        graph->items = items;
        graph->cap = cap;
    }
    return &graph->items[graph->count++];
}

int
snag_response_graph_set_provider_id(struct snag_response_graph *graph,
                                   const char *provider_response_id)
{
    char *copy;
    if (!provider_id_valid(provider_response_id)) {
        errno = EINVAL;
        return -1;
    }
    copy = snag_strdup_checked(provider_response_id, SNAG_MAX_PROVIDER_ID);
    if (!copy)
        return -1;
    free(graph->provider_response_id);
    graph->provider_response_id = copy;
    return 0;
}

static int
set_local_id(char out[SNAG_ID_HEX_LEN + 1u], const char *persisted)
{
    if (!persisted)
        return snag_random_id(out);
    memcpy(out, persisted, SNAG_ID_HEX_LEN + 1u);
    return 0;
}

static int
add_public(struct snag_response_graph *graph, enum snag_item_kind kind,
           enum snag_item_phase phase, const char *provider_item_id,
           const char *text, const char *local_item_id)
{
    struct snag_response_item *item;
    bool shape = (kind == SNAG_ITEM_ASSISTANT &&
                  (phase == SNAG_PHASE_COMMENTARY ||
                   phase == SNAG_PHASE_FINAL_ANSWER)) ||
                 (kind == SNAG_ITEM_REFUSAL &&
                  phase == SNAG_PHASE_FINAL_ANSWER) ||
                 (kind == SNAG_ITEM_REASONING_SUMMARY &&
                  phase == SNAG_PHASE_SUMMARY);

    if (!shape || !provider_id_valid(provider_item_id) ||
        (local_item_id &&
         !snag_hex_is_lower(local_item_id, SNAG_ID_HEX_LEN)) ||
        !text_valid(text, SNAG_MAX_PUBLIC_ITEM)) {
        errno = EINVAL;
        return -1;
    }
    item = append_item(graph);
    if (!item)
        return -1;
    item->kind = kind;
    item->phase = phase;
    if (set_local_id(item->local_item_id, local_item_id) < 0 ||
        !(item->provider_item_id =
          snag_strdup_checked(provider_item_id, SNAG_MAX_PROVIDER_ID)) ||
        !(item->text = snag_strdup_checked(text, SNAG_MAX_PUBLIC_ITEM)) ||
        account_last_item(graph) < 0) {
        item_free(item);
        --graph->count;
        return -1;
    }
    return 0;
}

int
snag_response_graph_add_public(struct snag_response_graph *graph,
                              enum snag_item_kind kind,
                              enum snag_item_phase phase,
                              const char *provider_item_id,
                              const char *text)
{
    return add_public(graph, kind, phase, provider_item_id, text, NULL);
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
    struct snag_buf encoded;
    int rc;
    if (!json_is_object(arguments))
        return false;
    snag_buf_init(&encoded, SNAG_MAX_TOOL_ARGUMENTS);
    rc = snag_json_canonical(arguments, &encoded);
    snag_buf_free(&encoded);
    return rc == 0;
}

static int
add_call(struct snag_response_graph *graph, const char *provider_item_id,
         const char *provider_call_id, const char *name, json_t *arguments,
         const char *call_id)
{
    struct snag_response_item *item;
    size_t calls = 0;

    for (size_t i = 0; i < graph->count; ++i)
        if (graph->items[i].kind == SNAG_ITEM_TOOL_CALL)
            ++calls;
    if (calls >= SNAG_MAX_CALLS_PER_RESPONSE ||
        !provider_id_valid(provider_item_id) ||
        !provider_id_valid(provider_call_id) || !tool_name_valid(name) ||
        (call_id && !snag_hex_is_lower(call_id, SNAG_ID_HEX_LEN)) ||
        !arguments_bounded(arguments)) {
        if (arguments)
            json_decref(arguments);
        errno = EINVAL;
        return -1;
    }
    item = append_item(graph);
    if (!item) {
        json_decref(arguments);
        return -1;
    }
    item->kind = SNAG_ITEM_TOOL_CALL;
    item->phase = SNAG_PHASE_NONE;
    item->arguments = arguments;
    if (set_local_id(item->call_id, call_id) < 0 ||
        !(item->provider_item_id =
          snag_strdup_checked(provider_item_id, SNAG_MAX_PROVIDER_ID)) ||
        !(item->provider_call_id =
          snag_strdup_checked(provider_call_id, SNAG_MAX_PROVIDER_ID)) ||
        !(item->name = snag_strdup_checked(name, 64u)) ||
        account_last_item(graph) < 0) {
        item_free(item);
        --graph->count;
        return -1;
    }
    return 0;
}

int
snag_response_graph_add_call(struct snag_response_graph *graph,
                            const char *provider_item_id,
                            const char *provider_call_id,
                            const char *name, json_t *arguments)
{
    return add_call(graph, provider_item_id, provider_call_id, name,
                    arguments, NULL);
}

int
snag_response_graph_add_opaque(struct snag_response_graph *graph,
                              const char *provider_item_id,
                              const char *provider_type, json_t *payload)
{
    struct snag_response_item *item;
    if (!provider_id_valid(provider_item_id) ||
        !provider_id_valid(provider_type) || !json_is_object(payload) ||
        !arguments_bounded(payload)) {
        if (payload)
            json_decref(payload);
        errno = EINVAL;
        return -1;
    }
    item = append_item(graph);
    if (!item) {
        json_decref(payload);
        return -1;
    }
    item->kind = SNAG_ITEM_OPAQUE;
    item->phase = SNAG_PHASE_NONE;
    item->payload = payload;
    if (!(item->provider_item_id =
          snag_strdup_checked(provider_item_id, SNAG_MAX_PROVIDER_ID)) ||
        !(item->provider_type =
          snag_strdup_checked(provider_type, SNAG_MAX_PROVIDER_ID)) ||
        account_last_item(graph) < 0) {
        item_free(item);
        --graph->count;
        return -1;
    }
    return 0;
}

static int
identifiers_valid(const struct snag_response_graph *graph,
                  char *error, size_t error_size)
{
    for (size_t i = 0; i < graph->count; ++i) {
        const struct snag_response_item *item = &graph->items[i];

        if (public_kind(item->kind) &&
            !snag_hex_is_lower(item->local_item_id, SNAG_ID_HEX_LEN)) {
            snag_errorf(error, error_size,
                      "response item %zu has an invalid local id", i);
            errno = EINVAL;
            return -1;
        }
        if (item->kind == SNAG_ITEM_TOOL_CALL &&
            !snag_hex_is_lower(item->call_id, SNAG_ID_HEX_LEN)) {
            snag_errorf(error, error_size,
                      "response item %zu has an invalid call id", i);
            errno = EINVAL;
            return -1;
        }
        for (size_t j = 0; j < i; ++j) {
            if (public_kind(item->kind) &&
                public_kind(graph->items[j].kind) &&
                strcmp(item->local_item_id,
                       graph->items[j].local_item_id) == 0) {
                snag_errorf(error, error_size,
                          "response graph repeats a local item id");
                errno = EINVAL;
                return -1;
            }
            if (item->kind == SNAG_ITEM_TOOL_CALL &&
                graph->items[j].kind == SNAG_ITEM_TOOL_CALL &&
                strcmp(item->call_id, graph->items[j].call_id) == 0) {
                snag_errorf(error, error_size,
                          "response graph repeats a call id");
                errno = EINVAL;
                return -1;
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
    struct snag_buf encoded;

    memset(decision, 0, sizeof(*decision));
    if (!graph->provider_response_id || graph->count > SNAG_MAX_RESPONSE_ITEMS) {
        snag_errorf(error, error_size, "response graph has no valid response id");
        errno = EINVAL;
        return -1;
    }
    if (identifiers_valid(graph, error, error_size) < 0)
        return -1;
    for (size_t i = 0; i < graph->count; ++i) {
        const struct snag_response_item *item = &graph->items[i];
        bad_index = i;
        if (!provider_id_valid(item->provider_item_id)) {
            snag_errorf(error, error_size, "response item %zu has invalid identity", i);
            errno = EINVAL;
            return -1;
        }
        switch (item->kind) {
        case SNAG_ITEM_ASSISTANT:
            if ((item->phase != SNAG_PHASE_COMMENTARY &&
                 item->phase != SNAG_PHASE_FINAL_ANSWER) ||
                !text_valid(item->text, SNAG_MAX_PUBLIC_ITEM))
                goto bad_item;
            have_speech = true;
            last_speech = i;
            if (item->phase == SNAG_PHASE_FINAL_ANSWER) {
                ++terminal_count;
                terminal_index = i;
            }
            break;
        case SNAG_ITEM_REFUSAL:
            if (item->phase != SNAG_PHASE_FINAL_ANSWER ||
                !text_valid(item->text, SNAG_MAX_PUBLIC_ITEM))
                goto bad_item;
            have_speech = true;
            last_speech = i;
            ++terminal_count;
            terminal_index = i;
            break;
        case SNAG_ITEM_REASONING_SUMMARY:
            if (item->phase != SNAG_PHASE_SUMMARY ||
                !text_valid(item->text, SNAG_MAX_PUBLIC_ITEM))
                goto bad_item;
            break;
        case SNAG_ITEM_TOOL_CALL:
            if (!snag_hex_is_lower(item->call_id, SNAG_ID_HEX_LEN) ||
                !provider_id_valid(item->provider_call_id) ||
                !tool_name_valid(item->name) ||
                !arguments_bounded(item->arguments))
                goto bad_item;
            ++calls;
            break;
        case SNAG_ITEM_OPAQUE:
            if (!provider_id_valid(item->provider_type) ||
                !json_is_object(item->payload) || !arguments_bounded(item->payload))
                goto bad_item;
            break;
        }
    }
    if (calls > SNAG_MAX_CALLS_PER_RESPONSE) {
        snag_errorf(error, error_size, "response graph exceeds 32 tool calls");
        errno = EOVERFLOW;
        return -1;
    }
    snag_buf_init(&encoded, SNAG_MAX_RESPONSE_GRAPH);
    {
        json_t *items = snag_response_graph_json(graph);
        int rc = items ? snag_json_canonical(items, &encoded) : -1;
        if (items)
            json_decref(items);
        snag_buf_free(&encoded);
        if (rc < 0) {
            snag_errorf(error, error_size, "response graph exceeds 8 MiB");
            errno = EOVERFLOW;
            return -1;
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
        decision->outcome = graph->items[terminal_index].kind == SNAG_ITEM_REFUSAL ?
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
    snag_errorf(error, error_size, "response item %zu has an invalid shape", bad_index);
    errno = EINVAL;
    return -1;
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

static json_t *
item_json(const struct snag_response_item *item)
{
    const char *kind = snag_item_kind_name(item->kind);

    switch (item->kind) {
    case SNAG_ITEM_ASSISTANT:
    case SNAG_ITEM_REFUSAL:
    case SNAG_ITEM_REASONING_SUMMARY:
        return json_pack("{s:s,s:s,s:s,s:s,s:s}",
            "kind", kind, "local_item_id", item->local_item_id,
            "phase", snag_item_phase_name(item->phase),
            "provider_item_id", item->provider_item_id, "text", item->text);
    case SNAG_ITEM_TOOL_CALL:
        return json_pack("{s:o,s:s,s:s,s:s,s:s,s:s}",
            "arguments", json_deep_copy(item->arguments),
            "kind", kind, "call_id", item->call_id, "name", item->name,
            "provider_call_id", item->provider_call_id,
            "provider_item_id", item->provider_item_id);
    case SNAG_ITEM_OPAQUE:
        return json_pack("{s:o,s:s,s:s,s:s}",
            "payload", json_deep_copy(item->payload), "kind", kind,
            "provider_item_id", item->provider_item_id,
            "provider_type", item->provider_type);
    }
    return NULL;
}

json_t *
snag_response_graph_json(const struct snag_response_graph *graph)
{
    json_t *items = json_array();
    if (!items)
        return NULL;
    for (size_t i = 0; i < graph->count; ++i) {
        json_t *item = item_json(&graph->items[i]);
        if (!item) {
            json_decref(items);
            return NULL;
        }
        if (json_array_append_new(items, item) < 0) {
            json_decref(items);
            return NULL;
        }
    }
    return items;
}

static int
parse_public(struct snag_response_graph *graph, const json_t *value,
             enum snag_item_kind kind, char *error, size_t error_size)
{
    static const char *const keys[] = {
        "kind", "local_item_id", "phase", "provider_item_id", "text"
    };
    const char *phase = snag_json_string(value, "phase");
    const char *provider_id = snag_json_string(value, "provider_item_id");
    const char *text = snag_json_string(value, "text");
    const char *local_id = snag_json_string(value, "local_item_id");
    enum snag_item_phase p;
    if (!snag_json_exact_keys((json_t *)value, keys, 5u) || !phase || !provider_id ||
        !text || !local_id || !snag_hex_is_lower(local_id, SNAG_ID_HEX_LEN))
        goto invalid;
    if (strcmp(phase, "commentary") == 0)
        p = SNAG_PHASE_COMMENTARY;
    else if (strcmp(phase, "final_answer") == 0)
        p = SNAG_PHASE_FINAL_ANSWER;
    else if (strcmp(phase, "summary") == 0)
        p = SNAG_PHASE_SUMMARY;
    else
        goto invalid;
    if (add_public(graph, kind, p, provider_id, text, local_id) < 0)
        goto invalid;
    return 0;
invalid:
    snag_errorf(error, error_size, "invalid public response item");
    errno = EINVAL;
    return -1;
}

int
snag_response_graph_from_json(struct snag_response_graph *graph,
                             const json_t *items,
                             char *error, size_t error_size)
{
    if (!json_is_array(items) || json_array_size(items) > SNAG_MAX_RESPONSE_ITEMS) {
        snag_errorf(error, error_size, "invalid response item array");
        errno = EINVAL;
        return -1;
    }
    for (size_t i = 0; i < json_array_size(items); ++i) {
        json_t *value = json_array_get(items, i);
        const char *kind = snag_json_string(value, "kind");
        if (!json_is_object(value) || !kind)
            goto invalid;
        if (strcmp(kind, "assistant") == 0) {
            if (parse_public(graph, value, SNAG_ITEM_ASSISTANT,
                             error, error_size) < 0)
                return -1;
        } else if (strcmp(kind, "refusal") == 0) {
            if (parse_public(graph, value, SNAG_ITEM_REFUSAL,
                             error, error_size) < 0)
                return -1;
        } else if (strcmp(kind, "reasoning_summary") == 0) {
            if (parse_public(graph, value, SNAG_ITEM_REASONING_SUMMARY,
                             error, error_size) < 0)
                return -1;
        } else if (strcmp(kind, "tool_call") == 0) {
            static const char *const keys[] = {
                "arguments", "call_id", "kind", "name", "provider_call_id",
                "provider_item_id"
            };
            const char *call_id = snag_json_string(value, "call_id");
            const char *name = snag_json_string(value, "name");
            const char *provider_call_id = snag_json_string(value, "provider_call_id");
            const char *provider_item_id = snag_json_string(value, "provider_item_id");
            json_t *arguments = json_object_get(value, "arguments");
            if (!snag_json_exact_keys(value, keys, 6u) || !call_id ||
                !snag_hex_is_lower(call_id, SNAG_ID_HEX_LEN) || !arguments ||
                add_call(graph, provider_item_id, provider_call_id, name,
                         json_deep_copy(arguments), call_id) < 0)
                goto invalid;
        } else if (strcmp(kind, "opaque") == 0) {
            static const char *const keys[] = {
                "kind", "payload", "provider_item_id", "provider_type"
            };
            const char *provider_item_id = snag_json_string(value, "provider_item_id");
            const char *provider_type = snag_json_string(value, "provider_type");
            json_t *payload = json_object_get(value, "payload");
            if (!snag_json_exact_keys(value, keys, 4u) || !payload ||
                snag_response_graph_add_opaque(graph, provider_item_id,
                                              provider_type,
                                              json_deep_copy(payload)) < 0)
                goto invalid;
        } else {
            goto invalid;
        }
    }
    if (identifiers_valid(graph, error, error_size) < 0)
        return -1;
    return 0;
invalid:
    snag_errorf(error, error_size, "invalid response item at index %zu",
              graph->count);
    errno = EINVAL;
    return -1;
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
        if (!public_kind(graph.items[i].kind)) {
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
        "signal", "status", "stderr", "stdout", "max_output_tokens"
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

    if ((!snag_json_exact_keys((json_t *)result, keys, 10u) &&
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
                            strcmp(reason, "steering_handoff") == 0))) ? 0 : -1;
    if (!json_is_null(reason_value) || !json_is_null(handle))
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

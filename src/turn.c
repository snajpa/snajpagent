/* SPDX-License-Identifier: GPL-2.0-only */
#include "turn.h"

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
set_error(char *error, size_t size, const char *fmt, ...)
{
    va_list ap;
    if (!size)
        return;
    va_start(ap, fmt);
    (void)vsnprintf(error, size, fmt, ap);
    va_end(ap);
}

const char *
snj_item_kind_name(enum snj_item_kind kind)
{
    switch (kind) {
    case SNJ_ITEM_ASSISTANT: return "assistant";
    case SNJ_ITEM_REFUSAL: return "refusal";
    case SNJ_ITEM_REASONING_SUMMARY: return "reasoning_summary";
    case SNJ_ITEM_TOOL_CALL: return "tool_call";
    case SNJ_ITEM_OPAQUE: return "opaque";
    }
    return NULL;
}

const char *
snj_item_phase_name(enum snj_item_phase phase)
{
    switch (phase) {
    case SNJ_PHASE_COMMENTARY: return "commentary";
    case SNJ_PHASE_FINAL_ANSWER: return "final_answer";
    case SNJ_PHASE_SUMMARY: return "summary";
    case SNJ_PHASE_NONE: break;
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
    if (len > SNJ_MAX_PROVIDER_ID ||
        !snj_utf8_valid((const unsigned char *)s, len, true))
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
    return len <= max && snj_utf8_valid((const unsigned char *)s, len, true);
}

static bool
public_kind(enum snj_item_kind kind)
{
    return kind == SNJ_ITEM_ASSISTANT || kind == SNJ_ITEM_REFUSAL ||
           kind == SNJ_ITEM_REASONING_SUMMARY;
}

static json_t *item_json(const struct snj_response_item *item);

static void
item_free(struct snj_response_item *item)
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
account_last_item(struct snj_response_graph *graph)
{
    struct snj_buf encoded;
    json_t *value;
    size_t total;
    int rc;

    value = item_json(&graph->items[graph->count - 1u]);
    if (!value)
        return -1;
    snj_buf_init(&encoded, SNJ_MAX_RESPONSE_GRAPH);
    rc = snj_json_canonical(value, &encoded);
    json_decref(value);
    if (rc < 0) {
        snj_buf_free(&encoded);
        errno = EOVERFLOW;
        return -1;
    }
    if (graph->count == 1u) {
        if (!snj_size_add(encoded.len, 2u, &total)) {
            snj_buf_free(&encoded);
            errno = EOVERFLOW;
            return -1;
        }
    } else if (!snj_size_add(graph->encoded_bytes, encoded.len, &total) ||
               !snj_size_add(total, 1u, &total)) {
        snj_buf_free(&encoded);
        errno = EOVERFLOW;
        return -1;
    }
    snj_buf_free(&encoded);
    if (total > SNJ_MAX_RESPONSE_GRAPH) {
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
snj_response_usage_valid(const struct snj_response_usage *usage)
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
snj_response_usage_json(const struct snj_response_usage *usage)
{
    json_t *value = json_object();

    if (!value || snj_response_usage_valid(usage) < 0 ||
        snj_json_set_new(value, "input_tokens",
            usage->input_known ? json_integer((json_int_t)usage->input_tokens) :
                                 json_null()) < 0 ||
        snj_json_set_new(value, "output_tokens",
            usage->output_known ? json_integer((json_int_t)usage->output_tokens) :
                                  json_null()) < 0 ||
        snj_json_set_new(value, "reasoning_tokens",
            usage->reasoning_known ?
                json_integer((json_int_t)usage->reasoning_tokens) : json_null()) < 0 ||
        snj_json_set_new(value, "total_tokens",
            usage->total_known ? json_integer((json_int_t)usage->total_tokens) :
                                 json_null()) < 0) {
        if (value)
            json_decref(value);
        return NULL;
    }
    return value;
}

int
snj_response_usage_from_json(const json_t *value,
                             struct snj_response_usage *usage)
{
    static const char *const keys[] = {
        "input_tokens", "output_tokens", "reasoning_tokens", "total_tokens"
    };
    struct snj_response_usage parsed;

    memset(&parsed, 0, sizeof(parsed));
    if (!usage || !snj_json_exact_keys(value, keys, 4u) ||
        nullable_usage_member(value, "input_tokens", &parsed.input_tokens,
                              &parsed.input_known) < 0 ||
        nullable_usage_member(value, "output_tokens", &parsed.output_tokens,
                              &parsed.output_known) < 0 ||
        nullable_usage_member(value, "reasoning_tokens", &parsed.reasoning_tokens,
                              &parsed.reasoning_known) < 0 ||
        nullable_usage_member(value, "total_tokens", &parsed.total_tokens,
                              &parsed.total_known) < 0 ||
        snj_response_usage_valid(&parsed) < 0) {
        errno = EINVAL;
        return -1;
    }
    *usage = parsed;
    return 0;
}

void
snj_response_graph_init(struct snj_response_graph *graph)
{
    memset(graph, 0, sizeof(*graph));
}

void
snj_response_graph_free(struct snj_response_graph *graph)
{
    for (size_t i = 0; i < graph->count; ++i)
        item_free(&graph->items[i]);
    free(graph->items);
    free(graph->provider_response_id);
    snj_response_graph_init(graph);
}

static struct snj_response_item *
append_item(struct snj_response_graph *graph)
{
    struct snj_response_item *items;
    size_t cap;

    if (graph->count >= SNJ_MAX_RESPONSE_ITEMS) {
        errno = EOVERFLOW;
        return NULL;
    }
    if (graph->count == graph->cap) {
        cap = graph->cap ? graph->cap * 2u : 8u;
        if (cap > SNJ_MAX_RESPONSE_ITEMS)
            cap = SNJ_MAX_RESPONSE_ITEMS;
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
snj_response_graph_set_provider_id(struct snj_response_graph *graph,
                                   const char *provider_response_id)
{
    char *copy;
    if (!provider_id_valid(provider_response_id)) {
        errno = EINVAL;
        return -1;
    }
    copy = snj_strdup_checked(provider_response_id, SNJ_MAX_PROVIDER_ID);
    if (!copy)
        return -1;
    free(graph->provider_response_id);
    graph->provider_response_id = copy;
    return 0;
}

static int
set_local_id(char out[SNJ_ID_HEX_LEN + 1u], const char *persisted)
{
    if (!persisted)
        return snj_random_id(out);
    memcpy(out, persisted, SNJ_ID_HEX_LEN + 1u);
    return 0;
}

static int
add_public(struct snj_response_graph *graph, enum snj_item_kind kind,
           enum snj_item_phase phase, const char *provider_item_id,
           const char *text, const char *local_item_id)
{
    struct snj_response_item *item;
    bool shape = (kind == SNJ_ITEM_ASSISTANT &&
                  (phase == SNJ_PHASE_COMMENTARY ||
                   phase == SNJ_PHASE_FINAL_ANSWER)) ||
                 (kind == SNJ_ITEM_REFUSAL &&
                  phase == SNJ_PHASE_FINAL_ANSWER) ||
                 (kind == SNJ_ITEM_REASONING_SUMMARY &&
                  phase == SNJ_PHASE_SUMMARY);

    if (!shape || !provider_id_valid(provider_item_id) ||
        (local_item_id &&
         !snj_hex_is_lower(local_item_id, SNJ_ID_HEX_LEN)) ||
        !text_valid(text, SNJ_MAX_PUBLIC_ITEM)) {
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
          snj_strdup_checked(provider_item_id, SNJ_MAX_PROVIDER_ID)) ||
        !(item->text = snj_strdup_checked(text, SNJ_MAX_PUBLIC_ITEM))) {
        item_free(item);
        --graph->count;
        return -1;
    }
    if (account_last_item(graph) < 0) {
        item_free(item);
        --graph->count;
        return -1;
    }
    return 0;
}

int
snj_response_graph_add_public(struct snj_response_graph *graph,
                              enum snj_item_kind kind,
                              enum snj_item_phase phase,
                              const char *provider_item_id,
                              const char *text)
{
    return add_public(graph, kind, phase, provider_item_id, text, NULL);
}

static bool
tool_name_valid(const char *name)
{
    return name && (strcmp(name, "exec_command") == 0 ||
                    strcmp(name, "write_stdin") == 0 ||
                    strcmp(name, "apply_patch") == 0);
}

static bool
arguments_bounded(const json_t *arguments)
{
    struct snj_buf encoded;
    int rc;
    if (!json_is_object(arguments))
        return false;
    snj_buf_init(&encoded, SNJ_MAX_TOOL_ARGUMENTS);
    rc = snj_json_canonical(arguments, &encoded);
    snj_buf_free(&encoded);
    return rc == 0;
}

static int
add_call(struct snj_response_graph *graph, const char *provider_item_id,
         const char *provider_call_id, const char *name, json_t *arguments,
         const char *call_id)
{
    struct snj_response_item *item;
    size_t calls = 0;

    for (size_t i = 0; i < graph->count; ++i)
        if (graph->items[i].kind == SNJ_ITEM_TOOL_CALL)
            ++calls;
    if (calls >= SNJ_MAX_CALLS_PER_RESPONSE ||
        !provider_id_valid(provider_item_id) ||
        !provider_id_valid(provider_call_id) || !tool_name_valid(name) ||
        (call_id && !snj_hex_is_lower(call_id, SNJ_ID_HEX_LEN)) ||
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
    item->kind = SNJ_ITEM_TOOL_CALL;
    item->phase = SNJ_PHASE_NONE;
    item->arguments = arguments;
    if (set_local_id(item->call_id, call_id) < 0 ||
        !(item->provider_item_id =
          snj_strdup_checked(provider_item_id, SNJ_MAX_PROVIDER_ID)) ||
        !(item->provider_call_id =
          snj_strdup_checked(provider_call_id, SNJ_MAX_PROVIDER_ID)) ||
        !(item->name = snj_strdup_checked(name, 64u))) {
        item_free(item);
        --graph->count;
        return -1;
    }
    if (account_last_item(graph) < 0) {
        item_free(item);
        --graph->count;
        return -1;
    }
    return 0;
}

int
snj_response_graph_add_call(struct snj_response_graph *graph,
                            const char *provider_item_id,
                            const char *provider_call_id,
                            const char *name, json_t *arguments)
{
    return add_call(graph, provider_item_id, provider_call_id, name,
                    arguments, NULL);
}

int
snj_response_graph_add_opaque(struct snj_response_graph *graph,
                              const char *provider_item_id,
                              const char *provider_type, json_t *payload)
{
    struct snj_response_item *item;
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
    item->kind = SNJ_ITEM_OPAQUE;
    item->phase = SNJ_PHASE_NONE;
    item->payload = payload;
    if (!(item->provider_item_id =
          snj_strdup_checked(provider_item_id, SNJ_MAX_PROVIDER_ID)) ||
        !(item->provider_type =
          snj_strdup_checked(provider_type, SNJ_MAX_PROVIDER_ID))) {
        item_free(item);
        --graph->count;
        return -1;
    }
    if (account_last_item(graph) < 0) {
        item_free(item);
        --graph->count;
        return -1;
    }
    return 0;
}

static int
identifiers_valid(const struct snj_response_graph *graph,
                  char *error, size_t error_size)
{
    for (size_t i = 0; i < graph->count; ++i) {
        const struct snj_response_item *item = &graph->items[i];

        if (public_kind(item->kind) &&
            !snj_hex_is_lower(item->local_item_id, SNJ_ID_HEX_LEN)) {
            set_error(error, error_size,
                      "response item %zu has an invalid local id", i);
            errno = EINVAL;
            return -1;
        }
        if (item->kind == SNJ_ITEM_TOOL_CALL &&
            !snj_hex_is_lower(item->call_id, SNJ_ID_HEX_LEN)) {
            set_error(error, error_size,
                      "response item %zu has an invalid call id", i);
            errno = EINVAL;
            return -1;
        }
        for (size_t j = 0; j < i; ++j) {
            if (public_kind(item->kind) &&
                public_kind(graph->items[j].kind) &&
                strcmp(item->local_item_id,
                       graph->items[j].local_item_id) == 0) {
                set_error(error, error_size,
                          "response graph repeats a local item id");
                errno = EINVAL;
                return -1;
            }
            if (item->kind == SNJ_ITEM_TOOL_CALL &&
                graph->items[j].kind == SNJ_ITEM_TOOL_CALL &&
                strcmp(item->call_id, graph->items[j].call_id) == 0) {
                set_error(error, error_size,
                          "response graph repeats a call id");
                errno = EINVAL;
                return -1;
            }
        }
    }
    return 0;
}

int
snj_response_graph_classify(const struct snj_response_graph *graph,
                            struct snj_graph_decision *decision,
                            char *error, size_t error_size)
{
    size_t terminal_count = 0;
    size_t terminal_index = 0;
    size_t last_speech = 0;
    bool have_speech = false;
    size_t calls = 0;
    size_t bad_index = 0;
    struct snj_buf encoded;

    memset(decision, 0, sizeof(*decision));
    if (!graph->provider_response_id || graph->count > SNJ_MAX_RESPONSE_ITEMS) {
        set_error(error, error_size, "response graph has no valid response id");
        errno = EINVAL;
        return -1;
    }
    if (identifiers_valid(graph, error, error_size) < 0)
        return -1;
    for (size_t i = 0; i < graph->count; ++i) {
        const struct snj_response_item *item = &graph->items[i];
        bad_index = i;
        if (!provider_id_valid(item->provider_item_id)) {
            set_error(error, error_size, "response item %zu has invalid identity", i);
            errno = EINVAL;
            return -1;
        }
        switch (item->kind) {
        case SNJ_ITEM_ASSISTANT:
            if ((item->phase != SNJ_PHASE_COMMENTARY &&
                 item->phase != SNJ_PHASE_FINAL_ANSWER) ||
                !text_valid(item->text, SNJ_MAX_PUBLIC_ITEM))
                goto bad_item;
            have_speech = true;
            last_speech = i;
            if (item->phase == SNJ_PHASE_FINAL_ANSWER) {
                ++terminal_count;
                terminal_index = i;
            }
            break;
        case SNJ_ITEM_REFUSAL:
            if (item->phase != SNJ_PHASE_FINAL_ANSWER ||
                !text_valid(item->text, SNJ_MAX_PUBLIC_ITEM))
                goto bad_item;
            have_speech = true;
            last_speech = i;
            ++terminal_count;
            terminal_index = i;
            break;
        case SNJ_ITEM_REASONING_SUMMARY:
            if (item->phase != SNJ_PHASE_SUMMARY ||
                !text_valid(item->text, SNJ_MAX_PUBLIC_ITEM))
                goto bad_item;
            break;
        case SNJ_ITEM_TOOL_CALL:
            if (!snj_hex_is_lower(item->call_id, SNJ_ID_HEX_LEN) ||
                !provider_id_valid(item->provider_call_id) ||
                !tool_name_valid(item->name) ||
                !arguments_bounded(item->arguments))
                goto bad_item;
            ++calls;
            break;
        case SNJ_ITEM_OPAQUE:
            if (!provider_id_valid(item->provider_type) ||
                !json_is_object(item->payload) || !arguments_bounded(item->payload))
                goto bad_item;
            break;
        }
    }
    if (calls > SNJ_MAX_CALLS_PER_RESPONSE) {
        set_error(error, error_size, "response graph exceeds 32 tool calls");
        errno = EOVERFLOW;
        return -1;
    }
    snj_buf_init(&encoded, SNJ_MAX_RESPONSE_GRAPH);
    {
        json_t *items = snj_response_graph_json(graph);
        int rc = items ? snj_json_canonical(items, &encoded) : -1;
        if (items)
            json_decref(items);
        snj_buf_free(&encoded);
        if (rc < 0) {
            set_error(error, error_size, "response graph exceeds 8 MiB");
            errno = EOVERFLOW;
            return -1;
        }
    }
    decision->call_count = calls;
    if (terminal_count > 1u || (terminal_count && calls) ||
        (terminal_count && (!have_speech || terminal_index != last_speech))) {
        decision->outcome = SNJ_GRAPH_CONFLICT;
        decision->message = terminal_count > 1u ?
            "provider response contained multiple terminal answers" :
            calls ? "provider response combined a terminal answer with tool calls" :
            "terminal answer was not the last assistant or refusal item";
        return 0;
    }
    if (terminal_count == 1u) {
        decision->final_index = terminal_index;
        decision->outcome = graph->items[terminal_index].kind == SNJ_ITEM_REFUSAL ?
                            SNJ_GRAPH_REFUSAL : SNJ_GRAPH_FINAL;
        return 0;
    }
    if (calls) {
        decision->outcome = SNJ_GRAPH_CALLS;
        return 0;
    }
    decision->outcome = SNJ_GRAPH_NONPRODUCTIVE;
    decision->message = "provider completed without a final answer, refusal, or tool call";
    return 0;

bad_item:
    set_error(error, error_size, "response item %zu has an invalid shape", bad_index);
    errno = EINVAL;
    return -1;
}

int
snj_tool_action_digest(const struct snj_response_item *call,
                       const char *resolved_workdir,
                       char out[SNJ_SHA256_HEX_LEN + 1u])
{
    json_t *action = json_object();
    int rc = -1;

    if (!call || call->kind != SNJ_ITEM_TOOL_CALL || !resolved_workdir ||
        !action ||
        snj_json_set_new(action, "arguments",
                         json_deep_copy(call->arguments)) < 0 ||
        snj_json_set_new(action, "name", json_string(call->name)) < 0 ||
        snj_json_set_new(action, "resolved_workdir",
                         json_string(resolved_workdir)) < 0 ||
        snj_json_digest(action, out) < 0)
        goto out;
    rc = 0;
out:
    if (action)
        json_decref(action);
    return rc;
}

static json_t *
item_json(const struct snj_response_item *item)
{
    json_t *out = json_object();
    if (!out)
        return NULL;
    if (snj_json_set_new(out, "kind", json_string(snj_item_kind_name(item->kind))) < 0)
        goto fail;
    switch (item->kind) {
    case SNJ_ITEM_ASSISTANT:
    case SNJ_ITEM_REFUSAL:
    case SNJ_ITEM_REASONING_SUMMARY:
        if (snj_json_set_new(out, "local_item_id",
                             json_string(item->local_item_id)) < 0 ||
            snj_json_set_new(out, "phase",
                             json_string(snj_item_phase_name(item->phase))) < 0 ||
            snj_json_set_new(out, "provider_item_id",
                             json_string(item->provider_item_id)) < 0 ||
            snj_json_set_new(out, "text", json_string(item->text)) < 0)
            goto fail;
        break;
    case SNJ_ITEM_TOOL_CALL:
        if (snj_json_set_new(out, "arguments", json_deep_copy(item->arguments)) < 0 ||
            snj_json_set_new(out, "call_id", json_string(item->call_id)) < 0 ||
            snj_json_set_new(out, "name", json_string(item->name)) < 0 ||
            snj_json_set_new(out, "provider_call_id",
                             json_string(item->provider_call_id)) < 0 ||
            snj_json_set_new(out, "provider_item_id",
                             json_string(item->provider_item_id)) < 0)
            goto fail;
        break;
    case SNJ_ITEM_OPAQUE:
        if (snj_json_set_new(out, "payload", json_deep_copy(item->payload)) < 0 ||
            snj_json_set_new(out, "provider_item_id",
                             json_string(item->provider_item_id)) < 0 ||
            snj_json_set_new(out, "provider_type",
                             json_string(item->provider_type)) < 0)
            goto fail;
        break;
    }
    return out;
fail:
    json_decref(out);
    return NULL;
}

json_t *
snj_response_graph_json(const struct snj_response_graph *graph)
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
parse_public(struct snj_response_graph *graph, const json_t *value,
             enum snj_item_kind kind, char *error, size_t error_size)
{
    static const char *const keys[] = {
        "kind", "local_item_id", "phase", "provider_item_id", "text"
    };
    const char *phase = snj_json_string(value, "phase");
    const char *provider_id = snj_json_string(value, "provider_item_id");
    const char *text = snj_json_string(value, "text");
    const char *local_id = snj_json_string(value, "local_item_id");
    enum snj_item_phase p;
    if (!snj_json_exact_keys((json_t *)value, keys, 5u) || !phase || !provider_id ||
        !text || !local_id || !snj_hex_is_lower(local_id, SNJ_ID_HEX_LEN))
        goto invalid;
    if (strcmp(phase, "commentary") == 0)
        p = SNJ_PHASE_COMMENTARY;
    else if (strcmp(phase, "final_answer") == 0)
        p = SNJ_PHASE_FINAL_ANSWER;
    else if (strcmp(phase, "summary") == 0)
        p = SNJ_PHASE_SUMMARY;
    else
        goto invalid;
    if (add_public(graph, kind, p, provider_id, text, local_id) < 0)
        goto invalid;
    return 0;
invalid:
    set_error(error, error_size, "invalid public response item");
    errno = EINVAL;
    return -1;
}

int
snj_response_graph_from_json(struct snj_response_graph *graph,
                             const json_t *items,
                             char *error, size_t error_size)
{
    if (!json_is_array(items) || json_array_size(items) > SNJ_MAX_RESPONSE_ITEMS) {
        set_error(error, error_size, "invalid response item array");
        errno = EINVAL;
        return -1;
    }
    for (size_t i = 0; i < json_array_size(items); ++i) {
        json_t *value = json_array_get(items, i);
        const char *kind = snj_json_string(value, "kind");
        if (!json_is_object(value) || !kind)
            goto invalid;
        if (strcmp(kind, "assistant") == 0) {
            if (parse_public(graph, value, SNJ_ITEM_ASSISTANT,
                             error, error_size) < 0)
                return -1;
        } else if (strcmp(kind, "refusal") == 0) {
            if (parse_public(graph, value, SNJ_ITEM_REFUSAL,
                             error, error_size) < 0)
                return -1;
        } else if (strcmp(kind, "reasoning_summary") == 0) {
            if (parse_public(graph, value, SNJ_ITEM_REASONING_SUMMARY,
                             error, error_size) < 0)
                return -1;
        } else if (strcmp(kind, "tool_call") == 0) {
            static const char *const keys[] = {
                "arguments", "call_id", "kind", "name", "provider_call_id",
                "provider_item_id"
            };
            const char *call_id = snj_json_string(value, "call_id");
            const char *name = snj_json_string(value, "name");
            const char *provider_call_id = snj_json_string(value, "provider_call_id");
            const char *provider_item_id = snj_json_string(value, "provider_item_id");
            json_t *arguments = json_object_get(value, "arguments");
            if (!snj_json_exact_keys(value, keys, 6u) || !call_id ||
                !snj_hex_is_lower(call_id, SNJ_ID_HEX_LEN) || !arguments ||
                add_call(graph, provider_item_id, provider_call_id, name,
                         json_deep_copy(arguments), call_id) < 0)
                goto invalid;
        } else if (strcmp(kind, "opaque") == 0) {
            static const char *const keys[] = {
                "kind", "payload", "provider_item_id", "provider_type"
            };
            const char *provider_item_id = snj_json_string(value, "provider_item_id");
            const char *provider_type = snj_json_string(value, "provider_type");
            json_t *payload = json_object_get(value, "payload");
            if (!snj_json_exact_keys(value, keys, 4u) || !payload ||
                snj_response_graph_add_opaque(graph, provider_item_id,
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
    set_error(error, error_size, "invalid response item at index %zu",
              graph->count);
    errno = EINVAL;
    return -1;
}

int
snj_partial_public_validate(const json_t *items,
                            char *error, size_t error_size)
{
    struct snj_response_graph graph;
    int rc = -1;

    snj_response_graph_init(&graph);
    if (snj_response_graph_from_json(&graph, items,
                                     error, error_size) < 0)
        goto out;
    for (size_t i = 0; i < graph.count; ++i) {
        if (!public_kind(graph.items[i].kind)) {
            set_error(error, error_size,
                      "partial public array contains a non-public item");
            errno = EINVAL;
            goto out;
        }
    }
    rc = 0;
out:
    snj_response_graph_free(&graph);
    return rc;
}

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
tool_result(const char *status, const char *reason, const char *model_text,
            int exit_code)
{
    json_t *out = json_object();
    if (!out ||
        snj_json_set_new(out, "duration_ms", json_integer(0)) < 0 ||
        snj_json_set_new(out, "exit_code",
                         exit_code >= 0 ? json_integer(exit_code) : json_null()) < 0 ||
        snj_json_set_new(out, "handle", json_null()) < 0 ||
        snj_json_set_new(out, "model_text", json_string(model_text)) < 0 ||
        snj_json_set_new(out, "reason",
                         reason ? json_string(reason) : json_null()) < 0 ||
        snj_json_set_new(out, "signal", json_null()) < 0 ||
        snj_json_set_new(out, "status", json_string(status)) < 0 ||
        snj_json_set_new(out, "stderr", empty_excerpt()) < 0 ||
        snj_json_set_new(out, "stdout", empty_excerpt()) < 0) {
        if (out)
            json_decref(out);
        return NULL;
    }
    return out;
}

json_t *
snj_tool_result_not_run(const char *reason)
{
    char text[192];
    (void)snprintf(text, sizeof(text), "Tool was not run: %s", reason);
    return tool_result("not_run", reason, text, -1);
}

json_t *
snj_tool_result_terminal(bool succeeded, const char *model_text)
{
    return tool_result(succeeded ? "succeeded" : "failed", NULL, model_text,
                       succeeded ? 0 : 1);
}

json_t *
snj_tool_result_denied(void)
{
    return tool_result("denied", "user_denied",
                       "Tool was denied by the user.", -1);
}

json_t *
snj_tool_result_outcome_unknown(const char *reason)
{
    char text[192];
    (void)snprintf(text, sizeof(text), "Tool outcome is unknown: %s", reason);
    return tool_result("outcome_unknown", reason, text, -1);
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

    if (!snj_json_exact_keys((json_t *)excerpt, keys, 5u) ||
        !(encoding = snj_json_string(excerpt, "encoding")) ||
        !(retained = snj_json_string(excerpt, "retained")) ||
        snj_json_integer_u64(excerpt, "discarded_bytes", &discarded) < 0 ||
        snj_json_integer_u64(excerpt, "original_bytes", &original) < 0 ||
        snj_json_integer_u64(excerpt, "retained_bytes", &retained_bytes) < 0)
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
                      strcmp(reason, "recovery_unstarted") == 0 ||
                      strcmp(reason, "superseded_by_steering") == 0 ||
                      strcmp(reason, "turn_cancelled") == 0 ||
                      strcmp(reason, "process_interaction_required") == 0);
}

int
snj_tool_result_valid(const json_t *result)
{
    static const char *const keys[] = {
        "duration_ms", "exit_code", "handle", "model_text", "reason",
        "signal", "status", "stderr", "stdout"
    };
    const char *status;
    const char *reason;
    const char *model_text;
    uint64_t duration;
    json_t *reason_value;
    json_t *handle;
    json_t *exit_value;
    json_t *signal_value;

    if (!snj_json_exact_keys((json_t *)result, keys, 9u) ||
        snj_json_integer_u64(result, "duration_ms", &duration) < 0 ||
        !(status = snj_json_string(result, "status")) ||
        !(model_text = snj_json_string(result, "model_text")) ||
        json_string_length(json_object_get(result, "model_text")) >
            512u * 1024u ||
        tool_excerpt_valid(json_object_get(result, "stdout")) < 0 ||
        tool_excerpt_valid(json_object_get(result, "stderr")) < 0)
        return -1;
    (void)model_text;
    reason_value = json_object_get(result, "reason");
    handle = json_object_get(result, "handle");
    exit_value = json_object_get(result, "exit_code");
    signal_value = json_object_get(result, "signal");
    reason = snj_json_string(result, "reason");
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
               snj_hex_is_lower(json_string_value(handle), SNJ_ID_HEX_LEN) &&
               json_is_null(reason_value) ? 0 : -1;
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

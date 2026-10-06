/* SPDX-License-Identifier: GPL-2.0-only */
#include "vm_public.h"
#include "history_view.h"
#include "vm_source.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char response[] = "0123456789abcdef0123456789abcdef";
static const char *secret_values[] = {"sk-testing-a-secret"};
static const struct snag_wire_secrets secrets = {.values = secret_values, .count = 1u};

static json_t *
public_item(const char *local, const char *text)
{
    return json_pack("{s:s,s:s,s:s,s:s}", "kind", "assistant", "phase", "commentary",
        "local_item_id", local, "text", text);
}

static void
append_event(json_t *events, const char *type, json_t *data)
{
    uint64_t seq = json_array_size(events) + 1u;
    char error[256] = "";
    char *filtered = snag_history_event_data(seq, type, data, &secrets, error, sizeof(error));
    assert(filtered);
    json_error_t parse;
    json_t *value = json_loads(filtered, 0u, &parse);
    free(filtered);
    assert(value);
    json_t *event = json_pack("{s:I,s:s,s:o}", "seq", (json_int_t)seq, "type", type, "data", value);
    assert(event && snag_vm_public_source_bytes(event, data) == 0);
    assert(json_array_append_new(events, event) == 0);
    json_decref(data);
}

static void
output(json_t *events, uint64_t ordinal, uint64_t offset, const char *text)
{
    append_event(events, "response_output", json_pack("{s:s,s:I,s:I,s:o}",
        "response_id", response, "index", (json_int_t)ordinal, "offset", (json_int_t)offset,
        "item", public_item("streaming-local-id", text)));
}

static json_t *
project(json_t *events)
{
    char error[256] = "";
    json_t *blocks = snag_vm_public_blocks(events, &secrets, error, sizeof(error));
    if (!blocks) (void)fprintf(stderr, "%s\n", error);
    assert(blocks);
    return blocks;
}

int
main(void)
{
    json_t *events = json_array();
    assert(events);
    output(events, 0u, 0u, "hello ");
    output(events, 0u, 6u, "world");
    json_t *blocks = project(events);
    assert(json_array_size(blocks) == 1u);
    const json_t *block = json_array_get(blocks, 0u);
    assert(!strcmp(snag_json_string(block, "text"), "hello world"));
    char key[64];
    assert(snag_strcpy(key, sizeof(key), snag_json_string(block, "key")));
    json_decref(blocks);
    json_t *items = json_array();
    assert(items && json_array_append_new(items, json_pack("{s:s}", "kind", "tool_call")) == 0);
    assert(json_array_append_new(items, public_item("different-final-id", "hello world!")) == 0);
    append_event(events, "response_completed", json_pack("{s:s,s:o,s:s}", "response_id", response,
        "items", items, "continuation", "private-provider-state"));
    blocks = project(events);
    assert(json_array_size(blocks) == 1u);
    block = json_array_get(blocks, 0u);
    assert(!strcmp(snag_json_string(block, "key"), key));
    assert(!strcmp(snag_json_string(block, "text"), "hello world!"));
    assert(!strcmp(snag_json_string(block, "state"), "complete"));
    assert(json_integer_value(json_object_get(block, "seq")) == 1);
    assert(json_integer_value(json_object_get(block, "last_seq")) == 3);
    char *dump = json_dumps(blocks, JSON_COMPACT);
    assert(dump && !strstr(dump, "private-provider-state"));
    free(dump);
    json_decref(blocks);
    json_array_clear(events);
    output(events, 0u, 0u, "sk-testing-");
    blocks = project(events);
    assert(!strcmp(snag_json_string(json_array_get(blocks, 0u), "text"), "<redacted:secret>"));
    json_decref(blocks);
    output(events, 0u, 11u, "a-secret");
    blocks = project(events);
    assert(!strcmp(snag_json_string(json_array_get(blocks, 0u), "text"), "<redacted:secret>"));
    json_decref(blocks);
    json_array_clear(events);
    output(events, 0u, 0u, "sk-testing-a-secret");
    output(events, 0u, strlen(secret_values[0]), " done");
    blocks = project(events);
    block = json_array_get(blocks, 0u);
    assert(!strcmp(snag_json_string(block, "text"), "<redacted:secret> done"));
    assert(json_integer_value(json_object_get(block, "source_end")) ==
        (json_int_t)strlen("sk-testing-a-secret done"));
    json_decref(blocks);
    json_array_clear(events);
    output(events, 0u, 20u, "continued");
    blocks = project(events);
    block = json_array_get(blocks, 0u);
    assert(json_integer_value(json_object_get(block, "source_begin")) == 20);
    assert(snag_vm_source_position(block, 3u, true) == 23u);
    assert(snag_vm_source_position(block, 23u, false) == 3u);
    json_decref(blocks);
    output(events, 0u, 99u, "gap");
    char error[256] = "";
    assert(!snag_vm_public_blocks(events, &secrets, error, sizeof(error)) && errno == EINVAL);
    json_array_clear(events);
    output(events, 0u, 0u, "visible before error");
    append_event(events, "response_failed", json_pack("{s:s,s:[]}",
        "response_id", response, "partial_public"));
    blocks = project(events);
    block = json_array_get(blocks, 0u);
    assert(!strcmp(snag_json_string(block, "text"), "visible before error"));
    assert(!strcmp(snag_json_string(block, "state"), "unconfirmed"));
    json_decref(blocks);
    json_array_clear(events);
    output(events, 0u, 0u, "partial");
    append_event(events, "response_interrupted", json_pack("{s:s,s:[o]}",
        "response_id", response, "partial_public", public_item("partial-id", "partial")));
    blocks = project(events);
    assert(json_array_size(blocks) == 1u &&
        !strcmp(snag_json_string(json_array_get(blocks, 0u), "state"), "interrupted"));
    json_decref(blocks);
    assert(json_array_append(events, json_array_get(events, 0u)) == 0);
    assert(!snag_vm_public_blocks(events, &secrets, error, sizeof(error)) && errno == EINVAL);
    json_array_clear(events);
    output(events, 0u, 11u, "a-secret and sk-testing-");
    blocks = project(events);
    assert(!strcmp(snag_json_string(json_array_get(blocks, 0u), "text"),
        "<redacted:secret> and <redacted:secret>"));
    block = json_array_get(blocks, 0u);
    assert(snag_vm_source_position(block, 0u, true) == 11u);
    assert(snag_vm_source_position(block, 16u, true) == 11u);
    assert(snag_vm_source_position(block, 17u, true) == 19u);
    assert(snag_vm_source_position(block, 19u, false) == 17u);
    json_decref(blocks);
    append_event(events, "response_failed", json_pack("{s:s,s:[]}",
        "response_id", response, "partial_public"));
    blocks = project(events);
    assert(!strcmp(snag_json_string(json_array_get(blocks, 0u), "text"),
        "<redacted:secret> and <redacted:secret>"));
    json_decref(blocks);
    json_decref(events);
    puts("test_vm_public: ok");
    return 0;
}

/* SPDX-License-Identifier: GPL-2.0-only */
#include "vm_transcript.h"
#include "base64.h"
#include "irc.h"
#include "render.h"
#include "vm_document.h"
#include "vm_source.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char response[] = "0123456789abcdef0123456789abcdef";
static const char handle[] = "fedcba9876543210fedcba9876543210";
static const char *secret_values[] = {"top-secret"};
static const struct snag_wire_secrets secrets = {.values = secret_values, .count = 1u};

static void
event(json_t *events, const char *type, json_t *data)
{
    assert(data);
    json_t *value = json_pack("{s:I,s:s,s:o}", "seq", (json_int_t)json_array_size(events) + 1,
        "type", type, "data", data);
    assert(value && json_array_append_new(events, value) == 0);
}

static json_t *
public_item(const char *text)
{
    return json_pack("{s:s,s:s,s:s,s:s}", "kind", "assistant", "phase", "commentary",
        "local_item_id", handle, "text", text);
}

static json_t *
project(const json_t *events, unsigned int level)
{
    char error[256];
    json_t *blocks = snag_vm_transcript_blocks(events, level, 80u, &secrets, NULL, NULL,
        error, sizeof(error));
    if (!blocks) (void)fprintf(stderr, "%s\n", error);
    assert(blocks);
    return blocks;
}

static json_t *
find(const json_t *blocks, const char *kind, size_t nth)
{
    for (size_t i = 0u; i < json_array_size(blocks); ++i) {
        json_t *block = json_array_get(blocks, i);
        if (!strcmp(snag_json_string(block, "kind"), kind) && !nth--) return block;
    }
    return NULL;
}

static void
query_conversations(void)
{
    json_t *events = json_array();
    struct snag_irc_event irc = {.routed = true, .kind = SNAG_IRC_MESSAGE,
        .timestamp_ms = 1u, .endpoint = "test:6667", .nick = "top-secret",
        .text = "operator query top-secret", .route = {
            .connection = "11111111111111111111111111111111",
            .conversation = "22222222222222222222222222222222",
            .generation = 1u, .identity = SNAG_IRC_OPERATOR, .kind = SNAG_IRC_QUERY,
            .peer = "top-secret", .target = "operator"}};
    event(events, "irc_event_v2", snag_irc_event_data(&irc));
    irc.route.identity = SNAG_IRC_AGENT;
    strcpy(irc.route.conversation, "33333333333333333333333333333333");
    strcpy(irc.route.target, "agent");
    strcpy(irc.text, "agent query top-secret");
    event(events, "irc_event_v2", snag_irc_event_data(&irc));
    for (unsigned int level = 0u; level <= 6u; ++level) {
        json_t *blocks = project(events, level);
        json_t *operator = find(blocks, "irc", 0u);
        json_t *agent = find(blocks, "irc", 1u);
        assert(operator && agent && strstr(snag_json_string(operator, "label"), "operator query") &&
            strstr(snag_json_string(agent, "label"), "agent query"));
        assert(!strcmp(snag_json_string(operator, "target"), "<redacted:secret>"));
        assert(strstr(snag_json_string(operator, "text"), "operator query <redacted:secret>") &&
            strstr(snag_json_string(agent, "text"), "agent query <redacted:secret>"));
        char *encoded = json_dumps(blocks, JSON_COMPACT);
        assert(encoded && !strstr(encoded, "top-secret"));
        free(encoded);
        json_decref(blocks);
    }
    json_decref(events);
}

static void
conversation_and_tools(void)
{
    json_t *events = json_array();
    event(events, "input_received", json_pack("{s:s}", "text", "question top-secret"));
    event(events, "response_output", json_pack("{s:s,s:i,s:i,s:o}", "response_id", response,
        "index", 0, "offset", 0, "item", public_item("answer")));
    json_t *call = json_pack("{s:s,s:s,s:s,s:{s:s}}", "kind", "tool_call", "call_id", handle,
        "name", "exec_command", "arguments", "cmd", "printf top-secret");
    event(events, "response_completed", json_pack("{s:s,s:[o,o],s:s}",
        "response_id", response, "items", public_item("answer"), call,
        "continuation", "provider-private-continuation"));
    event(events, "tool_started", json_pack("{s:s,s:s}", "call_id", handle,
        "resolved_workdir", "/work"));
    event(events, "tool_finished", json_pack("{s:s,s:{s:s,s:s,s:i}}", "call_id", handle,
        "result", "status", "succeeded", "model_text", "tool result top-secret", "exit_code", 0));
    struct snag_irc_event irc = {.kind = SNAG_IRC_MESSAGE, .timestamp_ms = 1u};
    strcpy(irc.endpoint, "test:6667");
    strcpy(irc.room, "#work");
    strcpy(irc.nick, "peer");
    strcpy(irc.text, "one message top-secret");
    event(events, "irc_event", snag_irc_event_data(&irc));
    event(events, "irc_admitted", json_pack("{s:{s:s}}", "input", "text", "duplicate admission"));
    event(events, "compaction_completed", json_pack("{s:[{s:s,s:s}]}", "output",
        "text", "private model context", "encrypted_content", "private-ciphertext"));
    char *before = json_dumps(events, JSON_COMPACT);
    json_t *plain = project(events, 0u);
    assert(json_array_size(plain) == 4u && find(plain, "assistant", 0u) &&
        !find(plain, "assistant", 1u) && find(plain, "irc", 0u));
    assert(!find(plain, "tool_started", 0u) && !find(plain, "irc_admitted", 0u));
    json_t *details = project(events, 2u);
    json_t *start = find(details, "tool_started", 0u), *finish = find(details, "tool_finished", 0u);
    assert(start && finish && strstr(snag_json_string(start, "label"), "exec_command"));
    assert(!json_object_get(start, "needs_call"));
    struct snag_response_item expected_call = {.name = "exec_command", .call_id = handle,
        .arguments = json_pack("{s:s}", "cmd", "printf <redacted:secret>")};
    struct snag_render_block expected;
    assert(snag_render_prepare_tool_start(&expected, &expected_call, "/work", 0u, 2u, 80u) == 0);
    assert(strlen(snag_json_string(start, "label")) == expected.text.len &&
        !memcmp(snag_json_string(start, "label"), expected.text.data, expected.text.len));
    snag_render_block_free(&expected);
    json_decref(expected_call.arguments);
    assert(strstr(snag_json_string(finish, "text"), "tool result <redacted:secret>"));
    json_t *debug = project(events, 6u);
    assert(strstr(snag_json_string(find(debug, "tool_started", 0u), "text"),
        "timeout: unrecorded"));
    char *dump = json_dumps(debug, JSON_COMPACT), *after = json_dumps(events, JSON_COMPACT);
    assert(dump && before && after && !strcmp(before, after));
    assert(!strstr(dump, "top-secret") && !strstr(dump, "provider-private-continuation") &&
        !strstr(dump, "private-ciphertext") && !strstr(dump, "private model context"));
    free(before);
    free(after);
    free(dump);
    json_decref(debug);
    json_decref(details);
    json_decref(plain);
    json_decref(events);
}

static int
encoded_bytes(void *opaque, const unsigned char *bytes, size_t length)
{
    return snag_buf_append(opaque, bytes, length);
}

static void
output(json_t *events, unsigned int stream, size_t offset, const void *bytes, size_t length)
{
    struct snag_buf encoded = {.max = 65536u};
    struct snag_base64_stream encoder = {0};
    assert(snag_base64_write(&encoder, bytes, length, encoded_bytes, &encoded) == 0 &&
        snag_base64_finish(&encoder, encoded_bytes, &encoded) == 0 &&
        snag_buf_terminate(&encoded) == 0);
    event(events, "process_output", json_pack("{s:s,s:s,s:i,s:I,s:s,s:s}", "turn_id", response,
        "handle", handle, "stream", stream, "offset", (json_int_t)offset,
        "encoding", "base64", "data", (const char *)encoded.data));
    snag_buf_free(&encoded);
}

static void
encoded_interleaving(void)
{
    json_t *events = json_array();
    output(events, 0u, 0u, "before top-", 11u);
    output(events, 1u, 0u, "stderr\n", 7u);
    output(events, 0u, 11u, "secret after\n\xe2", 14u);
    const unsigned char tail[] = {0x82, 0xac, 0, 0xff, 0x1b, '[', '2', 'J'};
    output(events, 0u, 25u, tail, sizeof(tail));
    json_t *blocks = project(events, 3u);
    assert(json_array_size(blocks) == 4u);
    assert(json_integer_value(json_object_get(json_array_get(blocks, 0u), "last_seq")) == 3);
    assert(json_integer_value(json_object_get(json_array_get(blocks, 2u), "last_seq")) == 4);
    assert(!strcmp(snag_json_string(json_array_get(blocks, 0u), "text"),
        "before <redacted:secret>"));
    assert(!strcmp(snag_json_string(json_array_get(blocks, 1u), "text"), "stderr\n"));
    assert(!strcmp(snag_json_string(json_array_get(blocks, 2u), "text"), " after\n€"));
    assert(!strcmp(snag_json_string(json_array_get(blocks, 3u), "text"), "\\x00\\xFF\033[2J"));
    assert(strstr(snag_json_string(json_array_get(blocks, 3u), "label"), "binary escaped"));
    assert(json_integer_value(json_object_get(json_array_get(blocks, 0u), "source_end")) == 17);
    assert(json_integer_value(json_object_get(json_array_get(blocks, 2u), "source_begin")) == 17);
    assert(json_integer_value(json_object_get(json_array_get(blocks, 2u), "source_end")) == 27);
    assert(json_integer_value(json_object_get(json_array_get(blocks, 3u), "source_begin")) == 27);
    char *dump = json_dumps(blocks, JSON_COMPACT);
    assert(dump && !strstr(dump, "top-secret") && !strstr(dump, "YmVmb3JlIHRvcC0="));
    free(dump);
    json_decref(blocks);
    json_decref(events);
    /* A page starting/ending in a protected value must not reveal its pieces. */
    events = json_array();
    output(events, 0u, 42u, "secret and top-", 15u);
    blocks = project(events, 3u);
    assert(!strcmp(snag_json_string(json_array_get(blocks, 0u), "text"),
        "<redacted:secret> and <redacted:secret>"));
    json_decref(blocks);
    json_decref(events);
}

static void
redaction_expansion(void)
{
    const char *values[] = {"a"};
    struct snag_wire_secrets one = {.values = values, .count = 1u};
    char bytes[16384], error[256];
    memset(bytes, 'a', sizeof(bytes));
    json_t *events = json_array();
    output(events, 0u, 0u, bytes, sizeof(bytes));
    json_t *blocks = snag_vm_transcript_blocks(events, 3u, 80u, &one, NULL, NULL,
        error, sizeof(error));
    assert(blocks && json_array_size(blocks) == 1u);
    json_t *block = json_array_get(blocks, 0u);
    assert(strlen(snag_json_string(block, "text")) == 17u * sizeof(bytes));
    assert(json_array_size(json_object_get(block, "source_map")) == 1u);
    for (size_t i = 0u; i < sizeof(bytes); ++i) {
        assert(snag_vm_source_position(block, 17u * i + 8u, true) == i);
        assert(snag_vm_source_position(block, i, false) == 17u * i);
    }
    assert(snag_vm_source_position(block, 17u * sizeof(bytes), true) == sizeof(bytes));
    json_decref(blocks);
    json_decref(events);
}

static bool
cancel(void *opaque)
{
    unsigned int *remaining = opaque;
    return !*remaining || !--*remaining;
}

static void
sparse_document(void)
{
    char *text = malloc(65537u);
    assert(text);
    memset(text, 'x', 65536u);
    text[65536u] = 0;
    json_t *blocks = json_pack("[{s:s,s:i,s:s,s:s},{s:s,s:i,s:s,s:s}]",
        "key", "first", "seq", 1, "label", "title\n", "text", text,
        "key", "second", "seq", 2, "label", "next", "text", "界\t€\033\nend");
    free(text);
    for (unsigned int width = 1u; width <= 80u; width *= 2u) {
        struct snag_vm_document *doc = snag_vm_document_open(blocks, width, NULL, NULL);
        assert(doc);
        for (size_t i = 0u; i < snag_vm_document_rows(doc); i += 17u) {
            struct snag_vm_document_row row;
            assert(snag_vm_document_row(doc, i, &row) == 0);
            assert(row.begin <= row.end);
            const json_t *block = snag_vm_document_block(doc, row.block);
            assert(snag_vm_document_locate(doc, snag_json_string(block, "key"),
                (uint64_t)json_integer_value(json_object_get(block, "seq")),
                row.begin, row.heading) == i);
            assert(snag_vm_document_text(doc, &row));
        }
        struct snag_vm_document *copy = snag_vm_document_ref(doc);
        snag_vm_document_free(doc);
        assert(snag_vm_document_locate(copy, "removed", 3u, 0u, true) ==
            snag_vm_document_rows(copy) - 1u);
        snag_vm_document_free(copy);
    }
    unsigned int remaining = 200u;
    assert(!snag_vm_document_open(blocks, 1u, cancel, &remaining) && errno == ECANCELED);
    json_decref(blocks);
}

static void
resolved_call_metadata(void)
{
    json_t *events = json_array();
    event(events, "tool_started", json_pack("{s:s,s:s}", "call_id", handle,
        "resolved_workdir", "/work"));
    json_t *call = json_pack("{s:s,s:s,s:s,s:{s:s}}", "kind", "tool_call", "call_id", handle,
        "name", "exec_command", "arguments", "cmd", "echo top-secret");
    assert(json_object_set_new(json_array_get(events, 0u), "call", call) == 0);
    json_t *blocks = project(events, 2u);
    json_t *block = json_array_get(blocks, 0u);
    assert(strstr(snag_json_string(block, "label"), "exec_command"));
    assert(!json_object_get(block, "needs_call"));
    char *encoded = json_dumps(blocks, JSON_COMPACT);
    assert(encoded && !strstr(encoded, "top-secret") && strstr(encoded, "<redacted:secret>"));
    free(encoded);
    json_decref(blocks);
    assert(json_object_set_new(json_array_get(events, 0u), "call", json_null()) == 0);
    blocks = project(events, 2u);
    assert(!strcmp(snag_json_string(json_array_get(blocks, 0u), "needs_call"), handle));
    json_decref(blocks);
    json_decref(events);
}

static void
source_coordinates(void)
{
    json_t *events = json_array();
    event(events, "input_received", json_pack("{s:s}", "text", "top-secret\nmarker"));
    event(events, "response_output", json_pack("{s:s,s:i,s:i,s:o}", "response_id", response,
        "index", 0, "offset", 50, "item", public_item("prefix top-secret\nmarker 界")));
    const unsigned char bytes[] = {0xff, 0, 'a', '\n', 'm', 'a', 'r', 'k', 'e', 'r'};
    output(events, 0u, 90u, bytes, sizeof(bytes));
    json_t *blocks = project(events, 3u);
    json_t *input = find(blocks, "input_received", 0u);
    json_t *answer = find(blocks, "assistant", 0u);
    json_t *process = find(blocks, "output", 0u);
    assert(snag_vm_source_position(input, 18u, true) == 11u);
    assert(snag_vm_source_position(answer, 25u, true) == 68u);
    assert(snag_vm_source_position(answer, 68u, false) == 25u);
    assert(snag_vm_source_position(process, 0u, true) == 90u);
    assert(snag_vm_source_position(process, 4u, true) == 91u);
    assert(snag_vm_source_position(process, 8u, true) == 92u);
    assert(snag_vm_source_position(process, 94u, false) == 10u);
    const char *key = snag_json_string(answer, "key");
    for (unsigned int width = 5u; width < 80u; width += 11u) {
        struct snag_vm_document *doc = snag_vm_document_open(blocks, width, NULL, NULL);
        assert(doc);
        size_t at = snag_vm_document_locate_source(doc, key, 2u, 68u, false);
        struct snag_vm_document_row row;
        assert(snag_vm_document_row(doc, at, &row) == 0 && !row.heading);
        assert(!strncmp(snag_vm_document_text(doc, &row) + row.begin, "mark", 4u));
        assert(snag_vm_document_source(doc, &row, row.begin) == 68u);
        snag_vm_document_free(doc);
    }
    json_decref(blocks);
    /* The same public item can be loaded with an earlier prefix. Its source
     * coordinate keeps the marker in place despite the changed display offset. */
    json_t *data = json_object_get(json_array_get(events, 1u), "data");
    assert(json_object_set_new(data, "offset", json_integer(43)) == 0);
    assert(json_object_set_new(json_object_get(data, "item"), "text",
        json_string("earlierprefix top-secret\nmarker 界")) == 0);
    blocks = project(events, 3u);
    answer = find(blocks, "assistant", 0u);
    assert(snag_vm_source_position(answer, 68u, false) == 32u);
    assert(!strncmp(snag_json_string(answer, "text") + 32u, "marker", 6u));
    json_decref(blocks);
    json_decref(events);
}

static void
failure_paths(void)
{
    char error[256];
    json_t *events = json_array();
    event(events, "tool_started", json_pack("{s:s,s:s}", "call_id", handle,
        "resolved_workdir", "/work"));
    json_t *blocks = project(events, 1u);
    assert(!strcmp(snag_json_string(json_array_get(blocks, 0u), "needs_call"), handle));
    json_decref(blocks);
    output(events, 0u, 0u, "abc", 3u);
    output(events, 0u, 7u, "gap", 3u);
    assert(!snag_vm_transcript_blocks(events, 3u, 80u, &secrets, NULL, NULL,
        error, sizeof(error)) && errno == EINVAL);
    assert(json_array_remove(events, 2u) == 0);
    unsigned int remaining = 3u;
    assert(!snag_vm_transcript_blocks(events, 3u, 80u, &secrets, cancel, &remaining,
        error, sizeof(error)) && errno == ECANCELED);
    json_decref(events);
}

int
main(void)
{
    query_conversations();
    conversation_and_tools();
    encoded_interleaving();
    redaction_expansion();
    source_coordinates();
    sparse_document();
    resolved_call_metadata();
    failure_paths();
    puts("test_vm_transcript: ok");
    return 0;
}

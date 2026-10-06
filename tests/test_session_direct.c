/* SPDX-License-Identifier: GPL-2.0-only */
#include "session_view.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static json_t *
receive(struct snag_view_channel *channel)
{
    uint64_t until = snag_monotonic_ms() + 5000u;
    while (snag_monotonic_ms() < until) {
        json_t *value = NULL;
        int rc = snag_view_channel_read(channel, &value);
        assert(rc >= 0);
        if (rc) return value;
        assert(snag_sleep_ms(1u) == 0);
    }
    assert(!"private channel timed out");
    return NULL;
}

static void
send_value(struct snag_view_channel *channel, const json_t *value)
{
    assert(snag_view_channel_send(channel, value) == 0);
    uint64_t until = snag_monotonic_ms() + 5000u;
    while (channel->output && snag_monotonic_ms() < until) {
        assert(snag_view_channel_write(channel) >= 0);
        if (channel->output) assert(snag_sleep_ms(1u) == 0);
    }
    assert(!channel->output);
}

static void *
echo(void *opaque)
{
    struct snag_view_channel *channel = opaque;
    json_t *value = receive(channel);
    send_value(channel, value);
    json_decref(value);
    snag_view_channel_close(channel);
    return NULL;
}

static void
test_pair(void)
{
    struct snag_view_channel pair[2];
    assert(snag_view_channel_pair(pair) == 0);
    assert(snag_view_channel_opened(&pair[0]) && snag_view_channel_opened(&pair[1]));
    json_t *value = NULL;
    assert(snag_view_channel_read(&pair[0], &value) == 0 && !value);

    size_t length = 3u * SNAG_SESSION_FRAME_MAX;
    char *text = malloc(length + 1u);
    assert(text);
    memset(text, 'a', length);
    memcpy(text + length - 8u, "\"\nžluť", 8u);
    text[length] = '\0';
    json_t *message = json_pack("{s:s}", "text", text);
    assert(message && snag_view_channel_send(&pair[0], message) == 0);
    assert(snag_view_channel_write(&pair[0]) == 0);
    size_t first = pair[0].output_offset;
    assert(first > 0u && first < length);
    assert(snag_view_channel_write(&pair[0]) == 0);
    assert(pair[0].output_offset == first);
    assert(snag_view_channel_send(&pair[0], message) < 0 && errno == EAGAIN);

    pthread_t thread;
    assert(pthread_create(&thread, NULL, echo, &pair[1]) == 0);
    uint64_t until = snag_monotonic_ms() + 5000u;
    while (pair[0].output && snag_monotonic_ms() < until) {
        assert(snag_view_channel_write(&pair[0]) >= 0);
        assert(snag_sleep_ms(1u) == 0);
    }
    assert(!pair[0].output);
    value = receive(&pair[0]);
    assert(json_equal(value, message));
    assert(!strcmp(snag_json_string(value, "text"), text));
    assert(pthread_join(thread, NULL) == 0);
    json_decref(value);
    assert(snag_view_channel_read(&pair[0], &value) < 0 && errno == ECONNRESET);
    assert(snag_view_channel_send(&pair[0], message) == 0);
    assert(snag_view_channel_write(&pair[0]) < 0 && errno == EPIPE);
    snag_view_channel_close(&pair[0]);
    assert(!snag_view_channel_opened(&pair[0]) && !snag_view_channel_opened(&pair[1]));
    snag_view_channel_close(&pair[0]);
    json_decref(message);
    free(text);
}

static void
test_partial_close(void)
{
    struct snag_view_channel pair[2];
    assert(snag_view_channel_pair(pair) == 0);
    char *text = malloc(2u * SNAG_SESSION_FRAME_MAX + 1u);
    assert(text);
    memset(text, 'b', 2u * SNAG_SESSION_FRAME_MAX);
    text[2u * SNAG_SESSION_FRAME_MAX] = '\0';
    json_t *message = json_pack("{s:s}", "text", text);
    assert(message && snag_view_channel_send(&pair[0], message) == 0);
    json_decref(message);
    free(text);
    assert(snag_view_channel_write(&pair[0]) == 0);
    snag_view_channel_close(&pair[0]);
    json_t *value = NULL;
    assert(snag_view_channel_read(&pair[1], &value) == 0 && !value);
    assert(snag_view_channel_read(&pair[1], &value) < 0 && errno == ECONNRESET);
    snag_view_channel_close(&pair[1]);
}

struct engine {
    uint64_t generation;
    unsigned int submissions, cancels, quits;
};

static void
bound(void *opaque, uint64_t generation)
{
    ((struct engine *)opaque)->generation = generation;
}

static int
submit(void *opaque, const char *id, const char *text, const json_t *route,
    uint64_t generation, bool terminal)
{
    struct engine *engine = opaque;
    assert(generation == engine->generation && generation && !terminal);
    assert(strlen(id) == SNAG_ID_HEX_LEN && !strcmp(text, "direct prompt ž"));
    assert(!strcmp(json_string_value(route), "rollout"));
    ++engine->submissions;
    return 0;
}

static int
control(void *opaque, bool quit)
{
    struct engine *engine = opaque;
    if (quit) ++engine->quits;
    else ++engine->cancels;
    return 0;
}

static json_t *
exchange(struct snag_view_server *server, struct snag_view_channel *peer,
    json_t *request, const char *type)
{
    assert(request && snag_view_channel_send(peer, request) == 0);
    json_decref(request);
    uint64_t until = snag_monotonic_ms() + 5000u;
    while (snag_monotonic_ms() < until) {
        if (peer->output) assert(snag_view_channel_write(peer) >= 0);
        snag_view_server_step(server);
        json_t *reply = NULL;
        int rc = snag_view_channel_read(peer, &reply);
        assert(rc >= 0);
        if (rc && !strcmp(snag_json_string(reply, "type"), type)) return reply;
        json_decref(reply);
        assert(snag_sleep_ms(1u) == 0);
    }
    assert(!"direct server reply timed out");
    return NULL;
}

static bool
feature(const json_t *reply, const char *name)
{
    const json_t *features = json_object_get(reply, "features");
    for (size_t i = 0u; i < json_array_size(features); ++i)
        if (!strcmp(json_string_value(json_array_get(features, i)), name)) return true;
    return false;
}

static void
test_server(void)
{
    struct snag_view_channel pair[2];
    assert(snag_view_channel_pair(pair) == 0);
    struct engine engine = {0};
    struct snag_view_callbacks callbacks = {bound, submit, control, &engine};
    const char *session = "11111111111111111111111111111111";
    const char *request = "22222222222222222222222222222222";
    struct snag_view_server *server = snag_view_server_direct(&pair[1], session, callbacks);
    assert(server && !snag_view_channel_opened(&pair[1]));
    struct snag_view_channel *peer = &pair[0];
    json_t *reply = exchange(server, peer, json_pack("{s:s,s:i}",
        "type", "hello", "version", 1), "capabilities");
    assert(!strcmp(snag_json_string(reply, "session"), session));
    assert(feature(reply, "direct") && feature(reply, "drafts") && feature(reply, "receipts"));
    assert(!feature(reply, "detach") && !feature(reply, "terminal_commands"));
    json_decref(reply);
    reply = exchange(server, peer, json_pack("{s:s}", "type", "reserve"), "reserved");
    uint64_t generation = (uint64_t)json_integer_value(json_object_get(reply, "generation"));
    assert(generation && !engine.generation);
    json_decref(reply);
    reply = exchange(server, peer, json_pack("{s:s,s:I}", "type", "commit",
        "generation", (json_int_t)generation), "bound");
    json_decref(reply);
    assert(engine.generation == generation && snag_view_server_attached(server));

    reply = exchange(server, peer, json_pack("{s:s,s:I,s:s,s:i,s:i,s:s,s:i}",
        "type", "draft", "generation", (json_int_t)generation, "route", "rollout",
        "revision", 1, "edit", 1, "text", "direct prompt ž", "cursor", 0), "draft");
    assert(!strcmp(snag_json_string(reply, "status"), "accepted"));
    json_decref(reply);
    reply = exchange(server, peer, json_pack("{s:s,s:I,s:s,s:s,s:s,s:i}",
        "type", "submit", "generation", (json_int_t)generation, "route", "rollout",
        "id", request, "text", "direct prompt ž", "draft_revision", 2), "result");
    assert(!strcmp(snag_json_string(reply, "status"), "pending") && engine.submissions == 1u);
    json_decref(reply);
    reply = exchange(server, peer, json_pack("{s:s,s:s}", "type", "receipt", "id", request),
        "result");
    assert(!strcmp(snag_json_string(reply, "status"), "pending"));
    json_decref(reply);
    reply = exchange(server, peer, json_pack("{s:s,s:I,s:s,s:i,s:i,s:s,s:i}",
        "type", "draft", "generation", (json_int_t)generation, "route", "rollout",
        "revision", 2, "edit", 2, "text", "newer unsent draft", "cursor", 0), "draft");
    json_decref(reply);
    assert(snag_view_server_result(server, request, "committed", 17u, "input_received") == 0);
    reply = exchange(server, peer, json_pack("{s:s,s:s}", "type", "receipt", "id", request),
        "result");
    assert(!strcmp(snag_json_string(reply, "status"), "committed"));
    assert(json_integer_value(json_object_get(reply, "draft_cleared")) == 0);
    json_decref(reply);
    reply = exchange(server, peer, json_pack("{s:s,s:I,s:s}", "type", "draft_get",
        "generation", (json_int_t)generation, "route", "rollout"), "draft");
    assert(!strcmp(snag_json_string(json_object_get(reply, "draft"), "text"),
        "newer unsent draft"));
    json_decref(reply);
    reply = exchange(server, peer, json_pack("{s:s,s:I,s:s,s:s,s:s}",
        "type", "submit", "generation", (json_int_t)generation, "route", "rollout",
        "id", request, "text", "direct prompt ž"), "result");
    assert(!strcmp(snag_json_string(reply, "status"), "committed") && engine.submissions == 1u);
    json_decref(reply);
    reply = exchange(server, peer, json_pack("{s:s,s:I,s:s,s:s,s:s}",
        "type", "submit", "generation", (json_int_t)generation, "route", "rollout",
        "id", request, "text", "changed request body"), "error");
    assert(strstr(snag_json_string(reply, "message"), "already used"));
    json_decref(reply);
    reply = exchange(server, peer, json_pack("{s:s,s:I}", "type", "cancel",
        "generation", (json_int_t)(generation + 1u)), "error");
    assert(strstr(snag_json_string(reply, "message"), "stale controller"));
    json_decref(reply);
    assert(!engine.cancels && engine.submissions == 1u);

    reply = exchange(server, peer, json_pack("{s:s,s:I}", "type", "detach",
        "generation", (json_int_t)generation), "error");
    assert(strstr(snag_json_string(reply, "message"), "direct session"));
    json_decref(reply);
    assert(snag_view_server_attached(server) && engine.generation == generation && !engine.quits);
    reply = exchange(server, peer, json_pack("{s:s,s:I}", "type", "cancel",
        "generation", (json_int_t)generation), "control");
    json_decref(reply);
    assert(engine.cancels == 1u && !engine.quits);
    reply = exchange(server, peer, json_pack("{s:s,s:I}", "type", "quit",
        "generation", (json_int_t)generation), "control");
    json_decref(reply);
    assert(engine.quits == 1u);
    snag_view_server_exit(server, 7u);
    snag_view_server_step(server);
    snag_view_server_step(server);
    reply = receive(peer);
    assert(!strcmp(snag_json_string(reply, "type"), "exit"));
    assert(json_integer_value(json_object_get(reply, "status")) == 7);
    json_decref(reply);
    snag_view_channel_close(peer);
    snag_view_server_step(server);
    assert(!snag_view_server_busy(server) && !engine.generation && engine.quits == 1u);
    snag_view_server_close(server);
}

static void
test_lost_workspace(void)
{
    struct snag_view_channel pair[2];
    assert(snag_view_channel_pair(pair) == 0);
    struct engine engine = {0};
    struct snag_view_callbacks callbacks = {bound, submit, control, &engine};
    struct snag_view_server *server = snag_view_server_direct(&pair[1],
        "11111111111111111111111111111111", callbacks);
    assert(server);
    snag_view_channel_close(&pair[0]);
    snag_view_server_step(server);
    assert(engine.quits == 1u && !snag_view_server_busy(server));
    snag_view_server_step(server);
    assert(engine.quits == 1u);
    snag_view_server_close(server);
}

int
main(void)
{
    test_pair();
    test_partial_close();
    test_server();
    test_lost_workspace();
    (void)puts("direct session transport and protocol: ok");
    return 0;
}

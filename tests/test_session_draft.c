/* SPDX-License-Identifier: GPL-2.0-only */
#include "session_view.h"
#include "fs.h"
#include "irc.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef _WIN32
#include <fcntl.h>

/* The real transport and presentation owner run normally. This engine seam
 * deliberately withholds admission, making edit/commit ordering deterministic
 * without scheduler races or signal stops of production-like worker threads. */
struct engine {
    uint64_t generation;
    unsigned int submissions;
};

static void
bound(void *opaque, uint64_t generation)
{
    ((struct engine *)opaque)->generation = generation;
}

static int
submit(void *opaque, const char *id, const char *text, const json_t *route, uint64_t generation,
    bool terminal)
{
    assert(!terminal && route);
    struct engine *engine = opaque;
    assert(strlen(id) == SNAG_ID_HEX_LEN && *text && generation == engine->generation);
    ++engine->submissions;
    return 0;
}

static int
control(void *opaque, bool quit)
{
    (void)opaque;
    (void)quit;
    return 0;
}

static json_t *
exchange(struct snag_view_server *server, struct snag_view_channel *peer,
    json_t *request, const char *type)
{
    assert(request && snag_view_channel_send(peer, request) == 0);
    json_decref(request);
    uint64_t deadline = snag_monotonic_ms() + 5000u;
    while (snag_monotonic_ms() < deadline) {
        if (peer->output) assert(snag_view_channel_write(peer) >= 0);
        snag_view_server_step(server);
        json_t *reply = NULL;
        int rc = snag_view_channel_read(peer, &reply);
        assert(rc >= 0);
        if (rc && !strcmp(snag_json_string(reply, "type"), type)) return reply;
        json_decref(reply);
        assert(snag_sleep_ms(1u) == 0);
    }
    assert(!"owner reply timed out");
    return NULL;
}

static uint64_t
attach(struct snag_view_server *server, struct snag_view_channel *peer, int dir, const char *path)
{
    int fd = snag_session_view_connect(dir, path);
    assert(fd >= 0);
    snag_view_channel_init(peer, fd);
    json_t *reply = exchange(server, peer, json_pack("{s:s,s:i}",
        "type", "hello", "version", 1), "capabilities");
    json_decref(reply);
    reply = exchange(server, peer, json_pack("{s:s}", "type", "reserve"), "reserved");
    uint64_t generation = (uint64_t)json_integer_value(json_object_get(reply, "generation"));
    json_decref(reply);
    reply = exchange(server, peer, json_pack("{s:s,s:I}", "type", "commit",
        "generation", (json_int_t)generation), "bound");
    json_decref(reply);
    return generation;
}

static uint64_t
edit_route(struct snag_view_server *server, struct snag_view_channel *peer,
    uint64_t generation, uint64_t revision, const char *text, const json_t *route)
{
    json_t *reply = exchange(server, peer,
        json_pack("{s:s,s:I,s:O,s:I,s:i,s:s,s:I}", "type", "draft",
        "generation", (json_int_t)generation, "route", route,
        "revision", (json_int_t)revision, "edit", 1, "text", text,
        "cursor", (json_int_t)strlen(text)), "draft");
    assert(!strcmp(snag_json_string(reply, "status"), "accepted"));
    uint64_t next = (uint64_t)json_integer_value(
        json_object_get(json_object_get(reply, "draft"), "revision"));
    assert(next == revision + 1u);
    json_decref(reply);
    return next;
}

static uint64_t
edit(struct snag_view_server *server, struct snag_view_channel *peer,
    uint64_t generation, uint64_t revision, const char *text)
{
    json_t *route = json_string("rollout");
    uint64_t next = edit_route(server, peer, generation, revision, text, route);
    json_decref(route);
    return next;
}

static void
assert_route(struct snag_view_server *server, struct snag_view_channel *peer,
    uint64_t generation, uint64_t revision, const char *text, const json_t *route)
{
    json_t *reply = exchange(server, peer, json_pack("{s:s,s:I,s:O}", "type", "draft_get",
        "generation", (json_int_t)generation, "route", route), "draft");
    json_t *draft = json_object_get(reply, "draft");
    assert(json_equal(json_object_get(draft, "route"), route));
    assert(!strcmp(snag_json_string(draft, "text"), text));
    assert(json_integer_value(json_object_get(draft, "revision")) == (json_int_t)revision);
    assert(json_integer_value(json_object_get(draft, "cursor")) == (json_int_t)strlen(text));
    json_decref(reply);
}

static void
assert_draft(struct snag_view_server *server, struct snag_view_channel *peer,
    uint64_t generation, uint64_t revision, const char *text)
{
    json_t *route = json_string("rollout");
    assert_route(server, peer, generation, revision, text, route);
    json_decref(route);
}

static void
admit_later(struct snag_view_server *server, struct snag_view_channel *peer,
    uint64_t generation, uint64_t revision, const char *id, const char *text)
{
    json_t *reply = exchange(server, peer, json_pack("{s:s,s:I,s:s,s:I,s:s,s:s}",
        "type", "submit", "generation", (json_int_t)generation, "route", "rollout",
        "draft_revision", (json_int_t)revision, "id", id, "text", text), "result");
    assert(!strcmp(snag_json_string(reply, "status"), "pending"));
    json_decref(reply);
}

static void
assert_receipt(struct snag_view_server *server, struct snag_view_channel *peer,
    const char *id, uint64_t cleared)
{
    json_t *reply = exchange(server, peer, json_pack("{s:s,s:s}", "type", "receipt",
        "id", id), "result");
    assert(!strcmp(snag_json_string(reply, "status"), "committed"));
    assert(json_integer_value(json_object_get(reply, "draft_cleared")) == (json_int_t)cleared);
    json_decref(reply);
}

static void
test_admission_edit_order(void)
{
    char *root = snag_path_join(getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp",
        "snag-view-draft-XXXXXX");
    assert(root && mkdtemp(root));
    int dir = open(root, O_RDONLY | O_DIRECTORY);
    assert(dir >= 0);
    int lock = snag_create_private_at(dir, "lock", true);
    assert(lock >= 0 && snag_lock_file(lock, false) == 0);
    int pipe_fd[2];
    assert(pipe(pipe_fd) == 0);
    struct snag_session_relay relay;
    assert(snag_session_relay_init(&relay, pipe_fd[0], -1) == 0);
    struct engine engine = {0};
    struct snag_view_callbacks callbacks = {bound, submit, control, &engine};
    struct snag_view_server *server = snag_view_server_open(dir, root, lock,
        "0123456789abcdef0123456789abcdef", &relay, callbacks);
    assert(server);
    struct snag_view_channel peer;
    uint64_t generation = attach(server, &peer, dir, root);
    uint64_t revision = edit(server, &peer, generation, 1u, "first prompt");
    const char *first = "11111111111111111111111111111111";
    admit_later(server, &peer, generation, revision, first, "first prompt");
    revision = edit(server, &peer, generation, revision, "newer draft");
    assert(snag_view_server_result(server, first, "committed", 2u, "input_received") == 0);
    assert_receipt(server, &peer, first, 0u);
    assert_draft(server, &peer, generation, revision, "newer draft");

    const char *second = "22222222222222222222222222222222";
    admit_later(server, &peer, generation, revision, second, "newer draft");
    snag_view_channel_close(&peer);
    uint64_t deadline = snag_monotonic_ms() + 5000u;
    while (engine.generation && snag_monotonic_ms() < deadline) {
        snag_view_server_step(server);
        assert(snag_sleep_ms(1u) == 0);
    }
    assert(!engine.generation);
    generation = attach(server, &peer, dir, root);
    revision = edit(server, &peer, generation, revision, "reconnected edit");
    assert(snag_view_server_result(server, second, "committed", 3u, "steering_added") == 0);
    assert_receipt(server, &peer, second, 0u);
    assert_draft(server, &peer, generation, revision, "reconnected edit");

    const char *third = "33333333333333333333333333333333";
    admit_later(server, &peer, generation, revision, third, "reconnected edit");
    assert(snag_view_server_result(server, third, "committed", 4u, "input_received") == 0);
    assert_receipt(server, &peer, third, revision + 1u);
    assert_draft(server, &peer, generation, revision + 1u, "");
    assert(engine.submissions == 3u);

    const char *command = "44444444444444444444444444444444";
    revision = edit(server, &peer, generation, revision + 1u, "/fast");
    json_t *reply = exchange(server, &peer,
        json_pack("{s:s,s:I,s:s,s:I,s:s,s:s}", "type", "command",
            "generation", (json_int_t)generation, "route", "rollout",
            "draft_revision", (json_int_t)revision, "id", command, "text", "/fast"), "result");
    assert(!strcmp(snag_json_string(reply, "status"), "pending"));
    json_decref(reply);
    revision = edit(server, &peer, generation, revision, "typed while command works");
    json_t *result = json_pack("{s:s,s:s,s:i,s:s}", "id", command,
        "status", "completed", "seq", 5, "outcome", "ok");
    assert(result && snag_view_server_command_result(server, result) == 0);
    json_decref(result);
    reply = exchange(server, &peer, json_pack("{s:s,s:s}", "type", "receipt",
        "id", command), "result");
    assert(!strcmp(snag_json_string(reply, "status"), "completed"));
    assert(json_integer_value(json_object_get(reply, "draft_cleared")) == 0);
    json_decref(reply);
    assert_draft(server, &peer, generation, revision, "typed while command works");
    assert(engine.submissions == 4u);

    struct snag_irc_query_target query = {.generation = 7u, .identity = SNAG_IRC_OPERATOR};
    (void)snag_strcpy(query.connection, sizeof(query.connection), first);
    (void)snag_strcpy(query.conversation, sizeof(query.conversation), second);
    (void)snag_strcpy(query.peer, sizeof(query.peer), "first-peer");
    json_t *a = snag_view_query_route(&query);
    (void)snag_strcpy(query.conversation, sizeof(query.conversation), third);
    (void)snag_strcpy(query.peer, sizeof(query.peer), "second-peer");
    json_t *b = snag_view_query_route(&query);
    assert(a && b);
    uint64_t a_revision = edit_route(server, &peer, generation, 1u, "same text", a);
    uint64_t b_revision = edit_route(server, &peer, generation, 1u, "same text", b);
    const char *private_id = "55555555555555555555555555555555";
    reply = exchange(server, &peer, json_pack("{s:s,s:I,s:O,s:I,s:s,s:s}",
        "type", "submit", "generation", (json_int_t)generation, "route", a,
        "draft_revision", (json_int_t)a_revision, "id", private_id, "text", "same text"),
        "result");
    assert(!strcmp(snag_json_string(reply, "status"), "pending"));
    json_decref(reply);
    b_revision = edit_route(server, &peer, generation, b_revision, "new second draft", b);
    assert(snag_view_server_result(server, private_id, "committed", 6u, "irc_event_v2") == 0);
    assert_receipt(server, &peer, private_id, a_revision + 1u);
    assert_route(server, &peer, generation, a_revision + 1u, "", a);
    assert_route(server, &peer, generation, b_revision, "new second draft", b);
    assert_draft(server, &peer, generation, revision, "typed while command works");
    reply = exchange(server, &peer, json_pack("{s:s,s:I,s:O,s:I,s:s,s:s}",
        "type", "submit", "generation", (json_int_t)generation, "route", b,
        "draft_revision", (json_int_t)a_revision, "id", private_id, "text", "same text"),
        "error");
    assert(strstr(snag_json_string(reply, "message"), "already used"));
    json_decref(reply);
    assert(engine.submissions == 5u);
    a_revision = edit_route(server, &peer, generation, a_revision + 1u, "first retained", a);
    ++query.generation;
    json_t *renewed = snag_view_query_route(&query);
    assert_route(server, &peer, generation, 1u, "", renewed);
    assert_route(server, &peer, generation, b_revision, "new second draft", b);
    assert_route(server, &peer, generation, a_revision, "first retained", a);
    json_decref(renewed);
    json_decref(a);
    json_decref(b);

    snag_view_channel_close(&peer);
    snag_view_server_close(server);
    snag_session_relay_close(&relay);
    assert(close(pipe_fd[1]) == 0);
    assert(close(lock) == 0);
    assert(snag_unlink_at(dir, "lock", false) == 0);
    assert(close(dir) == 0 && rmdir(root) == 0);
    free(root);
}
#endif /* !_WIN32 */

int
main(void)
{
#ifndef _WIN32
    test_admission_edit_order();
#endif
    (void)puts("session draft ordering: ok");
    return 0;
}

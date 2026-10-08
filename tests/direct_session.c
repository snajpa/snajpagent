/* SPDX-License-Identifier: GPL-2.0-only */
#include "app.h"
#include "vm_connection.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
step(struct snag_vm_connection *connection, uint64_t deadline)
{
    assert(snag_monotonic_ms() < deadline);
    snag_vm_connection_step(connection);
    assert(snag_sleep_ms(1u) == 0);
}

static bool
active(const struct snag_vm_connection *connection)
{
    return json_is_true(json_object_get(connection->state, "active"));
}

static void
submit(struct snag_vm_connection *connection, const char *text, uint64_t deadline)
{
    struct snag_vm_buffer *buffer = connection->rollout;
    while (!buffer->draft_ready || connection->channel.output) step(connection, deadline);
    assert(snag_vm_draft_replace(buffer, 0u, buffer->draft.len, text, strlen(text)) == 0);
    assert(snag_vm_buffer_prepare(buffer, buffer, 1u, false) == 0);
    assert(snag_vm_buffer_send(buffer) == 0);
}

int
main(int argc, char **argv)
{
    assert(argc == 4 || argc == 5);
    const char *program = argv[1], *dotdir = argv[2], *mode = argv[3];
    struct snag_view_channel channel;
    snag_view_channel_init(&channel, -1);
    struct snag_app_direct *owner = snag_app_direct_start(program, dotdir,
        argc == 5 ? argv[4] : NULL, argc == 5 ? NULL : "direct-fixture", &channel);
    assert(owner);
    if (!strcmp(mode, "startup-stop")) snag_app_direct_stop(owner);
    uint64_t deadline = snag_monotonic_ms() + 15000u;
    char id[SNAG_ID_HEX_LEN + 1u], error[256];
    int status;
    enum snag_app_direct_state state;
    do {
        state = snag_app_direct_state(owner, id, error, sizeof(error), &status);
        assert(snag_monotonic_ms() < deadline);
        assert(snag_sleep_ms(1u) == 0);
    } while (state == SNAG_APP_DIRECT_STARTING);
    struct snag_vm_connection *connection = NULL;
    if (strcmp(mode, "startup-stop") && strcmp(mode, "startup-error")) {
        if (state != SNAG_APP_DIRECT_READY) (void)fprintf(stderr, "%s\n", error);
        assert(state == SNAG_APP_DIRECT_READY && snag_hex_is_lower(id, SNAG_ID_HEX_LEN));
        connection = snag_vm_connection_new(id);
        assert(connection && snag_vm_connection_direct(connection, &channel) == 0);
        while (!connection->bound || !connection->rollout->draft_ready)
            step(connection, deadline);
        assert(connection->direct && connection->commands && connection->drafts);
        assert(!connection->terminal_commands);
        snag_vm_connection_detach(connection);
        assert(connection->bound && !connection->detaching);
        assert(snag_vm_connection_control(connection, "detach") < 0 && errno == ENOTSUP);
        struct snag_view_channel other;
        snag_view_channel_init(&other, -1);
        assert(!snag_app_direct_start(program, dotdir, NULL, NULL, &other) && errno == EBUSY);
        assert(!snag_view_channel_opened(&other));
        if (!strcmp(mode, "close")) {
            snag_vm_connection_close(connection);
        } else {
            submit(connection, "/fast", deadline);
            while (connection->rollout->pending || !json_array_size(connection->reports))
                step(connection, deadline);
            while (!snag_json_string(connection->state, "service_tier") ||
                strcmp(snag_json_string(connection->state, "service_tier"), "priority"))
                step(connection, deadline);
            submit(connection, "direct-driver-prompt ž", deadline);
            const char *draft = "next unsent ž";
            assert(snag_vm_draft_replace(connection->rollout, 0u, 0u, draft, strlen(draft)) == 0);
            while (!active(connection)) step(connection, deadline);
            (void)puts("{\"phase\":\"active\"}");
            assert(fflush(stdout) == 0);
            if (!strcmp(mode, "stop")) {
                /* The HTTP fixture acknowledges entry before cancellation. */
                assert(getchar() == '\n');
                snag_app_direct_stop(owner);
            }
            else {
                while (active(connection) || connection->rollout->pending)
                    step(connection, deadline);
                assert(!strcmp((const char *)connection->rollout->draft.data, draft));
                while (connection->channel.output) step(connection, deadline);
                assert(snag_vm_connection_control(connection, "quit") == 0);
            }
        }
    }
    while ((state = snag_app_direct_state(owner, id, error, sizeof(error), &status)) !=
        SNAG_APP_DIRECT_FINISHED) {
        if (connection) step(connection, deadline);
        else {
            assert(snag_monotonic_ms() < deadline);
            assert(snag_sleep_ms(1u) == 0);
        }
    }
    while (connection && !connection->exited &&
        snag_view_channel_opened(&connection->channel)) step(connection, deadline);
    snag_view_channel_close(&channel);
    snag_app_direct_free(owner);
    if (!strcmp(mode, "startup-error")) assert(status != 0 && *error);
    else assert(status == 0);
    json_t *result = json_pack("{s:s,s:s,s:i,s:b,s:b}", "phase", "finished", "session", id,
        "status", status, "exit_received", connection && connection->exited,
        "report", connection && json_array_size(connection->reports) != 0u);
    char *text = json_dumps(result, JSON_COMPACT);
    assert(text);
    (void)puts(text);
    free(text);
    json_decref(result);
    snag_vm_connections_free(connection);
    return 0;
}

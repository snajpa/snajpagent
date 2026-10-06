/* SPDX-License-Identifier: GPL-2.0-only */
#include "app_internal.h"
#include "irc_address.h"
#include "json.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool
query_identity(const json_t *data, enum snag_irc_identity identity)
{
    const json_t *route = json_object_get(data, "routing");
    const char *kind = snag_json_string(route, "conversation_kind");
    const char *role = snag_json_string(route, "identity");
    return kind && role && !strcmp(kind, "query") &&
        !strcmp(role, identity == SNAG_IRC_AGENT ? "agent" : "operator");
}

int
snag_app_irc_query_target(struct app_state *app, const struct snag_irc_scopes *scopes,
                         const json_t *directory, enum snag_irc_identity identity,
                         uint32_t preferred, const char *selector,
                         const struct snag_irc_query_target *captured,
                         struct snag_irc_query_target *target, char *error, size_t error_size)
{
    const char *query_id = !strncmp(selector, "query:", 6u) ? selector + 6u : NULL;
    struct snag_irc_address address = {0};
    if (query_id) {
        if (!snag_hex_is_lower(query_id, SNAG_ID_HEX_LEN))
            return snag_fail(error, error_size, EINVAL, "invalid query conversation ID");
    } else if (snag_irc_address_parse(&address, selector, SNAG_IRC_MESSAGE_ADDRESS,
        error, error_size) < 0) return -1;
    if (address.session[0] && strcmp(address.session, app->session.id) &&
        (!app->session.name || strcmp(address.session, app->session.name)))
        return snag_fail(error, error_size, EACCES, "select the addressed session before sending");
    if (!query_id && strchr("#&+!", address.target[0]))
        return snag_fail(error, error_size, EINVAL, "select a nick for a private query");

    if (captured && captured->conversation[0] && captured->identity == identity) {
        *target = *captured;
        return 0;
    }
    const struct snag_irc_scope *selected = NULL;
    if (!query_id) {
        selected = snag_irc_scope_resolve(scopes, preferred, address.endpoint, error, error_size);
        if (!selected) return -1;
    }
    for (size_t i = 0u; i < scopes->count; ++i) {
        const struct snag_irc_scope *scope = &scopes->items[i];
        if (selected && selected != scope) continue;
        const json_t *connection = json_object_get(directory, scope->target.connection);
        const char *key;
        json_t *item;
        json_object_foreach(json_object_get(connection, "conversations"), key, item) {
            const json_t *data = json_object_get(item, "data");
            if (!query_identity(data, identity) || (query_id && strcmp(key, query_id))) continue;
            struct snag_irc_event event;
            if (snag_irc_event_record_read("irc_event_v2", data, &event) < 0) return -1;
            if (!query_id && (event.route.generation != scope->target.generation ||
                event.kind == SNAG_IRC_QUIT || !snag_irc_name_equal(scope->casemapping[identity],
                    address.target, event.route.peer))) continue;
            if (!snag_irc_event_query_target(app->irc, &event, target))
                return snag_fail(error, error_size, ESTALE, "query endpoint is unavailable");
            target->destination = scope->target.destination;
            return 0;
        }
    }
    if (!selected) return snag_fail(error, error_size, ENOENT,
        "query is unavailable for this identity; use /query or irc_state");
    struct snag_irc_query_target frozen = selected->target;
    frozen.identity = identity;
    return snag_irc_query_open_frozen(app->irc, &frozen, address.target, target, error, error_size);
}

int
snag_app_irc_channel_target(struct app_state *app, const struct snag_irc_scopes *scopes,
    const json_t *directory, enum snag_irc_identity identity, uint32_t preferred,
    const char *selector, struct snag_irc_channel_target *target, char *error, size_t error_size)
{
    const char *id = !strncmp(selector, "channel:", 8u) ? selector + 8u : NULL;
    struct snag_irc_address address = {0};
    const struct snag_irc_scope *selected = NULL;
    if (id) {
        if (!snag_hex_is_lower(id, SNAG_ID_HEX_LEN))
            return snag_fail(error, error_size, EINVAL, "invalid channel conversation ID");
    } else {
        if (snag_irc_address_parse(&address, selector, SNAG_IRC_MESSAGE_ADDRESS,
            error, error_size) < 0) return -1;
        if (address.session[0] && strcmp(address.session, app->session.id) &&
            (!app->session.name || strcmp(address.session, app->session.name)))
            return snag_fail(error, error_size, EACCES, "select the addressed session first");
        selected = snag_irc_scope_resolve(scopes, preferred, address.endpoint, error, error_size);
        if (!selected) return -1;
    }
    for (size_t i = 0u; i < scopes->count; ++i) {
        const struct snag_irc_scope *scope = &scopes->items[i];
        if (selected && selected != scope) continue;
        const json_t *connection = json_object_get(directory, scope->target.connection);
        const char *key;
        json_t *item;
        json_object_foreach(json_object_get(connection, "conversations"), key, item) {
            const json_t *data = json_object_get(item, "data");
            const json_t *route = json_object_get(data, "routing");
            const char *kind = snag_json_string(route, "conversation_kind");
            const char *role = snag_json_string(route, "identity");
            if (!kind || !role || strcmp(kind, "channel") ||
                strcmp(role, identity == SNAG_IRC_AGENT ? "agent" : "operator") ||
                (id && strcmp(key, id))) continue;
            struct snag_irc_event event;
            if (snag_irc_event_record_read("irc_event_v2", data, &event) < 0) return -1;
            if (!id && !snag_irc_name_equal(scope->casemapping[identity],
                address.target, event.room)) continue;
            if (!event.route.joined || !event.route.membership[0] ||
                event.route.generation != scope->target.generation)
                return snag_fail(error, error_size, ESTALE, "IRC channel is not joined");
            /* The caller's directory pins the membership. Never reopen it in
             * the live runtime: PART/rejoin may have replaced that membership. */
            *target = (struct snag_irc_channel_target){.identity = identity,
                .destination = scope->target.destination, .generation = event.route.generation};
            memcpy(target->connection, event.route.connection, sizeof(target->connection));
            memcpy(target->conversation, event.route.conversation, sizeof(target->conversation));
            memcpy(target->membership, event.route.membership, sizeof(target->membership));
            memcpy(target->room, event.room, sizeof(target->room));
            return 0;
        }
    }
    return snag_fail(error, error_size, ENOENT,
        "channel is unavailable for this identity; use irc_state for joined channels");
}

static int
query_list(struct app_state *app)
{
    struct snag_buf text = {.max = SNAG_MAX_IRC_SNAPSHOT};
    const char *connection;
    json_t *entry;
    int rc = 0;
    json_object_foreach(app->session.irc_conversations, connection, entry) {
        (void)connection;
        const char *id;
        json_t *item;
        json_object_foreach(json_object_get(entry, "conversations"), id, item) {
            const json_t *data = json_object_get(item, "data");
            if (!query_identity(data, SNAG_IRC_OPERATOR)) continue;
            const json_t *route = json_object_get(data, "routing");
            if (snag_buf_printf(&text, "/query query:%s  %s/%s\n", id,
                snag_json_string(entry, "endpoint"), snag_json_string(route, "peer")) < 0)
                goto fail;
        }
    }
    if (!text.len && snag_buf_printf(&text, "No operator queries opened. Use /query NICK.\n") < 0)
        goto fail;
    if (snag_buf_terminate(&text) < 0) goto fail;
    rc = snag_app_report(app, SNAG_UI_HOST, (const char *)text.data);
    snag_buf_free(&text);
    return rc;
fail:
    snag_buf_free(&text);
    return -1;
}

int
snag_app_irc_command(struct app_state *app, const char *line, bool *handled)
{
    size_t verb = strcspn(line, " \t\r\n");
    bool query = verb == 6u && !strncmp(line, "/query", verb);
    bool message = verb == 4u && !strncmp(line, "/msg", verb);
    bool notice = verb == 7u && !strncmp(line, "/notice", verb);
    bool action = verb == 3u && !strncmp(line, "/me", verb);
    *handled = query || message || notice || action;
    if (!*handled) return 0;
    const char *text = line + verb;
    while (isspace((unsigned char)*text)) ++text;
    if (query && !*text) return query_list(app);
    struct snag_irc_query_target target = {0};
    char error[256u] = {0};
    char *operand = NULL;
    struct snag_buf report = {.max = SNAG_MAX_IRC_SNAPSHOT};
    int rc = 0;
    if (action) {
        target = app->ui.input_query;
        if (app->ui.input_view != SNAG_RENDER_CHAT || !target.conversation[0] ||
            target.identity != SNAG_IRC_OPERATOR) {
            (void)snag_errorf(error, sizeof(error), "select an operator query before /me");
            goto rejected;
        }
    } else {
        if (snag_irc_address_operand(text, &operand, &text, error, sizeof(error)) < 0)
            goto rejected;
        if (!query && !*text) {
            (void)snag_errorf(error, sizeof(error), "message text is required");
            goto rejected;
        }
        uint32_t preferred = app->ui.input_view == SNAG_RENDER_CHAT ?
            app->ui.input_destination : 0u;
        if (snag_app_irc_query_target(app, &app->ui.input_scopes,
            app->session.irc_conversations, SNAG_IRC_OPERATOR, preferred,
            operand, &app->ui.input_address_query, &target, error, sizeof(error)) < 0)
            goto rejected;
    }
    if (!query && !*text) {
        (void)snag_errorf(error, sizeof(error), "message text is required");
        goto rejected;
    }
    if (*text && snag_irc_query_send(app->irc, &target,
        notice ? SNAG_IRC_NOTICE : SNAG_IRC_MESSAGE, text, action,
        &report, error, sizeof(error)) < 0) goto rejected;
    if (query) {
        /* Opening emits its durable metadata before returning. Existing
         * metadata is already retained in the UI's tab directory. */
        rc = snag_app_irc_select_query(app, &target);
    }
    goto done;
rejected:
    if (report.len && snag_buf_terminate(&report) < 0) {
        rc = -1;
        goto done;
    }
    if (report.len) rc = snag_app_report(app, SNAG_UI_WARNING, (const char *)report.data);
    if (!rc) rc = snag_app_report(app, SNAG_UI_ERROR,
        error[0] ? error : "IRC command failed");
    if (!rc && !report.len && !app->ui.input_interface) {
        rc = snag_ui_send(&app->ui, (struct snag_ui_command){
            .kind = SNAG_UI_DRAFT, .text = line});
    }
done:
    free(operand);
    snag_buf_free(&report);
    return rc;
}

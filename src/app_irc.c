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
        return snag_fail(error, error_size, EACCES,
                "select the addressed session before sending");
    if (captured && captured->conversation[0] && captured->identity == identity) {
        *target = *captured;
        return 0;
    }
    const struct snag_irc_scope *selected = NULL;
    if (!query_id) {
        selected = snag_irc_scope_resolve(scopes, preferred, address.endpoint, error, error_size);
        if (!selected) return -1;
        if (strchr(selected->chantypes[identity], address.target[0]))
            return snag_fail(error, error_size, EINVAL, "select a nick for a private query");
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
            return snag_fail(error, error_size, EACCES,
                "select the addressed session before sending");
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

int
snag_app_irc_conversation_send(struct app_state *app,
    const struct snag_irc_conversation_target *target, enum snag_irc_event_kind kind,
    const char *text, bool action, struct snag_buf *report, char *error, size_t error_size)
{
    if (target->identity != SNAG_IRC_OPERATOR)
        return snag_fail(error, error_size, EACCES, "agent conversations are read-only");
    if (target->kind == SNAG_IRC_QUERY) {
        struct snag_irc_query_target query = {.destination = target->destination,
            .generation = target->generation, .identity = target->identity};
        memcpy(query.connection, target->connection, sizeof(query.connection));
        memcpy(query.conversation, target->conversation, sizeof(query.conversation));
        memcpy(query.peer, target->peer, sizeof(query.peer));
        return snag_irc_query_send(app->irc, &query, kind, text, action,
            report, error, error_size);
    }
    if (target->kind != SNAG_IRC_CHANNEL) return snag_errno(EINVAL);
    struct snag_irc_channel_target channel = {.destination = target->destination,
        .generation = target->generation, .identity = target->identity};
    memcpy(channel.connection, target->connection, sizeof(channel.connection));
    memcpy(channel.conversation, target->conversation, sizeof(channel.conversation));
    memcpy(channel.membership, target->membership, sizeof(channel.membership));
    memcpy(channel.room, target->room, sizeof(channel.room));
    return snag_irc_channel_send(app->irc, &channel, kind, text, action,
        report, error, error_size);
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

static void
channel_presentation(struct snag_irc_conversation_target *out,
    const struct snag_irc_channel_target *channel, const struct snag_irc_scope *scope)
{
    *out = (struct snag_irc_conversation_target){.kind = SNAG_IRC_CHANNEL,
        .destination = channel->destination, .generation = channel->generation,
        .identity = channel->identity, .casemapping = scope->casemapping[channel->identity]};
    memcpy(out->connection, channel->connection, sizeof(out->connection));
    memcpy(out->conversation, channel->conversation, sizeof(out->conversation));
    memcpy(out->membership, channel->membership, sizeof(out->membership));
    memcpy(out->room, channel->room, sizeof(out->room));
    (void)snag_strcpy(out->endpoint, sizeof(out->endpoint), scope->endpoint);
}

enum operator_resolution {
    OP_QUERY, OP_MESSAGE, OP_CHANNEL_VIEW, OP_CHANNEL_JOIN, OP_CHANNEL_ACTION
};

static int
operator_conversation(struct app_state *app, const char *operand,
    enum operator_resolution resolution,
    struct snag_irc_conversation_target *target, char *error, size_t error_size)
{
    bool join = resolution == OP_CHANNEL_JOIN;
    const char *channel_id = !strncmp(operand, "channel:", 8u) ? operand + 8u : NULL;
    const char *query_id = !strncmp(operand, "query:", 6u) ? operand + 6u : NULL;
    struct snag_irc_address address = {0};
    uint32_t preferred = app->ui.input_view == SNAG_RENDER_CHAT ?
        app->ui.input_destination : 0u;
    const struct snag_irc_scope *scope = NULL;
    if (!channel_id && !query_id) {
        if (snag_irc_address_parse(&address, operand, SNAG_IRC_MESSAGE_ADDRESS,
            error, error_size) < 0) return -1;
        if (address.session[0] && strcmp(address.session, app->session.id) &&
            (!app->session.name || strcmp(address.session, app->session.name)))
            return snag_fail(error, error_size, EACCES,
                "select the addressed session before sending");
        scope = snag_irc_scope_resolve(&app->ui.input_scopes, preferred,
            address.endpoint, error, error_size);
        if (!scope) return -1;
    }
    bool channel = channel_id || (scope &&
        strchr(scope->chantypes[SNAG_IRC_OPERATOR], address.target[0]));
    if (resolution >= OP_CHANNEL_VIEW && !channel)
        return snag_fail(error, error_size, EINVAL, "select a channel for this command");
    if (resolution == OP_QUERY && channel)
        return snag_fail(error, error_size, EINVAL, "select a nick for a private query");
    const struct snag_irc_conversation_target *captured = &app->ui.input_address_conversation;
    if (channel && !join && resolution != OP_CHANNEL_VIEW &&
        captured->kind == SNAG_IRC_CHANNEL &&
        captured->identity == SNAG_IRC_OPERATOR) {
        *target = *captured;
        return 0;
    }
    if (!channel) {
        struct snag_irc_query_target frozen = {.destination = captured->destination,
            .generation = captured->generation, .identity = captured->identity};
        if (captured->kind == SNAG_IRC_QUERY) {
            memcpy(frozen.connection, captured->connection, sizeof(frozen.connection));
            memcpy(frozen.conversation, captured->conversation, sizeof(frozen.conversation));
            memcpy(frozen.peer, captured->peer, sizeof(frozen.peer));
        }
        struct snag_irc_query_target selected;
        if (snag_app_irc_query_target(app, &app->ui.input_scopes,
            app->session.irc_conversations, SNAG_IRC_OPERATOR, preferred,
            operand, &frozen, &selected, error, error_size) < 0) return -1;
        *target = (struct snag_irc_conversation_target){.kind = SNAG_IRC_QUERY,
            .destination = selected.destination, .generation = selected.generation,
            .identity = selected.identity};
        memcpy(target->connection, selected.connection, sizeof(target->connection));
        memcpy(target->conversation, selected.conversation, sizeof(target->conversation));
        memcpy(target->peer, selected.peer, sizeof(target->peer));
        return 0;
    }
    if (resolution == OP_MESSAGE || resolution == OP_CHANNEL_ACTION)
        return snag_fail(error, error_size, ESTALE,
            "operator channel was unavailable when this command was entered; use /chat or /join");
    if (channel_id && !snag_hex_is_lower(channel_id, SNAG_ID_HEX_LEN))
        return snag_fail(error, error_size, EINVAL, "invalid channel conversation ID");
    for (size_t i = 0u; i < app->ui.input_scopes.count; ++i) {
        const struct snag_irc_scope *candidate = &app->ui.input_scopes.items[i];
        if (scope && scope != candidate) continue;
        const json_t *connection = json_object_get(app->session.irc_conversations,
            candidate->target.connection);
        const char *id;
        json_t *item;
        json_object_foreach(json_object_get(connection, "conversations"), id, item) {
            if (channel_id && strcmp(id, channel_id)) continue;
            struct snag_irc_event event;
            if (snag_irc_event_record_read("irc_event_v2", json_object_get(item, "data"),
                &event) < 0) return -1;
            if (event.route.kind != SNAG_IRC_CHANNEL ||
                event.route.identity != SNAG_IRC_OPERATOR || !event.route.membership[0] ||
                (!channel_id && !snag_irc_name_equal(candidate->casemapping[SNAG_IRC_OPERATOR],
                    event.room, address.target))) continue;
            struct snag_irc_channel_target selected = {.identity = SNAG_IRC_OPERATOR,
                .destination = candidate->target.destination, .generation = event.route.generation};
            memcpy(selected.connection, event.route.connection, sizeof(selected.connection));
            memcpy(selected.conversation, event.route.conversation, sizeof(selected.conversation));
            memcpy(selected.membership, event.route.membership, sizeof(selected.membership));
            memcpy(selected.room, event.room, sizeof(selected.room));
            channel_presentation(target, &selected, candidate);
            if (!join) return 0;
            scope = candidate;
            memcpy(address.target, selected.room, strlen(selected.room) + 1u);
            break;
        }
        if (channel_id && scope) break;
    }
    if (!join || !scope)
        return snag_fail(error, error_size, ENOENT, "operator channel is unavailable; use /join");
    struct snag_irc_query_target frozen = scope->target;
    frozen.identity = SNAG_IRC_OPERATOR;
    struct snag_irc_channel_target selected;
    if (snag_irc_channel_open(app->irc, &frozen, address.target, true,
        &selected, error, error_size) < 0) return -1;
    channel_presentation(target, &selected, scope);
    return 0;
}

int
snag_app_irc_command(struct app_state *app, const char *line, bool *handled)
{
    size_t verb = strcspn(line, " \t\r\n");
    bool query = verb == 6u && !strncmp(line, "/query", verb);
    bool message = verb == 4u && !strncmp(line, "/msg", verb);
    bool notice = verb == 7u && !strncmp(line, "/notice", verb);
    bool action = verb == 3u && !strncmp(line, "/me", verb);
    bool chat = verb == 5u && !strncmp(line, "/chat", verb);
    bool join = verb == 5u && !strncmp(line, "/join", verb);
    bool part = verb == 5u && !strncmp(line, "/part", verb);
    bool names = verb == 6u && !strncmp(line, "/names", verb);
    bool topic = verb == 6u && !strncmp(line, "/topic", verb);
    const char *text = line + verb;
    while (isspace((unsigned char)*text)) ++text;
    if ((chat && !*text && !app->ui.input_interface &&
        !app->ui.input_conversation.conversation[0]) ||
        ((names || topic) && !app->ui.input_conversation.conversation[0])) {
        *handled = false;
        return 0;
    }
    *handled = query || message || notice || action || chat || join || part || names || topic;
    if (!*handled) return 0;
    if (query && !*text) return query_list(app);
    struct snag_irc_conversation_target target = {0};
    char error[256u] = {0};
    char *operand = NULL;
    struct snag_buf report = {.max = SNAG_MAX_IRC_SNAPSHOT};
    int rc = 0;
    if (action || names || topic || (part && !*text)) {
        target = app->ui.input_conversation;
        if (app->ui.input_view != SNAG_RENDER_CHAT || !target.conversation[0] ||
            target.identity != SNAG_IRC_OPERATOR) {
            (void)snag_errorf(error, sizeof(error), "select an operator conversation first");
            goto rejected;
        }
    } else {
        if (chat && !*text) {
            const struct snag_irc_scope *scope = snag_irc_scope_resolve(&app->ui.input_scopes,
                app->ui.input_view == SNAG_RENDER_CHAT ? app->ui.input_destination : 0u,
                "", error, sizeof(error));
            if (!scope) goto rejected;
            struct snag_irc_address address = {.kind = SNAG_IRC_CONVERSATION};
            (void)snag_strcpy(address.endpoint, sizeof(address.endpoint), scope->endpoint);
            (void)snag_strcpy(address.target, sizeof(address.target),
                app->ui.input_conversation.kind == SNAG_IRC_CHANNEL ?
                    app->ui.input_conversation.room : scope->room);
            operand = snag_irc_address_format(&address);
            if (!operand) goto rejected;
        } else if (snag_irc_address_operand(text, &operand, &text, error, sizeof(error)) < 0)
            goto rejected;
        if ((message || notice) && !*text) {
            (void)snag_errorf(error, sizeof(error), "message text is required");
            goto rejected;
        }
        if ((chat || join) && *text) {
            (void)snag_errorf(error, sizeof(error), "this command accepts one channel address");
            goto rejected;
        }
        if (operator_conversation(app, operand, query ? OP_QUERY : chat ? OP_CHANNEL_VIEW :
            join ? OP_CHANNEL_JOIN : part ? OP_CHANNEL_ACTION : OP_MESSAGE,
            &target, error, sizeof(error)) < 0)
            goto rejected;
    }
    if (query && target.kind != SNAG_IRC_QUERY) {
        (void)snag_errorf(error, sizeof(error), "select a nick for a private query");
        goto rejected;
    }
    if ((message || notice || action) && !*text) {
        (void)snag_errorf(error, sizeof(error), "message text is required");
        goto rejected;
    }
    if (part || names || topic) {
        if (names && *text) {
            (void)snag_errorf(error, sizeof(error), "/names takes no arguments");
            goto rejected;
        }
        if (target.kind != SNAG_IRC_CHANNEL) {
            (void)snag_errorf(error, sizeof(error), "this command requires a channel");
            goto rejected;
        }
        struct snag_irc_channel_target channel = {.identity = target.identity,
            .destination = target.destination, .generation = target.generation};
        memcpy(channel.connection, target.connection, sizeof(channel.connection));
        memcpy(channel.conversation, target.conversation, sizeof(channel.conversation));
        memcpy(channel.membership, target.membership, sizeof(channel.membership));
        memcpy(channel.room, target.room, sizeof(channel.room));
        bool inspect = names || (topic && !*text);
        if (snag_irc_channel_action(app->irc, &channel, part ? SNAG_IRC_CHANNEL_PART :
            names ? SNAG_IRC_CHANNEL_NAMES : SNAG_IRC_CHANNEL_TOPIC,
            *text ? text : NULL, inspect ? &report : NULL, error, sizeof(error)) < 0) goto rejected;
        if (inspect) {
            if (snag_buf_printf(&report, "Cached state; %s refresh requested.\n",
                names ? "NAMES" : "TOPIC") < 0 ||
                snag_buf_terminate(&report) < 0) {
                rc = -1;
                goto done;
            }
            rc = snag_app_report(app, SNAG_UI_HOST, (const char *)report.data);
        }
        goto done;
    }
    if (*text && snag_app_irc_conversation_send(app, &target,
        notice ? SNAG_IRC_NOTICE : SNAG_IRC_MESSAGE, text, action,
        &report, error, sizeof(error)) < 0) goto rejected;
    if (query || chat || join) {
        /* Opening emits its durable metadata before returning. Existing
         * metadata is already retained in the UI's tab directory. */
        rc = snag_app_irc_select_conversation(app, &target);
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

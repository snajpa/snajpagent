/* SPDX-License-Identifier: GPL-2.0-only */
#include "irc.h"
#include "json.h"

#include <errno.h>
#include <string.h>

const char *
snag_irc_kind_name(enum snag_irc_event_kind kind)
{
    static const char *const names[] = {
        "connected", "disconnected", "join", "part", "quit", "nick",
        "message", "notice", "topic", "mode", "history_ready"
    };

    return (unsigned int)kind < sizeof(names) / sizeof(names[0]) ?
           names[kind] : "unknown";
}

json_t *
snag_irc_event_data(const struct snag_irc_event *event)
{
    return json_pack("{s:s,s:b,s:s,s:b,s:s,s:b,s:s,s:s,s:I,s:s,s:I,s:b}",
        "endpoint", event->endpoint, "historical", event->historical,
        "kind", snag_irc_kind_name(event->kind), "local", event->local,
        "nick", event->nick, "op", event->op, "room", event->room,
        "text", event->text, "timestamp_ms", (json_int_t)event->timestamp_ms,
        "stream", event->stream, "sequence", (json_int_t)event->sequence, "input", event->input);
}

static bool
event_field(const json_t *data, const char *key, char *out, size_t size)
{
    const char *value = snag_json_string(data, key);

    if (!value || strlen(value) >= size ||
        !snag_utf8_valid((const unsigned char *)value, strlen(value), true))
        return false;
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p)
        if (*p < 0x20u || *p == 0x7fu)
            return false;
    return snag_strcpy(out, size, value);
}

int
snag_irc_event_read(const json_t *data, struct snag_irc_event *event)
{
    static const char *const keys[] = {
        "endpoint", "historical", "kind", "local", "nick", "op",
        "room", "text", "timestamp_ms", "stream", "sequence", "input"
    };
    const char *kind = snag_json_string(data, "kind");

    memset(event, 0, sizeof(*event));
    if (!snag_json_exact_keys(data, keys, sizeof(keys) / sizeof(keys[0])) || !kind ||
        !event_field(data, "endpoint", event->endpoint, sizeof(event->endpoint)) ||
        !event->endpoint[0] ||
        !event_field(data, "room", event->room, sizeof(event->room)) ||
        !event_field(data, "nick", event->nick, sizeof(event->nick)) ||
        !event_field(data, "text", event->text, sizeof(event->text)) ||
        !json_is_boolean(json_object_get(data, "historical")) ||
        !json_is_boolean(json_object_get(data, "local")) ||
        !json_is_boolean(json_object_get(data, "op")) ||
        snag_json_integer_u64(data, "timestamp_ms", &event->timestamp_ms) < 0 ||
        !event->timestamp_ms ||
        !event_field(data, "stream", event->stream, sizeof(event->stream)) ||
        (event->stream[0] && !snag_hex_is_lower(event->stream, SNAG_ID_HEX_LEN)) ||
        snag_json_integer_u64(data, "sequence", &event->sequence) < 0 ||
        (!!event->sequence != !!event->stream[0]) ||
        !json_is_boolean(json_object_get(data, "input")))
        goto invalid;
    for (unsigned int i = 0u; i <= (unsigned int)SNAG_IRC_HISTORY_READY; ++i) {
        if (strcmp(kind, snag_irc_kind_name((enum snag_irc_event_kind)i)) != 0)
            continue;
        event->kind = (enum snag_irc_event_kind)i;
        event->historical = json_is_true(json_object_get(data, "historical"));
        event->input = json_is_true(json_object_get(data, "input"));
        event->local = json_is_true(json_object_get(data, "local"));
        event->op = json_is_true(json_object_get(data, "op"));
        return 0;
    }
invalid:
    errno = EINVAL;
    return -1;
}

int
snag_irc_event_projection(struct snag_buf *out, const struct snag_irc_event *event)
{
    return snag_buf_printf(out,
        "[IRC endpoint=%s room=%s event=%s sender=%s operator=%s historical=%s id=%s:%llu]\n%s\n",
        event->endpoint, event->room, snag_irc_kind_name(event->kind),
        event->nick[0] ? event->nick : "server", event->op ? "true" : "false",
        event->historical ? "true" : "false", event->stream,
        (unsigned long long)event->sequence, event->text);
}

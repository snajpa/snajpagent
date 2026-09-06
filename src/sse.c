/* SPDX-License-Identifier: GPL-2.0-only */
#include "sse.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

void
snag_sse_init(struct snag_sse_parser *parser, snag_sse_record_fn record,
             void *opaque)
{
    memset(parser, 0, sizeof(*parser));
    snag_buf_init(&parser->line, SNAG_MAX_SSE_EVENT);
    snag_buf_init(&parser->event, SNAG_MAX_SSE_NAME);
    snag_buf_init(&parser->id, SNAG_MAX_SSE_NAME);
    snag_buf_init(&parser->data, SNAG_MAX_SSE_EVENT);
    parser->record = record;
    parser->opaque = opaque;
}

void
snag_sse_free(struct snag_sse_parser *parser)
{
    snag_buf_free(&parser->line);
    snag_buf_free(&parser->event);
    snag_buf_free(&parser->id);
    snag_buf_free(&parser->data);
    memset(parser, 0, sizeof(*parser));
}

static int
fail(struct snag_sse_parser *parser, char *error, size_t error_size,
     const char *message)
{
    parser->failed = true;
    snag_errorf(error, error_size, "%s", message);
    errno = EPROTO;
    return -1;
}

static int
assign(struct snag_buf *target, const unsigned char *value, size_t len)
{
    snag_buf_reset(target);
    return snag_buf_append(target, value, len);
}

static int
deliver(struct snag_sse_parser *parser, const struct snag_sse_record *record,
        char *error, size_t error_size)
{
    if (!parser->record || parser->record(parser->opaque, record) == 0)
        return 0;
    parser->failed = true;
    if (error_size && !error[0])
        snag_errorf(error, error_size, "SSE consumer rejected %s",
                    record->kind == SNAG_SSE_EVENT ? "an event" : "a comment");
    if (!errno)
        errno = EPROTO;
    return -1;
}

static int
dispatch(struct snag_sse_parser *parser, char *error, size_t error_size)
{
    struct snag_sse_record record;

    if (!parser->data_seen) {
        snag_buf_reset(&parser->event);
        return 0;
    }
    if (!snag_utf8_valid(parser->event.data, parser->event.len, true) ||
        !snag_utf8_valid(parser->id.data, parser->id.len, true) ||
        !snag_utf8_valid(parser->data.data, parser->data.len, true))
        return fail(parser, error, error_size,
                    "SSE event contains invalid UTF-8 or NUL");
    memset(&record, 0, sizeof(record));
    record.kind = SNAG_SSE_EVENT;
    record.event = parser->event.data;
    record.event_len = parser->event.len;
    record.id = parser->id.data;
    record.id_len = parser->id.len;
    record.data = parser->data.data;
    record.data_len = parser->data.len;
    if (deliver(parser, &record, error, error_size) < 0)
        return -1;
    snag_buf_reset(&parser->event);
    snag_buf_reset(&parser->data);
    parser->data_seen = false;
    return 0;
}

static int
process_line(struct snag_sse_parser *parser, char *error, size_t error_size)
{
    const unsigned char *line = parser->line.data;
    size_t len = parser->line.len;
    size_t colon = 0u;
    const unsigned char *value;
    size_t value_len;

    if (len == 0u)
        return dispatch(parser, error, error_size);
    if (!snag_utf8_valid(line, len, true))
        return fail(parser, error, error_size,
                    "SSE line contains invalid UTF-8 or NUL");
    if (line[0] == ':') {
        struct snag_sse_record record = { .kind = SNAG_SSE_COMMENT };

        value = line + 1u;
        value_len = len - 1u;
        if (value_len && value[0] == ' ') {
            ++value;
            --value_len;
        }
        record.data = value;
        record.data_len = value_len;
        return deliver(parser, &record, error, error_size);
    }
    while (colon < len && line[colon] != ':')
        ++colon;
    value = colon < len ? line + colon + 1u : line + len;
    value_len = colon < len ? len - colon - 1u : 0u;
    if (value_len && value[0] == ' ') {
        ++value;
        --value_len;
    }
    if (colon == 4u && memcmp(line, "data", 4u) == 0) {
        size_t extra = value_len + (parser->data_seen ? 1u : 0u);
        if (extra > SNAG_MAX_SSE_EVENT - parser->data.len)
            return fail(parser, error, error_size, "SSE event exceeds 1 MiB");
        if ((parser->data_seen && snag_buf_putc(&parser->data, '\n') < 0) ||
            snag_buf_append(&parser->data, value, value_len) < 0)
            return fail(parser, error, error_size, "SSE event exceeds 1 MiB");
        parser->data_seen = true;
    } else if (colon == 5u && memcmp(line, "event", 5u) == 0) {
        if (assign(&parser->event, value, value_len) < 0)
            return fail(parser, error, error_size, "SSE event name is too large");
    } else if (colon == 2u && memcmp(line, "id", 2u) == 0) {
        if (memchr(value, '\0', value_len))
            return fail(parser, error, error_size, "SSE id contains NUL");
        if (assign(&parser->id, value, value_len) < 0)
            return fail(parser, error, error_size, "SSE id is too large");
    } else if (colon == 5u && memcmp(line, "retry", 5u) == 0) {
        for (size_t i = 0; i < value_len; ++i)
            if (value[i] < '0' || value[i] > '9')
                return fail(parser, error, error_size,
                            "SSE retry field is not unsigned decimal");
    }
    return 0;
}

static int
end_line(struct snag_sse_parser *parser, char *error, size_t error_size)
{
    int rc = process_line(parser, error, error_size);
    snag_buf_reset(&parser->line);
    return rc;
}

int
snag_sse_feed(struct snag_sse_parser *parser, const void *data, size_t len,
             char *error, size_t error_size)
{
    const unsigned char *input = data;

    if (parser->failed) {
        errno = EPROTO;
        snag_errorf(error, error_size, "SSE parser is already failed");
        return -1;
    }
    if (len > SNAG_MAX_PROVIDER_WIRE - parser->wire_bytes)
        return fail(parser, error, error_size,
                    "provider wire aggregate exceeds 64 MiB");
    parser->wire_bytes += len;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = input[i];

        if (c == '\0')
            return fail(parser, error, error_size, "SSE stream contains NUL");
        if (parser->pending_cr) {
            parser->pending_cr = false;
            if (c != '\n')
                return fail(parser, error, error_size,
                            "SSE stream contains bare carriage return");
            if (end_line(parser, error, error_size) < 0)
                return -1;
            continue;
        }
        if (c == '\r') {
            parser->pending_cr = true;
        } else if (c == '\n') {
            if (end_line(parser, error, error_size) < 0)
                return -1;
        } else if (snag_buf_putc(&parser->line, c) < 0) {
            return fail(parser, error, error_size, "SSE line exceeds 1 MiB");
        }
    }
    return 0;
}

int
snag_sse_finish(struct snag_sse_parser *parser, char *error, size_t error_size)
{
    if (parser->failed) {
        errno = EPROTO;
        snag_errorf(error, error_size, "SSE parser is failed");
        return -1;
    }
    if (parser->pending_cr || parser->line.len || parser->data_seen ||
        parser->event.len)
        return fail(parser, error, error_size, "SSE stream ended mid-event");
    return 0;
}

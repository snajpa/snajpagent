/* SPDX-License-Identifier: GPL-2.0-only */
#include "sse.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
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

void
snj_sse_init(struct snj_sse_parser *parser, snj_sse_record_fn record,
             void *opaque)
{
    memset(parser, 0, sizeof(*parser));
    snj_buf_init(&parser->line, SNJ_MAX_SSE_EVENT);
    snj_buf_init(&parser->event, SNJ_MAX_SSE_NAME);
    snj_buf_init(&parser->id, SNJ_MAX_SSE_NAME);
    snj_buf_init(&parser->data, SNJ_MAX_SSE_EVENT);
    parser->record = record;
    parser->opaque = opaque;
}

void
snj_sse_free(struct snj_sse_parser *parser)
{
    snj_buf_free(&parser->line);
    snj_buf_free(&parser->event);
    snj_buf_free(&parser->id);
    snj_buf_free(&parser->data);
    memset(parser, 0, sizeof(*parser));
}

static int
fail(struct snj_sse_parser *parser, char *error, size_t error_size,
     const char *message)
{
    parser->failed = true;
    set_error(error, error_size, "%s", message);
    errno = EPROTO;
    return -1;
}

static int
assign(struct snj_buf *target, const unsigned char *value, size_t len)
{
    snj_buf_reset(target);
    return snj_buf_append(target, value, len);
}

static int
dispatch(struct snj_sse_parser *parser, char *error, size_t error_size)
{
    struct snj_sse_record record;
    int rc;

    if (!parser->data_seen) {
        snj_buf_reset(&parser->event);
        return 0;
    }
    if (!snj_utf8_valid(parser->event.data, parser->event.len, true) ||
        !snj_utf8_valid(parser->id.data, parser->id.len, true) ||
        !snj_utf8_valid(parser->data.data, parser->data.len, true))
        return fail(parser, error, error_size,
                    "SSE event contains invalid UTF-8 or NUL");
    memset(&record, 0, sizeof(record));
    record.kind = SNJ_SSE_EVENT;
    record.event = parser->event.data;
    record.event_len = parser->event.len;
    record.id = parser->id.data;
    record.id_len = parser->id.len;
    record.data = parser->data.data;
    record.data_len = parser->data.len;
    rc = parser->record ? parser->record(parser->opaque, &record) : 0;
    if (rc != 0) {
        parser->failed = true;
        if (error_size && !error[0])
            set_error(error, error_size, "SSE consumer rejected an event");
        if (!errno)
            errno = EPROTO;
        return -1;
    }
    snj_buf_reset(&parser->event);
    snj_buf_reset(&parser->data);
    parser->data_seen = false;
    return 0;
}

static int
dispatch_comment(struct snj_sse_parser *parser,
                 const unsigned char *value, size_t len,
                 char *error, size_t error_size)
{
    struct snj_sse_record record;
    int rc;

    if (!snj_utf8_valid(value, len, true))
        return fail(parser, error, error_size,
                    "SSE comment contains invalid UTF-8 or NUL");
    if (!parser->record)
        return 0;
    memset(&record, 0, sizeof(record));
    record.kind = SNJ_SSE_COMMENT;
    record.data = value;
    record.data_len = len;
    rc = parser->record(parser->opaque, &record);
    if (rc != 0) {
        parser->failed = true;
        if (error_size && !error[0])
            set_error(error, error_size, "SSE consumer rejected a comment");
        if (!errno)
            errno = EPROTO;
        return -1;
    }
    return 0;
}

static int
process_line(struct snj_sse_parser *parser, char *error, size_t error_size)
{
    const unsigned char *line = parser->line.data;
    size_t len = parser->line.len;
    size_t colon = 0u;
    const unsigned char *value;
    size_t value_len;

    if (len == 0u)
        return dispatch(parser, error, error_size);
    if (!snj_utf8_valid(line, len, true))
        return fail(parser, error, error_size,
                    "SSE line contains invalid UTF-8 or NUL");
    if (line[0] == ':') {
        value = line + 1u;
        value_len = len - 1u;
        if (value_len && value[0] == ' ') {
            ++value;
            --value_len;
        }
        return dispatch_comment(parser, value, value_len, error, error_size);
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
        if (extra > SNJ_MAX_SSE_EVENT - parser->data.len)
            return fail(parser, error, error_size, "SSE event exceeds 1 MiB");
        if ((parser->data_seen && snj_buf_putc(&parser->data, '\n') < 0) ||
            snj_buf_append(&parser->data, value, value_len) < 0)
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
end_line(struct snj_sse_parser *parser, char *error, size_t error_size)
{
    int rc = process_line(parser, error, error_size);
    snj_buf_reset(&parser->line);
    return rc;
}

int
snj_sse_feed(struct snj_sse_parser *parser, const void *data, size_t len,
             char *error, size_t error_size)
{
    const unsigned char *input = data;

    if (parser->failed) {
        errno = EPROTO;
        set_error(error, error_size, "SSE parser is already failed");
        return -1;
    }
    if (len > SNJ_MAX_PROVIDER_WIRE - parser->wire_bytes)
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
        } else if (snj_buf_putc(&parser->line, c) < 0) {
            return fail(parser, error, error_size, "SSE line exceeds 1 MiB");
        }
    }
    return 0;
}

int
snj_sse_finish(struct snj_sse_parser *parser, char *error, size_t error_size)
{
    if (parser->failed) {
        errno = EPROTO;
        set_error(error, error_size, "SSE parser is failed");
        return -1;
    }
    if (parser->pending_cr || parser->line.len || parser->data_seen ||
        parser->event.len)
        return fail(parser, error, error_size, "SSE stream ended mid-event");
    return 0;
}

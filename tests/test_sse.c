/* SPDX-License-Identifier: GPL-2.0-only */
#include "sse.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct capture {
    unsigned int events;
    unsigned int comments;
    char event[64];
    char id[64];
    char data[256];
};

static int
capture_record(void *opaque, const struct snag_sse_record *record)
{
    struct capture *capture = opaque;

    if (record->kind == SNAG_SSE_COMMENT) {
        ++capture->comments;
        assert(record->data_len < sizeof(capture->data));
        if (record->data_len)
            memcpy(capture->data, record->data, record->data_len);
        capture->data[record->data_len] = '\0';
        return 0;
    }
    ++capture->events;
    assert(record->event_len < sizeof(capture->event));
    assert(record->id_len < sizeof(capture->id));
    assert(record->data_len < sizeof(capture->data));
    if (record->event_len)
        memcpy(capture->event, record->event, record->event_len);
    capture->event[record->event_len] = '\0';
    if (record->id_len)
        memcpy(capture->id, record->id, record->id_len);
    capture->id[record->id_len] = '\0';
    if (record->data_len)
        memcpy(capture->data, record->data, record->data_len);
    capture->data[record->data_len] = '\0';
    return 0;
}

static int
count_record(void *opaque, const struct snag_sse_record *record)
{
    unsigned int *count = opaque;

    if (record->kind == SNAG_SSE_EVENT)
        ++*count;
    return 0;
}

static int
reject_record(void *opaque, const struct snag_sse_record *record)
{
    (void)opaque;
    (void)record;
    errno = ECANCELED;
    return -1;
}

static void
test_chunked_crlf_and_fields(void)
{
    static const char first[] =
        "id: one\r\nevent: response.output_text.delta\r\ndata: {\"delta\":\"ha\"}\r";
    static const char second[] =
        "\ndata: {\"delta\":\"ha\"}\r\nretry: 1000\r\n\r\n";
    struct snag_sse_parser parser;
    struct capture capture = {0};
    char error[128] = {0};

    snag_sse_init(&parser, capture_record, &capture);
    assert(snag_sse_feed(&parser, first, sizeof(first) - 1u,
                        error, sizeof(error)) == 0);
    assert(snag_sse_feed(&parser, second, sizeof(second) - 1u,
                        error, sizeof(error)) == 0);
    assert(snag_sse_finish(&parser, error, sizeof(error)) == 0);
    assert(capture.events == 1u);
    assert(strcmp(capture.id, "one") == 0);
    assert(strcmp(capture.event, "response.output_text.delta") == 0);
    assert(strcmp(capture.data,
                  "{\"delta\":\"ha\"}\n{\"delta\":\"ha\"}") == 0);
    snag_sse_free(&parser);
}

static void
test_comments_ids_and_empty_data(void)
{
    static const char stream[] =
        ": keepalive\n\n"
        "id: stable\n\n"
        "event: empty\ndata:\n\n"
        "data:\ndata:\n\n"
        "data: final\n\n";
    struct snag_sse_parser parser;
    struct capture capture = {0};
    char error[128] = {0};

    snag_sse_init(&parser, capture_record, &capture);
    assert(snag_sse_feed(&parser, stream, sizeof(stream) - 1u,
                        error, sizeof(error)) == 0);
    assert(snag_sse_finish(&parser, error, sizeof(error)) == 0);
    assert(capture.comments == 1u);
    assert(capture.events == 3u);
    assert(strcmp(capture.id, "stable") == 0);
    assert(strcmp(capture.event, "") == 0);
    assert(strcmp(capture.data, "final") == 0);
    snag_sse_free(&parser);
}

static void
test_split_utf8_and_identical_events(void)
{
    static const unsigned char stream[] =
        "data: {\"delta\":\"\xe2\x82\xac\"}\n\n"
        "data: {\"delta\":\"ha\"}\n\n"
        "data: {\"delta\":\"ha\"}\n\n";
    struct snag_sse_parser parser;
    struct capture capture = {0};
    char error[128] = {0};
    size_t split = 18u;

    snag_sse_init(&parser, capture_record, &capture);
    assert(snag_sse_feed(&parser, stream, split, error, sizeof(error)) == 0);
    assert(snag_sse_feed(&parser, stream + split,
                        sizeof(stream) - 1u - split,
                        error, sizeof(error)) == 0);
    assert(snag_sse_finish(&parser, error, sizeof(error)) == 0);
    assert(capture.events == 3u);
    assert(strcmp(capture.data, "{\"delta\":\"ha\"}") == 0);
    snag_sse_free(&parser);
}

static void
test_bounds(void)
{
    struct snag_sse_parser parser;
    char error[128] = {0};
    unsigned int count = 0u;
    unsigned char *input = malloc(SNAG_MAX_SSE_EVENT + 2u);

    assert(input);
    memcpy(input, "data: ", 6u);
    memset(input + 6u, 'x', SNAG_MAX_SSE_EVENT - 6u);
    input[SNAG_MAX_SSE_EVENT] = '\n';
    input[SNAG_MAX_SSE_EVENT + 1u] = '\n';
    snag_sse_init(&parser, count_record, &count);
    assert(snag_sse_feed(&parser, input, SNAG_MAX_SSE_EVENT + 2u,
                        error, sizeof(error)) == 0);
    assert(snag_sse_finish(&parser, error, sizeof(error)) == 0);
    assert(count == 1u);
    snag_sse_free(&parser);

    memset(error, 0, sizeof(error));
    snag_sse_init(&parser, NULL, NULL);
    memset(input, 'x', SNAG_MAX_SSE_EVENT + 1u);
    assert(snag_sse_feed(&parser, input, SNAG_MAX_SSE_EVENT + 1u,
                        error, sizeof(error)) < 0);
    assert(strstr(error, "line exceeds"));
    snag_sse_free(&parser);

    memset(error, 0, sizeof(error));
    snag_sse_init(&parser, NULL, NULL);
    memcpy(input, "data: ", 6u);
    memset(input + 6u, 'x', SNAG_MAX_SSE_EVENT - 6u);
    input[SNAG_MAX_SSE_EVENT] = '\n';
    assert(snag_sse_feed(&parser, input, SNAG_MAX_SSE_EVENT + 1u,
                        error, sizeof(error)) == 0);
    assert(snag_sse_feed(&parser, "data: 123456\n", 13u,
                        error, sizeof(error)) < 0);
    assert(strstr(error, "event exceeds"));
    snag_sse_free(&parser);

    memset(error, 0, sizeof(error));
    snag_sse_init(&parser, NULL, NULL);
    parser.wire_bytes = SNAG_MAX_PROVIDER_WIRE;
    assert(snag_sse_feed(&parser, "x", 1u, error, sizeof(error)) < 0);
    assert(strstr(error, "aggregate exceeds"));
    snag_sse_free(&parser);
    free(input);
}

static void
test_failures(void)
{
    static const unsigned char bare_cr[] = "data: x\rdata: y\n\n";
    static const unsigned char nul[] = "data: x\0y\n\n";
    static const unsigned char invalid_utf8[] = {'x', ':', ' ', 0xc0, '\n', '\n'};
    static const unsigned char retry[] = "retry: 1x\n\n";
    static const unsigned char truncated[] = "data: unfinished";
    static const unsigned char event_only[] = "event: unfinished\n";
    static const unsigned char cr_at_eof[] = "data: x\r";
    struct bad_case {
        const unsigned char *data;
        size_t len;
    } cases[] = {
        {bare_cr, sizeof(bare_cr) - 1u},
        {nul, sizeof(nul) - 1u},
        {invalid_utf8, sizeof(invalid_utf8)},
        {retry, sizeof(retry) - 1u},
        {truncated, sizeof(truncated) - 1u},
        {event_only, sizeof(event_only) - 1u},
        {cr_at_eof, sizeof(cr_at_eof) - 1u}
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        struct snag_sse_parser parser;
        struct capture capture = {0};
        char error[128] = {0};
        int rc;

        snag_sse_init(&parser, capture_record, &capture);
        rc = snag_sse_feed(&parser, cases[i].data, cases[i].len,
                          error, sizeof(error));
        if (rc == 0)
            rc = snag_sse_finish(&parser, error, sizeof(error));
        assert(rc < 0);
        assert(error[0]);
        assert(snag_sse_feed(&parser, "", 0u, error, sizeof(error)) < 0);
        snag_sse_free(&parser);
    }
}

static void
test_consumer_failure(void)
{
    static const char stream[] = "data: x\n\n";
    struct snag_sse_parser parser;
    char error[128] = {0};

    errno = 0;
    snag_sse_init(&parser, reject_record, NULL);
    assert(snag_sse_feed(&parser, stream, sizeof(stream) - 1u,
                        error, sizeof(error)) < 0);
    assert(errno == ECANCELED);
    assert(strstr(error, "consumer rejected"));
    snag_sse_free(&parser);
}

int
main(void)
{
    test_chunked_crlf_and_fields();
    test_comments_ids_and_empty_data();
    test_split_utf8_and_identical_events();
    test_bounds();
    test_failures();
    test_consumer_failure();
    puts("test_sse: ok");
    return 0;
}

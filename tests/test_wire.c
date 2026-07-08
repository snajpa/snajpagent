/* SPDX-License-Identifier: GPL-2.0-only */
#include "wire.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static int
contains(const struct snj_buf *buf, const void *needle, size_t len)
{
    if (!len)
        return 1;
    for (size_t i = 0; i + len <= buf->len; ++i)
        if (memcmp(buf->data + i, needle, len) == 0)
            return 1;
    return 0;
}

static void
expect_text(const struct snj_buf *buf, const char *expected)
{
    assert(buf->len == strlen(expected));
    assert(memcmp(buf->data, expected, buf->len) == 0);
}

static void
test_json(void)
{
    static const char *const secret_values[] = {"sk-test-secret", "needle"};
    const struct snj_wire_secrets secrets = {secret_values, 2u};
    static const unsigned char body[] =
        "{\"usage\":{\"input_tokens\":9},\"authorization\":\"Bearer bad\","
        "\"nested\":{\"encrypted_content\":\"opaque\","
        "\"text\":\"before needle after\"},\"real\":1.25,"
        "\"key\":\"sk-test-secret\"}";
    struct snj_buf out;
    char error[256];

    snj_buf_init(&out, SNJ_WIRE_BODY_MAX);
    assert(snj_wire_json_redact(body, sizeof(body) - 1u, &secrets, &out,
                                error, sizeof(error)) == 0);
    expect_text(&out,
        "{\"authorization\":\"<redacted:authorization>\","
        "\"key\":\"<redacted:secret>\","
        "\"nested\":{\"encrypted_content\":\"<redacted:encrypted_reasoning>\","
        "\"text\":\"before <redacted:secret> after\"},"
        "\"real\":1.25,\"usage\":{\"input_tokens\":9}}");
    assert(!contains(&out, "sk-test-secret", 14u));
    assert(!contains(&out, "needle", 6u));
    snj_buf_free(&out);
}

static void
test_invalid_json(void)
{
    struct snj_buf out;
    char error[256];
    static const unsigned char duplicate[] = "{\"x\":1,\"x\":2}";

    snj_buf_init(&out, SNJ_WIRE_BODY_MAX);
    errno = 0;
    assert(snj_wire_json_redact(duplicate, sizeof(duplicate) - 1u, NULL, &out,
                                error, sizeof(error)) < 0);
    assert(errno == EINVAL);
    snj_buf_free(&out);
}

static void
test_secret_object_key_fails_closed(void)
{
    static const char *const secret_values[] = {"secret-key"};
    const struct snj_wire_secrets secrets = {secret_values, 1u};
    static const unsigned char body[] = "{\"secret-key\":1}";
    struct snj_buf out;
    char error[256];

    snj_buf_init(&out, SNJ_WIRE_BODY_MAX);
    assert(snj_wire_json_redact(body, sizeof(body) - 1u, &secrets, &out,
                                error, sizeof(error)) < 0);
    assert(errno == EACCES);
    assert(out.len == 0u);
    snj_buf_free(&out);
}

static void
test_headers(void)
{
    static const char *const secret_values[] = {"hidden"};
    const struct snj_wire_secrets secrets = {secret_values, 1u};
    struct snj_buf out;

    snj_buf_init(&out, SNJ_WIRE_HEADER_MAX * 2u);
    assert(snj_wire_header_redact((const unsigned char *)
        "Authorization: Bearer sk-anything",
        sizeof("Authorization: Bearer sk-anything") - 1u, &secrets, &out) == 0);
    expect_text(&out, "authorization: <redacted:bearer>");
    assert(snj_wire_header_redact((const unsigned char *)
        "Set-Cookie: sid=bad", sizeof("Set-Cookie: sid=bad") - 1u, &secrets, &out) == 0);
    expect_text(&out, "set-cookie: <redacted:cookie>");
    assert(snj_wire_header_redact((const unsigned char *)
        "X-Trace: visible-hidden-tail",
        sizeof("X-Trace: visible-hidden-tail") - 1u, &secrets, &out) == 0);
    expect_text(&out, "x-trace: visible-<redacted:secret>-tail");
    snj_buf_free(&out);
}

static void
test_url_and_metadata(void)
{
    struct snj_buf out;
    static const char bytes[] = "not json";

    snj_buf_init(&out, SNJ_WIRE_BODY_MAX);
    assert(snj_wire_url_redact("https://example.test/x?a=one&token=two&flag",
                               NULL, &out) == 0);
    expect_text(&out,
        "https://example.test/x?a=<redacted:query>&token=<redacted:query>&flag");
    assert(snj_wire_body_metadata("application/octet-stream", bytes,
                                  sizeof(bytes) - 1u, &out) == 0);
    assert(contains(&out, "bytes=8", 7u));
    assert(contains(&out, "sha256=", 7u));
    assert(!contains(&out, bytes, sizeof(bytes) - 1u));
    snj_buf_free(&out);
}

int
main(void)
{
    test_json();
    test_invalid_json();
    test_secret_object_key_fails_closed();
    test_headers();
    test_url_and_metadata();
    puts("test_wire: ok");
    return 0;
}

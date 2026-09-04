/* SPDX-License-Identifier: GPL-2.0-only */
#include "provider.h"

#include "base.h"
#include "provider_retry.h"
#include "context.h"
#include "json.h"
#include "responses.h"
#include "secret.h"
#include "sse.h"
#include "snajpagent.h"
#include "wire.h"

#include <curl/curl.h>
#include <errno.h>
#include "snj_jansson.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

struct provider_ctx {
    struct snj_sse_parser sse;
    struct snj_responses_stream stream;
    struct snj_buf error_body;
    struct snj_secret_set secrets;
    const struct snj_config *config;
    const struct snj_provider_config *provider;
    struct snj_render *render;
    snj_provider_pump_fn pump;
    void *pump_opaque;
    long http_status;
    int cancel_code;
    uint32_t retry_after_ms;
    bool retry_after_present;
    bool body_failed;
    bool semantic_body_seen;
    bool request_may_have_been_sent;
    char error[256];
    struct snj_provider_failure provider_failure;
};

static void
set_error(char *error, size_t size, const char *message)
{
    if (size)
        (void)snprintf(error, size, "%s", message);
}

static void
ctx_error(struct provider_ctx *ctx, const char *message)
{
    (void)snprintf(ctx->error, sizeof(ctx->error), "%s", message);
}

static const char *
stream_or_sse_error(struct provider_ctx *ctx, const char *sse_error,
                    const char *fallback)
{
    if (ctx->stream.failed)
        return snj_responses_stream_error(&ctx->stream);
    if (sse_error && sse_error[0])
        return sse_error;
    return fallback;
}

static bool
ascii_printable(const unsigned char *data, size_t len)
{
    for (size_t i = 0; i < len; ++i)
        if (data[i] < 0x20u || data[i] > 0x7eu)
            return false;
    return true;
}

static void
strip_crlf(const char *input, size_t len, const unsigned char **out,
           size_t *out_len)
{
    while (len && (input[len - 1u] == '\r' || input[len - 1u] == '\n'))
        --len;
    *out = (const unsigned char *)input;
    *out_len = len;
}

static int
append_host_header(struct snj_buf *out, const char *base_url)
{
    const char *host;
    const char *end;

    if (strncmp(base_url, "https://", 8u) == 0)
        host = base_url + 8u;
    else if (strncmp(base_url, "http://", 7u) == 0)
        host = base_url + 7u;
    else {
        errno = EINVAL;
        return -1;
    }
    end = strchr(host, '/');
    if (!end)
        end = host + strlen(host);
    if (end == host)
        return -1;
    return snj_buf_append(out, "host: ", 6u) == 0 &&
           snj_buf_append(out, host, (size_t)(end - host)) == 0 &&
           snj_buf_terminate(out) == 0 ? 0 : -1;
}

static int
render_config_header(struct provider_ctx *ctx, struct snj_buf *redacted,
                     const char *name, const char *value)
{
    struct snj_buf line;
    int rc = 0;

    if (!value[0])
        return 0;
    snj_buf_init(&line, SNJ_WIRE_HEADER_MAX);
    if (snj_buf_printf(&line, "%s: %s", name, value) < 0 ||
        snj_wire_header_redact(line.data, line.len, &ctx->secrets.wire,
                               redacted) < 0 ||
        snj_render_transport(ctx->render, '>', (const char *)redacted->data,
                             redacted->len) < 0)
        rc = -1;
    snj_buf_free(&line);
    return rc;
}

static int
render_request_headers(struct provider_ctx *ctx, const char *request_line,
                       const char *accept)
{
    struct snj_buf redacted;
    struct snj_buf host;
    int rc = 0;

    if (!ctx->render || ctx->render->verbosity < 6u)
        return 0;
    snj_buf_init(&redacted, SNJ_WIRE_HEADER_MAX);
    snj_buf_init(&host, SNJ_CONFIG_URL_MAX + 8u);
    if (append_host_header(&host, ctx->provider->base_url) < 0) {
        snj_buf_free(&redacted);
        snj_buf_free(&host);
        return -1;
    }
    if (snj_render_transport(ctx->render, '>',
                             request_line, strlen(request_line)) < 0 ||
        snj_render_transport(ctx->render, '>',
                             (const char *)host.data, host.len) < 0 ||
        snj_render_transport(ctx->render, '>',
                             accept, strlen(accept)) < 0 ||
        snj_render_transport(ctx->render, '>',
                             "content-type: application/json",
                             strlen("content-type: application/json")) < 0 ||
        snj_wire_header_redact((const unsigned char *)"authorization: Bearer x",
                               23u, &ctx->secrets.wire, &redacted) < 0 ||
        snj_render_transport(ctx->render, '>', (const char *)redacted.data,
                             redacted.len) < 0)
        rc = -1;
    snj_buf_reset(&redacted);
    if (rc == 0 &&
        render_config_header(ctx, &redacted, "HTTP-Referer",
                             ctx->provider->openrouter_referer) < 0)
        rc = -1;
    if (rc == 0 &&
        render_config_header(ctx, &redacted, "X-OpenRouter-Title",
                             ctx->provider->openrouter_title) < 0)
        rc = -1;
    snj_buf_free(&redacted);
    snj_buf_free(&host);
    return rc;
}

static size_t
count_write_cb(char *ptr, size_t size, size_t nmemb, void *opaque)
{
    struct provider_ctx *ctx = opaque;
    size_t len;

    if (size && nmemb > SIZE_MAX / size)
        return 0;
    len = size * nmemb;
    if (len == 0u)
        return 0u;
    if (snj_buf_append(&ctx->error_body, ptr, len) < 0) {
        ctx->body_failed = true;
        ctx_error(ctx, ctx->http_status >= 200 && ctx->http_status < 300 ?
                  "provider JSON response body exceeds diagnostic bound" :
                  "provider error body exceeds diagnostic bound");
        return 0;
    }
    return len;
}

static long
parse_status(const unsigned char *line, size_t len)
{
    long value = 0;
    size_t i = 0;

    if (len < 12u || memcmp(line, "HTTP/", 5u) != 0)
        return 0;
    while (i < len && line[i] != ' ')
        ++i;
    if (i >= len || line[i] != ' ')
        return 0;
    ++i;
    if (i + 3u > len)
        return 0;
    for (size_t j = 0; j < 3u; ++j) {
        if (line[i + j] < '0' || line[i + j] > '9')
            return 0;
        value = value * 10 + (long)(line[i + j] - '0');
    }
    return value;
}

static size_t
header_cb(char *buffer, size_t size, size_t nmemb, void *opaque)
{
    struct provider_ctx *ctx = opaque;
    const unsigned char *line;
    size_t len = size * nmemb;
    size_t clean_len;
    struct snj_buf redacted;
    long status;

    if (size && nmemb > SIZE_MAX / size)
        return 0;
    strip_crlf(buffer, len, &line, &clean_len);
    if (clean_len == 0u)
        return len;
    status = parse_status(line, clean_len);
    if (status)
        ctx->http_status = status;
    if (clean_len > 12u && strncasecmp((const char *)line,
                                       "retry-after:", 12u) == 0) {
        uint32_t delay_ms;
        if (snj_provider_retry_after_parse(line + 12u, clean_len - 12u,
                                           &delay_ms) == 0) {
            ctx->retry_after_present = true;
            ctx->retry_after_ms = delay_ms;
        }
    }
    if (ctx->render && ctx->render->verbosity >= 6u) {
        if (status) {
            if (!ascii_printable(line, clean_len) ||
                snj_render_transport(ctx->render, '<', (const char *)line,
                                     clean_len) < 0) {
                ctx_error(ctx, "HTTP status diagnostics could not be rendered");
                return 0;
            }
        } else {
            snj_buf_init(&redacted, SNJ_WIRE_HEADER_MAX);
            if (snj_wire_header_redact(line, clean_len, &ctx->secrets.wire,
                                       &redacted) < 0 ||
                snj_render_transport(ctx->render, '<', (const char *)redacted.data,
                                     redacted.len) < 0) {
                snj_buf_free(&redacted);
                ctx_error(ctx, "HTTP header diagnostics could not be rendered");
                return 0;
            }
            snj_buf_free(&redacted);
        }
    }
    return len;
}

static size_t
write_cb(char *ptr, size_t size, size_t nmemb, void *opaque)
{
    struct provider_ctx *ctx = opaque;
    size_t len;
    char error[256] = {0};

    if (size && nmemb > SIZE_MAX / size)
        return 0;
    len = size * nmemb;
    if (len == 0u)
        return 0u;
    if (ctx->http_status >= 200 && ctx->http_status < 300) {
        ctx->semantic_body_seen = true;
        if (snj_sse_feed(&ctx->sse, ptr, len, error, sizeof(error)) < 0) {
            (void)snprintf(ctx->error, sizeof(ctx->error), "%s",
                           stream_or_sse_error(ctx, error,
                                               "invalid provider SSE stream"));
            return 0;
        }
    } else {
        if (snj_buf_append(&ctx->error_body, ptr, len) < 0) {
            ctx->body_failed = true;
            ctx_error(ctx, "provider error body exceeds diagnostic bound");
            return 0;
        }
    }
    return len;
}

static int
progress_cb(void *opaque, curl_off_t dltotal, curl_off_t dlnow,
            curl_off_t ultotal, curl_off_t ulnow)
{
    struct provider_ctx *ctx = opaque;
    int rc;

    (void)dltotal;
    (void)dlnow;
    (void)ultotal;
    (void)ulnow;
    if (!ctx->pump)
        return 0;
    rc = ctx->pump(ctx->pump_opaque, 0u);
    if (rc < 0) {
        ctx->cancel_code = 3;
        ctx_error(ctx, "active input could not be processed");
        return 1;
    }
    if (rc == 1 || rc == 2) {
        ctx->cancel_code = rc;
        return 1;
    }
    return 0;
}

static int
provider_endpoint_url(const struct snj_provider_config *provider,
                      const char *path,
                      char *buffer, size_t buffer_size, const char **url,
                      char *error, size_t error_size)
{
    const char *append_path;
    size_t base_len;
    int written;

    if (!provider || !path || !url || !buffer || !buffer_size) {
        set_error(error, error_size, "invalid provider endpoint");
        errno = EINVAL;
        return -1;
    }
    append_path = path;
    base_len = strlen(provider->base_url);
    while (base_len && provider->base_url[base_len - 1u] == '/')
        --base_len;
    if (base_len >= 3u &&
        memcmp(provider->base_url + base_len - 3u, "/v1", 3u) == 0 &&
        strncmp(path, "/v1/", 4u) == 0)
        append_path = path + 3u;
    written = snprintf(buffer, buffer_size, "%.*s%s", (int)base_len,
                       provider->base_url, append_path);
    if (written <= 0 || (size_t)written >= buffer_size) {
        set_error(error, error_size, "provider endpoint is too long");
        errno = ENAMETOOLONG;
        return -1;
    }
    *url = buffer;
#ifdef SNAJPAGENT_TEST_TRANSPORT_ENDPOINTS
    {
        const char *base = getenv("SNAJPAGENT_TEST_OPENAI_BASE");
        if (!base || !*base)
            return 0;
        append_path = path;
        base_len = strlen(base);
        while (base_len && base[base_len - 1u] == '/')
            --base_len;
        if (base_len >= 3u && memcmp(base + base_len - 3u, "/v1", 3u) == 0 &&
            strncmp(path, "/v1/", 4u) == 0)
            append_path = path + 3u;
        written = snprintf(buffer, buffer_size, "%.*s%s", (int)base_len,
                           base, append_path);
        if (written <= 0 || (size_t)written >= buffer_size) {
            set_error(error, error_size, "test provider endpoint is too long");
            errno = ENAMETOOLONG;
            return -1;
        }
        *url = buffer;
    }
#endif
    return 0;
}

static int
append_header(struct curl_slist **headers, const char *text)
{
    struct curl_slist *next = curl_slist_append(*headers, text);
    if (!next)
        return -1;
    *headers = next;
    return 0;
}

static int
append_authorization(struct curl_slist **headers,
                     const struct snj_credential *credential)
{
    struct snj_buf line;
    int rc;

    snj_buf_init(&line, SNJ_CREDENTIAL_MAX + 32u);
    rc = snj_buf_append(&line, "Authorization: Bearer ", 22u);
    if (rc == 0)
        rc = snj_buf_append(&line, credential->value, credential->len);
    if (rc == 0)
        rc = snj_buf_terminate(&line);
    if (rc == 0)
        rc = append_header(headers, (const char *)line.data);
    snj_buf_free(&line);
    return rc;
}

static int
append_named_header(struct curl_slist **headers, const char *name,
                    const char *value)
{
    struct snj_buf line;
    int rc;

    if (!value[0])
        return 0;
    snj_buf_init(&line, SNJ_WIRE_HEADER_MAX);
    rc = snj_buf_printf(&line, "%s: %s", name, value);
    if (rc == 0)
        rc = append_header(headers, (const char *)line.data);
    snj_buf_free(&line);
    return rc;
}

static int
append_provider_headers(struct curl_slist **headers,
                        const struct snj_provider_config *provider,
                        const struct snj_credential *credential)
{
    return append_authorization(headers, credential) == 0 &&
           append_named_header(headers, "HTTP-Referer",
                               provider->openrouter_referer) == 0 &&
           append_named_header(headers, "X-OpenRouter-Title",
                               provider->openrouter_title) == 0 ?
           0 : -1;
}

static unsigned int
low_speed_seconds(uint32_t idle_timeout_ms)
{
    uint32_t seconds = idle_timeout_ms / 1000u;
    if (idle_timeout_ms % 1000u)
        ++seconds;
    return seconds ? seconds : 1u;
}

static void
begin_attempt(struct provider_ctx *ctx)
{
    ctx->http_status = 0;
    ctx->cancel_code = 0;
    ctx->retry_after_ms = 0u;
    ctx->retry_after_present = false;
    ctx->body_failed = false;
    ctx->semantic_body_seen = false;
    ctx->error[0] = '\0';
    snj_buf_reset(&ctx->error_body);
}

static bool
curl_code_retryable(CURLcode code, bool semantic_body_seen)
{
    if (semantic_body_seen)
        return false;
    switch (code) {
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_RESOLVE_PROXY:
    case CURLE_COULDNT_CONNECT:
    case CURLE_OPERATION_TIMEDOUT:
    case CURLE_SEND_ERROR:
    case CURLE_RECV_ERROR:
    case CURLE_GOT_NOTHING:
    case CURLE_PARTIAL_FILE:
    case CURLE_SSL_CONNECT_ERROR:
        return true;
    default:
        return false;
    }
}

static int
sleep_ms(uint32_t delay_ms)
{
    struct timespec ts;

    ts.tv_sec = (time_t)(delay_ms / 1000u);
    ts.tv_nsec = (long)(delay_ms % 1000u) * 1000000L;
    while (nanosleep(&ts, &ts) < 0) {
        if (errno == EINTR)
            continue;
        return -1;
    }
    return 0;
}

static int
retry_wait(struct provider_ctx *ctx, unsigned int retries_done,
           const char *reason, char *error, size_t error_size)
{
    uint32_t delay_ms = snj_provider_retry_delay_ms(retries_done,
        ctx->retry_after_present, ctx->retry_after_ms);
    uint32_t remaining = delay_ms;

    if (ctx->render && ctx->render->verbosity >= 3u) {
        char line[160];
        (void)snprintf(line, sizeof(line),
                       "provider retry %u/%u after %s in %llums",
                       retries_done + 1u, SNJ_PROVIDER_MAX_RETRIES,
                       reason, (unsigned long long)delay_ms);
        if (snj_render_runtime(ctx->render, line) < 0) {
            set_error(error, error_size, "provider retry diagnostics could not be rendered");
            errno = EIO;
            return -1;
        }
    }
    while (remaining > 0u) {
        uint32_t slice = remaining > 100u ? 100u : remaining;
        if (ctx->pump) {
            int rc = ctx->pump(ctx->pump_opaque, slice);
            if (rc < 0) {
                set_error(error, error_size,
                          "active input could not be processed during provider retry");
                errno = EIO;
                return -1;
            }
            if (rc == 1 || rc == 2) {
                ctx->cancel_code = rc;
                return rc;
            }
        } else if (sleep_ms(slice) < 0) {
            set_error(error, error_size, "provider retry sleep failed");
            return -1;
        }
        remaining -= slice;
    }
    return 0;
}

static bool
retryable_attempt(struct provider_ctx *ctx, CURLcode code)
{
    if (ctx->body_failed)
        return false;
    if (code == CURLE_OK)
        return snj_provider_http_status_retryable(ctx->http_status);
    if (code == CURLE_ABORTED_BY_CALLBACK)
        return false;
    return curl_code_retryable(code, ctx->semantic_body_seen);
}

static int
retry_reason(struct provider_ctx *ctx, CURLcode code,
             char *reason, size_t reason_size)
{
    if (code == CURLE_OK)
        return snprintf(reason, reason_size, "HTTP %ld", ctx->http_status) > 0 ?
               0 : -1;
    return snprintf(reason, reason_size, "%s", curl_easy_strerror(code)) > 0 ?
           0 : -1;
}

static CURLcode
perform_with_retry(CURL *curl, struct provider_ctx *ctx,
                   char *error, size_t error_size, int *cancel_code,
                   unsigned int *retry_count)
{
    CURLcode code = CURLE_OK;
    unsigned int retries = 0u;

    if (retry_count)
        *retry_count = 0u;
    for (;;) {
        long request_size = 0;
        begin_attempt(ctx);
        code = curl_easy_perform(curl);
        if (curl_easy_getinfo(curl, CURLINFO_REQUEST_SIZE,
                              &request_size) == CURLE_OK && request_size > 0)
            ctx->request_may_have_been_sent = true;
        if (code == CURLE_ABORTED_BY_CALLBACK &&
            (ctx->cancel_code == 1 || ctx->cancel_code == 2)) {
            if (cancel_code)
                *cancel_code = ctx->cancel_code;
            break;
        }
        if (retries >= SNJ_PROVIDER_MAX_RETRIES ||
            !retryable_attempt(ctx, code))
            break;
        {
            char reason[96];
            int wait_rc;
            if (retry_reason(ctx, code, reason, sizeof(reason)) < 0)
                snprintf(reason, sizeof(reason), "retryable provider failure");
            wait_rc = retry_wait(ctx, retries, reason, error, error_size);
            if (wait_rc == 1 || wait_rc == 2) {
                code = CURLE_ABORTED_BY_CALLBACK;
                if (cancel_code)
                    *cancel_code = wait_rc;
                break;
            }
            if (wait_rc < 0) {
                code = CURLE_ABORTED_BY_CALLBACK;
                break;
            }
        }
        ++retries;
        if (retry_count)
            *retry_count = retries;
    }
    return code;
}

static void
append_retry_suffix(char *error, size_t error_size,
                    unsigned int retry_count, bool request_may_have_been_sent)
{
    size_t len;

    if (!error || !error_size || retry_count == 0u)
        return;
    len = strlen(error);
    if (len >= error_size - 1u)
        return;
    (void)snprintf(error + len, error_size - len,
                   "; retried %u time%s%s",
                   retry_count, retry_count == 1u ? "" : "s",
                   request_may_have_been_sent ?
                   "; request may have been sent during an earlier attempt" : "");
}

static int
classify_non2xx(struct provider_ctx *ctx, char *error, size_t error_size)
{
    struct snj_buf redacted;
    char json_error[128] = {0};
    int rc;

    if (ctx->body_failed) {
        set_error(error, error_size, ctx->error[0] ? ctx->error :
                  "provider error body could not be retained");
        errno = EOVERFLOW;
        return -1;
    }
    snj_buf_init(&redacted, SNJ_WIRE_BODY_MAX);
    if (ctx->error_body.len) {
        json_t *root = snj_json_load_strict(ctx->error_body.data,
                                            ctx->error_body.len,
                                            SNJ_WIRE_BODY_MAX,
                                            json_error,
                                            sizeof(json_error));
        if (root) {
            if (snj_provider_failure_from_json(root,
                                               &ctx->provider_failure) < 0)
                memset(&ctx->provider_failure, 0,
                       sizeof(ctx->provider_failure));
            json_decref(root);
        }
        json_error[0] = '\0';
    }
    rc = snj_wire_json_redact(ctx->error_body.data, ctx->error_body.len,
                              &ctx->secrets.wire, &redacted,
                              json_error, sizeof(json_error));
    if (rc == 0 && ctx->render && ctx->render->verbosity >= 5u)
        (void)snj_render_protocol(ctx->render, "response.error.body",
                                  (const char *)redacted.data, redacted.len);
    if (ctx->error_body.len) {
        if (rc == 0)
            (void)snprintf(error, error_size,
                           "provider HTTP %ld: %.*s", ctx->http_status,
                           (int)(redacted.len > 160u ? 160u : redacted.len),
                           (const char *)redacted.data);
        else
            (void)snprintf(error, error_size,
                           "provider HTTP %ld with non-JSON error body (%s)",
                           ctx->http_status, json_error[0] ? json_error : "unreadable");
    } else {
        (void)snprintf(error, error_size, "provider HTTP %ld", ctx->http_status);
    }
    snj_buf_free(&redacted);
    errno = EIO;
    return -1;
}

static int
parse_count_body(struct provider_ctx *ctx, uint64_t *input_tokens,
                 char *error, size_t error_size)
{
    static const char *const keys[] = {"input_tokens", "object"};
    char json_error[128] = {0};
    json_t *root;
    const char *object;
    int rc = -1;

    if (ctx->body_failed) {
        set_error(error, error_size, ctx->error[0] ? ctx->error :
                  "input-token count body could not be retained");
        errno = EOVERFLOW;
        return -1;
    }
    root = snj_json_load_strict(ctx->error_body.data, ctx->error_body.len,
                                SNJ_WIRE_BODY_MAX, json_error,
                                sizeof(json_error));
    if (!root) {
        (void)snprintf(error, error_size,
                       "invalid input-token count response: %s", json_error);
        errno = EPROTO;
        return -1;
    }
    object = snj_json_string(root, "object");
    if (!snj_json_exact_keys(root, keys, sizeof(keys) / sizeof(keys[0])) ||
        !object || strcmp(object, "response.input_tokens") != 0 ||
        snj_json_integer_u64(root, "input_tokens", input_tokens) < 0) {
        set_error(error, error_size,
                  "input-token count response has an invalid shape");
        errno = EPROTO;
        goto out;
    }
    rc = 0;
out:
    json_decref(root);
    return rc;
}

static int
parse_compact_body(struct provider_ctx *ctx, json_t **output,
                   uint64_t *output_tokens_bound,
                   char *error, size_t error_size)
{
    char json_error[128] = {0};
    char output_hash[SNJ_SHA256_HEX_LEN + 1u];
    size_t output_bytes = 0u;
    json_t *root;
    json_t *body_output;
    const char *object;
    int rc = -1;

    if (output)
        *output = NULL;
    if (output_tokens_bound)
        *output_tokens_bound = 0u;
    if (ctx->body_failed) {
        set_error(error, error_size, ctx->error[0] ? ctx->error :
                  "compact response body could not be retained");
        errno = EOVERFLOW;
        return -1;
    }
    root = snj_json_load_strict(ctx->error_body.data, ctx->error_body.len,
                                SNJ_CONTEXT_MAX_COMPACT, json_error,
                                sizeof(json_error));
    if (!root) {
        (void)snprintf(error, error_size,
                       "invalid compact response: %s", json_error);
        errno = EPROTO;
        return -1;
    }
    object = snj_json_string(root, "object");
    body_output = json_object_get(root, "output");
    if (!object || strcmp(object, "response.compaction") != 0 ||
        snj_context_compact_output_valid(body_output, output_hash,
                                         &output_bytes,
                                         error, error_size) < 0) {
        if (error && !error[0])
            set_error(error, error_size,
                      "compact response has an invalid shape");
        errno = EPROTO;
        goto out;
    }
    if (!output || !output_tokens_bound ||
        output_bytes > (size_t)UINT64_MAX) {
        set_error(error, error_size, "invalid compact response destination");
        errno = EINVAL;
        goto out;
    }
    *output = json_deep_copy(body_output);
    if (!*output) {
        set_error(error, error_size, "compact output could not be retained");
        errno = ENOMEM;
        goto out;
    }
    *output_tokens_bound = (uint64_t)output_bytes;
    rc = 0;
out:
    json_decref(root);
    return rc;
}

#define SNJ_PROVIDER_MODELS_MAX 4096u
#define SNJ_PROVIDER_EFFORTS_MAX 32u
#define SNJ_CODEX_CATALOG_CLIENT_VERSION "0.146.0"
#define SNJ_CODEX_CATALOG_PATH \
    "/models?client_version=" SNJ_CODEX_CATALOG_CLIENT_VERSION

struct codex_model_ref {
    const json_t *model;
    json_int_t priority;
    size_t order;
};

struct optional_limit {
    uint64_t value;
    bool known;
};

static int
merge_limit(const json_t *object, const char *key, uint64_t max,
            struct optional_limit *out)
{
    json_t *value;
    json_int_t integer;

    if (!object || !(value = json_object_get(object, key)) ||
        json_is_null(value))
        return 0;
    if (!json_is_integer(value) || (integer = json_integer_value(value)) <= 0 ||
        (uint64_t)integer > max ||
        (out->known && out->value != (uint64_t)integer))
        return -1;
    out->known = true;
    out->value = (uint64_t)integer;
    return 0;
}

static int
collect_limit(const json_t *const *objects, size_t object_count,
              const char *const *keys, size_t key_count, uint64_t max,
              struct optional_limit *out)
{
    for (size_t i = 0; i < object_count; ++i)
        for (size_t j = 0; j < key_count; ++j)
            if (merge_limit(objects[i], keys[j], max, out) < 0)
                return -1;
    return 0;
}

static int
set_optional_limit(json_t *limits, const char *key,
                   const struct optional_limit *value)
{
    return snj_json_set_new(limits, key,
        value->known ? json_integer((json_int_t)value->value) : json_null());
}

static int
build_model_limits(const json_t *source, bool codex, json_t **out)
{
    static const char *const context_keys[] = {
        "context_window_tokens", "contextWindowTokens", "context_window",
        "contextWindow", "context_length", "contextLength"
    };
    static const char *const max_context_keys[] = {
        "max_context_window_tokens", "maxContextWindowTokens",
        "max_context_window", "maxContextWindow"
    };
    static const char *const input_context_keys[] = {
        "input_context_window_tokens", "inputContextWindowTokens",
        "input_context_window", "inputContextWindow"
    };
    static const char *const max_input_keys[] = {
        "max_input_tokens", "maxInputTokens"
    };
    static const char *const max_output_keys[] = {
        "max_output_tokens", "maxOutputTokens"
    };
    static const char *const auto_compact_keys[] = {
        "auto_compact_input_tokens", "autoCompactInputTokens",
        "auto_compact_token_limit", "autoCompactTokenLimit"
    };
    static const char *const effective_keys[] = {
        "effective_context_window_percent", "effectiveContextWindowPercent"
    };
    const json_t *objects[3] = {source, NULL, NULL};
    size_t object_count = 1u;
    struct optional_limit context = {0};
    struct optional_limit max_context = {0};
    struct optional_limit input_context = {0};
    struct optional_limit max_input = {0};
    struct optional_limit max_output = {0};
    struct optional_limit auto_compact = {0};
    struct optional_limit effective = {0};
    json_t *limits = NULL;

    if (!codex) {
        static const char *const nested[] = {"metadata", "capabilities"};
        for (size_t i = 0; i < sizeof(nested) / sizeof(nested[0]); ++i) {
            json_t *value = json_object_get(source, nested[i]);
            if (!value || json_is_null(value))
                continue;
            if (!json_is_object(value))
                goto invalid;
            objects[object_count++] = value;
        }
    }
#define COLLECT(field, keys, max) \
    collect_limit(objects, object_count, keys, \
                  sizeof(keys) / sizeof((keys)[0]), max, &field)
    if (codex) {
        static const char *const native_context[] = {"context_window"};
        static const char *const native_max_context[] = {"max_context_window"};
        static const char *const native_input_context[] = {"input_context_window"};
        static const char *const native_max_input[] = {"max_input_tokens"};
        static const char *const native_max_output[] = {"max_output_tokens"};
        static const char *const native_auto[] = {"auto_compact_token_limit"};
        static const char *const native_effective[] = {
            "effective_context_window_percent"
        };
        if (COLLECT(context, native_context, SNJ_CONFIG_TOKEN_LIMIT_MAX) < 0 ||
            COLLECT(max_context, native_max_context,
                    SNJ_CONFIG_TOKEN_LIMIT_MAX) < 0 ||
            COLLECT(input_context, native_input_context,
                    SNJ_CONFIG_TOKEN_LIMIT_MAX) < 0 ||
            COLLECT(max_input, native_max_input,
                    SNJ_CONFIG_TOKEN_LIMIT_MAX) < 0 ||
            COLLECT(max_output, native_max_output,
                    SNJ_CONFIG_TOKEN_LIMIT_MAX) < 0 ||
            COLLECT(auto_compact, native_auto,
                    SNJ_CONFIG_TOKEN_LIMIT_MAX) < 0 ||
            COLLECT(effective, native_effective, 100u) < 0)
            goto invalid;
    } else if (COLLECT(context, context_keys,
                       SNJ_CONFIG_TOKEN_LIMIT_MAX) < 0 ||
               COLLECT(max_context, max_context_keys,
                       SNJ_CONFIG_TOKEN_LIMIT_MAX) < 0 ||
               COLLECT(input_context, input_context_keys,
                       SNJ_CONFIG_TOKEN_LIMIT_MAX) < 0 ||
               COLLECT(max_input, max_input_keys,
                       SNJ_CONFIG_TOKEN_LIMIT_MAX) < 0 ||
               COLLECT(max_output, max_output_keys,
                       SNJ_CONFIG_TOKEN_LIMIT_MAX) < 0 ||
               COLLECT(auto_compact, auto_compact_keys,
                       SNJ_CONFIG_TOKEN_LIMIT_MAX) < 0 ||
               COLLECT(effective, effective_keys, 100u) < 0) {
        goto invalid;
    }
#undef COLLECT
    if ((context.known && max_context.known &&
         context.value > max_context.value) ||
        (context.known && input_context.known &&
         input_context.value > context.value) ||
        (context.known && max_input.known && max_input.value > context.value) ||
        (context.known && max_output.known &&
         max_output.value > context.value) ||
        (context.known && max_input.known && max_output.known &&
         max_input.value > context.value - max_output.value) ||
        (context.known && auto_compact.known &&
         auto_compact.value > context.value))
        goto invalid;
    limits = json_object();
    if (!limits ||
        set_optional_limit(limits, "context_window_tokens", &context) < 0 ||
        set_optional_limit(limits, "max_context_window_tokens",
                           &max_context) < 0 ||
        set_optional_limit(limits, "input_context_window_tokens",
                           &input_context) < 0 ||
        set_optional_limit(limits, "max_input_tokens", &max_input) < 0 ||
        set_optional_limit(limits, "max_output_tokens", &max_output) < 0 ||
        set_optional_limit(limits, "auto_compact_input_tokens",
                           &auto_compact) < 0 ||
        set_optional_limit(limits, "effective_context_window_percent",
                           &effective) < 0)
        goto fail;
    *out = limits;
    return 0;
invalid:
    errno = EPROTO;
fail:
    if (limits)
        json_decref(limits);
    return -1;
}

static bool
bounded_utf8_string(const json_t *value, size_t max, const char **out)
{
    const char *text;
    size_t len;

    if (!json_is_string(value))
        return false;
    text = json_string_value(value);
    len = json_string_length(value);
    if (!text || !len || len > max || strlen(text) != len ||
        !snj_utf8_valid((const unsigned char *)text, len, true))
        return false;
    *out = text;
    return true;
}

static int
append_efforts(json_t *out, const json_t *source, bool codex)
{
    if (!source)
        return 0;
    if (!json_is_array(source) ||
        json_array_size(source) > SNJ_PROVIDER_EFFORTS_MAX)
        return -1;
    for (size_t i = 0; i < json_array_size(source); ++i) {
        json_t *value = json_array_get(source, i);
        const char *effort = NULL;
        bool duplicate = false;

        if (json_is_object(value))
            value = json_object_get(value, "effort");
        else if (codex)
            return -1;
        if (!bounded_utf8_string(value, SNJ_CONFIG_EFFORT_MAX - 1u,
                                 &effort))
            return -1;
        for (size_t j = 0; j < json_array_size(out); ++j)
            if (strcmp(json_string_value(json_array_get(out, j)), effort) == 0) {
                duplicate = true;
                break;
            }
        if (!duplicate && json_array_append_new(out, json_string(effort)) < 0)
            return -1;
    }
    return 0;
}

static int
append_model(json_t *out, const json_t *source, bool codex)
{
    const char *id;
    const char *default_effort = NULL;
    json_t *metadata;
    json_t *effort_source;
    json_t *entry = NULL;
    json_t *efforts = NULL;
    json_t *limits = NULL;

    if (!json_is_object(source) ||
        !bounded_utf8_string(json_object_get(source, codex ? "slug" : "id"),
                             SNJ_CONFIG_MODEL_MAX - 1u, &id))
        return -1;
    for (size_t i = 0; i < json_array_size(out); ++i) {
        const char *existing = snj_json_string(json_array_get(out, i), "id");
        if (existing && strcmp(existing, id) == 0)
            return 0;
    }
    metadata = json_object_get(source, "metadata");
    if (!json_is_object(metadata))
        metadata = NULL;
    effort_source = json_object_get(source, "supported_reasoning_levels");
    if (!effort_source && metadata)
        effort_source = json_object_get(metadata, "supported_reasoning_levels");
    if (json_is_null(effort_source))
        effort_source = NULL;
    {
        json_t *value = json_object_get(source, "default_reasoning_level");
        if (!value && metadata)
            value = json_object_get(metadata, "default_reasoning_level");
        if (value && !json_is_null(value) &&
            !bounded_utf8_string(value, SNJ_CONFIG_EFFORT_MAX - 1u,
                                 &default_effort))
            return -1;
    }
    if (codex && default_effort && !effort_source)
        return -1;
    entry = json_object();
    efforts = json_array();
    if (!entry || !efforts ||
        append_efforts(efforts, effort_source, codex) < 0 ||
        build_model_limits(source, codex, &limits) < 0)
        goto fail;
    if (codex && default_effort) {
        bool supported = false;

        for (size_t i = 0; i < json_array_size(efforts); ++i)
            if (strcmp(json_string_value(json_array_get(efforts, i)),
                       default_effort) == 0) {
                supported = true;
                break;
            }
        if (!supported)
            goto fail;
    }
    if (snj_json_set_new(entry, "default_effort",
                         default_effort ? json_string(default_effort) :
                                          json_null()) < 0)
        goto fail;
    if (snj_json_set_new(entry, "efforts", efforts) < 0) {
        efforts = NULL;
        goto fail;
    }
    efforts = NULL;
    if (snj_json_set_new(entry, "id", json_string(id)) < 0)
        goto fail;
    if (snj_json_set_new(entry, "limits", limits) < 0) {
        limits = NULL;
        goto fail;
    }
    limits = NULL;
    if (json_array_append_new(out, entry) < 0) {
        entry = NULL;
        goto fail;
    }
    entry = NULL;
    return 0;
fail:
    if (limits)
        json_decref(limits);
    if (efforts)
        json_decref(efforts);
    if (entry)
        json_decref(entry);
    errno = EPROTO;
    return -1;
}

static int
codex_model_ref_compare(const void *left, const void *right)
{
    const struct codex_model_ref *a = left;
    const struct codex_model_ref *b = right;

    if (a->priority < b->priority)
        return -1;
    if (a->priority > b->priority)
        return 1;
    return a->order < b->order ? -1 : a->order > b->order;
}

static int
decode_models(const unsigned char *data, size_t len, bool codex,
              json_t **models, char *error, size_t error_size)
{
    char json_error[128] = {0};
    json_t *root = NULL;
    json_t *source;
    json_t *out = NULL;
    struct codex_model_ref *refs = NULL;
    size_t ref_count = 0u;
    int rc = -1;

    if (models)
        *models = NULL;
    if (!data || !len || !models) {
        set_error(error, error_size, "invalid model catalog source");
        errno = EINVAL;
        return -1;
    }
    root = snj_json_load_strict(data, len,
                                SNJ_WIRE_BODY_MAX, json_error,
                                sizeof(json_error));
    if (!root || !json_is_object(root)) {
        (void)snprintf(error, error_size, "invalid model-list response: %s",
                       json_error[0] ? json_error : "root is not an object");
        errno = EPROTO;
        goto out;
    }
    source = json_object_get(root, codex ? "models" : "data");
    if (!json_is_array(source) ||
        json_array_size(source) > SNJ_PROVIDER_MODELS_MAX) {
        set_error(error, error_size,
                  "model-list response has no bounded models array");
        errno = EPROTO;
        goto out;
    }
    out = json_array();
    if (!out) {
        errno = ENOMEM;
        goto out;
    }
    if (codex && json_array_size(source)) {
        refs = calloc(json_array_size(source), sizeof(*refs));
        if (!refs) {
            errno = ENOMEM;
            goto out;
        }
        for (size_t i = 0; i < json_array_size(source); ++i) {
            json_t *model = json_array_get(source, i);
            const char *visibility;
            json_t *priority;

            if (!json_is_object(model)) {
                set_error(error, error_size,
                          "model-list response contains an invalid model entry");
                errno = EPROTO;
                goto out;
            }
            visibility = snj_json_string(model, "visibility");
            if (!visibility || strcmp(visibility, "list") != 0)
                continue;
            priority = json_object_get(model, "priority");
            if (!json_is_integer(priority)) {
                set_error(error, error_size,
                          "model-list response contains an invalid model entry");
                errno = EPROTO;
                goto out;
            }
            refs[ref_count].model = model;
            refs[ref_count].priority = json_integer_value(priority);
            refs[ref_count].order = i;
            ++ref_count;
        }
        qsort(refs, ref_count, sizeof(*refs), codex_model_ref_compare);
    }
    for (size_t i = 0; i < (codex ? ref_count : json_array_size(source)); ++i)
        if (append_model(out, codex ? refs[i].model : json_array_get(source, i),
                         codex) < 0) {
            set_error(error, error_size,
                      "model-list response contains an invalid model entry");
            errno = EPROTO;
            goto out;
        }
    *models = out;
    out = NULL;
    rc = 0;
out:
    free(refs);
    if (out)
        json_decref(out);
    if (root)
        json_decref(root);
    return rc;
}

static int
parse_models_body(struct provider_ctx *ctx, bool codex, json_t **models,
                  char *error, size_t error_size)
{
    if (ctx->body_failed) {
        set_error(error, error_size, ctx->error[0] ? ctx->error :
                  "model-list response body exceeds the supported limit");
        errno = EOVERFLOW;
        return -1;
    }
    return decode_models(ctx->error_body.data, ctx->error_body.len, codex,
                         models, error, error_size);
}

static bool
provider_uses_codex_catalog(const struct snj_provider_config *provider)
{
    static const char suffix[] = "/backend-api/codex";
    const char *authority = strstr(provider->base_url, "://");
    const char *path = authority ? strchr(authority + 3u, '/') : NULL;
    size_t len;

    if (!path)
        return false;
    len = strlen(path);
    while (len && path[len - 1u] == '/')
        --len;
    return len >= sizeof(suffix) - 1u &&
           memcmp(path + len - (sizeof(suffix) - 1u),
                  suffix, sizeof(suffix) - 1u) == 0;
}

const char *
snj_provider_catalog_protocol(const struct snj_provider_config *provider)
{
    if (!provider)
        return NULL;
    return provider_uses_codex_catalog(provider) ? "codex" : "openai";
}

static const char *
url_request_target(const char *url)
{
    const char *scheme = strstr(url, "://");
    const char *target = scheme ? strchr(scheme + 3u, '/') : NULL;

    return target ? target : "/";
}

int
snj_provider_models_list(const struct snj_config *config,
                         const struct snj_provider_config *provider,
                         const struct snj_credential *credential,
                         struct snj_render *render, json_t **models,
                         char *error, size_t error_size)
{
    struct provider_ctx ctx;
    struct curl_slist *headers = NULL;
    CURL *curl = NULL;
    CURLcode code;
    char url_buffer[SNJ_CONFIG_URL_MAX + 64u];
    char request_line[SNJ_CONFIG_URL_MAX + 96u];
    const char *url = NULL;
    const char *path;
    bool codex;
    unsigned int retry_count = 0u;
    int rc = -1;

    if (models)
        *models = NULL;
    if (!config || !provider || !credential || !credential->len || !models) {
        set_error(error, error_size, "invalid model-list request");
        errno = EINVAL;
        return -1;
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.config = config;
    ctx.provider = provider;
    ctx.render = render;
    snj_secret_set_build(&ctx.secrets, config, credential);
    snj_buf_init(&ctx.error_body, SNJ_WIRE_BODY_MAX);
    codex = provider_uses_codex_catalog(provider);
    path = codex ? SNJ_CODEX_CATALOG_PATH : "/v1/models";
    if (provider_endpoint_url(provider, path, url_buffer,
                              sizeof(url_buffer), &url,
                              error, error_size) < 0)
        goto out;
    {
        int written = snprintf(request_line, sizeof(request_line),
                               "GET %s HTTP/1.1",
                               url_request_target(url));
        if (written <= 0 || (size_t)written >= sizeof(request_line)) {
            set_error(error, error_size,
                      "model-list request line is too long");
            errno = ENAMETOOLONG;
            goto out;
        }
    }
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
        set_error(error, error_size, "libcurl could not initialize");
        errno = EIO;
        goto out;
    }
    curl = curl_easy_init();
    if (!curl) {
        set_error(error, error_size,
                  "libcurl easy handle could not initialize");
        errno = ENOMEM;
        goto out_global;
    }
    if (append_header(&headers, "Accept: application/json") < 0 ||
        append_provider_headers(&headers, provider, credential) < 0) {
        set_error(error, error_size, "provider headers could not be allocated");
        goto out_global;
    }
    if (render_request_headers(&ctx, request_line,
                               "accept: application/json") < 0) {
        set_error(error, error_size, ctx.error[0] ? ctx.error :
                  "model-list request headers could not be rendered");
        goto out_global;
    }
    if (curl_easy_setopt(curl, CURLOPT_URL, url) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, count_write_cb) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &ctx) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS,
                         (long)provider->connect_timeout_ms) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS,
                         (long)provider->request_timeout_ms) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME,
                         (long)low_speed_seconds(provider->idle_timeout_ms)) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_USERAGENT,
                         SNAJPAGENT_NAME "/" SNAJPAGENT_VERSION) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L) != CURLE_OK) {
        set_error(error, error_size, "libcurl option setup failed");
        errno = EIO;
        goto out_global;
    }
    code = perform_with_retry(curl, &ctx, error, error_size, NULL,
                              &retry_count);
    if (code != CURLE_OK) {
        (void)snprintf(error, error_size, "%s%s%s",
                       ctx.error[0] ? ctx.error : "model discovery failed",
                       ctx.error[0] ? "" : ": ",
                       ctx.error[0] ? "" : curl_easy_strerror(code));
        append_retry_suffix(error, error_size, retry_count,
                            ctx.request_may_have_been_sent);
        errno = EIO;
        goto out_global;
    }
    if (ctx.http_status < 200 || ctx.http_status >= 300) {
        (void)classify_non2xx(&ctx, error, error_size);
        append_retry_suffix(error, error_size, retry_count,
                            ctx.request_may_have_been_sent);
        goto out_global;
    }
    rc = parse_models_body(&ctx, codex, models, error, error_size);
out_global:
    if (curl)
        curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    curl_global_cleanup();
out:
    snj_buf_free(&ctx.error_body);
    return rc;
}

int
snj_provider_responses_count(const json_t *count_request,
                             const struct snj_config *config,
                             const struct snj_provider_config *provider,
                             const struct snj_credential *credential,
                             struct snj_render *render,
                             snj_provider_pump_fn pump,
                             void *pump_opaque,
                             uint64_t *input_tokens,
                             bool *endpoint_unsupported,
                             char *error, size_t error_size,
                             int *cancel_code,
                             unsigned int *retry_count)
{
    struct provider_ctx ctx;
    struct snj_buf body;
    struct curl_slist *headers = NULL;
    CURL *curl = NULL;
    CURLcode code;
    char url_buffer[SNJ_CONFIG_URL_MAX + 64u];
    const char *url = NULL;
    int rc = -1;

    if (cancel_code)
        *cancel_code = 0;
    if (retry_count)
        *retry_count = 0u;
    if (endpoint_unsupported)
        *endpoint_unsupported = false;
    if (!count_request || !config || !provider || !credential || !credential->len ||
        !input_tokens) {
        set_error(error, error_size, "invalid input-token count request");
        errno = EINVAL;
        return -1;
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.config = config;
    ctx.provider = provider;
    ctx.render = render;
    ctx.pump = pump;
    ctx.pump_opaque = pump_opaque;
    snj_secret_set_build(&ctx.secrets, config, credential);
    snj_buf_init(&ctx.error_body, SNJ_WIRE_BODY_MAX);
    snj_buf_init(&body, SNJ_CONTEXT_MAX_REQUEST);

    if (snj_json_canonical(count_request, &body) < 0) {
        set_error(error, error_size,
                  "input-token count request exceeds the bounded body limit");
        goto out;
    }
    if (provider_endpoint_url(provider, "/v1/responses/input_tokens",
                              url_buffer, sizeof(url_buffer), &url,
                              error, error_size) < 0)
        goto out;
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
        set_error(error, error_size, "libcurl could not initialize");
        errno = EIO;
        goto out;
    }
    curl = curl_easy_init();
    if (!curl) {
        set_error(error, error_size, "libcurl easy handle could not initialize");
        errno = ENOMEM;
        goto out_global;
    }
    if (append_header(&headers, "Accept: application/json") < 0 ||
        append_header(&headers, "Content-Type: application/json") < 0 ||
        append_provider_headers(&headers, provider, credential) < 0) {
        set_error(error, error_size, "provider headers could not be allocated");
        goto out_global;
    }
    if (render_request_headers(&ctx,
                               "POST /v1/responses/input_tokens HTTP/1.1",
                               "accept: application/json") < 0) {
        set_error(error, error_size, ctx.error[0] ? ctx.error :
                  "input-token count headers could not be rendered");
        goto out_global;
    }
    if (curl_easy_setopt(curl, CURLOPT_URL, url) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_POST, 1L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, (char *)body.data) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                         (curl_off_t)body.len) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, count_write_cb) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &ctx) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_cb) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS,
                         (long)provider->connect_timeout_ms) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS,
                         (long)provider->request_timeout_ms) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME,
                         (long)low_speed_seconds(provider->idle_timeout_ms)) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_USERAGENT,
                         SNAJPAGENT_NAME "/" SNAJPAGENT_VERSION) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L) != CURLE_OK) {
        set_error(error, error_size, "libcurl option setup failed");
        errno = EIO;
        goto out_global;
    }
    code = perform_with_retry(curl, &ctx, error, error_size, cancel_code,
                              retry_count);
    if (code == CURLE_ABORTED_BY_CALLBACK &&
        (ctx.cancel_code == 1 || ctx.cancel_code == 2)) {
        if (cancel_code)
            *cancel_code = ctx.cancel_code;
        rc = ctx.cancel_code;
        goto out_global;
    }
    if (code != CURLE_OK) {
        (void)snprintf(error, error_size, "%s%s%s",
                       ctx.error[0] ? ctx.error : "input-token count failed",
                       ctx.error[0] ? "" : ": ",
                       ctx.error[0] ? "" : curl_easy_strerror(code));
        append_retry_suffix(error, error_size,
                            retry_count ? *retry_count : 0u,
                            ctx.request_may_have_been_sent);
        errno = EIO;
        goto out_global;
    }
    if (ctx.http_status < 200 || ctx.http_status >= 300) {
        if (endpoint_unsupported &&
            (ctx.http_status == 405 || ctx.http_status == 501))
            *endpoint_unsupported = true;
        (void)classify_non2xx(&ctx, error, error_size);
        append_retry_suffix(error, error_size,
                            retry_count ? *retry_count : 0u,
                            ctx.request_may_have_been_sent);
        goto out_global;
    }
    rc = parse_count_body(&ctx, input_tokens, error, error_size);

out_global:
    if (curl)
        curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    curl_global_cleanup();
out:
    snj_buf_free(&body);
    snj_buf_free(&ctx.error_body);
    return rc;
}

int
snj_provider_responses_compact(const json_t *compact_request,
                               const struct snj_config *config,
                               const struct snj_provider_config *provider,
                               const struct snj_credential *credential,
                               struct snj_render *render,
                               snj_provider_pump_fn pump,
                               void *pump_opaque,
                               json_t **output,
                               uint64_t *output_tokens_bound,
                               char *error, size_t error_size,
                               int *cancel_code,
                               unsigned int *retry_count)
{
    struct provider_ctx ctx;
    struct snj_buf body;
    struct curl_slist *headers = NULL;
    CURL *curl = NULL;
    CURLcode code;
    char url_buffer[SNJ_CONFIG_URL_MAX + 64u];
    const char *url = NULL;
    int rc = -1;

    if (cancel_code)
        *cancel_code = 0;
    if (retry_count)
        *retry_count = 0u;
    if (output)
        *output = NULL;
    if (output_tokens_bound)
        *output_tokens_bound = 0u;
    if (!compact_request || !config || !provider || !credential ||
        !credential->len ||
        !output || !output_tokens_bound) {
        set_error(error, error_size, "invalid compact request");
        errno = EINVAL;
        return -1;
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.config = config;
    ctx.provider = provider;
    ctx.render = render;
    ctx.pump = pump;
    ctx.pump_opaque = pump_opaque;
    snj_secret_set_build(&ctx.secrets, config, credential);
    snj_buf_init(&ctx.error_body, SNJ_CONTEXT_MAX_COMPACT);
    snj_buf_init(&body, SNJ_CONTEXT_MAX_COMPACT);

    if (snj_json_canonical(compact_request, &body) < 0) {
        set_error(error, error_size,
                  "compact request exceeds the bounded body limit");
        goto out;
    }
    if (provider_endpoint_url(provider, "/v1/responses/compact",
                              url_buffer, sizeof(url_buffer), &url,
                              error, error_size) < 0)
        goto out;
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
        set_error(error, error_size, "libcurl could not initialize");
        errno = EIO;
        goto out;
    }
    curl = curl_easy_init();
    if (!curl) {
        set_error(error, error_size, "libcurl easy handle could not initialize");
        errno = ENOMEM;
        goto out_global;
    }
    if (append_header(&headers, "Accept: application/json") < 0 ||
        append_header(&headers, "Content-Type: application/json") < 0 ||
        append_provider_headers(&headers, provider, credential) < 0) {
        set_error(error, error_size, "provider headers could not be allocated");
        goto out_global;
    }
    if (render_request_headers(&ctx,
                               "POST /v1/responses/compact HTTP/1.1",
                               "accept: application/json") < 0) {
        set_error(error, error_size, ctx.error[0] ? ctx.error :
                  "compact request headers could not be rendered");
        goto out_global;
    }
    if (curl_easy_setopt(curl, CURLOPT_URL, url) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_POST, 1L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, (char *)body.data) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                         (curl_off_t)body.len) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, count_write_cb) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &ctx) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_cb) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS,
                         (long)provider->connect_timeout_ms) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS,
                         (long)provider->request_timeout_ms) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME,
                         (long)low_speed_seconds(provider->idle_timeout_ms)) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_USERAGENT,
                         SNAJPAGENT_NAME "/" SNAJPAGENT_VERSION) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L) != CURLE_OK) {
        set_error(error, error_size, "libcurl option setup failed");
        errno = EIO;
        goto out_global;
    }
    code = perform_with_retry(curl, &ctx, error, error_size, cancel_code,
                              retry_count);
    if (code == CURLE_ABORTED_BY_CALLBACK &&
        (ctx.cancel_code == 1 || ctx.cancel_code == 2)) {
        if (cancel_code)
            *cancel_code = ctx.cancel_code;
        rc = ctx.cancel_code;
        goto out_global;
    }
    if (code != CURLE_OK) {
        (void)snprintf(error, error_size, "%s%s%s",
                       ctx.error[0] ? ctx.error : "compact request failed",
                       ctx.error[0] ? "" : ": ",
                       ctx.error[0] ? "" : curl_easy_strerror(code));
        append_retry_suffix(error, error_size,
                            retry_count ? *retry_count : 0u,
                            ctx.request_may_have_been_sent);
        errno = EIO;
        goto out_global;
    }
    if (ctx.http_status < 200 || ctx.http_status >= 300) {
        (void)classify_non2xx(&ctx, error, error_size);
        append_retry_suffix(error, error_size,
                            retry_count ? *retry_count : 0u,
                            ctx.request_may_have_been_sent);
        goto out_global;
    }
    rc = parse_compact_body(&ctx, output, output_tokens_bound,
                            error, error_size);

out_global:
    if (curl)
        curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    curl_global_cleanup();
out:
    snj_buf_free(&body);
    snj_buf_free(&ctx.error_body);
    return rc;
}

int
snj_provider_responses_create(const json_t *create_request,
                              const struct snj_config *config,
                              const struct snj_provider_config *provider,
                              const struct snj_credential *credential,
                              struct snj_render *render,
                              snj_responses_emit_fn emit,
                              void *emit_opaque,
                              snj_provider_pump_fn pump,
                              void *pump_opaque,
                              struct snj_response_graph *graph,
                              struct snj_provider_failure *failure,
                              char *error, size_t error_size,
                              int *cancel_code,
                              unsigned int *retry_count)
{
    struct provider_ctx ctx;
    struct snj_buf body;
    struct curl_slist *headers = NULL;
    CURL *curl = NULL;
    CURLcode code;
    char url_buffer[SNJ_CONFIG_URL_MAX + 64u];
    const char *url = NULL;
    int rc = -1;

    if (cancel_code)
        *cancel_code = 0;
    if (failure)
        memset(failure, 0, sizeof(*failure));
    if (retry_count)
        *retry_count = 0u;
    if (!create_request || !config || !provider || !credential ||
        !credential->len || !graph) {
        set_error(error, error_size, "invalid provider request");
        errno = EINVAL;
        return -1;
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.config = config;
    ctx.provider = provider;
    ctx.render = render;
    ctx.pump = pump;
    ctx.pump_opaque = pump_opaque;
    snj_secret_set_build(&ctx.secrets, config, credential);
    snj_responses_stream_init(&ctx.stream, emit, emit_opaque);
    snj_sse_init(&ctx.sse, snj_responses_sse_record, &ctx.stream);
    snj_buf_init(&ctx.error_body, SNJ_WIRE_BODY_MAX);
    snj_buf_init(&body, SNJ_CONTEXT_MAX_REQUEST);

    if (snj_json_canonical(create_request, &body) < 0) {
        set_error(error, error_size, "provider request exceeds the bounded body limit");
        goto out;
    }
    if (provider_endpoint_url(provider, "/v1/responses",
                              url_buffer, sizeof(url_buffer), &url,
                              error, error_size) < 0)
        goto out;
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
        set_error(error, error_size, "libcurl could not initialize");
        errno = EIO;
        goto out;
    }
    curl = curl_easy_init();
    if (!curl) {
        set_error(error, error_size, "libcurl easy handle could not initialize");
        errno = ENOMEM;
        goto out_global;
    }
    if (append_header(&headers, "Accept: text/event-stream") < 0 ||
        append_header(&headers, "Content-Type: application/json") < 0 ||
        append_provider_headers(&headers, provider, credential) < 0) {
        set_error(error, error_size, "provider headers could not be allocated");
        goto out_global;
    }
    if (render_request_headers(&ctx, "POST /v1/responses HTTP/1.1",
                               "accept: text/event-stream") < 0) {
        set_error(error, error_size, ctx.error[0] ? ctx.error :
                  "request headers could not be rendered");
        goto out_global;
    }
    if (curl_easy_setopt(curl, CURLOPT_URL, url) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_POST, 1L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, (char *)body.data) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                         (curl_off_t)body.len) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &ctx) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_cb) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS,
                         (long)provider->connect_timeout_ms) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS,
                         (long)provider->request_timeout_ms) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME,
                         (long)low_speed_seconds(provider->idle_timeout_ms)) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_USERAGENT,
                         SNAJPAGENT_NAME "/" SNAJPAGENT_VERSION) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L) != CURLE_OK) {
        set_error(error, error_size, "libcurl option setup failed");
        errno = EIO;
        goto out_global;
    }
    code = perform_with_retry(curl, &ctx, error, error_size, cancel_code,
                              retry_count);
    if (code == CURLE_ABORTED_BY_CALLBACK &&
        (ctx.cancel_code == 1 || ctx.cancel_code == 2)) {
        if (cancel_code)
            *cancel_code = ctx.cancel_code;
        rc = ctx.cancel_code;
        goto out_global;
    }
    if (code != CURLE_OK) {
        (void)snprintf(error, error_size, "%s%s%s",
                       ctx.error[0] ? ctx.error : "provider transport failed",
                       ctx.error[0] ? "" : ": ",
                       ctx.error[0] ? "" : curl_easy_strerror(code));
        append_retry_suffix(error, error_size,
                            retry_count ? *retry_count : 0u,
                            ctx.request_may_have_been_sent);
        errno = EIO;
        goto out_global;
    }
    if (ctx.http_status < 200 || ctx.http_status >= 300) {
        (void)classify_non2xx(&ctx, error, error_size);
        append_retry_suffix(error, error_size,
                            retry_count ? *retry_count : 0u,
                            ctx.request_may_have_been_sent);
        goto out_global;
    }
    if (snj_sse_finish(&ctx.sse, error, error_size) < 0) {
        set_error(error, error_size,
                  stream_or_sse_error(&ctx, error,
                                      "invalid provider SSE stream"));
        goto out_global;
    }
    rc = snj_responses_stream_finish(&ctx.stream, graph, error, error_size);
    if (rc != 0) {
        if (rc > 0)
            rc = 3;
        goto out_global;
    }

out_global:
    if (curl)
        curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    curl_global_cleanup();
out:
    if (failure) {
        if (ctx.stream.provider_failure.code[0])
            *failure = ctx.stream.provider_failure;
        else
            *failure = ctx.provider_failure;
        failure->output_correction = ctx.stream.output_correction;
    }
    snj_buf_free(&body);
    snj_buf_free(&ctx.error_body);
    snj_sse_free(&ctx.sse);
    snj_responses_stream_free(&ctx.stream);
    return rc;
}

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
    if (append_host_header(&host, ctx->config->provider_base_url) < 0) {
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
provider_endpoint_url(const struct snj_config *config, const char *path,
                      char *buffer, size_t buffer_size, const char **url,
                      char *error, size_t error_size)
{
    int written;

    if (!config || !path || !url || !buffer || !buffer_size) {
        set_error(error, error_size, "invalid provider endpoint");
        errno = EINVAL;
        return -1;
    }
    written = snprintf(buffer, buffer_size, "%s%s",
                       config->provider_base_url, path);
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
        written = snprintf(buffer, buffer_size, "%s%s", base, path);
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

int
snj_provider_responses_count(const json_t *count_request,
                             const struct snj_config *config,
                             const struct snj_credential *credential,
                             struct snj_render *render,
                             snj_provider_pump_fn pump,
                             void *pump_opaque,
                             uint64_t *input_tokens,
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
    if (!count_request || !config || !credential || !credential->len ||
        !input_tokens) {
        set_error(error, error_size, "invalid input-token count request");
        errno = EINVAL;
        return -1;
    }
    *input_tokens = 0u;
    memset(&ctx, 0, sizeof(ctx));
    ctx.config = config;
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
    if (provider_endpoint_url(config, "/v1/responses/input_tokens",
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
        append_authorization(&headers, credential) < 0) {
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
                         (long)config->connect_timeout_ms) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS,
                         (long)config->request_timeout_ms) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME,
                         (long)low_speed_seconds(config->idle_timeout_ms)) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_USERAGENT,
                         "snajpagent/" SNAJPAGENT_VERSION) != CURLE_OK ||
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
    if (!compact_request || !config || !credential || !credential->len ||
        !output || !output_tokens_bound) {
        set_error(error, error_size, "invalid compact request");
        errno = EINVAL;
        return -1;
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.config = config;
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
    if (provider_endpoint_url(config, "/v1/responses/compact",
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
        append_authorization(&headers, credential) < 0) {
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
                         (long)config->connect_timeout_ms) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS,
                         (long)config->request_timeout_ms) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME,
                         (long)low_speed_seconds(config->idle_timeout_ms)) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_USERAGENT,
                         "snajpagent/" SNAJPAGENT_VERSION) != CURLE_OK ||
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
                              const struct snj_credential *credential,
                              struct snj_render *render,
                              snj_responses_emit_fn emit,
                              void *emit_opaque,
                              snj_provider_pump_fn pump,
                              void *pump_opaque,
                              struct snj_response_graph *graph,
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
    if (!create_request || !config || !credential || !credential->len || !graph) {
        set_error(error, error_size, "invalid provider request");
        errno = EINVAL;
        return -1;
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.config = config;
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
    if (provider_endpoint_url(config, "/v1/responses",
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
        append_authorization(&headers, credential) < 0) {
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
                         (long)config->connect_timeout_ms) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS,
                         (long)config->request_timeout_ms) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME,
                         (long)low_speed_seconds(config->idle_timeout_ms)) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_USERAGENT,
                         "snajpagent/" SNAJPAGENT_VERSION) != CURLE_OK ||
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
    if (snj_responses_stream_finish(&ctx.stream, graph, error, error_size) < 0)
        goto out_global;
    rc = 0;

out_global:
    if (curl)
        curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    curl_global_cleanup();
out:
    snj_buf_free(&body);
    snj_buf_free(&ctx.error_body);
    snj_sse_free(&ctx.sse);
    snj_responses_stream_free(&ctx.stream);
    return rc;
}

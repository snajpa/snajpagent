/* SPDX-License-Identifier: GPL-2.0-only */
#include "provider.h"

#include "base.h"
#include "auth.h"
#include "provider_retry.h"
#include "context.h"
#include "json.h"
#include "responses.h"
#include "secret.h"
#include "sse.h"
#include "snajpagent.h"
#include "wire.h"
#include "ui.h"

#include <curl/curl.h>
#include <errno.h>
#include <poll.h>
#include "snag_jansson.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

struct provider_ctx {
    struct snag_sse_parser sse;
    struct snag_responses_stream stream;
    struct snag_buf body;
    struct snag_buf error_body;
    struct snag_secret_set secrets;
    struct snag_credential credential;
    struct curl_slist *headers;
    CURL *curl;
    const struct snag_config *config;
    const struct snag_provider_config *provider;
    struct snag_ui *render;
    snag_provider_pump_fn pump;
    void *pump_opaque;
    const char *accept;
    bool has_body;
    long http_status;
    int cancel_code;
    uint32_t retry_after_ms;
    bool retry_after_present;
    bool body_failed;
    bool curl_global;
    bool semantic_body_seen;
    bool request_may_have_been_sent;
    char error[256];
    struct snag_provider_failure provider_failure;
};

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
        return snag_responses_stream_error(&ctx->stream);
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
append_host_header(struct snag_buf *out, const char *base_url)
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
    return snag_buf_append(out, "host: ", 6u) == 0 &&
           snag_buf_append(out, host, (size_t)(end - host)) == 0 &&
           snag_buf_terminate(out) == 0 ? 0 : -1;
}

static int
render_config_header(struct provider_ctx *ctx, struct snag_buf *redacted,
                     const char *name, const char *value)
{
    struct snag_buf line;
    int rc = 0;

    if (!value[0])
        return 0;
    snag_buf_init(&line, SNAG_WIRE_HEADER_MAX);
    if (snag_buf_printf(&line, "%s: %s", name, value) < 0 ||
        snag_wire_header_redact(line.data, line.len, &ctx->secrets.wire,
                               redacted) < 0 ||
        snag_ui_transport(ctx->render, '>', (const char *)redacted->data,
                             redacted->len) < 0)
        rc = -1;
    snag_buf_free(&line);
    return rc;
}

static int
render_request_headers(struct provider_ctx *ctx, const char *request_line,
                       const char *accept, bool has_body)
{
    struct snag_buf redacted;
    struct snag_buf host;
    struct snag_buf accept_line;
    int rc = 0;

    if (!snag_ui_enabled(ctx->render, SNAG_PRESENT_WIRE))
        return 0;
    snag_buf_init(&redacted, SNAG_WIRE_HEADER_MAX);
    snag_buf_init(&host, SNAG_CONFIG_URL_MAX + 8u);
    snag_buf_init(&accept_line, SNAG_WIRE_HEADER_MAX);
    if (append_host_header(&host, ctx->provider->base_url) < 0 ||
        snag_buf_printf(&accept_line, "accept: %s", accept) < 0)
        goto fail;
    if (snag_ui_transport(ctx->render, '>',
                             request_line, strlen(request_line)) < 0 ||
        snag_ui_transport(ctx->render, '>',
                             (const char *)host.data, host.len) < 0 ||
        snag_ui_transport(ctx->render, '>',
                             (const char *)accept_line.data,
                             accept_line.len) < 0 ||
        (has_body && snag_ui_transport(ctx->render, '>',
             "content-type: application/json",
             strlen("content-type: application/json")) < 0) ||
        snag_wire_header_redact((const unsigned char *)"authorization: Bearer x",
                               23u, &ctx->secrets.wire, &redacted) < 0 ||
        snag_ui_transport(ctx->render, '>', (const char *)redacted.data,
                             redacted.len) < 0)
        rc = -1;
    snag_buf_reset(&redacted);
    if (rc == 0 &&
        render_config_header(ctx, &redacted, "HTTP-Referer",
                             ctx->provider->openrouter_referer) < 0)
        rc = -1;
    if (rc == 0 &&
        render_config_header(ctx, &redacted, "X-OpenRouter-Title",
                             ctx->provider->openrouter_title) < 0)
        rc = -1;
    goto out;
fail:
    rc = -1;
out:
    snag_buf_free(&accept_line);
    snag_buf_free(&redacted);
    snag_buf_free(&host);
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
    if (snag_buf_append(&ctx->error_body, ptr, len) < 0) {
        ctx->body_failed = true;
        ctx_error(ctx, ctx->http_status >= 200 && ctx->http_status < 300 ?
                  "provider JSON response body exceeds diagnostic bound" :
                  "provider error body exceeds diagnostic bound");
        return 0;
    }
    return len;
}

static size_t
header_cb(char *buffer, size_t size, size_t nmemb, void *opaque)
{
    struct provider_ctx *ctx = opaque;
    const unsigned char *line;
    size_t len = size * nmemb;
    size_t clean_len;
    struct snag_buf redacted;
    bool status_line;

    if (size && nmemb > SIZE_MAX / size)
        return 0;
    strip_crlf(buffer, len, &line, &clean_len);
    if (clean_len == 0u)
        return len;
    status_line = clean_len >= 5u && memcmp(line, "HTTP/", 5u) == 0;
    if (status_line && curl_easy_getinfo(ctx->curl, CURLINFO_RESPONSE_CODE,
                                        &ctx->http_status) != CURLE_OK)
        return 0;
    if (clean_len > 12u && strncasecmp((const char *)line,
                                       "retry-after:", 12u) == 0) {
        uint32_t delay_ms;
        if (snag_provider_retry_after_parse(line + 12u, clean_len - 12u,
                                           &delay_ms) == 0) {
            ctx->retry_after_present = true;
            ctx->retry_after_ms = delay_ms;
        }
    }
    if (snag_ui_enabled(ctx->render, SNAG_PRESENT_WIRE)) {
        if (status_line) {
            if (!ascii_printable(line, clean_len) ||
                snag_ui_transport(ctx->render, '<', (const char *)line,
                                     clean_len) < 0) {
                ctx_error(ctx, "HTTP status diagnostics could not be rendered");
                return 0;
            }
        } else {
            snag_buf_init(&redacted, SNAG_WIRE_HEADER_MAX);
            if (snag_wire_header_redact(line, clean_len, &ctx->secrets.wire,
                                       &redacted) < 0 ||
                snag_ui_transport(ctx->render, '<', (const char *)redacted.data,
                                     redacted.len) < 0) {
                snag_buf_free(&redacted);
                ctx_error(ctx, "HTTP header diagnostics could not be rendered");
                return 0;
            }
            snag_buf_free(&redacted);
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
        if (snag_sse_feed(&ctx->sse, ptr, len, error, sizeof(error)) < 0) {
            (void)snprintf(ctx->error, sizeof(ctx->error), "%s",
                           stream_or_sse_error(ctx, error,
                                               "invalid provider SSE stream"));
            return 0;
        }
    } else {
        if (snag_buf_append(&ctx->error_body, ptr, len) < 0) {
            ctx->body_failed = true;
            ctx_error(ctx, "provider error body exceeds diagnostic bound");
            return 0;
        }
    }
    return len;
}

static int
process_controls(struct provider_ctx *ctx)
{
    int rc;

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

static CURLcode
perform_request(CURL *curl, struct provider_ctx *ctx)
{
    CURLM *multi = curl_multi_init();
    CURLcode result = CURLE_FAILED_INIT;
    int running = 0;

    if (!multi)
        return CURLE_OUT_OF_MEMORY;
    if (curl_multi_add_handle(multi, curl) != CURLM_OK)
        goto out;
    for (;;) {
        struct curl_waitfd wake = {
            .fd = snag_ui_wake_fd(ctx->render), .events = CURL_WAIT_POLLIN
        };
        CURLMsg *message;
        int remaining;

        if (process_controls(ctx)) {
            result = CURLE_ABORTED_BY_CALLBACK;
            break;
        }
        if (curl_multi_perform(multi, &running) != CURLM_OK)
            break;
        if (!running) {
            while ((message = curl_multi_info_read(multi, &remaining)))
                if (message->msg == CURLMSG_DONE && message->easy_handle == curl)
                    result = message->data.result;
            break;
        }
        if (curl_multi_poll(multi, wake.fd < 0 ? NULL : &wake,
                            wake.fd < 0 ? 0u : 1u, 25, NULL) != CURLM_OK)
            break;
    }
    (void)curl_multi_remove_handle(multi, curl);
out:
    (void)curl_multi_cleanup(multi);
    return result;
}

static int
provider_endpoint_url(const struct snag_provider_config *provider,
                      const char *path,
                      char *buffer, size_t buffer_size, const char **url,
                      char *error, size_t error_size)
{
    const char *base;
    const char *append_path;
    size_t base_len;
    int written;

    if (!provider || !path || !url || !buffer || !buffer_size) {
        snag_errorf(error, error_size, "invalid provider endpoint");
        errno = EINVAL;
        return -1;
    }
    base = provider->base_url;
#if defined(SNAJPAGENT_TEST_TRANSPORT_ENDPOINTS) || defined(SNAJPAGENT_TEST_FIXTURE)
    {
        const char *override = getenv("SNAJPAGENT_TEST_OPENAI_BASE");
        if (override && *override)
            base = override;
    }
#endif
    append_path = path;
    if (provider->auth == SNAG_AUTH_CHATGPT && strncmp(path, "/v1/", 4u) == 0)
        append_path = path + 3u;
    base_len = strlen(base);
    while (base_len && base[base_len - 1u] == '/')
        --base_len;
    if (base_len >= 3u &&
        memcmp(base + base_len - 3u, "/v1", 3u) == 0 &&
        strncmp(path, "/v1/", 4u) == 0)
        append_path = path + 3u;
    written = snprintf(buffer, buffer_size, "%.*s%s", (int)base_len,
                       base, append_path);
    if (written <= 0 || (size_t)written >= buffer_size) {
        snag_errorf(error, error_size, "provider endpoint is too long");
        errno = ENAMETOOLONG;
        return -1;
    }
    *url = buffer;
    return 0;
}

static size_t
receive_body(char *data, size_t size, size_t count, void *opaque)
{
    struct snag_buf *buf = opaque;
    if (size && count > SIZE_MAX / size)
        return 0;
    size *= count;
    return snag_buf_append(buf, data, size) == 0 ? size : 0;
}

int
snag_provider_auth_post(const char *issuer, const char *path, const char *type, const void *body, size_t size,
           json_t **response, long *status, snag_provider_pump_fn pump, void *opaque,
           char *error, size_t error_size)
{
    char url[4096], header[96], parse_error[128];
    struct snag_buf output;
    struct curl_slist *headers = NULL;
    CURL *curl = NULL;
    CURLM *multi = NULL;
    CURLMsg *message;
    int running = 0, pending, rc = -1;
    bool attached = false, initialized = false;

    *response = NULL;
    *status = 0;
    snag_buf_init(&output, (96u * 1024u));
    if (snprintf(url, sizeof(url), "%s%s", issuer, path) >= (int)sizeof(url) ||
        curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
        goto out;
    initialized = true;
    curl = curl_easy_init();
    multi = curl_multi_init();
    (void)snprintf(header, sizeof(header), "Content-Type: %s", type);
    headers = curl_slist_append(NULL, header);
    if (!curl || !multi || !headers ||
        curl_easy_setopt(curl, CURLOPT_URL, url) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_POST, 1L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)size) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive_body) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &output) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 30000L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_USERAGENT, SNAJPAGENT_NAME "/" SNAJPAGENT_VERSION) != CURLE_OK ||
        curl_multi_add_handle(multi, curl) != CURLM_OK)
        goto out;
    attached = true;
    do {
        if (pump && pump(opaque, 0u) != 0) {
            errno = ECANCELED;
            snag_errorf(error, error_size, "login or token refresh cancelled");
            goto out;
        }
        if (curl_multi_perform(multi, &running) != CURLM_OK)
            goto out;
        if (running && curl_multi_poll(multi, NULL, 0, 100, NULL) != CURLM_OK)
            goto out;
    } while (running);
    message = curl_multi_info_read(multi, &pending);
    if (!message || message->msg != CURLMSG_DONE || message->data.result != CURLE_OK ||
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, status) != CURLE_OK)
        goto out;
    if (*status >= 200 && *status < 300) {
        *response = snag_json_load_strict(output.data, output.len, (96u * 1024u),
                                        parse_error, sizeof(parse_error));
        if (!json_is_object(*response)) {
            snag_errorf(error, error_size, "authentication server returned an invalid response");
            goto out;
        }
    }
    rc = 0;
out:
    if (attached)
        (void)curl_multi_remove_handle(multi, curl);
    if (multi)
        (void)curl_multi_cleanup(multi);
    if (curl)
        curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    if (initialized)
        curl_global_cleanup();
    if (output.data)
        memset(output.data, 0, output.len);
    snag_buf_free(&output);
    if (rc < 0) {
        snag_auth_json_free(*response);
        *response = NULL;
        if (!error[0])
            snag_errorf(error, error_size, "authentication request failed (response details withheld)");
    }
    return rc;
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
                     const struct snag_credential *credential)
{
    struct snag_buf line;
    int rc;

    snag_buf_init(&line, SNAG_CREDENTIAL_MAX + 32u);
    rc = snag_buf_append(&line, "Authorization: Bearer ", 22u);
    if (rc == 0)
        rc = snag_buf_append(&line, credential->value, credential->len);
    if (rc == 0)
        rc = snag_buf_terminate(&line);
    if (rc == 0)
        rc = append_header(headers, (const char *)line.data);
    snag_buf_free(&line);
    return rc;
}

static int
append_named_header(struct curl_slist **headers, const char *name,
                    const char *value)
{
    struct snag_buf line;
    int rc;

    if (!value[0])
        return 0;
    snag_buf_init(&line, SNAG_WIRE_HEADER_MAX);
    rc = snag_buf_printf(&line, "%s: %s", name, value);
    if (rc == 0)
        rc = append_header(headers, (const char *)line.data);
    snag_buf_free(&line);
    return rc;
}

static int
append_provider_headers(struct curl_slist **headers,
                        const struct snag_provider_config *provider,
                        const struct snag_credential *credential)
{
    return append_authorization(headers, credential) == 0 &&
           append_named_header(headers, "ChatGPT-Account-Id",
                               credential->account_id) == 0 &&
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
    snag_buf_reset(&ctx->error_body);
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
retry_wait(struct provider_ctx *ctx, unsigned int retries_done,
           const char *reason, char *error, size_t error_size)
{
    uint32_t delay_ms = snag_provider_retry_delay_ms(retries_done,
        ctx->retry_after_present, ctx->retry_after_ms);
    uint64_t deadline = snag_monotonic_ms() + delay_ms;

    if (ctx->render) {
        char line[160];
        (void)snprintf(line, sizeof(line),
                       "provider retry %u/%u after %s in %llums",
                       retries_done + 1u, SNAG_PROVIDER_MAX_RETRIES,
                       reason, (unsigned long long)delay_ms);
        if (snag_ui_text(ctx->render, SNAG_UI_WARNING, line) < 0) {
            snag_errorf(error, error_size, "provider retry diagnostics could not be rendered");
            errno = EIO;
            return -1;
        }
    }
    for (;;) {
        uint64_t now = snag_monotonic_ms();
        uint64_t remaining = now < deadline ? deadline - now : 0u;
        uint32_t slice = remaining > 25u ? 25u : (uint32_t)remaining;
        struct pollfd wake = {snag_ui_wake_fd(ctx->render), POLLIN, 0};
        if (!remaining)
            break;
        if (ctx->pump) {
            int rc = ctx->pump(ctx->pump_opaque, 0u);
            if (rc < 0) {
                snag_errorf(error, error_size,
                          "active input could not be processed during provider retry");
                errno = EIO;
                return -1;
            }
            if (rc == 1 || rc == 2) {
                ctx->cancel_code = rc;
                return rc;
            }
        }
        if (poll(&wake, 1u, (int)slice) < 0 && errno != EINTR) {
            snag_errorf(error, error_size, "provider retry wait failed");
            return -1;
        }
    }
    return 0;
}

static bool
retryable_attempt(struct provider_ctx *ctx, CURLcode code)
{
    if (ctx->body_failed)
        return false;
    if (code == CURLE_OK)
        return snag_provider_http_status_retryable(ctx->http_status);
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
        code = perform_request(curl, ctx);
        if (curl_easy_getinfo(curl, CURLINFO_REQUEST_SIZE,
                              &request_size) == CURLE_OK && request_size > 0)
            ctx->request_may_have_been_sent = true;
        if (code == CURLE_ABORTED_BY_CALLBACK &&
            (ctx->cancel_code == 1 || ctx->cancel_code == 2)) {
            if (cancel_code)
                *cancel_code = ctx->cancel_code;
            break;
        }
        if (retries >= SNAG_PROVIDER_MAX_RETRIES ||
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
    struct snag_buf redacted;
    char json_error[128] = {0};
    int rc;

    if (ctx->body_failed) {
        snag_errorf(error, error_size, ctx->error[0] ? ctx->error :
                  "provider error body could not be retained");
        errno = EOVERFLOW;
        return -1;
    }
    snag_buf_init(&redacted, SNAG_WIRE_BODY_MAX);
    if (ctx->error_body.len) {
        json_t *root = snag_json_load_strict(ctx->error_body.data,
                                            ctx->error_body.len,
                                            SNAG_WIRE_BODY_MAX,
                                            json_error,
                                            sizeof(json_error));
        if (root) {
            if (snag_provider_failure_from_json(root,
                                               &ctx->provider_failure) < 0)
                memset(&ctx->provider_failure, 0,
                       sizeof(ctx->provider_failure));
            json_decref(root);
        }
        json_error[0] = '\0';
    }
    rc = snag_wire_json_redact(ctx->error_body.data, ctx->error_body.len,
                              &ctx->secrets.wire, &redacted,
                              json_error, sizeof(json_error));
    if (rc == 0 && snag_ui_enabled(ctx->render, SNAG_PRESENT_PROTOCOL))
        (void)snag_ui_protocol(ctx->render, "response.error.body",
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
    snag_buf_free(&redacted);
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
        snag_errorf(error, error_size, ctx->error[0] ? ctx->error :
                  "input-token count body could not be retained");
        errno = EOVERFLOW;
        return -1;
    }
    root = snag_json_load_strict(ctx->error_body.data, ctx->error_body.len,
                                SNAG_WIRE_BODY_MAX, json_error,
                                sizeof(json_error));
    if (!root) {
        (void)snprintf(error, error_size,
                       "invalid input-token count response: %s", json_error);
        errno = EPROTO;
        return -1;
    }
    object = snag_json_string(root, "object");
    if (!snag_json_exact_keys(root, keys, sizeof(keys) / sizeof(keys[0])) ||
        !object || strcmp(object, "response.input_tokens") != 0 ||
        snag_json_integer_u64(root, "input_tokens", input_tokens) < 0) {
        snag_errorf(error, error_size,
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
    char output_hash[SNAG_SHA256_HEX_LEN + 1u];
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
        snag_errorf(error, error_size, ctx->error[0] ? ctx->error :
                  "compact response body could not be retained");
        errno = EOVERFLOW;
        return -1;
    }
    root = snag_json_load_strict(ctx->error_body.data, ctx->error_body.len,
                                SNAG_CONTEXT_MAX_COMPACT, json_error,
                                sizeof(json_error));
    if (!root) {
        (void)snprintf(error, error_size,
                       "invalid compact response: %s", json_error);
        errno = EPROTO;
        return -1;
    }
    object = snag_json_string(root, "object");
    body_output = json_object_get(root, "output");
    if (!object || strcmp(object, "response.compaction") != 0 ||
        snag_context_compact_output_valid(body_output, output_hash,
                                         &output_bytes,
                                         error, error_size) < 0) {
        if (error && !error[0])
            snag_errorf(error, error_size,
                      "compact response has an invalid shape");
        errno = EPROTO;
        goto out;
    }
    if (!output || !output_tokens_bound ||
        output_bytes > (size_t)UINT64_MAX) {
        snag_errorf(error, error_size, "invalid compact response destination");
        errno = EINVAL;
        goto out;
    }
    *output = json_deep_copy(body_output);
    if (!*output) {
        snag_errorf(error, error_size, "compact output could not be retained");
        errno = ENOMEM;
        goto out;
    }
    *output_tokens_bound = (uint64_t)output_bytes;
    rc = 0;
out:
    json_decref(root);
    return rc;
}

#define SNAG_PROVIDER_MODELS_MAX 4096u
#define SNAG_PROVIDER_EFFORTS_MAX 32u
#define SNAG_CODEX_CATALOG_CLIENT_VERSION "0.146.0"
#define SNAG_CODEX_CATALOG_PATH \
    "/models?client_version=" SNAG_CODEX_CATALOG_CLIENT_VERSION

struct codex_model_ref {
    const json_t *model;
    json_int_t priority;
    size_t order;
};

struct optional_limit {
    uint64_t value;
    bool known;
};

enum limit_field {
    LIMIT_CONTEXT, LIMIT_MAX_CONTEXT, LIMIT_INPUT_CONTEXT, LIMIT_MAX_INPUT,
    LIMIT_MAX_OUTPUT, LIMIT_AUTO_COMPACT, LIMIT_EFFECTIVE, LIMIT_COUNT
};

struct limit_key {
    enum limit_field field;
    const char *key;
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
collect_limits(const json_t *const *objects, size_t object_count,
               const struct limit_key *keys, size_t key_count,
               struct optional_limit limits[LIMIT_COUNT])
{
    for (size_t i = 0; i < object_count; ++i)
        for (size_t j = 0; j < key_count; ++j)
            if (merge_limit(objects[i], keys[j].key,
                    keys[j].field == LIMIT_EFFECTIVE ? 100u :
                    SNAG_CONFIG_TOKEN_LIMIT_MAX, &limits[keys[j].field]) < 0)
                return -1;
    return 0;
}

static int
set_optional_limit(json_t *limits, const char *key,
                   const struct optional_limit *value)
{
    return snag_json_set_new(limits, key,
        value->known ? json_integer((json_int_t)value->value) : json_null());
}

static int
build_model_limits(const json_t *source, bool codex, json_t **out)
{
    static const struct limit_key generic[] = {
        {LIMIT_CONTEXT, "context_window_tokens"},
        {LIMIT_CONTEXT, "contextWindowTokens"}, {LIMIT_CONTEXT, "context_window"},
        {LIMIT_CONTEXT, "contextWindow"}, {LIMIT_CONTEXT, "context_length"},
        {LIMIT_CONTEXT, "contextLength"},
        {LIMIT_MAX_CONTEXT, "max_context_window_tokens"},
        {LIMIT_MAX_CONTEXT, "maxContextWindowTokens"},
        {LIMIT_MAX_CONTEXT, "max_context_window"},
        {LIMIT_MAX_CONTEXT, "maxContextWindow"},
        {LIMIT_INPUT_CONTEXT, "input_context_window_tokens"},
        {LIMIT_INPUT_CONTEXT, "inputContextWindowTokens"},
        {LIMIT_INPUT_CONTEXT, "input_context_window"},
        {LIMIT_INPUT_CONTEXT, "inputContextWindow"},
        {LIMIT_MAX_INPUT, "max_input_tokens"}, {LIMIT_MAX_INPUT, "maxInputTokens"},
        {LIMIT_MAX_OUTPUT, "max_output_tokens"},
        {LIMIT_MAX_OUTPUT, "maxOutputTokens"},
        {LIMIT_AUTO_COMPACT, "auto_compact_input_tokens"},
        {LIMIT_AUTO_COMPACT, "autoCompactInputTokens"},
        {LIMIT_AUTO_COMPACT, "auto_compact_token_limit"},
        {LIMIT_AUTO_COMPACT, "autoCompactTokenLimit"},
        {LIMIT_EFFECTIVE, "effective_context_window_percent"},
        {LIMIT_EFFECTIVE, "effectiveContextWindowPercent"}
    };
    static const struct limit_key native[] = {
        {LIMIT_CONTEXT, "context_window"},
        {LIMIT_MAX_CONTEXT, "max_context_window"},
        {LIMIT_INPUT_CONTEXT, "input_context_window"},
        {LIMIT_MAX_INPUT, "max_input_tokens"},
        {LIMIT_MAX_OUTPUT, "max_output_tokens"},
        {LIMIT_AUTO_COMPACT, "auto_compact_token_limit"},
        {LIMIT_EFFECTIVE, "effective_context_window_percent"}
    };
    static const char *const output_keys[LIMIT_COUNT] = {
        "context_window_tokens", "max_context_window_tokens",
        "input_context_window_tokens", "max_input_tokens", "max_output_tokens",
        "auto_compact_input_tokens", "effective_context_window_percent"
    };
    const json_t *objects[3] = {source, NULL, NULL};
    size_t object_count = 1u;
    struct optional_limit limit[LIMIT_COUNT] = {{0}};
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
    if (collect_limits(objects, object_count, codex ? native : generic,
            codex ? sizeof(native) / sizeof(native[0]) :
                    sizeof(generic) / sizeof(generic[0]), limit) < 0)
        goto invalid;
    if ((limit[LIMIT_CONTEXT].known && limit[LIMIT_MAX_CONTEXT].known &&
         limit[LIMIT_CONTEXT].value > limit[LIMIT_MAX_CONTEXT].value) ||
        (limit[LIMIT_CONTEXT].known && limit[LIMIT_INPUT_CONTEXT].known &&
         limit[LIMIT_INPUT_CONTEXT].value > limit[LIMIT_CONTEXT].value) ||
        (limit[LIMIT_CONTEXT].known && limit[LIMIT_MAX_INPUT].known &&
         limit[LIMIT_MAX_INPUT].value > limit[LIMIT_CONTEXT].value) ||
        (limit[LIMIT_CONTEXT].known && limit[LIMIT_MAX_OUTPUT].known &&
         limit[LIMIT_MAX_OUTPUT].value > limit[LIMIT_CONTEXT].value) ||
        (limit[LIMIT_CONTEXT].known && limit[LIMIT_MAX_INPUT].known &&
         limit[LIMIT_MAX_OUTPUT].known && limit[LIMIT_MAX_INPUT].value >
         limit[LIMIT_CONTEXT].value - limit[LIMIT_MAX_OUTPUT].value) ||
        (limit[LIMIT_CONTEXT].known && limit[LIMIT_AUTO_COMPACT].known &&
         limit[LIMIT_AUTO_COMPACT].value > limit[LIMIT_CONTEXT].value))
        goto invalid;
    limits = json_object();
    if (!limits)
        goto fail;
    for (size_t i = 0u; i < LIMIT_COUNT; ++i)
        if (set_optional_limit(limits, output_keys[i], &limit[i]) < 0)
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
        !snag_utf8_valid((const unsigned char *)text, len, true))
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
        json_array_size(source) > SNAG_PROVIDER_EFFORTS_MAX)
        return -1;
    for (size_t i = 0; i < json_array_size(source); ++i) {
        json_t *value = json_array_get(source, i);
        const char *effort = NULL;
        bool duplicate = false;

        if (json_is_object(value))
            value = json_object_get(value, "effort");
        else if (codex)
            return -1;
        if (!bounded_utf8_string(value, SNAG_CONFIG_EFFORT_MAX - 1u,
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
                             SNAG_CONFIG_MODEL_MAX - 1u, &id))
        return -1;
    for (size_t i = 0; i < json_array_size(out); ++i) {
        const char *existing = snag_json_string(json_array_get(out, i), "id");
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
            !bounded_utf8_string(value, SNAG_CONFIG_EFFORT_MAX - 1u,
                                 &default_effort))
            return -1;
    }
    if (codex && default_effort && !effort_source)
        return -1;
    efforts = json_array();
    if (!efforts ||
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
    entry = json_pack("{s:s?,s:O,s:s,s:O}", "default_effort", default_effort,
                      "efforts", efforts, "id", id, "limits", limits);
    json_decref(limits);
    json_decref(efforts);
    return json_array_append_new(out, entry);
fail:
    json_decref(limits);
    json_decref(efforts);
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
        snag_errorf(error, error_size, "invalid model catalog source");
        errno = EINVAL;
        return -1;
    }
    root = snag_json_load_strict(data, len,
                                SNAG_WIRE_BODY_MAX, json_error,
                                sizeof(json_error));
    if (!root || !json_is_object(root)) {
        (void)snprintf(error, error_size, "invalid model-list response: %s",
                       json_error[0] ? json_error : "root is not an object");
        errno = EPROTO;
        goto out;
    }
    source = json_object_get(root, codex ? "models" : "data");
    if (!json_is_array(source) ||
        json_array_size(source) > SNAG_PROVIDER_MODELS_MAX) {
        snag_errorf(error, error_size,
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
                snag_errorf(error, error_size,
                          "model-list response contains an invalid model entry");
                errno = EPROTO;
                goto out;
            }
            visibility = snag_json_string(model, "visibility");
            if (!visibility || strcmp(visibility, "list") != 0)
                continue;
            priority = json_object_get(model, "priority");
            if (!json_is_integer(priority)) {
                snag_errorf(error, error_size,
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
            snag_errorf(error, error_size,
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
        snag_errorf(error, error_size, ctx->error[0] ? ctx->error :
                  "model-list response body exceeds the supported limit");
        errno = EOVERFLOW;
        return -1;
    }
    return decode_models(ctx->error_body.data, ctx->error_body.len, codex,
                         models, error, error_size);
}

static bool
provider_uses_codex_catalog(const struct snag_provider_config *provider)
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
snag_provider_catalog_protocol(const struct snag_provider_config *provider)
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

static void
provider_ctx_init(struct provider_ctx *ctx, const struct snag_config *config,
                  const struct snag_provider_config *provider,
                  const struct snag_credential *credential,
                  struct snag_ui *render, snag_provider_pump_fn pump,
                  void *pump_opaque, size_t body_max, size_t response_max)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->config = config;
    ctx->provider = provider;
    ctx->render = render;
    ctx->pump = pump;
    ctx->pump_opaque = pump_opaque;
    ctx->credential = *credential;
    snag_buf_init(&ctx->body, body_max);
    snag_buf_init(&ctx->error_body, response_max);
}

static void
redact_diagnostic(const struct snag_secret_set *secrets, char *error, size_t error_size)
{
    json_t *message;
    const char *text = NULL;
    if (!error || !error_size || !error[0])
        return;
    message = json_object();
    if (message && json_object_set_new(message, "model_text", json_string(error)) == 0 &&
        snag_secret_result(secrets, message, NULL, 0u) == 0)
        text = snag_json_string(message, "model_text");
    (void)snprintf(error, error_size, "%s", text ? text : "provider diagnostic omitted");
    json_decref(message);
}

static void
provider_ctx_free(struct provider_ctx *ctx, char *error, size_t error_size)
{
    redact_diagnostic(&ctx->secrets, error, error_size);
    if (ctx->curl)
        curl_easy_cleanup(ctx->curl);
    curl_slist_free_all(ctx->headers);
    if (ctx->curl_global)
        curl_global_cleanup();
    snag_buf_free(&ctx->body);
    snag_buf_free(&ctx->error_body);
    snag_sse_free(&ctx->sse);
    snag_responses_stream_free(&ctx->stream);
    snag_credential_clear(&ctx->credential);
    snag_secret_set_free(&ctx->secrets);
}

static int
auth_pump(void *opaque, uint32_t wait_ms)
{
    struct provider_ctx *ctx = opaque;
    int rc = ctx->pump ? ctx->pump(ctx->pump_opaque, wait_ms) : 0;
    if (rc)
        ctx->cancel_code = rc < 0 ? 3 : rc;
    return rc;
}

static int
request_auth_headers(struct provider_ctx *ctx)
{
    struct curl_slist *headers = NULL;
    if (append_named_header(&headers, "Accept", ctx->accept) < 0 ||
        (ctx->has_body && append_header(&headers, "Content-Type: application/json") < 0) ||
        append_provider_headers(&headers, ctx->provider, &ctx->credential) < 0 ||
        curl_easy_setopt(ctx->curl, CURLOPT_HTTPHEADER, headers) != CURLE_OK) {
        curl_slist_free_all(headers);
        return -1;
    }
    curl_slist_free_all(ctx->headers);
    ctx->headers = headers;
    return 0;
}

static int
provider_request_setup(struct provider_ctx *ctx,
                       const struct snag_credential *credential,
                       const char *path, const char *accept,
                       const json_t *request, const char *body_error,
                       size_t (*write_fn)(char *, size_t, size_t, void *),
                       char *error, size_t error_size)
{
    char url[SNAG_CONFIG_URL_MAX + 64u];
    char request_line[SNAG_CONFIG_URL_MAX + 96u];
    const char *endpoint;
    bool has_body = request != NULL;
    int written;

    ctx->accept = accept;
    ctx->has_body = has_body;
    if (credential->root_fd >= 0 &&
        snag_auth_read(credential->root_fd, ctx->provider, false, NULL,
                      &ctx->credential, auth_pump, ctx,
                      error, error_size) < 0)
        return -1;
    if (snag_secret_set_build(&ctx->secrets, ctx->config, &ctx->credential,
                              error, error_size) < 0)
        return -1;
    if (has_body && snag_json_canonical(request, &ctx->body) < 0) {
        snag_errorf(error, error_size, "%s", body_error);
        return -1;
    }
    if (provider_endpoint_url(ctx->provider, path, url, sizeof(url), &endpoint,
                              error, error_size) < 0)
        return -1;
    written = snprintf(request_line, sizeof(request_line), "%s %s HTTP/1.1",
                       has_body ? "POST" : "GET",
                       url_request_target(endpoint));
    if (written <= 0 || (size_t)written >= sizeof(request_line)) {
        snag_errorf(error, error_size, "provider request line is too long");
        errno = ENAMETOOLONG;
        return -1;
    }
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
        snag_errorf(error, error_size, "libcurl could not initialize");
        errno = EIO;
        return -1;
    }
    ctx->curl_global = true;
    ctx->curl = curl_easy_init();
    if (!ctx->curl) {
        snag_errorf(error, error_size, "libcurl easy handle could not initialize");
        errno = ENOMEM;
        return -1;
    }
    if (request_auth_headers(ctx) < 0) {
        snag_errorf(error, error_size, "provider headers could not be allocated");
        return -1;
    }
    if (render_request_headers(ctx, request_line, accept, has_body) < 0) {
        snag_errorf(error, error_size, ctx->error[0] ? ctx->error :
                   "provider request headers could not be rendered");
        return -1;
    }
    if (curl_easy_setopt(ctx->curl, CURLOPT_URL, endpoint) != CURLE_OK ||
        curl_easy_setopt(ctx->curl, CURLOPT_NOSIGNAL, 1L) != CURLE_OK ||
        curl_easy_setopt(ctx->curl, CURLOPT_HTTPHEADER, ctx->headers) != CURLE_OK ||
        (has_body ?
         (curl_easy_setopt(ctx->curl, CURLOPT_POST, 1L) != CURLE_OK ||
          curl_easy_setopt(ctx->curl, CURLOPT_POSTFIELDS,
                           (char *)ctx->body.data) != CURLE_OK ||
          curl_easy_setopt(ctx->curl, CURLOPT_POSTFIELDSIZE_LARGE,
                           (curl_off_t)ctx->body.len) != CURLE_OK) :
         curl_easy_setopt(ctx->curl, CURLOPT_HTTPGET, 1L) != CURLE_OK) ||
        curl_easy_setopt(ctx->curl, CURLOPT_WRITEFUNCTION, write_fn) != CURLE_OK ||
        curl_easy_setopt(ctx->curl, CURLOPT_WRITEDATA, ctx) != CURLE_OK ||
        curl_easy_setopt(ctx->curl, CURLOPT_HEADERFUNCTION, header_cb) != CURLE_OK ||
        curl_easy_setopt(ctx->curl, CURLOPT_HEADERDATA, ctx) != CURLE_OK ||
        curl_easy_setopt(ctx->curl, CURLOPT_CONNECTTIMEOUT_MS,
                         (long)ctx->provider->connect_timeout_ms) != CURLE_OK ||
        curl_easy_setopt(ctx->curl, CURLOPT_TIMEOUT_MS,
                         (long)ctx->provider->request_timeout_ms) != CURLE_OK ||
        curl_easy_setopt(ctx->curl, CURLOPT_LOW_SPEED_LIMIT, 1L) != CURLE_OK ||
        curl_easy_setopt(ctx->curl, CURLOPT_LOW_SPEED_TIME,
                         (long)low_speed_seconds(
                             ctx->provider->idle_timeout_ms)) != CURLE_OK ||
        curl_easy_setopt(ctx->curl, CURLOPT_USERAGENT,
                         SNAJPAGENT_NAME "/" SNAJPAGENT_VERSION) != CURLE_OK ||
        curl_easy_setopt(ctx->curl, CURLOPT_FOLLOWLOCATION, 0L) != CURLE_OK) {
        snag_errorf(error, error_size, "libcurl option setup failed");
        errno = EIO;
        return -1;
    }
    return 0;
}

static int
provider_request_perform(struct provider_ctx *ctx, const char *failure,
                         char *error, size_t error_size, int *cancel_code,
                         unsigned int *retry_count)
{
    unsigned int retries = 0u;
    unsigned int *retry_out = retry_count ? retry_count : &retries;
    CURLcode code = perform_with_retry(ctx->curl, ctx, error, error_size,
                                       cancel_code, retry_out);

    if (code == CURLE_OK && ctx->http_status == 401 &&
        ctx->provider->auth == SNAG_AUTH_CHATGPT && ctx->credential.root_fd >= 0 &&
        !ctx->semantic_body_seen) {
        struct snag_credential refreshed;
        int rc = snag_auth_read(ctx->credential.root_fd, ctx->provider, true,
                               ctx->credential.value, &refreshed, auth_pump,
                               ctx, error, error_size);
        if (rc < 0) {
            if (ctx->cancel_code == 1 || ctx->cancel_code == 2) {
                if (cancel_code)
                    *cancel_code = ctx->cancel_code;
                return ctx->cancel_code;
            }
            return -1;
        }
        ctx->credential = refreshed;
        snag_credential_clear(&refreshed);
        if (snag_secret_set_build(&ctx->secrets, ctx->config, &ctx->credential,
                                  error, error_size) < 0)
            return -1;
        if (request_auth_headers(ctx) < 0 ||
            curl_easy_setopt(ctx->curl, CURLOPT_HTTPHEADER, ctx->headers) != CURLE_OK)
            return -1;
        code = perform_with_retry(ctx->curl, ctx, error, error_size,
                                   cancel_code, retry_out);
    }

    if (code == CURLE_ABORTED_BY_CALLBACK &&
        (ctx->cancel_code == 1 || ctx->cancel_code == 2)) {
        if (cancel_code)
            *cancel_code = ctx->cancel_code;
        return ctx->cancel_code;
    }
    if (code != CURLE_OK) {
        snag_errorf(error, error_size, "%s%s%s",
                   ctx->error[0] ? ctx->error : failure,
                   ctx->error[0] ? "" : ": ",
                   ctx->error[0] ? "" : curl_easy_strerror(code));
        append_retry_suffix(error, error_size, *retry_out,
                            ctx->request_may_have_been_sent);
        errno = EIO;
        return -1;
    }
    if (ctx->http_status < 200 || ctx->http_status >= 300) {
        (void)classify_non2xx(ctx, error, error_size);
        append_retry_suffix(error, error_size, *retry_out,
                            ctx->request_may_have_been_sent);
        return -1;
    }
    return 0;
}

int
snag_provider_models_list(const struct snag_config *config,
                         const struct snag_provider_config *provider,
                         const struct snag_credential *credential,
                         struct snag_ui *render,
                         snag_provider_pump_fn pump, void *pump_opaque,
                         json_t **models,
                         char *error, size_t error_size)
{
    struct provider_ctx ctx;
    const char *path;
    bool codex;
    unsigned int retry_count = 0u;
    int rc = -1;

    if (models)
        *models = NULL;
    if (!config || !provider || !credential || !credential->len || !models) {
        snag_errorf(error, error_size, "invalid model-list request");
        errno = EINVAL;
        return -1;
    }
    provider_ctx_init(&ctx, config, provider, credential, render, pump, pump_opaque,
                      SNAG_WIRE_BODY_MAX, SNAG_WIRE_BODY_MAX);
    codex = provider_uses_codex_catalog(provider);
    path = codex ? SNAG_CODEX_CATALOG_PATH : "/v1/models";
    if (provider_request_setup(&ctx, credential, path, "application/json",
                               NULL, NULL, count_write_cb,
                               error, error_size) == 0 &&
        provider_request_perform(&ctx, "model discovery failed",
                                 error, error_size, NULL, &retry_count) == 0)
        rc = parse_models_body(&ctx, codex, models, error, error_size);
    provider_ctx_free(&ctx, rc != 0 ? error : NULL, error_size);
    return rc;
}

int
snag_provider_responses_count(const json_t *count_request,
                             const struct snag_config *config,
                             const struct snag_provider_config *provider,
                             const struct snag_credential *credential,
                             struct snag_ui *render,
                             snag_provider_pump_fn pump,
                             void *pump_opaque,
                             uint64_t *input_tokens,
                             bool *endpoint_unsupported,
                             char *error, size_t error_size,
                             int *cancel_code,
                             unsigned int *retry_count)
{
    struct provider_ctx ctx;
    int rc = -1;

    if (cancel_code)
        *cancel_code = 0;
    if (retry_count)
        *retry_count = 0u;
    if (endpoint_unsupported)
        *endpoint_unsupported = false;
    if (!count_request || !config || !provider || !credential || !credential->len ||
        !input_tokens) {
        snag_errorf(error, error_size, "invalid input-token count request");
        errno = EINVAL;
        return -1;
    }
    *input_tokens = 0u;
    if (provider->auth == SNAG_AUTH_CHATGPT) {
        if (endpoint_unsupported)
            *endpoint_unsupported = true;
        snag_errorf(error, error_size, "direct Codex does not provide exact input-token preflight");
        errno = ENOTSUP;
        return -1;
    }
    provider_ctx_init(&ctx, config, provider, credential, render, pump,
                      pump_opaque, SNAG_CONTEXT_MAX_REQUEST, SNAG_WIRE_BODY_MAX);
    if (provider_request_setup(&ctx, credential,
            "/v1/responses/input_tokens", "application/json", count_request,
            "input-token count request exceeds the bounded body limit",
            count_write_cb, error, error_size) == 0)
        rc = provider_request_perform(&ctx, "input-token count failed",
                                      error, error_size, cancel_code,
                                      retry_count);
    if (rc != 0 && endpoint_unsupported &&
        (ctx.http_status == 405 || ctx.http_status == 501 ||
         (ctx.http_status == 404 && snag_config_provider_is_openrouter(provider))))
        *endpoint_unsupported = true;
    if (rc == 0)
        rc = parse_count_body(&ctx, input_tokens, error, error_size);
    if (ctx.cancel_code == 1 || ctx.cancel_code == 2) {
        rc = ctx.cancel_code;
        if (cancel_code)
            *cancel_code = rc;
    }
    provider_ctx_free(&ctx, rc != 0 ? error : NULL, error_size);
    return rc;
}

int
snag_provider_responses_compact(const json_t *compact_request,
                               const struct snag_config *config,
                               const struct snag_provider_config *provider,
                               const struct snag_credential *credential,
                               struct snag_ui *render,
                               snag_provider_pump_fn pump,
                               void *pump_opaque,
                               json_t **output,
                               uint64_t *output_tokens_bound,
                               char *error, size_t error_size,
                               int *cancel_code,
                               unsigned int *retry_count)
{
    struct provider_ctx ctx;
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
        snag_errorf(error, error_size, "invalid compact request");
        errno = EINVAL;
        return -1;
    }
    provider_ctx_init(&ctx, config, provider, credential, render, pump,
                      pump_opaque, SNAG_CONTEXT_MAX_COMPACT,
                      SNAG_CONTEXT_MAX_COMPACT);
    if (provider_request_setup(&ctx, credential, "/v1/responses/compact",
            "application/json", compact_request,
            "compact request exceeds the bounded body limit", count_write_cb,
            error, error_size) == 0)
        rc = provider_request_perform(&ctx, "compact request failed", error,
                                      error_size, cancel_code, retry_count);
    if (rc == 0)
        rc = parse_compact_body(&ctx, output, output_tokens_bound,
                                error, error_size);
    if (rc < 0 && provider->auth == SNAG_AUTH_CHATGPT &&
        (ctx.http_status == 404 || ctx.http_status == 405 || ctx.http_status == 501))
        rc = SNAG_PROVIDER_UNSUPPORTED;
    if (ctx.cancel_code == 1 || ctx.cancel_code == 2) {
        rc = ctx.cancel_code;
        if (cancel_code)
            *cancel_code = rc;
    }
    provider_ctx_free(&ctx, rc != 0 ? error : NULL, error_size);
    return rc;
}

int
snag_provider_responses_create(const json_t *create_request,
                              const struct snag_config *config,
                              const struct snag_provider_config *provider,
                              const struct snag_credential *credential,
                              struct snag_ui *render,
                              snag_responses_emit_fn emit,
                              void *emit_opaque,
                              snag_provider_pump_fn pump,
                              void *pump_opaque,
                              struct snag_response_graph *graph,
                              struct snag_provider_failure *failure,
                              char *error, size_t error_size,
                              int *cancel_code,
                              unsigned int *retry_count)
{
    struct provider_ctx ctx;
    int rc = -1;

    if (cancel_code)
        *cancel_code = 0;
    if (failure)
        memset(failure, 0, sizeof(*failure));
    if (retry_count)
        *retry_count = 0u;
    if (!create_request || !config || !provider || !credential ||
        !credential->len || !graph) {
        snag_errorf(error, error_size, "invalid provider request");
        errno = EINVAL;
        return -1;
    }
    provider_ctx_init(&ctx, config, provider, credential, render, pump,
                      pump_opaque, SNAG_CONTEXT_MAX_REQUEST, SNAG_WIRE_BODY_MAX);
    snag_responses_stream_init(&ctx.stream, emit, emit_opaque);
    snag_sse_init(&ctx.sse, snag_responses_sse_record, &ctx.stream);
    if (provider_request_setup(&ctx, credential, "/v1/responses",
            "text/event-stream", create_request,
            "provider request exceeds the bounded body limit", write_cb,
            error, error_size) == 0)
        rc = provider_request_perform(&ctx, "provider transport failed", error,
                                      error_size, cancel_code, retry_count);
    if (rc != 0)
        goto out;
    if (snag_sse_finish(&ctx.sse, error, error_size) < 0) {
        snag_errorf(error, error_size,
                  stream_or_sse_error(&ctx, error,
                                      "invalid provider SSE stream"));
        goto out;
    }
    rc = snag_responses_stream_finish(&ctx.stream, graph, error, error_size);
    if (rc != 0) {
        if (rc > 0)
            rc = 3;
        goto out;
    }
    rc = 0;
out:
    if (failure) {
        if (ctx.stream.provider_failure.code[0])
            *failure = ctx.stream.provider_failure;
        else
            *failure = ctx.provider_failure;
        failure->output_correction = ctx.stream.output_correction;
        redact_diagnostic(&ctx.secrets, failure->message, sizeof(failure->message));
    }
    if (ctx.cancel_code == 1 || ctx.cancel_code == 2) {
        rc = ctx.cancel_code;
        if (cancel_code)
            *cancel_code = rc;
    }
    provider_ctx_free(&ctx, rc != 0 ? error : NULL, error_size);
    return rc;
}

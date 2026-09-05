/* SPDX-License-Identifier: GPL-2.0-only */
#include "auth.h"
#include "base.h"
#include "json.h"
#include "snajpagent.h"

#include <curl/curl.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AUTH_BODY_MAX (96u * 1024u)
#define AUTH_ISSUER "https://auth.openai.com"
#define AUTH_CLIENT "app_EMoamEEZ73f0CkXaXp7hrann"

static const char *
auth_string(const json_t *object, const char *key)
{
    const char *value = snj_json_string(object, key);
    return value ? value : "";
}

static const char *
auth_issuer(void)
{
#if defined(SNAJPAGENT_TEST_TRANSPORT_ENDPOINTS) || defined(SNAJPAGENT_TEST_FIXTURE)
    const char *test = getenv("SNAJPAGENT_TEST_AUTH_BASE");
    if (test && strncmp(test, "http://127.0.0.1:", 17u) == 0)
        return test;
#endif
    return AUTH_ISSUER;
}

static size_t
receive_body(char *data, size_t size, size_t count, void *opaque)
{
    struct snj_buf *buf = opaque;
    if (size && count > SIZE_MAX / size)
        return 0;
    size *= count;
    return snj_buf_append(buf, data, size) == 0 ? size : 0;
}

static int
auth_post(const char *path, const char *type, const void *body, size_t size,
           json_t **response, long *status, snj_auth_pump_fn pump, void *opaque,
           char *error, size_t error_size)
{
    char url[4096], header[96], parse_error[128];
    struct snj_buf output;
    struct curl_slist *headers = NULL;
    CURL *curl = NULL;
    CURLM *multi = NULL;
    CURLMsg *message;
    int running = 0, pending, rc = -1;
    bool attached = false, initialized = false;

    *response = NULL;
    *status = 0;
    snj_buf_init(&output, AUTH_BODY_MAX);
    if (snprintf(url, sizeof(url), "%s%s", auth_issuer(), path) >= (int)sizeof(url) ||
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
            snj_errorf(error, error_size, "login or token refresh cancelled");
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
        *response = snj_json_load_strict(output.data, output.len, AUTH_BODY_MAX,
                                        parse_error, sizeof(parse_error));
        if (!json_is_object(*response)) {
            snj_errorf(error, error_size, "authentication server returned an invalid response");
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
    snj_buf_free(&output);
    if (rc < 0) {
        snj_auth_json_free(*response);
        *response = NULL;
        if (!error[0])
            snj_errorf(error, error_size, "authentication request failed (response details withheld)");
    }
    return rc;
}

static int
post_json(const char *path, json_t *request, json_t **response, long *status,
           snj_auth_pump_fn pump, void *opaque, char *error, size_t error_size)
{
    struct snj_buf body;
    int rc = -1;
    snj_buf_init(&body, AUTH_BODY_MAX);
    if (snj_json_canonical(request, &body) == 0)
        rc = auth_post(path, "application/json", body.data, body.len,
                       response, status, pump, opaque, error, error_size);
    if (body.data)
        memset(body.data, 0, body.len);
    snj_buf_free(&body);
    return rc;
}

static json_t *
token_claims(const char *token)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    const char *p = token ? strchr(token, '.') : NULL;
    struct snj_buf decoded;
    unsigned int value = 0u, bits = 0u;
    char error[128];
    json_t *claims = NULL;

    if (!p)
        return NULL;
    snj_buf_init(&decoded, AUTH_BODY_MAX);
    for (++p; *p && *p != '.'; ++p) {
        const char *digit = strchr(alphabet, *p);
        if (!digit)
            goto out;
        value = (value << 6) | (unsigned int)(digit - alphabet);
        bits += 6u;
        if (bits >= 8u) {
            bits -= 8u;
            if (snj_buf_putc(&decoded, (unsigned char)(value >> bits)) < 0)
                goto out;
        }
    }
    if (*p == '.')
        claims = snj_json_load_strict(decoded.data, decoded.len, AUTH_BODY_MAX,
                                      error, sizeof(error));
out:
    if (decoded.data)
        memset(decoded.data, 0, decoded.len);
    snj_buf_free(&decoded);
    return claims;
}

int
snj_auth_token_response(json_t *response, struct snj_auth_tokens *tokens,
                        char *error, size_t error_size)
{
    struct snj_auth_tokens next;
    json_t *access = NULL, *identity = NULL;
    const char *refresh = auth_string(response, "refresh_token");
    const char *account;
    uint64_t expires = 0u, lifetime = 0u;
    int rc = -1;

    snj_auth_clear(&next);
    if (snj_auth_key(&next, snj_json_string(response, "access_token"),
                     error, error_size) < 0)
        goto out;
    if (!*refresh)
        refresh = tokens->refresh_token;
    if (!*refresh || !snj_strcpy(next.refresh_token, sizeof(next.refresh_token), refresh))
        goto out;
    access = token_claims(next.credential.value);
    identity = token_claims(snj_json_string(response, "id_token"));
    account = auth_string(json_object_get(identity, "https://api.openai.com/auth"),
                              "chatgpt_account_id");
    if (!*account)
        account = auth_string(json_object_get(access, "https://api.openai.com/auth"),
                                  "chatgpt_account_id");
    if (!*account)
        account = tokens->credential.account_id;
    if (!*account || !snj_strcpy(next.credential.account_id,
                                sizeof(next.credential.account_id), account))
        goto out;
    for (const unsigned char *p = (const unsigned char *)account; *p; ++p)
        if (*p < 0x21u || *p > 0x7eu)
            goto out;
    if (tokens->credential.account_id[0] &&
        strcmp(tokens->credential.account_id, account) != 0) {
        snj_errorf(error, error_size, "refreshed account changed; log in again");
        goto out;
    }
    if (snj_json_integer_u64(access, "exp", &expires) == 0 &&
        expires > 0u && expires <= INT64_MAX / 1000u)
        next.expires_at_ms = expires * 1000u;
    else if (snj_json_integer_u64(response, "expires_in", &lifetime) == 0 &&
             lifetime > 0u && lifetime <= 365u * 86400u)
        next.expires_at_ms = snj_time_ms() + lifetime * 1000u;
    else
        goto out;
    if (next.expires_at_ms <= snj_time_ms())
        goto out;
    *tokens = next;
    rc = 0;
out:
    snj_auth_clear(&next);
    json_decref(access);
    json_decref(identity);
    if (rc < 0 && !error[0])
        snj_errorf(error, error_size, "authentication response lacks valid tokens, account, or expiry");
    return rc;
}

int
snj_auth_refresh(struct snj_auth_tokens *tokens, snj_auth_pump_fn pump,
                 void *opaque, char *error, size_t error_size)
{
    json_t *request = json_object(), *response = NULL;
    long status = 0;
    int rc = -1;
    if (!request ||
        snj_json_set_new(request, "grant_type", json_string("refresh_token")) < 0 ||
        snj_json_set_new(request, "client_id", json_string(AUTH_CLIENT)) < 0 ||
        snj_json_set_new(request, "refresh_token", json_string(tokens->refresh_token)) < 0 ||
        post_json("/oauth/token", request, &response, &status,
                   pump, opaque, error, error_size) < 0)
        goto out;
    if (status < 200 || status >= 300) {
        snj_errorf(error, error_size,
            status == 400 || status == 401 || status == 403 ?
            "Codex login expired or revoked; run snajpagent login again" :
            "Codex token refresh temporarily failed; try again");
        goto out;
    }
    rc = snj_auth_token_response(response, tokens, error, error_size);
out:
    snj_auth_json_free(request);
    snj_auth_json_free(response);
    return rc;
}

int
snj_auth_device(struct snj_auth_tokens *tokens, snj_auth_pump_fn pump,
                void *opaque, char *error, size_t error_size)
{
    json_t *request = NULL, *response = NULL, *code = NULL;
    struct snj_buf body;
    char *authorization = NULL, *verifier = NULL, *redirect = NULL;
    char callback[4096];
    uint64_t interval = 5u, deadline;
    long status = 0;
    int rc = -1;

    snj_auth_clear(tokens);
    snj_buf_init(&body, AUTH_BODY_MAX);
    request = json_object();
    if (!request || snj_json_set_new(request, "client_id", json_string(AUTH_CLIENT)) < 0 ||
        post_json("/api/accounts/deviceauth/usercode", request, &response,
                   &status, pump, opaque, error, error_size) < 0)
        goto out;
    if (status != 200 || !*auth_string(response, "device_auth_id") ||
        !*auth_string(response, "user_code")) {
        snj_errorf(error, error_size, "device login unavailable; enable device-code login in ChatGPT security/workspace settings (HTTP %ld)", status);
        goto out;
    }
    if (snj_json_integer_u64(response, "interval", &interval) < 0) {
        const char *s = auth_string(response, "interval");
        char *end;
        unsigned long n = strtoul(s, &end, 10);
        interval = *s && !*end ? (uint64_t)n : 5u;
    }
    if (!interval || interval > 60u)
        interval = 5u;
    /* Print only the bounded user code, never an arbitrary server message. */
    {
        const char *user_code = snj_json_string(response, "user_code");
        if (strlen(user_code) > 64u)
            goto out;
        for (const unsigned char *p = (const unsigned char *)user_code; *p; ++p)
            if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                  (*p >= '0' && *p <= '9') || *p == '-'))
                goto out;
        (void)fprintf(stderr, "Open %s/codex/device\nEnter code: %s\nWaiting for login (up to 15 minutes; Ctrl-C cancels)...\n",
                       auth_issuer(), user_code);
    }
    json_decref(request);
    request = json_object();
    if (!request ||
        snj_json_set_new(request, "device_auth_id", json_string(snj_json_string(response, "device_auth_id"))) < 0 ||
        snj_json_set_new(request, "user_code", json_string(snj_json_string(response, "user_code"))) < 0)
        goto out;
    snj_auth_json_free(response);
    response = NULL;
    deadline = snj_time_ms() + 15u * 60u * 1000u;
    for (;;) {
        if (post_json("/api/accounts/deviceauth/token", request, &code,
                       &status, pump, opaque, error, error_size) < 0)
            goto out;
        if (status == 200)
            break;
        if (status != 403 && status != 404) {
            snj_errorf(error, error_size, "device login failed (HTTP %ld)", status);
            goto out;
        }
        uint64_t next = snj_time_ms() + interval * 1000u;
        while (snj_time_ms() < next) {
            if (snj_time_ms() >= deadline) {
                snj_errorf(error, error_size, "device login expired; start login again");
                goto out;
            }
            if (pump ? pump(opaque, 100u) != 0 : poll(NULL, 0, 100) < 0) {
                errno = ECANCELED;
                snj_errorf(error, error_size, "device login cancelled");
                goto out;
            }
        }
    }
    authorization = curl_easy_escape(NULL, auth_string(code, "authorization_code"), 0);
    verifier = curl_easy_escape(NULL, auth_string(code, "code_verifier"), 0);
    (void)snprintf(callback, sizeof(callback), "%s/deviceauth/callback", auth_issuer());
    redirect = curl_easy_escape(NULL, callback, 0);
    if (!*auth_string(code, "authorization_code") || !*auth_string(code, "code_verifier") ||
        !authorization || !verifier || !redirect ||
        snj_buf_printf(&body, "grant_type=authorization_code&client_id=%s&code=%s&code_verifier=%s&redirect_uri=%s",
                        AUTH_CLIENT, authorization, verifier, redirect) < 0 ||
        auth_post("/oauth/token", "application/x-www-form-urlencoded", body.data,
                   body.len, &response, &status, pump, opaque, error, error_size) < 0)
        goto out;
    if (status != 200) {
        snj_errorf(error, error_size, "device login token exchange failed (HTTP %ld)", status);
        goto out;
    }
    rc = snj_auth_token_response(response, tokens, error, error_size);
out:
    if (authorization)
        memset(authorization, 0, strlen(authorization));
    if (verifier)
        memset(verifier, 0, strlen(verifier));
    curl_free(authorization);
    curl_free(verifier);
    curl_free(redirect);
    snj_auth_json_free(request);
    snj_auth_json_free(response);
    snj_auth_json_free(code);
    if (body.data)
        memset(body.data, 0, body.len);
    snj_buf_free(&body);
    if (rc < 0 && !error[0])
        snj_errorf(error, error_size, "invalid device login response");
    return rc;
}

/* SPDX-License-Identifier: GPL-2.0-only */
#include "auth.h"
#include "base.h"
#include "json.h"
#include "snajpagent.h"

#include "provider.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AUTH_BODY_MAX (96u * 1024u)
#define AUTH_ISSUER "https://auth.openai.com"
#define AUTH_CLIENT "app_EMoamEEZ73f0CkXaXp7hrann"

static const char *
auth_string(const json_t *object, const char *key)
{
    const char *value = snag_json_string(object, key);
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

static int
auth_post(const char *path, const char *type, const void *body, size_t size,
           json_t **response, long *status, snag_auth_pump_fn pump, void *opaque,
           char *error, size_t error_size)
{
    return snag_provider_auth_post(auth_issuer(), path, type, body, size,
                                   response, status, pump, opaque, error, error_size);
}

static int
post_json(const char *path, json_t *request, json_t **response, long *status,
           snag_auth_pump_fn pump, void *opaque, char *error, size_t error_size)
{
    struct snag_buf body;
    int rc = -1;
    snag_buf_init(&body, AUTH_BODY_MAX);
    if (snag_json_canonical(request, &body) == 0)
        rc = auth_post(path, "application/json", body.data, body.len,
                       response, status, pump, opaque, error, error_size);
    if (body.data)
        memset(body.data, 0, body.len);
    snag_buf_free(&body);
    return rc;
}

static json_t *
token_claims(const char *token)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    const char *p = token ? strchr(token, '.') : NULL;
    struct snag_buf decoded;
    unsigned int value = 0u, bits = 0u;
    char error[128];
    json_t *claims = NULL;

    if (!p)
        return NULL;
    snag_buf_init(&decoded, AUTH_BODY_MAX);
    for (++p; *p && *p != '.'; ++p) {
        const char *digit = strchr(alphabet, *p);
        if (!digit)
            goto out;
        value = (value << 6) | (unsigned int)(digit - alphabet);
        bits += 6u;
        if (bits >= 8u) {
            bits -= 8u;
            if (snag_buf_putc(&decoded, (unsigned char)(value >> bits)) < 0)
                goto out;
        }
    }
    if (*p == '.')
        claims = snag_json_load_strict(decoded.data, decoded.len, AUTH_BODY_MAX,
                                      error, sizeof(error));
out:
    if (decoded.data)
        memset(decoded.data, 0, decoded.len);
    snag_buf_free(&decoded);
    return claims;
}

int
snag_auth_token_response(json_t *response, struct snag_auth_tokens *tokens,
                        char *error, size_t error_size)
{
    struct snag_auth_tokens next;
    json_t *access = NULL, *identity = NULL;
    const char *refresh = auth_string(response, "refresh_token");
    const char *account;
    uint64_t expires = 0u, lifetime = 0u;
    int rc = -1;

    snag_auth_clear(&next);
    if (snag_auth_key(&next, snag_json_string(response, "access_token"),
                     error, error_size) < 0)
        goto out;
    if (!*refresh)
        refresh = tokens->refresh_token;
    if (!*refresh || !snag_strcpy(next.refresh_token, sizeof(next.refresh_token), refresh))
        goto out;
    for (const unsigned char *p = (const unsigned char *)refresh; *p; ++p)
        if (*p < 0x21u || *p > 0x7eu)
            goto out;
    access = token_claims(next.credential.value);
    identity = token_claims(snag_json_string(response, "id_token"));
    account = auth_string(json_object_get(identity, "https://api.openai.com/auth"),
                              "chatgpt_account_id");
    if (!*account)
        account = auth_string(json_object_get(access, "https://api.openai.com/auth"),
                                  "chatgpt_account_id");
    if (!*account)
        account = tokens->credential.account_id;
    if (!*account || !snag_strcpy(next.credential.account_id,
                                sizeof(next.credential.account_id), account))
        goto out;
    for (const unsigned char *p = (const unsigned char *)account; *p; ++p)
        if (*p < 0x21u || *p > 0x7eu)
            goto out;
    if (tokens->credential.account_id[0] &&
        strcmp(tokens->credential.account_id, account) != 0) {
        snag_errorf(error, error_size, "refreshed account changed; log in again");
        goto out;
    }
    if (snag_json_integer_u64(access, "exp", &expires) == 0 &&
        expires > 0u && expires <= INT64_MAX / 1000u)
        next.expires_at_ms = expires * 1000u;
    else if (snag_json_integer_u64(response, "expires_in", &lifetime) == 0 &&
             lifetime > 0u && lifetime <= 365u * 86400u)
        next.expires_at_ms = snag_time_ms() + lifetime * 1000u;
    else
        goto out;
    if (next.expires_at_ms <= snag_time_ms())
        goto out;
    *tokens = next;
    rc = 0;
out:
    snag_auth_clear(&next);
    json_decref(access);
    json_decref(identity);
    if (rc < 0 && !error[0])
        snag_errorf(error, error_size, "authentication response lacks valid tokens, account, or expiry");
    return rc;
}

int
snag_auth_refresh(struct snag_auth_tokens *tokens, snag_auth_pump_fn pump,
                 void *opaque, char *error, size_t error_size)
{
    json_t *request = json_object(), *response = NULL;
    long status = 0;
    int rc = -1;
    if (!request ||
        snag_json_set_new(request, "grant_type", json_string("refresh_token")) < 0 ||
        snag_json_set_new(request, "client_id", json_string(AUTH_CLIENT)) < 0 ||
        snag_json_set_new(request, "refresh_token", json_string(tokens->refresh_token)) < 0 ||
        post_json("/oauth/token", request, &response, &status,
                   pump, opaque, error, error_size) < 0)
        goto out;
    if (status < 200 || status >= 300) {
        snag_errorf(error, error_size,
            status == 400 || status == 401 || status == 403 ?
            "Codex login expired or revoked; run snajpagent login again" :
            "Codex token refresh temporarily failed; try again");
        goto out;
    }
    rc = snag_auth_token_response(response, tokens, error, error_size);
out:
    snag_auth_json_free(request);
    snag_auth_json_free(response);
    return rc;
}

static int
snag_auth_form_field(struct snag_buf *body, const char *name, const char *value)
{
    if (snag_buf_printf(body, "&%s=", name) < 0)
        return -1;
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
            (*p >= '0' && *p <= '9') || *p == '-' || *p == '.' ||
            *p == '_' || *p == '~') {
            if (snag_buf_putc(body, *p) < 0)
                return -1;
        } else if (snag_buf_printf(body, "%%%02X", (unsigned int)*p) < 0) {
            return -1;
        }
    }
    return 0;
}

int
snag_auth_device(struct snag_auth_tokens *tokens, snag_auth_pump_fn pump,
                void *opaque, char *error, size_t error_size)
{
    json_t *request = NULL, *response = NULL, *code = NULL;
    struct snag_buf body;
    char callback[4096];
    uint64_t interval = 5u, deadline;
    long status = 0;
    int rc = -1;

    snag_auth_clear(tokens);
    snag_buf_init(&body, AUTH_BODY_MAX);
    request = json_object();
    if (!request || snag_json_set_new(request, "client_id", json_string(AUTH_CLIENT)) < 0 ||
        post_json("/api/accounts/deviceauth/usercode", request, &response,
                   &status, pump, opaque, error, error_size) < 0)
        goto out;
    if (!json_object_get(response, "user_code") && json_is_string(json_object_get(response, "usercode")) &&
        snag_json_set_new(response, "user_code", json_string(auth_string(response, "usercode"))) < 0)
        goto out;
    if (status != 200 || !*auth_string(response, "device_auth_id") ||
        !*auth_string(response, "user_code")) {
        snag_errorf(error, error_size, "device login unavailable; enable device-code login in ChatGPT security/workspace settings (HTTP %ld)", status);
        goto out;
    }
    if (snag_json_integer_u64(response, "interval", &interval) < 0) {
        const char *s = auth_string(response, "interval");
        char *end;
        unsigned long n = strtoul(s, &end, 10);
        interval = *s && !*end ? (uint64_t)n : 5u;
    }
    if (!interval || interval > 60u)
        interval = 5u;
    /* Print only the bounded user code, never an arbitrary server message. */
    {
        const char *user_code = snag_json_string(response, "user_code");
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
        snag_json_set_new(request, "device_auth_id", json_string(snag_json_string(response, "device_auth_id"))) < 0 ||
        snag_json_set_new(request, "user_code", json_string(snag_json_string(response, "user_code"))) < 0)
        goto out;
    snag_auth_json_free(response);
    response = NULL;
    deadline = snag_monotonic_ms() + 15u * 60u * 1000u;
    for (;;) {
        if (post_json("/api/accounts/deviceauth/token", request, &code,
                       &status, pump, opaque, error, error_size) < 0)
            goto out;
        if (status == 200)
            break;
        if (status != 403 && status != 404) {
            snag_errorf(error, error_size, status == 410 ?
                "device login expired; start login again (HTTP %ld)" :
                "device login denied or failed (HTTP %ld)", status);
            goto out;
        }
        uint64_t next = snag_monotonic_ms() + interval * 1000u;
        while (snag_monotonic_ms() < next) {
            if (snag_monotonic_ms() >= deadline) {
                snag_errorf(error, error_size, "device login expired; start login again");
                goto out;
            }
            if (pump ? pump(opaque, 100u) != 0 : snag_sleep_ms(100u) < 0) {
                errno = ECANCELED;
                snag_errorf(error, error_size, "device login cancelled");
                goto out;
            }
        }
    }
    (void)snprintf(callback, sizeof(callback), "%s/deviceauth/callback", auth_issuer());
    if (!*auth_string(code, "authorization_code") || !*auth_string(code, "code_verifier") ||
        snag_buf_printf(&body, "grant_type=authorization_code&client_id=%s", AUTH_CLIENT) < 0 ||
        snag_auth_form_field(&body, "code", auth_string(code, "authorization_code")) < 0 ||
        snag_auth_form_field(&body, "code_verifier", auth_string(code, "code_verifier")) < 0 ||
        snag_auth_form_field(&body, "redirect_uri", callback) < 0 ||
        auth_post("/oauth/token", "application/x-www-form-urlencoded", body.data,
                   body.len, &response, &status, pump, opaque, error, error_size) < 0)
        goto out;
    if (status != 200) {
        snag_errorf(error, error_size, "device login token exchange failed (HTTP %ld)", status);
        goto out;
    }
    rc = snag_auth_token_response(response, tokens, error, error_size);
out:
    snag_auth_json_free(request);
    snag_auth_json_free(response);
    snag_auth_json_free(code);
    if (body.data)
        memset(body.data, 0, body.len);
    snag_buf_free(&body);
    if (rc < 0 && !error[0])
        snag_errorf(error, error_size, "invalid device login response");
    return rc;
}

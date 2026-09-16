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
#define META_AUTH_ISSUER "https://auth.meta.com"
#define META_AUTH_CLIENT "1031625952748946"
#define META_DEVICE_GRANT "urn:ietf:params:oauth:grant-type:device_code"
#define META_AUTH_PATH "/oidc/device/authorization/"
#define META_TOKEN_PATH "/oidc/device/token/"

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

/* Same overrides the Meta launcher honors; anything else keeps the default. */
static const char *
meta_issuer(void)
{
#if defined(SNAJPAGENT_TEST_TRANSPORT_ENDPOINTS) || defined(SNAJPAGENT_TEST_FIXTURE)
    const char *test = getenv("SNAJPAGENT_TEST_META_AUTH_BASE");
    if (test && strncmp(test, "http://127.0.0.1:", 17u) == 0)
        return test;
#endif
    const char *env = getenv("MUSE_AUTH_URL");
    size_t len = env ? strlen(env) : 0u;
    if (env && len > 8u && len < 256u && strncmp(env, "https://", 8u) == 0) {
        for (const unsigned char *p = (const unsigned char *)env; *p; ++p)
            if (*p <= 32 || *p == 127 || *p == 64) return META_AUTH_ISSUER;
        return env;
    }
    return META_AUTH_ISSUER;
}

static const char *
meta_client_id(void)
{
    const char *env = getenv("MUSE_CLIENT_ID");
    size_t len = env ? strlen(env) : 0u;
    if (env && len && len <= 128u) {
        for (const unsigned char *p = (const unsigned char *)env; *p; ++p)
            if (!((*p >= 48 && *p <= 57) || (*p >= 65 && *p <= 90) ||
                  (*p >= 97 && *p <= 122) || *p == 45 || *p == 95)) return META_AUTH_CLIENT;
        return env;
    }
    return META_AUTH_CLIENT;
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
meta_post(const char *path, const char *type, const void *body, size_t size,
           json_t **response, long *status, snag_auth_pump_fn pump, void *opaque,
           char *error, size_t error_size)
{
    return snag_provider_auth_post(meta_issuer(), path, type, body, size,
                                   response, status, pump, opaque, error, error_size);
}

static int
post_json_to(const char *issuer, const char *path, json_t *request, json_t **response,
             long *status, snag_auth_pump_fn pump, void *opaque,
             char *error, size_t error_size)
{
    int rc = -1;
    struct snag_buf body = {.max = AUTH_BODY_MAX};
    if (snag_json_canonical(request, &body) == 0)
        rc = snag_provider_auth_post(issuer, path, "application/json", body.data,
                                   body.len, response, status, pump, opaque,
                                   error, error_size);
    if (body.data) memset(body.data, 0, body.len);
    snag_buf_free(&body);
    return rc;
}

static int
post_json(const char *path, json_t *request, json_t **response, long *status,
           snag_auth_pump_fn pump, void *opaque, char *error, size_t error_size)
{
    return post_json_to(auth_issuer(), path, request, response, status,
                        pump, opaque, error, error_size);
}

static json_t *
token_claims(const char *token)
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    const char *p = token ? strchr(token, '.') : NULL;
    unsigned int value = 0u, bits = 0u;
    char error[128];
    json_t *claims = NULL;

    if (!p) return NULL;
    struct snag_buf decoded = {.max = AUTH_BODY_MAX};
    for (++p; *p && *p != '.'; ++p) {
        const char *digit = strchr(alphabet, *p);
        if (!digit) goto out;
        value = (value << 6) | (unsigned int)(digit - alphabet);
        bits += 6u;
        if (bits >= 8u) {
            bits -= 8u;
            if (snag_buf_putc(&decoded, (unsigned char)(value >> bits)) < 0) goto out;
        }
    }
    if (*p == '.') claims = snag_json_load_strict(decoded.data, decoded.len, AUTH_BODY_MAX,
                                      error, sizeof(error));
out:
    if (decoded.data) memset(decoded.data, 0, decoded.len);
    snag_buf_free(&decoded);
    return claims;
}

int
snag_auth_token_response(json_t *response, struct snag_auth_tokens *tokens, char *error, size_t error_size)
{
    struct snag_auth_tokens next;
    json_t *access = NULL, *identity = NULL;
    const char *refresh = auth_string(response, "refresh_token");
    const char *account;
    uint64_t expires = 0u, lifetime = 0u;
    int rc = -1;

    snag_auth_clear(&next);
    if (snag_auth_key(&next, snag_json_string(response, "access_token"), error, error_size) < 0) goto out;
    if (!*refresh) refresh = tokens->refresh_token;
    if (!*refresh || !snag_strcpy(next.refresh_token, sizeof(next.refresh_token), refresh)) goto out;
    for (const unsigned char *p = (const unsigned char *)refresh; *p; ++p)
        if (*p < 0x21u || *p > 0x7eu) goto out;
    access = token_claims(next.credential.value);
    identity = token_claims(snag_json_string(response, "id_token"));
    account = auth_string(json_object_get(identity, "https://api.openai.com/auth"),
                              "chatgpt_account_id");
    if (!*account)
        account = auth_string(json_object_get(access, "https://api.openai.com/auth"),
                                  "chatgpt_account_id");
    if (!*account) account = tokens->credential.account_id;
    if (!*account || !snag_strcpy(next.credential.account_id, sizeof(next.credential.account_id), account))
        goto out;
    for (const unsigned char *p = (const unsigned char *)account; *p; ++p)
        if (*p < 0x21u || *p > 0x7eu) goto out;
    if (tokens->credential.account_id[0] && strcmp(tokens->credential.account_id, account) != 0) {
        snag_errorf(error, error_size, "refreshed account changed; log in again");
        goto out;
    }
    if (snag_json_integer_u64(access, "exp", &expires) == 0 && expires > 0u && expires <= INT64_MAX / 1000u)
        next.expires_at_ms = expires * 1000u;
    else if (snag_json_integer_u64(response, "expires_in", &lifetime) == 0 &&
             lifetime > 0u && lifetime <= 365u * 86400u)
        next.expires_at_ms = snag_time_ms() + lifetime * 1000u;
    else goto out;
    if (next.expires_at_ms <= snag_time_ms()) goto out;
    *tokens = next;
    rc = 0;
out: snag_auth_clear(&next);
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
    if (!request || snag_json_set_new(request, "grant_type", json_string("refresh_token")) < 0 ||
        snag_json_set_new(request, "client_id", json_string(AUTH_CLIENT)) < 0 ||
        snag_json_set_new(request, "refresh_token", json_string(tokens->refresh_token)) < 0 ||
        post_json("/oauth/token", request, &response, &status, pump, opaque, error, error_size) < 0) goto out;
    if (status < 200 || status >= 300) {
        snag_errorf(error, error_size, status == 400 || status == 401 || status == 403 ?
            "Codex login expired or revoked; run snajpagent login again" :
            "Codex token refresh temporarily failed; try again");
        goto out;
    }
    rc = snag_auth_token_response(response, tokens, error, error_size);
out: snag_auth_json_free(request);
    snag_auth_json_free(response);
    return rc;
}

static int
form_encode_value(struct snag_buf *body, const char *value)
{
    if (!value) return -1;
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
            (*p >= '0' && *p <= '9') || *p == '-' || *p == '.' ||
            *p == '_' || *p == '~') {
            if (snag_buf_putc(body, *p) < 0) return -1;
        } else if (snag_buf_printf(body, "%%%02X", (unsigned int)*p) < 0) {
            return -1;
        }
    }
    return 0;
}

static int
snag_auth_form_field(struct snag_buf *body, const char *name, const char *value)
{
    if (snag_buf_printf(body, "&%s=", name) < 0) return -1;
    return form_encode_value(body, value);
}

/* First field has no separator; the rest use snag_auth_form_field. */
static int
meta_form_first(struct snag_buf *body, const char *name, const char *value)
{
    if (snag_buf_printf(body, "%s=", name) < 0) return -1;
    return form_encode_value(body, value);
}

int
snag_auth_device(struct snag_auth_tokens *tokens, snag_auth_pump_fn pump,
                void *opaque, char *error, size_t error_size)
{
    json_t *request = NULL, *response = NULL, *code = NULL;
    char callback[4096];
    uint64_t interval = 5u, deadline;
    long status = 0;
    int rc = -1;

    snag_auth_clear(tokens);
    struct snag_buf body = {.max = AUTH_BODY_MAX};
    request = json_object();
    if (!request || snag_json_set_new(request, "client_id", json_string(AUTH_CLIENT)) < 0 ||
        post_json("/api/accounts/deviceauth/usercode", request, &response,
                   &status, pump, opaque, error, error_size) < 0) goto out;
    if (!json_object_get(response, "user_code") && json_is_string(json_object_get(response, "usercode")) &&
        snag_json_set_new(response, "user_code", json_string(auth_string(response, "usercode"))) < 0)
        goto out;
    if (status != 200 || !*auth_string(response, "device_auth_id") || !*auth_string(response, "user_code")) {
        snag_errorf(error, error_size, "device login unavailable; enable device-code login in ChatGPT security/workspace settings (HTTP %ld)", status);
        goto out;
    }
    if (snag_json_integer_u64(response, "interval", &interval) < 0) {
        const char *s = auth_string(response, "interval");
        char *end;
        unsigned long n = strtoul(s, &end, 10);
        interval = *s && !*end ? (uint64_t)n : 5u;
    }
    if (!interval || interval > 60u) interval = 5u;
    /* Print only the bounded user code, never an arbitrary server message. */
    {
        const char *user_code = snag_json_string(response, "user_code");
        if (strlen(user_code) > 64u) goto out;
        for (const unsigned char *p = (const unsigned char *)user_code; *p; ++p)
            if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                  (*p >= '0' && *p <= '9') || *p == '-')) goto out;
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
                       &status, pump, opaque, error, error_size) < 0) goto out;
        if (status == 200) break;
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
                   body.len, &response, &status, pump, opaque, error, error_size) < 0) goto out;
    if (status != 200) {
        snag_errorf(error, error_size, "device login token exchange failed (HTTP %ld)", status);
        goto out;
    }
    rc = snag_auth_token_response(response, tokens, error, error_size);
out: snag_auth_json_free(request);
    snag_auth_json_free(response);
    snag_auth_json_free(code);
    if (body.data) memset(body.data, 0, body.len);
    snag_buf_free(&body);
    if (rc < 0 && !error[0]) snag_errorf(error, error_size, "invalid device login response");
    return rc;
}

/* Meta subscription (Muse) authentication uses the standard OAuth2 device
 * authorization grant. Only the public protocol parameters also honored by
 * the Meta launcher are used here; no launcher code is shared or reused. */
static bool
meta_token_valid(const char *token)
{
    size_t len;
    if (!token || !(len = strlen(token)) || len > 512u) return false;
    for (const unsigned char *p = (const unsigned char *)token; *p; ++p)
        if (*p < 0x21u || *p > 0x7eu) return false;
    return true;
}

static bool
meta_code_valid(const char *code)
{
    size_t len;
    if (!code || !(len = strlen(code)) || len > 64u) return false;
    for (const unsigned char *p = (const unsigned char *)code; *p; ++p)
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
              (*p >= '0' && *p <= '9') || *p == '-')) return false;
    return true;
}

static bool
meta_url_valid(const char *url)
{
    size_t len;
    if (!url || !(len = strlen(url)) || len > 512u) return false;
    if (strncmp(url, "https://", 8u) != 0) return false;
    for (const unsigned char *p = (const unsigned char *)url; *p; ++p)
        if (*p <= 0x20u || *p == 0x7fu) return false;
    return true;
}

int
snag_auth_token_response_meta(json_t *response, struct snag_auth_tokens *tokens,
                              char *error, size_t error_size)
{
    struct snag_auth_tokens next;
    json_t *access = NULL, *identity = NULL;
    const char *refresh = auth_string(response, "refresh_token");
    const char *account;
    uint64_t expires = 0u, lifetime = 0u;
    int rc = -1;
    snag_auth_clear(&next);
    if (snag_auth_key(&next, snag_json_string(response, "access_token"), error, error_size) < 0)
        goto out;
    if (!*refresh) refresh = tokens->refresh_token;
    if (*refresh) {
        if (!snag_strcpy(next.refresh_token, sizeof(next.refresh_token), refresh)) goto out;
        for (const unsigned char *p = (const unsigned char *)refresh; *p; ++p)
            if (*p < 0x21u || *p > 0x7eu) goto out;
    }
    access = token_claims(next.credential.value);
    identity = token_claims(snag_json_string(response, "id_token"));
    account = auth_string(identity, "sub");
    if (!*account) account = auth_string(access, "sub");
    if (!*account) account = tokens->credential.account_id;
    if (*account) {
        if (!snag_strcpy(next.credential.account_id, sizeof(next.credential.account_id), account))
            goto out;
        for (const unsigned char *p = (const unsigned char *)account; *p; ++p)
            if (*p < 0x21u || *p > 0x7eu) goto out;
    }
    if (tokens->credential.account_id[0] && next.credential.account_id[0] &&
        strcmp(tokens->credential.account_id, next.credential.account_id) != 0) {
        snag_errorf(error, error_size, "refreshed Meta account changed; log in again");
        goto out;
    }
    if (snag_json_integer_u64(access, "exp", &expires) == 0 && expires > 0u &&
        expires <= (uint64_t)INT64_MAX / 1000u)
        next.expires_at_ms = expires * 1000u;
    else if (snag_json_integer_u64(response, "expires_in", &lifetime) == 0 &&
             lifetime > 0u && lifetime <= 365u * 86400u)
        next.expires_at_ms = snag_time_ms() + lifetime * 1000u;
    else goto out;
    if (next.expires_at_ms <= snag_time_ms()) goto out;
    *tokens = next;
    rc = 0;
out: snag_auth_clear(&next);
    json_decref(access);
    json_decref(identity);
    if (rc < 0 && !error[0])
        snag_errorf(error, error_size, "authentication response lacks valid tokens, account, or expiry");
    return rc;
}

int
snag_auth_refresh_meta(struct snag_auth_tokens *tokens, snag_auth_pump_fn pump,
                       void *opaque, char *error, size_t error_size)
{
    struct snag_buf body = {.max = AUTH_BODY_MAX};
    json_t *response = NULL;
    long status = 0;
    int rc = -1;
    if (!tokens->refresh_token[0]) {
        snag_errorf(error, error_size, "Meta login expired; run snajpagent login again");
        goto out;
    }
    if (meta_form_first(&body, "grant_type", "refresh_token") < 0 ||
        snag_auth_form_field(&body, "client_id", meta_client_id()) < 0 ||
        snag_auth_form_field(&body, "refresh_token", tokens->refresh_token) < 0 ||
        meta_post(META_TOKEN_PATH, "application/x-www-form-urlencoded", body.data,
                  body.len, &response, &status, pump, opaque, error, error_size) < 0) goto out;
    if (status < 200 || status >= 300) {
        snag_errorf(error, error_size, status == 400 || status == 401 || status == 403 ?
            "Meta login expired or revoked; run snajpagent login again" :
            "Meta token refresh temporarily failed; try again");
        goto out;
    }
    rc = snag_auth_token_response_meta(response, tokens, error, error_size);
out: snag_auth_json_free(response);
    if (body.data) memset(body.data, 0, body.len);
    snag_buf_free(&body);
    return rc;
}

int
snag_auth_device_meta(struct snag_auth_tokens *tokens, snag_auth_pump_fn pump,
                      void *opaque, char *error, size_t error_size)
{
    json_t *response = NULL, *polled = NULL;
    struct snag_buf body = {.max = AUTH_BODY_MAX};
    struct snag_buf poll = {.max = AUTH_BODY_MAX};
    char device_code[512 + 1u];
    const char *user_code, *uri, *uri_complete, *error_code;
    uint64_t interval = 5u, lifetime = 900u, deadline, next;
    unsigned int minutes;
    long status = 0;
    int rc = -1;
    snag_auth_clear(tokens);
    if (meta_form_first(&body, "client_id", meta_client_id()) < 0 ||
        meta_post(META_AUTH_PATH, "application/x-www-form-urlencoded", body.data,
                  body.len, &response, &status, pump, opaque, error, error_size) < 0) goto out;
    if (status < 200 || status >= 300 ||
        !meta_token_valid(snag_json_string(response, "device_code")) ||
        !meta_code_valid(snag_json_string(response, "user_code")) ||
        !meta_url_valid(snag_json_string(response, "verification_uri"))) {
        snag_errorf(error, error_size, "Meta sign-in could not be started (HTTP %ld)", status);
        goto out;
    }
    if (!snag_strcpy(device_code, sizeof(device_code), snag_json_string(response, "device_code")))
        goto out;
    user_code = snag_json_string(response, "user_code");
    uri = snag_json_string(response, "verification_uri");
    uri_complete = snag_json_string(response, "verification_uri_complete");
    if (snag_json_integer_u64(response, "expires_in", &lifetime) < 0) lifetime = 900u;
    if (lifetime < 60u) lifetime = 60u;
    if (lifetime > 1800u) lifetime = 1800u;
    if (snag_json_integer_u64(response, "interval", &interval) < 0) interval = 5u;
    if (!interval || interval > 60u) interval = 5u;
    (void)fprintf(stderr, "\nOpen this page to sign in:\n  %s\n", meta_url_valid(uri_complete) ? uri_complete : uri);
    if (meta_url_valid(uri_complete))
        (void)fprintf(stderr, "Confirm this code matches:\n");
    else
        (void)fprintf(stderr, "Enter this code:\n");
    (void)fprintf(stderr, "  %s\n\n", user_code);
    minutes = (unsigned int)((lifetime + 59u) / 60u);
    if (minutes == 1u)
        (void)fprintf(stderr, "Waiting for approval (link expires in 1 minute; Ctrl-C cancels)...\n");
    else
        (void)fprintf(stderr, "Waiting for approval (link expires in %u minutes; Ctrl-C cancels)...\n", minutes);
    if (meta_form_first(&poll, "grant_type", META_DEVICE_GRANT) < 0 ||
        snag_auth_form_field(&poll, "device_code", device_code) < 0 ||
        snag_auth_form_field(&poll, "client_id", meta_client_id()) < 0) goto out;
    deadline = snag_monotonic_ms() + lifetime * 1000u;
    for (;;) {
        if (snag_monotonic_ms() >= deadline) {
            snag_errorf(error, error_size, "Meta sign-in expired before approval; start login again");
            goto out;
        }
        if (meta_post(META_TOKEN_PATH, "application/x-www-form-urlencoded", poll.data,
                      poll.len, &polled, &status, pump, opaque, error, error_size) < 0) goto out;
        if (status >= 200 && status < 300) {
            if (!meta_token_valid(snag_json_string(polled, "access_token"))) {
                snag_errorf(error, error_size, "the Meta sign-in response carried no usable token");
                goto out;
            }
            rc = snag_auth_token_response_meta(polled, tokens, error, error_size);
            goto out;
        }
        error_code = auth_string(polled, "error");
        if (!strcmp(error_code, "slow_down")) {
            if (interval + 5u <= 60u) interval += 5u;
        } else if (!strcmp(error_code, "access_denied")) {
            snag_errorf(error, error_size, "Meta sign-in request was denied");
            goto out;
        } else if (!strcmp(error_code, "expired_token")) {
            snag_errorf(error, error_size, "Meta sign-in expired before approval; start login again");
            goto out;
        } else if (strcmp(error_code, "authorization_pending") != 0) {
            snag_errorf(error, error_size, "Meta sign-in failed (HTTP %ld)", status);
            goto out;
        }
        snag_auth_json_free(polled);
        polled = NULL;
        next = snag_monotonic_ms() + interval * 1000u;
        while (snag_monotonic_ms() < next) {
            if (snag_monotonic_ms() >= deadline) {
                snag_errorf(error, error_size, "Meta sign-in expired before approval; start login again");
                goto out;
            }
            if (pump ? pump(opaque, 100u) != 0 : snag_sleep_ms(100u) < 0) {
                errno = ECANCELED;
                snag_errorf(error, error_size, "Meta sign-in cancelled");
                goto out;
            }
        }
    }
out: snag_auth_json_free(response);
    snag_auth_json_free(polled);
    snag_buf_free(&body);
    if (poll.data) memset(poll.data, 0, poll.len);
    snag_buf_free(&poll);
    snag_secret_clear(device_code, sizeof(device_code));
    if (rc < 0 && !error[0]) snag_errorf(error, error_size, "invalid Meta sign-in response");
    return rc;
}

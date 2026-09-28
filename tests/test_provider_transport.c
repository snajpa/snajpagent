/* SPDX-License-Identifier: GPL-2.0-only */
#include "checked_json.h"
#include "base.h"
#include "app_internal.h"
#include "config.h"
#include "credential.h"
#include "json.h"
#include "model_cache.h"
#include "provider.h"
#include "tools.h"
#include "convert.h"
#include "media.h"
#include "av.h"
#include "snajpagent.h"
#include "turn.h"
#include "voice.h"
#include "audio_device.h"
#include "voice_rtc.h"
#if SNAJPAGENT_AUDIO_DEVICE
#include <rtc/rtc.h>
#endif

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>

#if SNAJPAGENT_AUDIO_DEVICE && defined(MA_NO_RUNTIME_LINKING) && defined(MA_ENABLE_ALSA)
#include <alsa/asoundlib.h>

/* Read configuration defaults only; never enumerate or open an audio device. */
static void test_static_alsa_config(void)
{
    const char *dirs[] = {NULL, "/fixture/alsa"};
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); ++i) {
        pid_t child = fork();
        assert(child >= 0);
        if (!child) {
            assert((dirs[i] ? setenv("ALSA_CONFIG_DIR", dirs[i], 1) : unsetenv("ALSA_CONFIG_DIR")) == 0);
            assert(!strcmp(snd_config_topdir(), dirs[i] ? dirs[i] : "/usr/share/alsa"));
            _exit(0);
        }
        int status; pid_t done;
        do { done = waitpid(child, &status, 0); } while (done < 0 && errno == EINTR);
        assert(done == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
}
#endif

#define REQUEST_MAX (64u * 1024u)
#define BODY_MAX (32u * 1024u)

/* Inherited only by the voice-request fixture child, never a product control. */
static int voice_request_ready_fd = -1;

struct local_server {
    int fd;
    pid_t pid;
    unsigned short port;
    char endpoint[128];
};

struct http_request {
    char method[8];
    char path[128];
    char headers[REQUEST_MAX];
    char body[BODY_MAX];
    size_t body_len;
};

struct emitted_text {
    struct snag_buf text;
    unsigned int calls;
};

enum model_fixture {
    MODEL_OPENAI,
    MODEL_CODEX_MALFORMED,
    MODEL_CODEX_LOOKALIKE,
    MODEL_CODEX_FAILURE,
    MODEL_LIMIT_CONFLICT,
    MODEL_CREATE_HTTP_FAILURE,
    MODEL_CREATE_SSE_FAILURE,
    MODEL_CREATE_TYPELESS,
    MODEL_VOICE_REQUEST,
    MODEL_VOICE_REQUEST_WAIT,
    MODEL_VOICE_QUEUE,
    MODEL_VOICE_READ,
    MODEL_OPENROUTER_SEARCH,
    MODEL_CREATE_RETRY,
    MODEL_COUNT_404,
    MODEL_COUNT_405,
    MODEL_COUNT_501,
    MODEL_COUNT_401,
    MODEL_COUNT_403,
    MODEL_COUNT_OK,
    MODEL_COUNT_MODEL_404,
    MODEL_COUNT_OVERFLOW,
    MODEL_COMPACT_404,
    MODEL_COMPACT_502,
    MODEL_COMPACT_OK,
    MODEL_COMPACT_403,
    MODEL_AUDIO_LISTEN,
    MODEL_AUDIO_TRANSCRIBE,
    MODEL_AUDIO_SPEAK,
    MODEL_AUDIO_FAILURE,
    MODEL_NATIVE_TRANSCRIBE,
    MODEL_NATIVE_CALL,
    MODEL_NATIVE_CALL_NO_ID,
    MODEL_NATIVE_CALL_DENIED,
    MODEL_AUTH_DEVICE,
    MODEL_AUTH_CANCEL,
    MODEL_AUTH_EXPIRED,
    MODEL_AUTH_REFRESH,
    MODEL_AUTH_REFRESH_FAILURE,
    MODEL_AUTH_401,
    MODEL_AUTH_401_TWICE,
    MODEL_META_DEVICE,
    MODEL_META_DENIED,
    MODEL_META_EXPIRED,
    MODEL_META_REFRESH,
    MODEL_META_REFRESH_FAILURE
};

static bool authentication_fixture;
/* When set, the transport fixture requires this exact header line. */
static const char *expected_session_header;
/* When set, the transport fixture requires this exact Authorization line. */
static const char *expected_authorization_header = "Authorization: Bearer transport-secret";
/* Scoped off for fixtures whose requests deliberately carry no OpenRouter headers. */
static bool fixture_requires_openrouter = true;

static const struct retry_case {
    const char *prefix;
    const char *body;
    unsigned int status;
    unsigned int failures;
    unsigned int retries;
    bool truncated;
    const char *diagnostic; /* NULL: failure(s), then success. */
    const char *emitted;
} *retry_case;

static void
write_all_or_die(int fd, const char *data, size_t len)
{
    while (len) {
        ssize_t n = write(fd, data, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            _exit(90);
        }
        if (n == 0) _exit(91);
        data += (size_t)n;
        len -= (size_t)n;
    }
}

static void
server_fail(const char *message)
{
    (void)write(STDERR_FILENO, message, strlen(message));
    (void)write(STDERR_FILENO, "\n", 1u);
    _exit(92);
}

static bool
header_contains(const char *headers, const char *needle)
{
    return strstr(headers, needle) != NULL;
}

static long
content_length(const char *headers)
{
    const char *p = strstr(headers, "Content-Length:");
    long value = 0;

    if (!p) p = strstr(headers, "content-length:");
    if (!p) return -1;
    p = strchr(p, ':');
    if (!p) return -1;
        ++p;
    while (*p == ' ' || *p == '\t') ++p;
    while (*p >= '0' && *p <= '9') {
        value = value * 10 + (long)(*p - '0');
        ++p;
    }
    return value;
}

static size_t
find_header_end(const char *buffer, size_t len)
{
    for (size_t i = 0; i + 3u < len; ++i)
        if (buffer[i] == '\r' && buffer[i + 1u] == '\n' && buffer[i + 2u] == '\r' && buffer[i + 3u] == '\n')
            return i + 4u;
    for (size_t i = 0; i + 1u < len; ++i)
        if (buffer[i] == '\n' && buffer[i + 1u] == '\n') return i + 2u;
    return 0u;
}

static void
read_request(int fd, struct http_request *request)
{
    char buffer[REQUEST_MAX];
    size_t used = 0u;
    size_t header_end = 0u;
    long cl;
    int matched;

    memset(request, 0, sizeof(*request));
    while (!header_end) {
        ssize_t n;
        if (used == sizeof(buffer)) server_fail("request headers exceeded test bound");
        n = read(fd, buffer + used, sizeof(buffer) - used);
        if (n < 0) {
            if (errno == EINTR) continue;
            server_fail("request read failed");
        }
        if (n == 0) server_fail("request closed before headers");
        used += (size_t)n;
        header_end = find_header_end(buffer, used);
    }
    if (header_end >= sizeof(request->headers)) server_fail("retained headers exceeded test bound");
    memcpy(request->headers, buffer, header_end);
    request->headers[header_end] = '\0';
    matched = sscanf(request->headers, "%7s %127s HTTP/1", request->method, request->path);
    if (matched != 2) server_fail("unexpected request line");
    if (!authentication_fixture && expected_authorization_header &&
        !header_contains(request->headers, expected_authorization_header))
        server_fail("authorization header missing or unredacted differently");
    if (!authentication_fixture && fixture_requires_openrouter && !header_contains(request->headers,
                         "HTTP-Referer: https://github.com/snajpa/snajpagent"))
        server_fail("OpenRouter referer header missing");
    if (!authentication_fixture && fixture_requires_openrouter &&
        !header_contains(request->headers, "X-OpenRouter-Title: snajpagent"))
        server_fail("OpenRouter title header missing");
    if (!header_contains(request->headers, "User-Agent: " SNAJPAGENT_NAME "/"
                         SNAJPAGENT_VERSION)) server_fail("product user agent missing or stale");
    if (expected_session_header &&
        !header_contains(request->headers, expected_session_header))
        server_fail("session identity header missing or altered");
    cl = content_length(request->headers);
    if (strcmp(request->method, "GET") == 0 && cl < 0) cl = 0;
    if (cl < 0 || cl > (long)(sizeof(request->body) - 1u)) server_fail("invalid content length");
    request->body_len = (size_t)cl;
    if (used - header_end > request->body_len) server_fail("request body overflowed expected length");
    memcpy(request->body, buffer + header_end, used - header_end);
    while (used - header_end < request->body_len) {
        ssize_t n = read(fd, request->body + (used - header_end), request->body_len - (used - header_end));
        if (n < 0) {
            if (errno == EINTR) continue;
            server_fail("body read failed");
        }
        if (n == 0) server_fail("request closed before body");
        used += (size_t)n;
    }
    request->body[request->body_len] = '\0';
}

static void
send_response(int fd, unsigned int status, const char *content_type, const char *body)
{
    char header[256];
    size_t len = strlen(body);
    int n = snprintf(header, sizeof(header), "HTTP/1.1 %u Test\r\n"
                     "Content-Type: %s\r\n" "Content-Length: %llu\r\n"
                     "Connection: close\r\n\r\n", status, content_type, (unsigned long long)len);
    if (n <= 0 || (size_t)n >= sizeof(header)) server_fail("response header build failed");
    write_all_or_die(fd, header, (size_t)n);
    write_all_or_die(fd, body, len);
}

static void
serve_one(int listen_fd, unsigned int status, const char *method, const char *path, const char *marker,
          const char *content_type, const char *body)
{
    struct http_request request;
    int fd;

    fd = accept(listen_fd, NULL, NULL);
    if (fd < 0) server_fail("accept failed");
    read_request(fd, &request);
    if (strcmp(request.method, method) != 0) server_fail("unexpected provider HTTP method");
    if (strcmp(request.path, path) != 0) server_fail("unexpected provider endpoint path");
    if (marker && !strstr(request.body, marker)) server_fail("request body marker missing");
    send_response(fd, status, content_type, body);
    if (close(fd) < 0) server_fail("close accepted socket failed");
}

static void
fixture_stop(int signal_number)
{
    (void)signal_number;
    _exit(0);
}

static void
meta_auth_server_child(int listen_fd, enum model_fixture fixture)
{
    struct http_request request;
    int fd;

    authentication_fixture = true;
    (void)alarm(120u);
    if (fixture == MODEL_META_REFRESH || fixture == MODEL_META_REFRESH_FAILURE) {
        fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) server_fail("meta accept failed");
        read_request(fd, &request);
        if (strcmp(request.method, "POST") || strcmp(request.path, "/oidc/device/token/") ||
            !strstr(request.body, "grant_type=refresh_token") ||
            !strstr(request.body, "meta-old-refresh")) server_fail("wrong Meta refresh request");
        if (fixture == MODEL_META_REFRESH_FAILURE)
            send_response(fd, 401u, "application/json", "{\"error\":\"revoked\"}");
        else
            send_response(fd, 200u, "application/json",
                          "{\"access_token\":\"meta-new-access\",\"refresh_token\":\"meta-new-refresh\",\"expires_in\":3600}");
        if (close(fd) < 0) server_fail("close Meta refresh socket failed");
        /* The client falls back to a device login when the refresh does not
         * succeed; serve those legs too so the test never waits out the link. */
        for (;;) {
            struct pollfd waiting = {.fd = listen_fd, .events = POLLIN};
            if (poll(&waiting, 1, 2000) <= 0) _exit(0);
            fd = accept(listen_fd, NULL, NULL);
            if (fd < 0) server_fail("meta accept failed");
            read_request(fd, &request);
            if (strstr(request.path, "/oidc/device/authorization/")) {
                send_response(fd, 200u, "application/json",
                              "{\"device_code\":\"meta-device\",\"user_code\":\"ABCD-1234\","
                              "\"verification_uri\":\"https://auth.meta.com/oidc/device/\","
                              "\"verification_uri_complete\":\"https://auth.meta.com/oidc/device/?code=ABCD-1234\","
                              "\"expires_in\":60,\"interval\":1}");
            } else if (strstr(request.path, "/oidc/device/token/")) {
                send_response(fd, 200u, "application/json",
                              "{\"access_token\":\"meta-access\",\"refresh_token\":\"meta-refresh\",\"expires_in\":3600}");
            } else {
                send_response(fd, 404u, "application/json", "{}");
            }
            if (close(fd) < 0) server_fail("close Meta socket failed");
        }
    }
fd = accept(listen_fd, NULL, NULL);
    if (fd < 0) server_fail("meta accept failed");
    read_request(fd, &request);
    if (strcmp(request.method, "POST") || strcmp(request.path, "/oidc/device/authorization/") ||
        !strstr(request.body, "client_id=")) server_fail("wrong Meta authorization request");
    send_response(fd, 200u, "application/json",
                  "{\"device_code\":\"meta-device\",\"user_code\":\"ABCD-1234\","
                  "\"verification_uri\":\"https://auth.meta.com/oidc/device/\","
                  "\"verification_uri_complete\":\"https://auth.meta.com/oidc/device/?code=ABCD-1234\","
                  "\"expires_in\":60,\"interval\":1}");
    if (close(fd) < 0) server_fail("close Meta authorization socket failed");
    if (fixture == MODEL_META_DENIED || fixture == MODEL_META_EXPIRED) {
        fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) server_fail("meta accept failed");
        read_request(fd, &request);
        if (strcmp(request.method, "POST") || strcmp(request.path, "/oidc/device/token/") ||
            !strstr(request.body, "device_code=meta-device"))
            server_fail("wrong Meta token request");
        if (fixture == MODEL_META_DENIED)
            send_response(fd, 400u, "application/json", "{\"error\":\"access_denied\"}");
        else
            send_response(fd, 400u, "application/json", "{\"error\":\"expired_token\"}");
        if (close(fd) < 0) server_fail("close Meta token socket failed");
        _exit(0);
    }
    /* Token polling repeats until the client stops: a loaded host may poll more
     * times than the two-step minimum, and a fixture that exited after a fixed
     * count would leave the client waiting out its whole link lifetime. The
     * grant repeats; the fixture exits after it or after a 30 s quiet grace. */
    for (;;) {
        struct pollfd waiting = {.fd = listen_fd, .events = POLLIN};
        if (poll(&waiting, 1, 30000) <= 0) _exit(0);
        fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) server_fail("meta accept failed");
        read_request(fd, &request);
        if (strcmp(request.method, "POST") || strcmp(request.path, "/oidc/device/token/") ||
            !strstr(request.body, "device_code=meta-device")) {
            /* Never die mid-flow: an unexpected request gets an error and the
             * loop keeps serving, so the client is never left waiting out its
             * whole link lifetime after the fixture vanished. */
            send_response(fd, 400u, "application/json", "{\"error\":\"invalid_request\"}");
            if (close(fd) < 0) server_fail("close Meta token socket failed");
            continue;
        }
        /* Grant on every poll and keep serving: exiting after the first grant
         * left a client that polled again with a dead listener, which then
         * waited out its whole link lifetime. The test stops this fixture
         * when its own client has finished. */
        send_response(fd, 200u, "application/json",
                      "{\"access_token\":\"meta-access\",\"refresh_token\":\"meta-refresh\",\"expires_in\":3600}");
        if (close(fd) < 0) server_fail("close Meta token socket failed");
        /* Keep serving until the test stops us. Exiting on a short grace after
         * the grant used to leave a client that polled again holding a
         * connection the dead fixture never accepted; the suite then parked
         * forever, because the test keeps its copy of the listener open. */
    }
}

static void
auth_server_child(int listen_fd, enum model_fixture fixture)
{
    if (fixture >= MODEL_META_DEVICE) {
        meta_auth_server_child(listen_fd, fixture);
        return;
    }
    static const char tokens[] =
        "{\"access_token\":\"new-access\",\"refresh_token\":\"new-refresh\",\"expires_in\":3600,"
        "\"id_token\":\"e30.eyJodHRwczovL2FwaS5vcGVuYWkuY29tL2F1dGgiOnsiY2hhdGdwdF9hY2NvdW50X2lkIjoiYWNjdC10ZXN0In19.sig\"}";
    unsigned int count = fixture == MODEL_AUTH_DEVICE ? 4u :
        fixture == MODEL_AUTH_CANCEL || fixture == MODEL_AUTH_EXPIRED ? 2u :
        fixture >= MODEL_AUTH_401 ? 3u : 1u;

    authentication_fixture = true;
    (void)alarm(120u);
    if (fixture <= MODEL_AUTH_EXPIRED) {
        struct http_request request;
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) server_fail("auth accept failed");
        read_request(fd, &request);
        if (strcmp(request.path, "/api/accounts/deviceauth/usercode"))
            server_fail("incorrect device flow path");
        if (!strstr(request.body, "app_EMoamEEZ73f0CkXaXp7hrann"))
            server_fail("missing device client identifier");
        send_response(fd, 200u, "application/json",
                      "{\"device_auth_id\":\"device-id\",\"user_code\":\"CODE-1234\",\"interval\":\"1\"}");
        (void)close(fd);
        /* Token polling repeats until the client stops: a loaded host may poll
         * more times than the minimum, and a fixture that exited after a fixed
         * count would leave the client waiting out its whole link lifetime. */
        for (unsigned int polls = 0u;;) {
            struct pollfd waiting = {.fd = listen_fd, .events = POLLIN};
            const char *body = tokens;
            unsigned int status = 200u;
            if (poll(&waiting, 1, 30000) <= 0) _exit(0);
            fd = accept(listen_fd, NULL, NULL);
            if (fd < 0) server_fail("auth accept failed");
            read_request(fd, &request);
            if (strcmp(request.path, "/api/accounts/deviceauth/token") == 0) {
                ++polls;
                (void)polls;
                /* The success case grants on the first poll so it cannot depend
                 * on the client's poll interval; the expired mode always
                 * answers 410 and the cancel mode keeps answering pending so
                 * the client keeps polling until its own cancel fires. */
                if (fixture == MODEL_AUTH_EXPIRED) {
                    status = 410u;
                    body = "{}";
                } else if (fixture == MODEL_AUTH_CANCEL) {
                    status = 403u;
                    body = "{}";
                } else {
                    body = "{\"authorization_code\":\"auth-code\",\"code_verifier\":\"verifier\",\"code_challenge\":\"challenge\"}";
                }
            } else if (strcmp(request.path, "/oauth/token") == 0) {
                if (!strstr(request.body, "grant_type=authorization_code") ||
                    !strstr(request.body, "code_verifier=verifier"))
                    server_fail("missing PKCE code exchange");
                send_response(fd, status, "application/json", body);
                (void)close(fd);
                _exit(0);
            } else {
                /* Tolerant like the Meta fixture: answer and keep serving. */
                send_response(fd, 404u, "application/json", "{}");
                (void)close(fd);
                continue;
            }
            send_response(fd, status, "application/json", body);
            (void)close(fd);
        }
    }
    for (unsigned int i = 0; fixture > MODEL_AUTH_EXPIRED && i < count; ++i) {
        struct http_request request;
        const char *body = tokens;
        unsigned int status = 200u;
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) server_fail("auth accept failed");
        read_request(fd, &request);
        if (fixture >= MODEL_AUTH_401 && i != 1u) {
            if (strcmp(request.path, "/models?client_version=0.146.0") ||
                !strstr(request.headers, "ChatGPT-Account-Id: acct-test") ||
                !strstr(request.headers, i == 0u ? "Bearer old-access" : "Bearer new-access"))
                server_fail("wrong Codex account or access header");
            if (i == 0u || fixture == MODEL_AUTH_401_TWICE) {
                status = 401u;
                body = "{\"error\":{\"message\":\"not authorized\"}}";
            } else {
                body = "{\"models\":[{\"slug\":\"gpt-5.6-luna\",\"visibility\":\"list\",\"priority\":0,\"default_reasoning_level\":\"high\",\"supported_reasoning_levels\":[{\"effort\":\"high\"}]}]}";
            }
        } else {
            if (strcmp(request.path, "/oauth/token") ||
                !strstr(request.body, "\"grant_type\":\"refresh_token\"") ||
                !strstr(request.body, "old-refresh")) server_fail("wrong refresh request");
            if (fixture == MODEL_AUTH_REFRESH_FAILURE) {
                status = 401u;
                body = "{\"error\":\"private-refresh-server-detail\"}";
            }
        }
        send_response(fd, status, "application/json", body);
        (void)close(fd);
    }
    _exit(0);
}

static unsigned char fixture_wav[1004];
static const unsigned char *audio_expected = fixture_wav;
static size_t audio_expected_len = sizeof(fixture_wav);

static void
make_fixture_wav(void)
{
    memcpy(fixture_wav, "RIFF", 4u);
    fixture_wav[4] = 0xe4; fixture_wav[5] = 3; /* 996 bytes after RIFF header. */
    memcpy(fixture_wav + 8u, "WAVEfmt ", 8u);
    fixture_wav[16] = 16; fixture_wav[20] = 1; fixture_wav[22] = 2;
    fixture_wav[24] = 0xc0; fixture_wav[25] = 0x5d; /* 24000 Hz. */
    fixture_wav[28] = 0; fixture_wav[29] = 0x77; fixture_wav[30] = 1; /* 96000 B/s. */
    fixture_wav[32] = 4; fixture_wav[34] = 16;
    memcpy(fixture_wav + 36u, "data", 4u);
    fixture_wav[40] = 0xc0; fixture_wav[41] = 3;
}

static void
audio_server_child(int listen_fd, enum model_fixture mode)
{
    struct http_request req;
    int fd = accept(listen_fd, NULL, NULL);
    if (fd < 0) server_fail("audio accept failed");
    read_request(fd, &req);
    if (strcmp(req.method, "POST")) server_fail("audio method mismatch");
    if (mode == MODEL_AUDIO_TRANSCRIBE) {
        if (strcmp(req.path, "/v1/audio/transcriptions") ||
            !strstr(req.headers, "Content-Type: multipart/form-data; boundary=") ||
            !strstr(req.body, "name=\"file\"") || !strstr(req.body, "filename=\"segment.wav\""))
            server_fail("audio multipart path/header mismatch");
        /* The WAV contains NUL bytes, so inspect the complete binary body. */
        bool found = false, model = false;
        for (size_t i = 0; i < req.body_len; ++i) {
            if (i + sizeof(fixture_wav) <= req.body_len && !memcmp(req.body + i, fixture_wav, sizeof(fixture_wav))) found = true;
            if (i + 13u <= req.body_len && !memcmp(req.body + i, "audio-fixture", 13u)) model = true;
        }
        if (!found || !model) server_fail("multipart WAV/model missing");
        send_response(fd,200u,"application/json", "{\"text\":\"fixture transcript\",\"usage\":{\"type\":\"duration\",\"seconds\":0.01}}");
    } else {
        json_t *body = json_loadb(req.body, req.body_len, JSON_REJECT_DUPLICATES, NULL);
        const char *model = snag_json_string(body, "model");
        if (!body || !model || strcmp(model, "audio-fixture") || json_object_get(body, "tools"))
            server_fail("audio request must be one model without tools");
        if (mode == MODEL_AUDIO_SPEAK) {
            if (strcmp(req.path, "/v1/audio/speech") || strcmp(snag_json_string(body, "response_format"), "wav"))
                server_fail("speech path/format mismatch");
            char header[160];
            int n = snprintf(header, sizeof(header), "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", sizeof(fixture_wav));
            write_all_or_die(fd, header, (size_t)n);
            write_all_or_die(fd, (char *)fixture_wav, sizeof(fixture_wav));
        } else {
            if (strcmp(req.path, "/v1/chat/completions")) server_fail("audio chat path mismatch");
            json_t *messages = json_object_get(body, "messages");
            if (!messages) server_fail("audio chat messages missing");
            if (messages) {
                json_t *user = json_array_get(messages, 1u);
                json_t *audio = json_object_get(json_array_get(json_object_get(user, "content"), 1u), "input_audio");
                struct snag_buf decoded;
                snag_buf_init(&decoded, 32768u);
                const char *base64 = snag_json_string(audio, "data");
                if (json_array_size(messages) != 2u || !base64 ||
                    snag_base64_decode(&decoded, base64) < 0 || decoded.len != audio_expected_len ||
                    memcmp(decoded.data, audio_expected, audio_expected_len)) server_fail("audio chat lost WAV data");
                snag_buf_free(&decoded);
            }
            send_response(fd,mode == MODEL_AUDIO_FAILURE ? 503u : 200u,"application/json",
                mode == MODEL_AUDIO_FAILURE ? "{\"error\":\"private audio diagnostic\"}" :
                "{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"role\":\"assistant\",\"content\":\"fixture sound answer\"}}],"
                "\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":4,\"total_tokens\":14}}");
        }
        json_decref(body);
    }
    (void)close(fd);
    _exit(0);
}

static void
server_child(int listen_fd, enum model_fixture models, bool transport)
{
    (void)signal(SIGTERM, fixture_stop);
    if (models>=MODEL_NATIVE_TRANSCRIBE && models<=MODEL_NATIVE_CALL_DENIED) {
        struct http_request request;int fd=accept(listen_fd,NULL,NULL);
        if (fd<0)server_fail("native voice accept failed");
        read_request(fd,&request);
        if (strcmp(request.method,"POST"))server_fail("native voice method");
        if (!strstr(request.headers, expected_authorization_header)) {
            server_fail("native voice authorization");
        }
        if (models==MODEL_NATIVE_TRANSCRIBE) {
            if (strcmp(request.path,"/backend-api/transcribe") ||
                !strstr(request.body,"name=\"file\""))
                server_fail("native transcription endpoint");
            send_response(fd,200u,"application/json","{\"text\":\"native transcript\"}");
        } else {
            if (strcmp(request.path,
                "/backend-api/codex/realtime/calls?intent=quicksilver&architecture=avas") ||
                !strstr(request.headers,"openai-alpha: quicksilver=v2") ||
                !strstr(request.body,"\"sdp\"") ||
                !strstr(request.body,"\"session\""))server_fail("native call request");
            if (models==MODEL_NATIVE_CALL_DENIED)
                send_response(fd,403u,"application/json",
                    "{\"error\":{\"message\":\"access denied\"}}");
            else {
                static const char answer[]="v=0\r\ns=native fixture\r\n";
                char header[256];int n=snprintf(header,sizeof(header),
                    "HTTP/1.1 201 Created\r\nContent-Type: application/sdp\r\n"
                    "Content-Length: %zu\r\n%sConnection: close\r\n\r\n",
                    sizeof(answer)-1u,
                    models==MODEL_NATIVE_CALL?"Location: /v1/realtime/calls/rtc_native\r\n":"");
                write_all_or_die(fd,header,(size_t)n);write_all_or_die(fd,answer,sizeof(answer)-1u);
            }
        }
        close(fd);close(listen_fd);_exit(0);
    }
    if (models >= MODEL_AUDIO_LISTEN && models <= MODEL_AUDIO_FAILURE)
        audio_server_child(listen_fd, models);
    if (models >= MODEL_AUTH_DEVICE)
        auth_server_child(listen_fd, models);
    if (models == MODEL_COMPACT_404 || models == MODEL_COMPACT_403 || models == MODEL_COMPACT_502 ||
        models == MODEL_COMPACT_OK) {
        /* The native compaction probe authenticates with its own credential and
         * sends no OpenRouter headers, so those transport checks do not apply. */
        expected_authorization_header = NULL;
        fixture_requires_openrouter = false;
        /* Keep answering until the test stops us: the client retries a failed
         * compact request, and a fixture that exited after the first answer
         * left the retry in the queue of a listener nobody accepts on, which
         * parked the suite in poll until it was killed. */
        for (;;) {
            struct http_request request;
            int fd = accept(listen_fd, NULL, NULL);
            if (fd < 0) server_fail("compact accept failed");
            read_request(fd, &request);
            if (strcmp(request.method, "POST") ||
                (strcmp(request.path, "/responses/compact") && strcmp(request.path, "/v1/responses/compact")))
                server_fail("invalid native compact path");
            send_response(fd, models == MODEL_COMPACT_OK ? 200u :
                              models == MODEL_COMPACT_404 ? 404u :
                              models == MODEL_COMPACT_502 ? 502u : 403u,
                          "application/json", models == MODEL_COMPACT_OK ?
                              "{\"object\":\"response.compaction\",\"output\":[]}" : "{\"detail\":\"Not Found\"}");
            (void)close(fd);
        }
    }
    if (models == MODEL_CREATE_TYPELESS) {
        serve_one(listen_fd, 200u, "POST", "/v1/responses", NULL, "text/event-stream",
                  "data: {\"kind\":\"private-value\"}\n\n");
        _exit(0);
    }
    static const char create_sse[] = "event: response.created\n"
        "data: {\"type\":\"response.created\",\"response\":{\"id\":\"resp_transport\",\"status\":\"in_progress\",\"output\":[]}}\n\n"
        "event: response.output_item.added\n"
        "data: {\"type\":\"response.output_item.added\",\"output_index\":0,\"item\":{\"id\":\"rs_transport\",\"type\":\"reasoning\",\"content\":[],\"summary\":[]}}\n\n"
        "event: response.output_item.done\n"
        "data: {\"type\":\"response.output_item.done\",\"output_index\":0,\"item\":{\"id\":\"rs_transport\",\"type\":\"reasoning\",\"content\":[],\"summary\":[]}}\n\n"
        "event: response.output_item.added\n"
        "data: {\"type\":\"response.output_item.added\",\"output_index\":1,\"item\":{\"id\":\"msg_transport\",\"type\":\"message\",\"status\":\"in_progress\",\"role\":\"assistant\",\"phase\":\"final_answer\",\"content\":[]}}\n\n"
        "event: response.content_part.added\n"
        "data: {\"type\":\"response.content_part.added\",\"item_id\":\"msg_transport\",\"output_index\":1,\"content_index\":0,\"part\":{\"type\":\"output_text\",\"text\":\"\",\"annotations\":[]}}\n\n"
        "event: response.output_text.delta\n"
        "data: {\"type\":\"response.output_text.delta\",\"item_id\":\"msg_transport\",\"output_index\":1,\"content_index\":0,\"delta\":\"local transport\"}\n\n"
        "event: response.output_text.done\n"
        "data: {\"type\":\"response.output_text.done\",\"item_id\":\"msg_transport\",\"output_index\":1,\"content_index\":0,\"text\":\"local transport\"}\n\n"
        "event: response.output_item.done\n"
        "data: {\"type\":\"response.output_item.done\",\"output_index\":1,\"item\":{\"id\":\"msg_transport\",\"type\":\"message\",\"status\":\"completed\",\"role\":\"assistant\",\"phase\":\"final_answer\",\"content\":[{\"type\":\"output_text\",\"text\":\"local transport\",\"annotations\":[]}]}}\n\n"
        "event: response.completed\n"
        "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_transport\",\"status\":\"completed\",\"usage\":{\"input_tokens\":7,\"output_tokens\":2,\"total_tokens\":9},\"output\":[]}}\n\n";

    if (models == MODEL_VOICE_QUEUE || models == MODEL_VOICE_READ) {
        unsigned int count = models == MODEL_VOICE_QUEUE ? 4u : 3u;
        for (unsigned int i = 0; i < count; ++i) {
            int fd = accept(listen_fd, NULL, NULL);
            struct http_request request;
            if (fd < 0) server_fail("voice interface accept failed");
            read_request(fd, &request);
            if (strcmp(request.method, "POST") || strcmp(request.path, "/v1/responses") ||
                !strstr(request.body, "inspect_session") ||
                !strstr(request.body, "get_cwd")) server_fail("missing voice capabilities");
            bool final = models == MODEL_VOICE_QUEUE ? i % 2u : i == 2u;
            if (final) {
                if (!strstr(request.body, "function_call_output")) {
                    server_fail("missing voice tool result");
                }
                if (models == MODEL_VOICE_READ && !strstr(request.body, "unavailable")) {
                    server_fail("write tool was not refused");
                }
                send_response(fd, 200u, "text/event-stream", create_sse);
            } else {
                const char *name = models == MODEL_VOICE_QUEUE ? "submit_input" :
                    i ? "write_file" : "get_cwd";
                const char *arguments = models == MODEL_VOICE_QUEUE ?
                    "{\"target\":\"queue\",\"text\":\"model interpretation\"}" :
                    i ? "{\"path\":\"AGENTS.md\",\"content\":\"must not write\"}" : "{}";
                json_t *event = json_pack("{s:s,s:i,s:{s:s,s:s,s:s,s:s,s:s,s:s}}",
                    "type", "response.output_item.done", "output_index", 0,
                    "item", "type", "function_call", "id", "voice_function",
                    "call_id", i ? "voice_call_next" : "voice_call_first",
                    "name", name, "arguments", arguments, "status", "completed");
                char *wire = json_dumps(event, JSON_COMPACT);
                char response[BODY_MAX];
                if (!wire || snprintf(response, sizeof(response),
                        "data: {\"type\":\"response.created\","
                        "\"response\":{\"id\":\"voice_request\","
                        "\"status\":\"in_progress\",\"output\":[]}}\n\n"
                        "data: %s\n\n"
                        "data: {\"type\":\"response.completed\","
                        "\"response\":{\"id\":\"voice_request\","
                        "\"status\":\"completed\",\"output\":[]}}\n\n", wire) <= 0) {
                    server_fail("voice fixture encoding failed");
                }
                free(wire);
                json_decref(event);
                send_response(fd, 200u, "text/event-stream", response);
            }
            close(fd);
        }
        _exit(0);
    }
    if (models == MODEL_VOICE_REQUEST || models == MODEL_VOICE_REQUEST_WAIT) {
        struct http_request request;
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) server_fail("voice interface accept failed");
        read_request(fd, &request);
        if (strcmp(request.method, "POST") || strcmp(request.path, "/v1/responses") ||
            !strstr(request.body, "get_cwd")) server_fail("invalid voice interface request");
        if (voice_request_ready_fd >= 0) {
            if (write(voice_request_ready_fd, "R", 1u) != 1) {
                server_fail("cannot acknowledge voice request");
            }
            close(voice_request_ready_fd);
        }
        snag_sleep_ms(models == MODEL_VOICE_REQUEST_WAIT ? 2000u : 250u);
        send_response(fd, 200u, "text/event-stream", create_sse);
        close(fd);
        _exit(0);
    }
    if (models == MODEL_OPENROUTER_SEARCH) {
        serve_one(listen_fd, 200u, "POST", "/v1/responses", "openrouter:web_search", "text/event-stream",
                  "event: response.created\n"
                  "data: {\"type\":\"response.created\",\"response\":{\"id\":\"resp_search\",\"status\":\"in_progress\",\"output\":[]}}\n\n"
                  "event: response.output_item.added\n"
                  "data: {\"type\":\"response.output_item.added\",\"output_index\":0,\"item\":{\"type\":\"openrouter:web_search\",\"id\":\"ws_tmp_abc123\",\"status\":\"in_progress\"}}\n\n"
                  "event: response.output_item.done\n"
                  "data: {\"type\":\"response.output_item.done\",\"output_index\":0,\"item\":{\"type\":\"openrouter:web_search\",\"id\":\"ws_tmp_abc123\",\"status\":\"completed\",\"action\":{\"type\":\"search\",\"query\":\"example domains\",\"sources\":[{\"type\":\"url\",\"url\":\"https://example.com\"}]}}}\n\n"
                  "event: response.output_item.done\n"
                  "data: {\"type\":\"response.output_item.done\",\"output_index\":1,\"item\":{\"type\":\"message\",\"id\":\"msg_search\",\"role\":\"assistant\",\"status\":\"completed\",\"content\":[{\"type\":\"output_text\",\"text\":\"Found https://example.com\",\"annotations\":[{\"type\":\"url_citation\",\"url\":\"https://example.com\",\"title\":\"Example\",\"start_index\":6,\"end_index\":25}]}]}}\n\n"
                  "event: response.output_item.done\n"
                  "data: {\"type\":\"response.output_item.done\",\"output_index\":2,\"item\":{\"type\":\"function_call\",\"id\":\"fc_after_search\",\"call_id\":\"call_after_search\",\"name\":\"read_file\",\"arguments\":\"{\\\"path\\\":\\\"README.md\\\",\\\"start_line\\\":1,\\\"end_line\\\":1}\",\"status\":\"completed\"}}\n\n"
                  "event: response.completed\n"
                  "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_search\",\"status\":\"completed\",\"output\":[]}}\n\n"
                  "data: [DONE]\n\n");
        serve_one(listen_fd, 200u, "POST", "/v1/responses", "function_call_output",
                  "text/event-stream", create_sse);
        _exit(0);
    }
    if (models == MODEL_CREATE_RETRY) {
        struct http_request request;
        char first[BODY_MAX], body[BODY_MAX];
        unsigned int attempts = retry_case->failures + !retry_case->diagnostic;
        alarm(15u);
        for (unsigned int i = 0; i < attempts; ++i) {
            int fd = accept(listen_fd, NULL, NULL);
            if (fd < 0) server_fail("retry accept failed");
            read_request(fd, &request);
            if (strcmp(request.method, "POST") || strcmp(request.path, "/v1/responses"))
                server_fail("unexpected retry request");
            if (i == 0) memcpy(first, request.body, request.body_len + 1u);
            else if (strcmp(first, request.body)) server_fail("retry changed request bytes");
            if (i == retry_case->failures) {
                send_response(fd, 200u, "text/event-stream", create_sse);
            } else {
                int n = snprintf(body, sizeof(body), "%s%s", retry_case->prefix, retry_case->body);
                if (n < 0 || (size_t)n >= sizeof(body)) server_fail("retry fixture overflow");
                if (retry_case->truncated) {
                    char header[256];
                    int h = snprintf(header, sizeof(header),
                        "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                        "Content-Length: %u\r\nConnection: close\r\n\r\n", (unsigned int)n + 100u);
                    assert(h > 0 && (size_t)h < sizeof(header));
                    write_all_or_die(fd, header, (size_t)h);
                    write_all_or_die(fd, body, (size_t)n);
                } else if (retry_case->status == 200u) {
                    send_response(fd, 200u, "text/event-stream", body);
                } else {
                    send_response(fd, retry_case->status, "application/json", body);
                }
            }
            if (close(fd) < 0) server_fail("close retry socket failed");
        }
        _exit(0);
    }
    if (models >= MODEL_COUNT_404) {
        unsigned int status = models == MODEL_COUNT_404 ? 404u : models == MODEL_COUNT_405 ? 405u :
                              models == MODEL_COUNT_401 ? 401u : models == MODEL_COUNT_403 ? 403u :
                              models == MODEL_COUNT_OK ? 200u : models == MODEL_COUNT_MODEL_404 ? 404u :
                              models == MODEL_COUNT_OVERFLOW ? 400u : 501u;
        const char *body = models == MODEL_COUNT_OK ?
            "{\"object\":\"response.input_tokens\",\"input_tokens\":42}" : models == MODEL_COUNT_MODEL_404 ?
            "{\"error\":{\"code\":\"model_not_found\",\"message\":\"no such model\"}}" :
            models == MODEL_COUNT_OVERFLOW ?
            "{\"error\":{\"code\":400,\"type\":\"exceed_context_size_error\",\"n_ctx\":10,\"n_prompt_tokens\":12}}" :
            "{\"error\":{\"message\":\"not available\"}}";
        serve_one(listen_fd, status, "POST", "/v1/responses/input_tokens", NULL, "application/json", body);
        _exit(0);
    }
    static const struct {
        enum model_fixture fixture;
        unsigned int status;
        const char *method, *path, *content_type, *body;
    } replies[] = {
        {MODEL_CREATE_HTTP_FAILURE, 400u, "POST", "/v1/responses", "application/json",
         "{\"error\":{\"code\":\"context_length_exceeded\","
         "\"message\":\"too large\",\"max_context_tokens\":272000," "\"requested_input_tokens\":300000}}"},
        {MODEL_CREATE_SSE_FAILURE, 200u, "POST", "/v1/responses", "text/event-stream",
         "event: response.failed\n" "data: {\"type\":\"response.failed\",\"response\":{"
         "\"error\":{\"code\":\"context_length_exceeded\","
         "\"message\":\"stream too large transport-secret\"," "\"context_length\":872000}}}\n\n"},
        {MODEL_CODEX_FAILURE, 400u, "GET", "/backend-api/codex/models?client_version=0.146.0", "application/json",
         "{\"error\":{\"message\":\"catalog rejected\"}}"},
        {MODEL_CODEX_MALFORMED, 200u, "GET", "/backend-api/codex/models?client_version=0.146.0", "application/json",
         "{\"models\":[{\"slug\":\"malformed\",\"visibility\":\"list\",\"priority\":1,\"supported_reasoning_levels\":[\"high\"]}]}"},
        {MODEL_CODEX_LOOKALIKE, 200u, "GET", "/backend-api/codexish/v1/models", "application/json",
         "{\"data\":[{\"id\":\"lookalike-openai\"}]}"},
        {MODEL_LIMIT_CONFLICT, 200u, "GET", "/v1/models", "application/json",
         "{\"data\":[{\"id\":\"conflict\",\"context_length\":100," "\"metadata\":{\"contextWindow\":101}}]}"},
        {MODEL_OPENAI, 200u, "GET", "/v1/models", "application/json",
         "{\"object\":\"list\",\"data\":[{\"id\":\"gpt-standard\","
         "\"contextLength\":100000,\"metadata\":{\"context_window\":100000,"
         "\"inputContextWindow\":90000,\"supported_reasoning_levels\":[\"medium\",\"high\"],"
         "\"default_reasoning_level\":\"medium\"},\"capabilities\":{"
         "\"maxOutputTokens\":10000,\"effective_context_window_percent\":80}},"
         "{\"id\":\"future-standard\",\"supported_reasoning_levels\":[\"quantum\",\"cosmic\"]}]}"}
    };
    size_t reply = sizeof(replies) / sizeof(replies[0]) - 1u;
    for (size_t i = 0; i < sizeof(replies) / sizeof(replies[0]); ++i)
        if (replies[i].fixture == models) reply = i;
    serve_one(listen_fd, replies[reply].status, replies[reply].method,
              replies[reply].path, NULL, replies[reply].content_type, replies[reply].body);
    if (!transport || models == MODEL_CREATE_HTTP_FAILURE ||
        models == MODEL_CREATE_SSE_FAILURE || models == MODEL_CODEX_FAILURE) _exit(0);
    serve_one(listen_fd, 200u, "POST", "/v1/responses/input_tokens", "transport-count", "application/json",
              "{\"object\":\"response.input_tokens\",\"input_tokens\":7}");
    serve_one(listen_fd, 200u, "POST", "/v1/responses", "transport-create", "text/event-stream", create_sse);
    serve_one(listen_fd, 200u, "POST", "/v1/responses/compact", "transport-compact", "application/json",
              "{\"object\":\"response.compaction\",\"output\":[{\"type\":\"compaction\",\"encrypted_content\":\"transport-compact-output\"}]}");
    _exit(0);
}

static void
start_server(struct local_server *server, enum model_fixture models, bool transport, const char *suffix)
{
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    int one = 1;

    memset(server, 0, sizeof(*server));
    server->fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(server->fd >= 0);
    assert(setsockopt(server->fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) == 0);
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    assert(bind(server->fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    assert(listen(server->fd, 4) == 0);
    assert(getsockname(server->fd, (struct sockaddr *)&addr, &len) == 0);
    server->port = ntohs(addr.sin_port);
    int written = snprintf(server->endpoint, sizeof(server->endpoint),
                           "http://127.0.0.1:%u%s", server->port, suffix);
    assert(written > 0 && (size_t)written < sizeof(server->endpoint));
    server->pid = fork();
    assert(server->pid >= 0);
    if (server->pid == 0) server_child(server->fd, models, transport);
    /* The fixture owns the listener. Closing the parent's copy means a fixture
     * that dies cannot leave a client waiting on a queued connection nobody
     * accepts on: the peer sees a reset and the test fails instead of parking. */
    assert(close(server->fd) == 0);
    server->fd = -1;
}

static void
stop_server(struct local_server *server)
{
    int status;

    if (server->fd >= 0) assert(close(server->fd) == 0);
    /* Fixtures keep serving until told, so ask this one to stop; the handler
     * exits with status 0, which keeps the assertions below meaningful. */
    (void)kill(server->pid, SIGTERM);
    assert(waitpid(server->pid, &status, 0) == server->pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
}

static json_t *
request_with_marker(const char *marker)
{
    return checked_json(json_pack("{s:s,s:[{s:s,s:s}]}", "model", "gpt-transport-test",
        "input", "role", "user", "content", marker));
}

static int
emit_capture(void *opaque, size_t item_index, enum snag_item_kind kind,
             enum snag_item_phase phase, const char *provider_item_id, const char *text, size_t len)
{
    struct emitted_text *emitted = opaque;

    (void)item_index;
    (void)kind;
    (void)phase;
    (void)provider_item_id;
    ++emitted->calls;
    return snag_buf_append(&emitted->text, text, len);
}

static int
response_ready(void *opaque)
{
    unsigned int *count = opaque;
    ++*count;
    return 0;
}

static void
credential_set(struct snag_credential *credential, const char *value)
{
    snag_credential_clear(credential);
    credential->len = strlen(value);
    assert(credential->len <= SNAG_CREDENTIAL_MAX);
    memcpy(credential->value, value, credential->len + 1u);
}

static void
transport_settings(struct snag_provider_config *provider, struct snag_credential *credential)
{
    provider->connect_timeout_ms = 1000u;
    provider->idle_timeout_ms = 1000u;
    provider->request_timeout_ms = 3000u;
    assert(snag_strcpy(provider->openrouter_referer, sizeof(provider->openrouter_referer),
                       "https://github.com/snajpa/snajpagent"));
    assert(snag_strcpy(provider->openrouter_title, sizeof(provider->openrouter_title), "snajpagent"));
    credential_set(credential, "transport-secret");
}

static struct snag_provider_connection
transport_connection(struct snag_config *config, struct snag_credential *credential, const char *base_url)
{
    snag_config_init(config);
    assert(snag_strcpy(config->providers[0].base_url, sizeof(config->providers[0].base_url), base_url));
    transport_settings(&config->providers[0], credential);
    return (struct snag_provider_connection){config, &config->providers[0], credential, NULL, NULL, NULL, NULL, 0};
}

/* The proxy keys prompt-cache affinity on a session identity from its session lane; without it every
 * request is pinned per-request and reuse collapses. This asserts the header reaches the wire: the fixture
 * aborts the request if the exact line is missing, so the case cannot pass vacuously. */
static void
test_session_identity_header(void)
{
    static const char session_id[] = "0123456789abcdef0123456789abcdef";
    struct local_server server;
    struct snag_config config;
    struct snag_credential credential;
    struct emitted_text emitted;
    json_t *request;
    json_t *models = NULL;
    struct snag_response_graph graph = {0};
    uint64_t tokens = 0u;
    unsigned int retries = 99u;
    char error[256] = {0};

    /* Set before the server forks: the fixture child inherits this expectation. */
    expected_session_header = "session_id: 0123456789abcdef0123456789abcdef";
    start_server(&server, MODEL_OPENAI, true, "");
    struct snag_provider_connection connection;
    snag_config_init(&config);
    connection = (struct snag_provider_connection){
        &config, &config.providers[1], &credential, NULL, NULL, NULL, session_id, 0};
    snag_config_provider_init(&config.providers[1], "second");
    config.provider_count = 2u;
    assert(snprintf(config.providers[1].name, sizeof(config.providers[1].name), "transport") > 0);
    assert(snprintf(config.providers[1].base_url, sizeof(config.providers[1].base_url),
                    "%s/v1/", server.endpoint) > 0);
    transport_settings(&config.providers[1], &credential);

    /* Every provider request must carry the session identity: the fixture aborts the request when the exact
     * header line is absent or altered, so the case cannot pass vacuously. The call order mirrors the
     * fixture's canned replies (models, count, create, compact). */
    assert(snag_provider_models_list(connection, &models, error, sizeof(error)) == 0);
    json_decref(models);

    request = request_with_marker("transport-count");
    assert(snag_provider_responses_count(connection, request, &tokens, NULL, error, sizeof(error),
                                         &retries) == 0);
    json_decref(request);

    memset(&emitted, 0, sizeof(emitted));
    snag_buf_init(&emitted.text, 128u);
    request = request_with_marker("transport-create");
    unsigned int ready_count = 0u;
    assert(snag_provider_responses_create(connection, request, emit_capture, &emitted,
        NULL, NULL, response_ready, &ready_count, &graph, NULL,
                                          error, sizeof(error), &retries) == 0);
    assert(ready_count == 1u);
    json_decref(request);
    snag_buf_free(&emitted.text);
    snag_response_graph_free(&graph);
    struct snag_json_document compact_output = {0};
    request = request_with_marker("transport-compact");
    assert(snag_provider_responses_compact(connection, request, &compact_output,
                                          error, sizeof(error), &retries) == 0);
    json_decref(request);
    snag_json_document_free(&compact_output);
    snag_credential_clear(&credential);
    snag_config_free(&config);
    stop_server(&server);
    expected_session_header = NULL;
}

static void
test_local_provider_transport(void)
{
    struct local_server server;
    struct snag_config config;
    struct snag_credential credential;
    struct emitted_text emitted;
    json_t *request;
    struct snag_json_document compact_output = {0};
    json_t *models = NULL;
    uint64_t tokens = 0u;
    unsigned int retries = 99u;
    char error[256] = {0};

    start_server(&server, MODEL_OPENAI, true, "");
    struct snag_provider_connection connection;
    snag_config_init(&config);
    connection = (struct snag_provider_connection){
        &config, &config.providers[1], &credential, NULL, NULL, NULL, NULL, 0};
    snag_config_provider_init(&config.providers[1], "second");
    config.provider_count = 2u;
    assert(snprintf(config.providers[1].name, sizeof(config.providers[1].name), "transport") > 0);
    assert(snprintf(config.providers[1].base_url, sizeof(config.providers[1].base_url),
                    "%s/v1/", server.endpoint) > 0);
    transport_settings(&config.providers[1], &credential);

    assert(snag_provider_models_list(connection, &models, error, sizeof(error)) == 0);
    assert(json_array_size(models) == 2u);
    assert(strcmp(snag_json_string(json_array_get(models, 0), "id"), "gpt-standard") == 0);
    assert(strcmp(json_string_value(json_array_get(json_object_get(
                      json_array_get(models, 0), "efforts"), 1)), "high") == 0);
    assert(strcmp(snag_json_string(json_array_get(models, 0), "default_effort"), "medium") == 0);
    {
        json_t *limits = json_object_get(json_array_get(models, 0), "limits");
        assert(limits);
        assert(json_integer_value(json_object_get( limits, "context_window_tokens")) == 100000);
        assert(json_integer_value(json_object_get( limits, "input_context_window_tokens")) == 90000);
        assert(json_integer_value(json_object_get( limits, "max_output_tokens")) == 10000);
        assert(json_integer_value(json_object_get( limits, "effective_context_window_percent")) == 80);
        assert(json_is_null(json_object_get(limits, "max_input_tokens")));
    }
    {
        json_t *limits = json_object_get(json_array_get(models, 1), "limits");
        assert(limits);
        assert(json_is_null(json_object_get( limits, "context_window_tokens")));
        assert(json_is_null(json_object_get(limits, "max_output_tokens")));
    }
    assert(strcmp(snag_model_best_effort(NULL, NULL, NULL, json_array_get(models, 1), "fallback"), "quantum") == 0);
    json_decref(models);
    models = NULL;

    request = request_with_marker("transport-count");
    assert(snag_provider_responses_count(connection,
        request, &tokens, NULL, error, sizeof(error), &retries) == 0);
    assert(tokens == 7u);
    assert(retries == 0u);
    json_decref(request);

    request = request_with_marker("transport-create");
    struct snag_response_graph graph = {0};
    memset(&emitted, 0, sizeof(emitted));
    snag_buf_init(&emitted.text, 128u);
    assert(snag_provider_responses_create(connection,
        request, emit_capture, &emitted, NULL, NULL, NULL, NULL, &graph,
        NULL, error, sizeof(error), &retries) == 0);
    assert(strcmp(graph.provider_response_id, "resp_transport") == 0);
    assert(graph.count == 1u);
    assert(strcmp(snag_response_graph_item(&graph, 0).text, "local transport") == 0);
    assert(emitted.calls == 1u);
    assert(emitted.text.len == strlen("local transport"));
    assert(memcmp(emitted.text.data, "local transport", strlen("local transport")) == 0);
    assert(retries == 0u);
    snag_buf_free(&emitted.text);
    snag_response_graph_free(&graph);
    json_decref(request);

    request = request_with_marker("transport-compact");
    assert(snag_provider_responses_compact(connection,
        request, &compact_output, error, sizeof(error), &retries) == 0);
    assert(json_is_array(compact_output.value));
    assert(json_array_size(compact_output.value) == 1u);
    assert(compact_output.bytes > 0u);
    assert(retries == 0u);
    snag_json_document_free(&compact_output);
    json_decref(request);

    snag_config_free(&config);
    stop_server(&server);
}

static void
test_codex_path_selection(void)
{
    static const struct {
        enum model_fixture fixture;
        const char *path, *provider, *base_override, *id, *diagnostic;
        size_t count;
    } cases[] = {
        {MODEL_CODEX_LOOKALIKE, "/backend-api/codexish", "codex", NULL, "lookalike-openai", NULL, 1u},
        {MODEL_OPENAI, "", "codex", "http://backend-api/codex", NULL, NULL, 2u},
        {MODEL_CODEX_MALFORMED, "/backend-api/codex", "codex", NULL, NULL, "invalid model entry", 0u},
        {MODEL_CODEX_FAILURE, "/backend-api/codex", "neutral", NULL, NULL, "catalog rejected", 0u},
        {MODEL_LIMIT_CONFLICT, "", "neutral", NULL, NULL, "invalid model entry", 0u}
    };
    struct local_server server;
    struct snag_config config;
    struct snag_credential credential;
    char error[256] = {0};
    struct snag_provider_connection connection;

    snag_config_init(&config);
    connection = (struct snag_provider_connection){
        &config, &config.providers[0], &credential, NULL, NULL, NULL, NULL, 0};
    transport_settings(&config.providers[0], &credential);
    for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        json_t *models = NULL;
        start_server(&server, cases[i].fixture, false, cases[i].path);
        assert(snag_strcpy(config.providers[0].name, sizeof(config.providers[0].name), cases[i].provider));
        assert(snag_strcpy(config.providers[0].base_url, sizeof(config.providers[0].base_url),
                           cases[i].base_override ? cases[i].base_override : server.endpoint));
        if (cases[i].base_override) assert(setenv("SNAJPAGENT_TEST_OPENAI_BASE", server.endpoint, 1) == 0);
        int rc = snag_provider_models_list(connection, &models, error, sizeof(error));
        if (cases[i].diagnostic) {
            assert(rc < 0 && models == NULL);
            assert(strstr(error, cases[i].diagnostic));
        } else {
            assert(rc == 0 && json_array_size(models) == cases[i].count);
            if (cases[i].id) assert(!strcmp(snag_json_string(json_array_get(models, 0), "id"), cases[i].id));
        }
        json_decref(models);
        if (cases[i].base_override) assert(unsetenv("SNAJPAGENT_TEST_OPENAI_BASE") == 0);
        stop_server(&server);
    }
    snag_config_free(&config);
}

static void
test_structured_create_failures(void)
{
    const enum model_fixture fixtures[] = {
        MODEL_CREATE_HTTP_FAILURE, MODEL_CREATE_SSE_FAILURE };

    for (size_t i = 0; i < sizeof(fixtures) / sizeof(fixtures[0]); ++i) {
        struct local_server server;
        struct snag_config config;
        struct snag_credential credential;
        struct snag_provider_failure failure;
        json_t *request = request_with_marker("capacity-failure");
        char error[256] = {0};

        start_server(&server, fixtures[i], false, "/v1");
        struct snag_provider_connection connection =
            transport_connection(&config, &credential, server.endpoint);
        struct snag_response_graph graph = {0};
        memset(&failure, 0, sizeof(failure));
        assert(snag_provider_responses_create(connection,
            request, NULL, NULL, NULL, NULL, NULL, NULL, &graph,
            &failure, error, sizeof(error), NULL) < 0);
        assert(snag_provider_failure_is_capacity(&failure));
        assert(!strstr(error, "transport-secret"));
        assert(!strstr(failure.message, "transport-secret"));
        assert(failure.context_limit_tokens == (fixtures[i] == MODEL_CREATE_HTTP_FAILURE ?
                    272000u : 872000u));
        if (fixtures[i] == MODEL_CREATE_HTTP_FAILURE) {
            assert(failure.requested_input_tokens == 300000u);
        }
        snag_response_graph_free(&graph);
        json_decref(request);
        snag_config_free(&config);
        stop_server(&server);
    }
}

static void
test_typeless_create_diagnostic(void)
{
    struct local_server server;
    struct snag_config config;
    struct snag_credential credential;
    json_t *request = request_with_marker("typeless-diagnostic");
    char error[1024] = {0};
    struct snag_response_graph graph = {0};

    start_server(&server, MODEL_CREATE_TYPELESS, false, "/v1");
    assert(snag_provider_responses_create(transport_connection(&config, &credential, server.endpoint),
        request, NULL, NULL, NULL, NULL, NULL, NULL, &graph, NULL, error, sizeof(error), NULL) < 0);
    assert(strstr(error, "Responses event has no type") != NULL);
    assert(strstr(error, "Responses record diagnostic: event=-") != NULL);
    assert(strstr(error, "json=object") != NULL);
    assert(strstr(error, "keys=kind") != NULL);
    assert(strstr(error, "private-value") == NULL);
    snag_response_graph_free(&graph);
    json_decref(request);
    snag_config_free(&config);
    stop_server(&server);
}

struct retry_cancel {
    int fd, code;
    struct snag_buf notice;
};

static int
cancel_retry(void *opaque, uint32_t wait_ms)
{
    struct retry_cancel *cancel = opaque;
    char buf[512];
    ssize_t n;
    (void)wait_ms;
    while ((n = read(cancel->fd, buf, sizeof(buf))) > 0)
        assert(snag_buf_append(&cancel->notice, buf, (size_t)n) == 0);
    assert(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
    return cancel->notice.data && strstr((const char *)cancel->notice.data,
        "provider retry 1/2") ? cancel->code : 0;
}

static void
test_create_retries(void)
{
    static const char created[] = ": heartbeat\n\n" "id: discarded-event\n"
        "data: {\"type\":\"response.created\",\"response\":{\"id\":\"discarded\",\"status\":\"in_progress\",\"output\":[]}}\n\n";
    static const char transient[] =
        "data: {\"type\":\"response.failed\",\"response\":{\"error\":{\"code\":\"server_error\","
        "\"message\":\"Response payload is not completed: <TransferEncodingError: 400, message='Not enough data to satisfy transfer length header.'> transport-secret\"}}}\n\n";
    static const char partial[] =
        "data: {\"type\":\"response.created\",\"response\":{\"id\":\"partial\",\"status\":\"in_progress\",\"output\":[]}}\n\n"
        "data: {\"type\":\"response.output_item.added\",\"output_index\":0,\"item\":{\"id\":\"m\",\"type\":\"message\",\"status\":\"in_progress\",\"role\":\"assistant\",\"phase\":\"commentary\",\"content\":[]}}\n\n"
        "data: {\"type\":\"response.content_part.added\",\"output_index\":0,\"item_id\":\"m\",\"content_index\":0,\"part\":{\"type\":\"output_text\",\"text\":\"\"}}\n\n"
        "data: {\"type\":\"response.output_text.delta\",\"output_index\":0,\"item_id\":\"m\",\"content_index\":0,\"delta\":\"once\"}\n\n";
    const struct retry_case cases[] = {
        {created, transient, 200, 1, 1, false, NULL, "local transport"},
        {created, transient, 200, 3, 2, false, "retried 2 times", ""},
        {"", "data: {\"type\":\"error\",\"code\":\"rate_limit_exceeded\"}\n\n",
            200, 1, 1, false, NULL, "local transport"},
        {"", "data: {\"type\":\"response.failed\",\"response\":{\"error\":{\"type\":\"service_unavailable_error\"}}}\n\n",
            200, 1, 1, false, NULL, "local transport"},
        {"", "data: {\"type\":\"response.failed\",\"response\":{\"error\":{\"code\":\"cyber_policy\",\"message\":\"flagged for possible cybersecurity risk\"}}}\n\n",
            200, 1, 0, false, "[cyber_policy]", ""},
        {"", "data: {\"type\":\"error\",\"code\":\"unknown\",\"message\":\"TransferEncodingError\"}\n\n",
            200, 1, 0, false, "[unknown]", ""},
        {"", "data: {\"type\":\"error\",\"message\":\"TransferEncodingError\"}\n\n",
            200, 1, 0, false, "TransferEncodingError", ""},
        {"", "data: {\"type\":\"response.incomplete\",\"response\":{\"incomplete_details\":{\"reason\":\"max_output_tokens\"}}}\n\n",
            200, 1, 0, false, "[max_output_tokens]", ""},
        {"", "data: {\"type\":\"error\",\"code\":\"server_error\",\"context_limit\":-1}\n\n",
            200, 1, 0, false, "invalid structured", ""},
        {"", "{\"error\":{\"code\":\"slow_down\",\"type\":\"rate_limit_error\"}}",
            429, 1, 1, false, NULL, "local transport"},
        {"", "{\"error\":{\"code\":\"credit_balance_exhausted\",\"type\":\"insufficient_quota\"}}",
            429, 1, 0, false, "credit_balance_exhausted", ""},
        {"", "{\"error\":{\"type\":\"insufficient_quota\"}}", 429, 1, 0, false, "insufficient_quota", ""},
        {"", "{\"error\":{\"code\":\"cyber_policy\"}}", 500, 1, 0, false, "cyber_policy", ""},
        {"", "{\"error\":{\"code\":\"unknown\",\"type\":\"server_error\"}}", 503, 1, 0, false, "unknown", ""},
        {"", "{\"error\":{\"code\":\"upstream_error\",\"type\":\"server_error\",\"message\":\"{\\\"detail\\\":\\\"Unable to verify provider access. Please try again.\\\"}\"}}",
            503, 3, 2, false, "retried 2 times", ""},
        {"", "{\"error\":{\"code\":23,\"type\":\"server_error\"}}", 500, 1, 0, false, "HTTP 500", ""},
        {"", "{\"error\":{\"code\":\"server_error\"}}", 503, 3, 2, false, "retried 2 times", ""},
        {"", "temporarily unavailable", 503, 1, 1, false, NULL, "local transport"},
        {"", "", 200, 1, 1, true, NULL, "local transport"},
        {created, "", 200, 1, 1, true, NULL, "local transport"},
        {created, "", 200, 1, 1, false, NULL, "local transport"},
        {created, "data: {", 200, 1, 0, false, "mid-event", ""},
        {created, "data: {", 200, 1, 0, true, "provider transport failed", ""},
        {created, "data: {bad}\n\n", 200, 1, 0, false, "JSON", ""},
        {partial, transient, 200, 1, 0, false, "[server_error]", "once"},
        {partial, "", 200, 1, 0, true, "provider transport failed", "once"},
        {created, "data: {\"type\":\"response.failed\",\"response\":{\"output\":[{\"type\":\"function_call\"}],\"error\":{\"code\":\"server_error\"}}}\n\n",
            200, 1, 0, false, "invalid function call snapshot", ""},
        {"data: {\"type\":\"response.web_search_call.in_progress\"}\n\n",
            transient, 200, 1, 0, false, "[server_error]", ""},
        {"data: {\"type\":\"response.future_activity\"}\n\n",
            transient, 200, 1, 0, false, "[server_error]", ""},
        {"data: {\"type\":\"response.created\",\"response\":{\"id\":\"tools\",\"status\":\"in_progress\",\"output\":[]}}\n\n"
         "data: {\"type\":\"response.output_item.added\",\"output_index\":0,\"item\":{\"type\":\"function_call\",\"status\":\"in_progress\",\"id\":\"f\",\"call_id\":\"c\",\"name\":\"exec_command\",\"arguments\":\"\"}}\n\n",
            transient, 200, 1, 0, false, "[server_error]", ""}
    };

    size_t count = sizeof(cases) / sizeof(cases[0]);
    const struct retry_case cancelled = {created, transient, 200, 1, 0, false, "", ""};
    for (size_t i = 0; i < count + 3u; ++i) {
        struct local_server server;
        struct snag_config config;
        struct snag_credential credential;
        struct snag_provider_failure failure;
        struct emitted_text emitted = {0};
        json_t *request = request_with_marker("retry-current-cycle");
        char error[512] = {0};
        unsigned int retries = 99u;
        int pipefd[2], saved_stderr = -1;
        struct snag_ui ui;
        struct retry_cancel cancellation = {.code = i < count ? 0 : (int)(i - count + 1u)};

        retry_case = i < count ? &cases[i] : &cancelled;
        start_server(&server, MODEL_CREATE_RETRY, false, "/v1");
        if (cancellation.code) {
            assert(pipe(pipefd) == 0);
            assert(fcntl(pipefd[0], F_SETFL, O_NONBLOCK) == 0);
            saved_stderr = dup(STDERR_FILENO);
            assert(saved_stderr >= 0 && dup2(pipefd[1], STDERR_FILENO) == STDERR_FILENO);
            assert(close(pipefd[1]) == 0);
            cancellation.fd = pipefd[0];
            snag_buf_init(&cancellation.notice, 4096u);
            assert(snag_ui_init(&ui) == 0);
        }
        snag_config_init(&config);
        strcpy(config.providers[0].base_url, server.endpoint);
        strcpy(config.providers[0].openrouter_referer, "https://github.com/snajpa/snajpagent");
        strcpy(config.providers[0].openrouter_title, "snajpagent");
        config.providers[0].request_timeout_ms = 3000u;
        credential_set(&credential, "transport-secret");
        struct snag_response_graph graph = {0};
        snag_buf_init(&emitted.text, 1024u);
        int rc = snag_provider_responses_create((struct snag_provider_connection){
            &config, &config.providers[0], &credential, cancellation.code ? &ui : NULL,
            cancellation.code ? cancel_retry : NULL, &cancellation, NULL, 0},
            request, emit_capture, &emitted, NULL, NULL, NULL, NULL, &graph,
            &failure, error, sizeof(error), &retries);
        if (cancellation.code) {
            snag_ui_free(&ui);
            assert(dup2(saved_stderr, STDERR_FILENO) == STDERR_FILENO);
            assert(close(saved_stderr) == 0 && close(pipefd[0]) == 0);
            snag_buf_free(&cancellation.notice);
        }
        if ((!cancellation.code && (retry_case->diagnostic ? rc >= 0 : rc != 0)) ||
            retries != retry_case->retries ||
            (retry_case->diagnostic && !strstr(error, retry_case->diagnostic)))
            fprintf(stderr, "retry case %zu: rc=%d retries=%u: %s\n", i, rc, retries, error);
        int expected_cancel = cancellation.code == SNAG_PROVIDER_NEW_INPUT ? 0 : cancellation.code;
        assert(rc == (expected_cancel ? expected_cancel : retry_case->diagnostic ? -1 : 0));
        assert(retries == retry_case->retries);
        assert(!strstr(error, "transport-secret") && !strstr(failure.message, "transport-secret"));
        if (retry_case->diagnostic) {
            assert(strstr(error, retry_case->diagnostic));
            assert(graph.count == 0u);
        } else {
            assert(!failure.code[0] && !failure.type[0] && !failure.message[0]);
            assert(graph.count == 1u && strcmp(graph.provider_response_id, "resp_transport") == 0);
        }
        assert(emitted.text.len == strlen(retry_case->emitted));
        if (emitted.text.len) assert(memcmp(emitted.text.data, retry_case->emitted, emitted.text.len) == 0);
        snag_response_graph_free(&graph);
        snag_buf_free(&emitted.text);
        snag_credential_clear(&credential);
        snag_config_free(&config);
        json_decref(request);
        stop_server(&server);
    }
    retry_case = NULL;
}

static void
test_policy_clarification_after_reasoning(void)
{
    const struct retry_case policy = {
        "data: {\"type\":\"response.created\",\"response\":{\"id\":\"policy\",\"status\":\"in_progress\",\"output\":[]}}\n\n"
        "data: {\"type\":\"response.output_item.added\",\"output_index\":0,\"item\":{\"type\":\"reasoning\",\"id\":\"r\",\"summary\":[]}}\n\n"
        "data: {\"type\":\"response.reasoning_summary_text.delta\",\"output_index\":0,\"item_id\":\"r\",\"summary_index\":0,\"delta\":\"Reviewing the task\"}\n\n",
        "data: {\"type\":\"response.failed\",\"response\":{\"output\":[{\"type\":\"reasoning\",\"id\":\"r\",\"summary\":[]}],\"error\":{\"code\":\"cyber_policy\"}}}\n\n",
        200, 1, 0, false, "[cyber_policy]", "" };
    struct local_server server;
    struct snag_config config;
    struct snag_credential credential;
    struct snag_provider_failure failure;
    struct snag_response_graph graph = {0};
    struct emitted_text emitted = {0};
    json_t *request = request_with_marker("policy-reasoning");
    char error[512] = {0};
    unsigned int retries = 99u;

    retry_case = &policy;
    start_server(&server, MODEL_CREATE_RETRY, false, "/v1");
    snag_config_init(&config);
    strcpy(config.providers[0].base_url, server.endpoint);
    strcpy(config.providers[0].openrouter_referer, "https://github.com/snajpa/snajpagent");
    strcpy(config.providers[0].openrouter_title, "snajpagent");
    credential_set(&credential, "transport-secret");
    snag_buf_init(&emitted.text, 1024u);
    int rc = snag_provider_responses_create((struct snag_provider_connection){
        &config, &config.providers[0], &credential, NULL, NULL, NULL, NULL, 0},
        request, emit_capture, &emitted, NULL, NULL, NULL, NULL, &graph,
            &failure, error, sizeof(error), &retries);
    assert(rc < 0 && retries == 0u && emitted.text.len == 0u);
    assert(!strcmp(failure.code, "cyber_policy"));
    assert(failure.output_correction == SNAG_OUTPUT_CORRECTION_CYBER_POLICY);
    snag_response_graph_free(&graph);
    snag_buf_free(&emitted.text);
    snag_credential_clear(&credential);
    snag_config_free(&config);
    json_decref(request);
    stop_server(&server);
    retry_case = NULL;
}

static void
test_count_capability_statuses(void)
{
    const enum model_fixture fixtures[] = {
        MODEL_COUNT_404, MODEL_COUNT_405, MODEL_COUNT_501 };
    for (size_t i = 0; i < sizeof(fixtures) / sizeof(fixtures[0]); ++i) {
        struct local_server server;
        struct snag_config config;
        struct snag_credential credential;
        json_t *request = request_with_marker("count-status");
        uint64_t tokens = 0u;
        bool endpoint_unsupported = true;
        char error[256] = {0};

        start_server(&server, fixtures[i], false, "/v1");
        struct snag_provider_connection connection =
            transport_connection(&config, &credential, server.endpoint);
        assert(snag_provider_responses_count(connection,
            request, &tokens, &endpoint_unsupported, error, sizeof(error), NULL) < 0);
        assert(endpoint_unsupported);
        json_decref(request);
        snag_config_free(&config);
        stop_server(&server);
    }
}

static void
test_count_modes(void)
{
    const struct {
        enum model_fixture fixture;
        enum snag_token_count_mode mode;
        int result;
        enum snag_count_capability capability;
        bool openrouter, images;
    } cases[] = {
        {MODEL_COUNT_405, SNAG_TOKEN_COUNT_AUTO,
         SNAG_APP_COUNT_SKIPPED, SNAG_COUNT_UNSUPPORTED, false, false},
        {MODEL_COUNT_405, SNAG_TOKEN_COUNT_STRICT, -1, SNAG_COUNT_UNSUPPORTED, false, false},
        {MODEL_COUNT_404, SNAG_TOKEN_COUNT_AUTO, SNAG_APP_COUNT_SKIPPED, SNAG_COUNT_UNSUPPORTED, false, false},
        {MODEL_COUNT_404, SNAG_TOKEN_COUNT_AUTO,
         SNAG_APP_COUNT_SKIPPED, SNAG_COUNT_UNSUPPORTED, true, false},
        {MODEL_COUNT_404, SNAG_TOKEN_COUNT_STRICT, -1, SNAG_COUNT_UNSUPPORTED, true, false},
        {MODEL_COUNT_401, SNAG_TOKEN_COUNT_AUTO, -1, SNAG_COUNT_UNKNOWN, true, false},
        {MODEL_COUNT_403, SNAG_TOKEN_COUNT_AUTO, -1, SNAG_COUNT_UNKNOWN, true, false},
        {MODEL_COUNT_MODEL_404, SNAG_TOKEN_COUNT_AUTO, -1, SNAG_COUNT_UNKNOWN, false, false},
        {MODEL_COUNT_OVERFLOW, SNAG_TOKEN_COUNT_AUTO, SNAG_PROVIDER_CONTEXT_OVERFLOW, SNAG_COUNT_UNKNOWN, false, false},
        {MODEL_COUNT_OK, SNAG_TOKEN_COUNT_AUTO, 0, SNAG_COUNT_SUPPORTED, false, false},
        {MODEL_COUNT_OK, SNAG_TOKEN_COUNT_STRICT, 0, SNAG_COUNT_SUPPORTED, false, false},
        {MODEL_COUNT_405, SNAG_TOKEN_COUNT_AUTO, SNAG_APP_COUNT_SKIPPED, SNAG_COUNT_UNSUPPORTED, false, true},
        {MODEL_COUNT_405, SNAG_TOKEN_COUNT_STRICT, -1, SNAG_COUNT_UNSUPPORTED, false, true},
        {MODEL_COUNT_401, SNAG_TOKEN_COUNT_AUTO, -1, SNAG_COUNT_UNKNOWN, false, true}
    };

    assert(!snag_app_exact_count_enabled(SNAG_TOKEN_COUNT_OFF, SNAG_COUNT_UNKNOWN));
    assert(!snag_app_exact_count_enabled(SNAG_TOKEN_COUNT_AUTO, SNAG_COUNT_UNSUPPORTED));
    assert(snag_app_exact_count_enabled(SNAG_TOKEN_COUNT_AUTO, SNAG_COUNT_SUPPORTED));
    assert(snag_app_exact_count_enabled(SNAG_TOKEN_COUNT_STRICT, SNAG_COUNT_UNSUPPORTED));

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        struct local_server server;
        struct snag_config config;
        struct snag_credential credential;
        struct app_state app;
        json_t *request = request_with_marker("count-mode");
        const char *method = cases[i].mode == SNAG_TOKEN_COUNT_STRICT ? "anchored_upper_bound" : "unknown";
        uint64_t tokens = 99u;
        char error[256] = {0};
        char temp[4096];
        int rc;

        assert(snprintf(temp, sizeof(temp), "%s/snajpagent-count-mode-XXXXXX",
            getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp") > 0);
        assert(mkdtemp(temp));
        start_server(&server,cases[i].fixture,false,"/v1");
        (void)transport_connection(&config,&credential,cases[i].images?"https://api.openai.com":
            cases[i].openrouter?"https://openrouter.ai/api/v1":server.endpoint);
        assert(setenv("SNAJPAGENT_TEST_OPENAI_BASE",server.endpoint,1)==0);
        config.providers[0].exact_token_count = cases[i].mode;
        memset(&app, 0, sizeof(app));
        snag_store_init(&app.store);
        assert(snag_store_open(&app.store, temp, error, sizeof(error)) == 0);
        app.model_cache = (struct snag_model_cache){0};
        assert(snag_ui_init(&app.ui) == 0);
        app.config = &config;
        app.turn_provider = &config.providers[0];
        app.turn_model = "gpt-transport-test";
        uint64_t expected = tokens;
        if (cases[i].images) {
            app.turn_model = "gpt-5.5";
            assert(json_object_set_new(request, "model", json_string("gpt-5.5")) == 0);
            json_t *item = json_pack("{s:s,s:[{s:s,s:s,s:s}]}", "role", "user", "content",
                "type", "input_image", "detail", "high", "image_url", "data:image/png;base64,YWJj");
            assert(json_array_append_new(json_object_get(request, "input"), item) == 0);
            assert(snag_media_token_bound(request, 0u, &expected, error, sizeof(error)) == 0);
        }
        rc = snag_app_provider_count(&app, request, &credential,
                                    &tokens, &method, error, sizeof(error));
        assert((cases[i].result < 0 && rc < 0) || rc == cases[i].result);
        assert(app.turn_capacity.count_capability == cases[i].capability);
        if (cases[i].images && rc == SNAG_APP_COUNT_SKIPPED)
            assert(tokens == expected && !strcmp(method, "media_upper_bound"));
        else if(cases[i].fixture==MODEL_COUNT_OK)assert(tokens==42u && !strcmp(method,"exact"));
        else assert(tokens==99u);
        assert(unsetenv("SNAJPAGENT_TEST_OPENAI_BASE") == 0);
        snag_ui_free(&app.ui);
        snag_model_cache_free(&app.model_cache);
        if (unlinkat(app.store.root_fd, "models.lock", 0) < 0) assert(errno == ENOENT);
        snag_store_close(&app.store);
        {
            char path[512];
            assert(snprintf(path, sizeof(path), "%s/sessions", temp) > 0);
            assert(rmdir(path) == 0);
            assert(snprintf(path, sizeof(path), "%s/trash", temp) > 0);
            assert(rmdir(path) == 0);
        }
        assert(rmdir(temp) == 0);
        json_decref(request);
        snag_config_free(&config);
        stop_server(&server);
    }
}

static void
test_openrouter_search_transport(void)
{
    static const char search_tools[] = "[{\"type\":\"openrouter:web_search\"},{\"type\":\"function\","
        "\"name\":\"read_file\",\"parameters\":{\"type\":\"object\",\"properties\":{"
        "\"path\":{\"type\":\"string\"},\"start_line\":{\"type\":\"integer\"},"
        "\"end_line\":{\"type\":\"integer\"}}}}]";
    static const char local_call[] =
        "{\"type\":\"function_call\",\"call_id\":\"call_after_search\",\"name\":\"read_file\","
        "\"arguments\":\"{\\\"path\\\":\\\"README.md\\\",\\\"start_line\\\":1,\\\"end_line\\\":1}\"}";
    static const char local_output[] =
        "{\"type\":\"function_call_output\",\"call_id\":\"call_after_search\",\"output\":\"snajpagent\"}";
    struct local_server server;
    struct snag_config config;
    struct snag_credential credential;
    struct emitted_text emitted = {0};
    json_t *request;
    char error[256] = {0};
    unsigned int retries = 0u;

    start_server(&server, MODEL_OPENROUTER_SEARCH, false, "");
    assert(setenv("SNAJPAGENT_TEST_OPENAI_BASE", server.endpoint, 1) == 0);
    struct snag_provider_connection connection =
        transport_connection(&config, &credential, "https://openrouter.ai/api/v1");
    request = request_with_marker("search example domains");
    assert(json_object_set_new(request, "tools", json_loadb(
        search_tools, sizeof(search_tools) - 1u, 0, NULL)) == 0);
    assert(snag_config_provider_is_openrouter(&config.providers[0]));
    snag_buf_init(&emitted.text, 128u);
    struct snag_response_graph graph = {0};
    assert(snag_provider_responses_create(connection,
        request, emit_capture, &emitted, NULL, NULL, NULL, NULL, &graph,
        NULL, error, sizeof(error), &retries) == 0);
    assert(!retries);
    assert(graph.count == 2u);
    assert(snag_response_graph_item(&graph, 0).kind == SNAG_ITEM_ASSISTANT);
    assert(strcmp(snag_response_graph_item(&graph, 0).text, "Found https://example.com") == 0);
    assert(snag_response_graph_item(&graph, 1).kind == SNAG_ITEM_TOOL_CALL);
    assert(strcmp(snag_response_graph_item(&graph, 1).name, "read_file") == 0);
    assert(strcmp(snag_response_graph_item(&graph, 1).provider_call_id, "call_after_search") == 0);
    assert(strcmp(snag_json_string(snag_response_graph_item(&graph, 1).arguments, "path"), "README.md") == 0);
    assert(emitted.calls == 1u);
    assert(emitted.text.len == strlen("Found https://example.com"));
    assert(memcmp(emitted.text.data, "Found https://example.com", emitted.text.len) == 0);
    /* A hosted item must not become a local call or contaminate the next
     * stateless response. The existing context tests cover replay projection. */
    assert(json_array_append_new(json_object_get(request, "input"), json_loadb(
        local_call, sizeof(local_call) - 1u, 0, NULL)) == 0);
    assert(json_array_append_new(json_object_get(request, "input"), json_loadb(
        local_output, sizeof(local_output) - 1u, 0, NULL)) == 0);
    snag_response_graph_free(&graph);
    snag_buf_reset(&emitted.text);
    emitted.calls = 0u;
    assert(snag_provider_responses_create(connection,
        request, emit_capture, &emitted, NULL, NULL, NULL, NULL, &graph,
        NULL, error, sizeof(error), &retries) == 0);
    assert(graph.count == 1u && snag_response_graph_item(&graph, 0).kind == SNAG_ITEM_ASSISTANT);
    assert(strcmp(snag_response_graph_item(&graph, 0).text, "local transport") == 0);
    assert(emitted.calls == 1u && !retries);
    snag_response_graph_free(&graph);
    snag_buf_free(&emitted.text);
    json_decref(request);
    snag_credential_clear(&credential);
    assert(unsetenv("SNAJPAGENT_TEST_OPENAI_BASE") == 0);
    stop_server(&server);
}

static void
test_read_only_dispatch(void)
{
    static const char *const denied[] = {
        "exec_command", "write_stdin", "apply_patch", "create_goal",
        "update_goal", "irc_send", "irc_topic", "irc_nick", "irc_state", "unknown",
        "web_search", "openrouter:web_search", "speak_text", "write_file", "edit_file"
    };
    struct app_state app = {0};
    struct snag_response_item call = {0};
    json_t *result = NULL;
    char error[256] = {0};

    app.session.active_read_only = true;
    call.kind = SNAG_ITEM_TOOL_CALL;
    call.arguments = json_object();
    for (size_t i = 0; i < sizeof(denied) / sizeof(denied[0]); ++i) {
        call.name = (char *)denied[i];
        /* No config, IRC, credential or process: no handler may be reached. */
        assert(snag_app_tool_run(&app, &call, NULL, &result, error, sizeof(error)) == 0);
        assert(snag_tool_result_valid(result) == 0);
        assert(strstr(snag_json_string(result, "model_text"), "read-only"));
        json_decref(result);
    }
    app.session.active_read_only = false;
    {
        char root[] = "/tmp/snajpagent-dispatch-XXXXXX";
        char probe[8192];
        FILE *out;
        assert(mkdtemp(root) != NULL);
        assert(snprintf(probe, sizeof(probe), "%s/probe.txt", root) > 0);
        out = fopen(probe, "w");
        assert(out && fputs("hello\n", out) >= 0 && fclose(out) == 0);
        app.session.cwd = root;
    }
    call.name = "read_file";
    json_decref(call.arguments);
    call.arguments = json_pack("{s:s}", "path", "missing.txt");
    assert(snag_app_tool_run(&app, &call, NULL, &result, error, sizeof(error)) == 0);
    assert(snag_tool_result_valid(result) == 0);
    assert(!strstr(snag_json_string(result, "model_text"), "only in /ro"));
    json_decref(result);
    call.name = "write_file";
    json_decref(call.arguments);
    call.arguments = json_pack("{s:s,s:s}", "path", "written.txt", "content", "x\n");
    assert(snag_app_tool_run(&app, &call, NULL, &result, error, sizeof(error)) == 0);
    assert(strcmp(snag_json_string(result, "status"), "succeeded") == 0);
    json_decref(result);
    json_decref(call.arguments);
}

static void
test_media_count_fallback(void)
{
    struct snag_config config;
    struct app_state app = {0};
    struct snag_credential credential;
    snag_config_init(&config); app.config = &config;
    app.turn_provider = &config.providers[0]; app.turn_model = "gpt-5.5";
    snag_credential_clear(&credential);
    assert(snag_ui_init(&app.ui) == 0);
    json_t *part = json_pack("{s:s,s:s,s:s}", "type", "input_image", "detail", "high",
        "image_url", "data:image/png;base64,YWJj");
    json_t *request = json_pack("{s:s,s:[{s:s,s:[O]}]}", "model", "gpt-5.5", "input", "role", "user", "content", part);
    uint64_t tokens = 0, expected = 0;
    const char *method = "qualified_upper_bound";
    char error[256] = {0};
    assert(snag_media_token_bound(request, 0u, &expected, error, sizeof(error)) == 0);
    config.providers[0].exact_token_count = SNAG_TOKEN_COUNT_OFF;
    assert(snag_app_provider_count(&app, request, &credential, &tokens, &method, error, sizeof(error)) == SNAG_APP_COUNT_SKIPPED);
    assert(tokens == expected && !strcmp(method, "media_upper_bound"));
    /* A configured per-image ceiling supersedes the generic budget on any route. */
    strcpy(config.providers[0].base_url, "https://api.deepseek.com");
    config.model_limit_count = 1u;
    strcpy(config.model_limits[0].provider, config.providers[0].name);
    config.model_limits[0].image_tokens = 1025u;
    assert(snag_media_token_bound(request, 1025u, &expected, error, sizeof(error)) == 0);
    assert(snag_app_provider_count(&app, request, &credential, &tokens, &method, error, sizeof(error)) == SNAG_APP_COUNT_SKIPPED);
    assert(tokens == expected && !strcmp(method, "media_upper_bound"));
    /* A media bound is a valid count method for compaction: the pre-response
     * and post-turn guards must accept it instead of failing the turn. */
    {
        bool compacted = true;
        assert(snag_app_compact_before_response(&app, &credential, tokens, method,
                                                &compacted, error, sizeof(error)) == 0);
        assert(!compacted);
        assert(snag_app_compact_after_turn(&app, tokens, method, error, sizeof(error)) == 0);
    }
    /* An exceeding upper bound does not prove actual overflow. Reproduce the
     * large-text plus one-image route without an exact token counter. */
    app.turn_capacity.hard_input_known = true;
    app.turn_capacity.hard_input_tokens = 258400u;
    {
        char *large = malloc(300001u);
        assert(large);
        memset(large, 'a', 300000u);
        large[300000] = '\0';
        json_t *parts = json_object_get(json_array_get(
            json_object_get(request, "input"), 0u), "content");
        assert(json_array_append_new(parts,
            json_pack("{s:s,s:s}", "type", "input_text", "text", large)) == 0);
        free(large);
        assert(snag_app_provider_count(&app, request, &credential, &tokens, &method,
            error, sizeof(error)) == SNAG_APP_COUNT_SKIPPED);
        assert(tokens > app.turn_capacity.hard_input_tokens &&
            !strcmp(method, "media_upper_bound"));
        bool compacted = false;
        char guard_error[256] = {0};
        int guard_rc = snag_app_compact_before_response(&app, &credential, tokens, method,
                                                        &compacted, guard_error, sizeof(guard_error));
        assert(guard_rc == 0);
        assert(!compacted);
        /* A real exact count above the same hard capacity remains guarded. */
        assert(snag_app_compact_before_response(&app, &credential, tokens, "exact",
            &compacted, guard_error, sizeof(guard_error)) != 0);
        assert(!compacted);
        assert(json_array_remove(parts, json_array_size(parts) - 1u) == 0);
    }
    app.turn_capacity.hard_input_known = false;
    strcpy(config.providers[0].base_url, "https://api.openai.com");
    config.model_limit_count = 0u;
    config.providers[0].exact_token_count = SNAG_TOKEN_COUNT_AUTO;
    app.turn_capacity.count_capability = SNAG_COUNT_UNSUPPORTED;
    /* Back on the generic image budget: recompute the expected bound. */
    assert(snag_media_token_bound(request, 0u, &expected, error, sizeof(error)) == 0);
    assert(snag_app_provider_count(&app, request, &credential, &tokens, &method, error, sizeof(error)) == SNAG_APP_COUNT_SKIPPED);
    /* Compressed payload length changes neither image budget nor textual count. */
    assert(json_object_set_new(part, "image_url", json_string("data:image/png;base64,YWJjYWJjYWJjYWJj")) == 0);
    assert(snag_app_provider_count(&app, request, &credential, &tokens, &method, error, sizeof(error)) == SNAG_APP_COUNT_SKIPPED && tokens == expected);
    assert(json_object_set_new(part, "detail", json_string("original")) == 0);
    assert(snag_app_provider_count(&app, request, &credential, &tokens, &method, error, sizeof(error)) < 0);
    assert(json_object_set_new(part, "detail", json_string("high")) == 0);
    /* Any model, including one absent from every table, budgets images. */
    assert(json_object_set_new(request, "model", json_string("unknown")) == 0);
    assert(snag_app_provider_count(&app, request, &credential, &tokens, &method, error, sizeof(error)) == SNAG_APP_COUNT_SKIPPED &&
           !strcmp(method, "media_upper_bound"));
    json_decref(part); json_decref(request);
    snag_ui_free(&app.ui); snag_config_free(&config);
}

static void
test_native_compaction_probe(void)
{
    struct snag_config config;
    struct snag_credential credential;
    struct local_server server;
    char error[256] = {0};
    const struct {
        enum model_fixture fixture;
        bool live;
        int expected;
    } cases[] = {
        {MODEL_COMPACT_OK, true, 1},
        {MODEL_COMPACT_404, true, 0},
        {MODEL_COMPACT_502, true, 0},
        {MODEL_COMPACT_OK, false, -1}, /* server stopped: no response at all */
    };

    snag_config_init(&config);
    config.providers[0].auth = SNAG_AUTH_API_KEY;
    strcpy(config.providers[0].base_url, "https://api.openai.com");
    credential_set(&credential, "probe-secret");
    credential.root_fd = -1;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        error[0] = '\0';
        start_server(&server, cases[i].fixture, false, "");
        assert(setenv("SNAJPAGENT_TEST_OPENAI_BASE", server.endpoint, 1) == 0);
        if (!cases[i].live) stop_server(&server);
        int rc = snag_provider_native_compaction_probe((struct snag_provider_connection){
            &config, &config.providers[0], &credential, NULL, NULL, NULL, NULL, 0},
            "gpt-5.5", error, sizeof(error));
        if (cases[i].live) stop_server(&server);
        assert(rc == cases[i].expected);
    }
    assert(unsetenv("SNAJPAGENT_TEST_OPENAI_BASE") == 0);
    snag_config_free(&config);
}

static void
test_local_audio_admission(void)
{
    struct app_state app = {0};
    struct snag_config config;
    bool handled = false;
    snag_config_init(&config); app.config = &config;
    assert(snag_ui_init(&app.ui) == 0);
    assert(snag_app_voice_command(&app, "/voice on", &handled) == 0 && handled && !app.voice);
    assert(snag_app_audio_command(&app, "/dictate", &handled) == 0 && handled && !app.audio);
    config.providers[0].auth = SNAG_AUTH_API_KEY;
    strcpy(config.audio.provider, config.providers[0].name);
    strcpy(config.audio.transcribe_model, "fixture");
    /* UI not opened: must reject before calling a device backend. */
    assert(snag_app_audio_command(&app, "/dictate", &handled) == 0 && handled && !app.audio);
    assert(snag_app_audio_command(&app, "/play /unaccepted.wav", &handled) == 0 && handled && !app.audio);
    assert(snag_app_audio_command(&app, "/play stop", &handled) == 0 && handled && !app.audio);
    assert(snag_app_audio_command(&app, "/other", &handled) == 0 && !handled);
    strcpy(config.audio.realtime_model, "fixture-realtime");
    strcpy(config.audio.voice, "fixture-voice");
    assert(snag_app_voice_command(&app, "/voice on", &handled) == 0 && handled && !app.voice);
    assert(snag_app_voice_command(&app, "/voice off", &handled) == 0 && handled && !app.voice);
    assert(snag_app_voice_command(&app, "/voice mute", &handled) == 0 && handled && !app.voice);
    assert(snag_app_voice_command(&app, "/other", &handled) == 0 && !handled);
    assert(snag_ui_insert_draft(&app.ui, "UI still works") == 0);
    snag_app_audio_close(&app); snag_ui_free(&app.ui); snag_config_free(&config);
}

static int voice_close_record(void *opaque,const struct snag_session *state,uint64_t seq,const char *type,const json_t *data,char *error,size_t size)
{
    (void)state;(void)seq;(void)error;(void)size;
    if(strcmp(type,"voice_event"))return 0;
    unsigned int *counts=opaque;
    const char *kind=snag_json_string(json_object_get(data,"event"),"type");
    if(!strcmp(kind,"voice_transcript"))++counts[0];
    else if(!strcmp(kind,"voice_stopped"))++counts[1];
    else assert(false);
    return 0;
}

static void test_voice_close(void)
{
    char path[4096],error[256],id[33];const char *tmp=getenv("TMPDIR");
    assert(snprintf(path,sizeof(path),"%s/snajpagent-voice-close-XXXXXX",tmp?tmp:"/tmp")>0);
    assert(mkdtemp(path));
    struct app_state app={0};
    struct snag_config config;snag_config_init(&config);app.config=&config;
    snag_store_init(&app.store);snag_session_init(&app.session);
    assert(snag_store_open(&app.store,path,error,sizeof(error))==0);
    assert(snag_ui_init(&app.ui)==0);
    for(unsigned int mode=0;mode<3u;++mode) {
        assert(snag_session_create(&app.store,&app.session,path,"default","fixture","medium",error,sizeof(error))==0);
        strcpy(id,app.session.id);
        json_t *events=json_pack("[{s:s},{s:s,s:s,s:s,s:s},{s:s},{s:s},{s:s}]",
            "type","voice_ready","type","voice_transcript","speaker","user","item_id","input-1","text","final words",
            "type","voice_handoff","type","voice_buffering","type","voice_expiring");
        assert(events && snag_app_voice_fixture(&app,events,mode!=0u)==0);json_decref(events);
        if(!mode)snag_app_voice_close(&app);
        else if(mode==1u) {
            bool handled=false;assert(snag_app_voice_command(&app,"/voice off",&handled)==0 && handled);
        } else assert(snag_app_voice_service(&app)==0);
        assert(!app.voice && !app.session.pending_queue_count);
        snag_session_close(&app.session);snag_session_init(&app.session);
        assert(snag_session_open(&app.store,&app.session,id,error,sizeof(error))==0);
        unsigned int counts[2]={0};
        assert(snag_session_each_event(&app.session,voice_close_record,counts,error,sizeof(error))==0);
        assert(counts[0]==1u && counts[1]==1u && !app.session.pending_queue_count);
        snag_app_voice_close(&app); /* Repeated close cannot append another stop. */
        snag_session_close(&app.session);snag_session_init(&app.session);
    }
    snag_ui_free(&app.ui);snag_store_close(&app.store);snag_config_free(&config);
}

static void
test_voice_transcript_labels(void)
{
    char path[4096];
    char error[256];
    const char *tmp = getenv("TMPDIR");
    assert(snprintf(path, sizeof(path), "%s/snajpagent-voice-labels-XXXXXX",
        tmp ? tmp : "/tmp") > 0 && mkdtemp(path));
    struct snag_config config;
    snag_config_init(&config);
    struct app_state app = {.config = &config};
    snag_store_init(&app.store);
    snag_session_init(&app.session);
    assert(snag_store_open(&app.store, path, error, sizeof(error)) == 0);
    assert(snag_session_create(&app.store, &app.session, path, "default", "fixture",
        "medium", error, sizeof(error)) == 0);

    for (unsigned int verbosity = 0u; verbosity <= 6u; ++verbosity) {
        int captured[2];
        assert(pipe(captured) == 0);
        int saved = dup(STDERR_FILENO);
        assert(saved >= 0 && dup2(captured[1], STDERR_FILENO) >= 0);
        assert(close(captured[1]) == 0);
        assert(snag_ui_init(&app.ui) == 0);
        assert(snag_ui_set_verbosity(&app.ui, verbosity) == 0);
        json_t *events = json_pack("[{s:s,s:s,s:s,s:s},{s:s,s:s,s:s,s:s}]",
            "type", "voice_transcript", "speaker", "user", "item_id", "input-label",
            "text", "show the -l option", "type", "voice_transcript", "speaker", "assistant",
            "item_id", "reply-label", "text", "192.0.2.1\n/fixture/file");
        assert(events && snag_app_voice_fixture(&app, events, false) == 0);
        json_decref(events);
        assert(snag_app_voice_service(&app) == 0);
        assert(!app.session.pending_queue_count && !app.session.active_turn);
        snag_app_voice_close(&app);
        snag_ui_free(&app.ui);
        assert(dup2(saved, STDERR_FILENO) >= 0 && close(saved) == 0);

        char output[8192];
        size_t used = 0u;
        ssize_t n;
        while ((n = read(captured[0], output + used, sizeof(output) - used - 1u)) > 0) {
            used += (size_t)n;
            assert(used < sizeof(output) - 1u);
        }
        assert(n == 0 && close(captured[0]) == 0);
        output[used] = '\0';
        assert(strstr(output, "You [voice, ASR]: show the -l option"));
        assert(strstr(output, "Voice model [generated]: 192.0.2.1\n/fixture/file"));
    }
    snag_session_close(&app.session);
    snag_store_close(&app.store);
    snag_config_free(&config);
}

static void
test_voice_concurrent_owner(void)
{
    char path[4096], error[256], queues[2][33];
    const char *tmp = getenv("TMPDIR");
    struct app_state app = {0};
    struct snag_config config;
    struct snag_credential credential;
    struct local_server server;

    assert(snprintf(path, sizeof(path), "%s/snajpagent-voice-calls-XXXXXX",
        tmp ? tmp : "/tmp") > 0 && mkdtemp(path));
    start_server(&server, MODEL_VOICE_QUEUE, false, "/v1");
    (void)transport_connection(&config, &credential, server.endpoint);
    config.providers[0].auth = SNAG_AUTH_API_KEY;
    assert(snag_secret_source_parse(&config.providers[0].api_key,
        "\"transport-secret\"", NULL, error, sizeof(error)) == 0);
    app.config = &config;
    snag_store_init(&app.store);
    snag_session_init(&app.session);
    assert(snag_ui_init(&app.ui) == 0);
    assert(snag_store_open(&app.store, path, error, sizeof(error)) == 0);
    assert(snag_session_create(&app.store, &app.session, path, "openai",
        "fixture", "medium", error, sizeof(error)) == 0);
    json_t *events = json_array();
    for (unsigned int i = 0; i < 2u; ++i) {
        const char *id = i ? "second" : "first";
        assert(json_array_append_new(events,
            json_pack("{s:s,s:s,s:s,s:s,s:s,s:s}", "type", "voice_handoff",
                "input_id", id, "response_id", id, "call_id", id,
                "transcript", id, "request", id)) == 0);
    }
    assert(snag_app_voice_fixture(&app, events, false) == 0);
    json_decref(events);
    unsigned int acknowledgements = 0u;
    uint64_t started = snag_monotonic_ms();
    while (acknowledgements < 2u && snag_monotonic_ms() - started < 5000u) {
        assert(snag_app_voice_service(&app) == 0 && app.voice);
        json_t *reply = snag_app_voice_fixture_result(&app);
        if (reply) {
            assert(!strcmp(snag_json_string(reply, "call_id"),
                acknowledgements ? "second" : "first"));
            assert(json_is_false(json_object_get(reply, "final")));
            ++acknowledgements;
            json_decref(reply);
        }
        snag_sleep_ms(10u);
    }
    assert(acknowledgements == 2u && app.session.pending_queue_count == 2u);
    assert(app.session.queue_armed);
    for (size_t i = 0; i < 2u; ++i) {
        strcpy(queues[i], app.session.pending_queue[i].queue_id);
    }

    /* Both results can become ready before the audio owner consumes either. */
    for (size_t i = 2u; i-- > 0u;) {
        json_t *cancel = json_pack("{s:[s],s:s}", "queue_ids", queues[i],
            "reason", "user");
        assert(snag_session_commit(&app.session, "future_turn_cancelled",
            json_incref(cancel), NULL, error, sizeof(error)) == 0);
        snag_app_voice_event(&app, "future_turn_cancelled", cancel);
        json_decref(cancel);
    }
    assert(snag_app_voice_service(&app) == 0 && app.voice);
    uint64_t seq = app.session.next_seq;
    assert(snag_app_voice_service(&app) == 0 && app.voice);
    assert(app.session.next_seq == seq);
    json_t *result = snag_app_voice_fixture_result(&app);
    assert(result && !strcmp(snag_json_string(result, "call_id"), "first"));
    assert(strstr(snag_json_string(result, "text"), "cancelled"));
    json_decref(result);
    assert(snag_app_voice_service(&app) == 0 && app.voice);
    result = snag_app_voice_fixture_result(&app);
    assert(result && !strcmp(snag_json_string(result, "call_id"), "second"));
    assert(strstr(snag_json_string(result, "text"), "cancelled"));
    json_decref(result);
    seq = app.session.next_seq;
    assert(snag_app_voice_service(&app) == 0 && app.voice);
    assert(!snag_app_voice_fixture_result(&app) && app.session.next_seq == seq);
    snag_app_voice_close(&app);
    snag_session_close(&app.session);
    snag_store_close(&app.store);
    snag_ui_free(&app.ui);
    snag_config_free(&config);
    snag_credential_clear(&credential);
    stop_server(&server);
}

static void
test_voice_read_tools(void)
{
    char path[4096], file[4096], error[256];
    const char *tmp = getenv("TMPDIR");
    struct app_state app = {0};
    struct snag_config config;
    struct snag_response_item call = {.kind = SNAG_ITEM_TOOL_CALL};
    json_t *result = NULL;

    assert(snprintf(path, sizeof(path), "%s/snajpagent-voice-read-XXXXXX",
        tmp ? tmp : "/tmp") > 0 && mkdtemp(path));
    assert(snprintf(file, sizeof(file), "%s/AGENTS.md", path) > 0);
    FILE *out = fopen(file, "w");
    assert(out && fputs("Existing project instructions.\n", out) >= 0 && fclose(out) == 0);
    snag_config_init(&config);
    app.config = &config;
    snag_store_init(&app.store);
    snag_session_init(&app.session);
    assert(snag_store_open(&app.store, path, error, sizeof(error)) == 0);
    assert(snag_session_create(&app.store, &app.session, path, "default",
        "fixture", "medium", error, sizeof(error)) == 0);
    uint64_t seq = app.session.next_seq;
    json_t *tools = snag_app_voice_tools();
    assert(tools && json_array_size(tools) == 9u);
    json_t *input = json_pack("[{s:s,s:s},{s:s,s:s},{s:s,s:s}]",
        "role", "developer", "content", "instructions",
        "role", "developer", "content", "host context",
        "role", "user", "content", "spoken request");
    for (unsigned int kind = 0; kind < 3u; ++kind) {
        config.providers[0].auth = kind == 1u ? SNAG_AUTH_CHATGPT : SNAG_AUTH_API_KEY;
        config.providers[0].leading_instructions = kind == 2u;
        json_t *request = snag_context_interface_request(&app.session, &config.providers[0],
            "fixture", "medium", input, tools);
        assert(request && json_array_size(input) == 3u);
        assert(strlen(snag_json_string(request, "prompt_cache_key")) == SNAG_CACHE_KEY_LEN);
        assert(!strcmp(snag_json_string(request, "model"), "fixture"));
        assert(json_is_true(json_object_get(request, "stream")));
        if (kind == 1u) {
            assert(json_is_string(json_object_get(request, "instructions")));
            assert(!json_object_get(request, "truncation"));
        }
        if (kind == 2u) {
            json_t *normalized = json_object_get(request, "input");
            assert(json_array_size(normalized) == 2u);
            assert(!strcmp(snag_json_string(json_array_get(normalized, 0u), "role"), "system"));
        }
        json_decref(request);
    }
    json_decref(input);
    assert(!snag_context_read_tool_schema("exec_command"));
    assert(!snag_context_read_tool_schema("write_file"));
    assert(!snag_context_read_tool_schema(NULL));
    for (size_t i = 0; i < 4u; ++i) {
        const char *name = snag_json_string(json_array_get(tools, i), "name");
        json_t *shared = snag_context_read_tool_schema(name);
        assert(shared && json_equal(shared, json_array_get(tools, i)));
        json_decref(shared);
    }
    json_decref(tools);
    call.name = "get_cwd";
    call.arguments = json_object();
    assert(snag_app_voice_read(&app, &call, &result, error, sizeof(error)) == 0);
    assert(!strcmp(snag_json_string(result, "model_text"), path));
    json_decref(result);
    call.name = "inspect_session";
    assert(snag_app_voice_read(&app, &call, &result, error, sizeof(error)) == 0);
    const char *text = snag_json_string(result, "model_text");
    json_t *context = json_loads(text, 0, NULL);
    assert(context && !strcmp(snag_json_string(context, "cwd"), path));
    assert(!strcmp(snag_json_string(context, "session_id"), app.session.id));
    const json_t *paths = json_object_get(context, "instructions");
    bool found = false;
    for (size_t i = 0; i < json_array_size(paths); ++i) {
        if (!strcmp(json_string_value(json_array_get(paths, i)), file)) found = true;
    }
    assert(found);
    json_decref(context);
    json_decref(result);
    json_decref(call.arguments);
    call.name = "read_file";
    call.arguments = json_pack("{s:s}", "path", "AGENTS.md");
    assert(snag_app_voice_read(&app, &call, &result, error, sizeof(error)) == 0);
    assert(strstr(snag_json_string(result, "model_text"), "Existing project instructions."));
    json_decref(result);
    json_decref(call.arguments);
    call.name = "write_file";
    call.arguments = json_pack("{s:s,s:s}", "path", "AGENTS.md", "content", "changed");
    assert(snag_app_voice_read(&app, &call, &result, error, sizeof(error)) == 0);
    assert(!strcmp(snag_json_string(result, "status"), "failed"));
    json_decref(result);
    call.name = "exec_command";
    assert(snag_app_voice_read(&app, &call, &result, error, sizeof(error)) == 0);
    assert(!strcmp(snag_json_string(result, "status"), "failed"));
    json_decref(result);
    json_decref(call.arguments);
    call.name = "read_file";
    call.arguments = json_pack("{s:s}", "path", "AGENTS.md");
    assert(snag_app_voice_read(&app, &call, &result, error, sizeof(error)) == 0);
    assert(strstr(snag_json_string(result, "model_text"), "Existing project instructions."));
    assert(app.session.next_seq == seq && !app.session.pending_queue_count);
    json_decref(result);
    json_decref(call.arguments);
    snag_session_close(&app.session);
    snag_store_close(&app.store);
    snag_config_free(&config);
}

static void
test_voice_interface_read(void)
{
    struct app_state app = {0};
    struct snag_config config;
    struct snag_credential credential;
    struct local_server server;
    char path[4096], file[4096], error[512];
    const char *tmp = getenv("TMPDIR");

    assert(snprintf(path, sizeof(path), "%s/snajpagent-voice-interface-XXXXXX",
        tmp ? tmp : "/tmp") > 0 && mkdtemp(path));
    assert(snprintf(file, sizeof(file), "%s/AGENTS.md", path) > 0);
    int fd = open(file, O_CREAT | O_EXCL | O_RDWR, 0600);
    assert(fd >= 0 && write(fd, "unchanged\n", 10u) == 10);
    start_server(&server, MODEL_VOICE_READ, false, "/v1");
    (void)transport_connection(&config, &credential, server.endpoint);
    config.providers[0].auth = SNAG_AUTH_API_KEY;
    assert(snag_secret_source_parse(&config.providers[0].api_key,
        "\"transport-secret\"", NULL, error, sizeof(error)) == 0);
    app.config = &config;
    snag_store_init(&app.store);
    snag_session_init(&app.session);
    assert(snag_ui_init(&app.ui) == 0);
    assert(snag_store_open(&app.store, path, error, sizeof(error)) == 0);
    assert(snag_session_create(&app.store, &app.session, path, "openai", "fixture",
        "medium", error, sizeof(error)) == 0);
    json_t *events = json_pack("[{s:s,s:s,s:s,s:s,s:s,s:s}]", "type", "voice_handoff",
        "input_id", "read_input", "response_id", "read_response", "call_id", "read_call",
        "transcript", "Which directory is this?", "request", "Read the session cwd.");
    assert(snag_app_voice_fixture(&app, events, false) == 0);
    json_decref(events);
    json_t *reply = NULL;
    uint64_t start = snag_monotonic_ms();
    while (!reply && snag_monotonic_ms() - start < 5000u) {
        assert(snag_app_voice_service(&app) == 0 && app.voice);
        reply = snag_app_voice_fixture_result(&app);
        if (!reply) snag_sleep_ms(10u);
    }
    assert(reply && !strcmp(snag_json_string(reply, "call_id"), "read_call"));
    assert(json_is_true(json_object_get(reply, "final")));
    assert(strstr(snag_json_string(reply, "text"), "transport"));
    assert(!app.session.pending_queue_count && !app.session.active_turn);
    char contents[10];
    assert(lseek(fd, 0, SEEK_SET) == 0 && read(fd, contents, sizeof(contents)) == 10);
    assert(!memcmp(contents, "unchanged\n", 10u));
    close(fd);
    json_decref(reply);
    snag_app_voice_close(&app);
    snag_session_close(&app.session);
    snag_store_close(&app.store);
    snag_ui_free(&app.ui);
    snag_config_free(&config);
    snag_credential_clear(&credential);
    stop_server(&server);
}

static void
test_voice_independent_request(void)
{
    for (unsigned int mode = 0; mode < 3u; ++mode) {
        struct local_server server;
        struct app_state app = {0};
        struct snag_config config;
        struct snag_credential credential;
        struct snag_response_graph graph = {0};
        char path[4096], error[512];
        const char *tmp = getenv("TMPDIR");

        assert(snprintf(path, sizeof(path), "%s/snajpagent-voice-request-XXXXXX",
            tmp ? tmp : "/tmp") > 0 && mkdtemp(path));
        int received[2] = {-1, -1};
        if (mode == 2u) {
            assert(pipe(received) == 0);
            voice_request_ready_fd = received[1];
        }
        start_server(&server, mode == 2u ? MODEL_VOICE_REQUEST_WAIT :
            mode == 1u ? MODEL_CREATE_TYPELESS : MODEL_VOICE_REQUEST, false, "/v1");
        if (mode == 2u) {
            assert(close(received[1]) == 0);
            voice_request_ready_fd = -1;
        }
        (void)transport_connection(&config, &credential, server.endpoint);
        config.providers[0].auth = SNAG_AUTH_API_KEY;
        assert(snag_secret_source_parse(&config.providers[0].api_key,
            "\"transport-secret\"", NULL, error, sizeof(error)) == 0);
        app.config = &config;
        snag_store_init(&app.store);
        snag_session_init(&app.session);
        assert(snag_ui_init(&app.ui) == 0);
        assert(snag_store_open(&app.store, path, error, sizeof(error)) == 0);
        assert(snag_session_create(&app.store, &app.session, path, config.providers[0].name,
            "fixture", "medium", error, sizeof(error)) == 0);
        json_t *notices = json_pack("[{s:s,s:s,s:s,s:s}]", "type", "voice_transcript",
            "speaker", "user", "item_id", "input", "text", "still listening");
        assert(snag_app_voice_fixture(&app, notices, false) == 0);
        json_decref(notices);
        json_t *request = request_with_marker("get_cwd");
        assert(snag_app_voice_request_start(&app, request, error, sizeof(error)) == 0);
        assert(snag_app_voice_request_start(&app, request, error, sizeof(error)) < 0);
        json_decref(request);
        /* Servicing audio notices does not wait for the independent response. */
        uint64_t begin = snag_monotonic_ms();
        assert(snag_app_voice_service(&app) == 0 && app.voice);
        assert(snag_monotonic_ms() - begin < 1000u);
        if (mode == 2u) {
            /* Closing immediately after thread creation could cancel before
             * any network I/O. Exercise a request already waiting for a reply. */
            struct pollfd ready = {.fd = received[0], .events = POLLIN};
            assert(poll(&ready, 1u, 5000) == 1 && (ready.revents & POLLIN));
            char reply;
            assert(read(received[0], &reply, 1u) == 1 && reply == 'R');
            assert(close(received[0]) == 0);
            begin = snag_monotonic_ms();
            snag_app_voice_close(&app);
            assert(snag_monotonic_ms() - begin < 1000u && !app.voice);
        } else {
            int ready = 0, outcome = 0;
            uint64_t seq = app.session.next_seq;
            while (!ready && snag_monotonic_ms() - begin < 5000u) {
                ready = snag_app_voice_request_take(&app, &graph, &outcome,
                    error, sizeof(error));
                assert(ready >= 0);
                if (!ready) snag_sleep_ms(10u);
            }
            assert(ready == 1 && app.voice && app.session.next_seq == seq);
            if (mode == 1u) {
                assert(outcome < 0 && strstr(error, "Responses event has no type"));
                assert(!strstr(error, "private-value"));
            } else {
                assert(outcome == 0 && graph.count == 1u);
                assert(!strcmp(graph.provider_response_id, "resp_transport"));
            }
            snag_response_graph_free(&graph);
            assert(snag_app_voice_request_take(&app, &graph, &outcome,
                error, sizeof(error)) < 0);
            snag_app_voice_close(&app);
        }
        snag_session_close(&app.session);
        snag_store_close(&app.store);
        snag_ui_free(&app.ui);
        snag_config_free(&config);
        snag_credential_clear(&credential);
        stop_server(&server);
    }
}

static void
test_voice_session_controls(void)
{
    struct app_state app = {0};
    struct snag_config config;
    char path[4096], error[512] = {0}, id[33], first[33], session_id[33];
    const char *tmp = getenv("TMPDIR");
    const char *turn = "0123456789abcdef0123456789abcdef";
    json_t *result = NULL;

    assert(snprintf(path, sizeof(path), "%s/snajpagent-voice-controls-XXXXXX",
        tmp ? tmp : "/tmp") > 0 && mkdtemp(path));
    snag_config_init(&config);
    app.config = &config;
    snag_store_init(&app.store);
    snag_session_init(&app.session);
    assert(snag_ui_init(&app.ui) == 0);
    assert(snag_store_open(&app.store, path, error, sizeof(error)) == 0);
    assert(snag_session_create(&app.store, &app.session, path, "openai", "fixture",
        "medium", error, sizeof(error)) == 0);
    strcpy(session_id, app.session.id);
    json_t *source = json_pack("{s:s,s:s,s:s,s:s,s:s,s:s,s:s,s:s}",
        "connection_id", turn, "input_id", "spoken_input", "response_id", "spoken_response",
        "call_id", "spoken_call", "provider", "openai", "model", "fixture",
        "transcript", "Změň prosím tento úkol.", "request", "Change the current task.");
    assert(source);
    uint64_t seq = app.session.next_seq;
    assert(snag_app_voice_submit(&app, source, turn, id, &result, error, sizeof(error)) == 0);
    assert(!strcmp(snag_json_string(result, "status"), "failed"));
    assert(app.session.next_seq == seq && !id[0]);
    json_decref(result);

    json_t *started = json_pack("{s:{s:s,s:s,s:n,s:s,s:s,s:s,s:i,s:i,s:i,s:i,s:b},"
        "s:s,s:b,s:o,s:n,s:n,s:s,s:s,s:I,s:s}",
        "config", "capability_version", SNAJPAGENT_CAPABILITY_VERSION, "effort", "medium",
        "max_output_tokens", "model", "fixture", "provider", "openai",
        "profile_id", SNAJPAGENT_PROFILE_ID, "prompt_schema", 1, "replay_schema", 1,
        "tool_schema", 1, "max_parallel_commands", 4, "parallel_tool_calls", 1,
        "input_kind", "direct", "read_only", 0, "instructions", json_array(),
        "queue_id", "queue_seq", "text", "Existing task", "turn_id", turn,
        "turn_number", (json_int_t)1, "cwd", app.session.cwd);
    assert(started && snag_session_commit(&app.session, "turn_started", started,
        NULL, error, sizeof(error)) == 0);
    app.ui.input_received_ms = 123u;
    assert(snag_app_voice_submit(&app, source, turn, id, &result, error, sizeof(error)) == 0);
    assert(!strcmp(snag_json_string(result, "status"), "succeeded"));
    assert(app.steering_requested && app.session.pending_steering_count == 1u);
    assert(!app.session.pending_queue_count && app.ui.input_received_ms == 123u);
    assert(strstr(app.session.pending_steering[0].text, "Změň prosím tento úkol."));
    assert(strstr(app.session.pending_steering[0].text, "derived context"));
    assert(app.session.pending_steering[0].received_ms != 123u);
    strcpy(first, id);
    json_decref(result);
    seq = app.session.next_seq;
    app.steering_requested = false;
    assert(snag_app_voice_submit(&app, source, turn, id, &result, error, sizeof(error)) == 0);
    assert(!strcmp(first, id) && !app.steering_requested && app.session.next_seq == seq);
    json_decref(result);
    assert(json_object_set_new(source, "transcript", json_string("Different input")) == 0);
    assert(snag_app_voice_submit(&app, source, turn, id, &result, error, sizeof(error)) == 0);
    assert(!strcmp(snag_json_string(result, "status"), "failed"));
    assert(app.session.next_seq == seq && app.session.pending_steering_count == 1u);
    json_decref(result);
    assert(json_object_set_new(source, "transcript", json_string("Změň prosím tento úkol.")) == 0);
    assert(json_object_set_new(source, "input_id", json_string("later_input")) == 0);
    app.session.steering_deferred = true;
    assert(snag_app_voice_submit(&app, source, turn, id, &result, error, sizeof(error)) == 0);
    assert(!app.steering_requested && app.session.pending_steering_count == 2u);
    assert(strstr(snag_json_string(result, "model_text"), "next permitted boundary"));
    json_decref(result);
    app.session.steering_deferred = false;

    assert(snag_app_voice_submit(&app, source, "queue", id, &result, error, sizeof(error)) == 0);
    assert(app.session.pending_queue_count == 1u && app.session.queue_armed);
    assert(strstr(snag_json_string(result, "model_text"), "queued"));
    json_decref(result);
    strcpy(first, id);
    seq = app.session.next_seq;
    assert(snag_app_voice_submit(&app, source, "queue", id, &result, error, sizeof(error)) == 0);
    assert(!strcmp(first, id) && app.session.next_seq == seq);
    json_decref(result);
    assert(snag_session_commit(&app.session, "future_turn_cancelled",
        json_pack("{s:[s],s:s}", "queue_ids", id, "reason", "user"),
        NULL, error, sizeof(error)) == 0);
    assert(snag_app_voice_submit(&app, source, "queue", id, &result, error, sizeof(error)) == 0);
    assert(!app.session.pending_queue_count && !app.session.queue_armed);
    json_decref(result);

    seq = app.session.next_seq;
    assert(snag_app_voice_interrupt(&app, "stale_turn", &result, error, sizeof(error)) == 0);
    assert(!strcmp(snag_json_string(result, "status"), "failed"));
    assert(!app.interrupt_requested && app.session.next_seq == seq);
    json_decref(result);
    assert(snag_app_voice_interrupt(&app, turn, &result, error, sizeof(error)) == 0);
    assert(app.interrupt_requested && app.session.cancel_requested);
    json_decref(result);
    seq = app.session.next_seq;
    assert(snag_app_voice_interrupt(&app, turn, &result, error, sizeof(error)) == 0);
    assert(app.session.next_seq == seq);
    json_decref(result);

    snag_session_close(&app.session);
    assert(snag_session_open(&app.store, &app.session, session_id, error, sizeof(error)) == 0);
    assert(app.session.pending_steering_count == 2u && app.session.cancel_requested);
    seq = app.session.next_seq;
    assert(snag_app_voice_submit(&app, source, turn, id, &result, error, sizeof(error)) == 0);
    assert(app.session.next_seq == seq && app.session.pending_steering_count == 2u);
    json_decref(result);
    json_decref(source);
    snag_session_close(&app.session);
    snag_store_close(&app.store);
    snag_ui_free(&app.ui);
    snag_config_free(&config);
}

static void
test_voice_output_tool(void)
{
    char path[4096], error[256];
    const char *tmp = getenv("TMPDIR");
    struct app_state app = {0};
    struct snag_config config;
    struct snag_response_item call = {.kind = SNAG_ITEM_TOOL_CALL, .name = "voice_output"};
    json_t *result = NULL;
    snag_config_init(&config);
    app.config = &config;
    snag_store_init(&app.store);
    snag_session_init(&app.session);
    assert(snprintf(path, sizeof(path), "%s/snajpagent-voice-output-XXXXXX",
        tmp ? tmp : "/tmp") > 0 && mkdtemp(path));
    assert(snag_ui_init(&app.ui) == 0);
    assert(snag_store_open(&app.store, path, error, sizeof(error)) == 0);
    assert(snag_session_create(&app.store, &app.session, path, "default",
        "fixture", "medium", error, sizeof(error)) == 0);
    call.arguments = json_object();
    uint64_t seq = app.session.next_seq;
    assert(snag_app_tool_run(&app, &call, NULL, &result, error, sizeof(error)) == 0);
    assert(strstr(snag_json_string(result, "model_text"), "\"ready\":false"));
    json_decref(result);
    assert(json_object_set_new(call.arguments, "text", json_string("The build finished.")) == 0);
    assert(snag_app_tool_run(&app, &call, NULL, &result, error, sizeof(error)) == 0);
    assert(strstr(snag_json_string(result, "model_text"), "unavailable"));
    assert(!app.voice && app.session.next_seq == seq);
    json_decref(result);
    assert(snag_app_voice_fixture(&app, NULL, false) == 0);
    assert(snag_app_voice_fixture_mute(&app) == 0);
    app.session.active_read_only = true;
    assert(snag_app_tool_run(&app, &call, NULL, &result, error, sizeof(error)) == 0);
    assert(strstr(snag_json_string(result, "model_text"), "Read-only"));
    assert(app.session.next_seq == seq && !snag_app_voice_fixture_result(&app));
    json_decref(result);
    app.session.active_read_only = false;
    assert(snag_app_tool_run(&app, &call, NULL, &result, error, sizeof(error)) == 0);
    assert(strstr(snag_json_string(result, "model_text"), "queued; playback is not confirmed"));
    json_decref(result);
    seq = app.session.next_seq;
    assert(snag_app_tool_run(&app, &call, NULL, &result, error, sizeof(error)) == 0);
    assert(strstr(snag_json_string(result, "model_text"), "not accepted"));
    assert(app.session.next_seq == seq && !app.session.pending_queue_count);
    json_decref(result);
    result = snag_app_voice_fixture_result(&app);
    assert(json_is_true(json_object_get(result, "standalone")));
    assert(!strcmp(snag_json_string(result, "text"), "Model output:\nThe build finished."));
    json_decref(result);
    assert(snag_app_tool_run(&app, &call, NULL, &result, error, sizeof(error)) == 0);
    json_decref(result);
    snag_app_voice_close(&app);
    assert(!app.voice);
    json_decref(call.arguments);
    snag_session_close(&app.session);
    snag_store_close(&app.store);
    snag_ui_free(&app.ui);
    snag_config_free(&config);
}

static void test_voice_owner_mute(void)
{
    struct app_state app={0};
    assert(snag_app_voice_fixture(&app,NULL,false)==0);
    assert(snag_app_voice_fixture_mute(&app)==0);
    /* The close fixture separately tests durable notice draining. This owner
     * fixture needs a real private session for that same close path. */
    char path[4096],error[256];const char *tmp=getenv("TMPDIR");
    assert(snprintf(path,sizeof(path),"%s/snajpagent-voice-mute-XXXXXX",tmp?tmp:"/tmp")>0 && mkdtemp(path));
    struct snag_config config;snag_config_init(&config);app.config=&config;
    snag_store_init(&app.store);snag_session_init(&app.session);assert(snag_ui_init(&app.ui)==0);
    assert(snag_store_open(&app.store,path,error,sizeof(error))==0);
    assert(snag_session_create(&app.store,&app.session,path,"default","fixture","medium",error,sizeof(error))==0);
    snag_app_voice_close(&app);snag_session_close(&app.session);snag_store_close(&app.store);
    snag_ui_free(&app.ui);snag_config_free(&config);
}

static void
test_goal_tool_manipulates_unfinished_goals(void)
{
    struct snag_config config;
    struct app_state app = {0};
    struct snag_response_item call = {0};
    json_t *result = NULL;
    char error[256] = {0};

    snag_config_init(&config);
    app.config = &config;
    call.kind = SNAG_ITEM_TOOL_CALL;
    call.name = "update_goal";

    /* A locked goal is frozen for the model: the operator lock, not the status,
     * is what keeps the model from changing the objective, and the refusal names
     * the lock so the model can report why it stopped. */
    app.session.goal_status = SNAG_GOAL_BLOCKED;
    app.session.goal_locked = true;
    call.arguments = json_pack("{s:s,s:s}", "action", "rewrite", "text", "reworded objective");
    assert(snag_app_tool_run(&app, &call, NULL, &result, error, sizeof(error)) == 0);
    assert(strstr(snag_json_string(result, "model_text"), "locked by the operator"));
    json_decref(result);
    json_decref(call.arguments);

    /* resume mirrors the /goal command: only a paused or blocked goal, text null. */
    app.session.goal_locked = false;
    call.arguments = json_pack("{s:s,s:s}", "action", "resume", "text", "extra");
    assert(snag_app_tool_run(&app, &call, NULL, &result, error, sizeof(error)) == 0);
    assert(strstr(snag_json_string(result, "model_text"), "resume requires text to be null"));
    json_decref(result);
    json_decref(call.arguments);

    app.session.goal_status = SNAG_GOAL_ACTIVE;
    call.arguments = json_pack("{s:s}", "action", "resume");
    assert(snag_app_tool_run(&app, &call, NULL, &result, error, sizeof(error)) == 0);
    assert(strstr(snag_json_string(result, "model_text"), "only a paused or blocked goal can be resumed"));
    json_decref(result);
    json_decref(call.arguments);

    /* A finished goal reports the unfinished-goal gate, not an active one. */
    app.session.goal_status = SNAG_GOAL_COMPLETED;
    call.arguments = json_pack("{s:s,s:s}", "action", "block", "text", "done");
    assert(snag_app_tool_run(&app, &call, NULL, &result, error, sizeof(error)) == 0);
    assert(strstr(snag_json_string(result, "model_text"), "there is no unfinished goal to update"));
    json_decref(result);
    json_decref(call.arguments);
    snag_config_free(&config);
}

static json_t *
history_checkpoint_document(void *opaque, const struct snag_session *session)
{
    (void)session;
    return json_incref(opaque);
}

static void
test_history_and_goal_list_tools(void)
{
    char path[4096], error[256] = {0};
    const char *tmp = getenv("TMPDIR");
    const char *old_goal = "61000000000000000000000000000000";
    const char *new_goal = "62000000000000000000000000000000";
    struct app_state app = {0};
    struct snag_response_item call = {.kind = SNAG_ITEM_TOOL_CALL};
    json_t *result = NULL;

    assert(snprintf(path, sizeof(path), "%s/snajpagent-history-tools-XXXXXX",
                    tmp ? tmp : "/tmp") > 0);
    assert(mkdtemp(path));
    snag_store_init(&app.store);
    snag_session_init(&app.session);
    assert(snag_store_open(&app.store, path, error, sizeof(error)) == 0);
    assert(snag_session_create(&app.store, &app.session, path, "default", "fixture", "medium",
                               error, sizeof(error)) == 0);
    app.session.tool_output_bytes = 2048u;

    uint64_t started_seq = app.session.next_seq;
    assert(snag_session_commit(&app.session, "goal_started",
        json_pack("{s:s,s:s}", "goal_id", old_goal, "prompt", "original objective"),
        NULL, error, sizeof(error)) == 0);
    uint64_t replaced_seq = app.session.next_seq;
    assert(snag_session_commit(&app.session, "goal_replaced",
        json_pack("{s:s,s:s,s:s,s:s}", "actor", "user", "goal_id", old_goal,
                  "new_goal_id", new_goal, "prompt", "replacement objective"),
        NULL, error, sizeof(error)) == 0);
    uint64_t completed_seq = app.session.next_seq;
    assert(snag_session_commit(&app.session, "goal_completed",
        json_pack("{s:s,s:s}", "actor", "user", "goal_id", new_goal),
        NULL, error, sizeof(error)) == 0);

    call.name = "read_session_history";
    call.arguments = json_pack("{s:i,s:i}", "limit", 2, "detail_bytes", 256);
    assert(snag_app_tool_run(&app, &call, NULL, &result, error, sizeof(error)) == 0);
    const char *text = snag_json_string(result, "model_text");
    assert(text && strstr(text, "returned=2") && strstr(text, "order=newest-first"));
    char completed[64], replaced[64], cursor[64];
    assert(snprintf(completed, sizeof(completed), "%llu goal_completed",
                    (unsigned long long)completed_seq) > 0);
    assert(snprintf(replaced, sizeof(replaced), "%llu goal_replaced",
                    (unsigned long long)replaced_seq) > 0);
    assert(snprintf(cursor, sizeof(cursor), "next_before_seq=%llu",
                    (unsigned long long)replaced_seq) > 0);
    assert(strstr(text, completed) < strstr(text, replaced));
    assert(strstr(text, cursor));
    json_decref(result);
    json_decref(call.arguments);

    call.arguments = json_pack("{s:I,s:i}", "before_seq", (json_int_t)replaced_seq, "limit", 50);
    assert(snag_app_tool_run(&app, &call, NULL, &result, error, sizeof(error)) == 0);
    text = snag_json_string(result, "model_text");
    char started[64];
    assert(snprintf(started, sizeof(started), "%llu goal_started",
                    (unsigned long long)started_seq) > 0);
    assert(text && strstr(text, started) && !strstr(text, "goal_replaced"));
    json_decref(result);
    json_decref(call.arguments);

    call.name = "list_goals";
    call.arguments = json_pack("{s:i}", "limit", 1);
    assert(snag_app_tool_run(&app, &call, NULL, &result, error, sizeof(error)) == 0);
    text = snag_json_string(result, "model_text");
    assert(text && strstr(text, new_goal) && strstr(text, "status=completed") &&
           strstr(text, "parent=61000000000000000000000000000000") &&
           !strstr(text, "id=61000000000000000000000000000000 status=replaced"));
    assert(snprintf(cursor, sizeof(cursor), "next_before_seq=%llu",
                    (unsigned long long)replaced_seq) > 0);
    assert(strstr(text, cursor));
    json_decref(result);
    json_decref(call.arguments);

    call.arguments = json_pack("{s:I,s:i}", "before_seq", (json_int_t)replaced_seq, "limit", 1);
    assert(snag_app_tool_run(&app, &call, NULL, &result, error, sizeof(error)) == 0);
    text = snag_json_string(result, "model_text");
    assert(text && strstr(text, old_goal) && strstr(text, "status=replaced") &&
           strstr(text, "replaced_by=62000000000000000000000000000000") &&
           !strstr(text, "id=62000000000000000000000000000000"));
    json_decref(result);
    json_decref(call.arguments);

    call.arguments = json_pack("{s:i}", "limit", 51);
    assert(snag_app_tool_run(&app, &call, NULL, &result, error, sizeof(error)) == 0);
    assert(!strcmp(snag_json_string(result, "status"), "failed"));
    json_decref(result);
    json_decref(call.arguments);

    /* A recent page must not replay an unrelated old prefix. */
    assert(snag_session_checkpoint(&app.session, error, sizeof(error)) == 0);
    assert(snag_session_commit(&app.session, "effort_changed",
        json_pack("{s:s,s:s}", "old_effort", "medium", "new_effort", "high"),
        NULL, error, sizeof(error)) == 0);
    uint64_t tail_seq = app.session.next_seq - 1u;
    unsigned char original;
    assert(pread(app.session.log_fd, &original, 1u, 0) == 1);
    int writer = openat(app.session.dir_fd, "events.jsonl", O_WRONLY | O_CLOEXEC);
    assert(writer >= 0 && pwrite(writer, "X", 1u, 0) == 1);
    call.name = "read_session_history";
    call.arguments = json_pack("{s:i,s:i}", "limit", 1, "detail_bytes", 512);
    assert(snag_app_tool_run(&app, &call, NULL, &result, error, sizeof(error)) == 0);
    text = snag_json_string(result, "model_text");
    assert(text && strstr(text, "returned=1") && strstr(text, "effort_changed"));
    json_decref(result);
    json_decref(call.arguments);
    call.arguments = json_pack("{s:I,s:i}", "before_seq", (json_int_t)tail_seq, "limit", 1);
    assert(snag_app_tool_run(&app, &call, NULL, &result, error, sizeof(error)) == 0);
    text = snag_json_string(result, "model_text");
    assert(text && strstr(text, "session_checkpoint") && strstr(text, "provider_view"));
    assert(!strstr(text, "\"state\"") && !strstr(text, "\"context\""));
    json_decref(result);
    json_decref(call.arguments);
    assert(pwrite(writer, &original, 1u, 0) == 1 && close(writer) == 0);

    call.arguments = json_pack("{s:[s],s:i}", "event_types", "goal_started", "limit", 2);
    assert(snag_app_tool_run(&app, &call, NULL, &result, error, sizeof(error)) == 0);
    text = snag_json_string(result, "model_text");
    assert(text && strstr(text, "returned=1") && strstr(text, started));
    assert(!strstr(text, "goal_replaced") && !strstr(text, "effort_changed"));
    assert(strstr(text, "scan_complete=true") && strstr(text, "next_before_seq=0"));
    json_decref(result);
    json_decref(call.arguments);

    /* A valid request that cannot fit a record is not an empty/end page. */
    app.session.tool_output_bytes = 256u;
    for (size_t i = 0u; i < 2u; ++i) {
        call.name = i ? "list_goals" : "read_session_history";
        call.arguments = json_pack("{s:i}", "limit", 1);
        assert(snag_app_tool_run(&app, &call, NULL, &result, error, sizeof(error)) == 0);
        assert(!strcmp(snag_json_string(result, "status"), "failed"));
        text = snag_json_string(result, "model_text");
        assert(text && strstr(text, "budget") && !strstr(text, "next_before_seq=0"));
        json_decref(result);
        json_decref(call.arguments);
    }

    /* A filtered scan quantum can end before any match; keep a usable cursor. */
    size_t padding_size = 4u * 1024u * 1024u + 4096u;
    char *padding = malloc(padding_size + 1u);
    assert(padding);
    memset(padding, 'p', padding_size);
    padding[padding_size] = '\0';
    json_t *document = json_pack("{s:s,s:s}", "marker", "history-private-payload",
        "padding", padding);
    free(padding);
    assert(document);
    app.session.tool_output_bytes = 2048u;
    app.session.on_checkpoint = history_checkpoint_document;
    app.session.on_commit_opaque = document;
    assert(snag_session_checkpoint(&app.session, error, sizeof(error)) == 0);
    app.session.on_checkpoint = NULL;
    app.session.on_commit_opaque = NULL;
    json_decref(document);
    uint64_t checkpoint_seq = app.session.checkpoint_seq;
    call.name = "read_session_history";
    call.arguments = json_pack("{s:[s]}", "event_types", "goal_started");
    assert(snag_app_tool_run(&app, &call, NULL, &result, error, sizeof(error)) == 0);
    text = snag_json_string(result, "model_text");
    assert(text && strstr(text, "returned=0") && strstr(text, "scan_complete=false"));
    (void)snprintf(cursor, sizeof(cursor), "next_before_seq=%llu",
        (unsigned long long)checkpoint_seq);
    assert(strstr(text, cursor));
    json_decref(result);
    assert(json_object_set_new(call.arguments, "before_seq",
        json_integer((json_int_t)checkpoint_seq)) == 0);
    assert(snag_app_tool_run(&app, &call, NULL, &result, error, sizeof(error)) == 0);
    text = snag_json_string(result, "model_text");
    assert(text && strstr(text, "returned=1") && strstr(text, started));
    assert(strstr(text, "scan_complete=true") && strstr(text, "next_before_seq=0"));
    json_decref(result);
    json_decref(call.arguments);

    call.arguments = json_pack("{s:i}", "limit", 1);
    app.interrupt_requested = true;
    assert(snag_app_tool_run(&app, &call, NULL, &result, error, sizeof(error)) < 0);
    assert(errno == ECANCELED && app.session.next_seq == checkpoint_seq + 1u);
    app.interrupt_requested = false;
    assert(snag_app_tool_run(&app, &call, NULL, &result, error, sizeof(error)) == 0);
    text = snag_json_string(result, "model_text");
    assert(text && strstr(text, "provider_view") && !strstr(text, "history-private-payload"));
    json_decref(result);
    json_decref(call.arguments);

    snag_session_close(&app.session);
    snag_store_close(&app.store);
}

static void
test_output_cache_failure(void)
{
    const char *a = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    const char *b = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    const char *text = "output from the second command";
    char error[256] = {0}, hash[65];
    struct app_state app = {0};
    struct snag_buf journal = {.max = 65536u}, line = {.max = 65536u};
    struct snag_response_item call = {.kind = SNAG_ITEM_TOOL_CALL, .name = "read_tool_output"};
    json_t *result = NULL;
    snag_session_init(&app.session);
    strcpy(app.session.id, "cccccccccccccccccccccccccccccccc");
    app.session.tool_output_bytes = 1024u;
    app.session.output_cache_bytes = 4096u;
    app.session.pending_log = &journal;
    app.session.processes = calloc(1u, sizeof(*app.session.processes));
    assert(app.session.processes);
    app.session.process_count = app.session.process_capacity = 1u;
    struct snag_process_state *process = app.session.processes;
    strcpy(process->handle, b);
    memset(process->log_hash, 'e', 64u);
    process->log_hash[64] = '\0';
    /* The established process cursor makes this unrelated prefix irrelevant. */
    assert(snag_buf_printf(&journal, "unread earlier history\n") == 0);
    process->log_offset = journal.len;
    process->log_seq = 100u;
    process->output_bytes[0] = strlen(text);
    json_t *record = json_pack("{s:o,s:s,s:i,s:s,s:i,s:s,s:i}", "data",
        json_pack("{s:s,s:s,s:i,s:i,s:s,s:s}", "turn_id", app.session.id,
            "handle", b, "stream", 0, "offset", 0, "encoding", "utf8", "data", text),
        "prev_sha256", process->log_hash, "seq", 100, "session_id", app.session.id,
        "time_ms", 1, "type", "process_output", "v", 1);
    assert(record && snag_json_digest(record, hash) == 0);
    assert(json_object_set_new(record, "event_sha256", json_string(hash)) == 0);
    assert(snag_json_canonical(record, &line) == 0);
    assert(snag_buf_append(&journal, line.data, line.len) == 0 &&
        snag_buf_putc(&journal, '\n') == 0);
    json_decref(record);
    size_t complete = journal.len;
    assert(snag_buf_printf(&journal, "broken later record\n") == 0);
    app.session.log_end = (int64_t)journal.len;
    app.session.next_seq = 102u;

    /* Seed a previously verified cache, then fail while loading another handle. */
    strcpy(app.output_cache.handle, a);
    app.output_cache.valid = true;
    app.output_cache.total = 8u;
    snag_buf_init(&app.output_cache.data, 4096u);
    assert(snag_buf_printf(&app.output_cache.data, "original") == 0);
    call.arguments = json_pack("{s:s,s:s}", "handle", b, "stream", "stdout");
    assert(snag_app_output_page(&app, &call, &result, error, sizeof(error)) == 0);
    assert(!strcmp(snag_json_string(result, "status"), "failed"));
    assert(app.output_cache.valid && !strcmp(app.output_cache.handle, a));
    assert(app.output_cache.data.len == 8u &&
        !memcmp(app.output_cache.data.data, "original", 8u));
    assert(strstr(snag_json_string(result, "model_text"), "corrupt event 101"));
    assert(!strstr(snag_json_string(result, "model_text"), "No durable"));
    json_decref(result);

    assert(json_object_set_new(call.arguments, "handle", json_string(a)) == 0);
    assert(snag_app_output_page(&app, &call, &result, error, sizeof(error)) == 0);
    assert(strstr(snag_json_string(result, "model_text"), "cache=hit]\noriginal"));
    json_decref(result);
    app.interrupt_requested = true;
    assert(snag_app_output_page(&app, &call, &result, error, sizeof(error)) < 0);
    assert(errno == ECANCELED && !result && app.output_cache.valid);
    assert(!strcmp(app.output_cache.handle, a) && app.output_cache.data.len == 8u &&
        !memcmp(app.output_cache.data.data, "original", 8u));
    app.interrupt_requested = false;

    /* Repair only the selected suffix; successful fill never reads the prefix. */
    app.session.log_end = (int64_t)complete;
    app.session.next_seq = 101u;
    assert(json_object_set_new(call.arguments, "handle", json_string(b)) == 0);
    assert(snag_app_output_page(&app, &call, &result, error, sizeof(error)) == 0);
    assert(!strcmp(snag_json_string(result, "status"), "succeeded"));
    assert(strstr(snag_json_string(result, "model_text"), text));
    json_decref(result);
    app.interrupt_requested = true;
    assert(snag_app_output_page(&app, &call, &result, error, sizeof(error)) < 0);
    assert(errno == ECANCELED && !result && app.output_cache.valid);
    assert(app.output_cache.data.len == strlen(text) &&
        !memcmp(app.output_cache.data.data, text, strlen(text)));
    app.interrupt_requested = false;
    /* A bad cursor and a hole in retained bytes remain explicit read failures. */
    uint64_t anchor = process->log_offset;
    process->log_offset = (uint64_t)app.session.log_end + 1u;
    assert(snag_app_output_page(&app, &call, &result, error, sizeof(error)) == 0);
    assert(!strcmp(snag_json_string(result, "status"), "failed"));
    assert(strstr(snag_json_string(result, "model_text"), "invalid process output cursor"));
    json_decref(result);
    process->log_offset = anchor;
    ++process->output_bytes[0];
    assert(snag_app_output_page(&app, &call, &result, error, sizeof(error)) == 0);
    assert(!strcmp(snag_json_string(result, "status"), "failed"));
    assert(strstr(snag_json_string(result, "model_text"), "missing bytes"));
    json_decref(result);
    --process->output_bytes[0];
    assert(app.output_cache.valid && !strcmp(app.output_cache.handle, b));
    assert(app.output_cache.data.len == strlen(text) &&
        !memcmp(app.output_cache.data.data, text, strlen(text)));
    /* Collection advances this cursor: older bytes must not be declared absent. */
    process->collected_bytes[0] = 1u;
    assert(snag_app_output_page(&app, &call, &result, error, sizeof(error)) == 0);
    assert(!strcmp(snag_json_string(result, "status"), "failed"));
    assert(strstr(snag_json_string(result, "model_text"), "corrupt event 1"));
    json_decref(result);
    json_decref(call.arguments);
    app.session.pending_log = NULL;
    snag_session_close(&app.session);
    snag_buf_free(&app.output_cache.data);
    snag_buf_free(&journal);
    snag_buf_free(&line);
}

static void
test_ui_bounded_history(void)
{
    char path[4096], output_path[4096], error[256] = {0}, output[8192];
    struct snag_store store;
    struct snag_session session;
    struct snag_ui ui;
    const char *tmp = getenv("TMPDIR");
    assert(snprintf(path, sizeof(path), "%s/snajpagent-ui-history-XXXXXX", tmp ? tmp : "/tmp") > 0);
    assert(mkdtemp(path));
    snag_store_init(&store);
    snag_session_init(&session);
    assert(snag_store_open(&store, path, error, sizeof(error)) == 0);
    assert(snag_session_create(&store, &session, path, "default", "fixture", "medium",
        error, sizeof(error)) == 0);
    assert(snprintf(output_path, sizeof(output_path), "%s/capture", path) > 0);
    int fd = open(output_path, O_CREAT | O_EXCL | O_RDWR, 0600), saved = dup(STDERR_FILENO);
    assert(fd >= 0 && saved >= 0 && dup2(fd, STDERR_FILENO) == STDERR_FILENO);
    assert(snag_ui_init(&ui) == 0);
    int64_t damaged = 0;
    json_t *checkpoint = NULL;
    for (unsigned int i = 1u; i <= 3u; ++i) {
        char id[33], text[32];
        assert(snprintf(id, sizeof(id), "%032x", i) > 0);
        assert(snprintf(text, sizeof(text), "history input %u", i) > 0);
        json_t *started = json_pack("{s:{s:s,s:s,s:n,s:s,s:s,s:s,s:i,s:i,s:i,s:i,s:b},"
            "s:s,s:b,s:o,s:n,s:n,s:s,s:s,s:I,s:s}",
            "config", "capability_version", SNAJPAGENT_CAPABILITY_VERSION, "effort", "medium",
            "max_output_tokens", "model", "fixture", "provider", "default",
            "profile_id", SNAJPAGENT_PROFILE_ID,
            "prompt_schema", 1, "replay_schema", 1, "tool_schema", 1, "max_parallel_commands", 4,
            "parallel_tool_calls", 1, "input_kind", "direct", "read_only", 0,
            "instructions", json_array(), "queue_id", "queue_seq", "text", text,
            "turn_id", id, "turn_number", (json_int_t)i, "cwd", session.cwd);
        assert(snag_session_commit(&session, "turn_started", started,
            NULL, error, sizeof(error)) == 0);
        if (i == 1u) damaged = session.log_end;
        if (i == 3u) {
            size_t size = SNAG_JOURNAL_PAGE_BYTES + 4096u;
            char *padding = malloc(size);
            assert(padding); memset(padding, 'x', size);
            checkpoint = json_pack("{s:o}", "padding", json_stringn(padding, size));
            free(padding); assert(checkpoint);
            session.on_checkpoint = history_checkpoint_document;
            session.on_commit_opaque = checkpoint;
            assert(snag_session_checkpoint(&session, error, sizeof(error)) == 0);
            session.on_checkpoint = NULL; session.on_commit_opaque = NULL;
        }
        assert(snag_session_commit(&session, "turn_failed",
            json_pack("{s:s,s:s,s:s}", "class", "provider", "message", "fixture", "turn_id", id),
            NULL, error, sizeof(error)) == 0);
        if (i == 2u) {
            char original;
            int writer = openat(session.dir_fd, "events.jsonl", O_WRONLY | O_CLOEXEC);
            assert(writer >= 0);
            assert(pread(session.log_fd, &original, 1u, damaged) == 1);
            assert(pwrite(writer, "!", 1u, damaged) == 1);
            assert(snag_ui_history(&ui, &session, 0u) == 0);
            assert(snag_ui_history(&ui, &session, 1u) == 0);
            assert(snag_ui_history(&ui, &session, 2u) < 0);
            assert(pwrite(writer, &original, 1u, damaged) == 1);
            assert(close(writer) == 0);
        }
    }
    assert(snag_ui_history(&ui, &session, 1u) == 0);
    /* Admission metadata beyond a large checkpoint is not an invented turn. */
    struct snag_irc_event event = {.kind = SNAG_IRC_MESSAGE, .timestamp_ms = 1u,
        .stream = "11111111111111111111111111111111", .sequence = 1u,
        .endpoint = "fixture:1234", .room = "#fixture", .nick = "peer", .text = "background",
        .classified = true, .input = true};
    uint64_t irc_seq;
    assert(snag_session_commit(&session, "irc_event", snag_irc_event_data(&event),
        &irc_seq, error, sizeof(error)) == 0);
    session.on_checkpoint = history_checkpoint_document;
    session.on_commit_opaque = checkpoint;
    assert(snag_session_checkpoint(&session, error, sizeof(error)) == 0);
    session.on_checkpoint = NULL; session.on_commit_opaque = NULL;
    json_decref(checkpoint);
    assert(snag_session_commit(&session, "irc_admitted",
        json_pack("{s:[I]}", "sequences", (json_int_t)irc_seq), NULL, error, sizeof(error)) == 0);
    assert(snag_ui_history(&ui, &session, 1u) == 0);
    /* Integrity-valid envelopes still need safe presentation-field checks. */
    for (unsigned int malformed = 0u; malformed < 3u; ++malformed) {
        struct snag_session broken;
        struct snag_buf bytes = {.max = 65536u};
        snag_session_init(&broken);
        memcpy(broken.id, session.id, sizeof(broken.id));
        broken.pending_log = &bytes; broken.turn_count = 1u;
        for (unsigned int row = 0u; row <= (malformed ? 1u : 0u); ++row) {
            const char *type = !row ? "turn_started" : malformed == 1u ?
                "response_output" : "response_completed";
            json_t *data = !row ? (malformed ? json_pack("{s:s}", "text", "fixture") :
                json_object()) : malformed == 1u ? json_pack("{s:i,s:{}}", "offset", 0, "item") :
                json_pack("{s:[{s:s}]}", "items", "kind", "assistant");
            json_t *record = json_pack("{s:o,s:s,s:I,s:s,s:i,s:s,s:i}", "data", data,
                "prev_sha256", broken.prev_sha256, "seq", (json_int_t)broken.next_seq,
                "session_id", broken.id, "time_ms", 1, "type", type, "v", 1);
            assert(record && snag_json_digest(record, broken.prev_sha256) == 0);
            assert(json_object_set_new(record, "event_sha256",
                json_string(broken.prev_sha256)) == 0);
            struct snag_buf line = {.max = 65536u};
            assert(snag_json_canonical(record, &line) == 0);
            assert(snag_buf_append(&bytes, line.data, line.len) == 0 &&
                snag_buf_putc(&bytes, '\n') == 0);
            snag_buf_free(&line);
            json_decref(record);
            ++broken.next_seq;
        }
        broken.log_end = (int64_t)bytes.len;
        assert(snag_ui_history(&ui, &broken, 1u) < 0 && errno == EINVAL);
        assert(broken.history_cursor.next_seq == 1u && broken.history_cursor.offset == 0);
        snag_buf_free(&bytes);
    }
    snag_ui_signal(&ui);
    assert(snag_ui_history(&ui, &session, 1u) < 0 && errno == ECANCELED);
    snag_ui_free(&ui);
    assert(dup2(saved, STDERR_FILENO) == STDERR_FILENO && close(saved) == 0);
    ssize_t got = pread(fd, output, sizeof(output) - 1u, 0);
    assert(got > 0); output[got] = '\0';
    assert(strstr(output, "history input 2") && !strstr(output, "history input 1"));
    assert(!strstr(output, "history input 3"));
    assert(strstr(output, "earlier input outside restored history window"));
    assert(!strstr(strstr(output, "earlier input outside restored history window") + 1,
        "earlier input outside restored history window"));
    assert(strstr(output, "History scan window ended"));
    assert(strstr(output, "1 shown · 0 completed among shown · 3 total"));
    assert(strstr(output, "0 shown · 0 completed among shown · 3 total"));
    assert(close(fd) == 0);
    snag_session_close(&session);
    snag_store_close(&store);
}

static void
test_ui_output_order_and_failure(void)
{
    struct snag_ui ui;
    unsigned char text[1024];
    enum snag_term_action action;
    char *line;
    int pipefd[2], status;
    pid_t reader;

    assert(pipe(pipefd) == 0);
    reader = fork();
    assert(reader >= 0);
    if (!reader) {
        size_t received = 0u;
        (void)close(pipefd[1]);
        while (received < 128u * sizeof(text)) {
            ssize_t got = read(pipefd[0], text, sizeof(text));
            assert(got > 0);
            for (ssize_t i = 0; i < got; ++i) assert(text[i] == (received + (size_t)i) / sizeof(text));
            received += (size_t)got;
        }
        (void)close(pipefd[0]);
        _exit(0);
    }
    assert(close(pipefd[0]) == 0);
    assert(snag_ui_init(&ui) == 0);
    /* Capture may not start without an interactive raw prompt; rejection and
     * invalid transcript insertion must not poison the presentation owner. */
    assert(snag_ui_audio(&ui, "[mic] ", true) == 1);
    assert(snag_ui_insert_draft(&ui, "\xff") == 1);
    assert(snag_ui_insert_draft(&ui, "kept draft") == 0);
    for (unsigned int i = 0u; i < 128u; ++i) {
        memset(text, (int)i, sizeof(text));
        assert(snag_ui_send(&ui, (struct snag_ui_command){
            .kind = SNAG_UI_RAW, .data.value = (unsigned int)(pipefd[1]), .text = (char *)text, .len = sizeof(text)}) == 0);
    }
    assert(waitpid(reader, &status, 0) == reader);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    /* Beginning a public item must not erase an already delivered shutdown. */
    {
        struct snag_buf delivered = {.max = 16u};
        snag_ui_signal(&ui);
        assert(snag_ui_send(&ui, (struct snag_ui_command){
            .kind = SNAG_UI_PUBLIC_BEGIN, .label = NULL, .data.public = {STDOUT_FILENO, SNAG_PRESENT_CONVERSATION}}) == 0);
        assert(snag_ui_public(&ui, "stopped", 7u, &delivered) == 0);
        assert(delivered.len == 0u);
        assert(snag_ui_text(&ui, SNAG_UI_ROLLOUT_END, NULL) == 0);
        assert(snag_ui_poll(&ui, 0, &action, &line) == 1);
        assert(action == SNAG_TERM_EXIT);
        snag_buf_free(&delivered);
    }
    assert(close(pipefd[1]) == 0);
    assert(snag_ui_send(&ui, (struct snag_ui_command){
        .kind = SNAG_UI_RAW, .data.value = (unsigned int)(-1), .text = "x", .len = 1u}) < 0 && errno == EBADF);
    assert(snag_ui_send(&ui, (struct snag_ui_command){
        .kind = SNAG_UI_RAW, .data.value = (unsigned int)(pipefd[1]), .text = "x", .len = 1u}) < 0);
    assert(snag_ui_poll(&ui, 0, &action, &line) < 0);
    snag_ui_free(&ui);
}

static int
cancel_device_poll(void *opaque, uint32_t wait_ms)
{
    (void)opaque;
    return wait_ms ? 2 : 0;
}

static void
test_provider_auth(void)
{
    struct snag_config config;
    struct snag_store store;
    struct snag_auth_tokens tokens, previous, loaded;
    struct snag_credential credential;
    struct local_server server;
    char path[4096], error[256] = {0};
    const char *tmp = getenv("TMPDIR");
    struct stat st;
    int status;

    assert(snprintf(path, sizeof(path), "%s/snajpagent-auth-XXXXXX", tmp ? tmp : "/tmp") > 0);
    assert(mkdtemp(path));
    snag_config_init(&config);
    snag_secret_source_free(&config.providers[0].api_key);
    strcpy(config.providers[0].name, "default");
    snag_store_init(&store);
    assert(snag_store_open(&store, path, error, sizeof(error)) == 0);
    config.providers[0].auth = SNAG_AUTH_API_KEY;
    assert(snag_auth_load(store.root_fd, &config.providers[0], &tokens, error, sizeof(error)) == 1);
    assert(snag_auth_key(&tokens, "stored-key", error, sizeof(error)) == 0);
    assert(snag_auth_save(store.root_fd, &config.providers[0], &tokens, &previous,
                          NULL, NULL, error, sizeof(error)) == 0);
    assert(!previous.credential.len);
    assert(fstatat(store.root_fd, "auth/default.json", &st, AT_SYMLINK_NOFOLLOW) == 0);
    assert((st.st_mode & 0777u) == 0600u);
    assert(snag_auth_read(store.root_fd, &config.providers[0], false, NULL, &credential,
                         NULL, NULL, error, sizeof(error)) == 0);
    assert(strcmp(credential.value, "stored-key") == 0);
    assert(credential.root_fd == store.root_fd);
    assert(snag_secret_source_parse(&config.providers[0].api_key, "${SNAG_MISSING_EXPLICIT_KEY}",
                                    NULL, error, sizeof(error)) == 0);
    assert(unsetenv("SNAG_MISSING_EXPLICIT_KEY") == 0);
    assert(snag_auth_read(store.root_fd, &config.providers[0], false, NULL, &credential,
                         NULL, NULL, error, sizeof(error)) < 0);
    assert(credential.len == 0u);
    assert(snag_secret_source_parse(&config.providers[0].api_key, "\"explicit-key\"",
                                    NULL, error, sizeof(error)) == 0);
    assert(snag_auth_read(store.root_fd, &config.providers[0], false, NULL, &credential,
                         NULL, NULL, error, sizeof(error)) == 0);
    assert(strcmp(credential.value, "explicit-key") == 0);
    snag_secret_source_free(&config.providers[0].api_key);
    error[0] = '\0';
    snag_config_provider_init(&config.providers[1], "other");
    strcpy(config.providers[1].name, "other");
    assert(snag_auth_load(store.root_fd, &config.providers[1], &loaded, error, sizeof(error)) == 1);
    strcpy(config.providers[0].base_url, "https://different.test");
    assert(snag_auth_load(store.root_fd, &config.providers[0], &loaded, error, sizeof(error)) < 0);
    strcpy(config.providers[0].base_url, "https://api.openai.com");
    assert(fchmodat(store.root_fd, "auth/default.json", 0644, 0) == 0);
    assert(snag_auth_load(store.root_fd, &config.providers[0], &loaded, error, sizeof(error)) < 0);
    assert(fchmodat(store.root_fd, "auth/default.json", 0600, 0) == 0);
    assert(snag_auth_key(&tokens, "replacement", error, sizeof(error)) == 0);
    assert(snag_auth_save(store.root_fd, &config.providers[0], &tokens, &previous,
                          NULL, NULL, error, sizeof(error)) == 0);
    assert(snag_auth_restore(store.root_fd, &config.providers[0], &tokens, &previous,
                             error, sizeof(error)) == 0);
    assert(snag_auth_load(store.root_fd, &config.providers[0], &loaded, error, sizeof(error)) == 0);
    assert(strcmp(loaded.credential.value, "stored-key") == 0);
    assert(snag_auth_logout(store.root_fd, &config.providers[0], NULL, NULL, error, sizeof(error)) == 0);
    assert(symlinkat("outside", store.root_fd, "auth/default.json") == 0);
    assert(snag_auth_load(store.root_fd, &config.providers[0], &loaded, error, sizeof(error)) < 0);
    assert(snag_auth_save(store.root_fd, &config.providers[0], &tokens, &previous,
                          NULL, NULL, error, sizeof(error)) < 0);
    assert(unlinkat(store.root_fd, "auth/default.json", 0) == 0);

    config.providers[0].auth = SNAG_AUTH_CHATGPT;
    strcpy(config.providers[0].base_url, SNAG_CHATGPT_BASE);
    {
        json_t *response = json_object();
        snag_auth_clear(&tokens);
        assert(snag_auth_token_response(response, &tokens, error, sizeof(error)) < 0);
        assert(tokens.credential.len == 0u);
        assert(snag_auth_key(&tokens, "old-access", error, sizeof(error)) == 0);
        strcpy(tokens.credential.account_id, "acct-test");
        strcpy(tokens.refresh_token, "old-refresh");
        assert(json_object_set_new(response, "access_token", json_string("rotated-access")) == 0);
        assert(json_object_set_new(response, "expires_in", json_integer(3600)) == 0);
        assert(snag_auth_token_response(response, &tokens, error, sizeof(error)) == 0);
        assert(strcmp(tokens.refresh_token, "old-refresh") == 0);
        assert(strcmp(tokens.credential.account_id, "acct-test") == 0);
        snag_auth_json_free(response);
    }
    for (int mode = MODEL_AUTH_DEVICE; mode <= MODEL_AUTH_EXPIRED; ++mode) {
        start_server(&server, (enum model_fixture)mode, false, "");
        assert(setenv("SNAJPAGENT_TEST_AUTH_BASE", server.endpoint, 1) == 0);
        error[0] = '\0';
        int rc = snag_auth_device(&tokens, mode == MODEL_AUTH_CANCEL ? cancel_device_poll : NULL,
                                  NULL, error, sizeof(error));
        if (mode == MODEL_AUTH_DEVICE) {
            assert(rc == 0);
            assert(strcmp(tokens.credential.value, "new-access") == 0);
            assert(strcmp(tokens.credential.account_id, "acct-test") == 0);
            assert(strcmp(tokens.refresh_token, "new-refresh") == 0);
            assert(tokens.expires_at_ms > snag_time_ms());
        } else {
            assert(rc < 0);
            assert(tokens.credential.len == 0u);
        }
        stop_server(&server);
    }
    for (int mode = MODEL_AUTH_REFRESH; mode <= MODEL_AUTH_401_TWICE; ++mode) {
        assert(snag_auth_key(&tokens, "old-access", error, sizeof(error)) == 0);
        strcpy(tokens.refresh_token, "old-refresh");
        strcpy(tokens.credential.account_id, "acct-test");
        tokens.expires_at_ms = mode >= MODEL_AUTH_401 ? snag_time_ms() + 3600000u : 1u;
        assert(snag_auth_save(store.root_fd, &config.providers[0], &tokens, NULL,
                              NULL, NULL, error, sizeof(error)) == 0);
        start_server(&server, (enum model_fixture)mode, false, "");
        assert(setenv("SNAJPAGENT_TEST_AUTH_BASE", server.endpoint, 1) == 0);
        error[0] = '\0';
        if (mode == MODEL_AUTH_REFRESH) {
            pid_t children[2];
            for (size_t i = 0; i < 2u; ++i) {
                children[i] = fork();
                assert(children[i] >= 0);
                if (children[i] == 0) {
                    error[0] = '\0';
                    int rc = snag_auth_read(store.root_fd, &config.providers[0], false,
                        NULL, &credential, NULL, NULL, error, sizeof(error));
                    bool matched = rc == 0 && strcmp(credential.value, "new-access") == 0;
                    if (!matched)
                        (void)fprintf(stderr,
                            "concurrent auth refresh child failed: rc=%d matched=%d error=%s\n",
                            rc, matched ? 1 : 0, error[0] ? error : "(none)");
                    _exit(matched ? 0 : 1);
                }
            }
            for (size_t i = 0; i < 2u; ++i) {
                assert(waitpid(children[i], &status, 0) == children[i]);
                assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
            }
            assert(snag_auth_load(store.root_fd, &config.providers[0], &loaded, error, sizeof(error)) == 0);
            assert(strcmp(loaded.refresh_token, "new-refresh") == 0);
        } else if (mode == MODEL_AUTH_REFRESH_FAILURE) {
            assert(snag_auth_read(store.root_fd, &config.providers[0], false, NULL,
                &credential, NULL, NULL, error, sizeof(error)) < 0);
            assert(!strstr(error, "private-refresh-server-detail"));
            assert(snag_auth_load(store.root_fd, &config.providers[0], &loaded, error, sizeof(error)) == 0);
            assert(strcmp(loaded.refresh_token, "old-refresh") == 0);
        } else {
            json_t *models = NULL;
            assert(setenv("SNAJPAGENT_TEST_OPENAI_BASE", server.endpoint, 1) == 0);
            assert(snag_auth_read(store.root_fd, &config.providers[0], false, NULL,
                &credential, NULL, NULL, error, sizeof(error)) == 0);
            int rc = snag_provider_models_list((struct snag_provider_connection){
                &config, &config.providers[0], &credential, NULL, NULL, NULL, NULL, 0}, &models, error, sizeof(error));
            if (rc < 0 && mode == MODEL_AUTH_401) (void)fprintf(stderr, "auth fixture failed: %s\n", error);
            assert((rc == 0) == (mode == MODEL_AUTH_401));
            if (rc == 0) assert(json_array_size(models) == 1u);
            json_decref(models);
            assert(unsetenv("SNAJPAGENT_TEST_OPENAI_BASE") == 0);
        }
        stop_server(&server);
    }
    assert(unsetenv("SNAJPAGENT_TEST_AUTH_BASE") == 0);
    /* Meta subscription: token responses keep prior refresh and account. */
    config.providers[1].auth = SNAG_AUTH_META;
    strcpy(config.providers[1].base_url, SNAG_META_BASE);
    {
        json_t *response = json_object();
        snag_auth_clear(&tokens);
        assert(snag_auth_token_response_meta(response, &tokens, error, sizeof(error)) < 0);
        assert(tokens.credential.len == 0u);
        assert(snag_auth_key(&tokens, "old-access", error, sizeof(error)) == 0);
        strcpy(tokens.refresh_token, "meta-old-refresh");
        strcpy(tokens.credential.account_id, "meta-user");
        assert(json_object_set_new(response, "access_token", json_string("rotated-access")) == 0);
        assert(json_object_set_new(response, "expires_in", json_integer(3600)) == 0);
        assert(snag_auth_token_response_meta(response, &tokens, error, sizeof(error)) == 0);
        assert(!strcmp(tokens.credential.value, "rotated-access"));
        assert(!strcmp(tokens.refresh_token, "meta-old-refresh"));
        assert(!strcmp(tokens.credential.account_id, "meta-user"));
        assert(tokens.expires_at_ms > snag_time_ms());
        snag_auth_json_free(response);
    }
    {
        json_t *jwt = json_object();
        snag_auth_clear(&tokens);
        assert(json_object_set_new(jwt, "access_token", json_string("h.eyJzdWIiOiJtZXRhLXVzZXIiLCJleHAiOjk5OTk5OTk5OTl9.sig")) == 0);
        assert(snag_auth_token_response_meta(jwt, &tokens, error, sizeof(error)) == 0);
        assert(!strcmp(tokens.credential.account_id, "meta-user"));
        assert(tokens.expires_at_ms == 9999999999u * 1000u);
        assert(!tokens.refresh_token[0]);
        assert(json_object_set_new(jwt, "access_token", json_string("h.eyJzdWIiOiJzb21lb25lLWVsc2UiLCJleHAiOjk5OTk5OTk5OTl9.sig")) == 0);
        assert(json_object_set_new(jwt, "expires_in", json_integer(3600)) == 0);
        assert(snag_auth_token_response_meta(jwt, &tokens, error, sizeof(error)) < 0);
        snag_auth_json_free(jwt);
    }
    for (int mode = MODEL_META_DEVICE; mode <= MODEL_META_EXPIRED; ++mode) {
        start_server(&server, (enum model_fixture)mode, false, "");
        assert(setenv("SNAJPAGENT_TEST_META_AUTH_BASE", server.endpoint, 1) == 0);
        memset(error, 0, sizeof(error));
        int rc = snag_auth_device_meta(&tokens, NULL, NULL, error, sizeof(error));
        if (mode == MODEL_META_DEVICE) {
            assert(rc == 0);
            assert(!strcmp(tokens.credential.value, "meta-access"));
            assert(!strcmp(tokens.refresh_token, "meta-refresh"));
            assert(tokens.expires_at_ms > snag_time_ms());
        } else {
            assert(rc < 0);
            assert(tokens.credential.len == 0u);
        }
        stop_server(&server);
    }

    for (int mode = MODEL_META_REFRESH; mode <= MODEL_META_REFRESH_FAILURE; ++mode) {
        assert(snag_auth_key(&tokens, "old-access", error, sizeof(error)) == 0);
        strcpy(tokens.refresh_token, "meta-old-refresh");
        tokens.expires_at_ms = 1u;
        assert(snag_auth_save(store.root_fd, &config.providers[1], &tokens, NULL,
                              NULL, NULL, error, sizeof(error)) == 0);
        start_server(&server, (enum model_fixture)mode, false, "");
        assert(setenv("SNAJPAGENT_TEST_META_AUTH_BASE", server.endpoint, 1) == 0);
        memset(error, 0, sizeof(error));
        if (mode == MODEL_META_REFRESH) {
            assert(snag_auth_read(store.root_fd, &config.providers[1], false, NULL,
                &credential, NULL, NULL, error, sizeof(error)) == 0);
            assert(!strcmp(credential.value, "meta-new-access"));
            assert(snag_auth_load(store.root_fd, &config.providers[1], &loaded, error, sizeof(error)) == 0);
            assert(!strcmp(loaded.refresh_token, "meta-new-refresh"));
        } else {
            assert(snag_auth_read(store.root_fd, &config.providers[1], false, NULL,
                &credential, NULL, NULL, error, sizeof(error)) < 0);
            assert(snag_auth_load(store.root_fd, &config.providers[1], &loaded, error, sizeof(error)) == 0);
            assert(!strcmp(loaded.refresh_token, "meta-old-refresh"));
        }
        stop_server(&server);
    }
    assert(unsetenv("SNAJPAGENT_TEST_META_AUTH_BASE") == 0);
    strcpy(config.providers[1].base_url, "https://different.test");
    assert(snag_auth_load(store.root_fd, &config.providers[1], &loaded, error, sizeof(error)) < 0);
    strcpy(config.providers[1].base_url, SNAG_META_BASE);
    assert(snag_auth_load(store.root_fd, &config.providers[1], &loaded, error, sizeof(error)) == 0);
    assert(!strcmp(loaded.credential.value, "old-access"));
    assert(snag_auth_logout(store.root_fd, &config.providers[1], NULL, NULL, error, sizeof(error)) == 0);
    assert(unlinkat(store.root_fd, "auth/other.lock", 0) == 0);
    for (unsigned int pass = 0u; pass < 4u; ++pass) {
        json_t *request = request_with_marker("transport-compact");
        struct snag_json_document output = {0};
        credential_set(&credential, "transport-secret");
        credential.root_fd = -1;
        config.providers[0].auth = pass == 0u ? SNAG_AUTH_API_KEY : SNAG_AUTH_CHATGPT;
        strcpy(config.providers[0].base_url, pass == 0u ? "https://api.openai.com" : SNAG_CHATGPT_BASE);
        strcpy(config.providers[0].openrouter_referer, "https://github.com/snajpa/snajpagent");
        strcpy(config.providers[0].openrouter_title, "snajpagent");
        start_server(&server, pass == 3u ? MODEL_COMPACT_403 :
                          pass == 2u ? MODEL_COMPACT_502 : MODEL_COMPACT_404, false, "");
        assert(setenv("SNAJPAGENT_TEST_OPENAI_BASE", server.endpoint, 1) == 0);
        int rc = snag_provider_responses_compact((struct snag_provider_connection){
            &config, &config.providers[0], &credential, NULL, NULL, NULL, NULL, 0},
            request, &output, error, sizeof(error), NULL);
        assert(rc == (pass < 3u ? SNAG_PROVIDER_UNSUPPORTED : -1));
        assert(output.value == NULL);
        stop_server(&server);
        json_decref(request);
    }
    assert(unsetenv("SNAJPAGENT_TEST_OPENAI_BASE") == 0);
    assert(snag_auth_logout(store.root_fd, &config.providers[0], NULL, NULL, error, sizeof(error)) == 0);
    assert(unlinkat(store.root_fd, "auth/default.lock", 0) == 0);
    assert(unlinkat(store.root_fd, "auth", AT_REMOVEDIR) == 0);
    assert(unlinkat(store.root_fd, "sessions", AT_REMOVEDIR) == 0);
    assert(unlinkat(store.root_fd, "trash", AT_REMOVEDIR) == 0);
    snag_store_close(&store);
    assert(rmdir(path) == 0);
    snag_config_free(&config);
    snag_auth_clear(&tokens);
    snag_auth_clear(&previous);
    snag_auth_clear(&loaded);
    snag_credential_clear(&credential);
}

static void
test_irc_steering_mode(void)
{
    struct snag_config config = {0};
    struct app_state app = {0};
    char error[256] = {0};
    snag_config_init(&config);
    snag_session_init(&app.session);
    assert(snag_ui_init(&app.ui) == 0);
    app.config = &config;
    app.irc_urgent.max = 128u;
    app.irc_background.max = 128u;
    assert(snag_buf_append(&app.irc_background, "room", sizeof("room")) == 0);
    app.session.active_turn = true;
    strcpy(config.model, "m");
    strcpy(app.session.active_turn_provider, "p");
    /* Default `mentions`: mid-turn, background room traffic is not admitted. */
    assert(snag_app_irc_flush_urgent(&app, error, sizeof(error)) == 0);
    assert(app.irc_background.len == sizeof("room"));
    /* `all`: the background projection is attempted; invalid UTF-8 fails it
     * without consuming the projection. */
    config.model_limit_count = 1u;
    strcpy(config.model_limits[0].provider, "p");
    strcpy(config.model_limits[0].model, "m");
    strcpy(config.model_limits[0].steering, "all");
    app.irc_background.data[0] = 0xff;
    assert(snag_app_irc_flush_urgent(&app, error, sizeof(error)) < 0);
    assert(app.irc_background.len == sizeof("room"));
    snag_buf_free(&app.irc_background);
    snag_ui_free(&app.ui);
    snag_session_close(&app.session);
    snag_config_free(&config);
}

static void
test_plain_irc_pending_resume(void)
{
    struct snag_config config;
    struct app_state app = {0};
    char path[4096], id[33], error[256] = {0};
    const char *tmp = getenv("TMPDIR");
    assert(snprintf(path, sizeof(path), "%s/snajpagent-plain-irc-XXXXXX",
        tmp ? tmp : "/tmp") > 0);
    assert(mkdtemp(path));
    snag_config_init(&config);
    app.config = &config;
    snag_store_init(&app.store);
    snag_session_init(&app.session);
    assert(snag_store_open(&app.store, path, error, sizeof(error)) == 0);
    assert(snag_ui_init(&app.ui) == 0);
    for (unsigned int kind = 0u; kind < 2u; ++kind) {
        uint64_t received = 0u;
        struct snag_irc_event event = {
            .kind = kind ? SNAG_IRC_NOTICE : SNAG_IRC_MESSAGE, .timestamp_ms = 1u};
        strcpy(event.endpoint, "127.0.0.1:16669");
        strcpy(event.room, "#plain");
        strcpy(event.nick, "peer");
        strcpy(event.text, "unconsumed plain IRC input");
        for (unsigned int phase = 0u; phase < 3u; ++phase) {
            snag_buf_init(&app.irc_background, 4096u);
            snag_buf_init(&app.irc_background_refs, 1024u);
            snag_buf_init(&app.irc_urgent, 4096u);
            snag_buf_init(&app.irc_urgent_refs, 1024u);
            if (!phase) {
                assert(snag_session_create(&app.store, &app.session, path, "default",
                    "fixture", "medium", error, sizeof(error)) == 0);
                strcpy(id, app.session.id);
            } else {
                assert(snag_session_open(&app.store, &app.session, id,
                    error, sizeof(error)) == 0);
            }
            /* No endpoints: inject the already parsed event without networking. */
            assert(snag_irc_open(&app.irc, &config, path, NULL, NULL, NULL,
                error, sizeof(error)) == 0);
            if (!phase) {
                received = app.session.next_seq;
                assert(snag_app_irc_event(&app, &event) == 0);
            } else {
                assert(snag_app_irc_restore(&app, error, sizeof(error)) == 0);
            }
            if (phase < 2u) {
                assert(app.irc_background.len > 0u);
                assert(snag_buf_terminate(&app.irc_background) == 0);
                assert(strstr((const char *)app.irc_background.data, event.text));
            } else {
                assert(!app.irc_background.len && !app.irc_background_refs.len);
            }
            if (phase == 1u) {
                assert(app.irc_background_refs.len > 0u);
                assert(snag_session_commit(&app.session, "irc_admitted",
                    json_pack("{s:[I]}", "sequences", (json_int_t)received),
                    NULL, error, sizeof(error)) == 0);
            }
            snag_irc_close(app.irc);
            app.irc = NULL;
            snag_session_close(&app.session);
            snag_buf_free(&app.irc_background);
            snag_buf_free(&app.irc_background_refs);
            snag_buf_free(&app.irc_urgent);
            snag_buf_free(&app.irc_urgent_refs);
        }
    }
    snag_ui_free(&app.ui);
    snag_store_close(&app.store);
    snag_config_free(&config);
}

static void
test_irc_reply_restore_scope(void)
{
    struct snag_config config;
    struct snag_store store;
    char path[4096], error[256] = {0};
    const char *tmp = getenv("TMPDIR");
    assert(snprintf(path, sizeof(path), "%s/snajpagent-irc-replies-XXXXXX",
        tmp ? tmp : "/tmp") > 0);
    assert(mkdtemp(path));
    snag_config_init(&config);
    strcpy(config.irc.room_name, "#plain");
    int probe = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in address = {.sin_family = AF_INET};
    socklen_t address_size = sizeof(address);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(probe >= 0 && bind(probe, (struct sockaddr *)&address, sizeof(address)) == 0);
    assert(getsockname(probe, (struct sockaddr *)&address, &address_size) == 0);
    assert(snprintf(config.irc.listen, sizeof(config.irc.listen), "127.0.0.1:%u",
        (unsigned int)ntohs(address.sin_port)) > 0);
    assert(close(probe) == 0);
    config.irc.listen_explicit = true;
    snag_store_init(&store);
    assert(snag_store_open(&store, path, error, sizeof(error)) == 0);
    for (unsigned int mode = 0u; mode < 9u; ++mode) {
        struct app_state app = {0};
        char session_id[33];
        bool steer_current = mode == 8u;
        bool active = steer_current || (mode & 1u);
        bool current_reply = steer_current || (mode & 2u), cancel_old = mode & 4u;
        app.config = &config;
        snag_session_init(&app.session);
        assert(snag_session_create(&store, &app.session, path, "default", "fixture",
            "medium", error, sizeof(error)) == 0);
        strcpy(session_id, app.session.id);
        struct snag_irc_event event = {.kind = SNAG_IRC_MESSAGE, .timestamp_ms = 1u,
            .room = "#plain", .nick = "operator",
            .local = true, .classified = true, .input = true, .urgent = true, .reply = true};
        strcpy(event.endpoint, config.irc.listen);
        for (unsigned int turn = 1u; turn <= 2u; ++turn) {
            char id[33], text[64];
            assert(snprintf(id, sizeof(id), "%032x", turn) > 0);
            assert(snprintf(text, sizeof(text), "request %u for the agent", turn) > 0);
            json_t *input = json_pack("{s:s,s:o,s:s,s:s,s:b,s:I,s:s}",
                "effort", "medium", "instructions", json_array(), "model", "fixture",
                "provider", "default", "read_only", 0,
                "received_at_ms", (json_int_t)snag_time_ms(), "text", text);
            assert(input);
            if (turn == 1u || (current_reply && !steer_current)) {
                uint64_t received;
                strcpy(event.text, text);
                assert(snag_session_commit(&app.session, "irc_event", snag_irc_event_data(&event),
                    &received, error, sizeof(error)) == 0);
                input = json_pack("{s:[I],s:o}", "sequences", (json_int_t)received, "input", input);
                assert(snag_session_commit(&app.session, "irc_admitted", input,
                    NULL, error, sizeof(error)) == 0);
            } else {
                assert(snag_session_commit(&app.session, "input_received", input,
                    NULL, error, sizeof(error)) == 0);
            }
            if (turn == 1u && cancel_old) {
                assert(snag_session_commit(&app.session, "input_cancelled", json_object(),
                    NULL, error, sizeof(error)) == 0);
                continue;
            }
            if (turn == 1u || active) {
                json_t *started = json_pack("{s:{s:s,s:s,s:n,s:s,s:s,s:s,s:i,s:i,s:i,s:i,s:b},"
                    "s:s,s:b,s:o,s:n,s:n,s:s,s:s,s:I,s:s}",
                    "config", "capability_version", SNAJPAGENT_CAPABILITY_VERSION,
                    "effort", "medium", "max_output_tokens", "model", "fixture",
                    "provider", "default", "profile_id", SNAJPAGENT_PROFILE_ID,
                    "prompt_schema", 1, "replay_schema", 1, "tool_schema", 1,
                    "max_parallel_commands", 4, "parallel_tool_calls", 1,
                    "input_kind", "direct", "read_only", 0, "instructions", json_array(),
                    "queue_id", "queue_seq", "text", text, "turn_id", id,
                    "turn_number", (json_int_t)(turn - (cancel_old ? 1u : 0u)),
                    "cwd", app.session.cwd);
                assert(snag_session_commit(&app.session, "turn_started", started,
                    NULL, error, sizeof(error)) == 0);
            }
            if (turn == 1u)
                assert(snag_session_commit(&app.session, "turn_failed",
                    json_pack("{s:s,s:s,s:s}", "class", "provider", "message", "fixture",
                        "turn_id", id), NULL, error, sizeof(error)) == 0);
            if (turn == 2u && steer_current) {
                uint64_t received;
                strcpy(event.text, "current room steering");
                assert(snag_session_commit(&app.session, "irc_event", snag_irc_event_data(&event),
                    &received, error, sizeof(error)) == 0);
                assert(snag_session_commit(&app.session, "irc_admitted",
                    json_pack("{s:[I],s:{s:s,s:s,s:s}}", "sequences", (json_int_t)received,
                        "steering", "turn_id", id, "steering_id",
                        "33333333333333333333333333333333", "text", event.text),
                    NULL, error, sizeof(error)) == 0);
            }
        }
        snag_session_close(&app.session);
        assert(snag_session_open(&store, &app.session, session_id, error, sizeof(error)) == 0);
        snag_buf_init(&app.irc_background, 4096u);
        snag_buf_init(&app.irc_background_refs, 1024u);
        snag_buf_init(&app.irc_urgent, 4096u);
        snag_buf_init(&app.irc_urgent_refs, 1024u);
        /* An owned loopback listener supplies identity; no pump starts owners. */
        assert(snag_irc_open(&app.irc, &config, path, NULL, NULL, NULL,
            error, sizeof(error)) == 0);
        struct snag_irc_target target;
        assert(snag_irc_event_target(app.irc, &event, &target));
        assert(snag_app_irc_restore(&app, error, sizeof(error)) == 0);
        assert(app.irc_turn_replies.count == (current_reply ? 1u : 0u));
        if (current_reply) assert(app.irc_turn_replies.targets[0].id == target.id);
        assert(!app.irc_urgent.len && !app.irc_background.len);
        snag_irc_close(app.irc);
        snag_buf_free(&app.irc_background);
        snag_buf_free(&app.irc_background_refs);
        snag_buf_free(&app.irc_urgent);
        snag_buf_free(&app.irc_urgent_refs);
        snag_session_close(&app.session);
    }
    snag_store_close(&store);
    snag_config_free(&config);
}

static void
test_irc_failed_intent_retains_pending(void)
{
    struct snag_config config = {0};
    struct app_state app = {0};
    char error[256] = {0};
    snag_session_init(&app.session);
    assert(snag_ui_init(&app.ui) == 0);
    app.config = &config;
    app.irc_urgent.max = 128u;
    assert(snag_buf_append(&app.irc_urgent, "retained", sizeof("retained")) == 0);
    /* No provider means input construction fails before event admission. */
    assert(!snag_app_irc_take_pending(&app, NULL, false));
    assert(app.irc_urgent.len == sizeof("retained"));
    assert(app.input_closed);
    app.input_closed = false;
    app.session.active_turn = true;
    /* Invalid UTF-8 exercises failed steering JSON construction without an
     * allocator hook. Neither input nor steering may consume its projection. */
    app.irc_urgent.data[0] = 0xff;
    assert(snag_app_irc_flush_urgent(&app, error, sizeof(error)) < 0);
    assert(app.irc_urgent.len == sizeof("retained"));
    assert(app.input_closed);
    snag_buf_free(&app.irc_urgent);
    snag_ui_free(&app.ui);
    snag_session_close(&app.session);
}

static int
audio_cancel(void *opaque, unsigned int wait_ms)
{
    (void)opaque; (void)wait_ms; return 2;
}

struct video_audio_fixture {
    struct snag_session *session;
    const struct snag_config *config;
    int root_fd;
    const char *original;
    unsigned int calls;
    char source[SNAG_ID_HEX_LEN + 1u];
};

static int
fixture_video_transcribe(void *opaque, const json_t *source, uint64_t start, uint64_t end, json_t **result)
{
    struct video_audio_fixture *fixture = opaque;
    assert(start == 0u && end == 1u && ++fixture->calls == 1u);
    strcpy(fixture->source, snag_json_string(source, "id"));
    /* The source snapshot serves both operations after the original is gone. */
    assert(unlink(fixture->original) == 0);
    return snag_tools_transcribe_asset(fixture->session, source, start, end, fixture->root_fd,
        fixture->config, NULL, NULL, SNAG_WAKE_INVALID, result);
}

static void
test_audio_transport(void)
{
    struct snag_config config;
    struct snag_credential credential;
    struct snag_buf wav, out;
    struct local_server server;
    char endpoint[128], error[256];
    make_fixture_wav();
    snag_config_init(&config);
    config.providers[0].auth = SNAG_AUTH_API_KEY;
    config.providers[0].request_timeout_ms = 1500u;
    strcpy(config.providers[0].base_url, "https://openrouter.ai/api/v1");
    strcpy(config.providers[0].openrouter_referer, "https://github.com/snajpa/snajpagent");
    strcpy(config.providers[0].openrouter_title, "snajpagent");
    credential_set(&credential, "transport-secret");
    snag_buf_init(&wav, 2048u); snag_buf_init(&out, 4096u);
    assert(snag_buf_append(&wav, fixture_wav, sizeof(fixture_wav)) == 0);
    json_t *request = json_pack("{s:s,s:s,s:s,s:s}", "model", "audio-fixture",
        "response_format", "wav", "voice", "alloy", "input", "fixture text");
    assert(json_object_set_new(request, "question", json_string("What sounds?")) == 0);
    for (int mode = MODEL_AUDIO_LISTEN; mode <= MODEL_AUDIO_FAILURE; ++mode) {
        enum snag_audio_operation op = mode == MODEL_AUDIO_TRANSCRIBE ? SNAG_AUDIO_TRANSCRIBE :
            mode == MODEL_AUDIO_SPEAK ? SNAG_AUDIO_SPEAK : SNAG_AUDIO_LISTEN;
        start_server(&server, (enum model_fixture)mode, false, "");
        snprintf(endpoint, sizeof(endpoint), "http://127.0.0.1:%u/v1", server.port);
        assert(setenv("SNAJPAGENT_TEST_OPENAI_BASE", endpoint, 1) == 0);
        snag_buf_reset(&out);
        assert(snag_buf_append(&out, "kept:", 5u) == 0);
        int rc = snag_provider_audio(op, request, &wav, &config, &config.providers[0],
            &credential, NULL, NULL, &out, error, sizeof(error));
        if (mode == MODEL_AUDIO_FAILURE) {
            assert(rc < 0 && out.len == 5u && strstr(error, "HTTP 503") && strstr(error, "not retried"));
            assert(!strstr(error, "private audio"));
        } else {
            assert(rc == 0 && out.len > 5u);
            if (mode == MODEL_AUDIO_SPEAK) assert(out.len == 5u + wav.len && !memcmp(out.data + 5u, wav.data, wav.len));
        }
        stop_server(&server);
    }
    /* Callback cancellation is before transport; no server/device necessary. */
    size_t kept = out.len;
    assert(snag_provider_audio(SNAG_AUDIO_LISTEN, request, &wav, &config, &config.providers[0],
        &credential, audio_cancel, NULL, &out, error, sizeof(error)) == 2 && out.len == kept);
    config.providers[0].auth = SNAG_AUTH_CHATGPT;
    assert(snag_provider_audio(SNAG_AUDIO_LISTEN, request, &wav, &config, &config.providers[0],
        &credential, NULL, NULL, &out, error, sizeof(error)) < 0 && out.len == kept);
    config.providers[0].auth = SNAG_AUTH_API_KEY;
    /* Cross curl's upload-buffer boundary, exercise every base64 remainder,
     * and verify escaped question metadata without materialized base64 input. */
    struct snag_buf large;
    snag_buf_init(&large, 24000u);
    for (size_t i = 0; i < 20003u; ++i) assert(snag_buf_putc(&large, (unsigned char)i) == 0);
    assert(json_object_set_new(request, "question", json_string("What \"sounds\"?\n\\details")) == 0);
    for (size_t n = 20001u; n <= 20003u; ++n) {
        large.len = n; audio_expected = large.data; audio_expected_len = n;
        start_server(&server, MODEL_AUDIO_LISTEN, false, "");
        snprintf(endpoint, sizeof(endpoint), "http://127.0.0.1:%u/v1", server.port);
        assert(setenv("SNAJPAGENT_TEST_OPENAI_BASE", endpoint, 1) == 0);
        snag_buf_reset(&out);
        assert(snag_provider_audio(SNAG_AUDIO_LISTEN, request, &large, &config, &config.providers[0],
            &credential, NULL, NULL, &out, error, sizeof(error)) == 0);
        stop_server(&server);
    }
    audio_expected = fixture_wav; audio_expected_len = sizeof(fixture_wav);
    snag_buf_free(&large);
    if (getenv("SNAJPAGENT_TEST_MEDIA")) {
        struct snag_store store;
        struct snag_session session;
        char *temp = snag_path_join(getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", "snag-audio-XXXXXX");
        assert(temp && mkdtemp(temp));
        snag_store_init(&store); snag_session_init(&session);
        assert(snag_store_open(&store, temp, error, sizeof(error)) == 0);
        assert(snag_session_create(&store, &session, temp, "default", "audio-fixture", "medium", error, sizeof(error)) == 0);
        char *file = snag_path_join(temp, "fixture audio.wav");
        int fd = open(file, O_WRONLY | O_CREAT | O_EXCL, 0600);
        assert(fd >= 0 && write(fd, fixture_wav, sizeof(fixture_wav)) == (ssize_t)sizeof(fixture_wav) && close(fd) == 0);
        struct snag_buf decoded;
        snag_buf_init(&decoded, 96004u);
        assert(snag_buf_append(&decoded, "kept", 4u) == 0);
        assert(snag_av_pcm(file, 0u, 1u, &decoded, audio_cancel, NULL, error, sizeof(error)) == 2 && decoded.len == 4u);
        assert(snag_av_pcm(file, 1u, 0u, &decoded, NULL, NULL, error, sizeof(error)) < 0 && decoded.len == 4u);
        char *resample_path = snag_path_join(temp, "stereo48.wav");
        FILE *pcm_file = fopen(resample_path, "wb");
        unsigned char header48[44]; memcpy(header48, fixture_wav, sizeof(header48));
        const uint32_t values[] = {384036u, 48000u, 192000u, 384000u};
        const size_t offsets[] = {4u, 24u, 28u, 40u};
        for (size_t k = 0; k < 4u; ++k)
            for (size_t b = 0; b < 4u; ++b) header48[offsets[k] + b] = (unsigned char)(values[k] >> (b * 8u));
        assert(pcm_file && fwrite(header48, 1u, sizeof(header48), pcm_file) == sizeof(header48));
        for (size_t i = 0; i < 96000u; ++i) {
            uint16_t left = i < 48000u ? 1000u : 2000u, right = (uint16_t)(0u - left);
            unsigned char pair[] = {(unsigned char)left, (unsigned char)(left >> 8),
                                    (unsigned char)right, (unsigned char)(right >> 8)};
            assert(fwrite(pair, 1u, 4u, pcm_file) == 4u);
        }
        assert(fclose(pcm_file) == 0);
        assert(snag_av_pcm(resample_path, 1u, 2u, &decoded, NULL, NULL, error, sizeof(error)) == 0);
        assert(decoded.len == 96004u && !memcmp(decoded.data, "kept", 4u));
        assert(decoded.data[48004] == 0xd0 && decoded.data[48005] == 0x07 &&
               decoded.data[48006] == 0x30 && decoded.data[48007] == 0xf8);
        snag_buf_reset(&decoded); assert(snag_buf_append(&decoded, "kept", 4u) == 0);
        decoded.max = 20u;
        assert(snag_av_pcm(file, 0u, 1u, &decoded, NULL, NULL, error, sizeof(error)) < 0 && decoded.len == 4u);
        struct snag_av_audio *reader = NULL;
        assert(snag_av_audio_open(resample_path, 1u, 2u, NULL, NULL, &reader, error, sizeof(error)) == 0);
        assert(unlink(resample_path) == 0); /* Open decoder retains its descriptor. */
        uint32_t frames = 0, total = 0, chunks = 0;
        int16_t samples[16384];
        do {
            assert(snag_av_audio_read(reader, samples, &frames, error, sizeof(error)) == 0);
            assert(frames <= 8192u);
            total += frames; ++chunks;
        } while (frames);
        assert(total == 24000u && chunks > 2u);
        assert(snag_av_audio_read(reader, samples, &frames, error, sizeof(error)) == 0 && !frames);
        snag_av_audio_close(reader);
        free(resample_path); snag_buf_free(&decoded);
        strcpy(config.audio.provider, config.providers[0].name);
        strcpy(config.audio.listen_model, "audio-fixture"); strcpy(config.audio.transcribe_model, "audio-fixture");
        strcpy(config.audio.speech_model, "audio-fixture"); strcpy(config.audio.voice, "alloy");
        assert(snag_secret_source_parse(&config.providers[0].api_key, "\"transport-secret\"", NULL, error, sizeof(error)) == 0);
        const char *names[] = {"listen_audio", "transcribe_audio", "speak_text"};
        /* The real graph and context surface must accept every declared tool. */
        json_t *started=json_pack("{s:{s:s,s:s,s:n,s:s,s:s,s:s,s:i,s:i,s:i,s:i,s:b},"
            "s:s,s:b,s:o,s:n,s:n,s:s,s:s,s:I,s:s}",
            "config","capability_version",SNAJPAGENT_CAPABILITY_VERSION,"effort","medium",
            "max_output_tokens","model","audio-fixture","provider","default","profile_id",SNAJPAGENT_PROFILE_ID,
            "prompt_schema",1,"replay_schema",1,"tool_schema",1,"max_parallel_commands",4,"parallel_tool_calls",1,
            "input_kind","direct","read_only",0,"instructions",json_array(),"queue_id","queue_seq",
            "text","Inspect fixture sound","turn_id","01010101010101010101010101010101","turn_number",(json_int_t)1,
            "cwd",session.cwd);
        assert(started && snag_session_commit(&session, "turn_started", started,
            NULL, error, sizeof(error)) == 0);
        struct snag_context_projection projection;
        memset(&projection,0,sizeof(projection));
        json_t *steering = json_array();
        assert(snag_context_build(&session, "audio-fixture", "medium", 1u, steering, 2048u, true,
            &config, NULL, NULL, "Fixture: tool details hidden; report meaningful progress.",
            &projection, error, sizeof(error), NULL) == 0);
        json_t *input = json_object_get(projection.create_request.value, "input");
        assert(input == json_object_get(projection.count_request.value, "input"));
        bool visible = false;
        for (size_t i = 0; i < json_array_size(input); ++i) {
            json_t *item = json_array_get(input, i);
            const char *role = snag_json_string(item, "role");
            const char *content = snag_json_string(item, "content");
            if (role && !strcmp(role, "system") && content &&
                !strcmp(content, "Fixture: tool details hidden; report meaningful progress."))
                visible = true;
        }
        assert(visible);
        json_t *tools = json_object_get(projection.create_request.value, "tools");
        for (size_t i = 0; i < 3u; ++i) {
            bool declared = false;
            for (size_t j = 0; j < json_array_size(tools); ++j) {
                const char *name = snag_json_string(json_array_get(tools, j), "name");
                if (name && !strcmp(name, names[i])) {
                    declared = true;
                    json_t *properties = json_object_get(json_object_get(json_array_get(tools, j),
                        "parameters"), "properties");
                    for (void *it = json_object_iter(properties); it;
                         it = json_object_iter_next(properties, it)) {
                        const char *description = snag_json_string(json_object_iter_value(it), "description");
                        assert(description && *description);
                    }
                }
            }
            assert(declared);
        }
        snag_context_projection_free(&projection); json_decref(steering);
        /* Invalid arguments must fail locally, with no provider fixture running. */
        const char *bad_audio[] = {
            "{}", "{\"path\":null}", "{\"path\":\"x\",\"start_s\":\"null\"}",
            "{\"path\":\"x\",\"start_s\":86401}", "{\"path\":\"x\",\"end_s\":0}",
            "{\"path\":\"x\",\"end_s\":61}", "{\"path\":\"x\",\"extra\":null}"
        };
        for (size_t i = 0; i < sizeof(bad_audio) / sizeof(bad_audio[0]); ++i) {
            struct snag_response_item invalid = {.name = "transcribe_audio",
                .arguments = json_loadb(bad_audio[i], strlen(bad_audio[i]), 0, NULL)};
            json_t *failure = NULL;
            assert(snag_tools_audio(&invalid, &session, store.root_fd, &config, NULL, NULL,
                                    SNAG_WAKE_INVALID, &failure) == 0);
            assert(!strcmp(snag_json_string(failure, "status"), "failed"));
            json_decref(failure); json_decref(invalid.arguments);
        }
        for (unsigned invalid_mode = 0; invalid_mode < 3u; ++invalid_mode) {
            struct snag_response_item invalid = {.name = invalid_mode ? "speak_text" : "listen_audio"};
            invalid.arguments = invalid_mode ? json_pack("{s:s}", "text", "x") : json_pack("{s:s}", "path", file);
            if (invalid_mode == 1u)
                assert(json_object_set_new(invalid.arguments, "extra", json_null()) == 0);
            if (invalid_mode == 2u)
                assert(json_object_set_new(invalid.arguments, "text", json_stringn("x\0y", 3u)) == 0);
            json_t *failure = NULL;
            assert(snag_tools_audio(&invalid, &session, store.root_fd, &config, NULL, NULL,
                                    SNAG_WAKE_INVALID, &failure) == 0);
            assert(!strcmp(snag_json_string(failure, "status"), "failed"));
            json_decref(failure); json_decref(invalid.arguments);
        }
        for (int mode = MODEL_AUDIO_LISTEN; mode <= MODEL_AUDIO_SPEAK; ++mode) {
            start_server(&server, (enum model_fixture)mode, false, "");
            snprintf(endpoint, sizeof(endpoint), "http://127.0.0.1:%u/v1", server.port);
            assert(setenv("SNAJPAGENT_TEST_OPENAI_BASE", endpoint, 1) == 0);
            struct snag_response_item call = {.name = (char *)names[mode - MODEL_AUDIO_LISTEN]};
            call.arguments = mode == MODEL_AUDIO_SPEAK ? json_pack("{s:s}", "text", "fixture text") :
                json_pack("{s:s,s:i,s:i}", "path", file, "start_s", 0, "end_s", 1);
            if (mode == MODEL_AUDIO_LISTEN) {
                assert(json_object_set_new(call.arguments, "question", json_string("What sounds?")) == 0);
                assert(json_object_del(call.arguments, "start_s") == 0);
            }
            if (mode == MODEL_AUDIO_TRANSCRIBE) {
                assert(json_object_del(call.arguments, "start_s") == 0);
                assert(json_object_del(call.arguments, "end_s") == 0);
            }
            struct snag_response_graph graph;
            memset(&graph,0,sizeof(graph));
            assert(snag_response_graph_add_call(&graph, "audio-item", "audio-call", call.name,
                json_incref(call.arguments)) == 0);
            snag_response_graph_free(&graph);
            json_t *result = NULL;
            assert(snag_tools_audio(&call, &session, store.root_fd, &config, NULL, NULL, SNAG_WAKE_INVALID, &result) == 0);
            if (strcmp(snag_json_string(result, "status"), "succeeded")) fprintf(stderr, "audio: %s\n", snag_json_string(result, "model_text"));
            assert(!strcmp(snag_json_string(result, "status"), "succeeded") && snag_tool_result_valid(result) == 0);
            assert(strstr(snag_json_string(result, "model_text"), "audio-fixture"));
            json_t *parts = json_object_get(result, "content");
            assert(json_array_size(parts) == (mode == MODEL_AUDIO_SPEAK ? 1u : 5u));
            if (mode != MODEL_AUDIO_SPEAK) {
                assert(strstr(snag_json_string(json_array_get(parts, 3u), "text"), "Auxiliary audio API usage"));
                const char *reported = snag_json_string(json_array_get(parts, 4u), "text");
                assert(reported && strstr(reported, mode == MODEL_AUDIO_LISTEN ? "total_tokens" : "seconds"));
            }
            json_decref(result); json_decref(call.arguments);
            stop_server(&server);
        }
        char *video = snag_path_join(temp, "sound.mkv");
        const char *args[] = {"ffmpeg", "-nostdin", "-v", "error", "-f", "lavfi", "-i",
            "color=red:s=64x64:r=4:d=1", "-i", file, "-map", "0:v:0", "-map", "1:a:0",
            "-c:v", "mpeg4", "-c:a", "copy", "-threads", "1", video, NULL};
        snag_buf_reset(&out);
        assert(snag_convert(args, temp, &out, NULL, NULL, SNAG_WAKE_INVALID, error, sizeof(error)) == 0);
        struct snag_response_item call = {.kind = SNAG_ITEM_TOOL_CALL, .name = "view_video"};
        call.arguments = json_pack("{s:s,s:i,s:i,s:i}", "path", video, "start_s", 0, "end_s", 1, "frames", 2);
        struct video_audio_fixture fixture = {.session = &session, .config = &config,
            .root_fd = store.root_fd, .original = video};
        start_server(&server, MODEL_AUDIO_TRANSCRIBE, false, "");
        snprintf(endpoint, sizeof(endpoint), "http://127.0.0.1:%u/v1", server.port);
        assert(setenv("SNAJPAGENT_TEST_OPENAI_BASE", endpoint, 1) == 0);
        json_t *result = NULL;
        assert(snag_tools_video(&call, &session, NULL, &fixture, SNAG_WAKE_INVALID,
            fixture_video_transcribe, &result) == 0);
        if (strcmp(snag_json_string(result, "status"), "succeeded")) fprintf(stderr, "video audio: %s\n", snag_json_string(result, "model_text"));
        assert(!strcmp(snag_json_string(result, "status"), "succeeded") && snag_tool_result_valid(result) == 0);
        json_t *parts = json_object_get(result, "content");
        assert(fixture.calls == 1u && !strcmp(fixture.source,
            snag_json_string(json_object_get(json_array_get(parts, 0u), "asset"), "id")));
        bool transcript_seen = false;
        for (size_t i = 0; i < json_array_size(parts); ++i) {
            const char *text = snag_json_string(json_array_get(parts, i), "text");
            if (text && !strcmp(text, "fixture transcript")) transcript_seen = true;
        }
        assert(transcript_seen);
        json_decref(result); json_decref(call.arguments); free(video); stop_server(&server);
        assert(unlink(file) == 0); free(file);
        snag_session_close(&session); snag_store_close(&store); free(temp);
    }
    assert(unsetenv("SNAJPAGENT_TEST_OPENAI_BASE") == 0);
    json_decref(request); snag_buf_free(&out); snag_buf_free(&wav); snag_config_free(&config);
}

/* Small RFC 6455 wire fixture only; production framing belongs to libcurl. */
static void
ws_read_full(int fd,void *data,size_t length)
{
    unsigned char *p=data;
    while(length) {
        ssize_t n=read(fd,p,length);
        if(n<0 && errno==EINTR)continue;
        if(n<=0)server_fail("WebSocket fixture read failed");
        p+=n;length-=(size_t)n;
    }
}

static unsigned int
ws_read_client(int fd,size_t expected,unsigned char byte)
{
    unsigned char h[2],mask[4],extended[8],buffer[4096];
    ws_read_full(fd,h,2u);
    if(!(h[0]&0x80u) || !(h[1]&0x80u))server_fail("WebSocket fixture expected final masked frame");
    uint64_t length=h[1]&127u;
    if(length>=126u) {
        size_t n=length==126u?2u:8u;length=0;
        ws_read_full(fd,extended,n);
        for(size_t i=0;i<n;++i)length=(length<<8u)|extended[i];
    }
    if(length!=expected)server_fail("WebSocket fixture payload length mismatch");
    ws_read_full(fd,mask,4u);
    for(size_t offset=0;offset<expected;) {
        size_t n=expected-offset;if(n>sizeof(buffer))n=sizeof(buffer);
        ws_read_full(fd,buffer,n);
        for(size_t i=0;i<n;++i)
            if((buffer[i]^mask[(offset+i)%4u])!=byte)server_fail("WebSocket fixture duplicated or changed payload");
        offset+=n;
    }
    return h[0]&15u;
}

static void
ws_server(unsigned int mode,int listen_fd,bool native)
{
    alarm(15u);
    int fd=accept(listen_fd,NULL,NULL);if(fd<0)server_fail("WebSocket fixture accept failed");
    struct http_request request;read_request(fd,&request);
    const char *path = native ? "/backend-api/codex/rtc_native" :
        "/v1/realtime?model=fixture%20voice";
    if (strcmp(request.method,"GET") || strcmp(request.path,path) ||
        !header_contains(request.headers,"Authorization: Bearer transport-secret") ||
        (native && !header_contains(request.headers,"openai-alpha: quicksilver=v2")) ||
        !header_contains(request.headers,"Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ=="))
        server_fail("WebSocket handshake URL/nonce mismatch");
    if(mode==4u) {
        send_response(fd,401u,"application/json","{\"error\":\"private denied body\"}");close(fd);_Exit(0);
    }
    if(mode==5u) {while(read(fd,&mode,1u)>0){}close(fd);_Exit(0);}
    const char *upgrade="HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n"
        "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n";
    write_all_or_die(fd,upgrade,strlen(upgrade));
    if(mode) {
        const char *frame=mode==1u?"\202\001x":mode==2u?"\201\176\020\000x":"\201\002\377\377";
        write_all_or_die(fd,frame,mode==1u?3u:mode==2u?5u:4u);
        char discard;while(read(fd,&discard,1u)>0){}close(fd);_Exit(0);
    }
    if(ws_read_client(fd,4u*1024u*1024u,'q')!=1u)server_fail("Expected one complete client text message");
    /* Fragmented text, interleaved ping, and a UTF-8 sequence across fragments. */
    const unsigned char frames[]={0x01,2,'A',0xe2,0x89,1,'p',0x80,3,0x82,0xac,'B'};
    for(size_t i=0;i<sizeof(frames);++i)write_all_or_die(fd,(const char *)frames+i,1u);
    if(ws_read_client(fd,1u,'p')!=10u)server_fail("WebSocket automatic pong missing");
    const unsigned char large[]={0x81,126,0x80,1};
    write_all_or_die(fd,(const char *)large,sizeof(large));
    char block[4096];memset(block,'x',sizeof(block));
    for(size_t left=32769u;left;) {
        size_t n=left>sizeof(block)?sizeof(block):left;
        write_all_or_die(fd,block,n);left-=n;
    }
    write_all_or_die(fd,"\210\002\003\350",4u);
    char discard;while(read(fd,&discard,1u)>0){}close(fd);_Exit(0);
}

static int
ws_cancel(void *opaque,unsigned int timeout)
{
    (void)timeout;
    return snag_monotonic_ms()>=*(uint64_t *)opaque?2:0;
}

static void
test_voice_socket(void)
{
    struct snag_provider_config provider;struct snag_credential credential;
    snag_config_provider_init(&provider,"default");snag_credential_clear(&credential);
    provider.auth=SNAG_AUTH_API_KEY;provider.connect_timeout_ms=2000u;
    strcpy(provider.openrouter_referer,"https://github.com/snajpa/snajpagent");
    strcpy(provider.openrouter_title,"snajpagent");
    strcpy(credential.value,"transport-secret");credential.len=strlen(credential.value);
    struct snag_voice_socket *voice=NULL;char error[256];
    strcpy(provider.base_url,"ftp://remote.invalid");
    assert(snag_provider_voice_open(&provider,&credential,"fixture",NULL,NULL,&voice,error,sizeof(error))<0 && !voice);
    strcpy(provider.base_url,"http://127.0.0.1:1/v1/");
    assert(snag_provider_voice_open(&provider,&credential,"fixture",NULL,NULL,
        &voice,error,sizeof(error))<0 && !voice);
    assert(strstr(error,"/backend-api/codex"));
    for (unsigned int test=0;test<18u;++test) {
        unsigned int mode = test % 6u;
        unsigned int route = test / 6u;
        bool native = route == 2u;
        fprintf(stderr,"WebSocket fixture route %u mode %u\n",route,mode);
        struct local_server server;
        struct sockaddr_in address; socklen_t address_size=sizeof(address);
        memset(&server,0,sizeof(server));memset(&address,0,sizeof(address));
        server.fd=socket(AF_INET,SOCK_STREAM,0);assert(server.fd>=0);
        address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        assert(bind(server.fd,(struct sockaddr *)&address,sizeof(address))==0 && listen(server.fd,1)==0);
        assert(getsockname(server.fd,(struct sockaddr *)&address,&address_size)==0);
        server.port=ntohs(address.sin_port);
        server.pid=fork();assert(server.pid>=0);
        if (!server.pid) ws_server(mode,server.fd,native);
        close(server.fd);server.fd=-1;
        snprintf(provider.base_url,sizeof(provider.base_url),"http://%s:%u/%s",
            route ? "localhost" : "127.0.0.1",server.port,native ? "backend-api/codex" : "v1/");
        uint64_t deadline=snag_monotonic_ms()+200u;
        int rc = native ? snag_provider_voice_attach(&provider,&credential,"rtc_native",
            mode==5u?ws_cancel:NULL,&deadline,&voice,error,sizeof(error)) :
            snag_provider_voice_open(&provider,&credential,"fixture voice",mode==5u?ws_cancel:NULL,
                &deadline,&voice,error,sizeof(error));
        if(mode>=4u) {
            assert(rc==(mode==5u?2:-1) && !voice);
            if(mode==4u)assert(strstr(error,"401") && !strstr(error,"private denied body"));
            stop_server(&server);continue;
        }
        if (rc) {
            fprintf(stderr,"WebSocket fixture: %s\n",error);
            int status;
            (void)kill(server.pid,SIGTERM);
            assert(waitpid(server.pid,&status,0)==server.pid);
        }
        assert(rc==0 && voice);
        struct snag_buf received;snag_buf_init(&received,mode?64u:65536u);
        deadline=snag_monotonic_ms()+10000u;
        if(!mode) {
            size_t length=4u*1024u*1024u,offset=0u;char *payload=malloc(length);assert(payload);
            memset(payload,'q',length);
            while(offset<length) {
                assert(snag_monotonic_ms()<deadline);
                assert(snag_provider_voice_send(voice,payload,length,&offset,error,sizeof(error))==0);
                if(offset<length)assert(snag_provider_voice_wait(voice,true,20u)==0);
            }
            free(payload);
        }
        unsigned int messages=0;
        do {
            assert(snag_monotonic_ms()<deadline);
            rc=snag_provider_voice_receive(voice,&received,error,sizeof(error));
            if(rc==1) {
                assert(!mode && messages<2u);
                if(!messages)assert(received.len==5u && !memcmp(received.data,"A\342\202\254B",5u));
                else {
                    assert(received.len==32769u);
                    for(size_t i=0;i<received.len;++i)assert(received.data[i]=='x');
                }
                ++messages;snag_buf_reset(&received);
            }
            if(rc==0)assert(snag_provider_voice_wait(voice,false,20u)==0);
        }while(rc>=0);
        assert(messages==(mode?0u:2u));
        snag_provider_voice_close(voice);voice=NULL;snag_buf_free(&received);
        stop_server(&server);
    }
    snag_credential_clear(&credential);
}

struct voice_fixture {
    json_t *sent,*notices;
    uint32_t played,frames,interrupts,ends;
    const char *audio_item;
};
static int voice_send(void *opaque,const json_t *event)
{
    return json_array_append(((struct voice_fixture *)opaque)->sent,(json_t *)event);
}
static int voice_notice(void *opaque,const json_t *event)
{
    return json_array_append(((struct voice_fixture *)opaque)->notices,(json_t *)event);
}
static int voice_play(void *opaque,const char *item,const int16_t *pcm,uint32_t frames)
{
    struct voice_fixture *f=opaque;
    assert(!strcmp(item,f->audio_item?f->audio_item:"audio-1"));
    if(!frames) {assert(!pcm);++f->ends;return 0;}
    assert(frames==24u);
    for(uint32_t i=0;i<frames;++i)assert(pcm[i]==(int16_t)i-12);
    f->frames+=frames;return 0;
}
static uint32_t voice_interrupt(void *opaque)
{
    struct voice_fixture *f=opaque;++f->interrupts;return f->played;
}
static json_t *voice_last(json_t *events)
{
    assert(json_array_size(events));return json_array_get(events,json_array_size(events)-1u);
}
static int voice_deliver(struct snag_voice *voice,json_t *event)
{
    char error[256];assert(event);
    int rc=snag_voice_event(voice,event,error,sizeof(error));json_decref(event);return rc;
}
static void
voice_orientation(const json_t *session)
{
    const char *instructions = snag_json_string(session, "instructions");
    assert(instructions && strstr(instructions, "voice model"));
    assert(strstr(instructions, "The model means the working model"));
    assert(strstr(instructions, "The CLI is another interface to this same session"));
    assert(strstr(instructions, "do not automatically request new work"));
    assert(strstr(instructions, "do not authenticate a speaker or add permissions"));
}

static struct snag_voice *voice_start(struct voice_fixture *f)
{
    memset(f,0,sizeof(*f));f->sent=json_array();f->notices=json_array();assert(f->sent && f->notices);
    struct snag_voice_io io={voice_send,voice_notice,voice_play,voice_interrupt};
    struct snag_voice *voice=snag_voice_new(&io,f,"fixture-model","fixture-asr","fixture-voice");
    char error[256];assert(voice && !snag_voice_ready(voice));
    assert(snag_voice_begin(voice,error,sizeof(error))==0);
    json_t *session=json_object_get(voice_last(f->sent),"session");assert(json_is_object(session));
    voice_orientation(session);
    assert(json_array_size(json_object_get(session,"tools"))==1u);
    assert(voice_deliver(voice,json_pack("{s:s,s:O}","type","session.updated","session",session))==0);
    assert(snag_voice_ready(voice));return voice;
}
static void voice_end(struct snag_voice *voice,struct voice_fixture *f)
{
    snag_voice_free(voice);json_decref(f->sent);json_decref(f->notices);
}

static size_t voice_notice_count(const struct voice_fixture *,const char *);

static void test_audio_provider_selection(void)
{
    struct snag_config config;snag_config_init(&config);
    struct snag_audio_config resolved;
    const struct snag_provider_config *provider=snag_provider_audio_config(&config,NULL,&resolved);
    assert(provider && !strcmp(resolved.provider,provider->name));
    assert(!strcmp(resolved.realtime_model,"gpt-realtime") && !strcmp(resolved.voice,"marin"));
    assert(!config.audio.provider[0] && !config.audio.realtime_model[0]);
    strcpy(config.providers[0].base_url,SNAG_CHATGPT_BASE);
    config.providers[0].auth=SNAG_AUTH_CHATGPT;
    provider=snag_provider_audio_config(&config,NULL,&resolved);
    assert(provider && snag_provider_native_audio(provider));
    assert(!strcmp(resolved.realtime_model,"gpt-live-1-codex") && !strcmp(resolved.voice,"cove"));
    assert(!strcmp(resolved.transcribe_model,"gpt-4o-transcribe"));
    config.providers[0].auth=SNAG_AUTH_API_KEY;
    strcpy(config.providers[0].base_url,"http://127.0.0.1:2455/backend-api/codex/");
    assert(snag_provider_native_audio(snag_provider_audio_config(&config,NULL,&resolved)));
    config.provider_count=2u;snag_config_provider_init(&config.providers[1],"byok");
    strcpy(config.audio.provider,"byok");strcpy(config.audio.realtime_model,"custom-voice-model");
    strcpy(config.audio.voice,"custom-voice");
    provider=snag_provider_audio_config(&config,config.providers[0].name,&resolved);
    assert(provider==&config.providers[1] && !snag_provider_native_audio(provider));
    assert(!strcmp(resolved.realtime_model,"custom-voice-model") &&
        !strcmp(resolved.voice,"custom-voice"));
    snag_config_free(&config);
}

static void test_native_voice_playback(void)
{
    struct voice_fixture f={.sent=json_array(),.notices=json_array(),.audio_item="native-output"};
    struct snag_voice_io io={.send=voice_send,.notice=voice_notice,
        .play=voice_play,.interrupt=voice_interrupt};
    struct snag_voice *v=snag_voice_new(&io,&f,"gpt-live-1-codex","gpt-4o-transcribe","marin");
    assert(v);
    json_decref(snag_voice_native_session(v));
    char error[256];int16_t pcm[24];
    for (uint32_t i=0;i<24u;++i)pcm[i]=(int16_t)i-12;
    assert(snag_voice_native_output(v,pcm,24u)<0);
    assert(snag_voice_begin(v,error,sizeof(error))==0);
    /* RTC has no turn ID and can precede sideband assistant metadata. */
    assert(snag_voice_native_output(v,pcm,24u)==0 && f.frames==24u);
    assert(voice_deliver(v,json_pack("{s:s,s:{s:s,s:s}}","type","turn.created",
        "turn","id","user-1","role","user"))==0 && f.interrupts==1u);
    /* Barge-in flushes queued speech, not the entire future RTC stream. */
    assert(snag_voice_native_output(v,pcm,24u)==0 && f.frames==48u);
    assert(voice_deliver(v,json_pack("{s:s,s:{s:s,s:s}}","type","turn.created",
        "turn","id","audio-1","role","assistant"))==0);
    assert(snag_voice_native_output(v,pcm,24u)==0 && f.frames==72u);
    assert(voice_deliver(v,json_pack("{s:s,s:{s:s,s:s,s:s}}","type","turn.done",
        "turn","id","audio-1","role","assistant","transcript","Hello."))==0);
    assert(f.ends==1u);
    assert(snag_voice_native_output(v,pcm,24u)==0 && f.frames==96u);
    assert(voice_deliver(v,json_pack("{s:s,s:{s:s,s:s}}","type","turn.created",
        "turn","id","user-2","role","user"))==0 && f.interrupts==2u);
    assert(snag_voice_mute(v,true,error,sizeof(error))==0);
    assert(snag_voice_native_output(v,pcm,24u)==0 && f.frames==120u);
    voice_end(v,&f);
}

static void test_native_voice_protocol(void)
{
    struct voice_fixture f={.sent=json_array(),.notices=json_array()};char error[256];
    struct snag_voice_io io={voice_send,voice_notice,voice_play,voice_interrupt};
    struct snag_voice *v=snag_voice_new(&io,&f,"gpt-live-1-codex","gpt-4o-transcribe","cove");
    json_t *session=snag_voice_native_session(v);
    assert(session &&
        !strcmp(snag_json_string(json_object_get(session,"delegation"),"type"),"client"));
    voice_orientation(session);
    json_decref(session);
    assert(snag_voice_begin(v,error,sizeof(error))==0 && snag_voice_ready(v));
    assert(json_array_size(f.sent)==0u); /* Existing native call, no public session.update. */
    json_t *context = json_pack("{s:s}", "active_turn_id", "fixture");
    assert(snag_voice_context(v, context, error, sizeof(error)) == 0);
    json_decref(context);
    assert(!strcmp(snag_json_string(voice_last(f.sent), "channel"), "commentary"));
    json_t *created=json_pack("{s:s,s:{s:s,s:s}}","type","turn.created",
        "turn","id","native-input","role","user");
    assert(voice_deliver(v,created)==0 && f.interrupts==1u);
    json_t *delegation=json_pack("{s:s,s:{s:s,s:s,s:s,s:s,s:[{s:s,s:s}]}}",
        "type","delegation.created",
        "item","id","native-call","type","delegation","target","client",
        "user_bidi_turn_id","native-input",
        "content","type","input_text","text","inspect the worktree");
    assert(voice_deliver(v,json_incref(delegation))==0);
    assert(voice_notice_count(&f,"voice_handoff")==0u); /* Wait for final user transcript. */
    assert(voice_deliver(v,json_pack("{s:s,s:{s:s,s:s,s:s}}","type","turn.done","turn",
        "id","native-input","role","user","transcript","please inspect the worktree"))==0);
    assert(voice_notice_count(&f,"voice_handoff")==1u &&
        voice_notice_count(&f,"voice_transcript")==1u);
    assert(voice_deliver(v,json_incref(delegation))==0 &&
        voice_notice_count(&f,"voice_handoff")==1u);
    /* Coding can remain pending while a later spoken instruction arrives. */
    assert(voice_deliver(v,json_pack("{s:s,s:{s:s,s:s,s:s}}","type","turn.done","turn",
        "id","native-followup","role","user","transcript","also check the tests"))==0);
    assert(voice_deliver(v,json_pack("{s:s,s:{s:s,s:s,s:s,s:[{s:s,s:s}]}}",
        "type","delegation.created","item","id","native-call-2","target","client",
        "user_bidi_turn_id","native-followup","content","type","input_text",
        "text","check the tests"))==0);
    assert(voice_notice_count(&f,"voice_handoff")==2u && snag_voice_ready(v));
    assert(snag_voice_progress(v, "native-call-2", "request accepted",
        error, sizeof(error)) == 0);
    assert(!strcmp(snag_json_string(voice_last(f.sent), "type"), "delegation.context.append"));
    assert(snag_voice_output(v, "Model output:\nThe build finished.",
        error, sizeof(error)) == 0);
    assert(!strcmp(snag_json_string(voice_last(f.sent), "type"), "session.context.append"));
    assert(!strcmp(snag_json_string(voice_last(f.sent), "channel"), "speakable"));
    assert(!json_object_get(voice_last(f.sent), "delegation_item_id"));
    assert(snag_voice_result(v, "native-call-2", "tests checked", error, sizeof(error)) == 0);
    assert(!strcmp(snag_json_string(voice_last(f.sent),"delegation_item_id"),"native-call-2"));
    assert(voice_deliver(v,json_incref(delegation))==0 &&
        voice_notice_count(&f,"voice_handoff")==2u);
    assert(snag_voice_result(v,"native-call","inspection complete",error,sizeof(error))==0);
    assert(!strcmp(snag_json_string(voice_last(f.sent),"delegation_item_id"),"native-call"));
    assert(voice_deliver(v,json_incref(delegation))==0 &&
        voice_notice_count(&f,"voice_handoff")==2u && snag_voice_ready(v));
    assert(snag_voice_result(v,"wrong-call","result",error,sizeof(error))<0);
    voice_end(v,&f);json_decref(delegation);

    memset(&f,0,sizeof(f));f.sent=json_array();f.notices=json_array();
    v=snag_voice_new(&io,&f,"gpt-live-1-codex","gpt-4o-transcribe","cove");
    json_decref(snag_voice_native_session(v));assert(snag_voice_begin(v,error,sizeof(error))==0);
    assert(voice_deliver(v,json_pack("{s:s,s:{s:s,s:s,s:s}}","type","turn.done","turn",
        "id","native-input","role","user","transcript","please inspect the worktree"))==0);
    assert(voice_deliver(v,json_pack("{s:s,s:{s:s,s:s,s:s,s:[{s:s,s:s}]}}",
        "type","delegation.created",
        "item","id","native-call","target","client","user_bidi_turn_id","native-input",
        "content","type","input_text","text","inspect the worktree"))==0);
    assert(voice_notice_count(&f,"voice_handoff")==1u);
    assert(snag_voice_result(v,"native-call","inspection complete",error,sizeof(error))==0);
    assert(!strcmp(snag_json_string(voice_last(f.sent),"type"),"delegation.context.append"));
    assert(!strcmp(snag_json_string(voice_last(f.sent),"channel"),"speakable"));
    assert(snag_voice_mute(v,true,error,sizeof(error))==0);
    size_t sent=json_array_size(f.sent);
    assert(snag_voice_respond(v,true,error,sizeof(error))==0 && json_array_size(f.sent)==sent);
    voice_end(v,&f);
}

static json_t *
native_delegation(const char *call, const char *input)
{
    return json_pack("{s:s,s:{s:s,s:s,s:s,s:[{s:s,s:s}]}}",
        "type", "delegation.created", "item", "id", call, "target", "client",
        "user_bidi_turn_id", input, "content", "type", "input_text", "text", input);
}

static void
test_native_voice_concurrency(void)
{
    for (unsigned int delayed = 0; delayed < 2u; ++delayed) {
        struct voice_fixture f = {.sent = json_array(), .notices = json_array()};
        struct snag_voice_io io = {voice_send, voice_notice, voice_play, voice_interrupt};
        struct snag_voice *v = snag_voice_new(&io, &f, "voice", "transcribe", "cove");
        char error[256], ids[SNAG_VOICE_HANDOFFS + 2u][32];

        assert(v);
        json_decref(snag_voice_native_session(v));
        assert(snag_voice_begin(v, error, sizeof(error)) == 0);
        for (size_t i = 0; i < SNAG_VOICE_HANDOFFS + 2u; ++i) {
            assert(snprintf(ids[i], sizeof(ids[i]), "native-%zu", i) > 0);
        }
        for (size_t i = 0; i < SNAG_VOICE_HANDOFFS; ++i) {
            if (!delayed) {
                assert(voice_deliver(v, json_pack("{s:s,s:{s:s,s:s,s:s}}",
                    "type", "turn.done", "turn", "id", ids[i], "role", "user",
                    "transcript", ids[i])) == 0);
            }
            assert(voice_deliver(v, native_delegation(ids[i], ids[i])) == 0);
        }
        if (delayed) {
            assert(voice_notice_count(&f, "voice_handoff") == 0u);
            for (size_t i = SNAG_VOICE_HANDOFFS; i-- > 0u;) {
                assert(voice_deliver(v, json_pack("{s:s,s:{s:s,s:s,s:s}}",
                    "type", "turn.done", "turn", "id", ids[i], "role", "user",
                    "transcript", ids[i])) == 0);
                json_t *handoff = voice_last(f.notices);
                assert(!strcmp(snag_json_string(handoff, "call_id"), ids[i]));
                assert(!strcmp(snag_json_string(handoff, "transcript"), ids[i]));
            }
        }
        assert(voice_notice_count(&f, "voice_handoff") == SNAG_VOICE_HANDOFFS);
        /* A full coding map must not stop conversation or reassign old work. */
        const char *refused = ids[SNAG_VOICE_HANDOFFS];
        assert(voice_deliver(v, json_pack("{s:s,s:{s:s,s:s,s:s}}",
            "type", "turn.done", "turn", "id", refused, "role", "user",
            "transcript", refused)) == 0);
        assert(voice_deliver(v, native_delegation(refused, refused)) == 0);
        assert(snag_voice_ready(v));
        assert(!strcmp(snag_json_string(voice_last(f.notices), "status"), "refused"));
        assert(!strcmp(snag_json_string(voice_last(f.sent), "delegation_item_id"), refused));
        assert(snag_voice_result(v, ids[0], "first result", error, sizeof(error)) == 0);
        assert(voice_deliver(v, native_delegation(refused, refused)) == 0);
        assert(voice_notice_count(&f, "voice_handoff") == SNAG_VOICE_HANDOFFS);
        const char *fresh = ids[SNAG_VOICE_HANDOFFS + 1u];
        assert(voice_deliver(v, json_pack("{s:s,s:{s:s,s:s,s:s}}",
            "type", "turn.done", "turn", "id", fresh, "role", "user",
            "transcript", fresh)) == 0);
        assert(voice_deliver(v, native_delegation(fresh, fresh)) == 0);
        assert(voice_notice_count(&f, "voice_handoff") == SNAG_VOICE_HANDOFFS + 1u);
        /* A reused ID with different source authority is still an integrity error. */
        assert(voice_deliver(v, native_delegation(ids[0], fresh)) < 0);
        voice_end(v, &f);
    }
}

static void
test_native_voice_credential_snapshot(void)
{
    struct local_server server;
    struct snag_config config;
    struct snag_store store;
    struct snag_credential credential;
    struct snag_buf output = {.max = 65536u};
    char path[4096], error[256] = {0}, call[257] = {0};
    const char *tmp = getenv("TMPDIR");

    assert(snprintf(path, sizeof(path), "%s/snajpagent-voice-auth-XXXXXX",
        tmp ? tmp : "/tmp") > 0);
    assert(mkdtemp(path));
    snag_store_init(&store);
    assert(snag_store_open(&store, path, error, sizeof(error)) == 0);
    start_server(&server, MODEL_NATIVE_CALL, false, "/backend-api/codex");
    struct snag_provider_connection conn = transport_connection(&config,
        &credential, server.endpoint);
    config.providers[0].auth = SNAG_AUTH_API_KEY;
    assert(snag_secret_source_parse(&config.providers[0].api_key,
        "\"transport-secret\"", NULL, error, sizeof(error)) == 0);
    assert(snag_auth_read(store.root_fd, conn.provider, false, NULL,
        &credential, NULL, NULL, error, sizeof(error)) == 0);
    assert(credential.root_fd == store.root_fd);
    /* The voice owner keeps the resolved credential, not the mutable source.
     * No login file exists for a provider authenticated by a configured key. */
    snag_secret_source_free(&config.providers[0].api_key);
    json_t *session = json_pack("{s:s}", "model", "gpt-live-1-codex");
    int rc = snag_provider_voice_call(NULL, conn.provider, &credential,
        "v=0\r\n", session, NULL, NULL, &output, call, error, sizeof(error));
    bool valid = rc == 0 && !strcmp(call, "rtc_native") && output.len != 0u;
    bool unchanged = credential.root_fd == store.root_fd &&
        !strcmp(credential.value, "transport-secret");
    json_decref(session);
    snag_buf_free(&output);
    snag_credential_clear(&credential);
    snag_config_free(&config);
    snag_store_close(&store);
    stop_server(&server);
    if (!valid) fprintf(stderr, "native voice credential snapshot: %s\n", error);
    assert(valid && unchanged);
}

static void test_native_voice_transport(void)
{
    for (unsigned int direct=0;direct<2u;++direct)
        for (int mode=MODEL_NATIVE_TRANSCRIBE;mode<=MODEL_NATIVE_CALL_DENIED;++mode) {
        struct local_server server;struct snag_config config;struct snag_credential credential;
        char error[256]={0},call[257]={0};struct snag_buf output={.max=65536u};
        start_server(&server,(enum model_fixture)mode,false,"/backend-api/codex");
        struct snag_provider_connection conn=transport_connection(&config,&credential,
            direct?SNAG_CHATGPT_BASE:server.endpoint);
        config.providers[0].auth=direct?SNAG_AUTH_CHATGPT:SNAG_AUTH_API_KEY;
        if (direct)strcpy(credential.account_id,"acct-test");
        assert(setenv("SNAJPAGENT_TEST_OPENAI_BASE",server.endpoint,1)==0);
        int rc;
        if (mode==MODEL_NATIVE_TRANSCRIBE) {
            make_fixture_wav();
            struct snag_buf wav={.data=fixture_wav,.len=sizeof(fixture_wav),
                .max=sizeof(fixture_wav)};
            json_t *request=json_pack("{s:s}","model","gpt-4o-transcribe");
            rc=snag_provider_audio(SNAG_AUDIO_TRANSCRIBE,request,&wav,&config,
                conn.provider,&credential,
                NULL,NULL,&output,error,sizeof(error));json_decref(request);
            assert(!rc && output.len && !memcmp(output.data,"{\"text\":",8u));
        } else {
            json_t *session=json_pack("{s:s}","model","gpt-live-1-codex");
            rc=snag_provider_voice_call(&config,conn.provider,&credential,
                "v=0\r\n",session,NULL,NULL,
                &output,call,error,sizeof(error));json_decref(session);
            if (mode==MODEL_NATIVE_CALL)assert(!rc && !strcmp(call,"rtc_native") && output.len);
            else assert(rc<0 && !call[0] && error[0]);
        }
        snag_buf_free(&output);
        snag_config_free(&config);
        snag_credential_clear(&credential);stop_server(&server);
    }
    assert(unsetenv("SNAJPAGENT_TEST_OPENAI_BASE")==0);
}

#if SNAJPAGENT_AUDIO_DEVICE
static void native_echo(int id,const char *data,int length,void *opaque)
{
    (void)opaque;
    if (length<12 || length>2048 || (((const unsigned char *)data)[1]&127u)!=111u)return;
    unsigned char packet[2048];memcpy(packet,data,(size_t)length);
    packet[8]=packet[9]=packet[10]=0;packet[11]=88;
    (void)rtcSendMessage(id,(const char *)packet,length);
}
static void native_gathered(int id,rtcGatheringState state,void *opaque)
{(void)id;if (state==RTC_GATHERING_COMPLETE)atomic_store((atomic_bool *)opaque,true);}
static void test_native_media(void)
{
    struct snag_voice_rtc *media=NULL;char error[256]={0},answer[32768];
    struct snag_buf offer={.max=32768u};atomic_bool gathered;atomic_init(&gathered,false);
    assert(snag_voice_rtc_open(&media,error,sizeof(error))==0);
    uint64_t deadline=snag_monotonic_ms()+10000u;int rc=0;
    while (!rc && snag_monotonic_ms()<deadline) {
            rc=snag_voice_rtc_offer(media,&offer);snag_sleep_ms(5u);}
    assert(rc==1);
    rtcConfiguration configuration={.disableAutoNegotiation=true,.forceMediaTransport=true};
    int pc=rtcCreatePeerConnection(&configuration);assert(pc>=0);rtcSetUserPointer(pc,&gathered);
    assert(rtcSetGatheringStateChangeCallback(pc,native_gathered)==0);
    rtcTrackInit init={.direction=RTC_DIRECTION_SENDRECV,
        .codec=RTC_CODEC_OPUS,.payloadType=111,.ssrc=88,.mid="0"};
    int track=rtcAddTrackEx(pc,&init);assert(track>=0);rtcSetUserPointer(track,&gathered);
    assert(rtcSetMessageCallback(track,native_echo)==0);
    assert(rtcSetRemoteDescription(pc,(char *)offer.data,"offer")==0 &&
        rtcSetLocalDescription(pc,"answer")==0);
    while (!atomic_load(&gathered) && snag_monotonic_ms()<deadline)snag_sleep_ms(5u);
    assert(atomic_load(&gathered) && rtcGetLocalDescription(pc,answer,sizeof(answer))>0);
    assert(snag_voice_rtc_answer(media,answer)==0);
    while (!snag_voice_rtc_ready(media) && snag_monotonic_ms()<deadline)snag_sleep_ms(5u);
    assert(snag_voice_rtc_ready(media));
    int16_t input[480],output[2880];for (unsigned int i=0;i<480u;++i)input[i]=(i/24u)%2u?8000:-8000;
    unsigned int samples=0,peak=0;uint64_t next=snag_monotonic_ms();deadline=next+1200u;
    while (snag_monotonic_ms()<deadline) {
        if (snag_monotonic_ms()>=next) {
            assert(snag_voice_rtc_input(media,input,480u)==0);next+=20u;}
        int n=snag_voice_rtc_output(media,output,2880u);assert(n>=0);
        samples+=(unsigned int)n;
        for (int i = 0; i < n; ++i) {
            unsigned int v = output[i] < 0 ? -output[i] : output[i];
            if (v > peak) {
                peak = v;
            }
        }
        snag_sleep_ms(2u);
    }
    assert(samples>12000u && peak>1000u);
    assert(snag_voice_rtc_input(media,input,200u)==0 && snag_voice_rtc_input(media,NULL,0u)==0);
    snag_voice_rtc_flush(media);snag_voice_rtc_close(media);
    rtcSetMessageCallback(track,NULL);rtcDeleteTrack(track);
    rtcDeletePeerConnection(pc);snag_buf_free(&offer);
}
#endif
static void voice_commit(struct snag_voice *voice,const char *id,const char *transcript)
{
    assert(voice_deliver(voice,json_pack("{s:s,s:s}","type","input_audio_buffer.committed","item_id",id))==0);
    if(transcript)assert(voice_deliver(voice,json_pack("{s:s,s:s,s:s,s:i}","type",
        "conversation.item.input_audio_transcription.completed","item_id",id,"transcript",transcript,"content_index",0))==0);
}
static void voice_created(struct snag_voice *voice,struct voice_fixture *f,const char *id)
{
    json_t *request=voice_last(f->sent);assert(!strcmp(snag_json_string(request,"type"),"response.create"));
    json_t *meta=json_object_get(json_object_get(request,"response"),"metadata");
    assert(voice_deliver(voice,json_pack("{s:s,s:{s:s,s:O}}","type","response.created","response","id",id,"metadata",meta))==0);
}
static json_t *voice_call_response(const char *id,const char *status)
{
    return json_pack("{s:s,s:{s:s,s:s,s:[{s:s,s:s,s:s,s:s,s:s}]}}","type","response.done","response",
        "id",id,"status",status,"output","id","function-1","type","function_call","name","ask_agent",
        "call_id","call-1","arguments","{\"request\":\"voice paraphrase\"}");
}
static size_t voice_notice_count(const struct voice_fixture *f,const char *type)
{
    size_t count=0;
    for(size_t i=0;i<json_array_size(f->notices);++i)
        if(!strcmp(snag_json_string(json_array_get(f->notices,i),"type"),type))++count;
    return count;
}
static void
test_voice_concurrent_results(void)
{
    struct voice_fixture f;
    struct snag_voice *voice = voice_start(&f);
    char error[256];

    voice_commit(voice, "first", "first request");
    assert(snag_voice_respond(voice, true, error, sizeof(error)) == 0);
    voice_created(voice, &f, "response-1");
    assert(voice_deliver(voice, voice_call_response("response-1", "completed")) == 0);
    voice_commit(voice, "second", "second request");
    assert(snag_voice_respond(voice, true, error, sizeof(error)) == 0);
    voice_created(voice, &f, "response-2");
    json_t *response = voice_call_response("response-2", "completed");
    json_t *item = json_array_get(json_object_get(json_object_get(response, "response"),
        "output"), 0u);
    assert(json_object_set_new(item, "call_id", json_string("call-2")) == 0);
    assert(voice_deliver(voice, response) == 0);
    assert(voice_notice_count(&f, "voice_handoff") == 2u);
    assert(snag_voice_progress(voice, "call-2", "request accepted", error, sizeof(error)) == 0);
    item = json_object_get(voice_last(f.sent), "item");
    assert(!strcmp(snag_json_string(item, "type"), "message"));
    assert(snag_voice_output(voice, "Model output:\nThe build finished.",
        error, sizeof(error)) == 0);
    item = json_object_get(voice_last(f.sent), "item");
    assert(!strcmp(snag_json_string(item, "type"), "message"));
    assert(!json_object_get(item, "call_id"));
    assert(snag_voice_result(voice, "call-2", "second result", error, sizeof(error)) == 0);
    item = json_object_get(voice_last(f.sent), "item");
    assert(!strcmp(snag_json_string(item, "call_id"), "call-2"));
    const char *second_id = snag_json_string(item, "id");
    assert(second_id);
    assert(snag_voice_result(voice, "call-1", "first result", error, sizeof(error)) == 0);
    item = json_object_get(voice_last(f.sent), "item");
    assert(!strcmp(snag_json_string(item, "call_id"), "call-1"));
    assert(strcmp(snag_json_string(item, "id"), second_id));
    assert(snag_voice_respond(voice, true, error, sizeof(error)) == 0);
    response = json_object_get(voice_last(f.sent), "response");
    assert(!strcmp(snag_json_string(response, "tool_choice"), "none"));
    assert(!strcmp(snag_json_string(json_object_get(response, "metadata"),
        "voice_input_item"), ""));
    voice_end(voice, &f);
}

static void test_voice_protocol(void)
{
    struct voice_fixture f;char error[256];struct snag_voice *voice=voice_start(&f);
    voice_commit(voice,"input-1",NULL);voice_commit(voice,"input-2","later words");
    size_t sent=json_array_size(f.sent);
    /* Audio can be answered before independent ASR completes. The later
     * transcript must not hold up speech or become input-1's coding request. */
    assert(snag_voice_respond(voice,true,error,sizeof(error))==0 && json_array_size(f.sent)==sent+1u);
    json_t *request=json_object_get(voice_last(f.sent),"response");
    assert(!strcmp(snag_json_string(json_object_get(request,"metadata"),"voice_input_item"),"input-1"));
    json_t *input=json_object_get(request,"input");
    assert(json_array_size(input)==1u && !strcmp(snag_json_string(json_array_get(input,0u),"id"),"input-1"));
    voice_created(voice,&f,"response-1");
    int16_t continuing_pcm[480]={0};
    assert(snag_voice_input(voice,continuing_pcm,480u,error,sizeof(error))==0);
    assert(!strcmp(snag_json_string(voice_last(f.sent),"type"),"input_audio_buffer.append"));
    assert(voice_deliver(voice,json_pack("{s:s,s:s}","type","response.function_call_arguments.done","response_id","response-1"))==0);
    assert(!voice_notice_count(&f,"voice_handoff"));
    assert(voice_deliver(voice,voice_call_response("response-1","completed"))==0);
    assert(!voice_notice_count(&f,"voice_handoff"));
    assert(voice_deliver(voice,voice_call_response("response-1","completed"))==0);
    assert(!voice_notice_count(&f,"voice_handoff"));
    assert(voice_deliver(voice,json_pack("{s:s,s:s,s:s,s:i}","type","conversation.item.input_audio_transcription.completed",
        "item_id","input-1","transcript","original words","content_index",0))==0);
    assert(voice_notice_count(&f,"voice_handoff")==1u);
    json_t *handoff=voice_last(f.notices);
    assert(!strcmp(snag_json_string(handoff,"transcript"),"original words"));
    assert(!strcmp(snag_json_string(handoff,"request"),"voice paraphrase"));
    assert(voice_deliver(voice,voice_call_response("response-1","completed"))==0 && voice_notice_count(&f,"voice_handoff")==1u);
    assert(snag_voice_respond(voice,true,error,sizeof(error))==0);
    request=json_object_get(voice_last(f.sent),"response");
    assert(!strcmp(snag_json_string(request,"tool_choice"),"auto"));
    assert(!strcmp(snag_json_string(json_object_get(request,"metadata"),"voice_input_item"),"input-2"));
    voice_created(voice,&f,"response-2");
    assert(snag_voice_result(voice,"call-1","coding still has one owner",error,sizeof(error))==0);
    assert(!strcmp(snag_json_string(json_object_get(voice_last(f.sent),"item"),"call_id"),"call-1"));
    assert(voice_deliver(voice,json_pack("{s:s,s:{s:s,s:s,s:[]}}","type","response.done","response",
        "id","response-2","status","completed","output"))==0);
    assert(snag_voice_respond(voice,true,error,sizeof(error))==0);
    assert(!strcmp(snag_json_string(json_object_get(voice_last(f.sent),"response"),"tool_choice"),"none"));
    voice_end(voice,&f);

    for(unsigned int mode=0;mode<3u;++mode) {
        voice=voice_start(&f);voice_commit(voice,"input-1","do the work");
        assert(snag_voice_respond(voice,true,error,sizeof(error))==0);voice_created(voice,&f,"response-1");
        if(mode) {
            unsigned char pcm[48];
            for(unsigned int i=0;i<24u;++i) {uint16_t v=(uint16_t)((int)i-12);pcm[2u*i]=(unsigned char)v;pcm[2u*i+1u]=(unsigned char)(v>>8u);}
            struct snag_buf encoded;snag_buf_init(&encoded,128u);
            assert(snag_base64_append(&encoded,pcm,sizeof(pcm))==0 && snag_buf_terminate(&encoded)==0);
            json_t *audio=json_pack("{s:s,s:s,s:s,s:i,s:s}","type","response.output_audio.delta","response_id","response-1",
                "item_id","audio-1","content_index",0,"delta",(char *)encoded.data);
            assert(voice_deliver(voice,json_incref(audio))==0 && f.frames==24u);
            f.played=100u;
            assert(voice_deliver(voice,json_pack("{s:s,s:s}","type","input_audio_buffer.speech_started","item_id","barge-in"))==0);
            assert(!strcmp(snag_json_string(voice_last(f.sent),"type"),"conversation.item.truncate"));
            assert(json_integer_value(json_object_get(voice_last(f.sent),"audio_end_ms"))==1);
            assert(voice_deliver(voice,audio)==0 && f.frames==24u);snag_buf_free(&encoded);
        }
        assert(voice_deliver(voice,voice_call_response("response-1",mode==2u?"completed":"cancelled"))==0);
        assert(!voice_notice_count(&f,"voice_handoff"));
        if(mode)assert(voice_notice_count(&f,"voice_interrupted")==1u);
        voice_end(voice,&f);
    }
    voice=voice_start(&f);voice_commit(voice,"input-1","earlier utterance");
    assert(voice_deliver(voice,json_pack("{s:s,s:s}","type","input_audio_buffer.speech_started","item_id","input-2"))==0);
    sent=json_array_size(f.sent);
    assert(snag_voice_respond(voice,true,error,sizeof(error))==0 && json_array_size(f.sent)==sent);
    assert(voice_deliver(voice,json_pack("{s:s,s:s}","type","input_audio_buffer.speech_stopped","item_id","input-2"))==0);
    assert(snag_voice_respond(voice,true,error,sizeof(error))==0 && json_array_size(f.sent)==sent+1u);
    voice_end(voice,&f);
    voice=voice_start(&f);int16_t pcm[480]={0};
    assert(snag_voice_mute(voice,true,error,sizeof(error))==0);
    sent=json_array_size(f.sent);
    assert(snag_voice_input(voice,pcm,480u,error,sizeof(error))==0 && json_array_size(f.sent)==sent);
    assert(snag_voice_mute(voice,false,error,sizeof(error))==0 && snag_voice_input(voice,pcm,480u,error,sizeof(error))==0);
    assert(json_array_size(f.sent)==sent+1u);
    assert(voice_deliver(voice,json_pack("{s:s,s:s}","type","response.output_audio.delta","response_id","unknown"))<0);
    assert(!snag_voice_ready(voice));voice_end(voice,&f);
    voice=voice_start(&f);
    for(unsigned int i=0;i<8u;++i) {char id[32];snprintf(id,sizeof(id),"input-%u",i);voice_commit(voice,id,NULL);}
    assert(voice_deliver(voice,json_pack("{s:s,s:s}","type","input_audio_buffer.committed","item_id","input-overflow"))<0);
    voice_end(voice,&f);
}

static void test_voice_async_asr(void)
{
    struct voice_fixture f;char error[256];
    /* Failed ASR can arrive on either side of response.done. Neither ordering
     * submits work; the pending remote call gets one explicit failure result. */
    for(unsigned int early=0;early<2u;++early) {
        struct snag_voice *voice=voice_start(&f);
        voice_commit(voice,"input-1",NULL);
        assert(snag_voice_respond(voice,true,error,sizeof(error))==0);
        voice_created(voice,&f,"response-1");
        json_t *failed=json_pack("{s:s,s:s,s:i}","type",
            "conversation.item.input_audio_transcription.failed","item_id","input-1","content_index",0);
        if(early)assert(voice_deliver(voice,json_incref(failed))==0);
        assert(voice_deliver(voice,voice_call_response("response-1","completed"))==0);
        if(!early)assert(voice_deliver(voice,json_incref(failed))==0);
        json_decref(failed);
        assert(!voice_notice_count(&f,"voice_handoff") && voice_notice_count(&f,"voice_asr_failed")==1u);
        json_t *item=json_object_get(voice_last(f.sent),"item");
        assert(!strcmp(snag_json_string(item,"call_id"),"call-1"));
        assert(strstr(snag_json_string(item,"output"),"no coding work was submitted"));
        size_t sent=json_array_size(f.sent);
        assert(voice_deliver(voice,voice_call_response("response-1","completed"))==0);
        assert(json_array_size(f.sent)==sent);
        voice_end(voice,&f);
    }
    struct snag_voice *voice=voice_start(&f);
    voice_commit(voice,"input-1",NULL);
    assert(snag_voice_respond(voice,true,error,sizeof(error))==0);
    voice_created(voice,&f,"response-1");
    /* Output audio is consumed before the separate transcript arrives; capture
     * can append on both sides of that output event without ending the input. */
    int16_t pcm[480]={0};unsigned char raw[48];
    for(unsigned int i=0;i<24u;++i) {uint16_t v=(uint16_t)((int)i-12);raw[i*2u]=(unsigned char)v;raw[i*2u+1u]=(unsigned char)(v>>8u);}
    struct snag_buf encoded;snag_buf_init(&encoded,128u);
    assert(snag_base64_append(&encoded,raw,sizeof(raw))==0 && snag_buf_terminate(&encoded)==0);
    assert(snag_voice_input(voice,pcm,480u,error,sizeof(error))==0);
    assert(voice_deliver(voice,json_pack("{s:s,s:s,s:s,s:i,s:s}","type","response.output_audio.delta",
        "response_id","response-1","item_id","audio-1","content_index",0,"delta",(char *)encoded.data))==0);
    assert(f.frames==24u && !voice_notice_count(&f,"voice_transcript"));
    assert(snag_voice_input(voice,pcm,480u,error,sizeof(error))==0);
    snag_buf_free(&encoded);
    json_t *audio_end=json_pack("{s:s,s:s,s:s,s:i}","type","response.output_audio.done",
        "response_id","response-1","item_id","audio-1","content_index",0);
    assert(voice_deliver(voice,json_incref(audio_end))==0 && f.ends==1u);
    assert(voice_deliver(voice,audio_end)==0 && f.ends==1u);
    assert(voice_deliver(voice,json_pack("{s:s,s:{s:s,s:s,s:[]}}","type","response.done","response",
        "id","response-1","status","completed","output"))==0);
    assert(f.ends==1u);
    /* Finished responses retain only unresolved input slots; late ASR releases
     * them. More than eight sequential late captions must not exhaust slots. */
    for(unsigned int i=0;i<12u;++i) {
        char input[32],response[32];snprintf(input,sizeof(input),"input-%u",i+2u);
        snprintf(response,sizeof(response),"response-%u",i+2u);
        voice_commit(voice,input,NULL);
        assert(snag_voice_respond(voice,true,error,sizeof(error))==0);
        voice_created(voice,&f,response);
        assert(voice_deliver(voice,json_pack("{s:s,s:{s:s,s:s,s:[]}}","type","response.done","response",
            "id",response,"status","completed","output"))==0);
        assert(voice_deliver(voice,json_pack("{s:s,s:s,s:s,s:i}","type",
            "conversation.item.input_audio_transcription.completed","item_id",input,"transcript","late words","content_index",0))==0);
    }
    assert(voice_deliver(voice,json_pack("{s:s,s:s,s:s,s:i}","type",
        "conversation.item.input_audio_transcription.completed","item_id","input-1","transcript","oldest words","content_index",0))==0);
    assert(voice_notice_count(&f,"voice_transcript")==13u && !voice_notice_count(&f,"voice_handoff"));
    voice_end(voice,&f);
}

static void test_voice_muted_input(void)
{
    struct voice_fixture f;char error[256];struct snag_voice *voice=voice_start(&f);
    assert(voice_deliver(voice,json_pack("{s:s,s:s}","type","input_audio_buffer.speech_started","item_id","muted-input"))==0);
    assert(snag_voice_mute(voice,true,error,sizeof(error))==0);
    assert(voice_notice_count(&f,"voice_asr_failed")==1u);
    assert(!strcmp(snag_json_string(voice_last(f.notices),"reason"),"muted_incomplete"));
    assert(snag_voice_mute(voice,false,error,sizeof(error))==0);
    assert(voice_deliver(voice,json_pack("{s:s,s:s}","type","input_audio_buffer.speech_stopped","item_id","muted-input"))==0);
    voice_commit(voice,"muted-input","truncated request must not execute");
    size_t sent=json_array_size(f.sent);
    assert(snag_voice_respond(voice,true,error,sizeof(error))==0 && json_array_size(f.sent)==sent);
    assert(!voice_notice_count(&f,"voice_handoff") && !voice_notice_count(&f,"voice_transcript"));
    for(unsigned int i=0;i<12u;++i) {
        char item[32];snprintf(item,sizeof(item),"cleared-%u",i);
        assert(voice_deliver(voice,json_pack("{s:s,s:s}","type","input_audio_buffer.speech_started","item_id",item))==0);
        assert(snag_voice_mute(voice,true,error,sizeof(error))==0);
        assert(voice_deliver(voice,json_pack("{s:s}","type","input_audio_buffer.cleared"))==0);
        assert(snag_voice_mute(voice,false,error,sizeof(error))==0);
    }
    sent=json_array_size(f.sent);
    voice_commit(voice,"fresh-input","new words");
    assert(snag_voice_respond(voice,true,error,sizeof(error))==0 && json_array_size(f.sent)==sent+1u);
    assert(voice_notice_count(&f,"voice_transcript")==1u);
    voice_end(voice,&f);
}

static void test_voice_context(void)
{
    struct voice_fixture f;char error[256];struct snag_voice *voice=voice_start(&f);
    size_t sent=json_array_size(f.sent);json_t *context=json_pack("{s:s,s:s}","kind","session_context","status","old state");
    assert(context && snag_voice_context(voice,context,error,sizeof(error))==0);
    assert(json_object_set_new(context,"status",json_string("completed earlier"))==0);
    assert(snag_voice_context(voice,context,error,sizeof(error))==0);json_decref(context);
    assert(snag_voice_respond(voice,true,error,sizeof(error))==0 && json_array_size(f.sent)==sent);
    assert(!voice_notice_count(&f,"voice_handoff"));
    voice_commit(voice,"input-1","what happened earlier?");
    assert(snag_voice_respond(voice,true,error,sizeof(error))==0);
    const json_t *response=json_object_get(voice_last(f.sent),"response"),*input=json_object_get(response,"input");
    assert(json_array_size(input)==2u);
    const json_t *item=json_array_get(input,0u);
    const char *text=snag_json_string(json_array_get(json_object_get(item,"content"),0u),"text");
    assert(!strcmp(snag_json_string(item,"type"),"message") && !snag_json_string(item,"call_id"));
    assert(text && strstr(text,"completed earlier") && strstr(text,"not a new request") && !strstr(text,"old state"));
    assert(!strcmp(snag_json_string(json_array_get(input,1u),"id"),"input-1"));
    voice_end(voice,&f);
}

static void test_voice_captions(void)
{
    struct voice_fixture f;char error[256];struct snag_voice *voice=voice_start(&f);
    assert(voice_deliver(voice,json_pack("{s:s,s:s}","type","input_audio_buffer.speech_started","item_id","input-1"))==0);
    assert(voice_deliver(voice,json_pack("{s:s,s:s,s:s,s:i}","type","conversation.item.input_audio_transcription.delta",
        "item_id","input-1","delta","still speaking","content_index",0))==0);
    assert(voice_notice_count(&f,"voice_caption")==1u && !voice_notice_count(&f,"voice_transcript"));
    assert(!voice_notice_count(&f,"voice_handoff"));
    assert(voice_deliver(voice,json_pack("{s:s,s:s}","type","input_audio_buffer.speech_stopped","item_id","input-1"))==0);
    voice_commit(voice,"input-1","final words");
    assert(snag_voice_respond(voice,true,error,sizeof(error))==0);voice_created(voice,&f,"response-1");
    json_t *caption=json_pack("{s:s,s:s,s:s,s:s,s:i}","type","response.output_audio_transcript.delta",
        "response_id","response-1","item_id","audio-1","delta","answer so far","content_index",0);
    assert(voice_deliver(voice,json_incref(caption))==0 && voice_notice_count(&f,"voice_caption")==2u);
    assert(!strcmp(snag_json_string(voice_last(f.notices),"speaker"),"assistant"));
    assert(!voice_notice_count(&f,"voice_interrupted"));
    assert(voice_deliver(voice,json_pack("{s:s,s:s}","type","input_audio_buffer.speech_started","item_id","input-2"))==0);
    assert(voice_notice_count(&f,"voice_interrupted")==1u);
    assert(!snag_json_string(voice_last(f.notices),"item_id"));
    assert(!strcmp(snag_json_string(voice_last(f.sent),"type"),"response.cancel"));
    assert(voice_deliver(voice,caption)==0 && voice_notice_count(&f,"voice_caption")==2u);
    assert(!voice_notice_count(&f,"voice_handoff"));
    voice_end(voice,&f);
}

static void
test_context_preview_retains_catalog(void)
{
    struct app_state app = {0};
    struct snag_config config;
    struct snag_model_capacity capacity;
    struct snag_context_choice choice = {SNAG_CONTEXT_MODE_DEFAULT, 0u};
    char error[256] = {0};

    snag_config_init(&config);
    snag_store_init(&app.store);
    app.config = &config;
    const struct snag_provider_config *provider = &config.providers[0];
    json_t *limits = json_pack("{s:n,s:n,s:n,s:n,s:n,s:i,s:n}",
        "auto_compact_input_tokens", "context_window_tokens", "effective_context_window_percent",
        "input_context_window_tokens", "max_context_window_tokens", "max_input_tokens", 258400,
        "max_output_tokens");
    assert(limits);
    app.model_cache.providers = json_pack("[{s:s,s:s,s:s,s:[{s:s,s:o,s:s,s:i}]}]",
        "name", provider->name, "base_url", provider->base_url,
        "protocol", snag_provider_catalog_protocol(provider), "models",
        "id", "catalog-test", "limits", limits, "count_capability", "unknown",
        "observed_hard_input_tokens", 0);
    assert(app.model_cache.providers);
    json_t *original = app.model_cache.providers;

    /* A preview must work without access to the on-disk catalog at all. */
    for (unsigned int i = 0u; i < 3u; ++i) {
        assert(snag_app_context_preview(&app, provider, "catalog-test", &choice,
            &capacity, error, sizeof(error)) == 0);
        assert(capacity.hard_input_known && capacity.hard_input_tokens == 258400u);
        assert(app.model_cache.providers == original);
    }
    snag_model_cache_free(&app.model_cache);
    snag_config_free(&config);
}

#if defined(__linux__) && !defined(_WIN32)
static pid_t native_ui_owner;

static void
native_ui_failed(void)
{
    /* This PID came directly from this fixture's owner fork. A stopped owner
     * cannot receive its alarm; terminate/reap it before an assertion exits. */
    assert(native_ui_owner > 0);
    (void)kill(native_ui_owner, SIGKILL);
    int status;
    pid_t done;
    do { done = waitpid(native_ui_owner, &status, 0); } while (done < 0 && errno == EINTR);
    assert(done == native_ui_owner);
    native_ui_owner = 0;
}

static void
native_ui_frame(int fd, enum snag_session_message type, const void *data, size_t length)
{
    struct snag_session_packet packet = {0};
    if (type == SNAG_SESSION_COMMIT) {
        struct snag_terminal_profile profile = {.term = "xterm"};
        assert(length == 4u && snag_session_commit_set(&packet, data, &profile) == 0);
    } else assert(snag_session_packet_set(&packet, type, data, length) == 0);
    uint64_t deadline = snag_monotonic_ms() + 5000u;
    int rc;
    do {
        rc = snag_session_packet_write(fd, &packet);
        bool okay = rc >= 0 && snag_monotonic_ms() < deadline;
        if (!okay) native_ui_failed();
        assert(okay);
        if (!rc) (void)snag_sleep_ms(1u);
    } while (!rc);
}

static void
native_ui_expect(int fd, enum snag_session_message expected, struct snag_buf *output)
{
    struct snag_session_packet packet = {0};
    uint64_t deadline = snag_monotonic_ms() + 5000u;
    for (;;) {
        int rc = snag_session_packet_read(fd, &packet);
        bool okay = rc >= 0 && snag_monotonic_ms() < deadline;
        if (!okay) native_ui_failed();
        assert(okay);
        if (!rc) { (void)snag_sleep_ms(1u); continue; }
        enum snag_session_message type = snag_session_packet_type(&packet);
        if (type != SNAG_SESSION_OUTPUT) {
            assert(type == expected);
            return;
        }
        if (output) assert(snag_buf_append(output, packet.bytes + SNAG_SESSION_HEADER,
                                           snag_session_packet_length(&packet)) == 0);
        size_t length = snag_session_packet_length(&packet);
        unsigned char offset[2] = {(unsigned char)length, (unsigned char)(length >> 8u)};
        native_ui_frame(fd, SNAG_SESSION_OUTPUT_ACK, offset, sizeof(offset));
        if (type == expected) return;
        packet = (struct snag_session_packet){0};
    }
}

static void
test_native_ui(void)
{
    char path[4096], directory[4096], id[33], error[256];
    const char *tmp = getenv("TMPDIR");
    assert(snprintf(path, sizeof(path), "%s/snajpagent-native-ui-XXXXXX", tmp ? tmp : "/tmp") > 0);
    assert(mkdtemp(path));
    int saved[3], report[2], proceed[2];
    assert(pipe(report) == 0 && pipe(proceed) == 0 && fflush(NULL) == 0);
    for (int fd = 0; fd < 3; ++fd) {
        saved[fd] = fcntl(fd, F_DUPFD_CLOEXEC, 3);
        assert(saved[fd] >= 0);
    }
    int outer = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    assert(outer >= 0 && grantpt(outer) == 0 && unlockpt(outer) == 0);
    int screen = open(ptsname(outer), O_RDWR | O_NOCTTY | O_CLOEXEC);
    assert(screen >= 0);
    for (int fd = 0; fd < 3; ++fd) assert(dup2(screen, fd) == fd);
    struct snag_session_process process;
    int frontend = snag_session_process_start(&process);
    if (!frontend) {
        (void)alarm(10u);
        for (int fd = 0; fd < 3; ++fd) assert(close(saved[fd]) == 0);
        assert(close(outer) == 0 && close(screen) == 0);
        assert(close(report[0]) == 0 && close(proceed[1]) == 0);
        struct app_state app = {0};
        struct snag_config config;
        snag_config_init(&config);
        app.config = &config;
        struct snag_store *store = &app.store;
        struct snag_session *session = &app.session;
        struct snag_ui *ui = &app.ui;
        snag_store_init(store);
        snag_session_init(session);
        assert(snag_store_open(store, path, error, sizeof(error)) == 0);
        assert(snag_session_create(store, session, path, "default", "fixture", "medium",
                                    error, sizeof(error)) == 0);
        assert(snag_ui_init(ui) == 0 && snag_ui_session_start(ui, &process) == 0);
        assert(snag_ui_session_attachment(ui) == 1u);
        assert(process.master == -1 && process.peer == -1 && process.slave == -1);
        /* Startup output can exceed the PTY buffer before input is opened. */
        char startup[65537];
        memset(startup, 's', sizeof(startup) - 1u);
        startup[sizeof(startup) - 1u] = '\0';
        assert(snag_ui_text(ui, SNAG_UI_HOST, startup) == 0);
        assert(snag_ui_open(ui, error, sizeof(error)) == 0);
        assert(snag_ui_session_listen(ui, session) == 0);
        assert(snag_ui_simple_prompt(ui, false) == 0);
        assert(snag_ui_insert_draft(ui, "retained draft") == 0);
        assert(snag_ui_voice(ui, "[VOICE MIC ON] ") == 0);
        json_t *notices = json_pack("[{s:s,s:s,s:s,s:s},{s:s}]",
            "type", "voice_transcript", "speaker", "user", "item_id", "input-1",
            "text", "final words", "type", "voice_handoff");
        assert(notices && snag_app_voice_fixture(&app, notices, false) == 0);
        json_decref(notices);
        assert(snag_app_voice_fixture_checkpoint(&app) == 0);
        assert(write(report[1], session->id, sizeof(id)) == (ssize_t)sizeof(id));
        char release;
        assert(read(proceed[0], &release, 1u) == 1);
        assert(snag_ui_text(ui, SNAG_UI_HOST, "work after disconnect") == 0);
        assert(snag_ui_session_attachment(ui) == 0u);
        char capture = (char)('0' + snag_ui_voice(ui, "[VOICE MIC ON] "));
        assert(write(report[1], &capture, 1u) == 1);
        assert(capture == '1');
        assert(snag_ui_audio(ui, "[MIC ON] ", true) == 1);
        assert(snag_ui_audio(ui, "[playing] ", false) == 1);
        assert(snag_app_voice_fixture_checkpoint(&app) == 2);
        assert(snag_app_voice_service(&app) == 0 && !app.voice);
        unsigned int voice_counts[2] = {0};
        assert(snag_session_each_event(session, voice_close_record, voice_counts,
                                        error, sizeof(error)) == 0);
        assert(voice_counts[0] == 1u && voice_counts[1] == 1u && !session->pending_queue_count);
        for (unsigned int playing = 0u; playing < 2u; ++playing) {
            assert(snag_app_audio_fixture(&app, playing != 0u) == 0);
            assert(snag_app_audio_fixture_checkpoint(&app) == 2);
            assert(snag_app_audio_service(&app) == 0 && !app.audio);
        }
        assert(write(report[1], "D", 1u) == 1);
        uint64_t generation, deadline = snag_monotonic_ms() + 5000u;
        while (!(generation = snag_ui_session_pending(ui))) {
            assert(snag_monotonic_ms() < deadline);
            (void)snag_sleep_ms(1u);
        }
        assert(snag_ui_session_attachment(ui) == 0u);
        assert(snag_ui_session_rebind(ui, generation) == 0);
        assert(snag_ui_session_attachment(ui) == generation);
        assert(!app.voice && !app.audio);
        assert(snag_app_audio_fixture(&app, false) == 0);
        assert(snag_app_audio_fixture_checkpoint(&app) == 0);
        assert(snag_ui_text(ui, SNAG_UI_HOST, "semantic catch-up") == 0);
        assert(write(report[1], "B", 1u) == 1);
        assert(read(proceed[0], &release, 1u) == 1);
        enum snag_term_action action = SNAG_TERM_NONE;
        char *text = NULL;
        assert(snag_ui_poll(ui, 0, &action, &text) == 0);
        assert(snag_ui_session_ready(ui, generation) == 0);
        deadline = snag_monotonic_ms() + 2000u;
        int rc;
        do {
            uint64_t resumed = snag_ui_session_pending(ui);
            if (resumed) {
                generation = resumed;
                assert(snag_ui_session_rebind(ui, generation) == 0);
                assert(snag_ui_session_ready(ui, generation) == 0);
            }
            rc = snag_ui_poll(ui, 20, &action, &text);
            assert(rc >= 0 && snag_monotonic_ms() < deadline);
            assert(snag_app_audio_service(&app) == 0);
        } while (!rc);
        assert(!app.audio && !app.voice);
        assert(action == SNAG_TERM_SUBMIT && text && !strcmp(text, "retained draftz"));
        free(text);
        assert(snag_ui_session_rebind(ui, generation - 1u) < 0 && errno == ESTALE);
        assert(snag_ui_text(ui, SNAG_UI_HOST, "owner-ok") == 0);
        unsigned char success = 0u;
        assert(snag_ui_session_control(ui, SNAG_SESSION_EXIT, &success, 1u) == 0);
        snag_ui_free(ui);
        snag_session_close(session);
        snag_store_close(store);
        snag_config_free(&config);
        assert(close(report[1]) == 0 && close(proceed[0]) == 0);
        _exit(0);
    }
    for (int fd = 0; fd < 3; ++fd) {
        assert(dup2(saved[fd], fd) == fd && close(saved[fd]) == 0);
    }
    assert(frontend == 1);
    native_ui_owner = (pid_t)process.child;
    assert(close(outer) == 0 && close(screen) == 0);
    assert(close(report[1]) == 0 && close(proceed[0]) == 0);
    /* Act as the initial frontend while startup output exceeds one frame. */
    for (;;) {
        struct pollfd ready[] = {{report[0], POLLIN, 0}, {process.peer, POLLIN, 0}};
        int rc = poll(ready, 2u, 5000);
        if (rc <= 0) native_ui_failed();
        assert(rc > 0);
        if (ready[0].revents) break;
        native_ui_expect(process.peer, SNAG_SESSION_OUTPUT, NULL);
    }
    ssize_t identified = read(report[0], id, sizeof(id));
    if (identified != (ssize_t)sizeof(id)) {
        int failed;
        assert(waitpid((pid_t)process.child, &failed, 0) == (pid_t)process.child);
    }
    assert(identified == (ssize_t)sizeof(id));
    snag_session_process_close(&process);
    assert(write(proceed[1], "d", 1u) == 1);
    char phase;
    assert(read(report[0], &phase, 1u) == 1);
    if (phase != '1') {
        fprintf(stderr, "detached voice activation returned %c, expected 1 (refused)\n", phase);
        native_ui_failed();
    }
    assert(phase == '1');
    assert(read(report[0], &phase, 1u) == 1 && phase == 'D');
    assert(snprintf(directory, sizeof(directory), "%s/sessions/%s", path, id) > 0);
    int dir = open(directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    assert(dir >= 0);
    int peer = snag_session_endpoint_connect(dir, directory);
    assert(peer >= 0 && close(dir) == 0);
    native_ui_frame(peer, SNAG_SESSION_RESERVE, NULL, 0u);
    native_ui_expect(peer, SNAG_SESSION_READY, NULL);
    unsigned char geometry[4] = {31u, 0u, 97u, 0u};
    native_ui_frame(peer, SNAG_SESSION_COMMIT, geometry, sizeof(geometry));
    native_ui_expect(peer, SNAG_SESSION_READY, NULL);
    assert(read(report[0], &phase, 1u) == 1 && phase == 'B');
    /* Ctrl-Z must suspend only the replaceable frontend. The following byte
     * was already admitted and must survive suspension and recommit. */
    native_ui_frame(peer, SNAG_SESSION_INPUT, "\032z", 2u);
    (void)snag_sleep_ms(40u);
    assert(write(proceed[1], "r", 1u) == 1);
    struct snag_buf output = {.max = 64u * 1024u};
    native_ui_expect(peer, SNAG_SESSION_SUSPEND, &output);
    geometry[0] = 33u;
    native_ui_frame(peer, SNAG_SESSION_COMMIT, geometry, sizeof(geometry));
    native_ui_expect(peer, SNAG_SESSION_READY, &output);
    native_ui_frame(peer, SNAG_SESSION_INPUT, "\r", 1u);
    native_ui_expect(peer, SNAG_SESSION_EXIT, &output);
    assert(strstr((const char *)output.data, "semantic catch-up"));
    assert(strstr((const char *)output.data, "owner-ok"));
    assert(!strstr((const char *)output.data, "work after disconnect"));
    snag_buf_free(&output);
    assert(close(peer) == 0 && close(report[0]) == 0 && close(proceed[1]) == 0);
    int status;
    assert(waitpid((pid_t)process.child, &status, 0) == (pid_t)process.child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    native_ui_owner = 0;
}
#endif /* __linux__ && !_WIN32 */

int
main(void)
{
#if defined(__linux__) && !defined(_WIN32)
    test_native_ui();
#endif
    test_context_preview_retains_catalog();
    /* Every fixture is a forked copy of this process and is stopped with
     * SIGTERM: install the handler before the first fork so all of them
     * inherit it, whatever path starts them, and exit with status 0. */
    (void)signal(SIGTERM, fixture_stop);
    test_audio_provider_selection();
    test_native_voice_protocol();
    test_native_voice_concurrency();
    test_native_voice_playback();
    test_native_voice_credential_snapshot();
    test_native_voice_transport();
#if SNAJPAGENT_AUDIO_DEVICE
    test_native_media();
#endif
    test_irc_steering_mode();
    test_plain_irc_pending_resume();
    test_irc_reply_restore_scope();
    test_irc_failed_intent_retains_pending();
#if SNAJPAGENT_AUDIO_DEVICE && defined(MA_NO_RUNTIME_LINKING) && defined(MA_ENABLE_ALSA)
    test_static_alsa_config();
#endif
    test_voice_protocol();
    test_voice_concurrent_results();
    test_voice_async_asr();
    test_voice_captions();
    test_voice_context();
    test_voice_muted_input();
#if SNAJPAGENT_AUDIO_DEVICE
    assert(snag_audio_fixture_capture()==0);
#endif
    test_voice_close();
    test_voice_transcript_labels();
    test_voice_concurrent_owner();
    test_voice_read_tools();
    test_voice_interface_read();
    test_voice_independent_request();
    test_voice_session_controls();
    test_voice_owner_mute();
    test_voice_output_tool();
    test_voice_socket();
    test_audio_transport();
    test_provider_auth();
    test_media_count_fallback();
    test_native_compaction_probe();
    test_local_audio_admission();
    test_ui_output_order_and_failure();
    test_ui_bounded_history();
    test_output_cache_failure();
    test_read_only_dispatch();
    test_goal_tool_manipulates_unfinished_goals();
    test_history_and_goal_list_tools();
    test_local_provider_transport();
    test_session_identity_header();
    test_openrouter_search_transport();
    test_codex_path_selection();
    test_structured_create_failures();
    test_typeless_create_diagnostic();
    test_create_retries();
    test_policy_clarification_after_reasoning();
    test_count_capability_statuses();
    test_count_modes();
    puts("test_provider_transport: ok");
    return 0;
}

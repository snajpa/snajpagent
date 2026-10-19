/* SPDX-License-Identifier: GPL-2.0-only */
#include "send.h"
#include "app.h"
#include "base.h"
#include "session_host.h"
#include "session_view.h"
#include "store.h"

#include <errno.h>
#if SNAJPAGENT_VM
#include <poll.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void
usage(void)
{
    (void)fprintf(stderr,
        "usage: snajpagent send [--dotdir DIR] SESSION_ID -- PROMPT...\n"
        "       snajpagent send [--dotdir DIR] SESSION_ID < prompt.txt\n");
}

#if SNAJPAGENT_VM
static int
read_prompt(struct snag_buf *prompt, int argc, char **argv)
{
    if (!argc) {
        if (snag_isatty(STDIN_FILENO) || snag_buf_read(prompt, STDIN_FILENO) < 0) return -1;
        if (prompt->len && prompt->data[prompt->len - 1u] == '\n') {
            --prompt->len;
            if (prompt->len && prompt->data[prompt->len - 1u] == '\r') --prompt->len;
        }
    } else {
        for (int i = 0; i < argc; ++i) {
            if ((i && snag_buf_putc(prompt, ' ') < 0) ||
                snag_buf_append(prompt, argv[i], strlen(argv[i])) < 0) return -1;
        }
    }
    if (!prompt->len || prompt->len > SNAG_MAX_DIRECT_PROMPT ||
        !snag_utf8_valid(prompt->data, prompt->len, true)) return snag_errno(EINVAL);
    return snag_buf_terminate(prompt);
}

static int
receipt(const json_t *value, const char *session, const char *id)
{
    const char *status = snag_json_string(value, "status");
    const char *event = snag_json_string(value, "event");
    if (status && !strcmp(status, "pending")) return -1;
    if (!status || !snag_string_in(status, "committed discarded rejected")) return 3;
    uint64_t seq = 0u;
    (void)snag_json_integer_u64(value, "seq", &seq);
    const char *outcome = !strcmp(status, "discarded") ? "discarded" :
        !strcmp(status, "committed") ?
            event && snag_string_in(event, "steering_added future_turn_queued") ?
                "queued" : "accepted" : "rejected";
    (void)printf("%s session=%s request=%s sequence=%llu event=%s\n", outcome, session, id,
        (unsigned long long)seq, event ? event : status);
    return !strcmp(status, "committed") || !strcmp(status, "discarded") ? 0 : 1;
}

static int
exchange(struct snag_view_channel *channel, const char *session, const char *id,
    const char *text, char *error, size_t error_size)
{
    json_t *hello = json_pack("{s:s,s:i}", "type", "hello", "version", 1);
    int rc = hello ? snag_view_channel_send(channel, hello) : -1;
    json_decref(hello);
    if (rc < 0) return 1;
    bool submitted = false;
    uint64_t deadline = snag_monotonic_ms() + 30000u;
    while (snag_monotonic_ms() < deadline) {
        if (snag_view_channel_write(channel) < 0) break;
        json_t *value = NULL;
        rc = snag_view_channel_read(channel, &value);
        if (rc < 0) break;
        if (rc > 0) {
            const char *type = snag_json_string(value, "type");
            if (!submitted && type && !strcmp(type, "capabilities")) {
                const json_t *features = json_object_get(value, "features");
                bool supported = false;
                for (size_t i = 0u; i < json_array_size(features); ++i) {
                    const char *feature = json_string_value(json_array_get(features, i));
                    if (feature && !strcmp(feature, "send")) supported = true;
                }
                const char *owner = snag_json_string(value, "session");
                if (!supported || !owner || strcmp(owner, session)) {
                    json_decref(value);
                    (void)snag_errorf(error, error_size,
                        "live owner does not support send; launch an updated owner");
                    return 1;
                }
                json_t *request = json_pack("{s:s,s:s,s:s}",
                    "type", "send", "id", id, "text", text);
                rc = request ? snag_view_channel_send(channel, request) : -1;
                json_decref(request);
                if (rc < 0) {
                    json_decref(value);
                    break;
                }
                submitted = true;
            } else if (type && !strcmp(type, "result")) {
                const char *request = snag_json_string(value, "id");
                if (submitted && request && !strcmp(request, id)) {
                    rc = receipt(value, session, id);
                    if (rc >= 0) {
                        if (rc) (void)snag_errorf(error, error_size, rc == 3 ?
                            "outcome unknown for request %s; do not automatically resend" :
                            "owner rejected request %s", id);
                        json_decref(value);
                        return rc;
                    }
                }
            } else if (type && !strcmp(type, "error")) {
                const char *message = snag_json_string(value, "message");
                (void)snag_errorf(error, error_size, "owner rejected request %s: %s",
                    id, message ? message : "protocol error");
                json_decref(value);
                return 1;
            }
            json_decref(value);
        }
        struct pollfd fd = {.fd = channel->fd,
            .events = POLLIN | (channel->output ? POLLOUT : 0)};
        uint64_t now = snag_monotonic_ms();
        if (now >= deadline) break;
        if (poll(&fd, 1u, (int)(deadline - now)) < 0 && errno != EINTR) break;
    }
    (void)snag_errorf(error, error_size, submitted ?
        "outcome unknown for request %s; no final receipt, do not automatically resend" :
        "owner handshake failed for request %s; prompt was not sent", id);
    return submitted ? 3 : 1;
}
#endif /* SNAJPAGENT_VM */

int
snag_send_main(int argc, char **argv)
{
    if (argc == 1 && (!strcmp(argv[0], "--help") || !strcmp(argv[0], "-h"))) {
        usage();
        return 0;
    }
#if SNAJPAGENT_VM
    const char *dotdir = NULL;
    int index = 0;
    if (index < argc && !strcmp(argv[index], "--dotdir")) {
        if (++index == argc) {
            usage();
            return 2;
        }
        dotdir = argv[index++];
    }
    if (index == argc || argv[index][0] == '-') {
        usage();
        return 2;
    }
    const char *prefix = argv[index++];
    if (index < argc) {
        if (strcmp(argv[index++], "--") || index == argc) {
            usage();
            return 2;
        }
    }
    struct snag_buf prompt = {.max = SNAG_MAX_DIRECT_PROMPT + 2u};
    if (read_prompt(&prompt, argc - index, argv + index) < 0) {
        snag_buf_free(&prompt);
        (void)fprintf(stderr, "send: prompt must be nonempty UTF-8, at most 1 MiB\n");
        return 2;
    }
    struct snag_store store;
    struct snag_session session;
    struct snag_view_channel channel;
    snag_store_init(&store);
    snag_session_init(&session);
    snag_view_channel_init(&channel, -1);
    char error[256] = {0};
    char id[SNAG_ID_HEX_LEN + 1u];
    int status = 1;
    char *path = snag_app_dotdir(dotdir, error, sizeof(error));
    if (!path || snag_store_open(&store, path, error, sizeof(error)) < 0 ||
        snag_session_locate(&store, &session, prefix, NULL, NULL, error, sizeof(error)) < 0)
        goto out;
    channel.fd = snag_session_view_connect(session.dir_fd, session.dir_path);
    if (channel.fd < 0 || snag_random_id(id) < 0) goto out;
    status = exchange(&channel, session.id, id, (const char *)prompt.data, error, sizeof(error));
out:
    if (status) (void)fprintf(stderr, "send: %s\n", error[0] ? error : strerror(errno));
    snag_view_channel_close(&channel);
    snag_session_close(&session);
    snag_store_close(&store);
    snag_buf_free(&prompt);
    free(path);
    return status;
#else
    (void)argv;
    (void)fprintf(stderr, "send requires semantic session support in this build\n");
    return 1;
#endif /* SNAJPAGENT_VM */
}

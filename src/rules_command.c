/* SPDX-License-Identifier: GPL-2.0-only */
#include "rules_command.h"
#include "base.h"
#include "process_host.h"
#include "tools.h"
#include "wake.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>

#define RULE_HELPER_IN_MAX (4u * 1024u * 1024u)
#define RULE_HELPER_OUT_MAX (64u * 1024u)
#define RULE_HELPER_POLL_MS 50

int
snag_rule_command_run(const struct snag_config *config, const char *command,
                      const char *workdir, unsigned int timeout_ms,
                      const char *envelope, size_t envelope_len,
                      json_t **reply, char *error, size_t error_size)
{
    struct snag_child child;
    char **env = NULL;
    struct snag_buf out;
    uint64_t deadline;
    size_t written = 0u, stderr_bytes = 0u;
    bool stdout_open = true, stderr_open = true, ok = false;

    if (!reply)
        return snag_fail(error, error_size, EINVAL, "invalid rule helper destination");
    *reply = NULL;
    if (!config || !config->shell || !command || !*command || !workdir || !envelope ||
        envelope_len > RULE_HELPER_IN_MAX || timeout_ms < 1u || timeout_ms > 60000u)
        return snag_fail(error, error_size, EINVAL, "invalid rule helper request");
    snag_child_init(&child);
    env = snag_tools_environment(config);
    if (!env || snag_child_spawn(&child, config->shell, command, workdir, env, false) < 0) {
        snag_environment_entries_free(env);
        return snag_errorf(error, error_size, "rule helper could not be started");
    }
    snag_environment_entries_free(env);
    snag_buf_init(&out, RULE_HELPER_OUT_MAX + 1u);
    deadline = snag_monotonic_ms() + timeout_ms;
    while (stdout_open || stderr_open || !child.reaped) {
        struct snag_child_event events[3];
        size_t count = written < envelope_len ? 3u : 2u;
        int64_t remaining = (int64_t)deadline - (int64_t)snag_monotonic_ms();
        int ready;

        if (remaining <= 0) {
            snag_errorf(error, error_size, "rule helper timed out");
            goto out;
        }
        events[0] = (struct snag_child_event){&child, 0u, stdout_open ? SNAG_CHILD_READ : 0u, 0u};
        events[1] = (struct snag_child_event){&child, 1u, stderr_open ? SNAG_CHILD_READ : 0u, 0u};
        if (count == 3u)
            events[2] = (struct snag_child_event){&child, 2u, SNAG_CHILD_WRITE, 0u};
        ready = snag_child_wait(events, count, SNAG_WAKE_INVALID,
                                remaining > RULE_HELPER_POLL_MS ? RULE_HELPER_POLL_MS : (int)remaining);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            snag_errorf(error, error_size, "rule helper wait failed");
            goto out;
        }
        if (count == 3u && (events[2].revents & (SNAG_CHILD_WRITE | SNAG_CHILD_END))) {
            while (written < envelope_len) {
                ssize_t n = snag_child_write(&child, envelope + written, envelope_len - written);
                if (n > 0) {
                    written += (size_t)n;
                    continue;
                }
                if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
                    break;
                /* A helper that closed its input early is still judged by its exit
                 * status and stdout; stop writing and keep draining output. */
                written = envelope_len;
                break;
            }
            if (written >= envelope_len) {
                snag_child_close_stream(&child, 2u);
                if (count == 3u)
                    count = 2u;
            }
        }
        for (unsigned int stream = 0u; stream < 2u; ++stream) {
            unsigned char chunk[4096];
            bool *open = stream == 0u ? &stdout_open : &stderr_open;
            if (!*open)
                continue;
            if (!(events[stream].revents & SNAG_CHILD_READ))
                goto stream_end;
            for (;;) {
                ssize_t got = snag_child_read(&child, stream, chunk, sizeof(chunk));
                if (got > 0) {
                    if (stream == 0u) {
                        if (out.len + (size_t)got > RULE_HELPER_OUT_MAX) {
                            snag_errorf(error, error_size, "rule helper stdout exceeded 64 KiB");
                            goto out;
                        }
                        if (snag_buf_append(&out, chunk, (size_t)got) < 0)
                            goto out;
                    } else if ((stderr_bytes += (size_t)got) > RULE_HELPER_OUT_MAX) {
                        snag_errorf(error, error_size, "rule helper stderr exceeded 64 KiB");
                        goto out;
                    }
                    continue;
                }
                if (got < 0 && errno == EINTR)
                    continue;
                if (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    snag_errorf(error, error_size, "rule helper output read failed");
                    goto out;
                }
                break;
            }
stream_end:
            if (events[stream].revents & (SNAG_CHILD_END | SNAG_CHILD_ERROR)) {
                snag_child_close_stream(&child, stream);
                *open = false;
            }
        }
        {
            int exited = snag_child_exited(&child);
            if (exited < 0) {
                snag_errorf(error, error_size, "rule helper state is unknown");
                goto out;
            }
            if (exited && !child.reaped) {
                if (snag_child_reap(&child) < 0 && errno != ECHILD) {
                    snag_errorf(error, error_size, "rule helper could not be reaped");
                    goto out;
                }
            }
            if (child.reaped && !stdout_open && !stderr_open)
                break;
        }
    }
    if (!child.reaped && snag_child_reap(&child) < 0 && errno != ECHILD) {
        snag_errorf(error, error_size, "rule helper could not be reaped");
        goto out;
    }
    if (written < envelope_len) {
        snag_errorf(error, error_size, "rule helper input was not accepted");
        goto out;
    }
    if (child.signal_number > 0 || child.exit_code != 0) {
        snag_errorf(error, error_size, "rule helper failed");
        goto out;
    }
    if (out.len == 0u) {
        ok = true; /* Empty successful stdout means pass. */
    } else {
        json_t *parsed = snag_json_load_strict(out.data, out.len, 1u << 20, error, error_size);
        if (!parsed || !json_is_object(parsed)) {
            json_decref(parsed);
            snag_errorf(error, error_size, "rule helper returned invalid JSON");
            goto out;
        }
        *reply = parsed;
        ok = true;
    }
out:
    if (!ok) {
        if (!error[0])
            snag_errorf(error, error_size, "rule helper failed");
        snag_child_signal(&child, SNAG_CHILD_KILL);
        if (!child.reaped)
            (void)snag_child_reap(&child);
    }
    snag_child_free(&child);
    snag_buf_free(&out);
    return ok ? 0 : -1;
}

/* SPDX-License-Identifier: GPL-2.0-only */
#include "tools.h"
#include "fs.h"
#include "process_host.h"

#include "tools_patch.h"

#include "base.h"
#include "json.h"
#include "secret.h"

#include <errno.h>
#include <fcntl.h>
#include "snag_jansson.h"
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

#define SNAG_TOOL_COMMAND_MAX (256u * 1024u)
#define SNAG_TOOL_STDIN_MAX (1024u * 1024u)
#define SNAG_TOOL_POLL_MS 50u
#define SNAG_TOOL_YIELD_MAX_MS 600000u
#define SNAG_TOOL_CLOSE_GRACE_MS 2000u
#define SNAG_TOOL_DRAIN_GRACE_MS 2000u
#define SNAG_TOOL_REDACTOR_MAX (8192u + SNAG_WIRE_SECRET_MAX)

struct output_excerpt {
    struct snag_buf data;
    uint64_t bytes;
};

struct process_output {
    bool open;
    struct snag_buf data, pending;
};

struct managed_process {
    char handle[SNAG_ID_HEX_LEN + 1u];
    struct snag_child child;
    bool stdin_open;
    bool child_done;
    bool output_incomplete;
    uint64_t drain_deadline_ms;
    bool closing;
    bool cancelled;
    uint64_t started_ms;
    uint64_t deadline_ms;
    uint64_t wait_started_ms;
    uint32_t max_wait_ms;
    uint32_t max_output_tokens;
    struct snag_secret_set secrets;
    size_t max_secret;
    struct process_output output[2];
    struct snag_buf input;
    size_t input_written;
    uint64_t input_accepted_total, input_written_total;
    bool input_eof, pty_eof_sent, in_call;
    uint64_t output_offset[2], collected_offset[2], result_offset[2];
    const char *handoff;
};

static struct managed_process *processes[SNAG_MAX_PROCESSES];
static snag_tool_output_fn journal_write;
static snag_tool_read_fn journal_read;
static void *journal_opaque;
static size_t next_fd;
static bool managed_cleanup_registered;
static int flush_capture(struct managed_process *, unsigned int);

static bool
json_bool_member(const json_t *object, const char *key, bool default_value,
                 bool *out)
{
    json_t *value = json_object_get(object, key);
    if (!value || json_is_null(value)) {
        *out = default_value;
        return true;
    }
    if (json_is_true(value)) {
        *out = true;
        return true;
    }
    if (json_is_false(value)) {
        *out = false;
        return true;
    }
    return false;
}

static bool
json_u32_member(const json_t *object, const char *key, uint32_t default_value,
                uint32_t min, uint32_t max, uint32_t *out)
{
    json_t *value = json_object_get(object, key);
    json_int_t n;

    if (!value || json_is_null(value)) {
        *out = default_value;
        return true;
    }
    if (!json_is_integer(value))
        return false;
    n = json_integer_value(value);
    if (n < 0 || (uint64_t)n < min || (uint64_t)n > max)
        return false;
    *out = (uint32_t)n;
    return true;
}

static const char *
json_nullable_string(const json_t *object, const char *key)
{
    json_t *value = json_object_get(object, key);
    if (!value || json_is_null(value))
        return NULL;
    return json_is_string(value) ? json_string_value(value) : (const char *)-1;
}

static bool
command_output_limit(const json_t *arguments, uint32_t ceiling, uint32_t *out)
{
    if (!json_u32_member(arguments, "max_output_tokens", ceiling, 1u,
                         (uint32_t)SNAG_CONFIG_TOKEN_LIMIT_MAX, out))
        return false;
    if (*out > ceiling)
        *out = ceiling;
    return true;
}

static bool
text_arg_valid(const char *text, size_t max)
{
    size_t len;
    if (!text || text == (const char *)-1)
        return false;
    len = strlen(text);
    return len <= max && snag_utf8_valid((const unsigned char *)text, len, true);
}

static bool
absolute_dir_arg_valid(const char *path)
{
    snag_file_info st;
    size_t len;

    if (!path || path == (const char *)-1 || !snag_path_root_len(path))
        return false;
    len = strlen(path);
    return len <= SNAG_PATH_MAX_BYTES &&
           snag_utf8_valid((const unsigned char *)path, len, true) &&
           snag_stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int
output_append(struct managed_process *proc, unsigned int stream,
              const unsigned char *text, size_t len)
{
    struct snag_buf *data = &proc->output[stream].data;

    if (len > data->max - data->len && flush_capture(proc, stream) < 0)
        return -1;
    return snag_buf_append(data, text, len);
}

static int
redact_output(struct managed_process *proc, unsigned int stream, bool final)
{
    static const unsigned char marker[] = "<redacted:secret>";
    struct snag_buf *pending = &proc->output[stream].pending;
    size_t limit = pending->len, off = 0u;

    if (!final && proc->max_secret) {
        size_t suffix = proc->max_secret - 1u;
        limit = limit > suffix ? limit - suffix : 0u;
    }
    if (!proc->max_secret) {
        if (limit && output_append(proc, stream, pending->data, limit) < 0)
            return -1;
        off = limit;
    } else {
        while (off < limit) {
            size_t matched = snag_wire_secret_match(pending->data + off,
                                pending->len - off, &proc->secrets.wire);
            if (output_append(proc, stream, matched ? marker : pending->data + off,
                               matched ? sizeof(marker) - 1u : 1u) < 0)
                return -1;
            off += matched ? matched : 1u;
        }
    }
    if (off) {
        memmove(pending->data, pending->data + off, pending->len - off);
        pending->len -= off;
    }
    return 0;
}

static json_t *
excerpt_json(const struct output_excerpt *stream)
{
    const struct snag_buf *data = &stream->data;
    bool textual = snag_utf8_valid(data->data, data->len, true);
    json_t *out = NULL;

    struct snag_buf encoded = {.max = SIZE_MAX};
    if (!textual) {
        if (snag_base64_append(&encoded, data->data, data->len) < 0)
            goto done;
        data = &encoded;
    }
    out = json_pack("{s:I,s:s,s:I,s:s%,s:I}",
        "discarded_bytes", (json_int_t)(stream->bytes - stream->data.len), "encoding", textual ? "utf8" : "base64",
        "original_bytes", (json_int_t)stream->bytes,
        "retained", data->len ? (const char *)data->data : "", data->len,
        "retained_bytes", (json_int_t)stream->data.len);
done:
    snag_buf_free(&encoded);
    return out;
}

static int
append_stream_text(struct snag_buf *out, const char *label,
                   const struct output_excerpt *stream)
{
    if (!stream->data.len)
        return 0;
    if (snag_buf_printf(out, "%s%s:\n", out->len ? "\n" : "", label) < 0)
        return -1;
    if (snag_utf8_valid(stream->data.data, stream->data.len, true)) {
        if (snag_buf_append(out, stream->data.data, stream->data.len) < 0)
            return -1;
        if (stream->data.data[stream->data.len - 1u] != '\n' &&
            snag_buf_putc(out, '\n') < 0)
            return -1;
    } else {
        if (snag_buf_printf(out, "<%llu binary bytes; base64 follows>\n",
                           (unsigned long long)stream->data.len) < 0 ||
            snag_base64_append(out, stream->data.data, stream->data.len) < 0 ||
            snag_buf_putc(out, '\n') < 0)
            return -1;
    }
    return 0;
}

static char *
model_text_for(const char *status, const char *reason, int64_t exit_code,
               int signal_number, const struct managed_process *proc, uint64_t wait_ms,
               const struct output_excerpt *stdout_stream,
               const struct output_excerpt *stderr_stream)
{
    char *out = NULL;

    struct snag_buf text = {.max = SIZE_MAX};
    if (strcmp(status, "succeeded") == 0 || strcmp(status, "failed") == 0) {
        if (snag_buf_printf(&text, "Process exited with code %lld.\n", (long long)exit_code) < 0)
            goto done;
    } else if (strcmp(status, "signaled") == 0) {
        if (snag_buf_printf(&text, "Process was terminated by signal %d.\n",
                           signal_number) < 0)
            goto done;
    } else if (strcmp(status, "cancelled") == 0) {
        const char *msg = "Process was cancelled by the user.\n";
        if (snag_buf_append(&text, msg, strlen(msg)) < 0)
            goto done;
    } else if (strcmp(status, "running") == 0) {
        const char *msg;

        if (reason && strcmp(reason, "wait_timeout") == 0) {
            if (snag_buf_printf(&text,
                "Tool wait limit reached after %llu ms (max_wait_ms=%u). ",
                (unsigned long long)wait_ms, proc->max_wait_ms) < 0)
                goto done;
            msg = "Control returned to the model; the process remains owned by this session. Evaluate its output and state, then use the same handle to wait, interact, or request termination. Do not restart the command merely because this wait expired.\n";
        } else if (reason && strcmp(reason, "operator_yield") == 0)
            msg = "The operator requested /yield: control returned to the model while the process remains owned by this session. Evaluate its output and state before deciding what to do next. Use the same handle to wait, interact, or request termination; /yield sends no signal.\n";
        else if (reason && strcmp(reason, "timeout_handoff") == 0)
            msg = "Command timeout elapsed; the process continues in the background. Use write_stdin with the active handle to wait for, interact with, or terminate it.\n";
        else if (reason && strcmp(reason, "steering_handoff") == 0)
            msg = "Command is still running because steering arrived. Use write_stdin with the active handle to wait for, interact with, or terminate it after considering the steer.\n";
        else
            msg = "Process is still running.\n";
        if (snag_buf_append(&text, msg, strlen(msg)) < 0)
            goto done;
        if (proc->closing) {
            const char *pending = "Termination was already requested; process exit or output drain is still pending. The live handle remains valid.\n";
            if (snag_buf_append(&text, pending, strlen(pending)) < 0)
                goto done;
        }
    } else if (strcmp(status, "io_failed") == 0) {
        const char *msg = "Tool I/O failed.\n";
        if (snag_buf_append(&text, msg, strlen(msg)) < 0)
            goto done;
    } else if (snag_buf_printf(&text, "Tool status: %s.\n", status) < 0) {
        goto done;
    }
    if (proc->output_incomplete) {
        const char *warning = "Post-exit output drain reached its 2000 ms limit; output may be incomplete. The command has exited; remaining capture streams were closed without signalling unrelated descriptor owners.\n";
        if (snag_buf_append(&text, warning, strlen(warning)) < 0)
            goto done;
    }
    if (append_stream_text(&text, "stdout", stdout_stream) < 0 ||
        append_stream_text(&text, "stderr", stderr_stream) < 0)
        goto done;
    if (snag_buf_terminate(&text) < 0)
        goto done;
    out = (char *)text.data;
    memset(&text, 0, sizeof(text));
done:
    snag_buf_free(&text);
    return out;
}

static json_t *
result_json(const char *status, const char *reason, int64_t exit_code,
            int signal_number, uint64_t duration_ms, const char *handle,
            const struct managed_process *proc,
            const struct output_excerpt *stdout_stream,
            const struct output_excerpt *stderr_stream)
{
    uint64_t wait_ms = snag_monotonic_ms() - proc->wait_started_ms;
    char *model_text = model_text_for(status, reason, exit_code, signal_number, proc, wait_ms,
                                     stdout_stream, stderr_stream);
    json_t *stdout_json = excerpt_json(stdout_stream);
    json_t *stderr_json = excerpt_json(stderr_stream);
    json_t *out = json_pack("{s:I,s:n,s:s?,s:s,s:s?,s:n,s:s,s:O,s:O}",
        "duration_ms", (json_int_t)duration_ms, "exit_code", "handle", handle,
        "model_text", model_text, "reason", reason, "signal", "status", status,
        "stderr", stderr_json, "stdout", stdout_json);

    if (out &&
        ((exit_code >= 0 &&
          snag_json_set_new(out, "exit_code", json_integer(exit_code)) < 0) ||
         (signal_number > 0 &&
          snag_json_set_new(out, "signal", json_integer(signal_number)) < 0))) {
        json_decref(out);
        out = NULL;
    }
    free(model_text);
    json_decref(stdout_json);
    json_decref(stderr_json);
    return out;
}

static bool
env_name_matches(const char *entry, const char *name)
{
    size_t len = strlen(name);
    return snag_environment_prefix(entry, name) && entry[len] == '=';
}

static bool
proxy_with_userinfo(const char *entry)
{
    const char *eq = strchr(entry, '=');
    const char *scheme;
    const char *at;
    const char *slash;

    if (!eq)
        return false;
    scheme = strstr(eq + 1u, "://");
    if (!scheme)
        return false;
    at = strchr(scheme + 3u, '@');
    slash = strchr(scheme + 3u, '/');
    return at && (!slash || at < slash);
}

static bool
remove_env_entry(const char *entry, const struct snag_config *config)
{
    static const char *const proxy_names[] = {
        "HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY",
        "http_proxy", "https_proxy", "all_proxy"
    };

    if (env_name_matches(entry, "OPENAI_API_KEY"))
        return true;
    for (size_t i = 0; i < config->provider_count; ++i)
        if (config->providers[i].api_key.kind == SNAG_SECRET_ENV &&
            env_name_matches(entry, config->providers[i].api_key.value))
            return true;
    for (size_t i = 0; i < config->secret_count; ++i)
        if (config->secrets[i].kind == SNAG_SECRET_ENV &&
            env_name_matches(entry, config->secrets[i].value))
            return true;
    for (size_t i = 0; i < sizeof(proxy_names) / sizeof(proxy_names[0]); ++i)
        if (env_name_matches(entry, proxy_names[i]) && proxy_with_userinfo(entry))
            return true;
    if (snag_environment_prefix(entry, "SNAJPAGENT_"))
        return true;
    return false;
}

static char **
filtered_environment(const struct snag_config *config)
{
    size_t kept = 0;
    char **env = snag_environment_entries();
    if (!env)
        return NULL;
    for (size_t i = 0; env[i]; ++i)
        if (!remove_env_entry(env[i], config))
            env[kept++] = env[i];
        else {
            size_t size = strlen(env[i]);
            volatile char *p = env[i];
            for (size_t j = 0; j < size; ++j)
                p[j] = 0;
            free(env[i]);
        }
    env[kept] = NULL;
    return env;
}

static void
write_stdin_chunk(struct managed_process *proc, const char *data, size_t len, size_t *written,
                  bool *open_flag)
{
    while (*written < len) {
        ssize_t n = snag_child_write(&proc->child, data + *written, len - *written);
        if (n > 0) {
            *written += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;
        *open_flag = false;
        return;
    }
}

static uint64_t
saturating_deadline(uint64_t start, uint32_t delta_ms)
{
    if (!delta_ms)
        return UINT64_MAX;
    uint64_t deadline = start + delta_ms;
    return deadline < start ? UINT64_MAX : deadline;
}

static struct managed_process *
find_process(const char *handle)
{
    for (size_t i = 0u; i < SNAG_MAX_PROCESSES; ++i)
        if (processes[i] && handle && !strcmp(processes[i]->handle, handle))
            return processes[i];
    return NULL;
}

void
snag_tools_journal(snag_tool_output_fn write, snag_tool_read_fn read, void *opaque)
{
    journal_write = write;
    journal_read = read;
    journal_opaque = opaque;
}

static void
managed_close_input(struct managed_process *proc)
{
    snag_child_close_stream(&proc->child, 2u);
    proc->stdin_open = false;
}

static void
managed_release(struct managed_process *proc)
{
    if (!proc)
        return;
    snag_child_free(&proc->child);
    for (unsigned int s = 0u; s < 2u; ++s) {
        snag_buf_free(&proc->output[s].pending);
        snag_buf_free(&proc->output[s].data);
    }
    snag_buf_free(&proc->input);
    for (size_t i = 0u; i < SNAG_MAX_PROCESSES; ++i)
        if (processes[i] == proc)
            processes[i] = NULL;
    snag_secret_set_free(&proc->secrets);
    free(proc);
}

static void
managed_cleanup_at_exit(void)
{
    for (size_t i = 0u; i < SNAG_MAX_PROCESSES; ++i)
        managed_release(processes[i]);
}

void
snag_tools_shutdown(void)
{
    managed_cleanup_at_exit();
}

static void
managed_register_cleanup(void)
{
    if (!managed_cleanup_registered && atexit(managed_cleanup_at_exit) == 0)
        managed_cleanup_registered = true;
}

static bool
process_ready(const struct managed_process *proc)
{
    return proc->child_done && !proc->output[0].open && !proc->output[1].open;
}

bool
snag_tools_ready(const char *handle)
{
    struct managed_process *proc = find_process(handle);
    return proc && process_ready(proc);
}

bool
snag_tools_busy(void)
{
    for (size_t i = 0u; i < SNAG_MAX_PROCESSES; ++i)
        if (processes[i] && !process_ready(processes[i]))
            return true;
    return false;
}

const char *
snag_tools_handoff(const char *handle)
{
    struct managed_process *proc = find_process(handle);
    return proc ? proc->handoff : NULL;
}

void
snag_tools_process_state(struct snag_process_state *state)
{
    struct managed_process *proc = find_process(state->handle);
    if (!proc)
        return;
    state->ready = process_ready(proc);
    state->draining = proc->child_done && !state->ready;
    state->input_accepted = proc->input_accepted_total;
    state->input_written = proc->input_written_total;
    state->input_pending = proc->input.len - proc->input_written;
}

static void
begin_close(struct managed_process *proc, bool user_interrupt)
{
    if (proc->closing || process_ready(proc))
        return;
    snag_buf_reset(&proc->input);
    proc->input_written = 0u;
    managed_close_input(proc);
    proc->closing = true;
    proc->cancelled = user_interrupt;
    snag_child_signal(&proc->child, user_interrupt ? SNAG_CHILD_INTERRUPT : SNAG_CHILD_TERMINATE);
    proc->deadline_ms = saturating_deadline(snag_monotonic_ms(),
                                            SNAG_TOOL_CLOSE_GRACE_MS);
}

void
snag_tools_close_all(bool user_interrupt)
{
    for (size_t i = 0u; i < SNAG_MAX_PROCESSES; ++i)
        if (processes[i])
            begin_close(processes[i], user_interrupt);
}

static int
flush_capture(struct managed_process *proc, unsigned int stream)
{
    struct snag_buf *data = &proc->output[stream].data;
    size_t consumed = 0u;
    while (consumed < data->len) {
        size_t n = data->len - consumed;
        bool open = proc->output[stream].open;
        if (n > 16384u)
            n = 16384u;
        /* Keep an incomplete UTF-8 suffix for the next read; binary data is
         * encoded losslessly by the journal callback. */
        size_t full = n;
        for (size_t tail = 1u; tail <= 3u && tail <= full; ++tail) {
            unsigned char c = data->data[consumed + full - tail];
            size_t width = c >= 0xc2u && c <= 0xdfu ? 2u :
                           c >= 0xe0u && c <= 0xefu ? 3u :
                           c >= 0xf0u && c <= 0xf4u ? 4u : 0u;
            if (width > tail && (open || consumed + full < data->len) &&
                snag_utf8_valid(data->data + consumed, full - tail, true)) {
                n = full - tail;
                break;
            }
        }
        if (!n)
            break;
        if (!journal_write ||
            journal_write(journal_opaque, proc->handle, stream,
                          proc->output_offset[stream], data->data + consumed, n) < 0)
            return -1;
        proc->output_offset[stream] += n;
        consumed += n;
    }
    if (data->len > consumed)
        memmove(data->data, data->data + consumed, data->len - consumed);
    data->len -= consumed;
    return 0;
}

static int
close_output(struct managed_process *proc, unsigned int stream)
{
    struct process_output *output = &proc->output[stream];
    if (redact_output(proc, stream, true) < 0)
        return -1;
    output->open = false;
    snag_child_close_stream(&proc->child, stream);
    if (proc->child.pty)
        proc->stdin_open = false;
    return flush_capture(proc, stream);
}

static int
process_read(struct managed_process *proc, unsigned int stream)
{
    struct process_output *output = &proc->output[stream];
    unsigned char bytes[4096];
    ssize_t n = snag_child_read(&proc->child, stream, bytes, sizeof(bytes));
    if (n > 0) {
        if (snag_buf_append(&output->pending, bytes, (size_t)n) < 0 ||
            redact_output(proc, stream, false) < 0)
            return -1;
    } else if (n == 0) {
        return close_output(proc, stream);
    } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
        return -1;
    }
    return flush_capture(proc, stream);
}

static void
process_write(struct managed_process *proc)
{
    size_t before = proc->input_written;
    size_t end = proc->input.len;
    if (end - before > 4096u)
        end = before + 4096u;
    if (before < end)
        write_stdin_chunk(proc, (const char *)proc->input.data, end,
                           &proc->input_written, &proc->stdin_open);
    proc->input_written_total += proc->input_written - before;
    if (proc->input_written == proc->input.len) {
        snag_buf_reset(&proc->input);
        proc->input_written = 0u;
        if (proc->input_eof && proc->stdin_open) {
            if (proc->child.pty && !proc->pty_eof_sent) {
                size_t written = 0u;
                write_stdin_chunk(proc, "\004", 1u, &written,
                                   &proc->stdin_open);
                proc->pty_eof_sent = written == 1u;
                if (!proc->pty_eof_sent)
                    return;
            }
            managed_close_input(proc);
        }
    }
}

int
snag_tools_service(int timeout_ms, snag_wake_fd wake_fd, char *error, size_t error_size)
{
    struct snag_child_event fds[SNAG_MAX_PROCESSES * 3u];
    struct { struct managed_process *proc; unsigned int stream; } map[SNAG_MAX_PROCESSES * 3u + 1u];
    size_t count = 0u;
    uint64_t now = snag_monotonic_ms();
    int rc;
    if (timeout_ms > (int)SNAG_TOOL_POLL_MS)
        timeout_ms = (int)SNAG_TOOL_POLL_MS;
    for (size_t i = 0u; i < SNAG_MAX_PROCESSES; ++i) {
        struct managed_process *proc = processes[i];
        if (!proc)
            continue;
        if (!proc->child_done) {
            int exited = snag_child_exited(&proc->child);
            if (exited < 0) {
                if (errno != EINTR)
                    goto fail;
            } else if (exited) {
                proc->child_done = true;
                proc->drain_deadline_ms = saturating_deadline(now, SNAG_TOOL_DRAIN_GRACE_MS);
                managed_close_input(proc);
            }
        }
        if (!process_ready(proc) && now >= proc->deadline_ms) {
            proc->deadline_ms = UINT64_MAX;
            if (proc->closing) {
                snag_child_signal(&proc->child, SNAG_CHILD_KILL);
            } else {
                proc->handoff = "timeout_handoff";
            }
        }
        if (proc->in_call && !process_ready(proc) && !proc->handoff &&
            now - proc->wait_started_ms >= proc->max_wait_ms)
            proc->handoff = "wait_timeout";
        if (proc->deadline_ms > now &&
            proc->deadline_ms - now < (uint64_t)timeout_ms)
            timeout_ms = (int)(proc->deadline_ms - now);
        if (proc->child.pty && proc->output[0].open)
            snag_child_resize(&proc->child);
        bool input = proc->stdin_open &&
                     (proc->input.len || (proc->input_eof && !proc->pty_eof_sent));
        if (input && !proc->input.len && !proc->child.pty)
            process_write(proc);
        for (unsigned int s = 0u; s < 3u; ++s) {
            bool open = s < 2u ? proc->output[s].open : input && proc->stdin_open && !proc->child.pty;
            if (!open)
                continue;
            fds[count] = (struct snag_child_event){&proc->child, s,
                s == 2u ? SNAG_CHILD_WRITE : SNAG_CHILD_READ, 0};
            if (s == 0u && proc->child.pty && input)
                fds[count].events |= SNAG_CHILD_WRITE;
            map[count].proc = proc;
            map[count++].stream = s;
        }
    }
    size_t streams = count;
    do {
        rc = snag_child_wait(fds, count, wake_fd, timeout_ms);
    } while (rc < 0 && errno == EINTR);
    if (rc < 0)
        goto fail;
    /* At most 16 bounded reads/writes, rotating across all streams. */
    size_t serviced = 0u, begin = next_fd;
    for (size_t j = 0u; j < streams && serviced < 16u; ++j) {
        size_t i = (begin + j) % streams;
        struct managed_process *proc = map[i].proc;
        if (!fds[i].revents)
            continue;
        if (fds[i].revents & SNAG_CHILD_WRITE) {
            process_write(proc);
            ++serviced;
        }
        if (serviced < 16u && map[i].stream < 2u && (fds[i].revents & (SNAG_CHILD_READ | SNAG_CHILD_END))) {
            if (process_read(proc, map[i].stream) < 0)
                goto fail;
            ++serviced;
        }
        if (map[i].stream == 2u && (fds[i].revents & SNAG_CHILD_END))
            managed_close_input(proc);
        if (fds[i].revents & SNAG_CHILD_ERROR)
            goto fail;
        next_fd = (i + 1u) % streams;
    }
    now = snag_monotonic_ms();
    for (size_t i = 0u; i < SNAG_MAX_PROCESSES; ++i) {
        struct managed_process *proc = processes[i];
        if (!proc || !proc->child_done || now < proc->drain_deadline_ms)
            continue;
        /* A separate server can retain a passed writer after the child exits.
         * Bound capture, not that unrelated process's lifetime. Output activity
         * never extends this deadline. */
        for (unsigned int s = 0u; s < 2u; ++s) {
            if (!proc->output[s].open)
                continue;
            proc->output_incomplete = true;
            if (close_output(proc, s) < 0)
                goto fail;
        }
    }
    return 0;
fail:
    return snag_errorf(error, error_size, "command I/O or output journal failed: %s", strerror(errno));
}

int
snag_tools_collect(const char *handle, const char *reason, json_t **result,
                    char *error, size_t error_size)
{
    struct managed_process *proc = find_process(handle);
    struct output_excerpt streams[2] = {0};
    const char *status = "running";
    int64_t exit_code = -1;
    int signal_number = -1, rc = -1;
    if (!proc || !journal_read) {
        return snag_errorf(error, error_size, "command handle or output journal unavailable");
    }
    for (unsigned int s = 0u; s < 2u; ++s) {
        size_t cap = proc->max_output_tokens;
        if (cap > SNAG_MAX_EVENT_LINE / 16u)
            cap = SNAG_MAX_EVENT_LINE / 16u;
        snag_buf_init(&streams[s].data, cap);
        streams[s].bytes = proc->output_offset[s] - proc->collected_offset[s];
        if (journal_read(journal_opaque, handle, s, proc->collected_offset[s],
                         proc->output_offset[s], &streams[s].data) < 0)
            goto out;
        proc->result_offset[s] = proc->output_offset[s];
    }
    if (process_ready(proc)) {
        /* Keep the exited leader unreaped until collection: its PID/group
         * cannot be reused while draining descendants or closing the job. */
        if (!proc->child.reaped) {
            snag_child_signal(&proc->child, SNAG_CHILD_KILL);
            if (snag_child_reap(&proc->child) < 0)
                goto out;
        }
        reason = proc->output_incomplete ? "output_drain_timeout" : NULL;
        if (proc->cancelled) {
            status = "cancelled";
            reason = "turn_cancelled";
        } else if (proc->child.exit_code >= 0) {
            exit_code = proc->child.exit_code;
            status = exit_code ? "failed" : "succeeded";
        } else if (proc->child.signal_number >= 0) {
            status = "signaled";
            signal_number = proc->child.signal_number;
        } else {
            status = "outcome_unknown";
            reason = "owner_lost";
        }
    } else if (proc->handoff && (!reason || !strcmp(reason, "batch_yield"))) {
        reason = proc->handoff;
    }
    *result = result_json(status, reason, exit_code, signal_number,
        snag_monotonic_ms() - proc->started_ms, process_ready(proc) ? NULL : handle, proc,
        &streams[0], &streams[1]);
    if (!*result ||
        snag_json_set_new(*result, "max_output_tokens", json_integer(proc->max_output_tokens)) < 0)
        goto out;
    json_t *ref = json_pack("{s:I,s:I,s:I,s:I,s:I,s:I,s:I,s:i,s:i,s:s,s:b}",
        "stdout_start", (json_int_t)proc->collected_offset[0],
        "stdout_end", (json_int_t)proc->result_offset[0],
        "stderr_start", (json_int_t)proc->collected_offset[1],
        "stderr_end", (json_int_t)proc->result_offset[1],
        "stdin_accepted", (json_int_t)proc->input_accepted_total,
        "stdin_written", (json_int_t)proc->input_written_total,
        "stdin_pending", (json_int_t)(proc->input.len - proc->input_written),
        "log_start", 0, "log_end", 0, "handle", handle,
        "stdin_open", (int)proc->stdin_open);
    if (snag_json_set_new(*result, "output_ref", ref) < 0)
        goto out;
    rc = 0;
out:
    for (unsigned int s = 0u; s < 2u; ++s)
        snag_buf_free(&streams[s].data);
    if (rc < 0 && *result) {
        json_decref(*result);
        *result = NULL;
    }
    return rc;
}

void
snag_tools_collected(const char *handle)
{
    struct managed_process *proc = find_process(handle);
    if (!proc)
        return;
    if (process_ready(proc)) {
        managed_release(proc);
    } else {
        memcpy(proc->collected_offset, proc->result_offset, sizeof(proc->result_offset));
        proc->in_call = false;
        proc->handoff = NULL;
    }
}

static int
start_command(const char *handle, const char *command, const char *workdir,
                         const char *stdin_text, uint32_t timeout_ms,
                         uint32_t max_output_tokens,
                         bool pty,
                         const struct snag_config *config,
                         const struct snag_credential *credential,
                         char *error, size_t error_size)
{
    char **env = NULL;
    struct managed_process *proc = NULL;
    size_t slot;
    size_t stdin_len = stdin_text ? strlen(stdin_text) : 0u;

    for (slot = 0u; slot < SNAG_MAX_PROCESSES && processes[slot]; ++slot)
        ;
    if (slot == SNAG_MAX_PROCESSES || !(proc = calloc(1u, sizeof(*proc))))
        return -1;
    snag_child_init(&proc->child);
    processes[slot] = proc;
    managed_register_cleanup();
    if (snag_secret_set_build(&proc->secrets, config, credential, error, error_size) < 0)
        goto out;
    for (size_t i = 0u; i < proc->secrets.wire.count; ++i) {
        size_t len = strlen(proc->secrets.values[i]);
        if (len > proc->max_secret)
            proc->max_secret = len;
    }
    env = filtered_environment(config);
    if (!env) {
        snag_errorf(error, error_size, "cannot allocate tool environment");
        goto out;
    }
    if (snag_child_spawn(&proc->child, config->shell, command, workdir, env, pty) < 0) {
        snag_errorf(error, error_size, "cannot start tool process: %s", strerror(errno));
        goto out;
    }
    proc->stdin_open = true;
    proc->output[0].open = true;
    proc->output[1].open = !pty;
    proc->started_ms = snag_monotonic_ms();
    proc->deadline_ms = saturating_deadline(proc->started_ms, timeout_ms);
    proc->max_output_tokens = max_output_tokens;
    memcpy(proc->handle, handle, sizeof(proc->handle));
    proc->in_call = true;
    snag_buf_init(&proc->input, SNAG_TOOL_STDIN_MAX);
    if (stdin_len && snag_buf_append(&proc->input, stdin_text, stdin_len) < 0)
        goto out;
    proc->input_accepted_total = stdin_len;
    proc->input_eof = stdin_text != NULL;
    for (unsigned int s = 0u; s < 2u; ++s) {
        struct process_output *output = &proc->output[s];
        snag_buf_init(&output->data, 128u * 1024u);
        snag_buf_init(&output->pending, SNAG_TOOL_REDACTOR_MAX);
    }
    snag_environment_entries_free(env);
    env = NULL;
    return 0;

out:
    managed_release(proc);
    snag_environment_entries_free(env);
    return -1;
}

struct command_args {
    const char *command, *workdir, *input, *handle;
    uint32_t timeout, yield, limit;
    bool pty, eof, terminate, exec;
};

static int
command_args(const struct snag_response_item *call, const struct snag_config *config,
              struct command_args *args)
{
    memset(args, 0, sizeof(*args));
    args->exec = !strcmp(call->name, "exec_command");
    if ((!args->exec && strcmp(call->name, "write_stdin")) ||
        !snag_json_exact_keys(call->arguments,
            args->exec ? "command max_output_tokens pty stdin timeout_ms workdir yield_ms" :
                         "data eof handle terminate yield_ms max_output_tokens") ||
        !json_u32_member(call->arguments, "yield_ms", config->default_yield_ms,
                          0u, SNAG_TOOL_YIELD_MAX_MS, &args->yield) ||
        !command_output_limit(call->arguments, config->max_output_tokens, &args->limit))
        return -1;
    if (args->exec) {
        args->command = snag_json_string(call->arguments, "command");
        args->workdir = snag_json_string(call->arguments, "workdir");
        args->input = json_nullable_string(call->arguments, "stdin");
        args->handle = call->call_id;
        args->eof = args->input != NULL;
        if (!text_arg_valid(args->command, SNAG_TOOL_COMMAND_MAX) ||
            !absolute_dir_arg_valid(args->workdir) ||
            (args->input && !text_arg_valid(args->input, SNAG_TOOL_STDIN_MAX)) ||
            !json_bool_member(call->arguments, "pty", false, &args->pty) ||
            !json_u32_member(call->arguments, "timeout_ms", config->default_timeout_ms,
                             1u, config->max_timeout_ms, &args->timeout))
            return -1;
    } else {
        args->handle = snag_json_string(call->arguments, "handle");
        args->input = snag_json_string(call->arguments, "data");
        if (!text_arg_valid(args->input, SNAG_TOOL_STDIN_MAX) ||
            !json_bool_member(call->arguments, "eof", false, &args->eof) ||
            !json_bool_member(call->arguments, "terminate", false, &args->terminate) ||
            (args->terminate && (args->input[0] || args->eof)))
            return -1;
    }
    return args->handle && snag_hex_is_lower(args->handle, SNAG_ID_HEX_LEN) ? 0 : -1;
}

int
snag_tools_prepare(const struct snag_response_item *call, const struct snag_config *config,
                    char handle[SNAG_ID_HEX_LEN + 1u], uint32_t *yield_ms,
                    json_t **rejected)
{
    struct command_args args;
    const char *reason = NULL;
    struct managed_process *proc;
    size_t used = 0u;
    *rejected = NULL;
    if (command_args(call, config, &args) < 0) {
        reason = "invalid_arguments";
    } else {
        *yield_ms = args.yield;
        proc = find_process(args.handle);
        for (size_t i = 0u; i < SNAG_MAX_PROCESSES; ++i)
            used += processes[i] != NULL;
        if (args.exec && used >= config->max_parallel_commands)
            reason = "process_limit";
        else if (args.exec && proc)
            reason = "process_busy";
        else if (!args.exec && !proc)
            reason = "managed_process_handle_mismatch";
        else if (!args.exec && proc->in_call)
            reason = "process_busy";
        else if (!args.exec && args.input[0] && proc->input.len)
            reason = "stdin_busy";
        else if (!args.exec && args.input[0] && !proc->stdin_open)
            reason = "stdin_closed";
    }
    if (reason) {
        *rejected = snag_tool_result_not_run(reason);
        return *rejected && snag_tools_attach_output_limit(call, config, *rejected) == 0 ? 1 : -1;
    }
    if (!journal_write || !journal_read)
        return snag_errno(EINVAL);
    memcpy(handle, args.handle, SNAG_ID_HEX_LEN + 1u);
    *yield_ms = args.yield;
    return 0;
}

int
snag_tools_start(const struct snag_response_item *call, const struct snag_config *config,
                  const struct snag_credential *credential, json_t **result,
                  char *error, size_t error_size)
{
    struct command_args args;
    struct managed_process *proc;
    *result = NULL;
    if (command_args(call, config, &args) < 0)
        return -1;
    if (args.exec) {
        if (start_command(args.handle, args.command, args.workdir, args.input,
                           args.timeout, args.limit, args.pty, config, credential,
                           error, error_size) < 0) {
            *result = snag_tool_result_terminal(false, error[0] ? error : "Command could not start.");
            return *result ? 0 : -1;
        }
        proc = find_process(args.handle);
    } else {
        proc = find_process(args.handle);
        if (!proc)
            return -1;
        proc->in_call = true;
        proc->max_output_tokens = args.limit;
        if (args.terminate) {
            begin_close(proc, false);
        } else {
            size_t len = strlen(args.input);
            if (len && snag_buf_append(&proc->input, args.input, len) < 0)
                return -1;
            proc->input_accepted_total += len;
            if (args.eof)
                proc->input_eof = true;
        }
    }
    proc->wait_started_ms = snag_monotonic_ms();
    proc->max_wait_ms = config->max_wait_ms;
    return 0;
}

int
snag_tools_attach_output_limit(const struct snag_response_item *call,
                              const struct snag_config *config,
                              json_t *result)
{
    uint32_t max_output_tokens;

    if (!call || !call->name || !config || !result ||
        json_object_get(result, "max_output_tokens") ||
        (strcmp(call->name, "exec_command") != 0 &&
         strcmp(call->name, "write_stdin") != 0))
        return 0;
    if (!command_output_limit(call->arguments, config->max_output_tokens,
                              &max_output_tokens))
        max_output_tokens = config->max_output_tokens;
    if (snag_json_set_new(result, "max_output_tokens",
                   json_integer((json_int_t)max_output_tokens)) < 0) {
        return -1;
    }
    return 0;
}

static int
wait_process(const char *handle, uint32_t yield_ms, snag_tool_pump_fn pump,
              void *opaque, snag_wake_fd wake_fd, json_t **result,
              char *error, size_t error_size)
{
    uint64_t end = saturating_deadline(snag_monotonic_ms(), yield_ms);
    const char *reason = NULL;
    bool cancelled = false;
    while (!snag_tools_ready(handle)) {
        int control = pump ? pump(opaque, 0u) : 0;
        if (control < 0 || control == 2) {
            snag_tools_close_all(true);
            cancelled = true;
        } else if (control == 1 && !cancelled) {
            reason = "steering_handoff";
            break;
        }
        if (!cancelled &&
            (snag_tools_handoff(handle) || snag_monotonic_ms() >= end))
            break;
        if (snag_tools_service(10, wake_fd, error, error_size) < 0)
            return -1;
    }
    if (snag_tools_collect(handle, reason, result, error, error_size) < 0)
        return -1;
    return 0;
}

int
snag_tools_close_managed(const char *handle, bool user_interrupt,
                        snag_tool_pump_fn pump, void *pump_opaque, snag_wake_fd wake_fd,
                        json_t **result, char *error, size_t error_size)
{
    struct managed_process *proc = find_process(handle);
    if (!proc) {
        *result = snag_tool_result_outcome_unknown("owner_lost");
        return *result ? 0 : -1;
    }
    begin_close(proc, user_interrupt);
    return wait_process(handle, 0u, pump, pump_opaque, wake_fd,
                         result, error, error_size);
}

int
snag_tools_run(const struct snag_response_item *call,
              const struct snag_config *config,
              const struct snag_credential *credential,
              const char *session_workspace,
              snag_tool_pump_fn pump, void *pump_opaque, snag_wake_fd wake_fd,
              json_t **result, char *error, size_t error_size)
{
    char handle[SNAG_ID_HEX_LEN + 1u];
    uint32_t yield_ms;
    int rc;
    *result = NULL;
    if (!call || call->kind != SNAG_ITEM_TOOL_CALL || !config || !session_workspace)
        return -1;
    if (!strcmp(call->name, "apply_patch")) {
        struct snag_secret_set secrets = {0};
        rc = snag_secret_set_build(&secrets, config, credential, error, error_size);
        if (rc == 0)
            rc = snag_tools_apply_patch(call, session_workspace, result, error, error_size);
        if (rc == 0 && *result)
            rc = snag_secret_result(&secrets, *result, error, error_size);
        snag_secret_set_free(&secrets);
        return rc;
    }
    rc = snag_tools_prepare(call, config, handle, &yield_ms, result);
    if (rc != 0)
        return rc < 0 ? -1 : 0;
    if (snag_tools_start(call, config, credential, result, error, error_size) < 0)
        return -1;
    if (!*result && wait_process(handle, yield_ms, pump, pump_opaque, wake_fd,
                                 result, error, error_size) < 0)
        return -1;
    snag_tools_collected(handle);
    return snag_tools_attach_output_limit(call, config, *result);
}

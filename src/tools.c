/* SPDX-License-Identifier: GPL-2.0-only */
#include "tools.h"

#include "tools_patch.h"

#include "base.h"
#include "json.h"
#include "secret.h"

#include <errno.h>
#include <fcntl.h>
#include "snj_jansson.h"
#include <limits.h>
#include <poll.h>
#if defined(__linux__)
#define SNAJPAGENT_HAVE_PTY 1
#include <pty.h>
#include <sys/ioctl.h>
#elif defined(__APPLE__)
#define SNAJPAGENT_HAVE_PTY 1
#include <sys/ioctl.h>
#include <util.h>
#endif
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

#define SNJ_TOOL_COMMAND_MAX (256u * 1024u)
#define SNJ_TOOL_STDIN_MAX (1024u * 1024u)
#define SNJ_TOOL_POLL_MS 50u
#define SNJ_TOOL_YIELD_MAX_MS 600000u
#define SNJ_TOOL_CLOSE_GRACE_MS 2000u
#define SNJ_TOOL_REDACTOR_MAX (8192u + SNJ_WIRE_SECRET_MAX)

extern char **environ;

struct capture_stream {
    struct snj_buf data;
    uint64_t bytes;
};

struct capture_redactor {
    struct snj_buf pending;
    struct capture_stream *stream;
    const struct snj_wire_secrets *secrets;
    size_t max_secret;
};

struct managed_secret_set {
    char storage[SNJ_SECRET_VALUES_MAX][SNJ_WIRE_SECRET_MAX + 1u];
    const char *values[SNJ_SECRET_VALUES_MAX];
    struct snj_wire_secrets wire;
};

struct managed_process {
    bool active;
    char handle[SNJ_ID_HEX_LEN + 1u];
    pid_t pid;
    bool pty;
    unsigned short pty_rows;
    unsigned short pty_cols;
    int stdin_fd;
    int stdout_fd;
    int stderr_fd;
    bool stdin_open;
    bool stdout_open;
    bool stderr_open;
    bool child_done;
    bool reaped;
    bool closing;
    bool cancelled;
    bool kill_sent;
    int child_status;
    uint64_t started_ms;
    uint64_t deadline_ms;
    uint32_t max_output_tokens;
    struct managed_secret_set secrets;
    struct capture_stream stdout_stream;
    struct capture_stream stderr_stream;
    struct capture_redactor stdout_redactor;
    struct capture_redactor stderr_redactor;
    struct snj_buf input;
    size_t input_written;
    uint64_t input_accepted_total, input_written_total;
    bool input_eof, pty_eof_sent, in_call;
    uint64_t output_offset[2], collected_offset[2], result_offset[2];
    const char *handoff;
};

static struct managed_process *processes[SNJ_MAX_PROCESSES];
static snj_tool_output_fn journal_write;
static snj_tool_read_fn journal_read;
static void *journal_opaque;
static size_t next_fd;
static bool managed_cleanup_registered;

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
                         (uint32_t)SNJ_CONFIG_TOKEN_LIMIT_MAX, out))
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
    return len <= max && snj_utf8_valid((const unsigned char *)text, len, true);
}

static bool
absolute_dir_arg_valid(const char *path)
{
    struct stat st;
    size_t len;

    if (!path || path == (const char *)-1 || path[0] != '/')
        return false;
    len = strlen(path);
    return len <= SNJ_PATH_MAX_BYTES &&
           snj_utf8_valid((const unsigned char *)path, len, true) &&
           stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int
set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        return -1;
    return 0;
}

static int
make_pipe(int p[2])
{
    if (pipe(p) < 0)
        return -1;
    if (snj_fd_cloexec(p[0]) < 0 || snj_fd_cloexec(p[1]) < 0) {
        int saved = errno;
        (void)close(p[0]);
        (void)close(p[1]);
        errno = saved;
        return -1;
    }
    return 0;
}

static void
capture_init(struct capture_stream *stream)
{
    memset(stream, 0, sizeof(*stream));
    snj_buf_init(&stream->data, 128u * 1024u);
}

static void
capture_free(struct capture_stream *stream)
{
    snj_buf_free(&stream->data);
    memset(stream, 0, sizeof(*stream));
}

static int
capture_append(struct capture_stream *stream, const unsigned char *data,
               size_t len)
{
    if (len && !data) {
        errno = EINVAL;
        return -1;
    }
    if (snj_buf_append(&stream->data, data, len) < 0)
        return -1;
    stream->bytes += len;
    return 0;
}

static size_t
secret_at(const unsigned char *data, size_t len, size_t offset,
          const struct snj_wire_secrets *secrets)
{
    size_t best = 0;

    if (!secrets)
        return 0;
    for (size_t i = 0; i < secrets->count; ++i) {
        size_t n;
        if (!secrets->values[i])
            continue;
        n = strlen(secrets->values[i]);
        if (n > best && n <= len - offset &&
            (unsigned char)secrets->values[i][0] == data[offset] &&
            memcmp(data + offset, secrets->values[i], n) == 0)
            best = n;
    }
    return best;
}

static int
redactor_emit(struct capture_redactor *redactor,
              const unsigned char *data, size_t len)
{
    return capture_append(redactor->stream, data, len);
}

static int
redactor_drain(struct capture_redactor *redactor, bool final)
{
    static const unsigned char marker[] = "<redacted:secret>";
    size_t limit;
    size_t off = 0;

    if (!redactor->pending.len)
        return 0;
    limit = redactor->pending.len;
    if (!final && redactor->max_secret) {
        if (limit <= redactor->max_secret - 1u)
            limit = 0;
        else
            limit -= redactor->max_secret - 1u;
    }
    if (!redactor->max_secret || !redactor->secrets ||
        redactor->secrets->count == 0u) {
        if (limit && redactor_emit(redactor, redactor->pending.data,
                                   limit) < 0)
            return -1;
        off = limit;
    } else {
        while (off < limit) {
            size_t matched = secret_at(redactor->pending.data,
                                      redactor->pending.len, off,
                                      redactor->secrets);
            if (matched) {
                if (redactor_emit(redactor, marker, sizeof(marker) - 1u) < 0)
                    return -1;
                off += matched;
            } else {
                if (redactor_emit(redactor, redactor->pending.data + off, 1u) < 0)
                    return -1;
                ++off;
            }
        }
    }
    if (off) {
        memmove(redactor->pending.data, redactor->pending.data + off,
                redactor->pending.len - off);
        redactor->pending.len -= off;
    }
    return 0;
}

static void
redactor_init(struct capture_redactor *redactor, struct capture_stream *stream,
              const struct snj_wire_secrets *secrets)
{
    memset(redactor, 0, sizeof(*redactor));
    snj_buf_init(&redactor->pending, SNJ_TOOL_REDACTOR_MAX);
    redactor->stream = stream;
    redactor->secrets = secrets;
    if (secrets) {
        for (size_t i = 0; i < secrets->count; ++i) {
            size_t n;
            if (!secrets->values[i])
                continue;
            n = strlen(secrets->values[i]);
            if (n > redactor->max_secret)
                redactor->max_secret = n;
        }
    }
}

static void
redactor_free(struct capture_redactor *redactor)
{
    snj_buf_free(&redactor->pending);
    memset(redactor, 0, sizeof(*redactor));
}

static int
redactor_feed(struct capture_redactor *redactor,
              const unsigned char *data, size_t len)
{
    if (len && snj_buf_append(&redactor->pending, data, len) < 0)
        return -1;
    return redactor_drain(redactor, false);
}

static int
redactor_finish(struct capture_redactor *redactor)
{
    if (redactor_drain(redactor, true) < 0)
        return -1;
    if (redactor->pending.len) {
        errno = EIO;
        return -1;
    }
    return 0;
}

static json_t *
excerpt_json(const struct capture_stream *stream)
{
    struct snj_buf encoded;
    json_t *out = json_object();
    bool textual;
    int rc = -1;

    if (!out)
        return NULL;
    snj_buf_init(&encoded, SIZE_MAX);
    textual = snj_utf8_valid(stream->data.data, stream->data.len, true);
    if (textual) {
        if (snj_buf_append(&encoded, stream->data.data, stream->data.len) < 0)
            goto out;
    } else if (snj_base64_append(&encoded, stream->data.data, stream->data.len) < 0) {
        goto out;
    }
    if (snj_json_set_new(out, "discarded_bytes",
                         json_integer((json_int_t)(stream->bytes - stream->data.len))) < 0 ||
        snj_json_set_new(out, "encoding",
                         json_string(textual ? "utf8" : "base64")) < 0 ||
        snj_json_set_new(out, "original_bytes",
                         json_integer((json_int_t)stream->bytes)) < 0 ||
        snj_json_set_new(out, "retained",
                         json_stringn(encoded.len ? (const char *)encoded.data : "",
                                      encoded.len)) < 0 ||
        snj_json_set_new(out, "retained_bytes",
                         json_integer((json_int_t)stream->data.len)) < 0)
        goto out;
    rc = 0;
out:
    snj_buf_free(&encoded);
    if (rc < 0) {
        json_decref(out);
        return NULL;
    }
    return out;
}

static int
append_stream_text(struct snj_buf *out, const char *label,
                   const struct capture_stream *stream)
{
    if (!stream->data.len)
        return 0;
    if (snj_buf_printf(out, "%s%s:\n", out->len ? "\n" : "", label) < 0)
        return -1;
    if (snj_utf8_valid(stream->data.data, stream->data.len, true)) {
        if (snj_buf_append(out, stream->data.data, stream->data.len) < 0)
            return -1;
        if (stream->data.data[stream->data.len - 1u] != '\n' &&
            snj_buf_putc(out, '\n') < 0)
            return -1;
    } else {
        if (snj_buf_printf(out, "<%llu binary bytes; base64 follows>\n",
                           (unsigned long long)stream->data.len) < 0 ||
            snj_base64_append(out, stream->data.data, stream->data.len) < 0 ||
            snj_buf_putc(out, '\n') < 0)
            return -1;
    }
    return 0;
}

static char *
model_text_for(const char *status, const char *reason, int exit_code,
               int signal_number,
               const struct capture_stream *stdout_stream,
               const struct capture_stream *stderr_stream)
{
    struct snj_buf text;
    char *out = NULL;

    snj_buf_init(&text, SIZE_MAX);
    if (strcmp(status, "succeeded") == 0) {
        if (snj_buf_printf(&text, "Process exited with code %d.\n", exit_code) < 0)
            goto done;
    } else if (strcmp(status, "failed") == 0) {
        if (snj_buf_printf(&text, "Process exited with code %d.\n", exit_code) < 0)
            goto done;
    } else if (strcmp(status, "signaled") == 0) {
        if (snj_buf_printf(&text, "Process was terminated by signal %d.\n",
                           signal_number) < 0)
            goto done;
    } else if (strcmp(status, "cancelled") == 0) {
        const char *msg = "Process was cancelled by the user.\n";
        if (snj_buf_append(&text, msg, strlen(msg)) < 0)
            goto done;
    } else if (strcmp(status, "running") == 0) {
        const char *msg;

        if (reason && strcmp(reason, "timeout_handoff") == 0)
            msg = "Command timeout elapsed; the process continues in the background. Use write_stdin with the active handle to wait for, interact with, or terminate it.\n";
        else if (reason && strcmp(reason, "steering_handoff") == 0)
            msg = "Command is still running because steering arrived. Use write_stdin with the active handle to wait for, interact with, or terminate it after considering the steer.\n";
        else
            msg = "Process is still running.\n";
        if (snj_buf_append(&text, msg, strlen(msg)) < 0)
            goto done;
    } else if (strcmp(status, "io_failed") == 0) {
        const char *msg = "Tool I/O failed.\n";
        if (snj_buf_append(&text, msg, strlen(msg)) < 0)
            goto done;
    } else if (snj_buf_printf(&text, "Tool status: %s.\n", status) < 0) {
        goto done;
    }
    if (append_stream_text(&text, "stdout", stdout_stream) < 0 ||
        append_stream_text(&text, "stderr", stderr_stream) < 0)
        goto done;
    if (snj_buf_terminate(&text) < 0)
        goto done;
    out = (char *)text.data;
    memset(&text, 0, sizeof(text));
done:
    snj_buf_free(&text);
    return out;
}

static int
result_set(json_t *object, const char *key, json_t *value)
{
    return snj_json_set_new(object, key, value);
}

static json_t *
result_json(const char *status, const char *reason, int exit_code,
            int signal_number, uint64_t duration_ms, const char *handle,
            const struct capture_stream *stdout_stream,
            const struct capture_stream *stderr_stream)
{
    json_t *out = json_object();
    json_t *stdout_json = NULL;
    json_t *stderr_json = NULL;
    char *model_text = NULL;
    int rc = -1;

    if (!out)
        return NULL;
    model_text = model_text_for(status, reason, exit_code, signal_number,
                                stdout_stream, stderr_stream);
    stdout_json = excerpt_json(stdout_stream);
    stderr_json = excerpt_json(stderr_stream);
    if (!model_text || !stdout_json || !stderr_json)
        goto done;
    if (result_set(out, "duration_ms",
                   json_integer((json_int_t)duration_ms)) < 0)
        goto done;
    if (result_set(out, "exit_code",
                   exit_code >= 0 ? json_integer(exit_code) : json_null()) < 0)
        goto done;
    if (result_set(out, "handle",
                   handle ? json_string(handle) : json_null()) < 0)
        goto done;
    if (result_set(out, "model_text", json_string(model_text)) < 0)
        goto done;
    if (result_set(out, "reason",
                   reason ? json_string(reason) : json_null()) < 0)
        goto done;
    if (result_set(out, "signal",
                   signal_number > 0 ? json_integer(signal_number) : json_null()) < 0)
        goto done;
    if (result_set(out, "status", json_string(status)) < 0)
        goto done;
    if (result_set(out, "stderr", stderr_json) < 0)
        goto done;
    stderr_json = NULL;
    if (result_set(out, "stdout", stdout_json) < 0)
        goto done;
    stdout_json = NULL;
    rc = 0;
done:
    free(model_text);
    if (stdout_json)
        json_decref(stdout_json);
    if (stderr_json)
        json_decref(stderr_json);
    if (rc < 0) {
        json_decref(out);
        return NULL;
    }
    return out;
}

static bool
env_name_matches(const char *entry, const char *name)
{
    size_t len = strlen(name);
    return strncmp(entry, name, len) == 0 && entry[len] == '=';
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
remove_env_entry(const char *entry, const struct snj_config *config)
{
    static const char *const proxy_names[] = {
        "HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY",
        "http_proxy", "https_proxy", "all_proxy"
    };

    if (env_name_matches(entry, "OPENAI_API_KEY"))
        return true;
    for (size_t i = 0; i < config->provider_count; ++i)
        if (env_name_matches(entry, config->providers[i].api_key_env))
            return true;
    for (size_t i = 0; i < config->secret_env_count; ++i)
        if (env_name_matches(entry, config->secret_env[i]))
            return true;
    for (size_t i = 0; i < sizeof(proxy_names) / sizeof(proxy_names[0]); ++i)
        if (env_name_matches(entry, proxy_names[i]) && proxy_with_userinfo(entry))
            return true;
    if (strncmp(entry, "SNAJPAGENT_", 11u) == 0)
        return true;
    return false;
}

static char **
filtered_environment(const struct snj_config *config)
{
    size_t count = 0;
    size_t kept = 0;
    char **env;

    while (environ[count])
        ++count;
    env = calloc(count + 1u, sizeof(*env));
    if (!env)
        return NULL;
    for (size_t i = 0; i < count; ++i)
        if (!remove_env_entry(environ[i], config))
            env[kept++] = environ[i];
    env[kept] = NULL;
    return env;
}

static void
close_if_open(int *fd)
{
    if (*fd >= 0) {
        (void)close(*fd);
        *fd = -1;
    }
}

static void
kill_child_group(pid_t pid, int signo)
{
    if (pid <= 0)
        return;
    if (kill(-pid, signo) < 0 && errno == ESRCH)
        (void)kill(pid, signo);
}


static int
write_stdin_chunk(int *fd, const char *data, size_t len, size_t *written,
                  bool *open_flag, bool close_on_done)
{
    while (*written < len) {
        ssize_t n = write(*fd, data + *written, len - *written);
        if (n > 0) {
            *written += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return 0;
        *open_flag = false;
        return 0;
    }
    if (close_on_done) {
        *open_flag = false;
        close_if_open(fd);
    }
    return 0;
}

static int
exec_child(const char *shell, const char *command, const char *workdir,
           int stdin_rd, int stdout_wr, int stderr_wr, char **env)
{
    (void)setpgid(0, 0);
    if (chdir(workdir) < 0)
        _exit(125);
    if (dup2(stdin_rd, STDIN_FILENO) < 0 ||
        dup2(stdout_wr, STDOUT_FILENO) < 0 ||
        dup2(stderr_wr, STDERR_FILENO) < 0)
        _exit(125);
    for (int fd = 3; fd < 256; ++fd)
        (void)close(fd);
    execle(shell, shell, "-c", command, (char *)NULL, env);
    _exit(errno == ENOENT ? 127 : 126);
}

#if defined(SNAJPAGENT_HAVE_PTY)
static void
host_winsize(unsigned short *rows, unsigned short *cols)
{
    static const int fds[] = {STDERR_FILENO, STDOUT_FILENO, STDIN_FILENO};
    struct winsize ws;

    *rows = 24;
    *cols = 80;
    for (size_t i = 0; i < sizeof(fds) / sizeof(fds[0]); ++i) {
        memset(&ws, 0, sizeof(ws));
        if (ioctl(fds[i], TIOCGWINSZ, &ws) == 0 && ws.ws_row && ws.ws_col) {
            *rows = ws.ws_row;
            *cols = ws.ws_col;
            return;
        }
    }
}

static void
pty_apply_current_size(int fd, unsigned short *rows, unsigned short *cols)
{
    struct winsize ws;
    unsigned short new_rows;
    unsigned short new_cols;

    if (fd < 0)
        return;
    host_winsize(&new_rows, &new_cols);
    if (*rows == new_rows && *cols == new_cols)
        return;
    memset(&ws, 0, sizeof(ws));
    ws.ws_row = new_rows;
    ws.ws_col = new_cols;
    if (ioctl(fd, TIOCSWINSZ, &ws) == 0) {
        *rows = new_rows;
        *cols = new_cols;
    }
}

static int
open_pty_pair(int *master_fd, int *slave_fd,
              unsigned short *rows, unsigned short *cols)
{
    struct winsize ws;

    host_winsize(rows, cols);
    memset(&ws, 0, sizeof(ws));
    ws.ws_row = *rows;
    ws.ws_col = *cols;
    return openpty(master_fd, slave_fd, NULL, NULL, &ws);
}

static int
exec_pty_child(const char *shell, const char *command, const char *workdir,
               int slave_fd, char **env)
{
    if (setsid() < 0)
        _exit(125);
    (void)ioctl(slave_fd, TIOCSCTTY, 0);
    if (chdir(workdir) < 0)
        _exit(125);
    if (dup2(slave_fd, STDIN_FILENO) < 0 ||
        dup2(slave_fd, STDOUT_FILENO) < 0 ||
        dup2(slave_fd, STDERR_FILENO) < 0)
        _exit(125);
    for (int fd = 3; fd < 256; ++fd)
        (void)close(fd);
    execle(shell, shell, "-c", command, (char *)NULL, env);
    _exit(errno == ENOENT ? 127 : 126);
}
#else
static void
pty_apply_current_size(int fd, unsigned short *rows, unsigned short *cols)
{
    (void)fd;
    (void)rows;
    (void)cols;
}

static int
open_pty_pair(int *master_fd, int *slave_fd,
              unsigned short *rows, unsigned short *cols)
{
    (void)master_fd;
    (void)slave_fd;
    (void)rows;
    (void)cols;
    errno = ENOTSUP;
    return -1;
}
#endif

static uint64_t
saturating_deadline(uint64_t start, uint32_t delta_ms)
{
    if (!delta_ms)
        return UINT64_MAX;
    uint64_t deadline = start + delta_ms;
    return deadline < start ? UINT64_MAX : deadline;
}

static void
managed_secret_set_build(struct managed_secret_set *set,
                         const struct snj_config *config,
                         const struct snj_credential *credential)
{
    size_t count = 0u;

    memset(set, 0, sizeof(*set));
    if (credential && credential->len && count < SNJ_SECRET_VALUES_MAX) {
        size_t len = credential->len;
        if (len > SNJ_WIRE_SECRET_MAX)
            len = SNJ_WIRE_SECRET_MAX;
        memcpy(set->storage[count], credential->value, len);
        set->storage[count][len] = '\0';
        set->values[count] = set->storage[count];
        ++count;
    }
    if (config) {
        for (size_t i = 0; i < config->provider_count &&
             count < SNJ_SECRET_VALUES_MAX; ++i) {
            const char *value = getenv(config->providers[i].api_key_env);
            size_t len;
            bool duplicate = false;
            if (!value)
                continue;
            len = strnlen(value, SNJ_WIRE_SECRET_MAX + 1u);
            if (!len || len > SNJ_WIRE_SECRET_MAX)
                continue;
            for (size_t j = 0; j < count; ++j)
                if (strcmp(set->values[j], value) == 0) {
                    duplicate = true;
                    break;
                }
            if (duplicate)
                continue;
            memcpy(set->storage[count], value, len);
            set->storage[count][len] = '\0';
            set->values[count] = set->storage[count];
            ++count;
        }
        for (size_t i = 0; i < config->secret_env_count &&
             count < SNJ_SECRET_VALUES_MAX; ++i) {
            const char *value = getenv(config->secret_env[i]);
            size_t len;
            if (!value)
                continue;
            len = strnlen(value, SNJ_WIRE_SECRET_MAX + 1u);
            if (!len || len > SNJ_WIRE_SECRET_MAX)
                continue;
            memcpy(set->storage[count], value, len);
            set->storage[count][len] = '\0';
            set->values[count] = set->storage[count];
            ++count;
        }
    }
    set->wire.values = set->values;
    set->wire.count = count;
}

static struct managed_process *
find_process(const char *handle)
{
    for (size_t i = 0u; i < SNJ_MAX_PROCESSES; ++i)
        if (processes[i] && handle && !strcmp(processes[i]->handle, handle))
            return processes[i];
    return NULL;
}

void
snj_tools_journal(snj_tool_output_fn write, snj_tool_read_fn read, void *opaque)
{
    journal_write = write;
    journal_read = read;
    journal_opaque = opaque;
}

static void
managed_close_input(struct managed_process *proc)
{
    if (!proc->pty)
        close_if_open(&proc->stdin_fd);
    proc->stdin_open = false;
}

static void
managed_release(struct managed_process *proc)
{
    if (!proc)
        return;
    if (!proc->reaped && proc->pid > 0) {
        kill_child_group(proc->pid, SIGKILL);
        while (waitpid(proc->pid, NULL, 0) < 0 && errno == EINTR)
            ;
    }
    if (!proc->pty)
        close_if_open(&proc->stdin_fd);
    close_if_open(&proc->stdout_fd);
    close_if_open(&proc->stderr_fd);
    redactor_free(&proc->stdout_redactor);
    redactor_free(&proc->stderr_redactor);
    capture_free(&proc->stdout_stream);
    capture_free(&proc->stderr_stream);
    snj_buf_free(&proc->input);
    for (size_t i = 0u; i < SNJ_MAX_PROCESSES; ++i)
        if (processes[i] == proc)
            processes[i] = NULL;
    memset(&proc->secrets, 0, sizeof(proc->secrets));
    free(proc);
}

static void
managed_cleanup_at_exit(void)
{
    for (size_t i = 0u; i < SNJ_MAX_PROCESSES; ++i)
        managed_release(processes[i]);
}

void
snj_tools_shutdown(void)
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
    return proc->child_done && !proc->stdout_open && !proc->stderr_open;
}

bool
snj_tools_ready(const char *handle)
{
    struct managed_process *proc = find_process(handle);
    return proc && process_ready(proc);
}

bool
snj_tools_busy(void)
{
    for (size_t i = 0u; i < SNJ_MAX_PROCESSES; ++i)
        if (processes[i] && !process_ready(processes[i]))
            return true;
    return false;
}

const char *
snj_tools_handoff(const char *handle)
{
    struct managed_process *proc = find_process(handle);
    return proc && !proc->closing ? proc->handoff : NULL;
}

void
snj_tools_process_state(struct snj_process_state *state)
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
    snj_buf_reset(&proc->input);
    proc->input_written = 0u;
    managed_close_input(proc);
    proc->closing = true;
    proc->cancelled = user_interrupt;
    if (!proc->reaped)
        kill_child_group(proc->pid, user_interrupt ? SIGINT : SIGTERM);
    proc->deadline_ms = saturating_deadline(snj_monotonic_ms(),
                                            SNJ_TOOL_CLOSE_GRACE_MS);
}

void
snj_tools_close_all(bool user_interrupt)
{
    for (size_t i = 0u; i < SNJ_MAX_PROCESSES; ++i)
        if (processes[i])
            begin_close(processes[i], user_interrupt);
}

static int
flush_capture(struct managed_process *proc, unsigned int stream)
{
    struct capture_stream *capture = stream ? &proc->stderr_stream : &proc->stdout_stream;
    size_t consumed = 0u;
    while (consumed < capture->data.len) {
        size_t n = capture->data.len - consumed;
        bool open = stream ? proc->stderr_open : proc->stdout_open;
        if (n > 16384u)
            n = 16384u;
        /* Keep an incomplete UTF-8 suffix for the next read; binary data is
         * encoded losslessly by the journal callback. */
        size_t full = n;
        for (size_t tail = 1u; tail <= 3u && tail <= full; ++tail) {
            unsigned char c = capture->data.data[consumed + full - tail];
            size_t width = c >= 0xc2u && c <= 0xdfu ? 2u :
                           c >= 0xe0u && c <= 0xefu ? 3u :
                           c >= 0xf0u && c <= 0xf4u ? 4u : 0u;
            if (width > tail && (open || consumed + full < capture->data.len) &&
                snj_utf8_valid(capture->data.data + consumed, full - tail, true)) {
                n = full - tail;
                break;
            }
        }
        if (!n)
            break;
        if (!journal_write ||
            journal_write(journal_opaque, proc->handle, stream,
                          proc->output_offset[stream], capture->data.data + consumed, n) < 0)
            return -1;
        proc->output_offset[stream] += n;
        consumed += n;
    }
    if (capture->data.len > consumed)
        memmove(capture->data.data, capture->data.data + consumed, capture->data.len - consumed);
    capture->data.len -= consumed;
    capture->bytes = capture->data.len;
    return 0;
}

static int
process_read(struct managed_process *proc, unsigned int stream)
{
    struct capture_redactor *redactor = stream ? &proc->stderr_redactor : &proc->stdout_redactor;
    int *fd = stream ? &proc->stderr_fd : &proc->stdout_fd;
    bool *open = stream ? &proc->stderr_open : &proc->stdout_open;
    unsigned char bytes[4096];
    ssize_t n = read(*fd, bytes, sizeof(bytes));
    if (n > 0) {
        if (redactor_feed(redactor, bytes, (size_t)n) < 0)
            return -1;
    } else if (n == 0 || (n < 0 && proc->pty && errno == EIO)) {
        if (redactor_finish(redactor) < 0)
            return -1;
        *open = false;
        close_if_open(fd);
        if (proc->pty) {
            proc->stdin_open = false;
            proc->stdin_fd = -1;
        }
    } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
        return -1;
    }
    return flush_capture(proc, stream);
}

static int
process_write(struct managed_process *proc)
{
    size_t before = proc->input_written;
    size_t end = proc->input.len;
    if (end - before > 4096u)
        end = before + 4096u;
    if (before < end &&
        write_stdin_chunk(&proc->stdin_fd, (const char *)proc->input.data, end,
                           &proc->input_written, &proc->stdin_open, false) < 0)
        return -1;
    proc->input_written_total += proc->input_written - before;
    if (proc->input_written == proc->input.len) {
        snj_buf_reset(&proc->input);
        proc->input_written = 0u;
        if (proc->input_eof && proc->stdin_open) {
            if (proc->pty && !proc->pty_eof_sent) {
                size_t written = 0u;
                if (write_stdin_chunk(&proc->stdin_fd, "\004", 1u, &written,
                                       &proc->stdin_open, false) < 0)
                    return -1;
                proc->pty_eof_sent = written == 1u;
                if (!proc->pty_eof_sent)
                    return 0;
            }
            managed_close_input(proc);
        }
    }
    return 0;
}

int
snj_tools_service(int timeout_ms, int wake_fd, char *error, size_t error_size)
{
    struct pollfd fds[SNJ_MAX_PROCESSES * 3u + 1u];
    struct { struct managed_process *proc; unsigned int stream; } map[SNJ_MAX_PROCESSES * 3u + 1u];
    nfds_t count = 0u;
    uint64_t now = snj_monotonic_ms();
    int rc;
    if (timeout_ms > (int)SNJ_TOOL_POLL_MS)
        timeout_ms = (int)SNJ_TOOL_POLL_MS;
    for (size_t i = 0u; i < SNJ_MAX_PROCESSES; ++i) {
        struct managed_process *proc = processes[i];
        if (!proc)
            continue;
        if (!proc->child_done) {
            siginfo_t info;
            memset(&info, 0, sizeof(info));
            if (waitid(P_PID, (id_t)proc->pid, &info, WEXITED | WNOHANG | WNOWAIT) < 0) {
                if (errno != EINTR)
                    goto fail;
            } else if (info.si_pid == proc->pid) {
                proc->child_done = true;
                managed_close_input(proc);
            }
        }
        if (!process_ready(proc) && now >= proc->deadline_ms) {
            proc->deadline_ms = UINT64_MAX;
            if (proc->closing) {
                kill_child_group(proc->pid, SIGKILL);
                proc->kill_sent = true;
            } else {
                proc->handoff = "timeout_handoff";
            }
        }
        if (proc->deadline_ms > now &&
            proc->deadline_ms - now < (uint64_t)timeout_ms)
            timeout_ms = (int)(proc->deadline_ms - now);
        if (proc->pty && proc->stdout_open)
            pty_apply_current_size(proc->stdout_fd, &proc->pty_rows, &proc->pty_cols);
        bool input = proc->stdin_open &&
                     (proc->input.len || (proc->input_eof && !proc->pty_eof_sent));
        if (input && !proc->input.len && !proc->pty && process_write(proc) < 0)
            goto fail;
        for (unsigned int s = 0u; s < 3u; ++s) {
            int fd = s == 0u ? proc->stdout_fd : s == 1u ? proc->stderr_fd : proc->stdin_fd;
            bool open = s == 0u ? proc->stdout_open : s == 1u ? proc->stderr_open :
                                  input && !proc->pty;
            if (!open || fd < 0)
                continue;
            fds[count] = (struct pollfd){fd, s == 2u ? POLLOUT : POLLIN, 0};
            if (s == 0u && proc->pty && input)
                fds[count].events |= POLLOUT;
            map[count].proc = proc;
            map[count++].stream = s;
        }
    }
    nfds_t streams = count;
    if (wake_fd >= 0)
        fds[count++] = (struct pollfd){wake_fd, POLLIN, 0};
    do {
        rc = poll(fds, count, timeout_ms);
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
        if (fds[i].revents & POLLOUT) {
            if (process_write(proc) < 0)
                goto fail;
            ++serviced;
        }
        if (serviced < 16u && map[i].stream < 2u && (fds[i].revents & (POLLIN | POLLHUP | POLLERR))) {
            if (process_read(proc, map[i].stream) < 0)
                goto fail;
            ++serviced;
        }
        if (map[i].stream == 2u && (fds[i].revents & (POLLHUP | POLLERR)))
            managed_close_input(proc);
        if (fds[i].revents & POLLNVAL)
            goto fail;
        next_fd = (i + 1u) % streams;
    }
    return 0;
fail:
    snj_errorf(error, error_size, "command I/O or output journal failed: %s", strerror(errno));
    return -1;
}

int
snj_tools_collect(const char *handle, const char *reason, json_t **result,
                    char *error, size_t error_size)
{
    struct managed_process *proc = find_process(handle);
    struct capture_stream streams[2] = {0};
    const char *status = "running";
    int exit_code = -1, signal_number = -1, rc = -1;
    if (!proc || !journal_read) {
        snj_errorf(error, error_size, "command handle or output journal unavailable");
        return -1;
    }
    for (unsigned int s = 0u; s < 2u; ++s) {
        size_t cap = proc->max_output_tokens;
        if (cap > SNJ_MAX_EVENT_LINE / 16u)
            cap = SNJ_MAX_EVENT_LINE / 16u;
        snj_buf_init(&streams[s].data, cap);
        streams[s].bytes = proc->output_offset[s] - proc->collected_offset[s];
        if (journal_read(journal_opaque, handle, s, proc->collected_offset[s],
                         proc->output_offset[s], &streams[s].data) < 0)
            goto out;
        proc->result_offset[s] = proc->output_offset[s];
    }
    if (process_ready(proc)) {
        /* Keep the exited leader unreaped until collection: its PID/group
         * cannot be reused while draining descendants or closing the job. */
        if (!proc->reaped) {
            kill_child_group(proc->pid, SIGKILL);
            pid_t got;
            do {
                got = waitpid(proc->pid, &proc->child_status, WNOHANG);
            } while (got < 0 && errno == EINTR);
            if (got != proc->pid)
                goto out;
            proc->reaped = true;
        }
        reason = NULL;
        if (proc->cancelled) {
            status = "cancelled";
            reason = "turn_cancelled";
        } else if (WIFEXITED(proc->child_status)) {
            exit_code = WEXITSTATUS(proc->child_status);
            status = exit_code ? "failed" : "succeeded";
        } else if (WIFSIGNALED(proc->child_status)) {
            status = "signaled";
            signal_number = WTERMSIG(proc->child_status);
        } else {
            status = "outcome_unknown";
            reason = "owner_lost";
        }
    } else if (proc->handoff) {
        reason = proc->handoff;
    }
    *result = result_json(status, reason, exit_code, signal_number,
        snj_monotonic_ms() - proc->started_ms, process_ready(proc) ? NULL : handle,
        &streams[0], &streams[1]);
    if (!*result ||
        snj_json_set_new(*result, "max_output_tokens", json_integer(proc->max_output_tokens)) < 0)
        goto out;
    json_t *ref = json_object();
    static const char *const keys[] = {"stdout_start", "stdout_end", "stderr_start",
        "stderr_end", "stdin_accepted", "stdin_written", "stdin_pending"};
    uint64_t values[] = {proc->collected_offset[0], proc->result_offset[0],
        proc->collected_offset[1], proc->result_offset[1], proc->input_accepted_total,
        proc->input_written_total, proc->input.len - proc->input_written};
    if (!ref)
        goto out;
    for (size_t i = 0u; i < sizeof(values) / sizeof(values[0]); ++i)
        if (snj_json_set_new(ref, keys[i], json_integer((json_int_t)values[i])) < 0) {
            json_decref(ref);
            goto out;
        }
    if (snj_json_set_new(ref, "handle", json_string(handle)) < 0 ||
        snj_json_set_new(ref, "stdin_open", json_boolean(proc->stdin_open)) < 0) {
        json_decref(ref);
        goto out;
    }
    if (snj_json_set_new(*result, "output_ref", ref) < 0)
        goto out;
    rc = 0;
out:
    for (unsigned int s = 0u; s < 2u; ++s)
        capture_free(&streams[s]);
    if (rc < 0 && *result) {
        json_decref(*result);
        *result = NULL;
    }
    return rc;
}

void
snj_tools_collected(const char *handle)
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
                         const struct snj_config *config,
                         const struct snj_credential *credential,
                         char *error, size_t error_size)
{
    int in_pipe[2] = {-1, -1};
    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    int pty_master = -1;
    int pty_slave = -1;
    unsigned short pty_rows = 24;
    unsigned short pty_cols = 80;
    char **env = NULL;
    pid_t pid;
    struct managed_process *proc = NULL;
    size_t slot;
    size_t stdin_len = stdin_text ? strlen(stdin_text) : 0u;

    for (slot = 0u; slot < SNJ_MAX_PROCESSES && processes[slot]; ++slot)
        ;
    if (slot == SNJ_MAX_PROCESSES || !(proc = calloc(1u, sizeof(*proc))))
        return -1;
    proc->stdin_fd = proc->stdout_fd = proc->stderr_fd = -1;
    processes[slot] = proc;
    managed_register_cleanup();
    if (pty) {
        if (open_pty_pair(&pty_master, &pty_slave,
                          &pty_rows, &pty_cols) < 0) {
            snj_errorf(error, error_size, "cannot create tool PTY: %s",
                      strerror(errno));
            goto out;
        }
        if (snj_fd_cloexec(pty_master) < 0 || snj_fd_cloexec(pty_slave) < 0 ||
            set_nonblock(pty_master) < 0) {
            snj_errorf(error, error_size, "cannot configure tool PTY: %s",
                      strerror(errno));
            goto out;
        }
    } else {
        if (make_pipe(in_pipe) < 0 || make_pipe(out_pipe) < 0 ||
            make_pipe(err_pipe) < 0) {
            snj_errorf(error, error_size, "cannot create tool pipes: %s",
                      strerror(errno));
            goto out;
        }
        if (set_nonblock(in_pipe[1]) < 0 || set_nonblock(out_pipe[0]) < 0 ||
            set_nonblock(err_pipe[0]) < 0) {
            snj_errorf(error, error_size, "cannot configure tool pipes: %s",
                      strerror(errno));
            goto out;
        }
    }
    env = filtered_environment(config);
    if (!env) {
        snj_errorf(error, error_size, "cannot allocate tool environment");
        goto out;
    }
    pid = fork();
    if (pid < 0) {
        snj_errorf(error, error_size, "cannot fork tool process: %s", strerror(errno));
        goto out;
    }
    if (pid == 0) {
        sigset_t signals;
        sigemptyset(&signals);
        (void)sigprocmask(SIG_SETMASK, &signals, NULL);
        if (pty) {
            close_if_open(&pty_master);
#if defined(SNAJPAGENT_HAVE_PTY)
            exec_pty_child(config->shell, command, workdir, pty_slave, env);
#else
            _exit(125);
#endif
        } else {
            close_if_open(&in_pipe[1]);
            close_if_open(&out_pipe[0]);
            close_if_open(&err_pipe[0]);
            exec_child(config->shell, command, workdir,
                       in_pipe[0], out_pipe[1], err_pipe[1], env);
        }
    }
    if (pty) {
        close_if_open(&pty_slave);
    } else {
        (void)setpgid(pid, pid);
        close_if_open(&in_pipe[0]);
        close_if_open(&out_pipe[1]);
        close_if_open(&err_pipe[1]);
    }

    proc->stdin_fd = pty ? pty_master : in_pipe[1];
    proc->stdout_fd = pty ? pty_master : out_pipe[0];
    proc->stderr_fd = pty ? -1 : err_pipe[0];
    if (pty)
        pty_master = -1;
    else {
        in_pipe[1] = -1;
        out_pipe[0] = -1;
        err_pipe[0] = -1;
    }
    proc->active = true;
    proc->pid = pid;
    proc->pty = pty;
    proc->pty_rows = pty_rows;
    proc->pty_cols = pty_cols;
    proc->stdin_open = true;
    proc->stdout_open = true;
    proc->stderr_open = !pty;
    proc->started_ms = snj_monotonic_ms();
    proc->deadline_ms = saturating_deadline(proc->started_ms, timeout_ms);
    proc->max_output_tokens = max_output_tokens;
    memcpy(proc->handle, handle, sizeof(proc->handle));
    proc->in_call = true;
    snj_buf_init(&proc->input, SNJ_TOOL_STDIN_MAX);
    if (stdin_len && snj_buf_append(&proc->input, stdin_text, stdin_len) < 0)
        goto out;
    proc->input_accepted_total = stdin_len;
    proc->input_eof = stdin_text != NULL;
    managed_secret_set_build(&proc->secrets, config, credential);
    capture_init(&proc->stdout_stream);
    capture_init(&proc->stderr_stream);
    redactor_init(&proc->stdout_redactor, &proc->stdout_stream,
                  &proc->secrets.wire);
    redactor_init(&proc->stderr_redactor, &proc->stderr_stream,
                  &proc->secrets.wire);
    free(env);
    env = NULL;
    return 0;

out:
    managed_release(proc);
    close_if_open(&in_pipe[0]);
    close_if_open(&in_pipe[1]);
    close_if_open(&out_pipe[0]);
    close_if_open(&out_pipe[1]);
    close_if_open(&err_pipe[0]);
    close_if_open(&err_pipe[1]);
    close_if_open(&pty_master);
    close_if_open(&pty_slave);
    free(env);
    return -1;
}

struct command_args {
    const char *command, *workdir, *input, *handle;
    uint32_t timeout, yield, limit;
    bool pty, eof, terminate, exec;
};

static int
command_args(const struct snj_response_item *call, const struct snj_config *config,
              struct command_args *args)
{
    static const char *const exec_keys[] = {
        "command", "max_output_tokens", "pty", "stdin", "timeout_ms", "workdir", "yield_ms"
    };
    static const char *const stdin_keys[] = {
        "data", "eof", "handle", "terminate", "yield_ms", "max_output_tokens"
    };
    memset(args, 0, sizeof(*args));
    args->exec = !strcmp(call->name, "exec_command");
    if ((!args->exec && strcmp(call->name, "write_stdin")) ||
        !snj_json_exact_keys(call->arguments, args->exec ? exec_keys : stdin_keys,
                             args->exec ? 7u : 6u) ||
        !json_u32_member(call->arguments, "yield_ms", config->default_yield_ms,
                          0u, SNJ_TOOL_YIELD_MAX_MS, &args->yield) ||
        !command_output_limit(call->arguments, config->max_output_tokens, &args->limit))
        return -1;
    if (args->exec) {
        args->command = snj_json_string(call->arguments, "command");
        args->workdir = snj_json_string(call->arguments, "workdir");
        args->input = json_nullable_string(call->arguments, "stdin");
        args->handle = call->call_id;
        args->eof = args->input != NULL;
        if (!text_arg_valid(args->command, SNJ_TOOL_COMMAND_MAX) ||
            !absolute_dir_arg_valid(args->workdir) ||
            (args->input && !text_arg_valid(args->input, SNJ_TOOL_STDIN_MAX)) ||
            !json_bool_member(call->arguments, "pty", false, &args->pty) ||
            !json_u32_member(call->arguments, "timeout_ms", config->default_timeout_ms,
                             1u, config->max_timeout_ms, &args->timeout))
            return -1;
    } else {
        args->handle = snj_json_string(call->arguments, "handle");
        args->input = snj_json_string(call->arguments, "data");
        if (!text_arg_valid(args->input, SNJ_TOOL_STDIN_MAX) ||
            !json_bool_member(call->arguments, "eof", false, &args->eof) ||
            !json_bool_member(call->arguments, "terminate", false, &args->terminate) ||
            (args->terminate && (args->input[0] || args->eof)))
            return -1;
        if (args->terminate)
            args->yield = 0u;
    }
    return args->handle && snj_hex_is_lower(args->handle, SNJ_ID_HEX_LEN) ? 0 : -1;
}

int
snj_tools_prepare(const struct snj_response_item *call, const struct snj_config *config,
                    char handle[SNJ_ID_HEX_LEN + 1u], uint32_t *yield_ms,
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
        for (size_t i = 0u; i < SNJ_MAX_PROCESSES; ++i)
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
        *rejected = snj_tool_result_not_run(reason);
        return *rejected && snj_tools_attach_output_limit(call, config, *rejected) == 0 ? 1 : -1;
    }
    if (!journal_write || !journal_read) {
        errno = EINVAL;
        return -1;
    }
    memcpy(handle, args.handle, SNJ_ID_HEX_LEN + 1u);
    *yield_ms = args.yield;
    return 0;
}

int
snj_tools_start(const struct snj_response_item *call, const struct snj_config *config,
                  const struct snj_credential *credential, json_t **result,
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
            *result = snj_tool_result_terminal(false, error[0] ? error : "Command could not start.");
            return *result ? 0 : -1;
        }
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
            if (len && snj_buf_append(&proc->input, args.input, len) < 0)
                return -1;
            proc->input_accepted_total += len;
            if (args.eof)
                proc->input_eof = true;
        }
    }
    return 0;
}

int
snj_tools_attach_output_limit(const struct snj_response_item *call,
                              const struct snj_config *config,
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
    if (result_set(result, "max_output_tokens",
                   json_integer((json_int_t)max_output_tokens)) < 0) {
        return -1;
    }
    return 0;
}

static int
wait_process(const char *handle, uint32_t yield_ms, snj_tool_pump_fn pump,
              void *opaque, int wake_fd, json_t **result,
              char *error, size_t error_size)
{
    uint64_t end = saturating_deadline(snj_monotonic_ms(), yield_ms);
    const char *reason = NULL;
    bool cancelled = false;
    while (!snj_tools_ready(handle)) {
        int control = pump ? pump(opaque, 0u) : 0;
        if (control < 0 || control == 2) {
            snj_tools_close_all(true);
            cancelled = true;
        } else if (control == 1 && !cancelled && !find_process(handle)->closing) {
            reason = "steering_handoff";
            break;
        }
        if (!cancelled && !find_process(handle)->closing &&
            (snj_tools_handoff(handle) || snj_monotonic_ms() >= end))
            break;
        if (snj_tools_service(10, wake_fd, error, error_size) < 0)
            return -1;
    }
    if (snj_tools_collect(handle, reason, result, error, error_size) < 0)
        return -1;
    return 0;
}

int
snj_tools_close_managed(const char *handle, bool user_interrupt,
                        snj_tool_pump_fn pump, void *pump_opaque, int wake_fd,
                        json_t **result, char *error, size_t error_size)
{
    struct managed_process *proc = find_process(handle);
    if (!proc) {
        *result = snj_tool_result_outcome_unknown("owner_lost");
        return *result ? 0 : -1;
    }
    begin_close(proc, user_interrupt);
    return wait_process(handle, 0u, pump, pump_opaque, wake_fd,
                         result, error, error_size);
}

int
snj_tools_run(const struct snj_response_item *call,
              const struct snj_config *config,
              const struct snj_credential *credential,
              const char *session_workspace,
              snj_tool_pump_fn pump, void *pump_opaque, int wake_fd,
              json_t **result, char *error, size_t error_size)
{
    char handle[SNJ_ID_HEX_LEN + 1u];
    uint32_t yield_ms;
    int rc;
    *result = NULL;
    if (!call || call->kind != SNJ_ITEM_TOOL_CALL || !config || !session_workspace)
        return -1;
    if (!strcmp(call->name, "apply_patch"))
        return snj_tools_apply_patch(call, session_workspace, result, error, error_size);
    rc = snj_tools_prepare(call, config, handle, &yield_ms, result);
    if (rc != 0)
        return rc < 0 ? -1 : 0;
    if (snj_tools_start(call, config, credential, result, error, error_size) < 0)
        return -1;
    if (!*result && wait_process(handle, yield_ms, pump, pump_opaque, wake_fd,
                                 result, error, error_size) < 0)
        return -1;
    snj_tools_collected(handle);
    return snj_tools_attach_output_limit(call, config, *result);
}

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
#include <stdarg.h>
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
#define SNJ_TOOL_STREAM_HEAD (64u * 1024u)
#define SNJ_TOOL_STREAM_TOTAL (256u * 1024u)
#define SNJ_TOOL_STREAM_TAIL (SNJ_TOOL_STREAM_TOTAL - SNJ_TOOL_STREAM_HEAD)
#define SNJ_TOOL_MODEL_TEXT_MAX (512u * 1024u)
#define SNJ_TOOL_POLL_MS 50u
#define SNJ_TOOL_CLOSE_GRACE_MS 2000u
#define SNJ_TOOL_REDACTOR_MAX (8192u + SNJ_WIRE_SECRET_MAX)

extern char **environ;

struct capture_stream {
    struct snj_buf head;
    struct snj_buf tail;
    uint64_t total;
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
    bool timed_out;
    bool cancelled;
    bool kill_sent;
    int child_status;
    uint64_t started_ms;
    uint64_t deadline_ms;
    struct managed_secret_set secrets;
    struct capture_stream stdout_stream;
    struct capture_stream stderr_stream;
    struct capture_redactor stdout_redactor;
    struct capture_redactor stderr_redactor;
};

static struct managed_process managed_process;
static bool managed_cleanup_registered;

static void
set_error(char *error, size_t size, const char *fmt, ...)
{
    va_list ap;
    if (!size)
        return;
    va_start(ap, fmt);
    (void)vsnprintf(error, size, fmt, ap);
    va_end(ap);
}

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
set_cloexec(int fd)
{
    int flags = fcntl(fd, F_GETFD);
    if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0)
        return -1;
    return 0;
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
    if (set_cloexec(p[0]) < 0 || set_cloexec(p[1]) < 0) {
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
    snj_buf_init(&stream->head, SNJ_TOOL_STREAM_HEAD);
    snj_buf_init(&stream->tail, SNJ_TOOL_STREAM_TAIL);
}

static void
capture_free(struct capture_stream *stream)
{
    snj_buf_free(&stream->head);
    snj_buf_free(&stream->tail);
    memset(stream, 0, sizeof(*stream));
}

static int
capture_tail_append(struct capture_stream *stream,
                    const unsigned char *data, size_t len)
{
    if (!len)
        return 0;
    if (len >= SNJ_TOOL_STREAM_TAIL) {
        snj_buf_reset(&stream->tail);
        return snj_buf_append(&stream->tail,
                              data + len - SNJ_TOOL_STREAM_TAIL,
                              SNJ_TOOL_STREAM_TAIL);
    }
    if (stream->tail.len + len > SNJ_TOOL_STREAM_TAIL) {
        size_t drop = stream->tail.len + len - SNJ_TOOL_STREAM_TAIL;
        memmove(stream->tail.data, stream->tail.data + drop,
                stream->tail.len - drop);
        stream->tail.len -= drop;
    }
    return snj_buf_append(&stream->tail, data, len);
}

static int
capture_append(struct capture_stream *stream, const unsigned char *data,
               size_t len)
{
    size_t first = 0;

    if (len && !data) {
        errno = EINVAL;
        return -1;
    }
    if (stream->total > UINT64_MAX - len) {
        errno = EOVERFLOW;
        return -1;
    }
    stream->total += len;
    if (stream->head.len < SNJ_TOOL_STREAM_HEAD) {
        first = SNJ_TOOL_STREAM_HEAD - stream->head.len;
        if (first > len)
            first = len;
        if (snj_buf_append(&stream->head, data, first) < 0)
            return -1;
    }
    return capture_tail_append(stream, data + first, len - first);
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

static const char b64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int
append_base64(struct snj_buf *out, const unsigned char *data, size_t len)
{
    size_t i = 0;
    while (i + 3u <= len) {
        unsigned int v = ((unsigned int)data[i] << 16) |
                         ((unsigned int)data[i + 1u] << 8) |
                         (unsigned int)data[i + 2u];
        unsigned char enc[4] = {
            (unsigned char)b64[(v >> 18) & 63u],
            (unsigned char)b64[(v >> 12) & 63u],
            (unsigned char)b64[(v >> 6) & 63u],
            (unsigned char)b64[v & 63u]
        };
        if (snj_buf_append(out, enc, sizeof(enc)) < 0)
            return -1;
        i += 3u;
    }
    if (i < len) {
        unsigned int v = (unsigned int)data[i] << 16;
        unsigned char enc[4];
        if (i + 1u < len)
            v |= (unsigned int)data[i + 1u] << 8;
        enc[0] = (unsigned char)b64[(v >> 18) & 63u];
        enc[1] = (unsigned char)b64[(v >> 12) & 63u];
        enc[2] = (i + 1u < len) ? (unsigned char)b64[(v >> 6) & 63u] : '=';
        enc[3] = '=';
        if (snj_buf_append(out, enc, sizeof(enc)) < 0)
            return -1;
    }
    return 0;
}

static bool
bytes_textual(const unsigned char *data, size_t len)
{
    return snj_utf8_valid(data, len, true);
}

static int
capture_retained(const struct capture_stream *stream, struct snj_buf *retained,
                 uint64_t *retained_bytes, uint64_t *discarded_bytes)
{
    uint64_t kept;
    uint64_t tail_start;
    size_t skip = 0u;

    snj_buf_reset(retained);
    if (snj_buf_append(retained, stream->head.data, stream->head.len) < 0)
        return -1;
    tail_start = stream->total >= stream->tail.len ?
                 stream->total - stream->tail.len : 0u;
    if (tail_start < stream->head.len) {
        uint64_t overlap = (uint64_t)stream->head.len - tail_start;
        skip = overlap > stream->tail.len ? stream->tail.len : (size_t)overlap;
    }
    if (skip < stream->tail.len &&
        snj_buf_append(retained, stream->tail.data + skip,
                       stream->tail.len - skip) < 0)
        return -1;
    kept = (uint64_t)retained->len;
    *retained_bytes = kept;
    *discarded_bytes = stream->total > kept ? stream->total - kept : 0u;
    return 0;
}

static json_t *
excerpt_json(const struct capture_stream *stream)
{
    struct snj_buf retained;
    struct snj_buf encoded;
    json_t *out = json_object();
    uint64_t kept = 0;
    uint64_t discarded = 0;
    bool textual;
    int rc = -1;

    if (!out)
        return NULL;
    snj_buf_init(&retained, SNJ_TOOL_STREAM_TOTAL);
    snj_buf_init(&encoded, SNJ_TOOL_STREAM_TOTAL * 2u);
    if (capture_retained(stream, &retained, &kept, &discarded) < 0)
        goto out;
    textual = bytes_textual(retained.data, retained.len);
    if (textual) {
        if (snj_buf_append(&encoded, retained.data, retained.len) < 0)
            goto out;
    } else if (append_base64(&encoded, retained.data, retained.len) < 0) {
        goto out;
    }
    if (snj_json_set_new(out, "discarded_bytes",
                         json_integer((json_int_t)discarded)) < 0 ||
        snj_json_set_new(out, "encoding",
                         json_string(textual ? "utf8" : "base64")) < 0 ||
        snj_json_set_new(out, "original_bytes",
                         json_integer((json_int_t)stream->total)) < 0 ||
        snj_json_set_new(out, "retained",
                         json_stringn(encoded.len ? (const char *)encoded.data : "",
                                      encoded.len)) < 0 ||
        snj_json_set_new(out, "retained_bytes",
                         json_integer((json_int_t)kept)) < 0)
        goto out;
    rc = 0;
out:
    snj_buf_free(&retained);
    snj_buf_free(&encoded);
    if (rc < 0) {
        json_decref(out);
        return NULL;
    }
    return out;
}

static int
append_utf8_excerpt(struct snj_buf *out, const char *label,
                    const struct capture_stream *stream)
{
    struct snj_buf retained;
    uint64_t kept = 0;
    uint64_t discarded = 0;
    int rc = -1;

    snj_buf_init(&retained, SNJ_TOOL_STREAM_TOTAL);
    if (capture_retained(stream, &retained, &kept, &discarded) < 0)
        goto out;
    if (retained.len == 0u && discarded == 0u) {
        rc = 0;
        goto out;
    }
    if (snj_buf_printf(out, "%s%s:\n", out->len ? "\n" : "", label) < 0)
        goto out;
    if (bytes_textual(retained.data, retained.len)) {
        if (snj_buf_append(out, retained.data, retained.len) < 0)
            goto out;
        if (retained.len && retained.data[retained.len - 1u] != '\n' &&
            snj_buf_putc(out, '\n') < 0)
            goto out;
    } else if (snj_buf_printf(out,
            "<%llu retained bytes are binary; see structured %s base64 excerpt>\n",
            (unsigned long long)kept, label) < 0) {
        goto out;
    }
    if (discarded && snj_buf_printf(out,
            "<%llu redacted/captured bytes omitted from middle>\n",
            (unsigned long long)discarded) < 0)
        goto out;
    rc = 0;
out:
    snj_buf_free(&retained);
    return rc;
}

static char *
model_text_for(const char *status, int exit_code, int signal_number,
               const struct capture_stream *stdout_stream,
               const struct capture_stream *stderr_stream)
{
    struct snj_buf text;
    char *out = NULL;

    snj_buf_init(&text, SNJ_TOOL_MODEL_TEXT_MAX);
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
    } else if (strcmp(status, "timed_out") == 0) {
        const char *msg = "Process timed out and was killed.\n";
        if (snj_buf_append(&text, msg, strlen(msg)) < 0)
            goto done;
    } else if (strcmp(status, "cancelled") == 0) {
        const char *msg = "Process was cancelled by the user.\n";
        if (snj_buf_append(&text, msg, strlen(msg)) < 0)
            goto done;
    } else if (strcmp(status, "running") == 0) {
        const char *msg = "Process is still running.\n";
        if (snj_buf_append(&text, msg, strlen(msg)) < 0)
            goto done;
    } else if (strcmp(status, "io_failed") == 0) {
        const char *msg = "Tool I/O failed.\n";
        if (snj_buf_append(&text, msg, strlen(msg)) < 0)
            goto done;
    } else if (snj_buf_printf(&text, "Tool status: %s.\n", status) < 0) {
        goto done;
    }
    if (append_utf8_excerpt(&text, "stdout", stdout_stream) < 0 ||
        append_utf8_excerpt(&text, "stderr", stderr_stream) < 0)
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
    model_text = model_text_for(status, exit_code, signal_number,
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
reap_child(pid_t pid, int *status, bool *done)
{
    pid_t got;

    for (;;) {
        got = waitpid(pid, status, WNOHANG);
        if (got == pid) {
            *done = true;
            return 0;
        }
        if (got == 0)
            return 0;
        if (got < 0 && errno == EINTR)
            continue;
        return -1;
    }
}

static int
drain_fd_common(int fd, struct capture_redactor *redactor, bool *open_flag,
                bool eio_is_eof)
{
    unsigned char buf[8192];

    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            if (redactor_feed(redactor, buf, (size_t)n) < 0)
                return -1;
            continue;
        }
        if (n == 0 || (n < 0 && errno == EIO && eio_is_eof)) {
            *open_flag = false;
            return redactor_finish(redactor);
        }
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;
        return -1;
    }
}

static int
drain_fd(int fd, struct capture_redactor *redactor, bool *open_flag)
{
    return drain_fd_common(fd, redactor, open_flag, false);
}

static int
drain_pty_fd(int fd, struct capture_redactor *redactor, bool *open_flag)
{
    return drain_fd_common(fd, redactor, open_flag, true);
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

static json_t *
simple_result(const char *status, const char *reason, const char *message,
              int exit_code);

static uint64_t
saturating_deadline(uint64_t start, uint32_t delta_ms)
{
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

static void
managed_clear_fds(struct managed_process *proc)
{
    if (proc->pty) {
        close_if_open(&proc->stdout_fd);
        proc->stdin_fd = -1;
    } else {
        close_if_open(&proc->stdin_fd);
        close_if_open(&proc->stdout_fd);
        close_if_open(&proc->stderr_fd);
    }
    proc->stdin_open = false;
    proc->stdout_open = false;
    proc->stderr_open = false;
}

static void
managed_close_input(struct managed_process *proc)
{
    if (!proc->pty)
        close_if_open(&proc->stdin_fd);
    proc->stdin_open = false;
}

static void
managed_reset_storage(struct managed_process *proc)
{
    redactor_free(&proc->stderr_redactor);
    redactor_free(&proc->stdout_redactor);
    capture_free(&proc->stderr_stream);
    capture_free(&proc->stdout_stream);
    memset(proc, 0, sizeof(*proc));
    proc->stdin_fd = -1;
    proc->stdout_fd = -1;
    proc->stderr_fd = -1;
}

static void
managed_cleanup(void)
{
    struct managed_process *proc = &managed_process;

    if (!proc->active)
        return;
    if (!proc->child_done)
        kill_child_group(proc->pid, SIGKILL);
    managed_clear_fds(proc);
    if (!proc->child_done) {
        while (waitpid(proc->pid, NULL, 0) < 0 && errno == EINTR)
            ;
    }
    managed_reset_storage(proc);
}

static void
managed_cleanup_at_exit(void)
{
    managed_cleanup();
}

static void
managed_register_cleanup(void)
{
    if (!managed_cleanup_registered) {
        if (atexit(managed_cleanup_at_exit) == 0)
            managed_cleanup_registered = true;
    }
}

static int
managed_reap_once(struct managed_process *proc, char *error, size_t error_size)
{
    if (!proc->child_done && reap_child(proc->pid, &proc->child_status,
                                        &proc->child_done) < 0) {
        if (errno == ECHILD) {
            proc->child_done = true;
        } else {
            set_error(error, error_size, "tool process wait failed: %s",
                      strerror(errno));
            return -1;
        }
    }
    return 0;
}

static const char *
managed_terminal_status(const struct managed_process *proc, const char **reason,
                        int *exit_code, int *signal_number)
{
    *reason = NULL;
    *exit_code = -1;
    *signal_number = -1;
    if (proc->cancelled) {
        *reason = "turn_cancelled";
        return "cancelled";
    }
    if (proc->timed_out)
        return "timed_out";
    if (WIFEXITED(proc->child_status)) {
        *exit_code = WEXITSTATUS(proc->child_status);
        return *exit_code == 0 ? "succeeded" : "failed";
    }
    if (WIFSIGNALED(proc->child_status)) {
        *signal_number = WTERMSIG(proc->child_status);
        return "signaled";
    }
    return "failed";
}

static int
managed_make_result(struct managed_process *proc, const char *status,
                    const char *reason, int exit_code, int signal_number,
                    const char *handle, json_t **result,
                    char *error, size_t error_size)
{
    uint64_t ended = snj_time_ms();
    uint64_t duration = ended >= proc->started_ms ? ended - proc->started_ms : 0u;

    *result = result_json(status, reason, exit_code, signal_number, duration,
                          handle, &proc->stdout_stream, &proc->stderr_stream);
    if (!*result) {
        set_error(error, error_size, "cannot allocate tool result");
        return -1;
    }
    return 0;
}

static int
managed_return_running(struct managed_process *proc, json_t **result,
                       char *error, size_t error_size)
{
    return managed_make_result(proc, "running", NULL, -1, -1, proc->handle,
                               result, error, error_size);
}

static int
managed_return_terminal(struct managed_process *proc, json_t **result,
                        char *error, size_t error_size)
{
    const char *reason;
    const char *status;
    int exit_code;
    int signal_number;
    int rc;

    status = managed_terminal_status(proc, &reason, &exit_code, &signal_number);
    rc = managed_make_result(proc, status, reason, exit_code, signal_number,
                             NULL, result, error, error_size);
    managed_reset_storage(proc);
    return rc;
}

static bool
managed_input_done(const struct managed_process *proc, bool input_supplied,
                   size_t input_len, size_t input_written,
                   bool close_after_input, bool pty_eof_sent)
{
    if (!input_supplied || !proc->stdin_open)
        return true;
    if (input_written < input_len)
        return false;
    return !proc->pty || !close_after_input || pty_eof_sent;
}

static int
managed_write_pty_input(struct managed_process *proc, const char *input,
                        size_t input_len, size_t *input_written,
                        bool close_after_input, bool *pty_eof_sent)
{
    if (*input_written < input_len) {
        if (write_stdin_chunk(&proc->stdin_fd, input, input_len,
                              input_written, &proc->stdin_open, false) < 0)
            return -1;
        if (!proc->stdin_open || *input_written < input_len)
            return 0;
    }
    if (close_after_input && !*pty_eof_sent && proc->stdin_open) {
        static const char eot[] = "\004";
        size_t written = 0u;

        if (write_stdin_chunk(&proc->stdin_fd, eot, 1u, &written,
                              &proc->stdin_open, false) < 0)
            return -1;
        if (written == 1u) {
            *pty_eof_sent = true;
            proc->stdin_open = false;
        }
    }
    return 0;
}

static int
managed_drive(struct managed_process *proc, const char *input, size_t input_len,
              bool input_supplied, bool close_after_input, uint32_t yield_ms,
              snj_tool_pump_fn pump, void *pump_opaque,
              json_t **result, char *error, size_t error_size)
{
    uint64_t yield_deadline = yield_ms ? saturating_deadline(snj_time_ms(), yield_ms) : UINT64_MAX;
    size_t input_written = 0u;
    bool yield_enabled = yield_ms > 0u;
    bool pty_eof_sent = !close_after_input;

    if (input_supplied && input_len == 0u && close_after_input &&
        proc->stdin_open && !proc->pty)
        managed_close_input(proc);
    for (;;) {
        struct pollfd fds[3];
        nfds_t nfds = 0;
        int stdout_index = -1;
        int stderr_index = -1;
        int stdin_index = -1;
        uint64_t now = snj_time_ms();
        int timeout = (int)SNJ_TOOL_POLL_MS;
        bool input_done = managed_input_done(proc, input_supplied,
                                             input_len, input_written,
                                             close_after_input, pty_eof_sent);
        bool input_pending = input_supplied && proc->stdin_open && !input_done;
        int pr;

        if (proc->pty && proc->stdout_open)
            pty_apply_current_size(proc->stdout_fd, &proc->pty_rows,
                                   &proc->pty_cols);
        if (!proc->child_done && !proc->timed_out && now >= proc->deadline_ms) {
            proc->timed_out = true;
            proc->kill_sent = true;
            kill_child_group(proc->pid, SIGKILL);
        }
        if (pump && !proc->child_done) {
            int pump_rc = pump(pump_opaque, 0u);
            if (pump_rc < 0 || pump_rc == 2) {
                proc->cancelled = true;
                if (!proc->kill_sent) {
                    proc->kill_sent = true;
                    kill_child_group(proc->pid, SIGKILL);
                }
            }
        }
        if (managed_reap_once(proc, error, error_size) < 0)
            return -1;
        if (proc->child_done && !proc->stdout_open && !proc->stderr_open)
            return managed_return_terminal(proc, result, error, error_size);
        if (yield_enabled && now >= yield_deadline && input_done &&
            !proc->child_done && !proc->timed_out && !proc->cancelled)
            return managed_return_running(proc, result, error, error_size);

        if (proc->stdout_open) {
            stdout_index = (int)nfds;
            fds[nfds].fd = proc->stdout_fd;
            fds[nfds].events = POLLIN | POLLHUP;
            if (proc->pty && input_pending)
                fds[nfds].events |= POLLOUT;
            fds[nfds].revents = 0;
            ++nfds;
        }
        if (proc->stderr_open) {
            stderr_index = (int)nfds;
            fds[nfds].fd = proc->stderr_fd;
            fds[nfds].events = POLLIN | POLLHUP;
            fds[nfds].revents = 0;
            ++nfds;
        }
        if (!proc->pty && input_pending && input_written < input_len) {
            stdin_index = (int)nfds;
            fds[nfds].fd = proc->stdin_fd;
            fds[nfds].events = POLLOUT | POLLHUP;
            fds[nfds].revents = 0;
            ++nfds;
        } else if (!proc->pty && input_supplied && proc->stdin_open &&
                   close_after_input && input_written >= input_len) {
            managed_close_input(proc);
            input_done = true;
        }
        if (!proc->timed_out && !proc->cancelled && proc->deadline_ms > now) {
            uint64_t remain = proc->deadline_ms - now;
            if (remain < (uint64_t)timeout)
                timeout = (int)remain;
        }
        if (yield_enabled && yield_deadline > now) {
            uint64_t remain = yield_deadline - now;
            if (remain < (uint64_t)timeout)
                timeout = (int)remain;
        } else if (yield_enabled && input_done) {
            timeout = 0;
        }
        pr = poll(nfds ? fds : NULL, nfds, timeout);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            set_error(error, error_size, "tool process polling failed: %s",
                      strerror(errno));
            return -1;
        }
        if (stdout_index >= 0 && fds[stdout_index].revents) {
            short revents = fds[stdout_index].revents;

            if (proc->pty && (revents & POLLOUT) && input_pending) {
                if (managed_write_pty_input(proc, input, input_len,
                                            &input_written, close_after_input,
                                            &pty_eof_sent) < 0) {
                    set_error(error, error_size, "tool PTY stdin write failed");
                    return -1;
                }
            }
            if (revents & (POLLIN | POLLHUP | POLLERR)) {
                int dr = proc->pty ?
                         drain_pty_fd(proc->stdout_fd, &proc->stdout_redactor,
                                      &proc->stdout_open) :
                         drain_fd(proc->stdout_fd, &proc->stdout_redactor,
                                  &proc->stdout_open);
                if (dr < 0) {
                    set_error(error, error_size,
                              proc->pty ? "tool PTY capture failed" :
                                          "tool stdout capture failed");
                    return -1;
                }
            }
            if (!proc->stdout_open) {
                close_if_open(&proc->stdout_fd);
                if (proc->pty) {
                    proc->stdin_fd = -1;
                    proc->stdin_open = false;
                }
            }
        }
        if (stderr_index >= 0 && fds[stderr_index].revents) {
            if (drain_fd(proc->stderr_fd, &proc->stderr_redactor,
                         &proc->stderr_open) < 0) {
                set_error(error, error_size, "tool stderr capture failed");
                return -1;
            }
            if (!proc->stderr_open)
                close_if_open(&proc->stderr_fd);
        }
        if (stdin_index >= 0 && fds[stdin_index].revents) {
            if (write_stdin_chunk(&proc->stdin_fd, input, input_len,
                                  &input_written, &proc->stdin_open,
                                  close_after_input) < 0) {
                set_error(error, error_size, "tool stdin write failed");
                return -1;
            }
        }
        if ((proc->timed_out || proc->cancelled) && !proc->kill_sent) {
            proc->kill_sent = true;
            kill_child_group(proc->pid, SIGKILL);
        }
    }
}

static int
run_exec_command_managed(const char *command, const char *workdir,
                         const char *stdin_text, uint32_t timeout_ms,
                         uint32_t yield_ms, bool pty,
                         const struct snj_config *config,
                         const struct snj_credential *credential,
                         snj_tool_pump_fn pump, void *pump_opaque,
                         json_t **result, char *error, size_t error_size)
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
    struct managed_process *proc = &managed_process;
    size_t stdin_len = stdin_text ? strlen(stdin_text) : 0u;
    int rc = -1;

    if (proc->active) {
        *result = simple_result("not_run", "managed_process_conflict",
            "Another managed process is still running; close or finish it before starting a new yielded process.", -1);
        if (!*result) {
            set_error(error, error_size, "cannot allocate tool result");
            return -1;
        }
        return 0;
    }
    managed_register_cleanup();
    if (pty) {
        if (open_pty_pair(&pty_master, &pty_slave,
                          &pty_rows, &pty_cols) < 0) {
            set_error(error, error_size, "cannot create tool PTY: %s",
                      strerror(errno));
            goto out;
        }
        if (set_cloexec(pty_master) < 0 || set_cloexec(pty_slave) < 0 ||
            set_nonblock(pty_master) < 0) {
            set_error(error, error_size, "cannot configure tool PTY: %s",
                      strerror(errno));
            goto out;
        }
    } else {
        if (make_pipe(in_pipe) < 0 || make_pipe(out_pipe) < 0 ||
            make_pipe(err_pipe) < 0) {
            set_error(error, error_size, "cannot create tool pipes: %s",
                      strerror(errno));
            goto out;
        }
        if (set_nonblock(in_pipe[1]) < 0 || set_nonblock(out_pipe[0]) < 0 ||
            set_nonblock(err_pipe[0]) < 0) {
            set_error(error, error_size, "cannot configure tool pipes: %s",
                      strerror(errno));
            goto out;
        }
    }
    env = filtered_environment(config);
    if (!env) {
        set_error(error, error_size, "cannot allocate tool environment");
        goto out;
    }
    pid = fork();
    if (pid < 0) {
        set_error(error, error_size, "cannot fork tool process: %s", strerror(errno));
        goto out;
    }
    if (pid == 0) {
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

    memset(proc, 0, sizeof(*proc));
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
    proc->started_ms = snj_time_ms();
    proc->deadline_ms = saturating_deadline(proc->started_ms, timeout_ms);
    if (snj_random_id(proc->handle) < 0) {
        set_error(error, error_size, "cannot allocate managed process handle");
        goto out_active;
    }
    managed_secret_set_build(&proc->secrets, config, credential);
    capture_init(&proc->stdout_stream);
    capture_init(&proc->stderr_stream);
    redactor_init(&proc->stdout_redactor, &proc->stdout_stream,
                  &proc->secrets.wire);
    redactor_init(&proc->stderr_redactor, &proc->stderr_stream,
                  &proc->secrets.wire);
    free(env);
    env = NULL;
    rc = managed_drive(proc, stdin_text ? stdin_text : "", stdin_len,
                       stdin_text != NULL, true, yield_ms, pump, pump_opaque,
                       result, error, error_size);
    if (rc < 0)
        goto out_active;
    return rc;

out_active:
    managed_cleanup();
out:
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

static int
run_write_stdin(const struct snj_response_item *call,
                const struct snj_config *config,
                snj_tool_pump_fn pump, void *pump_opaque,
                json_t **result, char *error, size_t error_size)
{
    const char *handle = snj_json_string(call->arguments, "handle");
    const char *data = snj_json_string(call->arguments, "data");
    uint32_t yield_ms;
    bool eof;
    struct managed_process *proc = &managed_process;
    size_t len;

    if (!handle || !snj_hex_is_lower(handle, SNJ_ID_HEX_LEN) ||
        !text_arg_valid(data, SNJ_TOOL_STDIN_MAX) ||
        !json_bool_member(call->arguments, "eof", false, &eof) ||
        !json_u32_member(call->arguments, "yield_ms", config->default_yield_ms,
                         0u, config->max_timeout_ms, &yield_ms)) {
        set_error(error, error_size, "invalid write_stdin arguments");
        errno = EINVAL;
        return -1;
    }
    if (!proc->active || strcmp(proc->handle, handle) != 0) {
        *result = simple_result("failed", NULL,
            "No active managed process matches the supplied handle.", 1);
        if (!*result) {
            set_error(error, error_size, "cannot allocate tool result");
            return -1;
        }
        return 0;
    }
    len = strlen(data);
    if (!proc->stdin_open && len > 0u)
        return managed_drive(proc, "", 0u, false, false, yield_ms, pump,
                             pump_opaque, result, error, error_size);
    return managed_drive(proc, data, len, true, eof, yield_ms, pump,
                         pump_opaque, result, error, error_size);
}

static int
run_exec_command(const struct snj_response_item *call,
                 const struct snj_config *config,
                 const struct snj_credential *credential,
                 const char *session_workspace,
                 snj_tool_pump_fn pump, void *pump_opaque,
                 json_t **result, char *error, size_t error_size)
{
    const char *command = snj_json_string(call->arguments, "command");
    const char *workdir = snj_json_string(call->arguments, "workdir");
    const char *stdin_text = json_nullable_string(call->arguments, "stdin");
    uint32_t timeout_ms;
    uint32_t yield_ms;
    bool pty;
    int in_pipe[2] = {-1, -1};
    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    int pty_master = -1;
    int pty_slave = -1;
    unsigned short pty_rows = 24;
    unsigned short pty_cols = 80;
    char **env = NULL;
    pid_t pid = -1;
    uint64_t started = snj_time_ms();
    uint64_t deadline;
    bool stdout_open = true;
    bool stderr_open = true;
    bool stdin_open = true;
    bool child_done = false;
    bool timed_out = false;
    bool cancelled = false;
    bool kill_sent = false;
    size_t stdin_written = 0;
    int child_status = 0;
    int rc = -1;
    struct capture_stream stdout_stream;
    struct capture_stream stderr_stream;
    struct capture_redactor stdout_redactor;
    struct capture_redactor stderr_redactor;
    struct snj_secret_set secret_set;

    snj_secret_set_build(&secret_set, config, credential);
    capture_init(&stdout_stream);
    capture_init(&stderr_stream);
    redactor_init(&stdout_redactor, &stdout_stream, &secret_set.wire);
    redactor_init(&stderr_redactor, &stderr_stream, &secret_set.wire);
    (void)session_workspace;
    if (!text_arg_valid(command, SNJ_TOOL_COMMAND_MAX) ||
        (stdin_text && !text_arg_valid(stdin_text, SNJ_TOOL_STDIN_MAX)) ||
        !absolute_dir_arg_valid(workdir) ||
        !json_bool_member(call->arguments, "pty", false, &pty) ||
        !json_u32_member(call->arguments, "timeout_ms",
                         config->default_timeout_ms, 1u,
                         config->max_timeout_ms, &timeout_ms) ||
        !json_u32_member(call->arguments, "yield_ms",
                         config->default_yield_ms, 0u,
                         timeout_ms, &yield_ms)) {
        set_error(error, error_size, "invalid exec_command arguments");
        errno = EINVAL;
        goto out;
    }
    if (yield_ms > 0u) {
        redactor_free(&stderr_redactor);
        redactor_free(&stdout_redactor);
        capture_free(&stdout_stream);
        capture_free(&stderr_stream);
        return run_exec_command_managed(command, workdir, stdin_text,
                                        timeout_ms, yield_ms, pty, config,
                                        credential, pump, pump_opaque,
                                        result, error, error_size);
    }
    if (pty) {
        if (open_pty_pair(&pty_master, &pty_slave,
                          &pty_rows, &pty_cols) < 0) {
            set_error(error, error_size, "cannot create tool PTY: %s", strerror(errno));
            goto out;
        }
        if (set_cloexec(pty_master) < 0 || set_cloexec(pty_slave) < 0 ||
            set_nonblock(pty_master) < 0) {
            set_error(error, error_size, "cannot configure tool PTY: %s", strerror(errno));
            goto out;
        }
        stderr_open = false;
    } else {
        if (make_pipe(in_pipe) < 0 || make_pipe(out_pipe) < 0 ||
            make_pipe(err_pipe) < 0) {
            set_error(error, error_size, "cannot create tool pipes: %s", strerror(errno));
            goto out;
        }
        if (set_nonblock(in_pipe[1]) < 0 || set_nonblock(out_pipe[0]) < 0 ||
            set_nonblock(err_pipe[0]) < 0) {
            set_error(error, error_size, "cannot configure tool pipes: %s", strerror(errno));
            goto out;
        }
    }
    env = filtered_environment(config);
    if (!env) {
        set_error(error, error_size, "cannot allocate tool environment");
        goto out;
    }
    pid = fork();
    if (pid < 0) {
        set_error(error, error_size, "cannot fork tool process: %s", strerror(errno));
        goto out;
    }
    if (pid == 0) {
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
    if (!pty)
        (void)setpgid(pid, pid);
    if (pty) {
        close_if_open(&pty_slave);
    } else {
        close_if_open(&in_pipe[0]);
        close_if_open(&out_pipe[1]);
        close_if_open(&err_pipe[1]);
    }
    deadline = started + timeout_ms;
    if (deadline < started)
        deadline = UINT64_MAX;
    while (stdout_open || stderr_open || !child_done) {
        struct pollfd fds[3];
        nfds_t nfds = 0;
        uint64_t now = snj_time_ms();
        int timeout = (int)SNJ_TOOL_POLL_MS;
        int pump_rc;

        if (!timed_out && now >= deadline) {
            timed_out = true;
            kill_sent = true;
            kill_child_group(pid, SIGKILL);
        }
        if (pump && !child_done) {
            pump_rc = pump(pump_opaque, 0u);
            if (pump_rc < 0) {
                cancelled = true;
                kill_sent = true;
                kill_child_group(pid, SIGKILL);
            } else if (pump_rc == 2) {
                cancelled = true;
                kill_sent = true;
                kill_child_group(pid, SIGKILL);
            }
        }
        if (pty && stdout_open)
            pty_apply_current_size(pty_master, &pty_rows, &pty_cols);
        if (stdout_open) {
            fds[nfds].fd = pty ? pty_master : out_pipe[0];
            fds[nfds].events = POLLIN | POLLHUP;
            if (pty && stdin_open && stdin_text && stdin_written < strlen(stdin_text))
                fds[nfds].events |= POLLOUT;
            fds[nfds].revents = 0;
            ++nfds;
        }
        if (stderr_open) {
            fds[nfds].fd = err_pipe[0];
            fds[nfds].events = POLLIN | POLLHUP;
            fds[nfds].revents = 0;
            ++nfds;
        }
        if (!pty && stdin_open && stdin_text &&
            stdin_written < strlen(stdin_text)) {
            fds[nfds].fd = in_pipe[1];
            fds[nfds].events = POLLOUT | POLLHUP;
            fds[nfds].revents = 0;
            ++nfds;
        } else if (!pty && stdin_open) {
            close_if_open(&in_pipe[1]);
            stdin_open = false;
        } else if (pty && stdin_open &&
                   (!stdin_text || stdin_written >= strlen(stdin_text))) {
            stdin_open = false;
        }
        if (!timed_out && !cancelled && deadline > now) {
            uint64_t remain = deadline - now;
            if (remain < (uint64_t)timeout)
                timeout = (int)remain;
        }
        {
            int pr = poll(nfds ? fds : NULL, nfds, timeout);
            if (pr < 0) {
                if (errno == EINTR)
                    continue;
                set_error(error, error_size, "tool process polling failed: %s",
                          strerror(errno));
                goto out;
            }
        }
        if (nfds) {
            nfds_t at = 0;
            if (stdout_open) {
                short revents = fds[at].revents;
                if (pty && stdin_open && stdin_text &&
                    stdin_written < strlen(stdin_text) && (revents & POLLOUT)) {
                    if (write_stdin_chunk(&pty_master, stdin_text,
                                          strlen(stdin_text), &stdin_written,
                                          &stdin_open, false) < 0) {
                        set_error(error, error_size, "tool PTY stdin write failed");
                        goto out;
                    }
                }
                if (revents & (POLLIN | POLLHUP | POLLERR)) {
                    if ((pty ? drain_pty_fd(pty_master, &stdout_redactor,
                                            &stdout_open) :
                               drain_fd(out_pipe[0], &stdout_redactor,
                                        &stdout_open)) < 0) {
                        set_error(error, error_size,
                                  pty ? "tool PTY capture failed" :
                                        "tool stdout capture failed");
                        goto out;
                    }
                }
                ++at;
            }
            if (stderr_open) {
                if (fds[at].revents)
                    if (drain_fd(err_pipe[0], &stderr_redactor,
                                 &stderr_open) < 0) {
                        set_error(error, error_size, "tool stderr capture failed");
                        goto out;
                    }
                ++at;
            }
            if (!pty && stdin_open && stdin_text &&
                stdin_written < strlen(stdin_text)) {
                if (fds[at].revents)
                    if (write_stdin_chunk(&in_pipe[1], stdin_text,
                                          strlen(stdin_text), &stdin_written,
                                          &stdin_open, true) < 0) {
                        set_error(error, error_size, "tool stdin write failed");
                        goto out;
                    }
            }
        }
        if (!stdout_open) {
            if (pty)
                close_if_open(&pty_master);
            else
                close_if_open(&out_pipe[0]);
        }
        if (!stderr_open)
            close_if_open(&err_pipe[0]);
        if (!child_done && reap_child(pid, &child_status, &child_done) < 0) {
            if (errno == ECHILD)
                child_done = true;
            else {
                set_error(error, error_size, "tool process wait failed: %s",
                          strerror(errno));
                goto out;
            }
        }
        if ((timed_out || cancelled) && !kill_sent) {
            kill_sent = true;
            kill_child_group(pid, SIGKILL);
        }
    }
    {
        const char *status = "failed";
        const char *reason = NULL;
        int exit_code = -1;
        int signal_number = -1;
        uint64_t ended = snj_time_ms();
        uint64_t duration = ended >= started ? ended - started : 0u;

        if (cancelled) {
            status = "cancelled";
            reason = "turn_cancelled";
        } else if (timed_out) {
            status = "timed_out";
        } else if (WIFEXITED(child_status)) {
            exit_code = WEXITSTATUS(child_status);
            status = exit_code == 0 ? "succeeded" : "failed";
        } else if (WIFSIGNALED(child_status)) {
            signal_number = WTERMSIG(child_status);
            status = "signaled";
        }
        *result = result_json(status, reason, exit_code, signal_number, duration,
                              NULL, &stdout_stream, &stderr_stream);
        if (!*result) {
            set_error(error, error_size, "cannot allocate tool result");
            goto out;
        }
    }
    rc = cancelled ? 2 : 0;
out:
    if (pid > 0 && rc < 0) {
        kill_child_group(pid, SIGKILL);
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
            ;
    }
    close_if_open(&in_pipe[0]);
    close_if_open(&in_pipe[1]);
    close_if_open(&out_pipe[0]);
    close_if_open(&out_pipe[1]);
    close_if_open(&err_pipe[0]);
    close_if_open(&err_pipe[1]);
    close_if_open(&pty_master);
    close_if_open(&pty_slave);
    free(env);
    redactor_free(&stderr_redactor);
    redactor_free(&stdout_redactor);
    capture_free(&stdout_stream);
    capture_free(&stderr_stream);
    return rc;
}

static json_t *
simple_result(const char *status, const char *reason, const char *message,
              int exit_code)
{
    struct capture_stream out;
    struct capture_stream err;
    json_t *result;

    capture_init(&out);
    capture_init(&err);
    (void)capture_append(&err, (const unsigned char *)message, strlen(message));
    result = result_json(status, reason, exit_code, -1, 0u, NULL, &out, &err);
    capture_free(&out);
    capture_free(&err);
    return result;
}

int
snj_tools_close_managed(const char *handle, bool user_interrupt,
                        snj_tool_pump_fn pump, void *pump_opaque,
                        json_t **result, char *error, size_t error_size)
{
    struct managed_process *proc = &managed_process;
    uint64_t now;
    uint64_t close_deadline;

    if (result)
        *result = NULL;
    if (!handle || !snj_hex_is_lower(handle, SNJ_ID_HEX_LEN) || !result) {
        set_error(error, error_size, "invalid managed process closure");
        errno = EINVAL;
        return -1;
    }
    if (!proc->active || strcmp(proc->handle, handle) != 0) {
        *result = snj_tool_result_outcome_unknown("owner_lost");
        if (!*result) {
            set_error(error, error_size, "cannot allocate process closure result");
            return -1;
        }
        return 0;
    }
    managed_close_input(proc);
    if (user_interrupt)
        proc->cancelled = true;
    if (!proc->child_done && !proc->kill_sent) {
        proc->kill_sent = true;
        kill_child_group(proc->pid, user_interrupt ? SIGINT : SIGTERM);
    }
    now = snj_time_ms();
    close_deadline = saturating_deadline(now, SNJ_TOOL_CLOSE_GRACE_MS);
    if (proc->deadline_ms > close_deadline)
        proc->deadline_ms = close_deadline;
    if (managed_drive(proc, "", 0u, false, false, 0u, pump, pump_opaque,
                      result, error, error_size) < 0) {
        managed_cleanup();
        return -1;
    }
    return 0;
}

int
snj_tools_run(const struct snj_response_item *call,
              const struct snj_config *config,
              const struct snj_credential *credential,
              const char *session_workspace,
              snj_tool_pump_fn pump, void *pump_opaque,
              json_t **result, char *error, size_t error_size)
{
    if (result)
        *result = NULL;
    if (!call || call->kind != SNJ_ITEM_TOOL_CALL || !config ||
        !session_workspace || !result) {
        set_error(error, error_size, "invalid tool invocation");
        errno = EINVAL;
        return -1;
    }
    if (strcmp(call->name, "exec_command") == 0)
        return run_exec_command(call, config, credential, session_workspace,
                                pump, pump_opaque, result,
                                error, error_size);
    if (strcmp(call->name, "write_stdin") == 0)
        return run_write_stdin(call, config, pump, pump_opaque, result,
                               error, error_size);
    if (strcmp(call->name, "apply_patch") == 0)
        return snj_tools_apply_patch(call, session_workspace, result,
                                     error, error_size);
    set_error(error, error_size, "unknown tool name");
    errno = EINVAL;
    return -1;
}

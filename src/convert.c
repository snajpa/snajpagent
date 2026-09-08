/* SPDX-License-Identifier: GPL-2.0-only */
#include "convert.h"
#include "process_host.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *
find_converter(const char *name)
{
    char *path = snag_environment("PATH"), *found = NULL;
#ifdef _WIN32
    const char separator = ';';
    const char *suffix = ".exe";
#else
    const char separator = ':';
    const char *suffix = "";
#endif
    if (!path || strlen(path) > SNAG_PATH_MAX_BYTES) { free(path); errno = ENOENT; return NULL; }
    for (char *p = path; p;) {
        char *next = strchr(p, separator);
        if (next) *next++ = '\0';
        if (snag_path_root_len(p)) {
            struct snag_buf full;
            snag_buf_init(&full, SNAG_PATH_MAX_BYTES);
            if (snag_buf_printf(&full, "%s/%s%s", p, name, suffix) == 0 &&
                snag_buf_terminate(&full) == 0 && snag_file_executable((char *)full.data) == 0)
                found = snag_realpath((char *)full.data);
            snag_buf_free(&full);
            if (found) break;
        }
        p = next;
    }
    free(path);
    if (!found) errno = ENOENT;
    return found;
}

static int
convert_run(const char *const *argv, const char *workdir, struct snag_buf *output,
                     struct snag_buf *trace, bool internal, int (*pump)(void *, unsigned int),
                     void *opaque, snag_wake_fd wake, char *error, size_t error_size)
{
    char *executable = NULL, *environment[10] = {0};
    const char *args[64];
    size_t argc = 0, envc = 0, original = output->len;
    struct snag_child child;
    struct snag_buf diagnostic;
    bool started = false, open[2] = {true, true};
    int rc = -1;
    uint64_t deadline = snag_monotonic_ms() + 60000u;
    snag_child_init(&child);
    snag_buf_init(&diagnostic, trace ? 64u * 1024u : 4096u);
    if (error_size) error[0] = '\0';
    while (argv && argv[argc] && argc < 63u) { args[argc] = argv[argc]; ++argc; }
    if (!argc || argv[argc] || !workdir || !*argv[0] || (!internal && (strchr(argv[0], '/') ||
        strchr(argv[0], '\\'))) || output->max > 32u * 1024u * 1024u) {
        snag_errorf(error, error_size, "invalid bounded conversion request"); goto out;
    }
    args[argc] = NULL;
    executable = internal ? snag_realpath(argv[0]) : find_converter(argv[0]);
    if (!executable) {
        snag_errorf(error, error_size, "Converter %s is not installed in PATH", argv[0]); goto out;
    }
    args[0] = executable;
    const char *inherit[] = {"PATH", "SystemRoot", "WINDIR", NULL};
    for (size_t i = 0; inherit[i]; ++i) {
        char *value = snag_environment(inherit[i]);
        if (!value) continue;
        struct snag_buf entry;
        snag_buf_init(&entry, SNAG_PATH_MAX_BYTES + 64u);
        int added = snag_buf_printf(&entry, "%s=%s", inherit[i], value);
        free(value);
        if (added < 0 || snag_buf_terminate(&entry) < 0) { snag_buf_free(&entry); goto out; }
        environment[envc++] = (char *)entry.data;
    }
    const char *private_vars[] = {"HOME", "TMPDIR", "TMP", "TEMP", NULL};
    for (size_t i = 0; private_vars[i]; ++i) {
        struct snag_buf entry;
        snag_buf_init(&entry, SNAG_PATH_MAX_BYTES + 64u);
        if (snag_buf_printf(&entry, "%s=%s", private_vars[i], workdir) < 0 ||
            snag_buf_terminate(&entry) < 0) { snag_buf_free(&entry); goto out; }
        environment[envc++] = (char *)entry.data;
    }
    environment[envc++] = snag_strdup_checked("LC_ALL=C", 32u);
    if (!environment[envc - 1u]) goto out;
    if (pump && (rc = pump(opaque, 0u)) != 0) goto interrupted;
    rc = -1;
    if (snag_child_spawn_argv(&child, args, workdir, environment) < 0) {
        snag_errorf(error, error_size, "Cannot start %s: %s", argv[0], strerror(errno)); goto out;
    }
    started = true;
    snag_child_close_stream(&child, 2u);
    for (;;) {
        struct snag_child_event events[2];
        size_t n = 0;
        if (pump && (rc = pump(opaque, 0u)) != 0) goto interrupted;
        rc = -1;
        if (snag_monotonic_ms() >= deadline) {
            snag_errorf(error, error_size, "%s exceeded the 60-second conversion limit", argv[0]); goto out;
        }
        for (unsigned int i = 0; i < 2u; ++i)
            if (open[i]) events[n++] = (struct snag_child_event){&child, i, SNAG_CHILD_READ, 0};
        int exited = snag_child_exited(&child);
        if (exited < 0) goto out;
        if (!n && exited) break;
        if (snag_child_wait(events, n, wake, 20) < 0 && errno != EINTR) goto out;
        for (size_t i = 0; i < n; ++i) {
            if (!events[i].revents) continue;
            unsigned char buffer[32768];
            unsigned int stream = events[i].stream;
            ssize_t got = snag_child_read(&child, stream, buffer, sizeof(buffer));
            if (!got) { open[stream] = false; snag_child_close_stream(&child, stream); }
            else if (got < 0) { if (errno != EAGAIN && errno != EINTR) goto out; }
            else if (!stream) {
                if (snag_buf_append(output, buffer, (size_t)got) < 0) {
                    snag_errorf(error, error_size, "%s output exceeds the operation byte limit", argv[0]); goto out;
                }
            } else {
                size_t take = (size_t)got;
                if (take > diagnostic.max - diagnostic.len - 1u) take = diagnostic.max - diagnostic.len - 1u;
                if (snag_buf_append(&diagnostic, buffer, take) < 0) goto out;
            }
        }
    }
    /* The unreaped leader still owns its PID/group. Close any child descendants
     * before reaping so a converter cannot leave a background task behind. */
    snag_child_signal(&child, SNAG_CHILD_KILL);
    if (snag_child_reap(&child) < 0) goto out;
    if (child.exit_code != 0) {
        (void)snag_buf_terminate(&diagnostic);
        snag_errorf(error, error_size, "%s failed (exit %lld, signal %d): %.160s", argv[0],
            (long long)child.exit_code, child.signal_number, diagnostic.data ? (char *)diagnostic.data : "");
        goto out;
    }
    if (trace && snag_buf_append(trace, diagnostic.data, diagnostic.len) < 0) goto out;
    rc = 0;
    goto out;
interrupted:
    snag_errorf(error, error_size, "Conversion interrupted; no output accepted");
out:
    if (rc != 0) output->len = original;
    if (started) snag_child_free(&child);
    for (size_t i = 0; i < envc; ++i) free(environment[i]);
    free(executable);
    snag_buf_free(&diagnostic);
    if (rc < 0 && error_size && !error[0]) snag_errorf(error, error_size, "Conversion failed: %s", strerror(errno));
    return rc;
}

int
snag_convert_capture(const char *const *argv, const char *dir, struct snag_buf *out,
                     struct snag_buf *trace, int (*pump)(void *, unsigned int), void *opaque,
                     snag_wake_fd wake, char *error, size_t size)
{
    return convert_run(argv, dir, out, trace, false, pump, opaque, wake, error, size);
}
int
snag_convert_internal(const char *const *argv, const char *dir, struct snag_buf *out,
                      int (*pump)(void *, unsigned int), void *opaque,
                      snag_wake_fd wake, char *error, size_t size)
{
    return convert_run(argv, dir, out, NULL, true, pump, opaque, wake, error, size);
}

int
snag_convert(const char *const *argv, const char *workdir, struct snag_buf *output,
             int (*pump)(void *, unsigned int), void *opaque, snag_wake_fd wake,
             char *error, size_t error_size)
{
    return snag_convert_capture(argv, workdir, output, NULL, pump, opaque, wake, error, error_size);
}

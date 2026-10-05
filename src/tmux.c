/* SPDX-License-Identifier: GPL-2.0-only */
#include "tmux.h"
#include "process_host.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static int
server_path(const struct snag_terminal_profile *profile, char *path)
{
    if (!profile || !snag_terminal_profile_ansi(profile) || !profile->tmux[0] ||
        profile->pane[0] != '%' || !profile->pane[1]) return snag_errno(ENOTSUP);
    for (const char *p = profile->pane + 1; *p; ++p) {
        if (*p < '0' || *p > '9') return snag_errno(EINVAL);
    }
    memcpy(path, profile->tmux, SNAG_TERMINAL_NAME_BYTES);
    /* TMUX ends in ,server-pid,session-id; socket paths may contain commas. */
    for (unsigned int i = 0u; i < 2u; ++i) {
        char *end = strrchr(path, ',');
        if (!end || !end[1]) return snag_errno(EINVAL);
        for (const char *p = end + 1; *p; ++p) {
            if (*p < '0' || *p > '9') return snag_errno(EINVAL);
        }
        *end = '\0';
    }
    return path[0] == '/' ? 0 : snag_errno(EINVAL);
}

static int
client_row(char *row, const struct snag_terminal_profile *profile,
    char *terminal, unsigned int *matches)
{
    char *pane = strchr(row, '\t');
    if (!pane) return snag_errno(EPROTO);
    *pane++ = '\0';
    char *control = strchr(pane, '\t');
    if (!control) return snag_errno(EPROTO);
    *control++ = '\0';
    char *readonly = strchr(control, '\t');
    if (!readonly) return snag_errno(EPROTO);
    *readonly++ = '\0';
    if (strcmp(pane, profile->pane) || strcmp(control, "0") || strcmp(readonly, "0")) return 0;
    if (++*matches != 1u || row[0] != '/' || strlen(row) >= SNAG_TERMINAL_NAME_BYTES)
        return snag_errno(ENOTSUP);
    (void)snprintf(terminal, SNAG_TERMINAL_NAME_BYTES, "%s", row);
    return 0;
}

static int
query_client(struct snag_child *child, const struct snag_terminal_profile *profile,
    char *terminal)
{
    unsigned int matches = 0u;
    /* A row contains two bounded terminal/profile names and two booleans. */
    char row[2u * SNAG_TERMINAL_NAME_BYTES + 8u];
    size_t used = 0u;
    uint64_t deadline = snag_monotonic_ms() + 1000u;
    bool ended[2] = {false, false};
    while (!ended[0] || !ended[1]) {
        uint64_t now = snag_monotonic_ms();
        if (now >= deadline) return snag_errno(ETIMEDOUT);
        struct snag_child_event events[2];
        size_t live = 0u;
        for (unsigned int stream = 0u; stream < 2u; ++stream) {
            if (!ended[stream]) events[live++] =
                (struct snag_child_event){child, stream, SNAG_CHILD_READ, 0u};
        }
        int rc = snag_child_wait(events, live, SNAG_WAKE_INVALID, (int)(deadline - now));
        if (rc < 0 && errno == EINTR) continue;
        if (rc < 0) return -1;
        for (size_t event = 0u; event < live; ++event) {
            unsigned int stream = events[event].stream;
            if (!events[event].revents) continue;
            unsigned char bytes[1024];
            ssize_t count = snag_child_read(child, stream, bytes, sizeof(bytes));
            if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            if (count < 0) return -1;
            if (!count) {
                ended[stream] = true;
                continue;
            }
            if (stream || !profile) continue;
            for (ssize_t i = 0; i < count; ++i) {
                if (bytes[i] == '\n') {
                    row[used] = '\0';
                    if (client_row(row, profile, terminal, &matches) < 0) return -1;
                    used = 0u;
                } else {
                    if (!bytes[i] || used + 1u == sizeof(row)) return snag_errno(EPROTO);
                    row[used++] = (char)bytes[i];
                }
            }
        }
    }
    while (snag_child_exited(child) == 0) {
        if (snag_monotonic_ms() >= deadline) return snag_errno(ETIMEDOUT);
        (void)snag_child_wait(NULL, 0u, SNAG_WAKE_INVALID, 1);
    }
    if (snag_child_reap(child) < 0 || child->signal_number > 0 || child->exit_code)
        return snag_errno(ENOTSUP);
    return !profile || (!used && matches == 1u) ? 0 : snag_errno(ENOTSUP);
}

int
snag_tmux_output_open(const struct snag_terminal_profile *profile, char *name)
{
    char server[SNAG_TERMINAL_NAME_BYTES];
    char terminal[SNAG_TERMINAL_NAME_BYTES] = {0};
    if (server_path(profile, server) < 0) return -1;
    char *program = snag_program_path("tmux");
    char **environment = snag_environment_entries();
    struct snag_child child;
    snag_child_init(&child);
    int result = -1;
    const char *arguments[] = {program, "-S", server, "list-clients", "-F",
        "#{client_tty}\t#{pane_id}\t#{client_control_mode}\t#{client_readonly}", NULL};
    if (!program || !environment ||
        snag_child_spawn_argv(&child, arguments, "/", environment) < 0) goto done;
    if (query_client(&child, profile, terminal) < 0) goto done;
    int fd = open(terminal, O_WRONLY | O_NONBLOCK | O_NOCTTY | O_NOFOLLOW);
    if (fd < 0) goto done;
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISCHR(st.st_mode) || st.st_uid != geteuid() ||
        !isatty(fd) || snag_fd_cloexec(fd) < 0) {
        (void)close(fd);
        errno = EACCES;
        goto done;
    }
    if (name) memcpy(name, terminal, SNAG_TERMINAL_NAME_BYTES);
    result = fd;
done:
    {
        int saved = errno;
        snag_child_signal(&child, SNAG_CHILD_KILL);
        snag_child_free(&child);
        snag_environment_entries_free(environment);
        free(program);
        errno = saved;
    }
    return result;
}

void
snag_tmux_refresh(const struct snag_terminal_profile *profile, const char *terminal)
{
    struct snag_terminal_profile captured;
    if (!profile) {
        if (snag_terminal_profile_capture(&captured) < 0) return;
        profile = &captured;
    }
    char server[SNAG_TERMINAL_NAME_BYTES];
    if (server_path(profile, server) < 0) return;
    char *program = snag_program_path("tmux");
    char **environment = snag_environment_entries();
    struct snag_child child;
    snag_child_init(&child);
    const char *arguments[] = {program, "-S", server, "refresh-client", "-t", terminal, NULL};
    if (program && environment &&
        snag_child_spawn_argv(&child, arguments, "/", environment) == 0)
        (void)query_client(&child, NULL, NULL);
    snag_child_signal(&child, SNAG_CHILD_KILL);
    snag_child_free(&child);
    snag_environment_entries_free(environment);
    free(program);
}
#else
int
snag_tmux_output_open(const struct snag_terminal_profile *profile, char *name)
{
    (void)profile;
    (void)name;
    return snag_errno(ENOTSUP);
}

void
snag_tmux_refresh(const struct snag_terminal_profile *profile, const char *terminal)
{
    (void)profile;
    (void)terminal;
}
#endif /* _WIN32 */

/* SPDX-License-Identifier: GPL-2.0-only */
#include "process_host.h"
#include "base.h"
#include <errno.h>
#include <fcntl.h>
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
    if (snag_fd_cloexec(p[0]) < 0 || snag_fd_cloexec(p[1]) < 0) {
        int saved = errno;
        (void)close(p[0]);
        (void)close(p[1]);
        errno = saved;
        return -1;
    }
    return 0;
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

void
snag_child_init(struct snag_child *child)
{
    memset(child, 0, sizeof(*child));
    child->fd[0] = child->fd[1] = child->fd[2] = -1;
    child->exit_code = child->signal_number = -1;
}

int
snag_child_spawn(struct snag_child *child, const char *shell, const char *command,
                  const char *directory, char **environment, bool pty)
{
    int pipes[3][2] = {{-1, -1}, {-1, -1}, {-1, -1}};
    int master = -1, slave = -1;
    child->pty = pty;
    child->rows = 24;
    child->columns = 80;
    if (pty) {
        if (open_pty_pair(&master, &slave, &child->rows, &child->columns) < 0 ||
            snag_fd_cloexec(master) < 0 || snag_fd_cloexec(slave) < 0 || set_nonblock(master) < 0)
            goto fail;
    } else {
        for (size_t i = 0; i < 3u; ++i)
            if (make_pipe(pipes[i]) < 0 || set_nonblock(pipes[i][i == 2u ? 1 : 0]) < 0)
                goto fail;
    }
    child->pid = fork();
    if (child->pid < 0)
        goto fail;
    if (child->pid == 0) {
        if (pty) {
            close_if_open(&master);
#if defined(SNAJPAGENT_HAVE_PTY)
            exec_pty_child(shell, command, directory, slave, environment);
#else
            _exit(125);
#endif
        } else {
            close_if_open(&pipes[0][0]);
            close_if_open(&pipes[1][0]);
            close_if_open(&pipes[2][1]);
            exec_child(shell, command, directory, pipes[2][0], pipes[0][1], pipes[1][1], environment);
        }
    }
    if (pty) {
        close_if_open(&slave);
        child->fd[0] = child->fd[2] = master;
    } else {
        (void)setpgid(child->pid, child->pid);
        for (size_t i = 0; i < 3u; ++i) {
            unsigned int side = i == 2u ? 1u : 0u;
            child->fd[i] = pipes[i][side];
            close_if_open(&pipes[i][1u - side]);
        }
    }
    return 0;
fail:
    {
        int error = errno;
        close_if_open(&master);
        close_if_open(&slave);
        for (size_t i = 0; i < 3u; ++i) {
            close_if_open(&pipes[i][0]);
            close_if_open(&pipes[i][1]);
        }
        errno = error;
    }
    return -1;
}

void
snag_child_signal(struct snag_child *child, enum snag_child_signal signal)
{
    if (!child->reaped)
        kill_child_group(child->pid, signal == SNAG_CHILD_KILL ? SIGKILL :
                         signal == SNAG_CHILD_INTERRUPT ? SIGINT : SIGTERM);
}

int
snag_child_exited(struct snag_child *child)
{
    siginfo_t info = {0};
    if (waitid(P_PID, (id_t)child->pid, &info, WEXITED | WNOHANG | WNOWAIT) < 0) {
        if (errno == ECHILD)
            child->reaped = true; /* Never signal a reused PID after ownership loss. */
        return -1;
    }
    return info.si_pid == child->pid;
}

int
snag_child_reap(struct snag_child *child)
{
    if (child->reaped)
        return 0;
    int status;
    pid_t got;
    do {
        got = waitpid(child->pid, &status, WNOHANG);
    } while (got < 0 && errno == EINTR);
    if (got != child->pid) {
        if (got < 0 && errno == ECHILD)
            child->reaped = true;
        return -1;
    }
    child->reaped = true;
    if (WIFEXITED(status))
        child->exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status))
        child->signal_number = WTERMSIG(status);
    return 0;
}

void
snag_child_close_stream(struct snag_child *child, unsigned int stream)
{
    if (stream >= 3u || (child->pty && stream == 2u))
        return;
    int fd = child->fd[stream];
    close_if_open(&child->fd[stream]);
    for (size_t i = 0; i < 3u; ++i)
        if (child->fd[i] == fd)
            child->fd[i] = -1;
}

void
snag_child_free(struct snag_child *child)
{
    if (!child->reaped && child->pid > 0) {
        snag_child_signal(child, SNAG_CHILD_KILL);
        while (waitpid(child->pid, NULL, 0) < 0 && errno == EINTR)
            ;
    }
    for (unsigned int i = 0; i < 3u; ++i)
        snag_child_close_stream(child, i);
    snag_child_init(child);
}

void
snag_child_resize(struct snag_child *child)
{
    if (child->pty)
        pty_apply_current_size(child->fd[0], &child->rows, &child->columns);
}

ssize_t
snag_child_read(struct snag_child *child, unsigned int stream, void *buffer, size_t size)
{
    ssize_t n = read(child->fd[stream], buffer, size);
    return n < 0 && child->pty && errno == EIO ? 0 : n;
}

ssize_t
snag_child_write(struct snag_child *child, const void *buffer, size_t size)
{
    return write(child->fd[2], buffer, size);
}

int
snag_child_wait(struct snag_child_event *events, size_t count, snag_wake_fd wake, int timeout_ms)
{
    struct pollfd fds[97];
    if (count > 96u) {
        errno = EINVAL;
        return -1;
    }
    for (size_t i = 0; i < count; ++i) {
        fds[i] = (struct pollfd){events[i].child->fd[events[i].stream], 0, 0};
        if (events[i].events & SNAG_CHILD_READ)
            fds[i].events |= POLLIN;
        if (events[i].events & SNAG_CHILD_WRITE)
            fds[i].events |= POLLOUT;
        events[i].revents = 0;
    }
    fds[count] = (struct pollfd){wake, POLLIN, 0};
    int rc = poll(fds, (nfds_t)count + 1u, timeout_ms);
    if (rc < 0)
        return rc;
    for (size_t i = 0; i < count; ++i) {
        if (fds[i].revents & POLLIN)
            events[i].revents |= SNAG_CHILD_READ;
        if (fds[i].revents & POLLOUT)
            events[i].revents |= SNAG_CHILD_WRITE;
        if (fds[i].revents & (POLLHUP | POLLERR))
            events[i].revents |= SNAG_CHILD_END;
        if (fds[i].revents & POLLNVAL)
            events[i].revents |= SNAG_CHILD_ERROR;
    }
    return rc;
}

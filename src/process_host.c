/* SPDX-License-Identifier: GPL-2.0-only */
#include "process_host.h"
#include "base.h"
#ifdef _WIN32
#include "net.h"
#include <windows.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* Dynamically detected API; keep the rest of the import floor independent. */
#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE ((DWORD_PTR)0x00020016)
#endif

struct child_pipe {
    HANDLE handle;
    OVERLAPPED io;
    unsigned char bytes[4096];
    DWORD count, error;
    bool pending, ready;
};

struct snag_child_windows {
    HANDLE process, job, console;
    DWORD pid;
    struct child_pipe pipe[3];
    void (WINAPI *console_close)(HANDLE);
    HRESULT (WINAPI *console_resize)(HANDLE, COORD);
    COORD dimensions;
};

static int
child_error(DWORD error)
{
    errno = error == ERROR_BROKEN_PIPE || error == ERROR_NO_DATA ? EPIPE :
            error == ERROR_OPERATION_ABORTED ? EINTR :
            error == ERROR_ACCESS_DENIED ? EACCES :
            error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? ENOENT :
            error == ERROR_INVALID_HANDLE ? EBADF : EIO;
    return -1;
}

void
snag_child_init(struct snag_child *child)
{
    memset(child, 0, sizeof(*child));
    child->exit_code = child->signal_number = -1;
}

static bool
pipe_done(struct child_pipe *pipe)
{
    if (!pipe->pending)
        return pipe->ready;
    if (GetOverlappedResult(pipe->handle, &pipe->io, &pipe->count, FALSE))
        pipe->error = 0;
    else {
        pipe->error = GetLastError();
        if (pipe->error == ERROR_IO_INCOMPLETE)
            return false;
    }
    pipe->pending = false;
    pipe->ready = true;
    return true;
}

static void
pipe_begin(struct child_pipe *pipe, bool write, DWORD size)
{
    (void)ResetEvent(pipe->io.hEvent);
    pipe->ready = false;
    BOOL ok = write ? WriteFile(pipe->handle, pipe->bytes, size, &pipe->count, &pipe->io) :
                     ReadFile(pipe->handle, pipe->bytes, size, &pipe->count, &pipe->io);
    pipe->error = ok ? 0 : GetLastError();
    pipe->pending = !ok && pipe->error == ERROR_IO_PENDING;
    pipe->ready = !pipe->pending;
}

static int
create_pipe(struct child_pipe *pipe, bool input, HANDLE *other)
{
    char id[SNAG_ID_HEX_LEN + 1u];
    WCHAR name[96];
    if (snag_random_id(id) < 0)
        return -1;
    if (swprintf(name, 96u, L"\\\\.\\pipe\\snajpagent-%hs", id) < 0)
        return -1;
    pipe->handle = CreateNamedPipeW(name, (input ? PIPE_ACCESS_OUTBOUND : PIPE_ACCESS_INBOUND) |
        FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, 65536u, 65536u, 0, NULL);
    if (pipe->handle == INVALID_HANDLE_VALUE) {
        pipe->handle = NULL;
        return child_error(GetLastError());
    }
    pipe->io.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!pipe->io.hEvent)
        return child_error(GetLastError());
    BOOL connected = ConnectNamedPipe(pipe->handle, &pipe->io);
    if (!connected && GetLastError() != ERROR_IO_PENDING)
        return child_error(GetLastError());
    pipe->pending = !connected;
    SECURITY_ATTRIBUTES security = {sizeof(security), NULL, TRUE};
    *other = CreateFileW(name, input ? GENERIC_READ : GENERIC_WRITE, 0,
                         &security, OPEN_EXISTING, 0, NULL);
    if (*other == INVALID_HANDLE_VALUE) {
        *other = NULL;
        return child_error(GetLastError());
    }
    DWORD bytes, pid;
    if ((!connected && !GetOverlappedResult(pipe->handle, &pipe->io, &bytes, TRUE)) ||
        !GetNamedPipeClientProcessId(pipe->handle, &pid) || pid != GetCurrentProcessId()) {
        errno = EACCES;
        return -1;
    }
    (void)ResetEvent(pipe->io.hEvent);
    pipe->pending = false;
    return 0;
}

void
snag_child_close_stream(struct snag_child *child, unsigned int stream)
{
    if (!child->native || stream >= 3u || (child->pty && stream == 2u))
        return;
    struct child_pipe *pipe = &child->native->pipe[stream];
    if (pipe->handle) {
        if (pipe->pending) {
            (void)CancelIoEx(pipe->handle, &pipe->io);
            (void)GetOverlappedResult(pipe->handle, &pipe->io, &pipe->count, TRUE);
        }
        (void)CloseHandle(pipe->handle);
    }
    if (pipe->io.hEvent)
        (void)CloseHandle(pipe->io.hEvent);
    memset(pipe, 0, sizeof(*pipe));
}

void
snag_child_signal(struct snag_child *child, enum snag_child_signal signal)
{
    struct snag_child_windows *native = child->native;
    if (!native || !native->process || child->reaped)
        return;
    if (signal == SNAG_CHILD_INTERRUPT && child->pty) {
        struct child_pipe *pipe = &native->pipe[2];
        if (pipe->pending) {
            (void)CancelIoEx(pipe->handle, &pipe->io);
            (void)GetOverlappedResult(pipe->handle, &pipe->io, &pipe->count, TRUE);
        }
        pipe->bytes[0] = 3;
        pipe_begin(pipe, true, 1u);
    } else if (signal == SNAG_CHILD_INTERRUPT)
        (void)GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, native->pid);
    else
        (void)TerminateJobObject(native->job, signal == SNAG_CHILD_KILL ? 137u : 143u);
}

void
snag_child_free(struct snag_child *child)
{
    struct snag_child_windows *native = child->native;
    if (!native)
        return;
    if (native->job)
        (void)TerminateJobObject(native->job, 137u);
    if (native->process) {
        (void)WaitForSingleObject(native->process, INFINITE);
        (void)CloseHandle(native->process);
    }
    /* Closing host pipe ends also prevents ConPTY shutdown from blocking on output. */
    child->pty = false;
    for (unsigned int i = 0; i < 3u; ++i)
        snag_child_close_stream(child, i);
    if (native->console)
        native->console_close(native->console);
    if (native->job)
        (void)CloseHandle(native->job);
    free(native);
    snag_child_init(child);
}

static COORD
console_dimensions(void)
{
    CONSOLE_SCREEN_BUFFER_INFO info;
    return GetConsoleScreenBufferInfo(GetStdHandle(STD_ERROR_HANDLE), &info) ?
        (COORD){(SHORT)(info.srWindow.Right - info.srWindow.Left + 1),
                (SHORT)(info.srWindow.Bottom - info.srWindow.Top + 1)} : (COORD){80, 24};
}

int
snag_child_spawn(struct snag_child *child, const char *shell, const char *command,
                 const char *directory, char **environment, bool pty)
{
    struct snag_child_windows *native = calloc(1, sizeof(*native));
    HANDLE ends[3] = {0};
    struct snag_buf line, env;
    wchar_t *exe = NULL, *cwd = NULL, *text = NULL;
    STARTUPINFOEXW startup = {0};
    PROCESS_INFORMATION process = {0};
    int rc = -1;
    SIZE_T attributes = 0;
    bool attributes_ready = false;

    if (!native)
        return -1;
    child->native = native;
    child->pty = pty;
    snag_buf_init(&line, 256u * 1024u + 32768u);
    snag_buf_init(&env, SNAG_MEMORY_LIMIT);
    exe = snag_utf8_to_wide(shell);
    cwd = snag_utf8_to_wide(directory);
    if (!exe || !cwd)
        goto out;
    const wchar_t *base = wcsrchr(exe, L'\\');
    const wchar_t *slash = wcsrchr(exe, L'/');
    if (slash && (!base || slash > base))
        base = slash;
    base = base ? base + 1 : exe;
    if (!_wcsicmp(base, L"cmd.exe") || !_wcsicmp(base, L"cmd")) {
        if (snag_buf_printf(&line, "\"%s\" /d /q /v:off /s /c \"%s\"", shell, command) < 0)
            goto out;
    } else {
        /* A configured POSIX shell still receives one -c argument using CRT quoting. */
        if (snag_buf_printf(&line, "\"%s\" -c \"", shell) < 0)
            goto out;
        for (const char *p = command; *p;) {
            size_t slashes = 0;
            while (*p == '\\') {
                ++slashes;
                ++p;
            }
            size_t count = !*p || *p == '"' ? 2u * slashes : slashes;
            while (count--)
                if (snag_buf_putc(&line, '\\') < 0)
                    goto out;
            if (*p == '"' && snag_buf_putc(&line, '\\') < 0)
                goto out;
            if (*p && snag_buf_putc(&line, (unsigned char)*p++) < 0)
                goto out;
        }
        if (snag_buf_putc(&line, '"') < 0)
            goto out;
    }
    if (snag_buf_terminate(&line) < 0 || !(text = snag_utf8_to_wide((char *)line.data)))
        goto out;
    if (wcslen(text) >= 32767u) {
        errno = E2BIG;
        goto out;
    }
    for (size_t i = 0; environment[i]; ++i) {
        wchar_t *entry = snag_utf8_to_wide(environment[i]);
        if (!entry)
            goto out;
        int added = snag_buf_append(&env, entry, (wcslen(entry) + 1u) * sizeof(*entry));
        free(entry);
        if (added < 0)
            goto out;
    }
    const wchar_t zero[2] = {0};
    if (snag_buf_append(&env, zero, env.len ? sizeof(wchar_t) : sizeof(zero)) < 0)
        goto out;
    for (size_t i = 0; i < 3u; ++i)
        if ((!pty || i != 1u) && create_pipe(&native->pipe[i], i == 2u, &ends[i]) < 0)
            goto out;
    native->job = CreateJobObjectW(NULL, NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!native->job || !SetInformationJobObject(native->job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        goto native_error;
    if (pty) {
        HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
        HRESULT (WINAPI *create)(COORD, HANDLE, HANDLE, DWORD, HANDLE *);
        FARPROC function = GetProcAddress(kernel, "CreatePseudoConsole");
        memcpy(&create, &function, sizeof(create));
        function = GetProcAddress(kernel, "ClosePseudoConsole");
        memcpy(&native->console_close, &function, sizeof(native->console_close));
        function = GetProcAddress(kernel, "ResizePseudoConsole");
        memcpy(&native->console_resize, &function, sizeof(native->console_resize));
        native->dimensions = console_dimensions();
        if (!create || !native->console_close || !native->console_resize) {
            errno = ENOTSUP;
            goto out;
        }
        if (FAILED(create(native->dimensions, ends[2], ends[0], 0, &native->console)))
            goto native_error;
    }
    (void)InitializeProcThreadAttributeList(NULL, 1u, 0, &attributes);
    startup.lpAttributeList = malloc(attributes);
    if (!startup.lpAttributeList)
        goto out;
    if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1u, 0, &attributes))
        goto native_error;
    attributes_ready = true;
    if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0,
        pty ? PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE : PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
        pty ? native->console : (void *)ends, pty ? sizeof(HANDLE) : sizeof(ends), NULL, NULL))
        goto native_error;
    startup.StartupInfo.cb = sizeof(startup);
    if (!pty) {
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = ends[2];
        startup.StartupInfo.hStdOutput = ends[0];
        startup.StartupInfo.hStdError = ends[1];
    }
    if (!CreateProcessW(exe, text, NULL, NULL, !pty,
        CREATE_SUSPENDED | CREATE_NEW_PROCESS_GROUP | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
        env.data, cwd, &startup.StartupInfo, &process))
        goto native_error;
    native->process = process.hProcess;
    native->pid = process.dwProcessId;
    if (!AssignProcessToJobObject(native->job, process.hProcess)) {
        DWORD error = GetLastError();
        (void)TerminateProcess(process.hProcess, 125u);
        child_error(error);
        goto out;
    }
    if (ResumeThread(process.hThread) == (DWORD)-1)
        goto native_error;
    rc = 0;
    goto out;
native_error:
    child_error(GetLastError());
out:
    {
        int error = errno;
        if (process.hThread)
            (void)CloseHandle(process.hThread);
        if (attributes_ready)
            DeleteProcThreadAttributeList(startup.lpAttributeList);
        free(startup.lpAttributeList);
        for (size_t i = 0; i < 3u; ++i)
            if (ends[i])
                (void)CloseHandle(ends[i]);
        free(exe);
        free(cwd);
        free(text);
        snag_buf_free(&line);
        volatile unsigned char *wipe = env.data;
        for (size_t i = 0; i < env.len; ++i)
            wipe[i] = 0;
        snag_buf_free(&env);
        if (rc < 0)
            snag_child_free(child);
        errno = error;
    }
    return rc;
}

int
snag_child_exited(struct snag_child *child)
{
    DWORD rc = WaitForSingleObject(child->native->process, 0);
    return rc == WAIT_OBJECT_0 ? 1 : rc == WAIT_TIMEOUT ? 0 : child_error(GetLastError());
}

int
snag_child_reap(struct snag_child *child)
{
    DWORD code;
    if (snag_child_exited(child) != 1 || !GetExitCodeProcess(child->native->process, &code))
        return child_error(GetLastError());
    child->exit_code = code;
    child->reaped = true;
    return 0;
}

void
snag_child_resize(struct snag_child *child)
{
    struct snag_child_windows *native = child->native;
    if (!native || !native->console)
        return;
    COORD size = console_dimensions();
    if ((size.X != native->dimensions.X || size.Y != native->dimensions.Y) &&
        SUCCEEDED(native->console_resize(native->console, size)))
        native->dimensions = size;
}

ssize_t
snag_child_read(struct snag_child *child, unsigned int stream, void *buffer, size_t size)
{
    struct child_pipe *pipe = &child->native->pipe[stream];
    if (!pipe_done(pipe)) {
        errno = EAGAIN;
        return -1;
    }
    if (pipe->error)
        return pipe->error == ERROR_BROKEN_PIPE || pipe->error == ERROR_NO_DATA ? 0 : child_error(pipe->error);
    size_t count = pipe->count < size ? pipe->count : size;
    memcpy(buffer, pipe->bytes, count);
    memmove(pipe->bytes, pipe->bytes + count, pipe->count - count);
    pipe->count -= (DWORD)count;
    pipe->ready = pipe->count != 0;
    return (ssize_t)count;
}

ssize_t
snag_child_write(struct snag_child *child, const void *buffer, size_t size)
{
    struct child_pipe *pipe = &child->native->pipe[2];
    if (!pipe->handle) {
        errno = EPIPE;
        return -1;
    }
    if (!pipe->pending && !pipe->ready) {
        if (size > sizeof(pipe->bytes))
            size = sizeof(pipe->bytes);
        memcpy(pipe->bytes, buffer, size);
        pipe_begin(pipe, true, (DWORD)size);
    }
    if (!pipe_done(pipe)) {
        errno = EAGAIN;
        return -1;
    }
    pipe->ready = false;
    return pipe->error ? child_error(pipe->error) : (ssize_t)pipe->count;
}

int
snag_child_wait(struct snag_child_event *events, size_t count, snag_wake_fd wake, int timeout_ms)
{
    HANDLE waits[97], wake_event = NULL;
    size_t waiting = 0, group = 0;
    uint64_t start = snag_monotonic_ms();
    int rc = 0;
    if (count > 96u || timeout_ms < 0) {
        errno = EINVAL;
        return -1;
    }
    if (wake != SNAG_WAKE_INVALID) {
        wake_event = WSACreateEvent();
        if (wake_event == WSA_INVALID_EVENT)
            return snag_socket_error(WSAGetLastError());
        if (WSAEventSelect(wake, wake_event, FD_READ | FD_CLOSE) < 0) {
            int error = WSAGetLastError();
            (void)WSACloseEvent(wake_event);
            return snag_socket_error(error);
        }
    }
    for (;;) {
        waiting = 0;
        rc = 0;
        for (size_t i = 0; i < count; ++i) {
            struct snag_child_event *event = &events[i];
            struct snag_child_windows *native = event->child->native;
            event->revents = 0;
            for (unsigned int direction = 0; direction < 2u; ++direction) {
                unsigned int flag = direction ? SNAG_CHILD_WRITE : SNAG_CHILD_READ;
                if (!(event->events & flag))
                    continue;
                struct child_pipe *pipe = &native->pipe[direction ? 2u : event->stream];
                if (!pipe->handle) {
                    event->revents |= SNAG_CHILD_END;
                    continue;
                }
                if (!direction && !pipe->pending && !pipe->ready)
                    pipe_begin(pipe, false, sizeof(pipe->bytes));
                if (pipe_done(pipe) || (direction && !pipe->pending))
                    event->revents |= flag;
                else if (waiting < 96u)
                    waits[waiting++] = pipe->io.hEvent;
            }
            if (event->revents)
                ++rc;
        }
        if (wake_event) {
            if (WaitForSingleObject(wake_event, 0) == WAIT_OBJECT_0)
                ++rc;
            waits[waiting++] = wake_event;
        }
        uint64_t elapsed = snag_monotonic_ms() - start;
        if (rc || elapsed >= (uint64_t)timeout_ms)
            break;
        DWORD delay = (DWORD)((uint64_t)timeout_ms - elapsed);
        if (waiting > MAXIMUM_WAIT_OBJECTS && delay > 4u)
            delay = 4u;
        if (!waiting)
            Sleep(delay);
        else {
            size_t offset = group * MAXIMUM_WAIT_OBJECTS;
            if (offset >= waiting)
                offset = 0;
            DWORD n = (DWORD)(waiting - offset);
            if (n > MAXIMUM_WAIT_OBJECTS)
                n = MAXIMUM_WAIT_OBJECTS;
            DWORD ready = WaitForMultipleObjects(n, waits + offset, FALSE, delay);
            if (ready == WAIT_FAILED) {
                rc = child_error(GetLastError());
                break;
            }
            group = offset ? 0 : 1;
        }
    }
    if (wake_event) {
        int error = errno;
        if (WSAEventSelect(wake, NULL, 0) < 0 && rc >= 0)
            rc = snag_socket_error(WSAGetLastError());
        else
            errno = error;
        (void)WSACloseEvent(wake_event);
    }
    return rc;
}

#else
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

static void
exec_child(const char *shell, const char *command, const char *workdir,
           int stdin_rd, int stdout_wr, int stderr_wr, char **env)
{
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

static void
exec_pty_child(const char *shell, const char *command, const char *workdir,
               int slave_fd, char **env)
{
    if (setsid() < 0)
        _exit(125);
    (void)ioctl(slave_fd, TIOCSCTTY, 0);
    exec_child(shell, command, workdir, slave_fd, slave_fd, slave_fd, env);
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
        sigset_t unblocked;
        sigemptyset(&unblocked);
        if (sigprocmask(SIG_SETMASK, &unblocked, NULL) < 0)
            _exit(125);
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
            (void)setpgid(0, 0);
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
#endif

/* SPDX-License-Identifier: GPL-2.0-only */
#include "process_host.h"
#include "base.h"
#include "fs.h"
#ifdef _WIN32
#include "net.h"
#include "term_host.h"
#include <windows.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <process.h>
#include <fcntl.h>
#include <io.h>
#include <stdatomic.h>
#ifdef SNAG_LEGACY_PTY
void *snag_legacy_console_open(HANDLE, HANDLE, HANDLE, HANDLE, COORD);
int snag_legacy_console_run(void *);
void snag_legacy_console_free(void *);
#endif

/* Dynamically detected API; keep the rest of the import floor independent. */
#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE ((DWORD_PTR)0x00020016)
#endif
#ifndef PROC_THREAD_ATTRIBUTE_HANDLE_LIST
#define PROC_THREAD_ATTRIBUTE_HANDLE_LIST ((DWORD_PTR)0x00020002)
#endif

struct child_pipe {
    HANDLE handle;
    OVERLAPPED io;
    unsigned char bytes[4096];
    DWORD count, error;
    bool pending, ready;
};

struct snag_child_windows {
    HANDLE process, job, console, console_closer;
    DWORD pid;
    struct child_pipe pipe[3];
    void (WINAPI *console_close)(HANDLE);
    HRESULT (WINAPI *console_release)(HANDLE);
    HRESULT (WINAPI *console_resize)(HANDLE, COORD);
    COORD dimensions;
    struct snag_output_broker *broker;
    bool legacy_console;
    bool collector_done;
    int collector_error;
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

static HANDLE
private_pipe(const wchar_t *name, DWORD access)
{
    HANDLE token = NULL, pipe = INVALID_HANDLE_VALUE;
    TOKEN_USER *user = NULL;
    PACL acl = NULL;
    SECURITY_DESCRIPTOR descriptor;
    DWORD size = 0, error = 0;

    if (!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token) &&
        (GetLastError() != ERROR_NO_TOKEN ||
         !OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)))
        goto fail;
    (void)GetTokenInformation(token, TokenUser, NULL, 0, &size);
    if (!size)
        goto fail;
    user = malloc(size);
    if (!user) {
        error = ERROR_NOT_ENOUGH_MEMORY;
        goto out;
    }
    if (!GetTokenInformation(token, TokenUser, user, size, &size))
        goto fail;
    size = (DWORD)(sizeof(ACL) + offsetof(ACCESS_ALLOWED_ACE, SidStart)) +
           GetLengthSid(user->User.Sid);
    acl = malloc(size);
    if (!acl) {
        error = ERROR_NOT_ENOUGH_MEMORY;
        goto out;
    }
    if (!InitializeAcl(acl, size, ACL_REVISION) ||
        !AddAccessAllowedAce(acl, ACL_REVISION, FILE_ALL_ACCESS, user->User.Sid) ||
        !InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) ||
        !SetSecurityDescriptorOwner(&descriptor, user->User.Sid, FALSE) ||
        !SetSecurityDescriptorDacl(&descriptor, TRUE, acl, FALSE) ||
        !SetSecurityDescriptorControl(&descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED))
        goto fail;
    SECURITY_ATTRIBUTES security = {sizeof(security), &descriptor, FALSE};
    DWORD mode = PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT;
    if (GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetNamedPipeClientProcessId"))
        mode |= PIPE_REJECT_REMOTE_CLIENTS;
    pipe = CreateNamedPipeW(name, access | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                            mode, 1, 65536u, 65536u, 0, &security);
    if (pipe != INVALID_HANDLE_VALUE)
        goto out;
fail:
    error = GetLastError();
out:
    if (token && !CloseHandle(token) && !error)
        error = GetLastError();
    free(acl);
    free(user);
    if (error && pipe != INVALID_HANDLE_VALUE) {
        (void)CloseHandle(pipe);
        pipe = INVALID_HANDLE_VALUE;
    }
    SetLastError(error);
    return pipe;
}

static bool
peer_transfer(HANDLE handle, bool write, void *bytes, DWORD size, bool asynchronous)
{
    OVERLAPPED io = {0};
    DWORD count = 0;
    if (asynchronous && !(io.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL)))
        return false;
    BOOL ok = write ? WriteFile(handle, bytes, size, &count, asynchronous ? &io : NULL) :
                      ReadFile(handle, bytes, size, &count, asynchronous ? &io : NULL);
    if (!ok && asynchronous && GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(io.hEvent, 1000u) != WAIT_OBJECT_0)
            (void)CancelIo(handle);
        ok = GetOverlappedResult(handle, &io, &count, TRUE);
    }
    DWORD error = ok ? (count == size ? 0 : ERROR_INVALID_DATA) : GetLastError();
    if (io.hEvent)
        (void)CloseHandle(io.hEvent);
    SetLastError(error);
    return !error;
}

static int
verify_pipe_pair(struct child_pipe *pipe, bool input, HANDLE other, bool asynchronous)
{
    unsigned char expected[16], received[16];
    if (snag_random_bytes(expected, sizeof(expected)) < 0)
        return -1;
    if (input)
        memcpy(pipe->bytes, expected, sizeof(expected));
    else if (!peer_transfer(other, true, expected, sizeof(expected), asynchronous))
        return child_error(GetLastError());
    /* Both endpoints are owned and empty; the challenge fits their quota. */
    pipe_begin(pipe, input, sizeof(expected));
    if (pipe->pending && WaitForSingleObject(pipe->io.hEvent, 1000u) != WAIT_OBJECT_0) {
        errno = ETIMEDOUT;
        return -1;
    }
    if (!pipe_done(pipe) || pipe->error || pipe->count != sizeof(expected))
        return child_error(pipe->error ? pipe->error : ERROR_INVALID_DATA);
    if (input && !peer_transfer(other, false, received, sizeof(received), asynchronous))
        return child_error(GetLastError());
    const unsigned char *actual = input ? received : pipe->bytes;
    unsigned int difference = 0;
    for (size_t i = 0; i < sizeof(expected); ++i)
        difference |= actual[i] ^ expected[i];
    if (difference) {
        errno = EACCES;
        return -1;
    }
    (void)ResetEvent(pipe->io.hEvent);
    pipe->ready = false;
    pipe->count = 0;
    return 0;
}

static int
create_pipe(struct child_pipe *pipe, bool input, HANDLE *other, bool asynchronous)
{
    char id[SNAG_ID_HEX_LEN + 1u];
    WCHAR name[96];
    if (snag_random_id(id) < 0)
        return -1;
    if (swprintf(name, 96u, L"\\\\.\\pipe\\snajpagent-%hs", id) < 0)
        return -1;
    pipe->handle = private_pipe(name, input ? PIPE_ACCESS_OUTBOUND : PIPE_ACCESS_INBOUND);
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
                         &security, OPEN_EXISTING, asynchronous ? FILE_FLAG_OVERLAPPED : 0, NULL);
    if (*other == INVALID_HANDLE_VALUE) {
        *other = NULL;
        return child_error(GetLastError());
    }
    DWORD bytes;
    if (!connected && !GetOverlappedResult(pipe->handle, &pipe->io, &bytes, TRUE))
        return child_error(GetLastError());
    pipe->pending = false;
    BOOL (WINAPI *client_pid)(HANDLE, PULONG);
    FARPROC function = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetNamedPipeClientProcessId");
    memcpy(&client_pid, &function, sizeof(client_pid));
    if (client_pid) {
        ULONG pid;
        if (!client_pid(pipe->handle, &pid) || pid != GetCurrentProcessId()) {
            errno = EACCES;
            return -1;
        }
    }
    return verify_pipe_pair(pipe, input, *other, asynchronous);
}

static void
pipe_cancel(struct child_pipe *pipe)
{
    if (!pipe->pending)
        return;
    /* The engine owns issuance and cancellation on the same thread. */
    (void)CancelIo(pipe->handle);
    (void)GetOverlappedResult(pipe->handle, &pipe->io, &pipe->count, TRUE);
    pipe->pending = false;
}

struct snag_output_broker {
    HANDLE process;
    HANDLE standard[2];
    struct child_pipe control;
};

static const wchar_t broker_option[] = L"--snajpagent-private-writer";
static const wchar_t broker_prefix[] = L"\\\\.\\pipe\\snajpagent-writer-";
static _Atomic(HANDLE) broker_owned_job;
static CRITICAL_SECTION broker_spawn_lock;

enum { BROKER_WRITE = 1, BROKER_SPAWN = 2, BROKER_READ_CONSOLE = 3, BROKER_PTY = 4,
       BROKER_WRITE_STANDARD = 5 };
struct broker_spawn_request {
    uint64_t job, streams[3];
    uint32_t units[4]; /* executable, command line, cwd, double-NUL environment */
    uint32_t columns, rows;
};
struct broker_spawn_reply {
    uint64_t process;
    uint32_t pid, error;
};

void
snag_output_broker_cancel(struct snag_output_broker *broker)
{
    if (broker && broker->process)
        (void)TerminateProcess(broker->process, 125u);
}

void
snag_output_broker_close(struct snag_output_broker *broker)
{
    if (!broker)
        return;
    if (broker->control.handle) {
        pipe_cancel(&broker->control);
        (void)CloseHandle(broker->control.handle);
    }
    if (broker->control.io.hEvent)
        (void)CloseHandle(broker->control.io.hEvent);
    if (broker->process) {
        if (WaitForSingleObject(broker->process, 100u) != WAIT_OBJECT_0)
            (void)TerminateProcess(broker->process, 125u);
        (void)WaitForSingleObject(broker->process, INFINITE);
        (void)CloseHandle(broker->process);
    }
    free(broker);
}

static int
broker_wait(struct snag_output_broker *broker, uint64_t deadline,
            int (*checkpoint)(void *), void *opaque)
{
    struct child_pipe *pipe = &broker->control;
    HANDLE events[] = {pipe->io.hEvent, broker->process};
    while (!pipe_done(pipe)) {
        DWORD rc = WaitForMultipleObjects(2u, events, FALSE, 16u);
        if (rc == WAIT_FAILED)
            return child_error(GetLastError());
        if (rc == WAIT_OBJECT_0 + 1u) {
            errno = EPIPE;
            return -1;
        }
        if (rc == WAIT_TIMEOUT) {
            if (checkpoint && checkpoint(opaque) < 0)
                return -1;
            if (deadline && snag_monotonic_ms() >= deadline) {
                errno = ETIMEDOUT;
                return -1;
            }
        }
    }
    return pipe->error ? child_error(pipe->error) : 0;
}

static int
broker_transfer(struct snag_output_broker *broker, bool write,
                void *data, size_t size, uint64_t deadline,
                int (*checkpoint)(void *), void *opaque)
{
    struct child_pipe *pipe = &broker->control;
    unsigned char *bytes = data;
    while (size) {
        DWORD amount = size < sizeof(pipe->bytes) ? (DWORD)size : sizeof(pipe->bytes);
        if (write)
            memcpy(pipe->bytes, bytes, amount);
        pipe_begin(pipe, write, amount);
        if (broker_wait(broker, deadline, checkpoint, opaque) < 0)
            return -1;
        if (!pipe->count || pipe->count > amount) {
            errno = EIO;
            return -1;
        }
        if (!write)
            memcpy(bytes, pipe->bytes, pipe->count);
        volatile unsigned char *wipe = pipe->bytes;
        for (DWORD i = 0; i < pipe->count; ++i)
            wipe[i] = 0;
        bytes += pipe->count;
        size -= pipe->count;
    }
    return 0;
}

static struct snag_output_broker *
broker_open(int (*checkpoint)(void *), void *opaque, bool console)
{
    struct snag_output_broker *broker = calloc(1, sizeof(*broker));
    HANDLE mapping = NULL, remote = NULL, parent = NULL;
    PROCESS_INFORMATION child = {0};
    STARTUPINFOW startup = {.cb = sizeof(startup)};
    if (console) {
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
    }
    wchar_t program[32768], command[32768], name[96], environment[2] = {0};
    char id[SNAG_ID_HEX_LEN + 1u];
    unsigned char nonce[16], answer[16];
    void *view = NULL;
    int error;
    if (!broker)
        return NULL;
    broker->standard[0] = GetStdHandle(STD_OUTPUT_HANDLE);
    broker->standard[1] = GetStdHandle(STD_ERROR_HANDLE);
    if (snag_random_id(id) < 0 || snag_random_bytes(nonce, sizeof(nonce)) < 0)
        goto fail;
    DWORD length = GetModuleFileNameW(NULL, program, 32768u);
    if (!length)
        goto native_error;
    if (length >= 32768u || swprintf(name, 96u, L"%ls%hs", broker_prefix, id) < 0 ||
        swprintf(command, 32768u, L"\"%ls\" %ls %ls", program, broker_option, name) < 0) {
        errno = ENAMETOOLONG;
        goto fail;
    }
    broker->control.handle = private_pipe(name, PIPE_ACCESS_DUPLEX);
    if (broker->control.handle == INVALID_HANDLE_VALUE) {
        broker->control.handle = NULL;
        goto native_error;
    }
    broker->control.io.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!broker->control.io.hEvent)
        goto native_error;
    if (!ConnectNamedPipe(broker->control.handle, &broker->control.io) &&
        GetLastError() != ERROR_IO_PENDING)
        goto native_error;
    broker->control.pending = true;
    mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                  0, sizeof(nonce), NULL);
    if (!mapping || !(view = MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, sizeof(nonce))))
        goto native_error;
    memcpy(view, nonce, sizeof(nonce));
    if (!UnmapViewOfFile(view))
        goto native_error;
    view = NULL;
    if (!CreateProcessW(program, command, NULL, NULL, FALSE,
        CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT |
        (console ? CREATE_NEW_CONSOLE : CREATE_NEW_PROCESS_GROUP),
        environment, NULL, &startup, &child))
        goto native_error;
    broker->process = child.hProcess;
    if (ResumeThread(child.hThread) == (DWORD)-1)
        goto native_error;
    uint64_t deadline = snag_monotonic_ms() + 5000u;
    if (broker_wait(broker, deadline, checkpoint, opaque) < 0)
        goto fail;
    /* The real client clears startup stdio before opening this pipe. */
    if (!DuplicateHandle(GetCurrentProcess(), mapping, child.hProcess,
                          &remote, FILE_MAP_READ, FALSE, 0) ||
        !DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), child.hProcess,
                          &parent, SYNCHRONIZE | PROCESS_DUP_HANDLE, FALSE, 0))
        goto native_error;
    uint64_t target[2] = {(uintptr_t)remote, (uintptr_t)parent};
    if (broker_transfer(broker, true, target, sizeof(target), deadline, checkpoint, opaque) < 0 ||
        broker_transfer(broker, false, answer, sizeof(answer), deadline, checkpoint, opaque) < 0)
        goto fail;
    unsigned int difference = 0;
    for (size_t i = 0; i < sizeof(nonce); ++i)
        difference |= nonce[i] ^ answer[i];
    if (difference) {
        errno = EACCES;
        goto fail;
    }
    (void)CloseHandle(child.hThread);
    (void)CloseHandle(mapping);
    return broker;
native_error:
    child_error(GetLastError());
fail:
    error = errno;
    if (view)
        (void)UnmapViewOfFile(view);
    if (mapping)
        (void)CloseHandle(mapping);
    if (child.hThread)
        (void)CloseHandle(child.hThread);
    snag_output_broker_close(broker);
    errno = error;
    return NULL;
}

static int
broker_write(struct snag_output_broker **owner, int fd, int slot,
                         const void *data, size_t len,
                         int (*checkpoint)(void *), void *opaque)
{
    const unsigned char *bytes = data;
    if (!owner || (!data && len)) {
        errno = EINVAL;
        return -1;
    }
    if (!len)
        return 0;
    if (*owner && slot >= 0 && (*owner)->standard[slot] !=
        GetStdHandle(slot ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE)) {
        snag_output_broker_close(*owner);
        *owner = NULL;
    }
    if (!*owner && !(*owner = broker_open(checkpoint, opaque, false)))
        return -1;
    struct snag_output_broker *broker = *owner;
    while (len) {
        HANDLE remote = NULL;
        size_t amount = len < 4096u ? len : 4096u;
        while (amount < len && amount && (bytes[amount] & 0xc0u) == 0x80u)
            --amount;
        if (!amount)
            amount = len < 4096u ? len : 4096u;
        if (slot < 0 && !DuplicateHandle(GetCurrentProcess(), (HANDLE)_get_osfhandle(fd),
            broker->process, &remote, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
            child_error(GetLastError());
            goto fail;
        }
        uint64_t packet[3] = {slot < 0 ? BROKER_WRITE : BROKER_WRITE_STANDARD,
                              slot < 0 ? (uintptr_t)remote : (uint64_t)slot, amount};
        int32_t status;
        if (broker_transfer(broker, true, packet, sizeof(packet), 0, checkpoint, opaque) < 0 ||
            broker_transfer(broker, true, (void *)bytes, amount, 0, checkpoint, opaque) < 0 ||
            broker_transfer(broker, false, &status, sizeof(status), 0, checkpoint, opaque) < 0)
            goto fail;
        if (status) {
            errno = status;
            goto fail;
        }
        bytes += amount;
        len -= amount;
    }
    return 0;
fail:
    {
        int error = errno;
        snag_output_broker_close(broker);
        *owner = NULL;
        errno = error;
    }
    return -1;
}

int
snag_output_broker_write(struct snag_output_broker **owner, int fd,
                         const void *data, size_t len,
                         int (*checkpoint)(void *), void *opaque)
{
    return broker_write(owner, fd, -1, data, len, checkpoint, opaque);
}

int
snag_output_broker_write_standard(struct snag_output_broker **owner, unsigned int slot,
                                  const void *data, size_t len,
                                  int (*checkpoint)(void *), void *opaque)
{
    if (slot >= 2u) {
        errno = EINVAL;
        return -1;
    }
    HANDLE source = GetStdHandle(slot ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
    DWORD flags, mode;
    if (!GetConsoleMode(source, &mode) || !GetHandleInformation(source, &flags) ||
        !(flags & HANDLE_FLAG_INHERIT)) {
        errno = ENOTSUP;
        return -1;
    }
    return broker_write(owner, -1, (int)slot, data, len, checkpoint, opaque);
}

static bool
broker_child_transfer(HANDLE pipe, bool write, void *data, DWORD size)
{
    unsigned char *bytes = data;
    while (size) {
        DWORD count;
        BOOL ok = write ? WriteFile(pipe, bytes, size, &count, NULL) :
                          ReadFile(pipe, bytes, size, &count, NULL);
        if (!ok)
            return false;
        if (!count) {
            SetLastError(ERROR_BROKEN_PIPE);
            return false;
        }
        bytes += count;
        size -= count;
    }
    return true;
}

int
snag_input_broker_read(struct snag_output_broker **owner, wchar_t *text, size_t capacity,
                       int (*checkpoint)(void *), void *opaque)
{
    if (!owner || !text || !capacity || capacity > 256u) {
        errno = EINVAL;
        return -1;
    }
    if (!*owner && !(*owner = broker_open(checkpoint, opaque, false)))
        return -1;
    if (checkpoint && checkpoint(opaque) < 0)
        return -1;
    uint64_t request[2] = {BROKER_READ_CONSOLE, capacity};
    uint32_t reply[2];
    if (broker_transfer(*owner, true, request, sizeof(request), 0, checkpoint, opaque) < 0 ||
        broker_transfer(*owner, false, reply, sizeof(reply), 0, checkpoint, opaque) < 0)
        return -1;
    if (reply[0])
        return child_error(reply[0]);
    if (reply[1] > capacity) {
        errno = EIO;
        return -1;
    }
    if (broker_transfer(*owner, false, text, reply[1] * sizeof(wchar_t), 0, checkpoint, opaque) < 0)
        return -1;
    return (int)reply[1];
}

static unsigned int __stdcall
broker_parent_wait(void *parent)
{
    (void)WaitForSingleObject(parent, INFINITE);
    EnterCriticalSection(&broker_spawn_lock);
    HANDLE job = atomic_load(&broker_owned_job);
    if (job)
        (void)TerminateJobObject(job, 125u);
    ExitProcess(125u);
    return 0;
}

static bool
broker_child_spawn(HANDLE pipe, HANDLE parent, bool pty)
{
    struct broker_spawn_request request;
    struct broker_spawn_reply reply = {0};
    wchar_t *text[4] = {0};
    HANDLE streams[3] = {0};
    HANDLE console_streams[2] = {INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE};
    PROCESS_INFORMATION child = {0};
    bool sent = false;
    void *collector = NULL;
    if (atomic_load(&broker_owned_job) ||
        !broker_child_transfer(pipe, false, &request, sizeof(request)))
        return false;
    if (!request.job || (uint64_t)(uintptr_t)request.job != request.job)
        return false;
    HANDLE job = (HANDLE)(uintptr_t)request.job;
    atomic_store(&broker_owned_job, job);
    for (size_t i = 0; i < 3u; ++i) {
        if (pty && i == 1u)
            continue;
        if (!request.streams[i] || (uint64_t)(uintptr_t)request.streams[i] != request.streams[i])
            goto out;
        streams[i] = (HANDLE)(uintptr_t)request.streams[i];
    }
    for (size_t i = 0; i < 4u; ++i) {
        uint32_t units = request.units[i];
        if (units < (i == 3u ? 2u : 1u) ||
            units > (i == 3u ? SNAG_MEMORY_LIMIT / sizeof(wchar_t) : 32768u))
            goto out;
        text[i] = malloc((size_t)units * sizeof(wchar_t));
        if (!text[i] || !broker_child_transfer(pipe, false, text[i], units * sizeof(wchar_t)))
            goto out;
        if (text[i][units - 1u] ||
            (i == 3u ? text[i][units - 2u] != 0 : wcslen(text[i]) != units - 1u))
            goto out;
    }
    STARTUPINFOW startup = {.cb = sizeof(startup), .dwFlags = STARTF_USESTDHANDLES,
        .hStdInput = streams[2], .hStdOutput = streams[0], .hStdError = streams[1]};
    if (pty) {
#ifdef SNAG_LEGACY_PTY
        if (!request.columns || !request.rows || request.columns > 2500u || request.rows > 2000u)
            goto out;
        SECURITY_ATTRIBUTES security = {sizeof(security), NULL, TRUE};
        HANDLE input = console_streams[0] = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, NULL);
        HANDLE output = console_streams[1] = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, NULL);
        if (input == INVALID_HANDLE_VALUE || output == INVALID_HANDLE_VALUE)
            goto out;
        if (!SetStdHandle(STD_INPUT_HANDLE, input) || !SetStdHandle(STD_OUTPUT_HANDLE, output) ||
            !SetStdHandle(STD_ERROR_HANDLE, output))
            goto out;
        collector = snag_legacy_console_open(streams[2], streams[0], pipe, job,
            (COORD){(SHORT)request.columns, (SHORT)request.rows});
        if (!collector)
            goto out;
        startup.hStdInput = input;
        startup.hStdOutput = startup.hStdError = output;
#else
        goto out;
#endif
    } else {
        for (size_t i = 0; i < 3u; ++i)
            if (!SetHandleInformation(streams[i], HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT))
                goto out;
    }
    /* Parent death cannot interleave between creation and job assignment. */
    EnterCriticalSection(&broker_spawn_lock);
    if (!CreateProcessW(text[0], text[1], NULL, NULL, TRUE,
        CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | (pty ? 0 : CREATE_NEW_PROCESS_GROUP),
        text[3], text[2], &startup, &child)) {
        reply.error = GetLastError();
    } else if (!AssignProcessToJobObject(job, child.hProcess)) {
        reply.error = GetLastError();
        (void)TerminateProcess(child.hProcess, 125u);
    } else {
        HANDLE remote;
        if (!DuplicateHandle(GetCurrentProcess(), child.hProcess, parent, &remote,
                              0, FALSE, DUPLICATE_SAME_ACCESS))
            reply.error = GetLastError();
        else {
            reply.process = (uintptr_t)remote;
            reply.pid = child.dwProcessId;
        }
    }
    LeaveCriticalSection(&broker_spawn_lock);
    sent = broker_child_transfer(pipe, true, &reply, sizeof(reply));
    if (sent && !reply.error && ResumeThread(child.hThread) == (DWORD)-1)
        sent = false;
out:
    if (!sent || reply.error)
        (void)TerminateJobObject(job, 125u);
    if (child.hThread)
        (void)CloseHandle(child.hThread);
    if (child.hProcess)
        (void)CloseHandle(child.hProcess);
    for (size_t i = 0; i < 4u; ++i) {
        if (text[i]) {
            volatile wchar_t *wipe = text[i];
            for (size_t j = 0; j < request.units[i]; ++j)
                wipe[j] = 0;
        }
        free(text[i]);
    }
#ifdef SNAG_LEGACY_PTY
    if (collector && sent && !reply.error) {
        sent = snag_legacy_console_run(collector) == 0;
        uint32_t status = sent ? 0 : ERROR_GEN_FAILURE;
        sent = broker_child_transfer(pipe, true, &status, sizeof(status)) && sent;
    } else
        snag_legacy_console_free(collector);
#else
    (void)collector;
#endif
    /* Publish collector completion before the last output handle reaches EOF. */
    for (size_t i = 0; i < 3u; ++i)
        if (streams[i])
            (void)CloseHandle(streams[i]);
    for (size_t i = 0; i < 2u; ++i)
        if (console_streams[i] != INVALID_HANDLE_VALUE)
            (void)CloseHandle(console_streams[i]);
    return sent && !reply.error;
}

int
snag_output_broker_main(int argc, wchar_t **argv)
{
    if (argc < 2 || wcscmp(argv[1], broker_option))
        return -1;
    size_t prefix = wcslen(broker_prefix);
    if (argc != 3 || wcslen(argv[2]) != prefix + SNAG_ID_HEX_LEN ||
        wcsncmp(argv[2], broker_prefix, prefix))
        return 125;
    for (size_t i = prefix; argv[2][i]; ++i)
        if (!((argv[2][i] >= L'0' && argv[2][i] <= L'9') ||
              (argv[2][i] >= L'a' && argv[2][i] <= L'f')))
            return 125;
    const DWORD streams[] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
    HANDLE standard[2] = {NULL, NULL};
    for (size_t i = 0; i < 2u; ++i) {
        HANDLE source = GetStdHandle(streams[i + 1u]);
        DWORD mode;
        if (GetConsoleMode(source, &mode))
            (void)DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(),
                                   &standard[i], 0, FALSE, DUPLICATE_SAME_ACCESS);
    }
    for (int fd = 0; fd < 3; ++fd)
        (void)_close(fd);
    for (size_t i = 0; i < 3u; ++i)
        (void)SetStdHandle(streams[i], INVALID_HANDLE_VALUE);
    HANDLE pipe = CreateFileW(argv[2], GENERIC_READ | GENERIC_WRITE, 0, NULL,
                              OPEN_EXISTING, 0, NULL);
    if (pipe == INVALID_HANDLE_VALUE) {
        for (size_t i = 0; i < 2u; ++i)
            if (standard[i])
                (void)CloseHandle(standard[i]);
        return 125;
    }
    uint64_t target[2];
    int result = 125;
    HANDLE input = INVALID_HANDLE_VALUE;
    if (!broker_child_transfer(pipe, false, target, sizeof(target)) ||
        !target[0] || (uint64_t)(uintptr_t)target[0] != target[0] ||
        !target[1] || (uint64_t)(uintptr_t)target[1] != target[1])
        goto out;
    HANDLE mapping = (HANDLE)(uintptr_t)target[0];
    const void *view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 16u);
    if (!view) {
        (void)CloseHandle(mapping);
        goto out;
    }
    unsigned char nonce[16];
    memcpy(nonce, view, sizeof(nonce));
    (void)UnmapViewOfFile(view);
    (void)CloseHandle(mapping);
    InitializeCriticalSection(&broker_spawn_lock);
    HANDLE watcher = (HANDLE)_beginthreadex(NULL, 0, broker_parent_wait,
                                            (void *)(uintptr_t)target[1], 0, NULL);
    if (!watcher)
        goto out;
    (void)CloseHandle(watcher);
    if (!broker_child_transfer(pipe, true, nonce, sizeof(nonce)))
        goto out;
    for (;;) {
        uint64_t operation, packet[2];
        unsigned char bytes[4096];
        if (!broker_child_transfer(pipe, false, &operation, sizeof(operation))) {
            if (GetLastError() == ERROR_BROKEN_PIPE || GetLastError() == ERROR_NO_DATA)
                result = 0;
            break;
        }
        if (operation == BROKER_SPAWN || operation == BROKER_PTY) {
            if (!broker_child_spawn(pipe, (HANDLE)(uintptr_t)target[1], operation == BROKER_PTY))
                break;
            if (operation == BROKER_PTY) {
                result = 0;
                break;
            }
            continue;
        }
        if (operation == BROKER_READ_CONSOLE) {
            uint64_t capacity;
            uint32_t reply[2] = {0};
            wchar_t text[256];
            if (!broker_child_transfer(pipe, false, &capacity, sizeof(capacity)) ||
                !capacity || capacity > 256u)
                break;
            if (input == INVALID_HANDLE_VALUE)
                input = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
            DWORD got = 0;
            if (input == INVALID_HANDLE_VALUE ||
                !ReadConsoleW(input, text, (DWORD)capacity, &got, NULL))
                reply[0] = GetLastError();
            else
                reply[1] = got;
            bool sent = broker_child_transfer(pipe, true, reply, sizeof(reply)) &&
                        broker_child_transfer(pipe, true, text, reply[1] * sizeof(wchar_t));
            volatile wchar_t *wipe = text;
            for (size_t i = 0; i < 256u; ++i)
                wipe[i] = 0;
            if (!sent)
                break;
            continue;
        }
        if ((operation != BROKER_WRITE && operation != BROKER_WRITE_STANDARD) ||
            !broker_child_transfer(pipe, false, packet, sizeof(packet)))
            break;
        if (!packet[1] || packet[1] > sizeof(bytes) ||
            (operation == BROKER_WRITE_STANDARD ? packet[0] >= 2u :
             !packet[0] || (uint64_t)(uintptr_t)packet[0] != packet[0]))
            break;
        HANDLE output = operation == BROKER_WRITE ? (HANDLE)(uintptr_t)packet[0] : NULL;
        if (!broker_child_transfer(pipe, false, bytes, (DWORD)packet[1])) {
            if (output)
                (void)CloseHandle(output);
            break;
        }
        int32_t status = 0;
        if (operation == BROKER_WRITE_STANDARD &&
            (!standard[packet[0]] || !DuplicateHandle(GetCurrentProcess(), standard[packet[0]],
                GetCurrentProcess(), &output, 0, FALSE, DUPLICATE_SAME_ACCESS)))
            status = ENOTSUP;
        int fd = status ? -1 :
            _open_osfhandle((intptr_t)output, _O_WRONLY | _O_BINARY | _O_NOINHERIT);
        if (fd < 0) {
            if (!status)
                status = errno;
            if (output)
                (void)CloseHandle(output);
        } else {
            if (snag_term_output_write(NULL, fd, bytes, (size_t)packet[1], false, NULL, NULL) < 0)
                status = errno;
            if (_close(fd) < 0 && !status)
                status = errno;
        }
        if (!broker_child_transfer(pipe, true, &status, sizeof(status)))
            break;
    }
out:
    for (size_t i = 0; i < 2u; ++i)
        if (standard[i])
            (void)CloseHandle(standard[i]);
    if (input != INVALID_HANDLE_VALUE)
        (void)CloseHandle(input);
    {
        HANDLE job = atomic_load(&broker_owned_job);
        if (job)
            (void)TerminateJobObject(job, 125u);
    }
    (void)CloseHandle(pipe);
    return result;
}

void
snag_child_close_stream(struct snag_child *child, unsigned int stream)
{
    if (!child->native || stream >= 3u || (child->pty && stream == 2u))
        return;
    struct child_pipe *pipe = &child->native->pipe[stream];
    if (pipe->handle) {
        pipe_cancel(pipe);
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
        pipe_cancel(pipe);
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
    snag_output_broker_close(native->broker);
    if (native->process) {
        (void)WaitForSingleObject(native->process, INFINITE);
        (void)CloseHandle(native->process);
    }
    /* Closing host pipe ends also prevents ConPTY shutdown from blocking on output. */
    child->pty = false;
    for (unsigned int i = 0; i < 3u; ++i)
        snag_child_close_stream(child, i);
    if (native->console_closer) {
        (void)WaitForSingleObject(native->console_closer, INFINITE);
        (void)CloseHandle(native->console_closer);
    } else if (native->console)
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

static int
broker_spawn(struct snag_child_windows *native, HANDLE ends[3],
             wchar_t *exe, wchar_t *line, wchar_t *cwd, struct snag_buf *environment)
{
    struct broker_spawn_request request = {0};
    struct broker_spawn_reply reply;
    wchar_t *texts[] = {exe, line, cwd, (wchar_t *)environment->data};
    struct snag_output_broker *broker = broker_open(NULL, NULL, native->legacy_console);
    if (!broker)
        return -1;
    native->broker = broker;
    HANDLE remote;
    if (!DuplicateHandle(GetCurrentProcess(), native->job, broker->process, &remote,
                          JOB_OBJECT_ASSIGN_PROCESS | JOB_OBJECT_TERMINATE |
                          (native->legacy_console ? JOB_OBJECT_QUERY : 0), FALSE, 0))
        return child_error(GetLastError());
    request.job = (uintptr_t)remote;
    for (size_t i = 0; i < 3u; ++i) {
        if (native->legacy_console && i == 1u)
            continue;
        if (!DuplicateHandle(GetCurrentProcess(), ends[i], broker->process, &remote,
                              0, FALSE, DUPLICATE_SAME_ACCESS))
            return child_error(GetLastError());
        request.streams[i] = (uintptr_t)remote;
    }
    for (size_t i = 0; i < 4u; ++i)
        request.units[i] = (uint32_t)(i == 3u ? environment->len / sizeof(wchar_t) :
                                     wcslen(texts[i]) + 1u);
    request.columns = (uint32_t)native->dimensions.X;
    request.rows = (uint32_t)native->dimensions.Y;
    uint64_t operation = native->legacy_console ? BROKER_PTY : BROKER_SPAWN;
    uint64_t deadline = snag_monotonic_ms() + 5000u;
    if (broker_transfer(broker, true, &operation, sizeof(operation), deadline, NULL, NULL) < 0 ||
        broker_transfer(broker, true, &request, sizeof(request), deadline, NULL, NULL) < 0)
        return -1;
    for (size_t i = 0; i < 4u; ++i)
        if (broker_transfer(broker, true, texts[i], (size_t)request.units[i] * sizeof(wchar_t),
                             deadline, NULL, NULL) < 0)
            return -1;
    if (broker_transfer(broker, false, &reply, sizeof(reply), deadline, NULL, NULL) < 0)
        return -1;
    if (reply.error)
        return child_error(reply.error);
    if (!reply.process || (uint64_t)(uintptr_t)reply.process != reply.process || !reply.pid) {
        errno = EIO;
        return -1;
    }
    native->process = (HANDLE)(uintptr_t)reply.process;
    native->pid = reply.pid;
    return 0;
}

static int
child_spawn(struct snag_child *child, const char *shell, const char *command,
            const char *directory, char **environment, bool pty, bool isolated)
{
    struct snag_child_windows *native = calloc(1, sizeof(*native));
    HANDLE ends[3] = {0};
    struct snag_buf line, env;
    wchar_t *exe = NULL, *cwd = NULL, *text = NULL;
    struct { STARTUPINFOW StartupInfo; void *lpAttributeList; } startup = {0};
    BOOL (WINAPI *attributes_init)(void *, DWORD, DWORD, SIZE_T *);
    BOOL (WINAPI *attributes_update)(void *, DWORD, DWORD_PTR, void *, SIZE_T, void *, SIZE_T *);
    void (WINAPI *attributes_delete)(void *);
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    FARPROC function = GetProcAddress(kernel, "InitializeProcThreadAttributeList");
    memcpy(&attributes_init, &function, sizeof(attributes_init));
    function = GetProcAddress(kernel, "UpdateProcThreadAttribute");
    memcpy(&attributes_update, &function, sizeof(attributes_update));
    function = GetProcAddress(kernel, "DeleteProcThreadAttributeList");
    memcpy(&attributes_delete, &function, sizeof(attributes_delete));
    isolated |= !attributes_init || !attributes_update || !attributes_delete;
    PROCESS_INFORMATION process = {0};
    int rc = -1;
    SIZE_T attributes = 0;
    bool attributes_ready = false;

    if (!native)
        return -1;
    child->native = native;
    child->pty = pty;
#ifdef SNAG_LEGACY_PTY
    native->legacy_console = pty && (isolated || !GetProcAddress(kernel, "CreatePseudoConsole") ||
        !GetProcAddress(kernel, "ClosePseudoConsole") || !GetProcAddress(kernel, "ResizePseudoConsole"));
    isolated |= native->legacy_console;
    if (native->legacy_console) {
        native->dimensions = console_dimensions();
        if (native->dimensions.X > 2500)
            native->dimensions.X = 2500;
        if (native->dimensions.Y > 2000)
            native->dimensions.Y = 2000;
    }
#endif
    snag_buf_init(&line, 256u * 1024u + 32768u);
    snag_buf_init(&env, SNAG_MEMORY_LIMIT);
    exe = snag_utf8_to_wide(shell);
    cwd = snag_utf8_to_wide(directory);
    if (!exe || !cwd)
        goto out;
    for (wchar_t *p = exe; *p; ++p)
        if (*p == L'/')
            *p = L'\\';
    char *native_shell = snag_wide_to_utf8(exe);
    if (!native_shell)
        goto out;
    int prefix = snag_buf_printf(&line, "\"%s\"", native_shell);
    free(native_shell);
    if (prefix < 0)
        goto out;
    const wchar_t *base = wcsrchr(exe, L'\\');
    const wchar_t *slash = wcsrchr(exe, L'/');
    if (slash && (!base || slash > base))
        base = slash;
    base = base ? base + 1 : exe;
    if (!_wcsicmp(base, L"cmd.exe") || !_wcsicmp(base, L"cmd")) {
        if (snag_buf_printf(&line, " /d /q /v:off /s /c \"%s\"", command) < 0)
            goto out;
    } else {
        /* A configured POSIX shell still receives one -c argument using CRT quoting. */
        if (snag_buf_append(&line, " -c \"", 5u) < 0)
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
        if ((!pty || i != 1u) &&
            create_pipe(&native->pipe[i], i == 2u, &ends[i], native->legacy_console) < 0)
            goto out;
    native->job = CreateJobObjectW(NULL, NULL);
    if (!native->job)
        goto native_error;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    /* The isolated broker explicitly terminates this job on parent death. */
    if (!isolated && !SetInformationJobObject(native->job, JobObjectExtendedLimitInformation,
                                               &limits, sizeof(limits)))
        goto native_error;
    if (isolated) {
        if (pty && !native->legacy_console) {
            errno = ENOTSUP; /* Native hidden-console collection is separate. */
            goto out;
        }
        rc = broker_spawn(native, ends, exe, text, cwd, &env);
        goto out;
    }
    if (pty) {
        HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
        HRESULT (WINAPI *create)(COORD, HANDLE, HANDLE, DWORD, HANDLE *);
        FARPROC function = GetProcAddress(kernel, "CreatePseudoConsole");
        memcpy(&create, &function, sizeof(create));
        function = GetProcAddress(kernel, "ClosePseudoConsole");
        memcpy(&native->console_close, &function, sizeof(native->console_close));
        function = GetProcAddress(kernel, "ResizePseudoConsole");
        memcpy(&native->console_resize, &function, sizeof(native->console_resize));
        function = GetProcAddress(kernel, "ReleasePseudoConsole");
        memcpy(&native->console_release, &function, sizeof(native->console_release));
        native->dimensions = console_dimensions();
        if (!create || !native->console_close || !native->console_resize) {
            errno = ENOTSUP;
            goto out;
        }
        if (FAILED(create(native->dimensions, ends[2], ends[0], 0, &native->console)))
            goto native_error;
    }
    (void)attributes_init(NULL, 1u, 0, &attributes);
    startup.lpAttributeList = malloc(attributes);
    if (!startup.lpAttributeList)
        goto out;
    if (!attributes_init(startup.lpAttributeList, 1u, 0, &attributes))
        goto native_error;
    attributes_ready = true;
    if (!attributes_update(startup.lpAttributeList, 0,
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
    if (native->console && native->console_release && FAILED(native->console_release(native->console)))
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
            attributes_delete(startup.lpAttributeList);
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
snag_child_spawn(struct snag_child *child, const char *shell, const char *command,
                 const char *directory, char **environment, bool pty)
{
    return child_spawn(child, shell, command, directory, environment, pty, false);
}

int
snag_child_spawn_isolated(struct snag_child *child, const char *shell, const char *command,
                          const char *directory, char **environment)
{
    return child_spawn(child, shell, command, directory, environment, false, true);
}

#ifdef SNAG_LEGACY_PTY
int
snag_child_spawn_legacy_pty(struct snag_child *child, const char *shell, const char *command,
                            const char *directory, char **environment)
{
    return child_spawn(child, shell, command, directory, environment, true, true);
}
#endif

static unsigned int __stdcall
close_console(void *opaque)
{
    struct snag_child_windows *native = opaque;
    native->console_close(native->console);
    return 0;
}

static int
finish_console(struct snag_child_windows *native)
{
    if (!native->console || native->console_release || native->console_closer)
        return 0;
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info;
    if (!QueryInformationJobObject(native->job, JobObjectBasicAccountingInformation, &info, sizeof(info), NULL))
        return child_error(GetLastError());
    if (!info.ActiveProcesses) {
        /* Older ClosePseudoConsole may block until this owner drains output. */
        native->console_closer = (HANDLE)_beginthreadex(NULL, 0, close_console, native, 0, NULL);
        if (!native->console_closer)
            return -1;
    }
    return 0;
}

int
snag_child_exited(struct snag_child *child)
{
    struct snag_child_windows *native = child->native;
    if (native->legacy_console && WaitForSingleObject(native->broker->process, 0) == WAIT_OBJECT_0 &&
        WaitForSingleObject(native->process, 0) == WAIT_TIMEOUT)
        (void)TerminateJobObject(native->job, 125u);
    if (finish_console(child->native) < 0)
        return -1;
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
#ifdef SNAG_LEGACY_PTY
    if (native && native->legacy_console) {
        struct child_pipe *control = &native->broker->control;
        if (control->pending && !pipe_done(control))
            return;
        if (control->error) {
            (void)TerminateJobObject(native->job, 125u);
            return;
        }
        COORD size = console_dimensions();
        if (size.X > 2500)
            size.X = 2500;
        if (size.Y > 2000)
            size.Y = 2000;
        uint32_t packet[2] = {(uint32_t)size.X, (uint32_t)size.Y};
        if (size.X != native->dimensions.X || size.Y != native->dimensions.Y) {
            memcpy(control->bytes, packet, sizeof(packet));
            pipe_begin(control, true, sizeof(packet));
            native->dimensions = size;
        }
        return;
    }
#endif
    if (!native || !native->console || native->console_closer)
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
    if (pipe->error) {
        if (pipe->error != ERROR_BROKEN_PIPE && pipe->error != ERROR_NO_DATA)
            return child_error(pipe->error);
        struct snag_child_windows *native = child->native;
        if (native->legacy_console && !stream && !native->collector_done) {
            uint32_t status;
            pipe_cancel(&native->broker->control);
            int received = broker_transfer(native->broker, false, &status, sizeof(status),
                                             snag_monotonic_ms() + 1000u, NULL, NULL);
            native->collector_done = true;
            native->collector_error = received < 0 || status ? EIO : 0;
        }
        if (native->collector_error) {
            errno = native->collector_error;
            return -1;
        }
        return 0;
    }
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
    if (count > 96u || timeout_ms < -1) {
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
            if (finish_console(native) < 0) {
                rc = -1;
                goto done;
            }
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
        if (rc || (timeout_ms >= 0 && elapsed >= (uint64_t)timeout_ms))
            break;
        DWORD delay = timeout_ms < 0 ? INFINITE : (DWORD)((uint64_t)timeout_ms - elapsed);
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
done:
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
#define SNAJPAGENT_HAVE_PROC_CHILD 1
#include <pty.h>
#include <sys/ioctl.h>
#elif defined(__APPLE__) || defined(__OpenBSD__) || defined(__NetBSD__)
#define SNAJPAGENT_HAVE_PTY 1
#include <sys/ioctl.h>
#include <util.h>
#elif defined(__FreeBSD__)
#define SNAJPAGENT_HAVE_PTY 1
#include <sys/ioctl.h>
#include <libutil.h>
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
#if defined(__FreeBSD__) && !defined(WNOWAIT)
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#endif
#if (defined(__OpenBSD__) && !defined(WNOWAIT)) || (defined(__NetBSD__) && !defined(WEXITED))
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/proc.h>
#endif

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
    char *args[] = {(char *)shell, "-c", (char *)command, NULL};
    execve(shell, args, env);
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
    return snag_errno(ENOTSUP);
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

#if defined(SNAJPAGENT_HAVE_PROC_CHILD)
static int
proc_child_exited(struct snag_child *child)
{
    char path[64], record[1024], state, *end;
    long pid, parent;
    int length = snprintf(path, sizeof(path), "/proc/%ld/stat", (long)child->pid);
    if (length < 0 || (size_t)length >= sizeof(path)) {
        errno = EOVERFLOW;
        return -1;
    }
    int fd = snag_open_read(path, false);
    if (fd < 0) {
        if (errno == ENOENT || errno == ESRCH)
            errno = ECHILD;
        return -1;
    }
    ssize_t n;
    do {
        n = read(fd, record, sizeof(record) - 1u);
    } while (n < 0 && errno == EINTR);
    int error = errno;
    (void)close(fd);
    if (n < 0) {
        errno = error == ENOENT || error == ESRCH ? ECHILD : error;
        return -1;
    }
    record[n] = '\0';
    /* comm may contain spaces and parentheses; the final ')' ends it. */
    pid = strtol(record, &end, 10);
    char *comm_end = strrchr(end, ')');
    if (pid != child->pid || strncmp(end, " (", 2u) || !comm_end ||
        sscanf(comm_end + 1, " %c %ld", &state, &parent) != 2) {
        errno = EIO;
        return -1;
    }
    if (parent != (long)getpid()) {
        errno = ECHILD;
        return -1;
    }
    return state == 'Z';
}
#endif

int
snag_child_exited(struct snag_child *child)
{
#if (defined(__OpenBSD__) && !defined(WNOWAIT)) || (defined(__NetBSD__) && !defined(WEXITED))
#ifdef KERN_PROC2
    struct kinfo_proc2 info = {0};
    int mib[] = {CTL_KERN, KERN_PROC2, KERN_PROC_PID, child->pid, sizeof(info), 1};
#else
    struct kinfo_proc info = {0};
    int mib[] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, child->pid, sizeof(info), 1};
#endif
    size_t size = sizeof(info);
    if (sysctl(mib, 6u, &info, &size, NULL, 0) < 0)
        return -1;
    if (size != sizeof(info) || info.p_pid != child->pid || info.p_ppid != getpid()) {
        child->reaped = true;
        return snag_errno(ECHILD);
    }
    /* Preserve waitpid ownership while observing the native zombie state. */
#ifdef KERN_PROC2
    return info.p_stat == SZOMB;
#else
    return (info.p_psflags & PS_ZOMBIE) != 0;
#endif
#elif defined(__FreeBSD__) && !defined(WNOWAIT)
    /* KERN_PROC_PID omits zombies on old FreeBSD; the process list includes
     * them. Validate parentage and leave reaping exclusively to the owner. */
#ifdef KERN_PROC_PROC
    int mib[] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC};
#else
    int mib[] = {CTL_KERN, KERN_PROC, KERN_PROC_ALL};
#endif
    size_t size = 0;
    if (sysctl(mib, 3u, NULL, &size, NULL, 0) < 0)
        return -1;
    struct kinfo_proc *list = malloc(size ? size : 1u);
    if (!list)
        return -1;
    int rc = sysctl(mib, 3u, list, &size, NULL, 0), error = errno;
    if (rc < 0) {
        free(list);
        if (error == ENOMEM || error == EAGAIN)
            return 0; /* The snapshot changed; retry on the next poll. */
        errno = error;
        return -1;
    }
    rc = -1;
    error = ECHILD;
    if (size % sizeof(*list)) {
        error = EIO;
    } else for (size_t i = 0; i < size / sizeof(*list); ++i) {
        bool matches = list[i].ki_pid == child->pid;
#ifndef KERN_PROC_PROC
        /* 5.1 omits ki_pid for zombies. Each managed child leads its own
         * group; require that group and our parentage, never parent alone. */
        matches |= list[i].ki_pid == 0 && list[i].ki_stat == SZOMB &&
                   list[i].ki_pgid == child->pid;
#endif
        if (matches && list[i].ki_ppid == getpid()) {
            /* 5.5 fill_kinfo_thread reports zombies as SIDL; its list
             * skips newborns. Require the exit flag as well as that state. */
            rc = list[i].ki_stat == SZOMB ||
                 (list[i].ki_stat == SIDL && (list[i].ki_flag & P_WEXIT));
            break;
        }
    }
    free(list);
    if (rc < 0) {
        child->reaped = error == ECHILD;
        errno = error;
    }
    return rc;
#elif defined(__FreeBSD__)
    /* FreeBSD supports polling without releasing child ownership. */
    int status;
    pid_t pid = waitpid(child->pid, &status, WNOHANG | WNOWAIT);
    if (pid < 0 && errno == ECHILD)
        child->reaped = true;
    return pid < 0 ? -1 : pid == child->pid;
#else
    siginfo_t info = {0};
    if (waitid(P_PID, (id_t)child->pid, &info, WEXITED | WNOHANG | WNOWAIT) < 0) {
#if defined(SNAJPAGENT_HAVE_PROC_CHILD)
        if (errno == ENOSYS) {
            int rc = proc_child_exited(child);
            if (rc >= 0)
                return rc;
        }
#endif
        if (errno == ECHILD)
            child->reaped = true; /* Never signal a reused PID after ownership loss. */
        return -1;
    }
    return info.si_pid == child->pid;
#endif
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
    if (count > 96u)
        return snag_errno(EINVAL);
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

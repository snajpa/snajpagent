/* SPDX-License-Identifier: GPL-2.0-only */
#include "term_host.h"
#include "base.h"
#include "fs.h"
#include "net.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <stdio.h>
#include <stdatomic.h>
#include <signal.h>

#if (defined(__APPLE__) && __ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__ < 1070) || \
    (defined(__FreeBSD__) && __FreeBSD__ < 6)
#include <pthread.h>
static pthread_key_t output_owner;
static pthread_once_t output_owner_once = PTHREAD_ONCE_INIT;

static void
output_owner_init(void)
{
    if (pthread_key_create(&output_owner, NULL) != 0)
        abort();
}

struct snag_term *
snag_term_output_owner(void)
{
    if (pthread_once(&output_owner_once, output_owner_init) != 0)
        abort();
    return pthread_getspecific(output_owner);
}

void
snag_term_output_bind(struct snag_term *term)
{
    if (pthread_once(&output_owner_once, output_owner_init) != 0 ||
        pthread_setspecific(output_owner, term) != 0)
        abort();
}
#else
static _Thread_local struct snag_term *output_owner;

struct snag_term *
snag_term_output_owner(void)
{
    return output_owner;
}

void
snag_term_output_bind(struct snag_term *term)
{
    output_owner = term;
}
#endif

#ifdef _WIN32
#include "process_host.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#include <process.h>
#include <pthread.h>

static pthread_mutex_t console_read_lock = PTHREAD_MUTEX_INITIALIZER;
static HANDLE console_reader;
static struct snag_output_broker *console_read_broker;
static atomic_bool console_read_cancelled;

typedef BOOL (WINAPI *cancel_sync_fn)(HANDLE);
static cancel_sync_fn cancel_sync;
static pthread_once_t cancel_sync_once = PTHREAD_ONCE_INIT;

static void
find_cancel_sync(void)
{
    FARPROC function = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "CancelSynchronousIo");
    memcpy(&cancel_sync, &function, sizeof(cancel_sync));
}

static cancel_sync_fn
synchronous_cancel(void)
{
    if (pthread_once(&cancel_sync_once, find_cancel_sync) != 0)
        abort();
    return cancel_sync;
}

static void
control_lock(pthread_mutex_t *mutex)
{
    if (pthread_mutex_lock(mutex) != 0)
        abort();
}

static void
control_unlock(pthread_mutex_t *mutex)
{
    if (pthread_mutex_unlock(mutex) != 0)
        abort();
}

static void
cancel_console_read(void)
{
    atomic_store(&console_read_cancelled, true);
    for (;;) {
        control_lock(&console_read_lock);
        bool active = console_reader != NULL;
        cancel_sync_fn cancel = synchronous_cancel();
        if (console_read_broker)
            snag_output_broker_cancel(console_read_broker);
        BOOL sent = console_read_broker || (active && cancel && cancel(console_reader));
        DWORD error = cancel ? GetLastError() : ERROR_CALL_NOT_IMPLEMENTED;
        control_unlock(&console_read_lock);
        if (!active || sent || error != ERROR_NOT_FOUND)
            break;
        /* The reader either starts its I/O or observes the cancellation flag. */
        Sleep(1u);
    }
}

static int
read_broker_checkpoint(void *opaque)
{
    struct snag_term_host *host = opaque;
    control_lock(&console_read_lock);
    console_read_broker = host->input_broker;
    bool cancelled = atomic_load(&console_read_cancelled);
    control_unlock(&console_read_lock);
    if (cancelled)
        errno = EINTR;
    return cancelled ? -1 : 0;
}

static BOOL
read_console(struct snag_term_host *host, HANDLE input, WCHAR *wide, DWORD size, DWORD *got)
{
    control_lock(&console_read_lock);
    if (console_reader) {
        control_unlock(&console_read_lock);
        SetLastError(ERROR_BUSY);
        return FALSE;
    }
    if (atomic_exchange(&console_read_cancelled, false)) {
        control_unlock(&console_read_lock);
        SetLastError(ERROR_OPERATION_ABORTED);
        return FALSE;
    }
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                          &console_reader, THREAD_TERMINATE, FALSE, 0)) {
        control_unlock(&console_read_lock);
        return FALSE;
    }
    control_unlock(&console_read_lock);
    BOOL ok = FALSE;
    DWORD error = ERROR_OPERATION_ABORTED;
    if (!atomic_load(&console_read_cancelled)) {
        if (host->input_broker || !synchronous_cancel()) {
            int count = snag_input_broker_read(&host->input_broker, wide, size, read_broker_checkpoint, host);
            ok = count >= 0;
            *got = ok ? (DWORD)count : 0;
            error = ok ? ERROR_SUCCESS : errno == EINTR ? ERROR_OPERATION_ABORTED : ERROR_READ_FAULT;
        } else {
            ok = ReadConsoleW(input, wide, size, got, NULL);
            error = GetLastError();
        }
    }
    control_lock(&console_read_lock);
    (void)CloseHandle(console_reader);
    console_reader = NULL;
    console_read_broker = NULL;
    if (atomic_exchange(&console_read_cancelled, false)) {
        ok = FALSE;
        error = ERROR_OPERATION_ABORTED;
    }
    control_unlock(&console_read_lock);
    SetLastError(error);
    return ok;
}

static pthread_mutex_t shutdown_lock = PTHREAD_MUTEX_INITIALIZER;
static struct snag_shutdown *shutdown_owner;

static void
shutdown_signal(int number)
{
    control_lock(&shutdown_lock);
    if (shutdown_owner)
        shutdown_owner->handler(number);
    control_unlock(&shutdown_lock);
    cancel_console_read();
}

static BOOL WINAPI
shutdown_control(DWORD event)
{
    bool closing = event == CTRL_CLOSE_EVENT || event == CTRL_LOGOFF_EVENT || event == CTRL_SHUTDOWN_EVENT;
    if (!closing && event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT)
        return FALSE;
    HANDLE done = NULL;
    control_lock(&shutdown_lock);
    bool owned = shutdown_owner != NULL;
    if (owned) {
        shutdown_owner->handler(closing ? SIGTERM : SIGINT);
        if (closing)
            (void)DuplicateHandle(GetCurrentProcess(), shutdown_owner->done,
                                  GetCurrentProcess(), &done, SYNCHRONIZE, FALSE, 0);
    }
    control_unlock(&shutdown_lock);
    if (owned)
        cancel_console_read();
    if (done) {
        /* Windows grants only a bounded console-close cleanup interval. */
        (void)WaitForSingleObject(done, 4000u);
        (void)CloseHandle(done);
    }
    return owned;
}

void
snag_shutdown_detach(struct snag_shutdown *saved)
{
    if (saved->console) {
        (void)SetConsoleCtrlHandler(shutdown_control, FALSE);
        saved->console = false;
    }
    control_lock(&shutdown_lock);
    if (shutdown_owner == saved)
        shutdown_owner = NULL;
    control_unlock(&shutdown_lock);
    const int numbers[] = {SIGINT, SIGTERM};
    while (saved->count) {
        --saved->count;
        (void)signal(numbers[saved->count], saved->saved[saved->count]);
    }
}

void
snag_shutdown_finish(struct snag_shutdown *saved)
{
    snag_shutdown_detach(saved);
    if (saved->done) {
        (void)SetEvent(saved->done);
        (void)CloseHandle(saved->done);
        saved->done = NULL;
    }
}

int
snag_shutdown_install(struct snag_shutdown *saved, void (*handler)(int), bool hangup)
{
    (void)hangup;
    memset(saved, 0, sizeof(*saved));
    if (!handler)
        return snag_errno(EINVAL);
    saved->done = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!saved->done)
        return snag_errno(EIO);
    control_lock(&shutdown_lock);
    bool busy = shutdown_owner != NULL;
    if (!busy) {
        saved->handler = handler;
        shutdown_owner = saved;
    }
    control_unlock(&shutdown_lock);
    if (busy) {
        snag_shutdown_finish(saved);
        return snag_errno(EBUSY);
    }
    const int numbers[] = {SIGINT, SIGTERM};
    for (size_t i = 0; i < 2u; ++i) {
        saved->saved[i] = signal(numbers[i], shutdown_signal);
        if (saved->saved[i] == SIG_ERR)
            goto fail;
        ++saved->count;
    }
    if (!SetConsoleCtrlHandler(shutdown_control, TRUE))
        goto fail;
    saved->console = true;
    return 0;
fail:
    snag_shutdown_finish(saved);
    return snag_errno(EIO);
}

bool
snag_term_can_suspend(void)
{
    return false;
}

int
snag_term_suspend(void)
{
    return snag_errno(ENOTSUP);
}

struct snag_console_writer {
    HANDLE thread, work, done;
    atomic_bool stop, cancel;
    int fd, error;
    const unsigned char *bytes;
    size_t len;
};

static int
write_native(int fd, const unsigned char *bytes, size_t len, const atomic_bool *cancel)
{
    HANDLE handle = (HANDLE)_get_osfhandle(fd);
    DWORD mode, written;
    bool console = GetConsoleMode(handle, &mode) != 0;
    while (len) {
        if (cancel && atomic_load(cancel))
            return EINTR;
        size_t used = 0;
        if (console) {
            WCHAR wide[1024];
            size_t units = 0;
            while (used < len && units + 2u <= sizeof(wide) / sizeof(wide[0])) {
                uint32_t cp;
                size_t n = snag_utf8_decode(bytes + used, len - used, &cp);
                if (!n) {
                    cp = 0xfffdu;
                    n = 1;
                }
                if (cp == '\n')
                    wide[units++] = L'\r';
                if (cp <= 0xffffu)
                    wide[units++] = (WCHAR)cp;
                else {
                    cp -= 0x10000u;
                    wide[units++] = (WCHAR)(0xd800u + (cp >> 10));
                    wide[units++] = (WCHAR)(0xdc00u + (cp & 0x3ffu));
                }
                used += n;
            }
            size_t at = 0;
            while (at < units) {
                if (!WriteConsoleW(handle, wide + at, (DWORD)(units - at), &written, NULL))
                    goto fail;
                if (!written)
                    return EIO;
                at += written;
            }
        } else {
            DWORD amount = len > 1024u ? 1024u : (DWORD)len;
            if (!WriteFile(handle, bytes, amount, &written, NULL))
                goto fail;
            if (!written)
                return EIO;
            used = written;
        }
        bytes += used;
        len -= used;
    }
    return 0;
fail:
    switch (GetLastError()) {
    case ERROR_OPERATION_ABORTED: return EINTR;
    case ERROR_BROKEN_PIPE: case ERROR_NO_DATA: return EPIPE;
    case ERROR_INVALID_HANDLE: return EBADF;
    default: return EIO;
    }
}

static unsigned int __stdcall
console_writer(void *opaque)
{
    struct snag_console_writer *writer = opaque;
    while (WaitForSingleObject(writer->work, INFINITE) == WAIT_OBJECT_0) {
        if (atomic_load(&writer->stop))
            break;
        writer->error = write_native(writer->fd, writer->bytes, writer->len, &writer->cancel);
        (void)SetEvent(writer->done);
    }
    return 0;
}

void
snag_term_host_close(struct snag_term_host *host)
{
    struct snag_console_writer *writer = host->writer;
    snag_output_broker_close(host->broker);
    host->broker = NULL;
    snag_output_broker_close(host->input_broker);
    host->input_broker = NULL;
    if (host->line_input) {
        (void)CloseHandle(host->line_input);
        host->line_input = NULL;
    }
    /* stdout/stderr can share a console buffer; unwind in reverse order. */
    for (size_t i = 2u; i-- > 0u;)
        if (host->output_console[i]) {
            if (host->output_state[i].legacy)
                (void)SetConsoleTextAttribute(host->output_console[i], host->output_state[i].initial_attributes);
            (void)SetConsoleMode(host->output_console[i], host->output_mode[i]);
            host->output_console[i] = NULL;
            host->output_source[i] = NULL;
        }
    if (!writer)
        return;
    atomic_store(&writer->stop, true);
    (void)SetEvent(writer->work);
    if (writer->thread) {
        (void)WaitForSingleObject(writer->thread, INFINITE);
        (void)CloseHandle(writer->thread);
    }
    if (writer->work)
        (void)CloseHandle(writer->work);
    if (writer->done)
        (void)CloseHandle(writer->done);
    free(writer);
    host->writer = NULL;
}

static int
output_plain(struct snag_term_host *host, int fd,
             const void *text, size_t len, bool input,
             int (*checkpoint)(void *), void *opaque)
{
    (void)input;
    if (!host || !checkpoint) {
        int error = write_native(fd, text, len, NULL);
        if (error)
            errno = error;
        return error ? -1 : 0;
    }
    cancel_sync_fn cancel = synchronous_cancel();
    if (!cancel) {
        HANDLE output = (HANDLE)_get_osfhandle(fd);
        DWORD flags;
        for (unsigned int i = 0; i < 2u; ++i)
            if (host->output_console[i] == output && host->output_source[i] &&
                host->output_source[i] == GetStdHandle(i ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE) &&
                GetHandleInformation(host->output_source[i], &flags) && (flags & HANDLE_FLAG_INHERIT))
                return snag_output_broker_write_standard(&host->broker, i, text, len, checkpoint, opaque);
        return snag_output_broker_write(&host->broker, fd, text, len, checkpoint, opaque);
    }
    if (!host->writer) {
        host->writer = calloc(1, sizeof(*host->writer));
        if (!host->writer)
            return -1;
        host->writer->work = CreateEventW(NULL, FALSE, FALSE, NULL);
        host->writer->done = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (host->writer->work && host->writer->done)
            host->writer->thread = (HANDLE)_beginthreadex(NULL, 0, console_writer, host->writer, 0, NULL);
        if (!host->writer->thread) {
            snag_term_host_close(host);
            return snag_errno(EIO);
        }
    }
    struct snag_console_writer *writer = host->writer;
    writer->fd = fd;
    writer->bytes = text;
    writer->len = len;
    atomic_store(&writer->cancel, false);
    (void)ResetEvent(writer->done);
    (void)SetEvent(writer->work);
    int error = 0;
    for (;;) {
        DWORD rc = WaitForSingleObject(writer->done, 16u);
        if (rc == WAIT_OBJECT_0)
            break;
        if (rc == WAIT_FAILED && !error)
            error = EIO;
        if (!error && checkpoint(opaque) < 0)
            error = errno ? errno : EIO;
        /* Retry cancellation until completion to cover the start-I/O race. */
        if (error) {
            atomic_store(&writer->cancel, true);
            (void)cancel(writer->thread);
        }
    }
    if (!error)
        error = writer->error;
    if (error)
        errno = error;
    return error ? -1 : 0;
}

static int
console_failure(void)
{
    errno = GetLastError() == ERROR_INVALID_HANDLE ? EBADF : EIO;
    return -1;
}

static WORD
console_color(unsigned int color)
{
    return (WORD)(((color & 1u) ? FOREGROUND_RED : 0) |
                  ((color & 2u) ? FOREGROUND_GREEN : 0) |
                  ((color & 4u) ? FOREGROUND_BLUE : 0));
}

static int
console_erase(HANDLE output, const CONSOLE_SCREEN_BUFFER_INFO *info,
              bool display, unsigned int mode)
{
    DWORD width = (DWORD)info->dwSize.X;
    DWORD cursor = (DWORD)info->dwCursorPosition.Y * width + (DWORD)info->dwCursorPosition.X;
    DWORD first = display ? (DWORD)info->srWindow.Top * width : cursor - (DWORD)info->dwCursorPosition.X;
    DWORD end = display ? ((DWORD)info->srWindow.Bottom + 1u) * width : first + width;
    if (mode == 0u)
        first = cursor;
    else if (mode == 1u)
        end = cursor + 1u;
    else if (mode != 2u) {
        errno = EINVAL;
        return -1;
    }
    if (end <= first)
        return 0;
    COORD start = {(SHORT)(first % width), (SHORT)(first / width)};
    DWORD written, count = end - first;
    if (!FillConsoleOutputCharacterW(output, L' ', count, start, &written) || written != count ||
        !FillConsoleOutputAttribute(output, info->wAttributes, count, start, &written) || written != count)
        return console_failure();
    return 0;
}

static int
console_csi(HANDLE output, struct snag_console_state *state)
{
    unsigned int args[16] = {0}, count = 1;
    size_t at = 2u, end = state->sequence_len - 1u;
    bool private = state->sequence[at] == '?';
    if (private)
        ++at;
    for (; at < end; ++at) {
        unsigned char c = state->sequence[at];
        if (c == ';' && count < 16u)
            ++count;
        else if (c >= '0' && c <= '9' && args[count - 1u] <= 3276u)
            args[count - 1u] = args[count - 1u] * 10u + c - '0';
        else {
            errno = EINVAL;
            return -1;
        }
    }
    unsigned char command = state->sequence[end];
    if (private) {
        if (count == 1u && args[0] == 25u && (command == 'h' || command == 'l')) {
            CONSOLE_CURSOR_INFO cursor;
            if (!GetConsoleCursorInfo(output, &cursor))
                return console_failure();
            cursor.bVisible = command == 'h';
            if (!SetConsoleCursorInfo(output, &cursor))
                return console_failure();
        }
        /* Native INPUT_RECORD paste needs no terminal bracket negotiation. */
        return 0;
    }
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (!GetConsoleScreenBufferInfo(output, &info))
        return console_failure();
    if (command == 'm') {
        WORD attributes = info.wAttributes;
        for (unsigned int i = 0; i < count; ++i) {
            unsigned int value = args[i];
            if (value == 0u) {
                attributes = state->initial_attributes;
                state->bold = false;
                state->bright = (attributes & FOREGROUND_INTENSITY) != 0;
            } else if (value == 1u) {
                state->bold = true;
                attributes |= FOREGROUND_INTENSITY;
            } else if (value == 2u || value == 22u) {
                state->bold = false;
                attributes = (WORD)((attributes & ~FOREGROUND_INTENSITY) |
                    (value == 22u && state->bright ? FOREGROUND_INTENSITY : 0));
            } else if (value == 3u || value == 4u)
                attributes |= COMMON_LVB_UNDERSCORE;
            else if (value == 23u || value == 24u)
                attributes &= ~COMMON_LVB_UNDERSCORE;
            else if (value == 7u)
                attributes |= COMMON_LVB_REVERSE_VIDEO;
            else if (value == 27u)
                attributes &= ~COMMON_LVB_REVERSE_VIDEO;
            else if ((value >= 30u && value <= 37u) || (value >= 90u && value <= 97u)) {
                state->bright = value >= 90u;
                attributes = (WORD)((attributes & ~15u) | console_color(value % 10u) |
                                     (state->bright || state->bold ? FOREGROUND_INTENSITY : 0));
            } else if ((value >= 40u && value <= 47u) || (value >= 100u && value <= 107u))
                attributes = (WORD)((attributes & ~240u) | (console_color(value % 10u) << 4) |
                                     (value >= 100u ? BACKGROUND_INTENSITY : 0));
            else if (value == 39u) {
                state->bright = (state->initial_attributes & FOREGROUND_INTENSITY) != 0;
                attributes = (WORD)((attributes & ~15u) | (state->initial_attributes & 15u) |
                                     (state->bold ? FOREGROUND_INTENSITY : 0));
            } else if (value == 49u)
                attributes = (WORD)((attributes & ~240u) | (state->initial_attributes & 240u));
        }
        return SetConsoleTextAttribute(output, attributes) ? 0 : console_failure();
    }
    if (command == 'K' || command == 'J')
        return console_erase(output, &info, command == 'J', args[0]);
    int x = info.dwCursorPosition.X, y = info.dwCursorPosition.Y;
    int amount = args[0] ? (int)args[0] : 1;
    switch (command) {
    case 'A': y -= amount; break;
    case 'B': y += amount; break;
    case 'C': x += amount; break;
    case 'D': x -= amount; break;
    case 'G': x = info.srWindow.Left + amount - 1; break;
    case 'H': case 'f':
        y = info.srWindow.Top + amount - 1;
        x = info.srWindow.Left + (count > 1u && args[1] ? (int)args[1] : 1) - 1;
        break;
    default: return 0;
    }
    if (x < info.srWindow.Left) x = info.srWindow.Left;
    if (x > info.srWindow.Right) x = info.srWindow.Right;
    if (y < info.srWindow.Top) y = info.srWindow.Top;
    if (y > info.srWindow.Bottom) y = info.srWindow.Bottom;
    state->pending_wrap = false;
    state->cursor = (COORD){(SHORT)x, (SHORT)y};
    return SetConsoleCursorPosition(output, state->cursor) ? 0 : console_failure();
}

static int
console_legacy(struct snag_term_host *host, struct snag_console_state *state,
               int fd, const unsigned char *text, size_t len, bool input,
               int (*checkpoint)(void *), void *opaque)
{
    HANDLE output = (HANDLE)_get_osfhandle(fd);
    size_t at = 0;
    while (at < len) {
        if (checkpoint && checkpoint(opaque) < 0)
            return -1;
        unsigned char c = text[at];
        if (state->sequence_len || c == '\033') {
            if (state->sequence_len == sizeof(state->sequence)) {
                state->sequence_len = 0;
                errno = E2BIG;
                return -1;
            }
            state->sequence[state->sequence_len++] = c;
            ++at;
            if (state->sequence_len == 2u && c != '[') {
                state->sequence_len = 0;
                errno = EINVAL;
                return -1;
            }
            if (state->sequence_len > 2u && c >= 0x40u && c <= 0x7eu) {
                int rc = console_csi(output, state);
                state->sequence_len = 0;
                if (rc < 0)
                    return -1;
            }
            continue;
        }
        CONSOLE_SCREEN_BUFFER_INFO info;
        if (!GetConsoleScreenBufferInfo(output, &info))
            return console_failure();
        if (info.dwCursorPosition.X != state->cursor.X ||
            info.dwCursorPosition.Y != state->cursor.Y ||
            info.srWindow.Right != state->wrap_column)
            state->pending_wrap = false;
        if (c < 0x20u || c == 0x7fu) {
            state->pending_wrap = false;
            if (output_plain(host, fd, text + at, 1u, input, checkpoint, opaque) < 0)
                return -1;
            ++at;
        } else {
            uint32_t cp;
            size_t n = snag_utf8_decode(text + at, len - at, &cp);
            int width = n ? snag_char_width(cp) : 1;
            int remaining = info.srWindow.Right - info.dwCursorPosition.X + 1;
            if (width > info.srWindow.Right - info.srWindow.Left + 1) {
                errno = EOVERFLOW;
                return -1;
            }
            if ((state->pending_wrap && width != 0) || width > remaining) {
                if (output_plain(host, fd, "\r\n", 2u, input, checkpoint, opaque) < 0)
                    return -1;
                state->pending_wrap = false;
                if (!GetConsoleScreenBufferInfo(output, &info))
                    return console_failure();
                state->cursor = info.dwCursorPosition;
                continue;
            }
            size_t span = 0;
            int cells = 0;
            while (at + span < len && span < 1024u &&
                   text[at + span] >= 0x20u && text[at + span] != 0x7fu) {
                n = snag_utf8_decode(text + at + span, len - at - span, &cp);
                width = n ? snag_char_width(cp) : 1;
                if (width < 0)
                    width = 1;
                if (width > remaining - cells)
                    break;
                cells += width;
                span += n ? n : 1u;
            }
            if (!span) {
                errno = EIO;
                return -1;
            }
            if (output_plain(host, fd, text + at, span, input, checkpoint, opaque) < 0)
                return -1;
            /* Classic WriteConsoleW can leave a stale cursor after a full row
             * even though every cell was written with wrapping disabled. */
            int x = info.dwCursorPosition.X + cells;
            if (x > info.srWindow.Right)
                x = info.srWindow.Right;
            COORD cursor = {(SHORT)x, info.dwCursorPosition.Y};
            if (!SetConsoleCursorPosition(output, cursor))
                return console_failure();
            state->pending_wrap = state->pending_wrap || cells == remaining;
            state->wrap_column = info.srWindow.Right;
            at += span;
        }
        if (!GetConsoleScreenBufferInfo(output, &info))
            return console_failure();
        if (info.srWindow.Right != state->wrap_column)
            state->pending_wrap = false;
        state->cursor = info.dwCursorPosition;
    }
    return 0;
}

static pthread_once_t diagnostic_console_once = PTHREAD_ONCE_INIT;
static struct snag_console_state diagnostic_console[2];

static void
capture_diagnostic_console(void)
{
    const DWORD streams[] = {STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
    for (size_t i = 0; i < 2u; ++i) {
        CONSOLE_SCREEN_BUFFER_INFO info;
        if (GetConsoleScreenBufferInfo(GetStdHandle(streams[i]), &info)) {
            diagnostic_console[i].initial_attributes = info.wAttributes;
            diagnostic_console[i].bright = (info.wAttributes & FOREGROUND_INTENSITY) != 0;
            diagnostic_console[i].cursor = info.dwCursorPosition;
            diagnostic_console[i].legacy = true;
        }
    }
}

int
snag_term_output_write(struct snag_term_host *host, int fd,
                       const void *text, size_t len, bool input,
                       int (*checkpoint)(void *), void *opaque)
{
    struct snag_console_state *state = NULL;
    HANDLE output = (HANDLE)_get_osfhandle(fd);
    DWORD mode = 0;
    bool temporary = false;
    if (host) {
        for (size_t i = 0; i < 2u; ++i)
            if (host->output_console[i] == output && host->output_state[i].legacy)
                state = &host->output_state[i];
    } else if (fd >= 1 && fd <= 2 && GetConsoleMode(output, &mode) &&
               !(mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
        if (pthread_once(&diagnostic_console_once, capture_diagnostic_console) != 0)
            abort();
        if (diagnostic_console[fd - 1].legacy) {
            state = &diagnostic_console[fd - 1];
            if (!SetConsoleMode(output, (mode | ENABLE_PROCESSED_OUTPUT) & ~ENABLE_WRAP_AT_EOL_OUTPUT))
                return console_failure();
            temporary = true;
        }
    }
    int rc = state ? console_legacy(host, state, fd, text, len, input, checkpoint, opaque) :
                    output_plain(host, fd, text, len, input, checkpoint, opaque);
    int error = errno;
    if (rc < 0 && state) {
        state->pending_wrap = false;
        state->sequence_len = 0;
    }
    if (temporary && !SetConsoleMode(output, mode) && rc == 0)
        return console_failure();
    errno = error;
    return rc;
}

static _Atomic(void (*)(int)) console_interrupt;
static pthread_mutex_t console_control_lock = PTHREAD_MUTEX_INITIALIZER;
static HANDLE console_control_event;

static BOOL WINAPI
console_control(DWORD event)
{
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT)
        return FALSE;
    control_lock(&console_control_lock);
    void (*interrupt)(int) = atomic_load(&console_interrupt);
    if (!interrupt) {
        control_unlock(&console_control_lock);
        return FALSE;
    }
    interrupt(SIGINT);
    cancel_console_read();
    (void)SetEvent(console_control_event);
    control_unlock(&console_control_lock);
    return TRUE;
}

int
snag_term_controls_install(struct snag_term_host *host,
                           void (*interrupt)(int), void (*resize)(int))
{
    (void)resize;
    HANDLE event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!event)
        return snag_errno(EIO);
    control_lock(&console_control_lock);
    void (*absent)(int) = NULL;
    if (!interrupt || !atomic_compare_exchange_strong(&console_interrupt, &absent, interrupt)) {
        control_unlock(&console_control_lock);
        (void)CloseHandle(event);
        errno = interrupt ? EBUSY : EINVAL;
        return -1;
    }
    console_control_event = event;
    host->control_event = event;
    if (!SetConsoleCtrlHandler(console_control, TRUE)) {
        atomic_store(&console_interrupt, NULL);
        console_control_event = host->control_event = NULL;
        control_unlock(&console_control_lock);
        (void)CloseHandle(event);
        return snag_errno(EIO);
    }
    control_unlock(&console_control_lock);
    return 0;
}

void
snag_term_controls_restore(struct snag_term_host *host)
{
    (void)SetConsoleCtrlHandler(console_control, FALSE);
    control_lock(&console_control_lock);
    atomic_store(&console_interrupt, NULL);
    console_control_event = NULL;
    if (host->control_event)
        (void)CloseHandle(host->control_event);
    host->control_event = NULL;
    control_unlock(&console_control_lock);
}

static void
reset_input(struct snag_term_host *host)
{
    snag_output_broker_close(host->input_broker);
    host->input_broker = NULL;
    if (host->line_input) {
        (void)CloseHandle(host->line_input);
        host->line_input = NULL;
    }
    atomic_store(&console_read_cancelled, false);
    host->input_high = 0;
    host->input_cooked_pending = false;
    host->input_count = host->input_next = 0;
    host->input_key_len = host->input_key_at = host->input_repeats = 0;
    host->input_resized = false;
    memset(host->input_events, 0, sizeof(host->input_events));
    memset(host->input_key, 0, sizeof(host->input_key));
}

int
snag_term_output_open(struct snag_term_host *host, int fd)
{
    HANDLE copy;
    if (fd < 1 || fd > 2)
        return snag_errno(EINVAL);
    if (!snag_isatty(fd))
        return -1;
    if (!DuplicateHandle(GetCurrentProcess(), (HANDLE)_get_osfhandle(fd),
                          GetCurrentProcess(), &copy, 0, FALSE, DUPLICATE_SAME_ACCESS))
        return snag_errno(EIO);
    int result = _open_osfhandle((intptr_t)copy, _O_WRONLY | _O_BINARY | _O_NOINHERIT);
    if (result < 0) {
        int saved = errno;
        (void)CloseHandle(copy);
        errno = saved;
        return -1;
    }
    DWORD mode;
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (!GetConsoleMode(copy, &mode) || !GetConsoleScreenBufferInfo(copy, &info)) {
        (void)_close(result);
        errno = EIO;
        return -1;
    }
    bool legacy = !SetConsoleMode(copy, mode | ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT |
                                   ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN);
    if (legacy && !SetConsoleMode(copy, (mode | ENABLE_PROCESSED_OUTPUT) &
                                  ~(ENABLE_WRAP_AT_EOL_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING))) {
        (void)_close(result);
        return snag_errno(ENOTSUP);
    }
    host->output_mode[fd - 1] = mode;
    host->output_console[fd - 1] = copy;
    HANDLE source = (HANDLE)_get_osfhandle(fd);
    host->output_source[fd - 1] = source == GetStdHandle(fd == 1 ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE) ?
                                  source : NULL;
    host->output_state[fd - 1] = (struct snag_console_state){
        .initial_attributes = info.wAttributes, .cursor = info.dwCursorPosition, .legacy = legacy,
        .bright = (info.wAttributes & FOREGROUND_INTENSITY) != 0};
    return result;
}

bool
snag_term_host_capable(void)
{
    const char *name = getenv("TERM");
    DWORD mode;
    return (!name || strcmp(name, "dumb")) &&
           GetConsoleMode((HANDLE)_get_osfhandle(2), &mode);
}

int
snag_term_output_mode(struct snag_term_host *host, bool active)
{
    for (size_t n = 0; n < 2u; ++n) {
        size_t i = active ? n : 1u - n;
        DWORD mode = host->output_mode[i];
        if (active) {
            if (host->output_state[i].legacy)
                mode = (mode | ENABLE_PROCESSED_OUTPUT) &
                       ~(ENABLE_WRAP_AT_EOL_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
            else
                mode |= ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT |
                        ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN;
        }
        if (host->output_console[i] && !SetConsoleMode(host->output_console[i], mode))
            return snag_errno(EIO);
    }
    return 0;
}

unsigned int
snag_term_host_columns(void)
{
    CONSOLE_SCREEN_BUFFER_INFO info;
    return GetConsoleScreenBufferInfo((HANDLE)_get_osfhandle(2), &info) ?
           (unsigned int)(info.srWindow.Right - info.srWindow.Left + 1) : 0u;
}

unsigned int
snag_term_host_rows(void)
{
    CONSOLE_SCREEN_BUFFER_INFO info;
    return GetConsoleScreenBufferInfo((HANDLE)_get_osfhandle(2), &info) ?
           (unsigned int)(info.srWindow.Bottom - info.srWindow.Top + 1) : 0u;
}

int
snag_term_input_capture(struct snag_term_host *host)
{
    if (!GetConsoleMode((HANDLE)_get_osfhandle(0), &host->input_mode))
        return snag_errno(ENOTTY);
    host->raw_input = false;
    reset_input(host);
    return 0;
}

int
snag_term_input_raw(struct snag_term_host *host)
{
    DWORD mode = host->input_mode;
    if (host->raw_input)
        return 0;
    mode &= ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT |
              ENABLE_QUICK_EDIT_MODE | ENABLE_VIRTUAL_TERMINAL_INPUT);
    mode |= ENABLE_EXTENDED_FLAGS | ENABLE_WINDOW_INPUT;
    if (!SetConsoleMode((HANDLE)_get_osfhandle(0), mode))
        return snag_errno(ENOTSUP);
    host->raw_input = true;
    return 0;
}

int
snag_term_input_hidden(struct snag_term_host *host)
{
    if (!SetConsoleMode((HANDLE)_get_osfhandle(0), host->input_mode & ~ENABLE_ECHO_INPUT))
        return snag_errno(EIO);
    return 0;
}

int
snag_term_input_flush(struct snag_term_host *host)
{
    reset_input(host);
    if (FlushConsoleInputBuffer((HANDLE)_get_osfhandle(0)))
        return 0;
    return snag_errno(EIO);
}

int
snag_term_input_restore(struct snag_term_host *host, bool flush)
{
    int rc = flush ? snag_term_input_flush(host) : 0;
    if (!SetConsoleMode((HANDLE)_get_osfhandle(0), host->input_mode))
        rc = -1;
    if (rc == 0)
        host->raw_input = false;
    if (rc < 0)
        errno = EIO;
    return rc;
}

static int
encode_key(struct snag_term_host *host, const KEY_EVENT_RECORD *key)
{
    static const struct { WORD key; char final; } cursors[] = {
        {VK_UP, 'A'}, {VK_DOWN, 'B'}, {VK_RIGHT, 'C'}, {VK_LEFT, 'D'},
        {VK_HOME, 'H'}, {VK_END, 'F'}
    };
    DWORD control = key->dwControlKeyState;
    bool shift = (control & SHIFT_PRESSED) != 0;
    bool alt = (control & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) != 0;
    bool ctrl = (control & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
    unsigned int modifier = 1u + shift + 2u * alt + 4u * ctrl;
    WCHAR c = key->uChar.UnicodeChar;
    int n;

    host->input_key_at = host->input_key_len = 0;
    host->input_repeats = key->wRepeatCount;
    for (size_t i = 0; i < sizeof(cursors) / sizeof(cursors[0]); ++i)
        if (key->wVirtualKeyCode == cursors[i].key) {
            n = modifier == 1u ? snprintf(host->input_key, sizeof(host->input_key),
                    "\033[%c", cursors[i].final) :
                snprintf(host->input_key, sizeof(host->input_key), "\033[1;%u%c", modifier, cursors[i].final);
            host->input_key_len = (unsigned int)n;
            return 0;
        }
    unsigned int code = key->wVirtualKeyCode == VK_INSERT ? 2u :
                        key->wVirtualKeyCode == VK_DELETE ? 3u :
                        key->wVirtualKeyCode == VK_PRIOR ? 5u :
                        key->wVirtualKeyCode == VK_NEXT ? 6u : 0u;
    if (code) {
        n = modifier == 1u ? snprintf(host->input_key, sizeof(host->input_key), "\033[%u~", code) :
            snprintf(host->input_key, sizeof(host->input_key), "\033[%u;%u~", code, modifier);
        host->input_key_len = (unsigned int)n;
        return 0;
    }
    if (key->wVirtualKeyCode == VK_TAB && shift) {
        memcpy(host->input_key, "\033[Z", 3u);
        host->input_key_len = 3u;
        return 0;
    }
    if (!c) {
        if (!ctrl || key->wVirtualKeyCode != VK_SPACE)
            return 0;
        host->input_key[0] = 0;
        host->input_key_len = 1u;
        return 0;
    }
    WCHAR scalar[2] = {c, 0};
    int units = 1;
    if (c >= 0xd800u && c <= 0xdbffu) {
        bool incomplete = host->input_high != 0;
        host->input_high = c;
        if (!incomplete)
            return 0;
        scalar[0] = 0xfffdu;
    } else if (host->input_high) {
        scalar[0] = c >= 0xdc00u && c <= 0xdfffu ? host->input_high : 0xfffdu;
        scalar[1] = c;
        units = 2;
        host->input_high = 0;
    } else if (c >= 0xdc00u && c <= 0xdfffu)
        scalar[0] = 0xfffdu;
    /* AltGr produces printable text, not an Escape-prefixed meta command. */
    size_t prefix = alt && !(ctrl && c >= 0x20u) ? 1u : 0u;
    if (prefix)
        host->input_key[0] = '\033';
    n = (int)snag_utf16_to_utf8(scalar, (size_t)units, host->input_key + prefix,
                                sizeof(host->input_key) - prefix);
    if (n < 0)
        return -1;
    host->input_key_len = (unsigned int)n + (unsigned int)prefix;
    return 0;
}

static ssize_t
read_keys(struct snag_term_host *host, HANDLE input, unsigned char *buffer, size_t size)
{
    size_t used = 0;
    unsigned int consumed = 0;
    while (used < size) {
        if (host->input_key_len && host->input_repeats) {
            size_t take = host->input_key_len - host->input_key_at;
            if (take > size - used)
                take = size - used;
            memcpy(buffer + used, host->input_key + host->input_key_at, take);
            used += take;
            host->input_key_at += (unsigned int)take;
            if (host->input_key_at == host->input_key_len) {
                host->input_key_at = 0;
                --host->input_repeats;
            }
            continue;
        }
        if (consumed == 128u)
            break;
        if (host->input_next == host->input_count) {
            DWORD available, got;
            if (!GetNumberOfConsoleInputEvents(input, &available))
                return snag_errno(EIO);
            if (!available)
                break;
            if (available > 16u)
                available = 16u;
            if (!ReadConsoleInputW(input, host->input_events, available, &got))
                return snag_errno(EIO);
            host->input_next = 0;
            host->input_count = got;
            if (!got)
                break;
        }
        const INPUT_RECORD *event = &host->input_events[host->input_next++];
        ++consumed;
        if (event->EventType == WINDOW_BUFFER_SIZE_EVENT)
            host->input_resized = true;
        if (event->EventType == KEY_EVENT && event->Event.KeyEvent.bKeyDown &&
            event->Event.KeyEvent.wRepeatCount && encode_key(host, &event->Event.KeyEvent) < 0)
            return -1;
    }
    if (used)
        return (ssize_t)used;
    return snag_errno(EAGAIN);
}

bool
snag_term_input_resized(struct snag_term_host *host)
{
    bool resized = host->input_resized;
    host->input_resized = false;
    return resized;
}

int
snag_term_input_wait(struct snag_term_host *host, snag_wake_fd wake, int timeout_ms)
{
    HANDLE handles[3] = {(HANDLE)_get_osfhandle(0), NULL, NULL};
    DWORD count = 1;
    DWORD wake_index = MAXDWORD;
    int rc, error = 0;

    if (timeout_ms < -1)
        return snag_errno(EINVAL);
    if (host->input_cooked_pending || host->input_next < host->input_count ||
        (host->input_key_len && host->input_repeats))
        return SNAG_TERM_WAIT_INPUT;
    if (wake != SNAG_WAKE_INVALID) {
        handles[1] = WSACreateEvent();
        if (handles[1] == WSA_INVALID_EVENT)
            return snag_socket_error(WSAGetLastError());
        if (WSAEventSelect(wake, handles[1], FD_READ | FD_CLOSE) < 0) {
            error = WSAGetLastError();
            (void)WSACloseEvent(handles[1]);
            return snag_socket_error(error);
        }
        count = 2;
        wake_index = 1;
    }
    if (host->control_event)
        handles[count++] = host->control_event;
    DWORD ready = WaitForMultipleObjects(count, handles, FALSE,
                                         timeout_ms < 0 ? INFINITE : (DWORD)timeout_ms);
    rc = ready == WAIT_OBJECT_0 ? SNAG_TERM_WAIT_INPUT :
         ready > WAIT_OBJECT_0 && ready < WAIT_OBJECT_0 + count ? SNAG_TERM_WAIT_WAKE :
         ready == WAIT_TIMEOUT ? 0 : -1;
    if (wake_index != MAXDWORD) {
        if (WSAEventSelect(wake, NULL, 0) < 0)
            error = WSAGetLastError();
        (void)WSACloseEvent(handles[wake_index]);
    }
    if (error)
        return snag_socket_error(error);
    if (rc < 0)
        errno = EIO;
    return rc;
}

ssize_t
snag_term_input_read(struct snag_term_host *host, void *buffer, size_t size)
{
    HANDLE input = (HANDLE)_get_osfhandle(0);
    DWORD mode, got;
    WCHAR wide[257];
    size_t prefix = host->input_high ? 1u : 0u;

    if (!buffer || !size || size > INT_MAX)
        return snag_errno(EINVAL);
    if (!GetConsoleMode(input, &mode)) {
        if (ReadFile(input, buffer, (DWORD)size, &got, NULL))
            return (ssize_t)got;
        DWORD error = GetLastError();
        if (error == ERROR_BROKEN_PIPE)
            return 0;
        errno = error == ERROR_NO_DATA ? EAGAIN : error == ERROR_INVALID_HANDLE ? EBADF : EIO;
        return -1;
    }
    if (size < 4u)
        return snag_errno(EINVAL);
    if (!(mode & ENABLE_LINE_INPUT))
        return read_keys(host, input, buffer, size);
    size_t capacity = (size - prefix) / 3u;
    if (capacity > 256u)
        capacity = 256u;
    if (prefix)
        wide[0] = host->input_high;
    if (!host->line_input) {
        host->line_input = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (host->line_input == INVALID_HANDLE_VALUE) {
            host->line_input = NULL;
            return snag_errno(EIO);
        }
    }
    if (!read_console(host, host->line_input, wide + prefix, (DWORD)capacity, &got)) {
        DWORD error = GetLastError();
        (void)CloseHandle(host->line_input);
        host->line_input = NULL;
        snag_output_broker_close(host->input_broker);
        host->input_broker = NULL;
        host->input_cooked_pending = false;
        host->input_high = 0;
        errno = error == ERROR_OPERATION_ABORTED ? EINTR : EIO;
        return -1;
    }
    if (!got) {
        (void)CloseHandle(host->line_input);
        host->line_input = NULL;
        snag_output_broker_close(host->input_broker);
        host->input_broker = NULL;
        host->input_cooked_pending = false;
        host->input_high = 0;
        return 0;
    }
    /* ReadConsoleW owns a completed line even after the record queue empties. */
    host->input_cooked_pending = wide[got + prefix - 1u] != L'\n';
    if (!host->input_cooked_pending) {
        (void)CloseHandle(host->line_input);
        host->line_input = NULL;
        snag_output_broker_close(host->input_broker);
        host->input_broker = NULL;
    }
    size_t count = got + prefix;
    host->input_high = 0;
    if (wide[count - 1u] >= 0xd800u && wide[count - 1u] <= 0xdbffu)
        host->input_high = wide[--count];
    size_t used = 0;
    for (size_t i = 0; i < count; ++i) {
        WCHAR c = wide[i];
        /* Consume the complete cooked CRLF before returning a line ending. */
        if (c == L'\r')
            continue;
        wide[used++] = c;
    }
    if (!used)
        return snag_errno(EAGAIN);
    return snag_utf16_to_utf8(wide, used, (char *)buffer, size);
}

int
snag_term_signals_block(struct snag_signal_mask *saved)
{
    saved->unused = 0;
    return 0;
}

int
snag_term_signals_restore(const struct snag_signal_mask *saved)
{
    (void)saved;
    return 0;
}

int
snag_term_signals_unblock(void)
{
    return 0;
}
#else
#include <pthread.h>
#include <sys/ioctl.h>
#ifdef __FreeBSD__
#include <sys/sysctl.h>
#endif
#include <unistd.h>

int
snag_term_output_mode(struct snag_term_host *host, bool active)
{
    (void)host;
    (void)active;
    return 0;
}

void
snag_shutdown_detach(struct snag_shutdown *saved)
{
    /* Retain the original POSIX handler lifetime through final cleanup. */
    (void)saved;
}

void
snag_shutdown_finish(struct snag_shutdown *saved)
{
    while (saved->count) {
        --saved->count;
        (void)sigaction(saved->numbers[saved->count], &saved->saved[saved->count], NULL);
    }
}

int
snag_shutdown_install(struct snag_shutdown *saved, void (*handler)(int), bool hangup)
{
    struct sigaction action = {0};
    memset(saved, 0, sizeof(*saved));
    saved->numbers[0] = SIGINT;
    saved->numbers[1] = hangup ? SIGHUP : SIGTERM;
    saved->numbers[2] = SIGTERM;
    action.sa_handler = handler;
    sigemptyset(&action.sa_mask);
    for (unsigned int i = 0; i < (hangup ? 3u : 2u); ++i) {
        if (sigaction(saved->numbers[i], &action, &saved->saved[i]) < 0) {
            int error = errno;
            snag_shutdown_finish(saved);
            errno = error;
            return -1;
        }
        ++saved->count;
    }
    return 0;
}

bool
snag_term_can_suspend(void)
{
    return true;
}

int
snag_term_suspend(void)
{
    return raise(SIGSTOP);
}

int
snag_term_output_write(struct snag_term_host *host, int fd,
                       const void *text, size_t len, bool input,
                       int (*checkpoint)(void *), void *opaque)
{
    const unsigned char *bytes = text;
    struct pollfd fds[2] = {{fd, POLLOUT, 0}, {input ? STDIN_FILENO : -1, POLLIN, 0}};
    (void)host;
    while (len) {
        int rc = poll(fds, 2u, 0);
        if (rc >= 0 && !(fds[0].revents & POLLOUT) && checkpoint && checkpoint(opaque) < 0)
            return -1;
        if (rc >= 0 && !(fds[0].revents & POLLOUT))
            rc = poll(fds, 2u, checkpoint ? 16 : -1);
        if (rc < 0 && errno == EINTR)
            continue;
        if (rc < 0)
            return -1;
        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL))
            return snag_errno(EIO);
        if (!(fds[0].revents & POLLOUT))
            continue;
        size_t amount = len < 1024u ? len : 1024u;
        ssize_t written = write(fd, bytes, amount);
        if (written < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (written <= 0)
            return -1;
        bytes += written;
        len -= (size_t)written;
    }
    return 0;
}

void
snag_term_host_close(struct snag_term_host *host)
{
    (void)host;
}

int
snag_term_controls_install(struct snag_term_host *host,
                           void (*interrupt)(int), void (*resize)(int))
{
    struct sigaction action = {0};
    sigemptyset(&action.sa_mask);
    action.sa_handler = interrupt;
    if (sigaction(SIGINT, &action, &host->sigint) < 0)
        return -1;
    action.sa_handler = resize;
    if (sigaction(SIGWINCH, &action, &host->sigwinch) < 0) {
        int error = errno;
        (void)sigaction(SIGINT, &host->sigint, NULL);
        errno = error;
        return -1;
    }
    return 0;
}

void
snag_term_controls_restore(struct snag_term_host *host)
{
    (void)sigaction(SIGWINCH, &host->sigwinch, NULL);
    (void)sigaction(SIGINT, &host->sigint, NULL);
}

int
snag_term_output_open(struct snag_term_host *host, int fd)
{
    (void)host;
    char path[SNAG_PATH_MAX_BYTES];
    snag_file_info original, owned;
#if defined(__FreeBSD__) && __FreeBSD__ < 6
    struct stat st;
    int error = fstat(fd, &st) < 0 ? errno : 0;
    if (!error && !S_ISCHR(st.st_mode))
        error = ENOTTY;
    if (!error) {
        memcpy(path, "/dev/", 5u);
        size_t size = sizeof(path) - 5u;
        if (sysctlbyname("kern.devname", path + 5u, &size,
                         &st.st_rdev, sizeof(st.st_rdev)) < 0)
            error = errno;
        else if (!size || size > sizeof(path) - 5u || path[5u + size - 1u])
            error = ENOTTY;
    }
#else
    int error = ttyname_r(fd, path, sizeof(path));
#endif
    int copy = error ? -1 : open(path, O_WRONLY | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);

    if (copy >= 0 && snag_fd_cloexec(copy) < 0) {
        error = errno;
        (void)close(copy);
        copy = -1;
    }
    if (error)
        errno = error;
    if (copy < 0 || snag_fstat(fd, &original) < 0 || snag_fstat(copy, &owned) < 0 ||
        original.st_rdev != owned.st_rdev || !S_ISCHR(owned.st_mode)) {
        int saved = copy < 0 ? errno : EIO;
        if (copy >= 0)
            (void)close(copy);
        errno = saved;
        return -1;
    }
    return copy;
}

bool
snag_term_host_capable(void)
{
    const char *name = getenv("TERM");
    return name && strcmp(name, "dumb");
}

unsigned int
snag_term_host_columns(void)
{
    struct winsize size = {0};
    return ioctl(STDERR_FILENO, TIOCGWINSZ, &size) == 0 ? size.ws_col : 0u;
}

unsigned int
snag_term_host_rows(void)
{
    struct winsize size = {0};
    return ioctl(STDERR_FILENO, TIOCGWINSZ, &size) == 0 ? size.ws_row : 0u;
}

int
snag_term_input_capture(struct snag_term_host *host)
{
    return tcgetattr(STDIN_FILENO, &host->input_mode);
}

int
snag_term_input_raw(struct snag_term_host *host)
{
    struct termios raw = host->input_mode;
    raw.c_iflag &= (tcflag_t)~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_lflag &= (tcflag_t)~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    return tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

int
snag_term_input_flush(struct snag_term_host *host)
{
    (void)host;
    return tcflush(STDIN_FILENO, TCIFLUSH);
}

int
snag_term_input_hidden(struct snag_term_host *host)
{
    struct termios hidden = host->input_mode;
    hidden.c_lflag &= (tcflag_t)~ECHO;
    return tcsetattr(STDIN_FILENO, TCSAFLUSH, &hidden);
}

ssize_t
snag_term_input_read(struct snag_term_host *host, void *buffer, size_t size)
{
    (void)host;
    return read(STDIN_FILENO, buffer, size);
}

bool
snag_term_input_resized(struct snag_term_host *host)
{
    (void)host;
    return false;
}

int
snag_term_input_wait(struct snag_term_host *host, snag_wake_fd wake, int timeout_ms)
{
    struct pollfd fds[2] = {{STDIN_FILENO, POLLIN, 0}, {wake, POLLIN, 0}};
    (void)host;
    if (timeout_ms < -1)
        return snag_errno(EINVAL);
    int rc = poll(fds, 2u, timeout_ms);
    if (rc <= 0)
        return rc;
    return (fds[0].revents & POLLIN ? SNAG_TERM_WAIT_INPUT : 0) |
           (fds[0].revents & (POLLHUP | POLLERR | POLLNVAL) ? SNAG_TERM_WAIT_END : 0) |
           (fds[1].revents ? SNAG_TERM_WAIT_WAKE : 0);
}

int
snag_term_input_restore(struct snag_term_host *host, bool flush)
{
    return tcsetattr(STDIN_FILENO, flush ? TCSAFLUSH : TCSANOW, &host->input_mode);
}

static int
mask_signals(int how, const sigset_t *set, sigset_t *saved)
{
    int error = pthread_sigmask(how, set, saved);

    if (error) {
        errno = error;
        return -1;
    }
    return 0;
}

static void
terminal_signals(sigset_t *signals)
{
    sigemptyset(signals);
    sigaddset(signals, SIGINT);
    sigaddset(signals, SIGWINCH);
}

int
snag_term_signals_block(struct snag_signal_mask *saved)
{
    sigset_t signals;
    terminal_signals(&signals);
    return mask_signals(SIG_BLOCK, &signals, &saved->native);
}

int
snag_term_signals_restore(const struct snag_signal_mask *saved)
{
    return mask_signals(SIG_SETMASK, &saved->native, NULL);
}

int
snag_term_signals_unblock(void)
{
    sigset_t signals;
    terminal_signals(&signals);
    return mask_signals(SIG_UNBLOCK, &signals, NULL);
}
#endif

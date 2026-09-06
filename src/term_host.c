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

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>

static _Atomic(void (*)(int)) console_interrupt;

static BOOL WINAPI
console_control(DWORD event)
{
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT)
        return FALSE;
    void (*interrupt)(int) = atomic_load(&console_interrupt);
    if (!interrupt)
        return FALSE;
    interrupt(SIGINT);
    /* Wake the input wait without injecting a second Ctrl-C keystroke. */
    INPUT_RECORD record = {.EventType = FOCUS_EVENT};
    DWORD written;
    (void)WriteConsoleInputW(GetStdHandle(STD_INPUT_HANDLE), &record, 1u, &written);
    return TRUE;
}

int
snag_term_controls_install(struct snag_term_host *host,
                           void (*interrupt)(int), void (*resize)(int))
{
    (void)host;
    (void)resize;
    void (*absent)(int) = NULL;
    if (!interrupt || !atomic_compare_exchange_strong(&console_interrupt, &absent, interrupt)) {
        errno = interrupt ? EBUSY : EINVAL;
        return -1;
    }
    if (!SetConsoleCtrlHandler(console_control, TRUE)) {
        atomic_store(&console_interrupt, NULL);
        errno = EIO;
        return -1;
    }
    return 0;
}

void
snag_term_controls_restore(struct snag_term_host *host)
{
    (void)host;
    (void)SetConsoleCtrlHandler(console_control, FALSE);
    atomic_store(&console_interrupt, NULL);
}

static void
reset_input(struct snag_term_host *host)
{
    host->input_high = 0;
    host->input_skip_lf = false;
    host->input_count = host->input_next = 0;
    host->input_key_len = host->input_key_at = host->input_repeats = 0;
    host->input_resized = false;
    memset(host->input_events, 0, sizeof(host->input_events));
    memset(host->input_key, 0, sizeof(host->input_key));
}

int
snag_term_output_open(int fd)
{
    HANDLE copy;
    if (!snag_isatty(fd))
        return -1;
    if (!DuplicateHandle(GetCurrentProcess(), (HANDLE)_get_osfhandle(fd),
                          GetCurrentProcess(), &copy, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        errno = EIO;
        return -1;
    }
    int result = _open_osfhandle((intptr_t)copy, _O_WRONLY | _O_BINARY | _O_NOINHERIT);
    if (result < 0) {
        int saved = errno;
        (void)CloseHandle(copy);
        errno = saved;
    }
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

unsigned int
snag_term_host_columns(void)
{
    CONSOLE_SCREEN_BUFFER_INFO info;
    return GetConsoleScreenBufferInfo((HANDLE)_get_osfhandle(2), &info) ?
           (unsigned int)(info.srWindow.Right - info.srWindow.Left + 1) : 0u;
}

int
snag_term_input_capture(struct snag_term_host *host)
{
    if (!GetConsoleMode((HANDLE)_get_osfhandle(0), &host->input_mode)) {
        errno = ENOTTY;
        return -1;
    }
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
    if (!SetConsoleMode((HANDLE)_get_osfhandle(0), mode)) {
        errno = ENOTSUP;
        return -1;
    }
    host->raw_input = true;
    return 0;
}

int
snag_term_input_flush(struct snag_term_host *host)
{
    reset_input(host);
    if (FlushConsoleInputBuffer((HANDLE)_get_osfhandle(0)))
        return 0;
    errno = EIO;
    return -1;
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
    n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, scalar, units,
                            host->input_key + prefix, (int)(sizeof(host->input_key) - prefix), NULL, NULL);
    if (!n) {
        errno = EILSEQ;
        return -1;
    }
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
            if (!GetNumberOfConsoleInputEvents(input, &available)) {
                errno = EIO;
                return -1;
            }
            if (!available)
                break;
            if (available > 16u)
                available = 16u;
            if (!ReadConsoleInputW(input, host->input_events, available, &got)) {
                errno = EIO;
                return -1;
            }
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
    errno = EAGAIN;
    return -1;
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
    HANDLE handles[2] = {(HANDLE)_get_osfhandle(0), NULL};
    DWORD count = 1;
    int rc, error = 0;

    if (timeout_ms < -1) {
        errno = EINVAL;
        return -1;
    }
    if (host->input_next < host->input_count ||
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
    }
    DWORD ready = WaitForMultipleObjects(count, handles, FALSE,
                                         timeout_ms < 0 ? INFINITE : (DWORD)timeout_ms);
    rc = ready == WAIT_OBJECT_0 ? SNAG_TERM_WAIT_INPUT :
         ready == WAIT_OBJECT_0 + 1u && count == 2u ? SNAG_TERM_WAIT_WAKE :
         ready == WAIT_TIMEOUT ? 0 : -1;
    if (count == 2u) {
        if (WSAEventSelect(wake, NULL, 0) < 0)
            error = WSAGetLastError();
        (void)WSACloseEvent(handles[1]);
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

    if (!buffer || size < 4u || size > INT_MAX) {
        errno = EINVAL;
        return -1;
    }
    if (!GetConsoleMode(input, &mode)) {
        if (ReadFile(input, buffer, (DWORD)size, &got, NULL))
            return (ssize_t)got;
        DWORD error = GetLastError();
        if (error == ERROR_BROKEN_PIPE)
            return 0;
        errno = error == ERROR_NO_DATA ? EAGAIN : error == ERROR_INVALID_HANDLE ? EBADF : EIO;
        return -1;
    }
    if (!(mode & ENABLE_LINE_INPUT))
        return read_keys(host, input, buffer, size);
    size_t capacity = (size - prefix) / 3u;
    if (capacity > 256u)
        capacity = 256u;
    if (prefix)
        wide[0] = host->input_high;
    if (!ReadConsoleW(input, wide + prefix, (DWORD)capacity, &got, NULL)) {
        errno = EIO;
        return -1;
    }
    if (!got)
        return 0;
    size_t count = got + prefix;
    host->input_high = 0;
    if (wide[count - 1u] >= 0xd800u && wide[count - 1u] <= 0xdbffu)
        host->input_high = wide[--count];
    size_t used = 0;
    for (size_t i = 0; i < count; ++i) {
        WCHAR c = wide[i];
        if (mode & ENABLE_LINE_INPUT) {
            if (c == L'\n' && host->input_skip_lf) {
                host->input_skip_lf = false;
                continue;
            }
            host->input_skip_lf = c == L'\r';
            if (c == L'\r')
                c = L'\n';
        }
        wide[used++] = c;
    }
    if (!used) {
        errno = EAGAIN;
        return -1;
    }
    int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, (int)used,
                                    buffer, (int)size, NULL, NULL);
    if (!bytes) {
        errno = EILSEQ;
        return -1;
    }
    return bytes;
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
#include <unistd.h>

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
snag_term_output_open(int fd)
{
    char path[SNAG_PATH_MAX_BYTES];
    snag_file_info original, owned;
    int error = ttyname_r(fd, path, sizeof(path));
    int copy = error ? -1 : open(path, O_WRONLY | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);

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
    if (timeout_ms < -1) {
        errno = EINVAL;
        return -1;
    }
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

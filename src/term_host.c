/* SPDX-License-Identifier: GPL-2.0-only */
#include "term_host.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#include <fcntl.h>

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
    host->input_codepage = GetConsoleCP();
    host->binary_input = false;
    host->input_high = 0;
    host->input_skip_lf = false;
    if (!host->input_codepage) {
        errno = EIO;
        return -1;
    }
    return 0;
}

int
snag_term_input_raw(struct snag_term_host *host)
{
    DWORD mode = host->input_mode;
    if (host->binary_input)
        return 0;
    int old = _setmode(0, _O_BINARY);
    if (old < 0)
        return -1;
    mode &= ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT | ENABLE_QUICK_EDIT_MODE);
    mode |= ENABLE_EXTENDED_FLAGS | ENABLE_WINDOW_INPUT | ENABLE_VIRTUAL_TERMINAL_INPUT;
    if (!SetConsoleCP(CP_UTF8) || !SetConsoleMode((HANDLE)_get_osfhandle(0), mode)) {
        (void)SetConsoleCP(host->input_codepage);
        (void)_setmode(0, old);
        errno = ENOTSUP;
        return -1;
    }
    host->input_crt_mode = old;
    host->binary_input = true;
    return 0;
}

int
snag_term_input_flush(struct snag_term_host *host)
{
    host->input_high = 0;
    host->input_skip_lf = false;
    if (FlushConsoleInputBuffer((HANDLE)_get_osfhandle(0)))
        return 0;
    errno = EIO;
    return -1;
}

int
snag_term_input_restore(struct snag_term_host *host, bool flush)
{
    int rc = flush ? snag_term_input_flush(host) : 0;
    host->input_high = 0;
    host->input_skip_lf = false;
    if (!SetConsoleMode((HANDLE)_get_osfhandle(0), host->input_mode))
        rc = -1;
    if (host->binary_input) {
        if (!SetConsoleCP(host->input_codepage))
            rc = -1;
        if (_setmode(0, host->input_crt_mode) < 0)
            rc = -1;
        if (rc == 0)
            host->binary_input = false;
    }
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
    if (!GetConsoleMode(input, &mode))
        return _read(0, buffer, (unsigned int)size);
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

/* SPDX-License-Identifier: GPL-2.0-only */
#include "term.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
#define HISTORY_FILE_BYTES (SNJ_TERM_HISTORY_BYTES * 4u + SNJ_TERM_HISTORY_COUNT)

static volatile sig_atomic_t sigint_pending;
static volatile sig_atomic_t sigwinch_pending;
static int redraw(struct snj_term *term);
static int move_prompt_cursor(struct snj_term *term);
static int position_prompt_cursor(struct snj_term *term, size_t row, size_t col);

static void
mark_sigint(int signal_number)
{
    (void)signal_number;
    if (sigint_pending < INT_MAX)
        ++sigint_pending;
}

static void
mark_sigwinch(int signal_number)
{
    (void)signal_number;
    sigwinch_pending = 1;
}

static size_t
decode_utf8(const unsigned char *s, size_t len, uint32_t *cp)
{
    size_t need = snj_utf8_size(s[0]);
    uint32_t value;

    if (!need || len < need)
        return 0u;
    if (need == 1u) {
        *cp = s[0];
        return 1u;
    }
    value = s[0] & (need == 2u ? 0x1fu : need == 3u ? 0x0fu : 0x07u);
    for (size_t i = 1u; i < need; ++i) {
        if ((s[i] & 0xc0u) != 0x80u)
            return 0u;
        value = (value << 6) | (uint32_t)(s[i] & 0x3fu);
    }
    if (!snj_utf8_valid(s, need, true))
        return 0u;
    *cp = value;
    return need;
}

static bool
format_unsafe(uint32_t cp)
{
    return cp == 0x00adu || cp == 0x061cu || cp == 0x200bu ||
           cp == 0x200eu || cp == 0x200fu ||
           (cp >= 0x202au && cp <= 0x202eu) || cp == 0x2060u ||
           (cp >= 0x2066u && cp <= 0x206fu) || cp == 0xfeffu ||
           (cp >= 0xfff9u && cp <= 0xfffbu);
}

static int
append_escape(struct snj_buf *out, uint32_t cp)
{
    if (cp <= 0xffu)
        return snj_buf_printf(out, "\\x%02X", (unsigned int)cp);
    return snj_buf_printf(out, "\\u{%X}", (unsigned int)cp);
}

static int
append_safe(struct snj_buf *out, const unsigned char *text, size_t len,
            bool prompt, size_t indent, unsigned int columns,
            size_t stop, size_t *stop_row, size_t *stop_col,
            size_t *end_row, size_t *end_col)
{
    size_t row = 0u;
    size_t col = indent;
    size_t i = 0u;

    if (columns >= 20u && col >= columns) {
        row = col / columns;
        col %= columns;
    }
    if (stop == 0u) {
        *stop_row = row;
        *stop_col = col;
    }
    while (i < len) {
        uint32_t cp;
        size_t n = decode_utf8(text + i, len - i, &cp);
        int width = 0;
        size_t before = out->len;

        if (!n) {
            cp = text[i];
            n = 1u;
        }
        if (cp == '\n') {
            if (prompt) {
                if (snj_buf_append(out, "\r\n", 2u) < 0)
                    return -1;
                for (size_t j = 0u; j < indent; ++j)
                    if (snj_buf_putc(out, ' ') < 0)
                        return -1;
                ++row;
                col = indent;
                if (columns >= 20u && col >= columns) {
                    row += col / columns;
                    col %= columns;
                }
            } else if (snj_buf_putc(out, '\n') < 0) {
                return -1;
            }
        } else if (cp == '\t') {
            size_t spaces = 4u - (col % 4u);
            for (size_t j = 0u; j < spaces; ++j)
                if (snj_buf_putc(out, ' ') < 0)
                    return -1;
            width = (int)spaces;
        } else if (cp < 0x20u || cp == 0x7fu ||
                   (cp >= 0x80u && cp <= 0x9fu) || format_unsafe(cp)) {
            if (append_escape(out, cp) < 0)
                return -1;
            width = (int)(out->len - before);
        } else {
            int w = cp <= (uint32_t)WCHAR_MAX ? wcwidth((wchar_t)cp) : -1;
            if (w < 0) {
                if (append_escape(out, cp) < 0)
                    return -1;
                width = (int)(out->len - before);
            } else {
                if (snj_buf_append(out, text + i, n) < 0)
                    return -1;
                width = w;
            }
        }
        if (cp != '\n' || !prompt) {
            if (columns >= 20u && width > 0) {
                size_t w = (size_t)width;
                if (col > SIZE_MAX - w)
                    return -1;
                col += w;
                while (col >= columns) {
                    col -= columns;
                    ++row;
                }
            } else if (width > 0) {
                col += (size_t)width;
            }
        }
        i += n;
        if (i == stop) {
            *stop_row = row;
            *stop_col = col;
        }
    }
    *end_row = row;
    *end_col = col;
    return 0;
}

size_t
snj_term_text_width(const char *value, size_t len)
{
    const unsigned char *text = (const unsigned char *)value;
    size_t width = 0u;
    size_t i = 0u;

    while (i < len) {
        uint32_t cp;
        size_t n = decode_utf8(text + i, len - i, &cp);
        int w;

        if (!n) {
            cp = text[i];
            n = 1u;
        }
        if (cp == '\t') {
            size_t spaces = 4u - (width % 4u);
            if (width > SIZE_MAX - spaces) {
                errno = EOVERFLOW;
                return SIZE_MAX;
            }
            width += spaces;
        } else if (cp < 0x20u || cp == 0x7fu ||
                   (cp >= 0x80u && cp <= 0x9fu) || format_unsafe(cp)) {
            struct snj_buf out;
            size_t escaped;

            snj_buf_init(&out, 32u);
            if (append_escape(&out, cp) < 0) {
                snj_buf_free(&out);
                return SIZE_MAX;
            }
            escaped = out.len;
            snj_buf_free(&out);
            if (width > SIZE_MAX - escaped) {
                errno = EOVERFLOW;
                return SIZE_MAX;
            }
            width += escaped;
        } else {
            w = cp <= (uint32_t)WCHAR_MAX ? wcwidth((wchar_t)cp) : -1;
            if (w < 0) {
                struct snj_buf out;
                size_t escaped;

                snj_buf_init(&out, 32u);
                if (append_escape(&out, cp) < 0) {
                    snj_buf_free(&out);
                    return SIZE_MAX;
                }
                escaped = out.len;
                snj_buf_free(&out);
                if (width > SIZE_MAX - escaped) {
                    errno = EOVERFLOW;
                    return SIZE_MAX;
                }
                width += escaped;
            } else if ((size_t)w > SIZE_MAX - width) {
                errno = EOVERFLOW;
                return SIZE_MAX;
            } else {
                width += (size_t)w;
            }
        }
        i += n;
    }
    return width;
}

int
snj_term_write_safe(int fd, const char *text, size_t len)
{
    struct snj_buf out;
    size_t unused = 0u;
    size_t max;
    int rc;

    if (len > (SIZE_MAX - 32u) / 8u) {
        errno = EOVERFLOW;
        return -1;
    }
    max = len * 8u + 32u;
    snj_buf_init(&out, max);
    rc = append_safe(&out, (const unsigned char *)text, len, false, 0u, 0u,
                     len + 1u, &unused, &unused, &unused, &unused);
    if (rc == 0)
        rc = snj_write_full(fd, out.data, out.len);
    snj_buf_free(&out);
    return rc;
}

static int
snj_term_append_safe(struct snj_buf *out, const char *text, size_t len)
{
    size_t unused = 0u;

    return append_safe(out, (const unsigned char *)text, len, false, 0u, 0u,
                       len + 1u, &unused, &unused, &unused, &unused);
}

void
snj_term_init(struct snj_term *term)
{
    memset(term, 0, sizeof(*term));
    snj_buf_init(&term->draft, SNJ_MAX_DIRECT_PROMPT + 1u);
    snj_buf_init(&term->search_label, SNJ_MAX_DIRECT_PROMPT + 64u);
    snj_buf_init(&term->search_query, SNJ_MAX_DIRECT_PROMPT + 1u);
    term->columns = 80u;
    term->history_pos = SIZE_MAX;
    term->search_pos = SIZE_MAX;
}

void
snj_term_set_typing_pause(struct snj_term *term, uint32_t pause_ms)
{
    if (term)
        term->typing_pause_ms = pause_ms;
}

void
snj_term_set_color(struct snj_term *term, bool enabled, bool networked)
{
    if (!term)
        return;
    term->color = enabled;
    term->networked = networked;
}

uint32_t
snj_term_typing_pause_remaining(const struct snj_term *term, uint64_t now_ms)
{
    uint64_t elapsed;

    if (!term || !term->active || !term->typing_active ||
        term->typing_pause_ms == 0u)
        return 0u;
    elapsed = now_ms >= term->last_input_ms ? now_ms - term->last_input_ms : 0u;
    return elapsed >= term->typing_pause_ms ? 0u :
           term->typing_pause_ms - (uint32_t)elapsed;
}

bool
snj_term_typing_active(const struct snj_term *term)
{
    return term && term->active && term->typing_active;
}

void
snj_term_note_output(struct snj_term *term, const char *text, size_t len)
{
    if (!term || !len)
        return;
    term->output_seen = true;
    term->output_ended_lf = text[len - 1u] == '\n';
}

unsigned int
snj_term_columns(const struct snj_term *term)
{
    return term ? term->columns : 80u;
}

void
snj_term_set_commands(struct snj_term *term,
                      const struct snj_term_command *commands, size_t count)
{
    if (!term)
        return;
    term->commands = commands;
    term->command_count = commands ? count : 0u;
}

static bool
term_control_capable(void)
{
    const char *name = getenv("TERM");

    return name && strcmp(name, "dumb") != 0;
}

static void
update_size(struct snj_term *term)
{
    struct winsize size;

    memset(&size, 0, sizeof(size));
    if (!term_control_capable()) {
        term->columns = 80u;
        term->capable = false;
        return;
    }
    if (ioctl(STDERR_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col != 0u) {
        if (size.ws_col >= 20u) {
            term->columns = size.ws_col;
            if (term->raw)
                term->capable = true;
        } else {
            term->columns = 80u;
            term->capable = false;
        }
    } else {
        term->columns = 80u;
    }
}

static int
set_raw(struct snj_term *term)
{
    struct termios raw = term->saved;

    raw.c_iflag &= (tcflag_t)~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_lflag &= (tcflag_t)~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) < 0)
        return -1;
    term->raw = true;
    return 0;
}

int
snj_term_open(struct snj_term *term, char *error, size_t error_size)
{
    struct sigaction action;

    if (term->opened) {
        errno = EALREADY;
        snj_errorf(error, error_size, "terminal already open: %s", strerror(errno));
        return -1;
    }
    if (tcgetattr(STDIN_FILENO, &term->saved) < 0) {
        snj_errorf(error, error_size, "cannot read terminal attributes: %s", strerror(errno));
        return -1;
    }
    term->capable = term_control_capable();
    update_size(term);
    if (term->capable && set_raw(term) < 0) {
        snj_errorf(error, error_size, "cannot enter terminal input mode: %s", strerror(errno));
        return -1;
    }
    memset(&action, 0, sizeof(action));
    action.sa_handler = mark_sigint;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, &term->saved_sigint) < 0) {
        int saved_errno = errno;
        if (term->raw)
            (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &term->saved);
        term->raw = false;
        errno = saved_errno;
        snj_errorf(error, error_size, "cannot install terminal interrupt handler: %s", strerror(errno));
        return -1;
    }
    term->sigint_installed = true;
    memset(&action, 0, sizeof(action));
    action.sa_handler = mark_sigwinch;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGWINCH, &action, &term->saved_sigwinch) < 0) {
        int saved_errno = errno;
        if (term->raw)
            (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &term->saved);
        (void)sigaction(SIGINT, &term->saved_sigint, NULL);
        term->raw = false;
        term->sigint_installed = false;
        errno = saved_errno;
        snj_errorf(error, error_size, "cannot install terminal resize handler: %s", strerror(errno));
        return -1;
    }
    term->sigwinch_installed = true;
    sigint_pending = 0;
    sigwinch_pending = 0;
    term->opened = true;
    if (term->capable && snj_write_full(STDERR_FILENO, "\033[?2004h", 8u) < 0) {
        int saved_errno = errno;
        (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &term->saved);
        (void)sigaction(SIGINT, &term->saved_sigint, NULL);
        (void)sigaction(SIGWINCH, &term->saved_sigwinch, NULL);
        term->opened = false;
        term->raw = false;
        term->sigint_installed = false;
        term->sigwinch_installed = false;
        errno = saved_errno;
        snj_errorf(error, error_size, "cannot enable bracketed paste: %s", strerror(errno));
        return -1;
    }
    term->bracketed_paste = term->capable;
    return 0;
}

int
snj_term_external_begin(struct snj_term *term,
                        char *error, size_t error_size)
{
    if (!term || !term->opened) {
        errno = EINVAL;
        snj_errorf(error, error_size, "terminal is not open: %s",
                   strerror(errno));
        return -1;
    }
    if (snj_term_hide(term) < 0)
        goto fail;
    if (term->bracketed_paste &&
        snj_write_full(STDERR_FILENO, "\033[?2004l", 8u) < 0)
        goto fail;
    term->bracketed_paste = false;
    if (term->raw && tcsetattr(STDIN_FILENO, TCSAFLUSH, &term->saved) < 0)
        goto fail;
    term->raw = false;
    return 0;
fail:
    snj_errorf(error, error_size, "cannot release terminal for editor: %s",
               strerror(errno));
    return -1;
}

int
snj_term_external_end(struct snj_term *term,
                      char *error, size_t error_size)
{
    if (!term || !term->opened) {
        errno = EINVAL;
        snj_errorf(error, error_size, "terminal is not open: %s",
                   strerror(errno));
        return -1;
    }
    sigint_pending = 0;
    sigwinch_pending = 0;
    update_size(term);
    if (term->capable && set_raw(term) < 0)
        goto fail;
    if (term->capable &&
        snj_write_full(STDERR_FILENO, "\033[?2004h", 8u) < 0)
        goto fail;
    term->bracketed_paste = term->capable;
    return 0;
fail:
    snj_errorf(error, error_size, "cannot restore terminal after editor: %s",
               strerror(errno));
    return -1;
}

static int
move_cursor(size_t amount, char direction)
{
    char sequence[64];
    int n;

    if (!amount)
        return 0;
    n = snprintf(sequence, sizeof(sequence), "\033[%zu%c", amount, direction);
    if (n < 0 || (size_t)n >= sizeof(sequence)) {
        errno = EOVERFLOW;
        return -1;
    }
    return snj_write_full(STDERR_FILENO, sequence, (size_t)n);
}

static int
materialize_prompt_wrap(struct snj_term *term)
{
    if (!term->rendered_cursor_pending_wrap)
        return 0;
    if (snj_write_full(STDERR_FILENO, " \b", 2u) < 0)
        return -1;
    term->rendered_cursor_pending_wrap = false;
    return 0;
}

int
snj_term_hide(struct snj_term *term)
{
    if (!term->opened || !term->prompt_visible)
        return 0;
    if (!term->capable) {
        term->prompt_visible = false;
        return snj_write_full(STDERR_FILENO, "\n", 1u);
    }
    if (materialize_prompt_wrap(term) < 0 ||
        (term->rendered_cursor_row + 1u < term->rendered_rows &&
         move_cursor(term->rendered_rows - term->rendered_cursor_row - 1u,
                     'B') < 0))
        return -1;
    for (size_t row = term->rendered_rows; row != 0u; --row) {
        if (snj_write_full(STDERR_FILENO, "\r\033[2K", 5u) < 0)
            return -1;
        if (row > 1u && snj_write_full(STDERR_FILENO, "\033[1A", 4u) < 0)
            return -1;
    }
    if (snj_write_full(STDERR_FILENO, "\r", 1u) < 0)
        return -1;
    term->prompt_visible = false;
    term->rendered_rows = 0u;
    term->rendered_cursor_row = 0u;
    term->rendered_cursor_col = 0u;
    term->rendered_end_at_margin = false;
    term->rendered_cursor_pending_wrap = false;
    return 0;
}

static int
leave_prompt(struct snj_term *term)
{
    if (!term->opened || !term->prompt_visible)
        return 0;
    if (term->capable &&
        (materialize_prompt_wrap(term) < 0 ||
         (term->rendered_cursor_row + 1u < term->rendered_rows &&
          move_cursor(term->rendered_rows - term->rendered_cursor_row - 1u,
                      'B') < 0)))
        return -1;
    if (term->capable && term->rendered_end_at_margin) {
        if (snj_write_full(STDERR_FILENO, "\r", 1u) < 0)
            return -1;
    } else if (snj_write_full(STDERR_FILENO,
                              term->capable ? "\r\n" : "\n",
                              term->capable ? 2u : 1u) < 0) {
        return -1;
    }
    term->prompt_visible = false;
    term->rendered_rows = 0u;
    term->rendered_cursor_row = 0u;
    term->rendered_cursor_col = 0u;
    term->rendered_end_at_margin = false;
    term->rendered_cursor_pending_wrap = false;
    term->output_seen = false;
    term->output_ended_lf = true;
    return 0;
}

static void
mark_input_activity(struct snj_term *term)
{
    if (!term->active)
        return;
    term->typing_active = true;
    term->last_input_ms = snj_time_ms();
}

static int
prompt_render_max(const unsigned char *text, size_t len, size_t indent,
                  size_t extra, size_t *max)
{
    size_t newlines = 0u;
    size_t total;

    for (size_t i = 0u; i < len; ++i)
        if (text[i] == '\n')
            ++newlines;
    if (len > (SIZE_MAX - extra) / 4u) {
        errno = EOVERFLOW;
        return -1;
    }
    total = len * 4u + extra;
    if (newlines && indent > (SIZE_MAX - total) / newlines) {
        errno = EOVERFLOW;
        return -1;
    }
    *max = total + newlines * indent;
    return 0;
}

static const char *
prompt_label(const struct snj_term *term, size_t *len)
{
    if (term->searching) {
        *len = term->search_label.len;
        return (const char *)term->search_label.data;
    }
    *len = strlen(term->label);
    return term->label;
}

static uint64_t
monotonic_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return 0u;
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

void
snj_term_capture_prompt_clock(struct snj_term *term, time_t seconds)
{
    struct tm local;
    struct snj_prompt_clock *clock = &term->prompt_clock;

    if (clock->captured)
        return;
    clock->captured = true;
    clock->valid = seconds != (time_t)-1 && localtime_r(&seconds, &local) &&
                   local.tm_hour >= 0 && local.tm_hour <= 23 &&
                   local.tm_min >= 0 && local.tm_min <= 59 &&
                   local.tm_sec >= 0 && local.tm_sec <= 60;
    if (clock->valid) {
        clock->hour = local.tm_hour;
        clock->minute = local.tm_min;
        clock->second = local.tm_sec;
    }
}

static int
prepare_spinner(struct snj_term_spinner *spinner, const char *value)
{
    size_t pos = value[0] == '\\' && value[1] == '0' ? 2u :
                 snj_utf8_size((unsigned char)value[0]);

    memset(spinner, 0, sizeof(*spinner));
    spinner->value = value;
    spinner->inactive_len = value[0] == '\\' && value[1] == '0' ? 0u :
                            (unsigned char)pos;
    while (value[pos]) {
        size_t n = snj_utf8_size((unsigned char)value[pos]);

        if (!n || spinner->frame_count >= 16u)
            return -1;
        spinner->frame_offset[spinner->frame_count] = pos;
        spinner->frame_len[spinner->frame_count++] = (unsigned char)n;
        pos += n;
    }
    return 0;
}

static int
compose_prompt(const char *prompt, const struct snj_term_spinner *spinners,
               unsigned int states, uint64_t step, char *label,
               struct snj_term_spinner next[SNJ_TERM_SPINNER_COUNT])
{
    size_t used = 0u;

    memcpy(next, spinners, sizeof(*spinners) * SNJ_TERM_SPINNER_COUNT);
    for (size_t i = 0u; i < SNJ_TERM_SPINNER_COUNT; ++i) {
        next[i].present = false;
        next[i].current_len = 0u;
    }
    for (const unsigned char *p = (const unsigned char *)prompt; *p; ++p) {
        if (*p >= SNJ_TERM_SPINNER_MARKER_BASE) {
            struct snj_term_spinner *cell =
                &next[*p - SNJ_TERM_SPINNER_MARKER_BASE];
            size_t offset = 0u, len = cell->inactive_len;

            if (cell->present)
                return -1;
            cell->present = true;
            cell->label_offset = used;
            if ((states & (1u <<
                    (*p - SNJ_TERM_SPINNER_MARKER_BASE))) &&
                cell->frame_count) {
                unsigned int frame = (unsigned int)(step % cell->frame_count);
                offset = cell->frame_offset[frame];
                len = cell->frame_len[frame];
            }
            if (used > SNJ_TERM_LABEL_BYTES - 1u - len)
                return -1;
            memcpy(label + used, cell->value + offset, len);
            cell->current_len = (unsigned char)len;
            used += len;
        } else {
            if (used == SNJ_TERM_LABEL_BYTES - 1u)
                return -1;
            label[used++] = (char)*p;
        }
    }
    label[used] = '\0';
    return used ? 0 : -1;
}

static int
prompt_fits(const char *prompt,
            const struct snj_term_spinner spinners[SNJ_TERM_SPINNER_COUNT])
{
    size_t used = 0u;
    unsigned int seen = 0u;

    for (const unsigned char *p = (const unsigned char *)prompt; *p; ++p) {
        size_t len = 1u;

        if (*p >= SNJ_TERM_SPINNER_MARKER_BASE) {
            unsigned int id = *p - SNJ_TERM_SPINNER_MARKER_BASE;
            const struct snj_term_spinner *cell = &spinners[id];

            if (seen & (1u << id))
                return -1;
            seen |= 1u << id;
            len = cell->inactive_len;
            for (size_t i = 0u; i < cell->frame_count; ++i)
                if (cell->frame_len[i] > len)
                    len = cell->frame_len[i];
        }
        if (used > SNJ_TERM_LABEL_BYTES - 1u - len)
            return -1;
        used += len;
    }
    return used ? 0 : -1;
}

static void
install_prompt(struct snj_term *term, const char *label,
               const struct snj_term_spinner cells[SNJ_TERM_SPINNER_COUNT])
{
    memcpy(term->label, label, strlen(label) + 1u);
    memcpy(term->spinner, cells, sizeof(term->spinner));
}

static int
paint_spinners(struct snj_term *term, const char *old_label,
               const struct snj_term_spinner old[SNJ_TERM_SPINNER_COUNT])
{
    bool painted = false;

    for (size_t i = 0u; i < SNJ_TERM_SPINNER_COUNT; ++i) {
        const struct snj_term_spinner *cell = &term->spinner[i];
        size_t width, row, col;

        if (!cell->present || !cell->current_len ||
            (old[i].current_len == cell->current_len &&
             memcmp(old_label + old[i].label_offset,
                    term->label + cell->label_offset,
                    cell->current_len) == 0))
            continue;
        width = snj_term_text_width(old_label, old[i].label_offset);
        if (width == SIZE_MAX)
            return -1;
        row = width / term->columns;
        col = width % term->columns;
        if (position_prompt_cursor(term, row, col) < 0 ||
            (term->color && snj_write_full(STDERR_FILENO,
                term->networked ? "\033[1;35m" : "\033[1;36m", 7u) < 0) ||
            snj_write_full(STDERR_FILENO, term->label + cell->label_offset,
                           cell->current_len) < 0 ||
            (term->color && snj_write_full(STDERR_FILENO, "\033[0m", 4u) < 0))
            return -1;
        if (++col == term->columns) {
            ++row;
            col = 0u;
            term->rendered_cursor_pending_wrap = true;
        }
        term->rendered_cursor_row = row;
        term->rendered_cursor_col = col;
        painted = true;
    }
    return painted ? move_prompt_cursor(term) : 0;
}

static int
update_spinners(struct snj_term *term, uint64_t step)
{
    struct snj_term_spinner old[SNJ_TERM_SPINNER_COUNT], next[SNJ_TERM_SPINNER_COUNT];
    char old_label[SNJ_TERM_LABEL_BYTES], label[SNJ_TERM_LABEL_BYTES];
    bool stable = true, visible;

    memcpy(old, term->spinner, sizeof(old));
    memcpy(old_label, term->label, sizeof(old_label));
    if (compose_prompt(term->prompt_template, term->spinner,
                       term->spinner_states, step, label, next) < 0)
        return -1;
    for (size_t i = 0u; i < SNJ_TERM_SPINNER_COUNT; ++i)
        if (!!old[i].current_len != !!next[i].current_len)
            stable = false;
    visible = term->prompt_visible && term->capable && !term->searching &&
              !term->output_depth;
    if (visible && !stable && snj_term_hide(term) < 0)
        return -1;
    install_prompt(term, label, next);
    if (!visible)
        return 0;
    return stable ? paint_spinners(term, old_label, old) : redraw(term);
}

static bool
animated_spinners(const struct snj_term *term)
{
    for (size_t i = 0u; i < SNJ_TERM_SPINNER_COUNT; ++i)
        if (term->spinner[i].present && term->spinner[i].frame_count > 1u &&
            (term->spinner_states & (1u << i)))
            return true;
    return false;
}

static uint64_t
spinner_step(const struct snj_term *term, uint64_t now)
{
    uint64_t elapsed = now >= term->spinner_epoch_ms ?
                       now - term->spinner_epoch_ms : 0u;
    return elapsed * term->spinner_per_second / 1000u;
}

static int
spinner_timeout(struct snj_term *term, int timeout_ms)
{
    uint64_t now, elapsed, step, boundary, wait;

    if (!term->prompt_visible || !term->capable || term->searching ||
        term->output_depth || !animated_spinners(term))
        return timeout_ms;
    now = monotonic_ms();
    elapsed = now >= term->spinner_epoch_ms ? now - term->spinner_epoch_ms : 0u;
    step = spinner_step(term, now);
    boundary = ((step + 1u) * 1000u + term->spinner_per_second - 1u) /
               term->spinner_per_second;
    wait = boundary > elapsed ? boundary - elapsed : 1u;
    return timeout_ms < 0 || wait < (uint64_t)timeout_ms ? (int)wait : timeout_ms;
}

static int
sync_prompt_layout_after_resize(struct snj_term *term)
{
    struct snj_buf scratch;
    size_t cursor_row = 0u, cursor_col = 0u;
    size_t end_row = 0u, end_col = 0u;
    size_t label_len;
    const char *label = prompt_label(term, &label_len);
    size_t label_cols = snj_term_text_width(label, label_len);
    size_t max;
    int rc = -1;

    if (label_cols == SIZE_MAX ||
        prompt_render_max(term->draft.data, term->draft.len, label_cols, 32u,
                          &max) < 0)
        return -1;
    snj_buf_init(&scratch, max);
    if (append_safe(&scratch, term->draft.data, term->draft.len, true,
                    label_cols, term->columns, term->cursor,
                    &cursor_row, &cursor_col, &end_row, &end_col) < 0)
        goto out;
    /*
     * tmux and VT terminals represent an exact-margin cursor as the pending
     * wrap position on the preceding row after reflow.  The renderer tracks
     * that position as column zero on the next row.  Materialize the pending
     * wrap before erase/movement commands use the recomputed row counts.
     */
    if (cursor_row != 0u && cursor_col == 0u &&
        snj_write_full(STDERR_FILENO, " \b", 2u) < 0)
        goto out;
    term->rendered_rows = end_row + 1u;
    term->rendered_cursor_row = cursor_row;
    term->rendered_cursor_col = cursor_col;
    term->rendered_end_at_margin = end_row != 0u && end_col == 0u;
    term->rendered_cursor_pending_wrap = false;
    rc = 0;
out:
    snj_buf_free(&scratch);
    return rc;
}

static int
redraw(struct snj_term *term)
{
    struct snj_buf out;
    struct snj_term_spinner cells[SNJ_TERM_SPINNER_COUNT];
    char current[SNJ_TERM_LABEL_BYTES];
    size_t cursor_row = 0u, cursor_col = 0u;
    size_t end_row = 0u, end_col = 0u;
    size_t label_len;
    const char *label;
    size_t label_cols;
    size_t max;
    int rc = -1;

    if (!term->opened || !term->prompt_wanted || term->output_depth)
        return 0;
    if (term->prompt_template[0] &&
        compose_prompt(term->prompt_template, term->spinner,
                       term->spinner_states,
                       spinner_step(term, monotonic_ms()), current, cells) < 0)
        return -1;
    if (term->prompt_template[0])
        install_prompt(term, current, cells);
    label = prompt_label(term, &label_len);
    label_cols = snj_term_text_width(label, label_len);
    if (term->active && term->typing_active && !term->prompt_visible &&
        term->output_seen) {
        if (!term->output_ended_lf &&
            snj_write_full(STDERR_FILENO, term->capable ? "\r\n" : "\n",
                           term->capable ? 2u : 1u) < 0)
            return -1;
        term->output_seen = false;
    }
    if (!term->capable) {
        int fallback_rc = 0;

        if (term->prompt_visible)
            return 0;
        if (fallback_rc == 0 && term->color &&
            snj_write_full(STDERR_FILENO,
                           term->networked ? "\033[1;35m" : "\033[1;36m",
                           7u) < 0)
            fallback_rc = -1;
        if (fallback_rc == 0 &&
            snj_term_write_safe(STDERR_FILENO, label, label_len) < 0)
            fallback_rc = -1;
        if (fallback_rc == 0 && term->color &&
            snj_write_full(STDERR_FILENO, "\033[0m", 4u) < 0)
            fallback_rc = -1;
        if (fallback_rc == 0 && term->draft.len &&
            snj_term_write_safe(STDERR_FILENO, (char *)term->draft.data,
                                term->draft.len) < 0)
            fallback_rc = -1;
        if (fallback_rc == 0)
            term->prompt_visible = true;
        return fallback_rc;
    }
    if (term->prompt_visible && snj_term_hide(term) < 0)
        return -1;
    if (label_cols == SIZE_MAX ||
        prompt_render_max(term->draft.data, term->draft.len, label_cols,
                          SNJ_TERM_LABEL_BYTES * 4u + 64u, &max) < 0)
        return -1;
    snj_buf_init(&out, max);
    if ((term->color &&
         snj_buf_append(&out, term->networked ? "\033[1;35m" : "\033[1;36m",
                        7u) < 0) ||
        snj_term_append_safe(&out, label, label_len) < 0 ||
        (term->color && snj_buf_append(&out, "\033[0m", 4u) < 0) ||
        append_safe(&out, term->draft.data, term->draft.len, true, label_cols,
                    term->columns, term->cursor,
                    &cursor_row, &cursor_col, &end_row, &end_col) < 0)
        goto out;
    /*
     * A VT-style terminal keeps the cursor in a pending-wrap state after a
     * printable character fills the right margin.  Our row accounting already
     * places that cursor at column zero of the next row.  Force the pending
     * wrap before emitting cursor controls, otherwise a later redraw can think
     * the prompt occupies one row more than it really does and erase the model
     * output immediately above it.
     */
    if (end_row != 0u && end_col == 0u &&
        snj_buf_append(&out, " \b", 2u) < 0)
        goto out;
    if (snj_buf_append(&out, "\033[K", 3u) < 0 ||
        snj_write_full(STDERR_FILENO, out.data, out.len) < 0)
        goto out;
    if (end_row > cursor_row && move_cursor(end_row - cursor_row, 'A') < 0)
        goto out;
    if (snj_write_full(STDERR_FILENO, "\r", 1u) < 0 ||
        move_cursor(cursor_col, 'C') < 0)
        goto out;
    term->rendered_rows = end_row + 1u;
    term->rendered_cursor_row = cursor_row;
    term->rendered_cursor_col = cursor_col;
    term->rendered_end_at_margin = end_row != 0u && end_col == 0u;
    term->rendered_cursor_pending_wrap = false;
    term->prompt_visible = true;
    rc = 0;
out:
    snj_buf_free(&out);
    return rc;
}

int
snj_term_set_prompt(struct snj_term *term, bool active)
{
    const char *label = active ? "» " : "› ";

    return snj_term_set_prompt_label(term, active, label);
}

int
snj_term_set_prompt_label(struct snj_term *term, bool active,
                          const char *label)
{
    size_t len;

    if (!term || !label || !(len = strlen(label)) || len >= sizeof(term->label)) {
        errno = EINVAL;
        return -1;
    }
    if (snj_term_hide(term) < 0)
        return -1;
    term->active = active;
    if (!active)
        term->typing_active = false;
    term->prompt_wanted = true;
    term->line_submission_echoed = false;
    term->prompt_template[0] = '\0';
    term->spinner_states = 0u;
    memset(term->spinner, 0, sizeof(term->spinner));
    memcpy(term->label, label, len + 1u);
    if (term->output_depth)
        term->redraw_after_output = true;
    return redraw(term);
}

int
snj_term_set_prompt_template(struct snj_term *term, bool active,
                             const char *label,
                             const char *const spinners[SNJ_TERM_SPINNER_COUNT],
                             uint32_t per_second, unsigned int states)
{
    struct snj_term_spinner configured[SNJ_TERM_SPINNER_COUNT];
    struct snj_term_spinner cells[SNJ_TERM_SPINNER_COUNT];
    char expanded[SNJ_TERM_LABEL_BYTES];
    size_t len;

    if (!term || !label || !(len = strlen(label)) ||
        len >= sizeof(term->prompt_template) || !spinners ||
        per_second < 1u || per_second > 60u ||
        states >= (1u << SNJ_TERM_SPINNER_COUNT)) {
        errno = EINVAL;
        return -1;
    }
    for (size_t i = 0u; i < SNJ_TERM_SPINNER_COUNT; ++i)
        if (!spinners[i] || prepare_spinner(&configured[i], spinners[i]) < 0)
            goto invalid;
    if (prompt_fits(label, configured) < 0 ||
        compose_prompt(label, configured, states, 0u, expanded, cells) < 0)
        goto invalid;
    if (snj_term_hide(term) < 0)
        return -1;
    memcpy(term->prompt_template, label, len + 1u);
    install_prompt(term, expanded, cells);
    term->spinner_states = states;
    term->spinner_per_second = per_second;
    term->spinner_epoch_ms = monotonic_ms();
    term->active = active;
    if (!active)
        term->typing_active = false;
    term->prompt_wanted = true;
    term->line_submission_echoed = false;
    if (term->output_depth)
        term->redraw_after_output = true;
    return redraw(term);
invalid:
    errno = EINVAL;
    return -1;
}

int
snj_term_set_spinner_states(struct snj_term *term, unsigned int states)
{
    if (!term || states >= (1u << SNJ_TERM_SPINNER_COUNT)) {
        errno = EINVAL;
        return -1;
    }
    if (!term->prompt_template[0] || term->spinner_states == states)
        return 0;
    term->spinner_states = states;
    term->spinner_epoch_ms = monotonic_ms();
    return update_spinners(term, 0u);
}

const char *
snj_term_prompt_label(const struct snj_term *term)
{
    return term ? term->label : NULL;
}

int
snj_term_output_begin(struct snj_term *term, bool persistent)
{
    if (!term || !term->opened)
        return 0;
    (void)persistent;
    if (term->output_depth == UINT_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    if (term->output_depth++ == 0u) {
        int rc;

        if (term->active && term->typing_active) {
            rc = leave_prompt(term);
            term->typing_active = false;
        } else {
            rc = snj_term_hide(term);
        }
        if (rc < 0) {
            --term->output_depth;
            return -1;
        }
    }
    return 0;
}

int
snj_term_output_end(struct snj_term *term)
{
    bool redraw_requested;

    if (!term || !term->opened)
        return 0;
    if (!term->output_depth) {
        errno = EINVAL;
        return -1;
    }
    --term->output_depth;
    if (term->output_depth)
        return 0;
    redraw_requested = term->redraw_after_output;
    term->redraw_after_output = false;
    return term->active && !redraw_requested ? 0 : redraw(term);
}

static void
history_reset_navigation(struct snj_term *term)
{
    free(term->history_draft);
    term->history_draft = NULL;
    term->history_pos = SIZE_MAX;
}

static size_t previous_cp(const unsigned char *s, size_t pos);

static void
history_clear(struct snj_term *term)
{
    history_reset_navigation(term);
    for (size_t i = 0u; i < term->history_count; ++i)
        free(term->history[i]);
    term->history_count = 0u;
    term->history_bytes = 0u;
}

static void
history_note_warning(struct snj_term *term)
{
    if (!term->history_warned)
        term->history_warning = true;
    term->history_warned = true;
}

bool
snj_term_take_history_warning(struct snj_term *term)
{
    bool pending = term && term->history_warning;

    if (term)
        term->history_warning = false;
    return pending;
}

static int
history_memory_add(struct snj_term *term, const char *text, bool *dropped)
{
    size_t len = strlen(text);
    char *copy;

    if (!len || len > SNJ_TERM_HISTORY_BYTES)
        return 0;
    copy = snj_strdup_checked(text, SNJ_TERM_HISTORY_BYTES);
    if (!copy)
        return -1;
    while (term->history_count == SNJ_TERM_HISTORY_COUNT ||
           term->history_bytes > SNJ_TERM_HISTORY_BYTES - len) {
        size_t old = strlen(term->history[0]);
        if (dropped)
            *dropped = true;
        free(term->history[0]);
        memmove(term->history, term->history + 1u,
                (term->history_count - 1u) * sizeof(term->history[0]));
        --term->history_count;
        term->history_bytes -= old;
    }
    term->history[term->history_count++] = copy;
    term->history_bytes += len;
    history_reset_navigation(term);
    return 0;
}

static int
history_lock(int fd)
{
    struct flock lock;

    memset(&lock, 0, sizeof(lock));
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    while (fcntl(fd, F_SETLKW, &lock) < 0)
        if (errno != EINTR)
            return -1;
    return 0;
}

static int
history_file_open(struct snj_term *term)
{
    struct stat st;
    int fd = open(term->history_path,
                  O_RDWR | O_APPEND | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    int flags;

    if (fd < 0)
        return -1;
    if (fstat(fd, &st) < 0) {
        int saved = errno;
        (void)close(fd);
        errno = saved;
        return -1;
    }
    if (!S_ISREG(st.st_mode) || st.st_uid != geteuid()) {
        (void)close(fd);
        errno = EACCES;
        return -1;
    }
    flags = fcntl(fd, F_GETFD);
    if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0 ||
        fchmod(fd, 0600) < 0 || history_lock(fd) < 0) {
        int saved = errno;
        (void)close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

static int
history_decode(const unsigned char *line, size_t len, struct snj_buf *out)
{
    static const char hex[] = "0123456789ABCDEF";

    snj_buf_reset(out);
    for (size_t i = 0u; i < len; ++i) {
        unsigned char c = line[i];

        if (c != '\\') {
            if (c < 0x20u || c == 0x7fu || snj_buf_putc(out, c) < 0)
                return -1;
            continue;
        }
        if (++i >= len)
            return -1;
        c = line[i];
        if (c == '\\') c = '\\';
        else if (c == 'n') c = '\n';
        else if (c == 'r') c = '\r';
        else if (c == 't') c = '\t';
        else if (c == 'x' && i + 2u < len) {
            const char *hi = strchr(hex, line[++i]);
            const char *lo = strchr(hex, line[++i]);
            if (!hi || !*hi || !lo || !*lo)
                return -1;
            c = (unsigned char)(((hi - hex) << 4) | (lo - hex));
        } else {
            return -1;
        }
        if (!c || snj_buf_putc(out, c) < 0)
            return -1;
    }
    if (!out->len || !snj_utf8_valid(out->data, out->len, true) ||
        snj_buf_terminate(out) < 0)
        return -1;
    return 0;
}

static int
history_encode(struct snj_buf *out, const char *text)
{
    static const char hex[] = "0123456789ABCDEF";
    const unsigned char *p = (const unsigned char *)text;

    snj_buf_reset(out);
    for (; *p; ++p) {
        unsigned char c = *p;
        const char *escape = c == '\\' ? "\\\\" : c == '\n' ? "\\n" :
                             c == '\r' ? "\\r" : c == '\t' ? "\\t" : NULL;
        if (escape) {
            if (snj_buf_append(out, escape, 2u) < 0)
                return -1;
        } else if (c < 0x20u || c == 0x7fu) {
            unsigned char encoded[4] = {'\\', 'x', hex[c >> 4], hex[c & 15u]};
            if (snj_buf_append(out, encoded, sizeof(encoded)) < 0)
                return -1;
        } else if (snj_buf_putc(out, c) < 0) {
            return -1;
        }
    }
    return 0;
}

static int
history_rewrite(struct snj_term *term, int fd)
{
    struct snj_buf encoded;
    int rc = -1;

    snj_buf_init(&encoded, HISTORY_FILE_BYTES);
    if (ftruncate(fd, 0) < 0 || lseek(fd, 0, SEEK_SET) < 0)
        goto out;
    for (size_t i = 0u; i < term->history_count; ++i)
        if (history_encode(&encoded, term->history[i]) < 0 ||
            snj_write_full(fd, encoded.data, encoded.len) < 0 ||
            snj_write_full(fd, "\n", 1u) < 0)
            goto out;
    rc = snj_sync_file(fd);
out:
    snj_buf_free(&encoded);
    return rc;
}

static int
history_load_locked(struct snj_term *term, int fd, bool *damaged)
{
    struct stat st;
    struct snj_buf file, decoded;
    size_t pos = 0u;
    bool dirty = false;
    int rc = -1;

    if (fstat(fd, &st) < 0 || st.st_size < 0 ||
        (uintmax_t)st.st_size > HISTORY_FILE_BYTES)
        return -1;
    snj_buf_init(&file, HISTORY_FILE_BYTES + 1u);
    snj_buf_init(&decoded, SNJ_TERM_HISTORY_BYTES + 1u);
    if (lseek(fd, 0, SEEK_SET) < 0)
        goto out;
    while (file.len < (size_t)st.st_size) {
        unsigned char chunk[4096];
        ssize_t got = read(fd, chunk, sizeof(chunk));
        if (got < 0) {
            if (errno == EINTR) continue;
            goto out;
        }
        if (!got) break;
        if (snj_buf_append(&file, chunk, (size_t)got) < 0)
            goto out;
    }
    history_clear(term);
    while (pos < file.len) {
        unsigned char *lf = memchr(file.data + pos, '\n', file.len - pos);
        bool dropped = false;
        size_t len;

        if (!lf) {
            dirty = *damaged = true;
            break;
        }
        len = (size_t)(lf - file.data - pos);
        if (history_decode(file.data + pos, len, &decoded) < 0) {
            dirty = *damaged = true;
        } else if (history_memory_add(term, (char *)decoded.data, &dropped) < 0) {
            goto out;
        } else if (dropped) {
            dirty = true;
        }
        pos += len + 1u;
    }
    rc = dirty ? history_rewrite(term, fd) : 0;
out:
    snj_buf_free(&decoded);
    snj_buf_free(&file);
    return rc;
}

static int
history_refresh(struct snj_term *term)
{
    bool damaged = false;
    int fd, rc, saved;

    if (!term->history_path)
        return 0;
    fd = history_file_open(term);
    if (fd < 0)
        goto fail;
    rc = history_load_locked(term, fd, &damaged);
    saved = errno;
    (void)close(fd);
    errno = saved;
    if (rc < 0)
        goto fail;
    if (damaged)
        history_note_warning(term);
    return 0;
fail:
    history_note_warning(term);
    return -1;
}

int
snj_term_history_open(struct snj_term *term, const char *dotdir)
{
    struct snj_buf path;

    if (!term || !dotdir || dotdir[0] != '/') {
        errno = EINVAL;
        return -1;
    }
    snj_buf_init(&path, SNJ_PATH_MAX_BYTES);
    if (snj_buf_printf(&path, "%s/prompt_history", dotdir) < 0 ||
        snj_buf_terminate(&path) < 0) {
        snj_buf_free(&path);
        history_note_warning(term);
        return -1;
    }
    free(term->history_path);
    term->history_path = (char *)path.data;
    path.data = NULL;
    snj_buf_free(&path);
    return history_refresh(term);
}

int
snj_term_history_add(struct snj_term *term, const char *text)
{
    struct snj_buf encoded;
    bool damaged = false, dropped = false, retained = false;
    int fd, rc = -1, saved;

    if (!term || !text || !*text)
        return 0;
    if (!term->history_path)
        return history_memory_add(term, text, NULL);
    fd = history_file_open(term);
    if (fd < 0)
        goto memory;
    if (history_load_locked(term, fd, &damaged) < 0)
        goto close_memory;
    snj_buf_init(&encoded, HISTORY_FILE_BYTES);
    if (history_encode(&encoded, text) < 0 ||
        snj_write_full(fd, encoded.data, encoded.len) < 0 ||
        snj_write_full(fd, "\n", 1u) < 0 ||
        history_memory_add(term, text, &dropped) < 0)
        goto encoded_out;
    retained = true;
    rc = dropped ? history_rewrite(term, fd) : snj_sync_file(fd);
encoded_out:
    snj_buf_free(&encoded);
    saved = errno;
    (void)close(fd);
    errno = saved;
    if (rc == 0) {
        if (damaged)
            history_note_warning(term);
        return 0;
    }
    history_note_warning(term);
    if (!retained)
        (void)history_memory_add(term, text, NULL);
    return -1;
close_memory:
    saved = errno;
    (void)close(fd);
    errno = saved;
memory:
    history_note_warning(term);
    if (history_memory_add(term, text, NULL) < 0)
        return -1;
    return -1;
}

static int
replace_draft(struct snj_term *term, const char *text)
{
    size_t len = strlen(text);

    if (len > SNJ_MAX_DIRECT_PROMPT) {
        errno = EOVERFLOW;
        return -1;
    }
    snj_buf_reset(&term->draft);
    if (snj_buf_append(&term->draft, text, len) < 0)
        return -1;
    term->cursor = len;
    term->utf8_pending_len = 0u;
    return redraw(term);
}

int
snj_term_restore_draft(struct snj_term *term, const char *text)
{
    term->prompt_wanted = true;
    mark_input_activity(term);
    return replace_draft(term, text);
}

bool
snj_term_consume_echoed_submission(struct snj_term *term, const char *label)
{
    bool match;

    if (!term || !term->line_submission_echoed)
        return false;
    match = strcmp(label, term->label) == 0;
    term->line_submission_echoed = false;
    return match;
}

static int
history_up(struct snj_term *term)
{
    if (term->history_pos == SIZE_MAX)
        (void)history_refresh(term);
    if (!term->history_count)
        return 0;
    if (term->history_pos == SIZE_MAX) {
        if (snj_buf_terminate(&term->draft) < 0)
            return -1;
        term->history_draft = snj_strdup_checked((char *)term->draft.data,
                                                 SNJ_MAX_DIRECT_PROMPT);
        if (!term->history_draft)
            return -1;
        term->history_pos = term->history_count;
    }
    if (term->history_pos)
        --term->history_pos;
    return replace_draft(term, term->history[term->history_pos]);
}

static int
history_down(struct snj_term *term)
{
    char *draft;

    if (term->history_pos == SIZE_MAX) {
        (void)history_refresh(term);
        return 0;
    }
    if (term->history_pos + 1u < term->history_count) {
        ++term->history_pos;
        return replace_draft(term, term->history[term->history_pos]);
    }
    draft = term->history_draft;
    term->history_draft = NULL;
    term->history_pos = SIZE_MAX;
    if (!draft)
        return replace_draft(term, "");
    {
        int rc = replace_draft(term, draft);
        free(draft);
        return rc;
    }
}

static int
search_label_update(struct snj_term *term)
{
    const char *prefix = term->search_failed ?
                         "(failed reverse-i-search)`" :
                         "(reverse-i-search)`";

    snj_buf_reset(&term->search_label);
    if (snj_buf_append(&term->search_label, prefix, strlen(prefix)) < 0 ||
        snj_buf_append(&term->search_label, term->search_query.data,
                       term->search_query.len) < 0 ||
        snj_buf_append(&term->search_label, "': ", 3u) < 0 ||
        snj_buf_terminate(&term->search_label) < 0)
        return -1;
    return 0;
}

static int
search_find(struct snj_term *term, size_t before)
{
    if (snj_buf_terminate(&term->search_query) < 0)
        return -1;
    while (before) {
        size_t i = --before;
        if (strstr(term->history[i], (char *)term->search_query.data)) {
            term->search_pos = i;
            term->search_failed = false;
            if (search_label_update(term) < 0)
                return -1;
            return replace_draft(term, term->history[i]);
        }
    }
    term->search_pos = 0u;
    term->search_failed = true;
    return search_label_update(term) < 0 ? -1 : redraw(term);
}

static int
search_begin(struct snj_term *term)
{
    if (term->searching)
        return search_find(term, term->search_pos);
    (void)history_refresh(term);
    if (snj_buf_terminate(&term->draft) < 0)
        return -1;
    term->search_original = snj_strdup_checked((char *)term->draft.data,
                                               SNJ_MAX_DIRECT_PROMPT);
    if (!term->search_original)
        return -1;
    term->search_original_cursor = term->cursor;
    snj_buf_reset(&term->search_query);
    if (snj_buf_append(&term->search_query, term->draft.data,
                       term->draft.len) < 0) {
        free(term->search_original);
        term->search_original = NULL;
        return -1;
    }
    term->searching = true;
    term->search_failed = false;
    term->search_pos = term->history_count;
    return search_find(term, term->history_count);
}

static int
search_accept(struct snj_term *term, bool abort)
{
    char *original = term->search_original;
    size_t cursor = term->search_original_cursor;
    int rc;

    if (!term->searching)
        return 0;
    term->searching = false;
    term->search_original = NULL;
    term->search_failed = false;
    term->search_pos = SIZE_MAX;
    snj_buf_reset(&term->search_query);
    snj_buf_reset(&term->search_label);
    history_reset_navigation(term);
    if (abort) {
        rc = replace_draft(term, original ? original : "");
        if (rc == 0) {
            term->cursor = cursor;
            rc = move_prompt_cursor(term);
        }
    } else {
        rc = redraw(term);
    }
    free(original);
    return rc;
}

static int
search_insert(struct snj_term *term, const unsigned char *data, size_t len)
{
    size_t before = term->search_failed ? 0u : term->search_pos + 1u;

    if (len > SNJ_MAX_DIRECT_PROMPT - term->search_query.len) {
        errno = EOVERFLOW;
        return -1;
    }
    if (snj_buf_append(&term->search_query, data, len) < 0)
        return -1;
    mark_input_activity(term);
    return search_find(term, before);
}

static int
search_backspace(struct snj_term *term)
{
    if (term->search_query.len)
        term->search_query.len = previous_cp(term->search_query.data,
                                             term->search_query.len);
    mark_input_activity(term);
    return search_find(term, term->history_count);
}

static size_t
previous_cp(const unsigned char *s, size_t pos)
{
    if (!pos)
        return 0u;
    --pos;
    while (pos && (s[pos] & 0xc0u) == 0x80u)
        --pos;
    return pos;
}

static size_t
next_cp(const unsigned char *s, size_t len, size_t pos)
{
    size_t n;

    if (pos >= len)
        return len;
    n = snj_utf8_size(s[pos]);
    return n && n <= len - pos ? pos + n : pos + 1u;
}

static int
position_prompt_cursor(struct snj_term *term, size_t row, size_t col)
{
    if ((row != term->rendered_cursor_row ||
         col != term->rendered_cursor_col) &&
        materialize_prompt_wrap(term) < 0)
        return -1;
    if (row < term->rendered_cursor_row &&
        move_cursor(term->rendered_cursor_row - row, 'A') < 0)
        return -1;
    if (row > term->rendered_cursor_row &&
        move_cursor(row - term->rendered_cursor_row, 'B') < 0)
        return -1;
    if (col != term->rendered_cursor_col &&
        (snj_write_full(STDERR_FILENO, "\r", 1u) < 0 ||
         move_cursor(col, 'C') < 0))
        return -1;
    term->rendered_cursor_row = row;
    term->rendered_cursor_col = col;
    return 0;
}

static int
write_prompt_suffix(const unsigned char *data, size_t len, size_t start_col,
                    bool start_pending, unsigned int columns,
                    bool *end_pending)
{
    size_t start = 0u;
    bool reset = false;
    size_t width;

    for (size_t i = 0u; i + 1u < len; ++i) {
        if (data[i] != '\r' || data[i + 1u] != '\n')
            continue;
        if (snj_write_full(STDERR_FILENO, data + start, i - start) < 0 ||
            snj_write_full(STDERR_FILENO, "\033[K\r\n", 5u) < 0)
            return -1;
        start = i + 2u;
        reset = true;
        ++i;
    }
    if (snj_write_full(STDERR_FILENO, data + start, len - start) < 0)
        return -1;
    if (columns < 20u) {
        *end_pending = false;
        return 0;
    }
    width = snj_term_text_width((const char *)data + start, len - start);
    if (width == SIZE_MAX)
        return -1;
    if (reset) {
        start_col = 0u;
        start_pending = false;
    }
    if (!width)
        *end_pending = start_pending;
    else
        *end_pending = ((start_col % columns + width % columns) % columns) == 0u;
    return 0;
}

static int
move_prompt_cursor(struct snj_term *term)
{
    struct snj_buf scratch;
    size_t cursor_row = 0u, cursor_col = 0u;
    size_t end_row = 0u, end_col = 0u;
    size_t label_len;
    const char *label = prompt_label(term, &label_len);
    size_t label_cols = snj_term_text_width(label, label_len);
    size_t max;
    int rc = -1;

    if (!term->capable || !term->prompt_visible || term->output_depth)
        return redraw(term);
    if (label_cols == SIZE_MAX ||
        prompt_render_max(term->draft.data, term->draft.len, label_cols, 32u,
                          &max) < 0)
        return -1;
    snj_buf_init(&scratch, max);
    if (append_safe(&scratch, term->draft.data, term->draft.len, true,
                    label_cols, term->columns, term->cursor,
                    &cursor_row, &cursor_col, &end_row, &end_col) < 0)
        goto out;
    rc = position_prompt_cursor(term, cursor_row, cursor_col);
out:
    snj_buf_free(&scratch);
    return rc;
}

static int
redraw_edit(struct snj_term *term, size_t changed_at)
{
    struct snj_buf prefix;
    struct snj_buf rendered;
    size_t unused_row = 0u, unused_col = 0u;
    size_t prefix_row = 0u, prefix_col = 0u;
    size_t cursor_row = 0u, cursor_col = 0u;
    size_t end_row = 0u, end_col = 0u;
    size_t label_len;
    const char *label = prompt_label(term, &label_len);
    size_t label_cols = snj_term_text_width(label, label_len);
    size_t old_rows = term->rendered_rows;
    size_t new_rows;
    size_t max;
    bool cursor_pending;
    int rc = -1;

    if (!term->capable || !term->prompt_visible || term->output_depth)
        return redraw(term);
    if (changed_at > term->cursor || changed_at > term->draft.len) {
        errno = EINVAL;
        return -1;
    }
    if (label_cols == SIZE_MAX ||
        prompt_render_max(term->draft.data, term->draft.len, label_cols, 32u,
                          &max) < 0)
        return -1;
    snj_buf_init(&prefix, max);
    snj_buf_init(&rendered, max);
    if (append_safe(&prefix, term->draft.data, changed_at, true,
                    label_cols, term->columns, changed_at + 1u,
                    &unused_row, &unused_col, &prefix_row, &prefix_col) < 0 ||
        append_safe(&rendered, term->draft.data, term->draft.len, true,
                    label_cols, term->columns, term->cursor,
                    &cursor_row, &cursor_col, &end_row, &end_col) < 0)
        goto out;

    new_rows = end_row + 1u;
    if (position_prompt_cursor(term, prefix_row, prefix_col) < 0)
        goto out;
    cursor_pending = term->rendered_cursor_pending_wrap;
    if (rendered.len > prefix.len &&
        write_prompt_suffix(rendered.data + prefix.len,
                            rendered.len - prefix.len, prefix_col,
                            cursor_pending, term->columns,
                            &cursor_pending) < 0)
        goto out;
    if (snj_write_full(STDERR_FILENO, "\033[K", 3u) < 0)
        goto out;

    term->rendered_cursor_row = end_row;
    term->rendered_cursor_col = end_col;
    term->rendered_cursor_pending_wrap = cursor_pending;
    if (old_rows > new_rows) {
        size_t obsolete = old_rows - new_rows;

        if (materialize_prompt_wrap(term) < 0)
            goto out;
        for (size_t row = 0u; row < obsolete; ++row) {
            if (move_cursor(1u, 'B') < 0 ||
                snj_write_full(STDERR_FILENO, "\r\033[2K", 5u) < 0)
                goto out;
        }
        if (move_cursor(obsolete, 'A') < 0)
            goto out;
        term->rendered_cursor_col = 0u;
    }
    term->rendered_rows = new_rows;
    term->rendered_end_at_margin = end_col == 0u && end_row != 0u;
    if (position_prompt_cursor(term, cursor_row, cursor_col) < 0)
        goto out;
    rc = 0;
out:
    snj_buf_free(&rendered);
    snj_buf_free(&prefix);
    return rc;
}

static int
insert_bytes(struct snj_term *term, const unsigned char *data, size_t len)
{
    size_t changed_at = term->cursor;

    if (len > SNJ_MAX_DIRECT_PROMPT - term->draft.len) {
        errno = EOVERFLOW;
        return -1;
    }
    if (snj_buf_reserve(&term->draft, len) < 0)
        return -1;
    memmove(term->draft.data + term->cursor + len,
            term->draft.data + term->cursor,
            term->draft.len - term->cursor);
    memcpy(term->draft.data + term->cursor, data, len);
    term->draft.len += len;
    term->cursor += len;
    history_reset_navigation(term);
    mark_input_activity(term);
    return redraw_edit(term, changed_at);
}

static int
delete_range(struct snj_term *term, size_t start, size_t end)
{
    if (start > end || end > term->draft.len) {
        errno = EINVAL;
        return -1;
    }
    memmove(term->draft.data + start, term->draft.data + end,
            term->draft.len - end);
    term->draft.len -= end - start;
    term->cursor = start;
    history_reset_navigation(term);
    mark_input_activity(term);
    return redraw_edit(term, start);
}

static bool
word_space(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static size_t
command_name_length(const struct snj_term_command *command)
{
    size_t len = 0u;

    while (command->syntax[len] &&
           !word_space((unsigned char)command->syntax[len]))
        ++len;
    return len;
}

static int
replace_command_name(struct snj_term *term, const char *name, size_t name_len,
                     size_t token_end)
{
    size_t tail_len = term->draft.len - token_end;
    size_t next_len;

    if (!snj_size_add(name_len, tail_len, &next_len) ||
        next_len > SNJ_MAX_DIRECT_PROMPT) {
        errno = EOVERFLOW;
        return -1;
    }
    if (next_len > term->draft.len &&
        snj_buf_reserve(&term->draft, next_len - term->draft.len) < 0)
        return -1;
    memmove(term->draft.data + name_len, term->draft.data + token_end,
            tail_len);
    memcpy(term->draft.data, name, name_len);
    term->draft.len = next_len;
    term->cursor = name_len;
    history_reset_navigation(term);
    mark_input_activity(term);
    return redraw(term);
}

static int
complete_command_name(struct snj_term *term, bool *handled)
{
    const char *match = NULL;
    size_t match_len = 0u;
    size_t matches = 0u;
    size_t token_end = 0u;
    size_t prefix_len = term->cursor;

    *handled = false;
    if (!term->command_count || !prefix_len || !term->draft.len ||
        term->draft.data[0] != '/' ||
        (term->draft.len > 1u && term->draft.data[1] == '/'))
        return 0;
    while (token_end < term->draft.len &&
           !word_space(term->draft.data[token_end]))
        ++token_end;
    if (prefix_len > token_end)
        return 0;
    *handled = true;
    for (size_t i = 0u; i < term->command_count; ++i) {
        const struct snj_term_command *command = &term->commands[i];
        size_t name_len;
        size_t common;

        if (!command->syntax)
            continue;
        name_len = command_name_length(command);
        if (name_len < prefix_len ||
            memcmp(command->syntax, term->draft.data, prefix_len) != 0)
            continue;
        ++matches;
        if (!match) {
            match = command->syntax;
            match_len = name_len;
            continue;
        }
        common = prefix_len;
        while (common < match_len && common < name_len &&
               match[common] == command->syntax[common])
            ++common;
        match_len = common;
    }
    if (!match || (matches > 1u && match_len == prefix_len) ||
        (token_end == match_len &&
         memcmp(term->draft.data, match, match_len) == 0))
        return 0;
    return replace_command_name(term, match, match_len, token_end);
}

static int
suspend_terminal(struct snj_term *term)
{
    if (snj_term_hide(term) < 0)
        return -1;
    if (term->bracketed_paste &&
        snj_write_full(STDERR_FILENO, "\033[?2004l", 8u) < 0)
        return -1;
    term->bracketed_paste = false;
    if (tcflush(STDIN_FILENO, TCIFLUSH) < 0 ||
        tcsetattr(STDIN_FILENO, TCSANOW, &term->saved) < 0)
        return -1;
    term->raw = false;
    if (raise(SIGSTOP) < 0)
        return -1;
    if (set_raw(term) < 0)
        return -1;
    if (term->capable && snj_write_full(STDERR_FILENO, "\033[?2004h", 8u) < 0)
        return -1;
    term->bracketed_paste = term->capable;
    update_size(term);
    return redraw(term);
}

static int
complete_action(struct snj_term *term, enum snj_term_action action,
                enum snj_term_action *out, char **text)
{
    char *copy;

    if (term->utf8_pending_len || !term->draft.len)
        return 0;
    if (term->capable) {
        if (snj_term_hide(term) < 0)
            return -1;
    } else {
        term->prompt_visible = false;
        term->line_submission_echoed = true;
    }
    if (!snj_utf8_valid(term->draft.data, term->draft.len, true)) {
        errno = EILSEQ;
        return -1;
    }
    if (snj_buf_terminate(&term->draft) < 0)
        return -1;
    copy = snj_strdup_checked((char *)term->draft.data, SNJ_MAX_DIRECT_PROMPT);
    if (!copy)
        return -1;
    snj_buf_reset(&term->draft);
    term->cursor = 0u;
    term->prompt_wanted = false;
    term->typing_active = false;
    term->prompt_clock.captured = false;
    history_reset_navigation(term);
    *text = copy;
    *out = action;
    return 1;
}

static int
feed_text_byte(struct snj_term *term, unsigned char byte)
{
    size_t expected;

    if (!term->utf8_pending_len && byte < 0x80u) {
        if (byte == 0u) {
            errno = EILSEQ;
            return -1;
        }
        return term->searching ? search_insert(term, &byte, 1u) :
                                 insert_bytes(term, &byte, 1u);
    }
    if (term->utf8_pending_len >= sizeof(term->utf8_pending)) {
        term->utf8_pending_len = 0u;
        errno = EILSEQ;
        return -1;
    }
    term->utf8_pending[term->utf8_pending_len++] = byte;
    expected = snj_utf8_size(term->utf8_pending[0]);
    if (!expected || term->utf8_pending_len > expected) {
        term->utf8_pending_len = 0u;
        errno = EILSEQ;
        return -1;
    }
    if (term->utf8_pending_len < expected)
        return 0;
    if (!snj_utf8_valid(term->utf8_pending, expected, true)) {
        term->utf8_pending_len = 0u;
        errno = EILSEQ;
        return -1;
    }
    if ((term->searching ? search_insert(term, term->utf8_pending, expected) :
                           insert_bytes(term, term->utf8_pending, expected)) < 0)
        return -1;
    term->utf8_pending_len = 0u;
    return 0;
}

struct escape_key {
    const char *bytes;
    size_t len;
    int key;
};

enum {
    KEY_UP = 1,
    KEY_DOWN,
    KEY_RIGHT,
    KEY_LEFT,
    KEY_HOME,
    KEY_END,
    KEY_DELETE,
    KEY_PASTE_BEGIN
};

static const struct escape_key keys[] = {
    {"\033[A", 3u, KEY_UP}, {"\033[B", 3u, KEY_DOWN},
    {"\033[C", 3u, KEY_RIGHT}, {"\033[D", 3u, KEY_LEFT},
    {"\033[H", 3u, KEY_HOME}, {"\033[F", 3u, KEY_END},
    {"\033[1~", 4u, KEY_HOME}, {"\033[4~", 4u, KEY_END},
    {"\033[3~", 4u, KEY_DELETE}, {"\033[200~", 6u, KEY_PASTE_BEGIN}
};

static int
apply_key(struct snj_term *term, int key)
{
    if (term->searching && search_accept(term, false) < 0)
        return -1;
    mark_input_activity(term);
    switch (key) {
    case KEY_UP:
        return history_up(term);
    case KEY_DOWN:
        return history_down(term);
    case KEY_RIGHT:
        term->cursor = next_cp(term->draft.data, term->draft.len, term->cursor);
        return move_prompt_cursor(term);
    case KEY_LEFT:
        term->cursor = previous_cp(term->draft.data, term->cursor);
        return move_prompt_cursor(term);
    case KEY_HOME:
        term->cursor = 0u;
        return move_prompt_cursor(term);
    case KEY_END:
        term->cursor = term->draft.len;
        return move_prompt_cursor(term);
    case KEY_DELETE:
        return term->cursor < term->draft.len ?
            delete_range(term, term->cursor,
                         next_cp(term->draft.data, term->draft.len,
                                 term->cursor)) : 0;
    case KEY_PASTE_BEGIN:
        term->paste = true;
        term->paste_end_match = 0u;
        return 0;
    default:
        return 0;
    }
}

static int
feed_escape(struct snj_term *term, unsigned char byte)
{
    bool prefix = false;

    if (term->escape_len >= sizeof(term->escape)) {
        term->escape_len = 0u;
        return 0;
    }
    term->escape[term->escape_len++] = byte;
    for (size_t i = 0u; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        if (term->escape_len <= keys[i].len &&
            memcmp(term->escape, keys[i].bytes, term->escape_len) == 0) {
            prefix = true;
            if (term->escape_len == keys[i].len) {
                int key = keys[i].key;
                term->escape_len = 0u;
                return apply_key(term, key);
            }
        }
    }
    if (!prefix)
        term->escape_len = 0u;
    return 0;
}

static int
feed_paste(struct snj_term *term, unsigned char byte)
{
    static const unsigned char end[] = "\033[201~";

    if (byte == end[term->paste_end_match]) {
        ++term->paste_end_match;
        if (term->paste_end_match == sizeof(end) - 1u) {
            term->paste = false;
            term->paste_end_match = 0u;
        }
        return 0;
    }
    if (term->paste_end_match) {
        size_t matched = term->paste_end_match;
        term->paste_end_match = 0u;
        for (size_t i = 0u; i < matched; ++i)
            if (feed_text_byte(term, end[i]) < 0)
                return -1;
        if (byte == end[0]) {
            term->paste_end_match = 1u;
            return 0;
        }
    }
    if (byte == '\r')
        byte = '\n';
    return feed_text_byte(term, byte);
}

static int
complete_exit(struct snj_term *term, enum snj_term_action *action)
{
    if (snj_term_hide(term) < 0)
        return -1;
    term->prompt_wanted = false;
    *action = SNJ_TERM_EXIT;
    return 1;
}

static int
cancel_line(struct snj_term *term, enum snj_term_action *action)
{
    bool interrupt = term->active && !term->searching && !term->draft.len;

    if (!term->prompt_visible && redraw(term) < 0)
        return -1;
    if (term->capable && term->prompt_visible) {
        term->cursor = term->draft.len;
        if (move_prompt_cursor(term) < 0)
            return -1;
    }
    if (snj_write_full(STDERR_FILENO, "^C\n", 3u) < 0)
        return -1;
    free(term->search_original);
    term->search_original = NULL;
    term->searching = false;
    term->search_failed = false;
    term->search_pos = SIZE_MAX;
    snj_buf_reset(&term->search_query);
    snj_buf_reset(&term->search_label);
    snj_buf_reset(&term->draft);
    term->cursor = 0u;
    term->utf8_pending_len = 0u;
    term->escape_len = 0u;
    term->paste = false;
    term->paste_end_match = 0u;
    term->typing_active = false;
    term->prompt_visible = false;
    term->rendered_rows = 0u;
    term->rendered_cursor_row = 0u;
    term->rendered_cursor_col = 0u;
    term->rendered_end_at_margin = false;
    term->rendered_cursor_pending_wrap = false;
    term->output_seen = false;
    term->output_ended_lf = true;
    history_reset_navigation(term);
    term->prompt_clock.captured = false;
    term->prompt_wanted = false;
    *action = interrupt ? SNJ_TERM_INTERRUPT : SNJ_TERM_CANCEL;
    return 1;
}

static int
feed_byte(struct snj_term *term, unsigned char byte,
          enum snj_term_action *action, char **text)
{
    if (byte == 0x03u)
        return cancel_line(term, action);
    if (term->paste)
        return feed_paste(term, byte);
    if (term->escape_len)
        return feed_escape(term, byte);
    if (byte == 0x1bu) {
        term->escape[0] = byte;
        term->escape_len = 1u;
        return 0;
    }
    if (term->searching && byte < 0x20u && byte != 0x03u &&
        byte != 0x07u && byte != 0x08u && byte != 0x12u &&
        search_accept(term, false) < 0)
        return -1;
    switch (byte) {
    case '\r':
        return complete_action(term, SNJ_TERM_SUBMIT, action, text);
    case '\n': {
        if (!term->capable)
            return complete_action(term, SNJ_TERM_SUBMIT, action, text);
        unsigned char lf = '\n';
        return insert_bytes(term, &lf, 1u);
    }
    case '\t':
        if (!term->draft.len) {
            *action = SNJ_TERM_VIEW;
            return 1;
        }
        {
            bool handled;
            int rc = complete_command_name(term, &handled);

            if (rc < 0 || handled)
                return rc;
        }
        if (term->active)
            return complete_action(term, SNJ_TERM_QUEUE, action, text);
        else {
            unsigned char spaces[4] = {' ', ' ', ' ', ' '};
            size_t count = 4u - (term->cursor % 4u);
            return insert_bytes(term, spaces, count);
        }
    case 0x04u:
        if (!term->draft.len)
            return complete_exit(term, action);
        return term->cursor < term->draft.len ?
            delete_range(term, term->cursor,
                         next_cp(term->draft.data, term->draft.len,
                                 term->cursor)) : 0;
    case 0x0cu:
        if (snj_term_hide(term) < 0)
            return -1;
        if (term->capable && snj_write_full(STDERR_FILENO,
                                            "\033[2J\033[H", 7u) < 0)
            return -1;
        return redraw(term);
    case 0x15u:
        return delete_range(term, 0u, term->draft.len);
    case 0x17u: {
        size_t start = term->cursor;
        while (start && word_space(term->draft.data[previous_cp(term->draft.data,
                                                                 start)]))
            start = previous_cp(term->draft.data, start);
        while (start && !word_space(term->draft.data[previous_cp(term->draft.data,
                                                                  start)]))
            start = previous_cp(term->draft.data, start);
        return delete_range(term, start, term->cursor);
    }
    case 0x1au:
        return suspend_terminal(term);
    case 0x08u:
    case 0x7fu:
        if (term->searching)
            return search_backspace(term);
        return term->cursor ? delete_range(term,
                    previous_cp(term->draft.data, term->cursor), term->cursor) : 0;
    case 0x07u:
        return term->searching ? search_accept(term, true) : 0;
    case 0x12u:
        return search_begin(term);
    default:
        return feed_text_byte(term, byte);
    }
}

static int
consume_resize(struct snj_term *term)
{
    bool was_capable;
    bool now_capable;

    if (!sigwinch_pending)
        return 0;
    sigwinch_pending = 0;
    if (!term->opened)
        return 0;
    was_capable = term->capable;
    update_size(term);
    now_capable = term->capable;
    if (term->prompt_visible && was_capable) {
        if (now_capable && sync_prompt_layout_after_resize(term) < 0)
            return -1;
        if (!now_capable)
            term->capable = true;
        if (snj_term_hide(term) < 0) {
            term->capable = now_capable;
            return -1;
        }
        term->capable = now_capable;
    }
    if (!term->prompt_wanted || term->output_depth)
        return 0;
    return redraw(term);
}

int
snj_term_poll(struct snj_term *term, int timeout_ms,
              enum snj_term_action *action, char **text)
{
    struct pollfd pfd;
    ssize_t count;
    int rc;

    *action = SNJ_TERM_NONE;
    *text = NULL;
    if (term->prompt_visible && term->capable && !term->searching &&
        !term->output_depth && animated_spinners(term) &&
        update_spinners(term, spinner_step(term, monotonic_ms())) < 0)
        return -1;
    if (sigint_pending) {
        --sigint_pending;
        return feed_byte(term, 0x03u, action, text);
    }
    if (consume_resize(term) < 0)
        return -1;
    if (term->input_pos == term->input_len) {
        term->input_pos = 0u;
        term->input_len = 0u;
        pfd.fd = STDIN_FILENO;
        pfd.events = POLLIN;
        pfd.revents = 0;
        if (term->searching && term->escape_len == 1u &&
            (timeout_ms < 0 || timeout_ms > 30))
            timeout_ms = 30;
        timeout_ms = spinner_timeout(term, timeout_ms);
        rc = poll(&pfd, 1u, timeout_ms);
        if (sigint_pending) {
            --sigint_pending;
            return feed_byte(term, 0x03u, action, text);
        }
        if (sigwinch_pending) {
            if (consume_resize(term) < 0)
                return -1;
            if (rc < 0 && errno == EINTR)
                return 0;
        }
        if (rc == 0 && term->searching && term->escape_len == 1u) {
            term->escape_len = 0u;
            return search_accept(term, false);
        }
        if (rc == 0 && animated_spinners(term) &&
            update_spinners(term, spinner_step(term, monotonic_ms())) < 0)
            return -1;
        if (rc <= 0)
            return rc;
        if (!(pfd.revents & POLLIN)) {
            if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) {
                *action = SNJ_TERM_EXIT;
                return 1;
            }
            return 0;
        }
        count = read(STDIN_FILENO, term->input, sizeof(term->input));
        if (sigint_pending) {
            --sigint_pending;
            return feed_byte(term, 0x03u, action, text);
        }
        if (sigwinch_pending) {
            if (consume_resize(term) < 0)
                return -1;
            if (count < 0 && errno == EINTR)
                return 0;
        }
        if (count < 0)
            return -1;
        if (count == 0) {
            *action = SNJ_TERM_EXIT;
            return 1;
        }
        term->input_len = (size_t)count;
    }
    while (term->input_pos < term->input_len) {
        rc = feed_byte(term, term->input[term->input_pos++], action, text);
        if (rc < 0)
            return -1;
        if (rc > 0)
            return 1;
    }
    term->input_pos = 0u;
    term->input_len = 0u;
    return consume_resize(term);
}

void
snj_term_close(struct snj_term *term)
{
    if (!term)
        return;
    if (term->opened) {
        (void)snj_term_hide(term);
        if (term->bracketed_paste)
            (void)snj_write_full(STDERR_FILENO, "\033[?2004l", 8u);
        if (term->raw)
            (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &term->saved);
    }
    if (term->sigwinch_installed)
        (void)sigaction(SIGWINCH, &term->saved_sigwinch, NULL);
    if (term->sigint_installed)
        (void)sigaction(SIGINT, &term->saved_sigint, NULL);
    history_clear(term);
    free(term->history_path);
    free(term->search_original);
    snj_buf_free(&term->search_label);
    snj_buf_free(&term->search_query);
    snj_buf_free(&term->draft);
    memset(term, 0, sizeof(*term));
}

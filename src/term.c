/* SPDX-License-Identifier: GPL-2.0-only */
#include "term.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <wchar.h>

static volatile sig_atomic_t sigint_pending;
static volatile sig_atomic_t sigwinch_pending;

static void
mark_sigint(int signal_number)
{
    (void)signal_number;
    sigint_pending = 1;
}

static void
mark_sigwinch(int signal_number)
{
    (void)signal_number;
    sigwinch_pending = 1;
}

static void
set_error(char *error, size_t size, const char *message)
{
    if (size)
        (void)snprintf(error, size, "%s: %s", message, strerror(errno));
}

static size_t
utf8_size(unsigned char first)
{
    if (first < 0x80u)
        return 1u;
    if (first >= 0xc2u && first <= 0xdfu)
        return 2u;
    if (first >= 0xe0u && first <= 0xefu)
        return 3u;
    if (first >= 0xf0u && first <= 0xf4u)
        return 4u;
    return 0u;
}

static size_t
decode_utf8(const unsigned char *s, size_t len, uint32_t *cp)
{
    size_t need = utf8_size(s[0]);
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

static size_t
display_width(const unsigned char *text, size_t len)
{
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

void
snj_term_init(struct snj_term *term)
{
    memset(term, 0, sizeof(*term));
    snj_buf_init(&term->draft, SNJ_MAX_DIRECT_PROMPT + 1u);
    term->columns = 80u;
    term->history_pos = SIZE_MAX;
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
        set_error(error, error_size, "terminal already open");
        return -1;
    }
    if (tcgetattr(STDIN_FILENO, &term->saved) < 0) {
        set_error(error, error_size, "cannot read terminal attributes");
        return -1;
    }
    term->capable = term_control_capable();
    update_size(term);
    if (term->capable && set_raw(term) < 0) {
        set_error(error, error_size, "cannot enter terminal input mode");
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
        set_error(error, error_size, "cannot install terminal interrupt handler");
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
        set_error(error, error_size, "cannot install terminal resize handler");
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
        set_error(error, error_size, "cannot enable bracketed paste");
        return -1;
    }
    term->bracketed_paste = term->capable;
    return 0;
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

int
snj_term_hide(struct snj_term *term)
{
    if (!term->opened || !term->prompt_visible)
        return 0;
    if (!term->capable) {
        term->prompt_visible = false;
        return snj_write_full(STDERR_FILENO, "\n", 1u);
    }
    if (term->rendered_cursor_row + 1u < term->rendered_rows &&
        move_cursor(term->rendered_rows - term->rendered_cursor_row - 1u, 'B') < 0)
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
    return 0;
}

static int
redraw(struct snj_term *term)
{
    struct snj_buf out;
    size_t cursor_row = 0u, cursor_col = 0u;
    size_t end_row = 0u, end_col = 0u;
    size_t label_len = strlen(term->label);
    size_t label_cols = display_width((const unsigned char *)term->label,
                                      label_len);
    size_t max;
    int rc = -1;

    if (!term->opened || !term->prompt_wanted || term->output_depth)
        return 0;
    if (!term->capable) {
        int fallback_rc = 0;

        if (term->prompt_visible)
            return 0;
        if (term->status[0] &&
            (snj_term_write_safe(STDERR_FILENO, term->status,
                                 strlen(term->status)) < 0 ||
             snj_write_full(STDERR_FILENO, "\n", 1u) < 0))
            fallback_rc = -1;
        if (fallback_rc == 0 &&
            snj_write_full(STDERR_FILENO, term->label,
                           strlen(term->label)) < 0)
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
    if (term->draft.len > (SIZE_MAX - 1024u) / 8u) {
        errno = EOVERFLOW;
        return -1;
    }
    max = term->draft.len * 8u + 1024u;
    snj_buf_init(&out, max);
    if (term->status[0]) {
        if (snj_buf_append(&out, term->status, strlen(term->status)) < 0 ||
            snj_buf_append(&out, "\r\n", 2u) < 0)
            goto out;
    }
    if (label_cols == SIZE_MAX ||
        snj_buf_append(&out, term->label, label_len) < 0 ||
        append_safe(&out, term->draft.data, term->draft.len, true, label_cols,
                    term->columns, term->cursor,
                    &cursor_row, &cursor_col, &end_row, &end_col) < 0 ||
        snj_buf_append(&out, "\033[K", 3u) < 0 ||
        snj_write_full(STDERR_FILENO, out.data, out.len) < 0)
        goto out;
    if (end_row > cursor_row && move_cursor(end_row - cursor_row, 'A') < 0)
        goto out;
    if (snj_write_full(STDERR_FILENO, "\r", 1u) < 0 ||
        move_cursor(cursor_col, 'C') < 0)
        goto out;
    term->rendered_rows = end_row + 1u + (term->status[0] ? 1u : 0u);
    term->rendered_cursor_row = cursor_row + (term->status[0] ? 1u : 0u);
    term->prompt_visible = true;
    rc = 0;
out:
    snj_buf_free(&out);
    return rc;
}

int
snj_term_show(struct snj_term *term)
{
    return redraw(term);
}

int
snj_term_set_prompt(struct snj_term *term, bool active)
{
    const char *label = active ? "steer › " : "› ";

    if (snj_term_hide(term) < 0)
        return -1;
    term->active = active;
    term->prompt_wanted = true;
    term->line_submission_echoed = false;
    (void)snprintf(term->label, sizeof(term->label), "%s", label);
    return redraw(term);
}

int
snj_term_output_begin(struct snj_term *term, bool persistent)
{
    if (!term || !term->opened)
        return 0;
    if (persistent)
        term->status[0] = '\0';
    if (term->output_depth == UINT_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    if (term->output_depth++ == 0u && snj_term_hide(term) < 0) {
        --term->output_depth;
        return -1;
    }
    return 0;
}

int
snj_term_output_end(struct snj_term *term)
{
    if (!term || !term->opened)
        return 0;
    if (!term->output_depth) {
        errno = EINVAL;
        return -1;
    }
    --term->output_depth;
    return term->output_depth ? 0 : redraw(term);
}

int
snj_term_set_status(struct snj_term *term, const char *status)
{
    size_t len;

    if (!term || !term->opened)
        return 0;
    len = strlen(status);
    if (len >= sizeof(term->status)) {
        errno = EOVERFLOW;
        return -1;
    }
    if (snj_term_hide(term) < 0)
        return -1;
    memcpy(term->status, status, len + 1u);
    return redraw(term);
}

void
snj_term_clear_status(struct snj_term *term)
{
    if (term)
        term->status[0] = '\0';
}

static void
history_reset_navigation(struct snj_term *term)
{
    free(term->history_draft);
    term->history_draft = NULL;
    term->history_pos = SIZE_MAX;
}

int
snj_term_history_add(struct snj_term *term, const char *text)
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

    if (term->history_pos == SIZE_MAX)
        return 0;
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
    n = utf8_size(s[pos]);
    return n && n <= len - pos ? pos + n : pos + 1u;
}

static int
insert_bytes(struct snj_term *term, const unsigned char *data, size_t len)
{
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
    return redraw(term);
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
    return redraw(term);
}

static bool
word_space(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
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
        return insert_bytes(term, &byte, 1u);
    }
    if (term->utf8_pending_len >= sizeof(term->utf8_pending)) {
        term->utf8_pending_len = 0u;
        errno = EILSEQ;
        return -1;
    }
    term->utf8_pending[term->utf8_pending_len++] = byte;
    expected = utf8_size(term->utf8_pending[0]);
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
    if (insert_bytes(term, term->utf8_pending, expected) < 0)
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
    switch (key) {
    case KEY_UP:
        return history_up(term);
    case KEY_DOWN:
        return history_down(term);
    case KEY_RIGHT:
        term->cursor = next_cp(term->draft.data, term->draft.len, term->cursor);
        return redraw(term);
    case KEY_LEFT:
        term->cursor = previous_cp(term->draft.data, term->cursor);
        return redraw(term);
    case KEY_HOME:
        term->cursor = 0u;
        return redraw(term);
    case KEY_END:
        term->cursor = term->draft.len;
        return redraw(term);
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
feed_byte(struct snj_term *term, unsigned char byte,
          enum snj_term_action *action, char **text)
{
    if (term->paste)
        return feed_paste(term, byte);
    if (term->escape_len)
        return feed_escape(term, byte);
    if (byte == 0x1bu) {
        term->escape[0] = byte;
        term->escape_len = 1u;
        return 0;
    }
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
        if (term->active)
            return complete_action(term, SNJ_TERM_QUEUE, action, text);
        else {
            unsigned char spaces[4] = {' ', ' ', ' ', ' '};
            size_t count = 4u - (term->cursor % 4u);
            return insert_bytes(term, spaces, count);
        }
    case 0x03u:
        sigint_pending = 0;
        if (term->active) {
            *action = SNJ_TERM_INTERRUPT;
            return 1;
        }
        if (term->draft.len)
            return delete_range(term, 0u, term->draft.len);
        if (snj_term_hide(term) < 0)
            return -1;
        term->prompt_wanted = false;
        *action = SNJ_TERM_EXIT;
        return 1;
    case 0x04u:
        if (!term->active && !term->draft.len) {
            if (snj_term_hide(term) < 0)
                return -1;
            term->prompt_wanted = false;
            *action = SNJ_TERM_EXIT;
            return 1;
        }
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
        return term->cursor ? delete_range(term,
                    previous_cp(term->draft.data, term->cursor), term->cursor) : 0;
    default:
        return feed_text_byte(term, byte);
    }
}

static int
consume_resize(struct snj_term *term)
{
    bool was_capable;

    if (!sigwinch_pending)
        return 0;
    sigwinch_pending = 0;
    if (!term->opened)
        return 0;
    was_capable = term->capable;
    if (term->prompt_visible && was_capable && snj_term_hide(term) < 0)
        return -1;
    update_size(term);
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
    if (sigint_pending) {
        sigint_pending = 0;
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
        do {
            rc = poll(&pfd, 1u, timeout_ms);
        } while (rc < 0 && errno == EINTR && !sigint_pending &&
                 !sigwinch_pending);
        if (sigint_pending) {
            sigint_pending = 0;
            return feed_byte(term, 0x03u, action, text);
        }
        if (sigwinch_pending) {
            if (consume_resize(term) < 0)
                return -1;
            if (rc < 0 && errno == EINTR)
                return 0;
        }
        if (rc <= 0)
            return rc;
        if (!(pfd.revents & POLLIN)) {
            if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) {
                *action = SNJ_TERM_EXIT;
                return 1;
            }
            return 0;
        }
        do {
            count = read(STDIN_FILENO, term->input, sizeof(term->input));
        } while (count < 0 && errno == EINTR && !sigint_pending &&
                 !sigwinch_pending);
        if (sigint_pending) {
            sigint_pending = 0;
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
    for (size_t i = 0u; i < term->history_count; ++i)
        free(term->history[i]);
    free(term->history_draft);
    snj_buf_free(&term->draft);
    memset(term, 0, sizeof(*term));
}

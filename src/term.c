/* SPDX-License-Identifier: GPL-2.0-only */
#include "term.h"
#include "fs.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static atomic_uint sigint_pending;
static volatile sig_atomic_t sigwinch_pending;
/* Only the existing terminal owner uses these privately reopened descriptions. */
static _Thread_local struct snag_term *output_owner;
static int redraw(struct snag_term *term);
static size_t previous_cp(const unsigned char *s, size_t pos);
static int compose_frame(struct snag_term *term, struct snag_buf *out, size_t *label_bytes,
                         size_t *cursor_row, size_t *cursor_col,
                         size_t *end_row, size_t *end_col);

static void
mark_sigint(int signal_number)
{
    (void)signal_number;
    (void)atomic_fetch_add_explicit(&sigint_pending, 1u, memory_order_relaxed);
}

static void
mark_sigwinch(int signal_number)
{
    (void)signal_number;
    sigwinch_pending = 1;
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
append_escape(struct snag_buf *out, uint32_t cp)
{
    if (cp <= 0xffu)
        return snag_buf_printf(out, "\\x%02X", (unsigned int)cp);
    return snag_buf_printf(out, "\\u{%X}", (unsigned int)cp);
}

static int
append_safe(struct snag_buf *out, const unsigned char *text, size_t len,
            bool prompt, size_t indent, unsigned int columns,
            size_t stop, size_t *cursor_byte)
{
    size_t col = indent;
    size_t i = 0u;

    if (columns >= 20u)
        col %= columns;
    if (stop == 0u && cursor_byte)
        *cursor_byte = out->len;
    while (i < len) {
        uint32_t cp;
        size_t n = snag_utf8_decode(text + i, len - i, &cp);
        int width = 0;
        size_t before = out->len;
        bool invalid = !n;

        if (!n) {
            cp = text[i];
            n = 1u;
        }
        if (cp == '\n') {
            if (prompt) {
                if (snag_buf_append(out, "\r\n", 2u) < 0)
                    return -1;
                for (size_t j = 0u; j < indent; ++j)
                    if (snag_buf_putc(out, ' ') < 0)
                        return -1;
                col = indent;
                if (columns >= 20u)
                    col %= columns;
            } else if (snag_buf_putc(out, '\n') < 0) {
                return -1;
            }
        } else if (cp == '\t') {
            size_t spaces = 4u - (col % 4u);
            for (size_t j = 0u; j < spaces; ++j)
                if (snag_buf_putc(out, ' ') < 0)
                    return -1;
            width = (int)spaces;
        } else if (invalid || cp < 0x20u || cp == 0x7fu ||
                   (cp >= 0x80u && cp <= 0x9fu) || format_unsafe(cp)) {
            if (append_escape(out, cp) < 0)
                return -1;
            width = (int)(out->len - before);
        } else {
            int w = snag_char_width(cp);
            if (w < 0) {
                if (append_escape(out, cp) < 0)
                    return -1;
                width = (int)(out->len - before);
            } else {
                if (snag_buf_append(out, text + i, n) < 0)
                    return -1;
                width = w;
            }
        }
        if (cp != '\n' || !prompt) {
            if (columns >= 20u && width > 0) {
                size_t w = (size_t)width;
                if (w == 2u && out->len - before == n && col + w > columns)
                    col = 0u;
                if (col > SIZE_MAX - w)
                    return -1;
                col += w;
                col %= columns;
            } else if (width > 0) {
                col += (size_t)width;
            }
        }
        i += n;
        if (i == stop && cursor_byte)
            *cursor_byte = out->len;
    }
    return 0;
}

size_t
snag_term_text_width(const char *value, size_t len)
{
    const unsigned char *text = (const unsigned char *)value;
    size_t width = 0u;

    for (size_t i = 0u; i < len;) {
        uint32_t cp;
        size_t n = snag_utf8_decode(text + i, len - i, &cp);
        size_t amount;
        int w;

        if (!n) {
            cp = text[i];
            n = 1u;
        }
        w = snag_char_width(cp);
        if (cp == '\t')
            amount = 4u - (width % 4u);
        else if (cp < 0x20u || cp == 0x7fu ||
                 (cp >= 0x80u && cp <= 0x9fu) || format_unsafe(cp) || w < 0) {
            char escaped[16];
            int count = snprintf(escaped, sizeof(escaped),
                                 cp <= 0xffu ? "\\x%02X" : "\\u{%X}",
                                 (unsigned int)cp);
            if (count < 0)
                return SIZE_MAX;
            amount = (size_t)count;
        } else {
            amount = (size_t)w;
        }
        if (!snag_size_add(width, amount, &width)) {
            errno = EOVERFLOW;
            return SIZE_MAX;
        }
        i += n;
    }
    return width;
}

int
snag_term_write(int fd, const void *text, size_t len)
{
    const unsigned char *bytes = text;
    struct snag_term *term = output_owner;
    int target = term && fd >= STDOUT_FILENO && fd <= STDERR_FILENO ?
                 term->output_fd[fd - STDOUT_FILENO] : -1;
    struct pollfd pfd[2] = {{target >= 0 ? target : fd, POLLOUT, 0}, {-1, POLLIN, 0}};

    /* Input-only checkpoints capture intent, never recursively paint output. */
    if (term && term->input_only)
        return 0;

    if (fd < 0) {
        errno = EBADF;
        return -1;
    }
    while (len) {
        size_t amount = len < 1024u ? len : 1024u;
        pfd[1].fd = term && term->raw ? STDIN_FILENO : -1;
        int rc = poll(pfd, 2u, 0);
        if (rc >= 0 && !(pfd[0].revents & POLLOUT) && term && term->input_checkpoint) {
            term->input_only = true;
            rc = term->input_checkpoint(term->input_opaque);
            term->input_only = false;
            if (rc < 0)
                return -1;
        }
        if (rc >= 0 && !(pfd[0].revents & POLLOUT))
            rc = poll(pfd, 2u, term ? 16 : -1);
        if (rc < 0 && errno == EINTR)
            continue;
        if (rc < 0)
            return -1;
        if (pfd[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            errno = EIO;
            return -1;
        }
        if (!(pfd[0].revents & POLLOUT))
            continue;
        ssize_t written = write(pfd[0].fd, bytes, amount);
        if (written < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (written <= 0)
            return -1;
        bytes += written;
        len -= (size_t)written;
    }
    return 0;
}

int
snag_term_write_safe(int fd, const char *text, size_t len)
{
    struct snag_buf out;
    size_t max;
    int rc;

    if (len > (SIZE_MAX - 32u) / 8u) {
        errno = EOVERFLOW;
        return -1;
    }
    max = len * 8u + 32u;
    snag_buf_init(&out, max);
    rc = append_safe(&out, (const unsigned char *)text, len, false, 0u, 0u,
                     len + 1u, NULL);
    if (rc == 0)
        rc = snag_term_write(fd, out.data, out.len);
    snag_buf_free(&out);
    return rc;
}

int
snag_term_append_safe(struct snag_buf *out, const char *text, size_t len)
{
    return append_safe(out, (const unsigned char *)text, len, false, 0u, 0u,
                       len + 1u, NULL);
}

void
snag_term_init(struct snag_term *term)
{
    memset(term, 0, sizeof(*term));
    term->output_fd[0] = term->output_fd[1] = -1;
    snag_buf_init(&term->draft, SNAG_MAX_DIRECT_PROMPT + 1u);
    snag_buf_init(&term->search_label, SNAG_MAX_DIRECT_PROMPT + 64u);
    snag_buf_init(&term->search_query, SNAG_MAX_DIRECT_PROMPT + 1u);
    snag_buf_init(&term->output_cell, SIZE_MAX);
    snag_buf_init(&term->output_line, SIZE_MAX);
    snag_buf_init(&term->painted_prompt, SIZE_MAX);
    snag_buf_init(&term->completion_output, SNAG_MAX_DIRECT_PROMPT);
    term->columns = 80u;
    term->history_pos = SIZE_MAX;
    term->search_pos = SIZE_MAX;
}

void
snag_term_set_typing_pause(struct snag_term *term, uint32_t pause_ms)
{
    if (term)
        term->typing_pause_ms = pause_ms;
}

int
snag_term_set_destinations(struct snag_term *term,
                          const struct snag_irc_destinations *destinations)
{
    if (!destinations || destinations->count > SNAG_IRC_DESTINATIONS_MAX) {
        errno = EINVAL;
        return -1;
    }
    if (!term->destinations) {
        term->destinations = malloc(sizeof(*destinations));
        if (!term->destinations)
            return -1;
    }
    *term->destinations = *destinations;
    if (!term->destination.id && !term->draft.len && destinations->count)
        term->destination = destinations->items[0].target;
    return redraw(term);
}

int
snag_term_select_destination(struct snag_term *term, uint32_t id)
{
    for (size_t i = 0u; term->destinations && i < term->destinations->count; ++i)
        if (term->destinations->items[i].target.id == id) {
            term->destination = term->destinations->items[i].target;
            return redraw(term);
        }
    errno = ENOENT;
    return -1;
}

void
snag_term_destination_prefix(const struct snag_term *term, char *out, size_t size)
{
    const struct snag_irc_destination *selected = NULL;

    if (!size)
        return;
    out[0] = '\0';
    if (!term->chat || !term->destinations)
        return;
    for (size_t i = 0u; i < term->destinations->count; ++i)
        if (term->destinations->items[i].target.id == term->destination.id &&
            term->destinations->items[i].target.revision == term->destination.revision)
            selected = &term->destinations->items[i];
    if (!selected && term->destination.id)
        (void)snprintf(out, size, "[%u unavailable] ", term->destination.id);
    else if (!selected && term->destinations->count)
        (void)snprintf(out, size, "[choose destination] ");
    else if (selected && (!selected->joined || term->destinations->count > 1u))
        (void)snprintf(out, size, "[%u %s] ", selected->target.id,
            selected->joined ? selected->room : "connecting");
}

void
snag_term_destination_route(const struct snag_term *term, const char *text,
                           struct snag_irc_route *route)
{
    uint32_t id;
    size_t body;
    enum snag_irc_target_command command = snag_irc_target_parse(
        text, text ? strlen(text) : 0u, &id, &body);

    memset(route, 0, sizeof(*route));
    if (command == SNAG_IRC_TARGET_INVALID)
        return;
    if (command == SNAG_IRC_TARGET_NONE) {
        if (term->destination.id)
            route->targets[route->count++] = term->destination;
        return;
    }
    for (size_t i = 0u; term->destinations && i < term->destinations->count; ++i) {
        const struct snag_irc_target *target = &term->destinations->items[i].target;
        if (command == SNAG_IRC_TARGET_ALL || target->id == id)
            route->targets[route->count++] = *target;
    }
}

void
snag_term_set_color(struct snag_term *term, bool enabled)
{
    if (!term)
        return;
    term->color = enabled;
}

uint32_t
snag_term_typing_pause_remaining(const struct snag_term *term, uint64_t now_ms)
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
snag_term_typing_active(const struct snag_term *term)
{
    return term && term->active && term->typing_active;
}

static void
output_column_add(struct snag_term *term, size_t width)
{
    if (width && term->output_columns + width > term->columns)
        term->output_columns = 0u;
    term->output_columns += width;
}

int
snag_term_note_output(struct snag_term *term, const char *text, size_t len,
                     const char *style)
{
    struct snag_buf safe;
    int rc = -1;

    if (!term || !len)
        return 0;
    term->output_seen = true;
    term->last_output_ms = snag_monotonic_ms();
    term->output_ended_lf = text[len - 1u] == '\n';
    if (!term->opened || !term->capable)
        return 0;
    snag_buf_init(&safe, len * 8u + 32u);
    if (snag_term_append_safe(&safe, text, len) < 0)
        goto out;
    for (size_t i = 0u; i < safe.len;) {
        uint32_t cp;
        size_t n = snag_utf8_decode(safe.data + i, safe.len - i, &cp);
        int width = cp == '\n' ? 0 : snag_char_width(cp);

        if (cp == '\n') {
            term->output_columns = 0u;
            snag_buf_reset(&term->output_cell);
            snag_buf_reset(&term->output_line);
        } else {
            if (width > 0) {
                output_column_add(term, (size_t)width);
                term->output_cell_width = (size_t)width;
                snag_buf_reset(&term->output_cell);
                (void)snag_strcpy(term->output_cell_style,
                                 sizeof(term->output_cell_style), style);
            }
            if (snag_buf_append(&term->output_cell, safe.data + i, n) < 0 ||
                snag_buf_append(&term->output_line, safe.data + i, n) < 0)
                goto out;
        }
        i += n;
    }
    rc = 0;
out:
    snag_buf_free(&safe);
    return rc;
}

unsigned int
snag_term_columns(const struct snag_term *term)
{
    return term ? term->columns : 80u;
}

void
snag_term_set_commands(struct snag_term *term,
                      const struct snag_term_command *commands, size_t count)
{
    if (!term)
        return;
    term->commands = commands;
    term->command_count = commands ? count : 0u;
}

static bool
term_control_capable(void)
{
    return snag_term_host_capable();
}

static void
update_size(struct snag_term *term)
{
    if (!term_control_capable()) {
        term->columns = 80u;
        term->capable = false;
        return;
    }
    unsigned int columns = snag_term_host_columns();
    if (columns != 0u) {
        if (columns >= 20u) {
            term->columns = columns;
            if (term->raw)
                term->capable = true;
        } else {
            term->columns = columns;
            term->capable = false;
        }
    } else {
        term->columns = 80u;
    }
}

static int
set_raw(struct snag_term *term)
{
    if (snag_term_input_raw(&term->host) < 0)
        return -1;
    term->raw = true;
    return 0;
}

int
snag_term_open(struct snag_term *term, char *error, size_t error_size)
{
    struct sigaction action;

    if (term->opened) {
        errno = EALREADY;
        snag_errorf(error, error_size, "terminal already open: %s", strerror(errno));
        return -1;
    }
    if (snag_term_input_capture(&term->host) < 0) {
        snag_errorf(error, error_size, "cannot read terminal attributes: %s", strerror(errno));
        return -1;
    }
    term->capable = term_control_capable();
    update_size(term);
    if (term->capable && set_raw(term) < 0) {
        snag_errorf(error, error_size, "cannot enter terminal input mode: %s", strerror(errno));
        return -1;
    }
    memset(&action, 0, sizeof(action));
    action.sa_handler = mark_sigint;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, &term->host.sigint) < 0) {
        int saved_errno = errno;
        if (term->raw)
            (void)snag_term_input_restore(&term->host, true);
        term->raw = false;
        errno = saved_errno;
        snag_errorf(error, error_size, "cannot install terminal interrupt handler: %s", strerror(errno));
        return -1;
    }
    term->sigint_installed = true;
    memset(&action, 0, sizeof(action));
    action.sa_handler = mark_sigwinch;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGWINCH, &action, &term->host.sigwinch) < 0) {
        int saved_errno = errno;
        if (term->raw)
            (void)snag_term_input_restore(&term->host, true);
        (void)sigaction(SIGINT, &term->host.sigint, NULL);
        term->raw = false;
        term->sigint_installed = false;
        errno = saved_errno;
        snag_errorf(error, error_size, "cannot install terminal resize handler: %s", strerror(errno));
        return -1;
    }
    term->sigwinch_installed = true;
    sigint_pending = 0;
    sigwinch_pending = 0;
    term->opened = true;
    for (int fd = STDOUT_FILENO; fd <= STDERR_FILENO; ++fd) {
        char path[SNAG_PATH_MAX_BYTES];
        snag_file_info original, owned;
        if (!isatty(fd))
            continue;
        int name_error = ttyname_r(fd, path, sizeof(path));
        int copy = name_error ? -1 : open(path, O_WRONLY | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
        if (name_error)
            errno = name_error;
        if (copy < 0 || snag_fstat(fd, &original) < 0 || snag_fstat(copy, &owned) < 0 ||
            original.st_rdev != owned.st_rdev || !S_ISCHR(owned.st_mode)) {
            int saved_errno = copy < 0 ? errno : EIO;
            if (copy >= 0)
                close(copy);
            snag_term_close(term);
            errno = saved_errno;
            snag_errorf(error, error_size, "cannot open private terminal output: %s", strerror(errno));
            return -1;
        }
        term->output_fd[fd - STDOUT_FILENO] = copy;
    }
    output_owner = term;
    if (term->capable && snag_term_write(STDERR_FILENO, "\033[?2004h", 8u) < 0) {
        int saved_errno = errno;
        (void)snag_term_input_restore(&term->host, true);
        (void)sigaction(SIGINT, &term->host.sigint, NULL);
        (void)sigaction(SIGWINCH, &term->host.sigwinch, NULL);
        term->opened = false;
        term->raw = false;
        term->sigint_installed = false;
        term->sigwinch_installed = false;
        errno = saved_errno;
        snag_errorf(error, error_size, "cannot enable bracketed paste: %s", strerror(errno));
        return -1;
    }
    term->bracketed_paste = term->capable;
    return 0;
}

int
snag_term_external_begin(struct snag_term *term,
                        char *error, size_t error_size)
{
    if (!term || !term->opened) {
        errno = EINVAL;
        snag_errorf(error, error_size, "terminal is not open: %s",
                   strerror(errno));
        return -1;
    }
    if (snag_term_hide(term) < 0)
        goto fail;
    if (term->bracketed_paste &&
        snag_term_write(STDERR_FILENO, "\033[?2004l", 8u) < 0)
        goto fail;
    term->bracketed_paste = false;
    if (term->raw && snag_term_input_restore(&term->host, true) < 0)
        goto fail;
    term->raw = false;
    return 0;
fail:
    snag_errorf(error, error_size, "cannot release terminal for editor: %s",
               strerror(errno));
    return -1;
}

int
snag_term_external_end(struct snag_term *term,
                      char *error, size_t error_size)
{
    if (!term || !term->opened) {
        errno = EINVAL;
        snag_errorf(error, error_size, "terminal is not open: %s",
                   strerror(errno));
        return -1;
    }
    sigint_pending = 0;
    sigwinch_pending = 0;
    update_size(term);
    if (term->capable && set_raw(term) < 0)
        goto fail;
    if (term->capable &&
        snag_term_write(STDERR_FILENO, "\033[?2004h", 8u) < 0)
        goto fail;
    term->bracketed_paste = term->capable;
    return 0;
fail:
    snag_errorf(error, error_size, "cannot restore terminal after editor: %s",
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
    return snag_term_write(STDERR_FILENO, sequence, (size_t)n);
}

static int
materialize_prompt_wrap(struct snag_term *term)
{
    if (!term->rendered_cursor_pending_wrap)
        return 0;
    if (snag_term_write(STDERR_FILENO, " \b", 2u) < 0)
        return -1;
    term->rendered_cursor_pending_wrap = false;
    return 0;
}

int
snag_term_hide(struct snag_term *term)
{
    struct snag_buf out;
    size_t max;
    int rc = -1;

    if (term->input_only || !term->opened || !term->prompt_visible)
        return 0;
    snag_buf_reset(&term->painted_prompt);
    if (!term->capable) {
        term->prompt_visible = false;
        return snag_term_write(STDERR_FILENO, "\n", 1u);
    }
    if (term->rendered_rows > (SIZE_MAX - 256u) / 16u ||
        !snag_size_add(term->rendered_rows * 16u + 256u, term->output_cell.len, &max)) {
        errno = EOVERFLOW;
        return -1;
    }
    snag_buf_init(&out, max);
    if ((term->rendered_cursor_pending_wrap && snag_buf_append(&out, " \b", 2u) < 0) ||
        (term->rendered_cursor_row + 1u < term->rendered_rows &&
         snag_buf_printf(&out, "\033[%zuB",
            term->rendered_rows - term->rendered_cursor_row - 1u) < 0))
        goto out;
    for (size_t row = term->rendered_rows; row != 0u; --row)
        if (snag_buf_append(&out, "\r\033[2K", 5u) < 0 ||
            (row > 1u && snag_buf_append(&out, "\033[1A", 4u) < 0))
            goto out;
    if (snag_buf_putc(&out, '\r') < 0)
        goto out;
    if (term->output_detour) {
        size_t column = term->output_columns < term->columns ?
                        term->output_columns : term->columns - term->output_cell_width;
        if (snag_buf_append(&out, "\033[1A", 4u) < 0 ||
            (column && snag_buf_printf(&out, "\033[%zuC", column) < 0))
            goto out;
        /* Restore VT pending-wrap by repainting the final output cell. */
        if (term->output_columns >= term->columns &&
            (snag_buf_append(&out, term->output_cell_style,
                            strlen(term->output_cell_style)) < 0 ||
             snag_buf_append(&out, term->output_cell.data, term->output_cell.len) < 0 ||
             snag_buf_append(&out, "\033[0m", 4u) < 0))
            goto out;
    }
    if (snag_term_write(STDERR_FILENO, out.data, out.len) < 0)
        goto out;
    term->prompt_visible = false;
    term->rendered_rows = 0u;
    term->rendered_cursor_row = 0u;
    term->rendered_cursor_col = 0u;
    term->rendered_end_at_margin = false;
    term->rendered_cursor_pending_wrap = false;
    term->output_detour = false;
    rc = 0;
out:
    snag_buf_free(&out);
    return rc;
}

static int
leave_prompt(struct snag_term *term)
{
    if (!term->opened || !term->prompt_visible)
        return 0;
    snag_buf_reset(&term->painted_prompt);
    if (term->capable &&
        (materialize_prompt_wrap(term) < 0 ||
         (term->rendered_cursor_row + 1u < term->rendered_rows &&
          move_cursor(term->rendered_rows - term->rendered_cursor_row - 1u,
                      'B') < 0)))
        return -1;
    if (term->capable && term->rendered_end_at_margin) {
        if (snag_term_write(STDERR_FILENO, "\r", 1u) < 0)
            return -1;
    } else if (snag_term_write(STDERR_FILENO,
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
    term->output_detour = false;
    term->output_columns = 0u;
    snag_buf_reset(&term->output_line);
    return 0;
}

static void
mark_input_activity(struct snag_term *term)
{
    if (!term->active)
        return;
    term->typing_active = true;
    if (term->output_detour)
        term->output_seen = false;
    term->output_detour = false;
    term->last_input_ms = snag_monotonic_ms();
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
prompt_label(struct snag_term *term, size_t *len)
{
    if (term->searching) {
        *len = term->search_label.len;
        return (const char *)term->search_label.data;
    }
    snag_term_destination_prefix(term, term->destination_label, 128u);
    size_t prefix = strlen(term->destination_label);
    memcpy(term->destination_label + prefix, term->label, strlen(term->label) + 1u);
    *len = strlen(term->destination_label);
    return term->destination_label;
}

void
snag_term_capture_prompt_clock(struct snag_term *term, time_t seconds)
{
    struct tm local;
    struct snag_prompt_clock *clock = &term->prompt_clock;

    if (clock->captured)
        return;
    clock->captured = true;
    clock->valid = seconds != (time_t)-1 && snag_localtime(&seconds, &local) &&
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
prepare_spinner(struct snag_term_spinner *spinner, const char *value)
{
    size_t pos = value[0] == '\\' && value[1] == '0' ? 2u :
                 snag_utf8_size((unsigned char)value[0]);

    memset(spinner, 0, sizeof(*spinner));
    if (!snag_strcpy(spinner->value, sizeof(spinner->value), value))
        return -1;
    spinner->inactive_len = value[0] == '\\' && value[1] == '0' ? 0u :
                            (unsigned char)pos;
    while (value[pos]) {
        size_t n = snag_utf8_size((unsigned char)value[pos]);

        if (!n || spinner->frame_count >= 16u)
            return -1;
        spinner->frame_offset[spinner->frame_count] = pos;
        spinner->frame_len[spinner->frame_count++] = (unsigned char)n;
        pos += n;
    }
    return 0;
}

static int
compose_prompt(const char *prompt, const struct snag_term_spinner *spinners,
               unsigned int states, uint64_t step, char *label)
{
    size_t used = 0u;
    unsigned int seen = 0u;

    for (const unsigned char *p = (const unsigned char *)prompt; *p; ++p) {
        if (*p >= SNAG_TERM_SPINNER_MARKER_BASE) {
            unsigned int id = *p - SNAG_TERM_SPINNER_MARKER_BASE;
            const struct snag_term_spinner *cell;
            size_t offset = 0u, len;

            if (id >= SNAG_TERM_SPINNER_SLOTS)
                return -1;
            if (id == SNAG_TERM_SPINNER_PROVIDER &&
                (states & (1u << SNAG_TERM_SPINNER_TOOL)))
                id = SNAG_TERM_SPINNER_TOOL;
            cell = &spinners[id];
            len = cell->inactive_len;

            if (seen & (1u << id))
                return -1;
            seen |= 1u << id;
            if ((states & (1u << id)) && cell->frame_count) {
                unsigned int frame = (unsigned int)(step % cell->frame_count);
                offset = cell->frame_offset[frame];
                len = cell->frame_len[frame];
            }
            if (used > SNAG_TERM_LABEL_BYTES - 1u - len)
                return -1;
            memcpy(label + used, cell->value + offset, len);
            used += len;
        } else {
            if (used == SNAG_TERM_LABEL_BYTES - 1u)
                return -1;
            label[used++] = (char)*p;
        }
    }
    label[used] = '\0';
    return used ? 0 : -1;
}

static int
prompt_fits(const char *prompt,
            const struct snag_term_spinner spinners[SNAG_TERM_SPINNER_COUNT])
{
    size_t used = 0u;
    unsigned int seen = 0u;

    for (const unsigned char *p = (const unsigned char *)prompt; *p; ++p) {
        size_t len = 1u;

        if (*p >= SNAG_TERM_SPINNER_MARKER_BASE) {
            unsigned int id = *p - SNAG_TERM_SPINNER_MARKER_BASE;

            if (id >= SNAG_TERM_SPINNER_SLOTS || (seen & (1u << id)))
                return -1;
            seen |= 1u << id;
            len = 0u;
            for (unsigned int source = id; source <=
                    (id == SNAG_TERM_SPINNER_PROVIDER ? SNAG_TERM_SPINNER_TOOL : id);
                    ++source) {
                const struct snag_term_spinner *cell = &spinners[source];

                if (cell->inactive_len > len)
                    len = cell->inactive_len;
                for (size_t i = 0u; i < cell->frame_count; ++i)
                    if (cell->frame_len[i] > len)
                        len = cell->frame_len[i];
            }
        }
        if (used > SNAG_TERM_LABEL_BYTES - 1u - len)
            return -1;
        used += len;
    }
    return used ? 0 : -1;
}

static int
update_spinners(struct snag_term *term, uint64_t step)
{
    char label[SNAG_TERM_LABEL_BYTES];
    bool changed;

    if (term->input_only)
        return 0;
    if (compose_prompt(term->prompt_template, term->spinner,
                       term->spinner_states, step, label) < 0)
        return -1;
    changed = strcmp(label, term->label) != 0;
    memcpy(term->label, label, strlen(label) + 1u);
    return changed && term->prompt_visible && term->capable &&
           !term->searching && !term->output_depth ? redraw(term) : 0;
}

static bool
animated_spinners(const struct snag_term *term)
{
    for (unsigned int slot = 0u; slot < SNAG_TERM_SPINNER_SLOTS; ++slot) {
        unsigned int id = slot == SNAG_TERM_SPINNER_PROVIDER &&
            (term->spinner_states & (1u << SNAG_TERM_SPINNER_TOOL)) ?
            SNAG_TERM_SPINNER_TOOL : slot;
        if (strchr(term->prompt_template, SNAG_TERM_SPINNER_MARKER_BASE + slot) &&
            term->spinner[id].frame_count > 1u && (term->spinner_states & (1u << id)))
            return true;
    }
    return false;
}

static uint64_t
spinner_step(const struct snag_term *term, uint64_t now)
{
    uint64_t elapsed = now >= term->spinner_epoch_ms ?
                       now - term->spinner_epoch_ms : 0u;
    return elapsed * term->spinner_per_second / 1000u;
}

static int
spinner_timeout(struct snag_term *term, int timeout_ms)
{
    uint64_t now, elapsed, step, boundary, wait;

    if (!term->prompt_visible || !term->capable || term->searching ||
        term->output_depth || !animated_spinners(term))
        return timeout_ms;
    now = snag_monotonic_ms();
    elapsed = now >= term->spinner_epoch_ms ? now - term->spinner_epoch_ms : 0u;
    step = spinner_step(term, now);
    boundary = ((step + 1u) * 1000u + term->spinner_per_second - 1u) /
               term->spinner_per_second;
    wait = boundary > elapsed ? boundary - elapsed : 1u;
    return timeout_ms < 0 || wait < (uint64_t)timeout_ms ? (int)wait : timeout_ms;
}

static int
sync_prompt_layout_after_resize(struct snag_term *term)
{
    struct snag_buf scratch;
    size_t cursor_row = 0u, cursor_col = 0u;
    size_t end_row = 0u, end_col = 0u;
    size_t label_len;
    int rc = -1;

    if (compose_frame(term, &scratch, &label_len, &cursor_row, &cursor_col,
                       &end_row, &end_col) < 0)
        goto out;
    /*
     * tmux and VT terminals represent an exact-margin cursor as the pending
     * wrap position on the preceding row after reflow.  The renderer tracks
     * that position as column zero on the next row.  Materialize the pending
     * wrap before erase/movement commands use the recomputed row counts.
     */
    if (cursor_row != 0u && cursor_col == 0u &&
        snag_term_write(STDERR_FILENO, " \b", 2u) < 0)
        goto out;
    term->rendered_rows = end_row + 1u;
    term->rendered_cursor_row = cursor_row;
    term->rendered_cursor_col = cursor_col;
    term->rendered_end_at_margin = end_row != 0u && end_col == 0u;
    term->rendered_cursor_pending_wrap = false;
    rc = 0;
out:
    snag_buf_free(&scratch);
    return rc;
}

/* Views into sanitized prompt bytes, not a terminal-sized cell grid. */
struct prompt_row {
    size_t start, end, next, width;
    bool soft;
};

static struct prompt_row
prompt_row(const struct snag_buf *frame, size_t start, unsigned int columns)
{
    struct prompt_row row = {.start = start, .end = start};

    while (row.end < frame->len) {
        uint32_t cp;
        size_t n = snag_utf8_decode(frame->data + row.end, frame->len - row.end, &cp);
        int width = snag_char_width(cp);

        if (cp == '\r') {
            row.next = row.end + 2u; /* append_safe emits CRLF. */
            return row;
        }
        if (width > 0 && row.width + (size_t)width > columns) {
            row.next = row.end;
            row.soft = true;
            return row;
        }
        row.end += n;
        if (width > 0)
            row.width += (size_t)width;
    }
    row.soft = row.width == columns;
    row.next = row.end + (row.soft ? 0u : 1u);
    return row;
}

static void
frame_position(const struct snag_buf *frame, size_t offset, unsigned int columns,
                size_t *row, size_t *col)
{
    size_t start = 0u;
    *row = 0u;
    for (;;) {
        struct prompt_row line = prompt_row(frame, start, columns);
        if (offset <= line.end) {
            *col = snag_term_text_width((char *)frame->data + start, offset - start);
            if (*col == columns) {
                ++*row;
                *col = 0u;
            }
            return;
        }
        start = line.next;
        ++*row;
    }
}

static int
compose_frame(struct snag_term *term, struct snag_buf *out, size_t *label_bytes,
               size_t *cursor_row, size_t *cursor_col, size_t *end_row, size_t *end_col)
{
    size_t label_len, cursor_byte = 0u, max;
    const char *label = prompt_label(term, &label_len);
    size_t indent;

    snag_buf_init(out, 0u);
    if (label_len > (SIZE_MAX - 32u) / 4u) {
        errno = EOVERFLOW;
        return -1;
    }
    out->max = label_len * 4u + 32u;
    /* Search labels can contain a multiline draft; keep labels on their
     * logical line instead of letting a bare LF desynchronize row layout. */
    for (size_t start = 0u, pos = 0u; pos <= label_len; ++pos) {
        if (pos != label_len && label[pos] != '\n')
            continue;
        if (snag_term_append_safe(out, label + start, pos - start) < 0 ||
            (pos < label_len && snag_buf_append(out, "\\n", 2u) < 0))
            return -1;
        start = pos + 1u;
    }
    *label_bytes = out->len;
    frame_position(out, out->len, term->columns, end_row, end_col);
    indent = *end_row * term->columns + *end_col;
    if (prompt_render_max(term->draft.data, term->draft.len, indent,
                          out->len + 64u, &max) < 0)
        return -1;
    out->max = max;
    if (append_safe(out, term->draft.data, term->draft.len, true, indent,
                    term->columns, term->cursor, &cursor_byte) < 0)
        return -1;
    frame_position(out, cursor_byte, term->columns, cursor_row, cursor_col);
    frame_position(out, out->len, term->columns, end_row, end_col);
    return 0;
}

/* Keep a base character and its combining marks in the same paint span. */
static size_t
prompt_cell_end(const unsigned char *data, size_t start, size_t end)
{
    uint32_t cp;
    size_t pos = start + snag_utf8_decode(data + start, end - start, &cp);

    while (pos < end) {
        size_t n = snag_utf8_decode(data + pos, end - pos, &cp);
        if (snag_char_width(cp) != 0)
            break;
        pos += n;
    }
    return pos;
}

static size_t
prompt_cell_start(const unsigned char *data, size_t start, size_t end)
{
    uint32_t cp;
    size_t pos = previous_cp(data, end);

    while (pos > start) {
        (void)snag_utf8_decode(data + pos, end - pos, &cp);
        if (snag_char_width(cp) != 0)
            break;
        pos = previous_cp(data, pos);
    }
    return pos;
}

static size_t
colored_prefix(size_t label, size_t start, size_t end)
{
    return label <= start ? 0u : label < end ? label - start : end - start;
}

static bool
same_prompt_cell(const struct snag_term *term, const struct snag_buf *next,
                 size_t label, size_t a, size_t ae, size_t b, size_t be)
{
    size_t old_color = colored_prefix(term->painted_label_len, a, ae);
    size_t new_color = colored_prefix(label, b, be);

    if (!term->painted_color)
        old_color = 0u;
    if (!term->color)
        new_color = 0u;
    return ae - a == be - b && old_color == new_color &&
           memcmp(term->painted_prompt.data + a, next->data + b, be - b) == 0;
}

static int
prompt_move(struct snag_buf *out, size_t *row, size_t *col,
            size_t target_row, size_t target_col)
{
    if (target_row != *row &&
        snag_buf_printf(out, "\033[%zu%c", target_row > *row ? target_row - *row :
                       *row - target_row, target_row > *row ? 'B' : 'A') < 0)
        return -1;
    if (target_col != *col &&
        (snag_buf_putc(out, '\r') < 0 ||
         (target_col && snag_buf_printf(out, "\033[%zuC", target_col) < 0)))
        return -1;
    *row = target_row;
    *col = target_col;
    return 0;
}

static int
prompt_span(struct snag_buf *out, const struct snag_term *term,
            const struct snag_buf *frame, size_t label, size_t start, size_t end)
{
    size_t colored = term->color ? colored_prefix(label, start, end) : 0u;

    if (colored &&
        (snag_buf_append(out, "\033[1;36m", 7u) < 0 ||
         snag_buf_append(out, frame->data + start, colored) < 0 ||
         snag_buf_append(out, "\033[0m", 4u) < 0))
        return -1;
    return snag_buf_append(out, frame->data + start + colored, end - start - colored);
}

static bool
same_prompt_layout(const struct snag_term *term, const struct snag_buf *next)
{
    size_t a = 0u, b = 0u;

    if (!term->prompt_visible || !term->painted_prompt.len ||
        term->painted_columns != term->columns)
        return false;
    while (a <= term->painted_prompt.len && b <= next->len) {
        struct prompt_row old = prompt_row(&term->painted_prompt, a, term->columns);
        struct prompt_row row = prompt_row(next, b, term->columns);
        if (old.soft != row.soft ||
            (old.next <= term->painted_prompt.len) != (row.next <= next->len) ||
            (old.soft && old.width != row.width))
            return false;
        a = old.next;
        b = row.next;
    }
    return a > term->painted_prompt.len && b > next->len;
}

static int
paint_prompt(struct snag_term *term, struct snag_buf *frame, size_t label,
             size_t cursor_row, size_t cursor_col, size_t end_row, size_t end_col)
{
    struct snag_buf out;
    size_t row = term->prompt_visible ? term->rendered_cursor_row : 0u;
    size_t col = term->prompt_visible ? term->rendered_cursor_col : 0u;
    size_t old_rows = term->prompt_visible ? term->rendered_rows : 0u;
    bool stable = same_prompt_layout(term, frame);
    bool repair_wrap = false;
    size_t max;
    int rc = -1;

    if (frame->max > (SIZE_MAX - 256u) / 16u || old_rows > SIZE_MAX / 32u ||
        !snag_size_add(frame->max * 16u + 256u, old_rows * 32u, &max)) {
        errno = EOVERFLOW;
        return -1;
    }
    snag_buf_init(&out, max);
    if (term->rendered_cursor_pending_wrap && snag_buf_append(&out, " \b", 2u) < 0)
        goto out;
    if (stable) {
        size_t a = 0u, b = 0u, y = 0u;
        while (b <= frame->len) {
            struct prompt_row old = prompt_row(&term->painted_prompt, a, term->columns);
            struct prompt_row next = prompt_row(frame, b, term->columns);
            size_t ae = old.end, be = next.end, prefix = 0u;
            a = old.start;
            b = next.start;
            while (a < ae && b < be) {
                size_t ac = prompt_cell_end(term->painted_prompt.data, a, ae);
                size_t bc = prompt_cell_end(frame->data, b, be);
                if (!same_prompt_cell(term, frame, label, a, ac, b, bc))
                    break;
                prefix += snag_term_text_width((char *)frame->data + b, bc - b);
                a = ac;
                b = bc;
            }
            /* Equal-width rows can retain their unchanged trailing cells too. */
            while (old.width == next.width && a < ae && b < be) {
                size_t ac = prompt_cell_start(term->painted_prompt.data, a, ae);
                size_t bc = prompt_cell_start(frame->data, b, be);
                if (!same_prompt_cell(term, frame, label, ac, ae, bc, be))
                    break;
                ae = ac;
                be = bc;
            }
            if (a != ae || b != be) {
                if (prompt_move(&out, &row, &col, y, prefix) < 0 ||
                    prompt_span(&out, term, frame, label, b, be) < 0)
                    goto out;
                col += snag_term_text_width((char *)frame->data + b, be - b);
                /* Materialize soft wrap with the next row's actual first cell.
                 * CR here would discard the terminal's reflow/join marker. */
                if (col == term->columns) {
                    col = 0u;
                    if (next.soft) {
                        struct prompt_row following = prompt_row(frame, next.next, term->columns);
                        ++row;
                        if (following.start < following.end) {
                            size_t first = prompt_cell_end(frame->data, following.start, following.end);
                            if (prompt_span(&out, term, frame, label, following.start, first) < 0)
                                goto out;
                            col = snag_term_text_width((char *)frame->data + following.start,
                                                       first - following.start);
                        } else if (snag_buf_append(&out, " \b", 2u) < 0) {
                            goto out;
                        }
                    } else if (snag_buf_putc(&out, '\r') < 0) {
                        goto out;
                    }
                }
                if (next.width < old.width && snag_buf_append(&out, "\033[K", 3u) < 0)
                    goto out;
                if (next.width < old.width && end_row && !end_col && row == end_row)
                    repair_wrap = true;
            }
            a = old.next;
            b = next.next;
            ++y;
        }
    } else {
        size_t start = 0u, prefix_row = 0u, prefix_col = 0u;
        if (term->prompt_visible && term->painted_columns == term->columns) {
            while (start < frame->len && start < term->painted_prompt.len) {
                size_t a = term->painted_prompt.data[start] == '\r' ? start + 2u :
                    prompt_cell_end(term->painted_prompt.data, start, term->painted_prompt.len);
                size_t b = frame->data[start] == '\r' ? start + 2u :
                    prompt_cell_end(frame->data, start, frame->len);
                if (!same_prompt_cell(term, frame, label, start, a, start, b))
                    break;
                start = b;
            }
            frame_position(&term->painted_prompt, start, term->columns,
                            &prefix_row, &prefix_col);
        }
        if (prompt_move(&out, &row, &col, prefix_row, prefix_col) < 0)
            goto out;
        /* Overwrite in natural wrap order; clear only obsolete row suffixes. */
        for (size_t i = start; i + 1u < frame->len; ++i) {
            if (frame->data[i] != '\r' || frame->data[i + 1u] != '\n')
                continue;
            if (prompt_span(&out, term, frame, label, start, i) < 0 ||
                snag_buf_append(&out, "\033[K\r\n", 5u) < 0)
                goto out;
            start = i + 2u;
            ++i;
        }
        if (prompt_span(&out, term, frame, label, start, frame->len) < 0 ||
            (end_row && !end_col && snag_buf_append(&out, " \b", 2u) < 0) ||
            ((end_col || old_rows > end_row) && snag_buf_append(&out, "\033[K", 3u) < 0))
            goto out;
        row = end_row;
        col = end_col;
        repair_wrap = end_row && !end_col && old_rows > end_row;
        for (size_t y = end_row + 1u; y < old_rows; ++y)
            if (prompt_move(&out, &row, &col, y, 0u) < 0 ||
                snag_buf_append(&out, "\033[K", 3u) < 0)
                goto out;
    }
    /* EL on an empty continuation row can clear the preceding soft-wrap
     * marker (not just its cells). Restore it after shrinking to a margin. */
    if (repair_wrap) {
        size_t last = prompt_cell_start(frame->data, 0u, frame->len);
        size_t last_row, last_col;
        frame_position(frame, last, term->columns, &last_row, &last_col);
        if (prompt_move(&out, &row, &col, last_row, last_col) < 0 ||
            prompt_span(&out, term, frame, label, last, frame->len) < 0 ||
            snag_buf_append(&out, " \b", 2u) < 0)
            goto out;
        row = end_row;
        col = 0u;
    }
    if (prompt_move(&out, &row, &col, cursor_row, cursor_col) < 0 ||
        snag_term_write(STDERR_FILENO, out.data, out.len) < 0)
        goto out;
    snag_buf_free(&term->painted_prompt);
    term->painted_prompt = *frame;
    memset(frame, 0, sizeof(*frame));
    term->painted_label_len = label;
    term->painted_columns = term->columns;
    term->painted_color = term->color;
    term->rendered_rows = end_row + 1u;
    term->rendered_cursor_row = cursor_row;
    term->rendered_cursor_col = cursor_col;
    term->rendered_end_at_margin = end_row != 0u && end_col == 0u;
    term->rendered_cursor_pending_wrap = false;
    term->prompt_visible = true;
    rc = 0;
out:
    if (rc < 0)
        snag_buf_reset(&term->painted_prompt);
    snag_buf_free(&out);
    return rc;
}

static int
redraw(struct snag_term *term)
{
    struct snag_buf out;
    char current[SNAG_TERM_LABEL_BYTES];
    size_t cursor_row = 0u, cursor_col = 0u;
    size_t end_row = 0u, end_col = 0u;
    size_t label_len;
    const char *label;
    int rc = -1;

    if (term->input_only || !term->opened || !term->prompt_wanted || term->output_depth)
        return 0;
    if (term->prompt_template[0] &&
        compose_prompt(term->prompt_template, term->spinner,
                       term->spinner_states,
                       spinner_step(term, snag_monotonic_ms()), current) < 0)
        return -1;
    if (term->prompt_template[0])
        memcpy(term->label, current, strlen(current) + 1u);
    label = prompt_label(term, &label_len);
    if (term->active && !term->prompt_visible && term->output_seen) {
        if (!term->output_ended_lf &&
            snag_term_write(STDERR_FILENO, term->capable ? "\r\n" : "\n",
                           term->capable ? 2u : 1u) < 0)
            return -1;
        term->output_detour = term->capable && !term->output_ended_lf &&
                              !term->typing_active;
        if (!term->output_detour)
            term->output_seen = false;
    }
    if (!term->capable) {
        if (term->prompt_visible)
            return 0;
        if ((term->color &&
             snag_term_write(STDERR_FILENO,
                             "\033[1;36m", 7u) < 0) ||
            snag_term_write_safe(STDERR_FILENO, label, label_len) < 0 ||
            (term->color && snag_term_write(STDERR_FILENO, "\033[0m", 4u) < 0) ||
            (term->draft.len &&
             snag_term_write_safe(STDERR_FILENO, (char *)term->draft.data,
                                  term->draft.len) < 0))
            return -1;
        term->prompt_visible = true;
        return 0;
    }
    if (compose_frame(term, &out, &label_len, &cursor_row, &cursor_col,
                       &end_row, &end_col) < 0)
        goto out;
    rc = paint_prompt(term, &out, label_len, cursor_row, cursor_col, end_row, end_col);
out:
    snag_buf_free(&out);
    return rc;
}

int
snag_term_set_prompt_label(struct snag_term *term, bool active,
                          const char *label)
{
    size_t len;

    if (!term || !label || !(len = strlen(label)) || len >= sizeof(term->label)) {
        errno = EINVAL;
        return -1;
    }
    if (!term->capable &&
        (term->active != active || strcmp(term->label, label) != 0) &&
        snag_term_hide(term) < 0)
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
snag_term_set_prompt_template(struct snag_term *term, bool active,
                             const char *label,
                             const char *const spinners[SNAG_TERM_SPINNER_COUNT],
                             uint32_t per_second, unsigned int states)
{
    struct snag_term_spinner configured[SNAG_TERM_SPINNER_COUNT];
    char expanded[SNAG_TERM_LABEL_BYTES];
    size_t len;
    bool unchanged;

    if (!term || !label || !(len = strlen(label)) ||
        len >= sizeof(term->prompt_template) || !spinners ||
        per_second < 1u || per_second > 60u ||
        states >= (1u << SNAG_TERM_SPINNER_COUNT)) {
        errno = EINVAL;
        return -1;
    }
    for (size_t i = 0u; i < SNAG_TERM_SPINNER_COUNT; ++i)
        if (!spinners[i] || prepare_spinner(&configured[i], spinners[i]) < 0)
            goto invalid;
    if (prompt_fits(label, configured) < 0)
        goto invalid;
    unchanged = strcmp(term->prompt_template, label) == 0 &&
                term->spinner_states == states && term->spinner_per_second == per_second;
    for (size_t i = 0u; i < SNAG_TERM_SPINNER_COUNT && unchanged; ++i)
        unchanged = strcmp(term->spinner[i].value, spinners[i]) == 0;
    if (compose_prompt(label, configured, states,
                       unchanged ? spinner_step(term, snag_monotonic_ms()) : 0u,
                       expanded) < 0)
        return -1;
    if (!term->capable &&
        (term->active != active || strcmp(term->label, expanded) != 0) &&
        snag_term_hide(term) < 0)
        return -1;
    memcpy(term->prompt_template, label, len + 1u);
    memcpy(term->label, expanded, strlen(expanded) + 1u);
    memcpy(term->spinner, configured, sizeof(term->spinner));
    term->spinner_states = states;
    term->spinner_per_second = per_second;
    if (!unchanged)
        term->spinner_epoch_ms = snag_monotonic_ms();
    term->active = active;
    if (!active)
        term->typing_active = false;
    term->prompt_wanted = true;
    term->line_submission_echoed = false;
    if (term->output_depth)
        term->redraw_after_output = true;
    return term->defer_redraw ? 0 : redraw(term);
invalid:
    errno = EINVAL;
    return -1;
}

int
snag_term_set_spinner_states(struct snag_term *term, unsigned int states)
{
    if (!term || states >= (1u << SNAG_TERM_SPINNER_COUNT)) {
        errno = EINVAL;
        return -1;
    }
    if (!term->prompt_template[0] || term->spinner_states == states)
        return 0;
    term->spinner_states = states;
    term->spinner_epoch_ms = snag_monotonic_ms();
    return update_spinners(term, 0u);
}

int
snag_term_output_begin(struct snag_term *term, bool persistent)
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
            rc = snag_term_hide(term);
        }
        if (rc < 0) {
            --term->output_depth;
            return -1;
        }
    }
    return 0;
}

int
snag_term_output_end(struct snag_term *term)
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
    return (term->active || term->defer_redraw) && !redraw_requested ?
        0 : redraw(term);
}

static void
history_reset_navigation(struct snag_term *term)
{
    free(term->history_draft);
    term->history_draft = NULL;
    term->history_pos = SIZE_MAX;
}

static void
history_clear(struct snag_term *term)
{
    history_reset_navigation(term);
    snag_history_snapshot_free(&term->history);
}

static int
replace_draft(struct snag_term *term, const char *text)
{
    size_t len = strlen(text);

    if (len > SNAG_MAX_DIRECT_PROMPT) {
        errno = EOVERFLOW;
        return -1;
    }
    snag_buf_reset(&term->draft);
    if (snag_buf_append(&term->draft, text, len) < 0)
        return -1;
    term->cursor = len;
    term->utf8_pending_len = 0u;
    return redraw(term);
}

int
snag_term_restore_draft(struct snag_term *term, const char *text)
{
    term->completion_armed = false;
    term->prompt_wanted = true;
    mark_input_activity(term);
    return replace_draft(term, text);
}

bool
snag_term_consume_echoed_submission(struct snag_term *term, const char *label)
{
    bool match;

    if (!term || !term->line_submission_echoed)
        return false;
    match = strcmp(label, term->label) == 0;
    term->line_submission_echoed = false;
    return match;
}

static int
history_up(struct snag_term *term)
{
    if (term->history_pos == SIZE_MAX)
        term->history_refresh_requested = true;
    if (!term->history.count)
        return 0;
    if (term->history_pos == SIZE_MAX) {
        if (snag_buf_terminate(&term->draft) < 0)
            return -1;
        term->history_draft = snag_strdup_checked((char *)term->draft.data,
                                                 SNAG_MAX_DIRECT_PROMPT);
        if (!term->history_draft)
            return -1;
        term->history_pos = term->history.count;
    }
    if (term->history_pos)
        --term->history_pos;
    return replace_draft(term, term->history.items[term->history_pos]);
}

static int
history_down(struct snag_term *term)
{
    char *draft;

    if (term->history_pos == SIZE_MAX) {
        term->history_refresh_requested = true;
        return 0;
    }
    if (term->history_pos + 1u < term->history.count) {
        ++term->history_pos;
        return replace_draft(term, term->history.items[term->history_pos]);
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
search_label_update(struct snag_term *term)
{
    const char *prefix = term->search_failed ?
                         "(failed reverse-i-search)`" :
                         "(reverse-i-search)`";

    snag_buf_reset(&term->search_label);
    if (snag_buf_append(&term->search_label, prefix, strlen(prefix)) < 0 ||
        snag_buf_append(&term->search_label, term->search_query.data,
                       term->search_query.len) < 0 ||
        snag_buf_append(&term->search_label, "': ", 3u) < 0 ||
        snag_buf_terminate(&term->search_label) < 0)
        return -1;
    return 0;
}

static int
search_find(struct snag_term *term, size_t before)
{
    if (snag_buf_terminate(&term->search_query) < 0)
        return -1;
    while (before) {
        size_t i = --before;
        if (strstr(term->history.items[i], (char *)term->search_query.data)) {
            term->search_pos = i;
            term->search_failed = false;
            if (search_label_update(term) < 0)
                return -1;
            return replace_draft(term, term->history.items[i]);
        }
    }
    term->search_pos = 0u;
    term->search_failed = true;
    return search_label_update(term) < 0 ? -1 : redraw(term);
}

static int
search_begin(struct snag_term *term)
{
    if (term->searching)
        return search_find(term, term->search_pos);
    term->history_refresh_requested = true;
    if (snag_buf_terminate(&term->draft) < 0)
        return -1;
    term->search_original = snag_strdup_checked((char *)term->draft.data,
                                               SNAG_MAX_DIRECT_PROMPT);
    if (!term->search_original)
        return -1;
    term->search_original_cursor = term->cursor;
    snag_buf_reset(&term->search_query);
    if (snag_buf_append(&term->search_query, term->draft.data,
                       term->draft.len) < 0) {
        free(term->search_original);
        term->search_original = NULL;
        return -1;
    }
    term->searching = true;
    term->search_failed = false;
    term->search_pos = term->history.count;
    return search_find(term, term->history.count);
}

int
snag_term_history_set(struct snag_term *term,
                     struct snag_history_snapshot *snapshot, bool refresh)
{
    size_t distance = term->history_pos == SIZE_MAX ? 0u :
                      term->history.count - term->history_pos;
    size_t matched = SIZE_MAX;
    if (!refresh && (term->searching || distance)) {
        snag_history_snapshot_free(snapshot);
        return 0;
    }
    if (refresh && term->searching && !term->search_failed &&
        term->search_pos < term->history.count) {
        const char *selected = term->history.items[term->search_pos];
        for (size_t i = snapshot->count; i > 0u; --i)
            if (strcmp(selected, snapshot->items[i - 1u]) == 0) {
                matched = i - 1u;
                break;
            }
    }
    snag_history_snapshot_free(&term->history);
    term->history = *snapshot;
    memset(snapshot, 0, sizeof(*snapshot));
    if (refresh && term->searching && matched != SIZE_MAX) {
        term->search_pos = matched;
        return 0;
    }
    if (refresh && term->searching)
        return search_find(term, term->history.count);
    if (refresh && distance && term->history.count) {
        term->history_pos = distance > term->history.count ? 0u :
                            term->history.count - distance;
        return replace_draft(term, term->history.items[term->history_pos]);
    }
    return 0;
}

static int
search_accept(struct snag_term *term, bool abort)
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
    snag_buf_reset(&term->search_query);
    snag_buf_reset(&term->search_label);
    history_reset_navigation(term);
    if (abort) {
        rc = replace_draft(term, original ? original : "");
        if (rc == 0) {
            term->cursor = cursor;
            rc = redraw(term);
        }
    } else {
        rc = redraw(term);
    }
    free(original);
    return rc;
}

static int
search_insert(struct snag_term *term, const unsigned char *data, size_t len)
{
    size_t before = term->search_failed ? 0u : term->search_pos + 1u;

    if (len > SNAG_MAX_DIRECT_PROMPT - term->search_query.len) {
        errno = EOVERFLOW;
        return -1;
    }
    if (snag_buf_append(&term->search_query, data, len) < 0)
        return -1;
    mark_input_activity(term);
    return search_find(term, before);
}

static int
search_backspace(struct snag_term *term)
{
    if (term->search_query.len)
        term->search_query.len = previous_cp(term->search_query.data,
                                             term->search_query.len);
    mark_input_activity(term);
    return search_find(term, term->history.count);
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
    n = snag_utf8_size(s[pos]);
    return n && n <= len - pos ? pos + n : pos + 1u;
}

static int
insert_bytes(struct snag_term *term, const unsigned char *data, size_t len)
{
    if (len > SNAG_MAX_DIRECT_PROMPT - term->draft.len) {
        errno = EOVERFLOW;
        return -1;
    }
    if (snag_buf_reserve(&term->draft, len) < 0)
        return -1;
    memmove(term->draft.data + term->cursor + len,
            term->draft.data + term->cursor,
            term->draft.len - term->cursor);
    memcpy(term->draft.data + term->cursor, data, len);
    term->draft.len += len;
    term->cursor += len;
    history_reset_navigation(term);
    mark_input_activity(term);
    return redraw(term);
}

static int
delete_range(struct snag_term *term, size_t start, size_t end)
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
    return redraw(term);
}

static bool
word_space(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static size_t
command_name_length(const struct snag_term_command *command)
{
    size_t len = 0u;

    while (command->syntax[len] &&
           !word_space((unsigned char)command->syntax[len]))
        ++len;
    return len;
}

static int
replace_completion(struct snag_term *term, const char *name, size_t name_len,
                   size_t token_start, size_t token_end, bool unique)
{
    bool space = unique && (token_end == term->draft.len ||
                            term->draft.data[token_end] != ' ');
    size_t tail_len = term->draft.len - token_end;
    size_t next_len;

    if (!snag_size_add(name_len, tail_len, &next_len) ||
        !snag_size_add(next_len, token_start + (size_t)space, &next_len) ||
        next_len > SNAG_MAX_DIRECT_PROMPT) {
        errno = EOVERFLOW;
        return -1;
    }
    if (next_len > term->draft.len &&
        snag_buf_reserve(&term->draft, next_len - term->draft.len) < 0)
        return -1;
    memmove(term->draft.data + token_start + name_len + (size_t)space,
            term->draft.data + token_end,
            tail_len);
    memcpy(term->draft.data + token_start, name, name_len);
    if (space)
        term->draft.data[token_start + name_len] = ' ';
    term->draft.len = next_len;
    term->cursor = token_start + name_len + (size_t)space;
    if (unique && !space && term->cursor < term->draft.len &&
        term->draft.data[term->cursor] == ' ')
        ++term->cursor;
    history_reset_navigation(term);
    mark_input_activity(term);
    return redraw(term);
}

/* One matching/replacement/listing policy for commands, destinations and nicks. */
struct completion {
    struct snag_buf names;
    size_t count, common;
    bool fold;
};

static bool
completion_byte_equal(unsigned char a, unsigned char b, bool fold)
{
    return fold ? snag_irc_fold(a) == snag_irc_fold(b) : a == b;
}

static int
completion_add(struct completion *matches, const char *name, size_t len,
                const unsigned char *prefix, size_t prefix_len)
{
    size_t i = 0u;

    while (i < prefix_len && i < len &&
           completion_byte_equal((unsigned char)name[i], prefix[i], matches->fold))
        ++i;
    if (i != prefix_len || !len)
        return 0;
    for (size_t pos = 0u; pos < matches->names.len;) {
        const char *previous = (const char *)matches->names.data + pos;
        size_t previous_len = strlen(previous), same = 0u;
        while (same < len && same < previous_len &&
               completion_byte_equal((unsigned char)name[same],
                                     (unsigned char)previous[same], matches->fold))
            ++same;
        if (same == len && same == previous_len)
            return 0;
        pos += previous_len + 1u;
    }
    if (!matches->count)
        matches->common = len;
    else {
        while (i < matches->common && i < len &&
               completion_byte_equal((unsigned char)name[i], matches->names.data[i], matches->fold))
            ++i;
        matches->common = i;
    }
    ++matches->count;
    return snag_buf_append(&matches->names, name, len) < 0 ? -1 :
           snag_buf_putc(&matches->names, '\0');
}

static int
flush_completions(struct snag_term *term)
{
    if (!term->completion_output.len || term->input_only || term->output_depth)
        return 0;
    /* Output may read more input under backpressure. Detach this snapshot so
     * another double Tab cannot invalidate the bytes being written. */
    struct snag_buf output = term->completion_output;
    snag_buf_init(&term->completion_output, SNAG_MAX_DIRECT_PROMPT);
    int rc = -1;
    if (leave_prompt(term) < 0 || snag_term_output_begin(term, true) < 0)
        goto out;
    if ((!term->output_seen || term->output_ended_lf ||
         snag_term_write(STDERR_FILENO, "\n", 1u) == 0) &&
        snag_term_write(STDERR_FILENO, output.data, output.len) == 0)
        rc = snag_term_note_output(term, (const char *)output.data, output.len, "");
    term->redraw_after_output = true;
    if (snag_term_output_end(term) < 0)
        rc = -1;
out:
    snag_buf_free(&output);
    return rc;
}

static int
finish_completion(struct snag_term *term, struct completion *matches,
                   size_t start, size_t end)
{
    bool list = term->completion_armed && matches->count > 1u;
    term->completion_armed = matches->count > 1u;
    if (!matches->count)
        return 0;
    while (matches->common &&
           !snag_utf8_valid(matches->names.data, matches->common, true))
        --matches->common;
    if (matches->count == 1u || matches->common > term->cursor - start)
        if (replace_completion(term, (const char *)matches->names.data,
                               matches->common, start, end, matches->count == 1u) < 0)
            return -1;
    if (!list)
        return 0;
    snag_buf_reset(&term->completion_output);
    size_t column = 0u, width = 0u;
    for (size_t pos = 0u; pos < matches->names.len;) {
        const char *name = (const char *)matches->names.data + pos;
        size_t len = strlen(name), cells = snag_term_text_width(name, len) + matches->fold;
        if (cells > width)
            width = cells;
        pos += len + 1u;
    }
    for (size_t pos = 0u; pos < matches->names.len;) {
        const char *name = (const char *)matches->names.data + pos;
        size_t len = strlen(name), cells = snag_term_text_width(name, len) + matches->fold;
        if ((matches->fold && snag_buf_putc(&term->completion_output, '@') < 0) ||
            snag_term_append_safe(&term->completion_output, name, len) < 0)
            return -1;
        pos += len + 1u;
        column += width + 2u;
        if (pos == matches->names.len || column + width > snag_term_columns(term)) {
            if (snag_buf_putc(&term->completion_output, '\n') < 0)
                return -1;
            column = 0u;
        } else {
            for (size_t pad = cells; pad < width + 2u; ++pad)
                if (snag_buf_putc(&term->completion_output, ' ') < 0)
                    return -1;
        }
    }
    return flush_completions(term);
}

static int
complete_command_name(struct snag_term *term, bool *handled)
{
    size_t token_end = 0u, prefix_len = term->cursor;
    struct completion matches = {0};
    int rc = -1;

    *handled = false;
    if (!prefix_len || !term->draft.len || term->draft.data[0] != '/' ||
        (term->draft.len > 1u && term->draft.data[1] == '/'))
        return 0;
    while (token_end < term->draft.len && !word_space(term->draft.data[token_end]))
        ++token_end;
    if (prefix_len > token_end)
        return 0;
    *handled = true;
    snag_buf_init(&matches.names, SNAG_MAX_DIRECT_PROMPT);
    size_t destinations = term->destinations ? term->destinations->count : 0u;
    for (size_t i = 0u; i < term->command_count + destinations; ++i) {
        char numeric[16u];
        struct snag_term_command command;

        if (i < term->command_count)
            command = term->commands[i];
        else {
            (void)snprintf(numeric, sizeof(numeric), "/%u",
                term->destinations->items[i - term->command_count].target.id);
            command = (struct snag_term_command){numeric, NULL};
        }
        if (command.syntax && completion_add(&matches, command.syntax,
                command_name_length(&command), term->draft.data, prefix_len) < 0)
            goto out;
    }
    rc = finish_completion(term, &matches, 0u, token_end);
out:
    snag_buf_free(&matches.names);
    return rc;
}

static bool
nick_byte(unsigned char c)
{
    return c >= 0x80u || snag_irc_nick_char(c);
}

static int
complete_mention(struct snag_term *term, bool *handled)
{
    size_t start = term->cursor, end = term->cursor;
    struct completion matches = {.fold = true};
    int rc = -1;
    uint32_t id;
    size_t body;
    enum snag_irc_target_command command = snag_irc_target_parse(
        (const char *)term->draft.data, term->draft.len, &id, &body);

    *handled = false;
    if (!term->chat && command != SNAG_IRC_TARGET_SEND && command != SNAG_IRC_TARGET_ALL)
        return 0;
    while (start && nick_byte(term->draft.data[start - 1u]))
        --start;
    if (!start || term->draft.data[start - 1u] != '@' ||
        (start > 1u && nick_byte(term->draft.data[start - 2u])))
        return 0;
    *handled = true;
    while (end < term->draft.len && nick_byte(term->draft.data[end]))
        ++end;
    size_t prefix = term->cursor - start;
    snag_buf_init(&matches.names, SNAG_MAX_DIRECT_PROMPT);
    size_t destinations = term->destinations ? term->destinations->count : 1u;
    for (size_t destination = 0u; destination < destinations; ++destination) {
      const char *nicks = term->nicks;
      if (term->destinations) {
        const struct snag_irc_destination *item = &term->destinations->items[destination];
        if (command == SNAG_IRC_TARGET_INVALID ||
            (command != SNAG_IRC_TARGET_ALL && item->target.id !=
             (command == SNAG_IRC_TARGET_SEND ? id : term->destination.id)))
            continue;
        nicks = item->nicks;
      }
      for (const char *nick = nicks; nick && *nick;) {
        const char *line = strchr(nick, '\n');
        size_t len = line ? (size_t)(line - nick) : strlen(nick);

        if (completion_add(&matches, nick, len, term->draft.data + start, prefix) < 0)
            goto out;
        nick = line ? line + 1u : NULL;
      }
    }
    rc = finish_completion(term, &matches, start, end);
out:
    snag_buf_free(&matches.names);
    return rc;
}

static int
suspend_terminal(struct snag_term *term)
{
    if (snag_term_hide(term) < 0)
        return -1;
    if (term->bracketed_paste &&
        snag_term_write(STDERR_FILENO, "\033[?2004l", 8u) < 0)
        return -1;
    term->bracketed_paste = false;
    if (snag_term_input_flush(&term->host) < 0 ||
        snag_term_input_restore(&term->host, false) < 0)
        return -1;
    term->raw = false;
    if (raise(SIGSTOP) < 0)
        return -1;
    if (set_raw(term) < 0)
        return -1;
    if (term->capable && snag_term_write(STDERR_FILENO, "\033[?2004h", 8u) < 0)
        return -1;
    term->bracketed_paste = term->capable;
    update_size(term);
    return redraw(term);
}

static int
complete_action(struct snag_term *term, enum snag_term_action action,
                enum snag_term_action *out, char **text)
{
    char *copy;
    uint32_t target;
    size_t body;
    enum snag_irc_target_command destination = snag_irc_target_parse(
        (const char *)term->draft.data, term->draft.len, &target, &body);
    bool verbosity = snag_verbosity_command((const char *)term->draft.data,
                                           term->draft.len);

    if (action == SNAG_TERM_QUEUE && verbosity)
        action = SNAG_TERM_SUBMIT;
    bool local = action == SNAG_TERM_SUBMIT &&
                 (destination == SNAG_IRC_TARGET_SELECT || verbosity);
    if (local ? term->local_backlog : term->input_backlog)
        return snag_term_write(STDERR_FILENO, "\a", 1u);
    if (term->utf8_pending_len || !term->draft.len)
        return 0;
    if (term->capable) {
        if (snag_term_hide(term) < 0)
            return -1;
    } else {
        term->prompt_visible = false;
        term->line_submission_echoed = true;
    }
    if (!snag_utf8_valid(term->draft.data, term->draft.len, true)) {
        errno = EILSEQ;
        return -1;
    }
    if (snag_buf_terminate(&term->draft) < 0)
        return -1;
    copy = snag_strdup_checked((char *)term->draft.data, SNAG_MAX_DIRECT_PROMPT);
    if (!copy)
        return -1;
    /* A completion separator is not an argument to a bare slash command.
     * Preserve argument/body whitespace and all ordinary or escaped text. */
    if (copy[0] == '/' && copy[1] != '/' && !strchr(copy, '\n') && !strchr(copy, '\r')) {
        size_t token = strcspn(copy, " \t");
        if (copy[token] && strspn(copy + token, " \t") == strlen(copy + token))
            copy[token] = '\0';
    }
    snag_buf_reset(&term->draft);
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
feed_text_byte(struct snag_term *term, unsigned char byte)
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
    expected = snag_utf8_size(term->utf8_pending[0]);
    if (!expected || term->utf8_pending_len > expected) {
        term->utf8_pending_len = 0u;
        errno = EILSEQ;
        return -1;
    }
    if (term->utf8_pending_len < expected)
        return 0;
    if (!snag_utf8_valid(term->utf8_pending, expected, true)) {
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
apply_key(struct snag_term *term, int key)
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
feed_escape(struct snag_term *term, unsigned char byte)
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
feed_paste(struct snag_term *term, unsigned char byte)
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
complete_exit(struct snag_term *term, enum snag_term_action *action)
{
    if (snag_term_hide(term) < 0)
        return -1;
    term->prompt_wanted = false;
    *action = SNAG_TERM_EXIT;
    return 1;
}

static int
cancel_line(struct snag_term *term, enum snag_term_action *action)
{
    bool interrupt = term->active && !term->searching && !term->draft.len;

    if (!term->input_only && !term->prompt_visible && redraw(term) < 0)
        return -1;
    if (!term->input_only && term->capable && term->prompt_visible) {
        term->cursor = term->draft.len;
        if (redraw(term) < 0)
            return -1;
    }
    if (!term->input_only && snag_term_write(STDERR_FILENO, "^C\n", 3u) < 0)
        return -1;
    free(term->search_original);
    term->search_original = NULL;
    term->searching = false;
    term->search_failed = false;
    term->search_pos = SIZE_MAX;
    snag_buf_reset(&term->search_query);
    snag_buf_reset(&term->search_label);
    snag_buf_reset(&term->draft);
    term->cursor = 0u;
    term->utf8_pending_len = 0u;
    term->escape_len = 0u;
    term->paste = false;
    term->paste_end_match = 0u;
    term->typing_active = false;
    if (term->input_only) {
        term->cancel_pending = true;
        goto logical;
    }
    term->prompt_visible = false;
    term->rendered_rows = 0u;
    term->rendered_cursor_row = 0u;
    term->rendered_cursor_col = 0u;
    term->rendered_end_at_margin = false;
    term->rendered_cursor_pending_wrap = false;
    term->output_seen = false;
    term->output_ended_lf = true;
    term->output_detour = false;
    term->output_columns = 0u;
    snag_buf_reset(&term->output_line);
logical:
    history_reset_navigation(term);
    term->prompt_clock.captured = false;
    term->prompt_wanted = false;
    *action = interrupt ? SNAG_TERM_INTERRUPT : SNAG_TERM_CANCEL;
    return 1;
}

static int
feed_byte(struct snag_term *term, unsigned char byte,
          enum snag_term_action *action, char **text)
{
    if (byte != '\t')
        term->completion_armed = false;
    if (byte == 0x03u) {
        uint64_t now = snag_monotonic_ms();
        if (!term->ctrl_c_count || now - term->ctrl_c_since_ms > 2000u) {
            term->ctrl_c_since_ms = now;
            term->ctrl_c_count = 0u;
        }
        if (++term->ctrl_c_count == 5u)
            return complete_exit(term, action);
        return cancel_line(term, action);
    }
    term->ctrl_c_count = 0u;
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
        return complete_action(term, SNAG_TERM_SUBMIT, action, text);
    case '\n': {
        if (!term->capable)
            return complete_action(term, SNAG_TERM_SUBMIT, action, text);
        unsigned char lf = '\n';
        return insert_bytes(term, &lf, 1u);
    }
    case '\t':
        if (!term->draft.len) {
            if (term->input_backlog)
                return snag_term_write(STDERR_FILENO, "\a", 1u);
            *action = SNAG_TERM_VIEW;
            return 1;
        }
        {
            bool handled;
            int rc = complete_command_name(term, &handled);

            if (rc < 0 || handled)
                return rc;
            rc = complete_mention(term, &handled);
            if (rc < 0 || handled)
                return rc;
        }
        if (term->active)
            return complete_action(term, SNAG_TERM_QUEUE, action, text);
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
        if (snag_term_hide(term) < 0)
            return -1;
        if (term->capable && snag_term_write(STDERR_FILENO,
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
consume_resize(struct snag_term *term)
{
    bool was_capable;
    bool now_capable;

    if (term->input_only || !sigwinch_pending)
        return 0;
    sigwinch_pending = 0;
    if (!term->opened)
        return 0;
    was_capable = term->capable;
    update_size(term);
    now_capable = term->capable;
    term->output_columns = 0u;
    for (size_t i = 0u; i < term->output_line.len;) {
        uint32_t cp;
        size_t n = snag_utf8_decode(term->output_line.data + i,
                                term->output_line.len - i, &cp);
        int width = snag_char_width(cp);

        if (width > 0)
            output_column_add(term, (size_t)width);
        i += n;
    }
    if (term->prompt_visible && was_capable) {
        if (now_capable && sync_prompt_layout_after_resize(term) < 0)
            return -1;
        if (!now_capable)
            term->capable = true;
        if (snag_term_hide(term) < 0) {
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
snag_term_poll(struct snag_term *term, int timeout_ms, snag_wake_fd wake_fd,
              enum snag_term_action *action, char **text)
{
    struct pollfd pfd[2] = {{STDIN_FILENO, POLLIN, 0},
                           {wake_fd, POLLIN, 0}};
    ssize_t count;
    int rc;

    *action = SNAG_TERM_NONE;
    *text = NULL;
    if (!term->input_only && term->cancel_pending) {
        term->cancel_pending = false;
        if (snag_term_hide(term) < 0 ||
            snag_term_write(STDERR_FILENO, "\n^C\n", 4u) < 0)
            return -1;
        term->output_seen = false;
        term->output_ended_lf = true;
        term->output_detour = false;
        term->output_columns = 0u;
        snag_buf_reset(&term->output_line);
    }
    if (consume_resize(term) < 0 || flush_completions(term) < 0)
        return -1;
    if (term->prompt_visible && term->capable && !term->searching &&
        !term->output_depth && animated_spinners(term) &&
        update_spinners(term, spinner_step(term, snag_monotonic_ms())) < 0)
        return -1;
    if (sigint_pending) {
        (void)atomic_fetch_sub_explicit(&sigint_pending, 1u, memory_order_relaxed);
        return feed_byte(term, 0x03u, action, text);
    }
    if (term->input_pos == term->input_len) {
        term->input_pos = 0u;
        term->input_len = 0u;
        if (term->searching && term->escape_len == 1u &&
            (timeout_ms < 0 || timeout_ms > 30))
            timeout_ms = 30;
        timeout_ms = spinner_timeout(term, timeout_ms);
        bool reveal = timeout_ms != 0 && term->active && term->prompt_wanted &&
                      !term->prompt_visible && !term->output_depth &&
                      !term->defer_redraw;
        uint64_t now = snag_monotonic_ms();
        uint64_t quiet = now >= term->last_output_ms ? now - term->last_output_ms : 0u;
        int reveal_wait = quiet < 150u ? (int)(150u - quiet) : 0;
        if (reveal && (timeout_ms < 0 || timeout_ms > reveal_wait))
            timeout_ms = reveal_wait;
        rc = poll(pfd, 2u, timeout_ms);
        if (sigint_pending) {
            (void)atomic_fetch_sub_explicit(&sigint_pending, 1u, memory_order_relaxed);
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
        if (rc == 0 && reveal &&
            snag_monotonic_ms() - term->last_output_ms >= 150u && redraw(term) < 0)
            return -1;
        if (rc == 0 && animated_spinners(term) &&
            update_spinners(term, spinner_step(term, snag_monotonic_ms())) < 0)
            return -1;
        if (rc <= 0)
            return rc;
        if (!(pfd[0].revents & POLLIN)) {
            if (pfd[0].revents & (POLLHUP | POLLERR | POLLNVAL)) {
                *action = SNAG_TERM_EXIT;
                return 1;
            }
            return 0;
        }
        count = snag_term_input_read(&term->host, term->input, sizeof(term->input));
        if (sigint_pending) {
            (void)atomic_fetch_sub_explicit(&sigint_pending, 1u, memory_order_relaxed);
            return feed_byte(term, 0x03u, action, text);
        }
        if (sigwinch_pending) {
            if (consume_resize(term) < 0)
                return -1;
            if (count < 0 && errno == EINTR)
                return 0;
        }
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return 0;
        if (count < 0)
            return -1;
        if (count == 0) {
            *action = SNAG_TERM_EXIT;
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
snag_term_close(struct snag_term *term)
{
    if (!term)
        return;
    if (term->opened) {
        (void)snag_term_hide(term);
        if (term->bracketed_paste)
            (void)snag_term_write(STDERR_FILENO, "\033[?2004l", 8u);
        if (term->raw)
            (void)snag_term_input_restore(&term->host, true);
    }
    if (term->sigwinch_installed)
        (void)sigaction(SIGWINCH, &term->host.sigwinch, NULL);
    if (term->sigint_installed)
        (void)sigaction(SIGINT, &term->host.sigint, NULL);
    history_clear(term);
    free(term->search_original);
    free(term->nicks);
    free(term->destinations);
    snag_buf_free(&term->search_label);
    snag_buf_free(&term->search_query);
    snag_buf_free(&term->draft);
    snag_buf_free(&term->output_cell);
    snag_buf_free(&term->output_line);
    snag_buf_free(&term->painted_prompt);
    snag_buf_free(&term->completion_output);
    if (output_owner == term)
        output_owner = NULL;
    for (size_t i = 0u; i < 2u; ++i)
        if (term->output_fd[i] >= 0)
            close(term->output_fd[i]);
    memset(term, 0, sizeof(*term));
    term->output_fd[0] = term->output_fd[1] = -1;
}

/* SPDX-License-Identifier: GPL-2.0-only */
#include "render.h"
#include "base.h"
#include "snajpagent.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define COLOR_RESET "\033[0m"
#define COLOR_META "\033[2m"
#define COLOR_AGENT "\033[1;36m"
#define COLOR_OPERATOR "\033[1;35m"
#define COLOR_ACTIVITY "\033[33m"
#define COLOR_SUCCESS "\033[32m"
#define COLOR_WARNING "\033[1;33m"
#define COLOR_ERROR "\033[1;31m"
#define COLOR_LIFECYCLE "\033[1;32m"
#define COLOR_DURABLE "\033[2;36m"
#define COLOR_PROTOCOL "\033[35m"
#define COLOR_TRANSPORT "\033[2;34m"
#define COLOR_HOST "\033[34m"
#define MARKDOWN_TABLE_COLUMNS 16u

enum markdown_table_alignment {
    TABLE_LEFT,
    TABLE_CENTER,
    TABLE_RIGHT
};

struct markdown_table_cell {
    const unsigned char *text;
    size_t len;
};

enum snj_render_record_kind {
    SNJ_RENDER_RECORD_BLOCK,
    SNJ_RENDER_RECORD_IRC,
    SNJ_RENDER_RECORD_PUBLIC
};

struct snj_render_record {
    struct snj_render_record *next;
    enum snj_render_record_kind kind;
    struct snj_buf text;
    const char *color;
    char *label;
    struct snj_irc_event irc;
    size_t colored_len;
    size_t displayed;
    int fd;
    unsigned char utf8_pending[4];
    size_t utf8_pending_len;
    bool terminal_safe;
    bool persistent;
    bool complete;
    bool aborted;
    bool physical_open;
    bool label_displayed;
    bool own_agent;
};

static int render_irc_event_now(struct snj_render *render,
                                const struct snj_irc_event *event,
                                bool own_agent);
static int flush_view(struct snj_render *render, enum snj_render_view view);

static int
write_literal(int fd, const char *s)
{
    return snj_write_full(fd, s, strlen(s));
}

static int
output_begin(struct snj_render *render, bool persistent)
{
    return render->term ? snj_term_output_begin(render->term, persistent) : 0;
}

static int
output_end(struct snj_render *render)
{
    return render->term ? snj_term_output_end(render->term) : 0;
}

static bool
color_enabled(const struct snj_render *render, int fd)
{
    return fd == STDOUT_FILENO ? render->color_stdout : render->color_stderr;
}

static int
write_role_block(struct snj_render *render, int fd, const char *color,
                 const char *text, size_t len, size_t colored_len,
                 bool terminal_safe, bool persistent)
{
    bool colored = color_enabled(render, fd) && colored_len != 0u;
    int rc = -1;
    int saved_errno = 0;

    if (colored_len > len) {
        errno = EINVAL;
        return -1;
    }
    if (output_begin(render, persistent) < 0)
        return -1;
    if (colored && write_literal(fd, color) < 0)
        goto out;
    if (colored_len &&
        (terminal_safe ? snj_term_write_safe(fd, text, colored_len) :
                         snj_write_full(fd, text, colored_len)) < 0)
        goto out;
    if (colored && write_literal(fd, COLOR_RESET) < 0)
        goto out;
    if (len > colored_len &&
        (terminal_safe ? snj_term_write_safe(fd, text + colored_len,
                                              len - colored_len) :
                         snj_write_full(fd, text + colored_len,
                                        len - colored_len)) < 0)
        goto out;
    if (render->term &&
        ((fd == STDOUT_FILENO && render->stdout_terminal) ||
         (fd == STDERR_FILENO && render->stderr_terminal)))
        snj_term_note_output(render->term, text, len);
    rc = 0;
out:
    if (rc < 0)
        saved_errno = errno;
    if (colored && rc < 0)
        (void)write_literal(fd, COLOR_RESET);
    if (output_end(render) < 0 && rc == 0)
        rc = -1;
    if (rc == 0 && persistent)
        render->previous_public_item = false;
    if (saved_errno)
        errno = saved_errno;
    return rc;
}

static void
free_record(struct snj_render_record *record)
{
    if (!record)
        return;
    snj_buf_free(&record->text);
    free(record->label);
    free(record);
}

static void
queue_record(struct snj_render *render, enum snj_render_view view,
             struct snj_render_record *record)
{
    if (render->view_tail[view])
        render->view_tail[view]->next = record;
    else
        render->view_head[view] = record;
    render->view_tail[view] = record;
}

static void
pop_record(struct snj_render *render, enum snj_render_view view)
{
    struct snj_render_record *record = render->view_head[view];

    if (!record)
        return;
    render->view_head[view] = record->next;
    if (!render->view_head[view])
        render->view_tail[view] = NULL;
    if (render->rollout_open == record)
        render->rollout_open = NULL;
    free_record(record);
}

static int
view_block(struct snj_render *render, enum snj_render_view view, int fd,
           const char *color, const char *text, size_t len,
           size_t colored_len, bool terminal_safe, bool persistent)
{
    struct snj_render_record *record;

    if (!render->networked ||
        (render->view == view && !render->view_head[view]))
        return write_role_block(render, fd, color, text, len, colored_len,
                                terminal_safe, persistent);
    record = calloc(1u, sizeof(*record));
    if (!record)
        return -1;
    record->kind = SNJ_RENDER_RECORD_BLOCK;
    record->fd = fd;
    record->color = color;
    record->colored_len = colored_len;
    record->terminal_safe = terminal_safe;
    record->persistent = persistent;
    snj_buf_init(&record->text, len);
    if (snj_buf_append(&record->text, text, len) < 0) {
        free_record(record);
        return -1;
    }
    queue_record(render, view, record);
    return render->view == view ? flush_view(render, view) : 0;
}

static size_t
first_line_len(const char *text, size_t len)
{
    const char *newline = memchr(text, '\n', len);

    return newline ? (size_t)(newline - text) + 1u : len;
}

static int
write_block(struct snj_render *render, int fd, const char *text, size_t len,
            bool terminal_safe, bool persistent)
{
    int rc;
    int saved_errno = 0;

    if (output_begin(render, persistent) < 0)
        return -1;
    rc = terminal_safe ? snj_term_write_safe(fd, text, len) :
                         snj_write_full(fd, text, len);
    if (rc == 0 && render->term &&
        ((fd == STDOUT_FILENO && render->stdout_terminal) ||
         (fd == STDERR_FILENO && render->stderr_terminal)))
        snj_term_note_output(render->term, text, len);
    if (rc < 0)
        saved_errno = errno;
    if (output_end(render) < 0 && rc == 0)
        rc = -1;
    if (saved_errno)
        errno = saved_errno;
    return rc;
}

void
snj_render_init(struct snj_render *render, unsigned int verbosity)
{
    memset(render, 0, sizeof(*render));
    render->verbosity = verbosity > 6u ? 6u : verbosity;
    render->markdown = true;
    render->view = SNJ_RENDER_ROLLOUT;
    render->public_fd = -1;
    render->stdout_terminal = isatty(STDOUT_FILENO) == 1;
    render->stderr_terminal = isatty(STDERR_FILENO) == 1;
}

void
snj_render_free(struct snj_render *render)
{
    if (!render)
        return;
    if (render->public_item_open)
        (void)snj_render_public_abort(render);
    for (unsigned int view = 0u; view < SNJ_RENDER_VIEW_COUNT; ++view)
        while (render->view_head[view])
            pop_record(render, (enum snj_render_view)view);
    render->rollout_open = NULL;
}

void
snj_render_set_markdown(struct snj_render *render, bool enabled)
{
    render->markdown = enabled;
}

void
snj_render_set_color(struct snj_render *render, enum snj_color_mode mode)
{
    const char *term = getenv("TERM");
    bool disabled = mode == SNJ_COLOR_NEVER ||
                    (mode == SNJ_COLOR_AUTO &&
                     (getenv("NO_COLOR") != NULL || !term ||
                      strcmp(term, "dumb") == 0));

    render->color_stdout = !disabled &&
        (mode == SNJ_COLOR_ALWAYS || render->stdout_terminal);
    render->color_stderr = !disabled &&
        (mode == SNJ_COLOR_ALWAYS || render->stderr_terminal);
    if (render->term)
        snj_term_set_color(render->term, render->color_stderr,
                           render->networked);
}

void
snj_render_set_networked(struct snj_render *render, bool networked,
                         const char *model_nick)
{
    render->networked = networked;
    render->view = networked ? SNJ_RENDER_CHAT : SNJ_RENDER_ROLLOUT;
    render->model_nick[0] = '\0';
    if (model_nick)
        (void)snprintf(render->model_nick, sizeof(render->model_nick), "%s",
                       model_nick);
    if (render->term)
        snj_term_set_color(render->term, render->color_stderr, networked);
}

void
snj_render_attach_term(struct snj_render *render, struct snj_term *term)
{
    render->term = term;
    snj_term_set_color(term, render->color_stderr, render->networked);
}

int
snj_render_orientation(struct snj_render *render,
                       const struct snj_session *session, bool resumed)
{
    struct snj_buf line;
    int rc;

    snj_buf_init(&line, 32768u);
    if (resumed) {
        rc = snj_buf_printf(&line,
            SNAJPAGENT_IDENTITY " · resumed · %s · session id %.8s "
            "· %llu turns · %zu queued%s\n",
            session->workspace, session->id,
            (unsigned long long)session->turn_count,
            session->pending_queue_count,
            session->pending_queue_count ? " paused" : "");
    } else {
        rc = snj_buf_printf(&line, SNAJPAGENT_IDENTITY
                            " · %s · session id %.8s\n",
                            session->workspace, session->id);
    }
    if (rc == 0)
        rc = write_role_block(render, STDERR_FILENO, COLOR_AGENT,
                              (char *)line.data, line.len, line.len,
                              render->stderr_terminal, true);
    snj_buf_free(&line);
    return rc;
}

int
snj_render_history(struct snj_render *render, const struct snj_session *session)
{
    struct snj_buf line;
    int rc = 0;

    if (!session->last_user && !session->last_assistant)
        return 0;
    snj_buf_init(&line, 4u * 1024u * 1024u);
    if (snj_buf_append(&line, "--- recent history ---\n", 23u) < 0 ||
        (session->last_user &&
         (snj_buf_append(&line, "user: ", 6u) < 0 ||
          snj_buf_append(&line, session->last_user, strlen(session->last_user)) < 0 ||
          snj_buf_putc(&line, '\n') < 0)))
        rc = -1;
    else if (line.len)
        rc = write_block(render, STDERR_FILENO, (char *)line.data, line.len,
                         render->stderr_terminal, true);
    if (rc == 0 && session->last_assistant) {
        if (snj_render_public_begin(render, STDERR_FILENO, "assistant: ") < 0) {
            rc = -1;
        } else if (snj_render_public(render, session->last_assistant,
                                     strlen(session->last_assistant), NULL) < 0) {
            int saved_errno = errno;
            (void)snj_render_public_abort(render);
            errno = saved_errno;
            rc = -1;
        } else {
            rc = snj_render_public_end(render);
        }
    }
    if (rc == 0)
        rc = write_block(render, STDERR_FILENO, "--- end history ---\n", 20u,
                         false, true);
    snj_buf_free(&line);
    return rc;
}

int
snj_render_prompt(struct snj_render *render, const char *label)
{
    return write_role_block(render, STDERR_FILENO,
                            render->networked ? COLOR_OPERATOR : COLOR_AGENT,
                            label, strlen(label), strlen(label), false, false);
}

static int
render_submitted(struct snj_render *render, const char *label, const char *text,
                 bool separate)
{
    struct snj_buf line;
    int rc;

    if (render->term &&
        snj_term_consume_echoed_submission(render->term, label)) {
        rc = separate && render->stderr_terminal ?
             write_block(render, STDERR_FILENO, "\n", 1u, false, true) : 0;
        if (rc == 0 && render->stderr_terminal) {
            render->previous_public_item = false;
            if (render->public_item_open) {
                render->public_item_ended_lf = true;
                render->public_trailing_newlines = separate ? 2u : 1u;
            }
        }
        return rc;
    }
    snj_buf_init(&line, SNJ_MAX_DIRECT_PROMPT * 8u + 64u);
    if (render->public_item_open && !render->public_item_ended_lf) {
        if (snj_buf_putc(&line, '\n') < 0) {
            snj_buf_free(&line);
            return -1;
        }
        render->public_item_ended_lf = true;
    }
    rc = snj_buf_append(&line, label, strlen(label));
    if (rc == 0)
        rc = snj_buf_append(&line, text, strlen(text));
    if (rc == 0 && separate && render->stderr_terminal) {
        size_t len = strlen(text);
        size_t trailing = 0u;

        while (trailing < len && trailing < 2u &&
               text[len - trailing - 1u] == '\n')
            ++trailing;
        while (trailing++ < 2u)
            if (snj_buf_putc(&line, '\n') < 0) {
                rc = -1;
                break;
            }
    } else if (rc == 0) {
        rc = snj_buf_putc(&line, '\n');
    }
    if (rc == 0)
        rc = write_role_block(render, STDERR_FILENO,
                              render->networked ? COLOR_OPERATOR : COLOR_AGENT,
                              (char *)line.data, line.len, strlen(label),
                              render->stderr_terminal, true);
    if (rc == 0 && render->stderr_terminal) {
        render->previous_public_item = false;
        if (render->public_item_open) {
            render->public_item_ended_lf = true;
            render->public_trailing_newlines = separate ? 2u : 1u;
        }
    }
    snj_buf_free(&line);
    return rc;
}

int
snj_render_submitted(struct snj_render *render, const char *label,
                     const char *text)
{
    return render_submitted(render, label, text, false);
}

int
snj_render_input_submitted(struct snj_render *render, const char *label,
                           const char *text)
{
    return render_submitted(render, label, text, true);
}

int
snj_render_before_prompt(struct snj_render *render)
{
    static const char newlines[] = "\n\n";
    size_t count;

    if (!render->previous_public_item)
        return 0;
    if (!render->stderr_terminal) {
        render->previous_public_item = false;
        return 0;
    }
    count = render->previous_public_newlines < 2u ?
            2u - render->previous_public_newlines : 0u;
    if (count && write_block(render, STDERR_FILENO, newlines, count,
                             false, true) < 0)
        return -1;
    render->previous_public_item = false;
    return 0;
}

static size_t
utf8_sequence_size(unsigned char first)
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

static int public_write(struct snj_render *render, const char *text, size_t len);
static int close_public_output(struct snj_render *render);
static int markdown_finish(struct snj_render *render);
static int markdown_abort(struct snj_render *render);
static int markdown_write(struct snj_render *render,
                          const unsigned char *text, size_t len);
static int markdown_table_finish(struct snj_render *render);
static bool public_terminal(const struct snj_render *render);

static bool
markdown_has_style(const struct snj_render *render)
{
    const struct snj_markdown_state *md = &render->markdown_state;

    return md->heading || md->quote || md->strong || md->emphasis ||
           md->strike || md->inline_code || md->table_header ||
           md->fence != '\0' || md->link_url;
}

static int
markdown_paint_style(struct snj_render *render)
{
    const struct snj_markdown_state *md = &render->markdown_state;
    char sequence[64];
    size_t len = 3u;

    if (!render->markdown_rendering || md->style_painted ||
        !color_enabled(render, render->public_fd) || !markdown_has_style(render))
        return 0;
    memcpy(sequence, "\033[0", 3u);
#define ADD_STYLE(value) do { \
        size_t amount = strlen(value); \
        if (len > sizeof(sequence) - amount - 1u) { errno = EOVERFLOW; return -1; } \
        memcpy(sequence + len, value, amount); \
        len += amount; \
    } while (0)
    if (md->heading) ADD_STYLE(";1;36");
    if (md->quote) ADD_STYLE(";34");
    if ((md->strong && !md->heading) || md->table_header) ADD_STYLE(";1");
    if (md->emphasis) ADD_STYLE(";3");
    if (md->strike) ADD_STYLE(";2");
    if (md->inline_code || md->fence) ADD_STYLE(";33");
    if (md->link_url) ADD_STYLE(";4;34");
#undef ADD_STYLE
    sequence[len++] = 'm';
    if (snj_write_full(render->public_fd, sequence, len) < 0)
        return -1;
    render->markdown_state.style_painted = true;
    return 0;
}

static int
markdown_clear_style(struct snj_render *render)
{
    if (!render->markdown_state.style_painted)
        return 0;
    render->markdown_state.style_painted = false;
    return write_literal(render->public_fd, COLOR_RESET);
}

int
snj_render_public_begin(struct snj_render *render, int fd, const char *label)
{
    size_t label_len = label ? strlen(label) : 0u;
    bool terminal;

    if (render->public_item_open || render->utf8_pending_len ||
        render->wrap_pending.data) {
        errno = EBUSY;
        return -1;
    }
    if (fd != STDOUT_FILENO && fd != STDERR_FILENO) {
        errno = EINVAL;
        return -1;
    }
    terminal = fd == STDOUT_FILENO ? render->stdout_terminal :
                                     render->stderr_terminal;
    if (render->markdown && terminal && render->previous_public_item &&
        render->previous_public_markdown && render->previous_public_fd == fd &&
        render->previous_public_newlines < 2u) {
        static const char newlines[] = "\n\n";
        size_t count = 2u - render->previous_public_newlines;

        if (write_block(render, fd, newlines, count, false, true) < 0)
            return -1;
    }
    render->previous_public_item = false;
    if (fd == STDOUT_FILENO && render->stdout_item_seen &&
        !render->stdout_item_ended_lf && !render->stdout_terminal &&
        write_literal(STDOUT_FILENO, "\n") < 0)
        return -1;
    snj_buf_init(&render->wrap_pending, SNJ_MAX_PUBLIC_ITEM);
    render->public_fd = fd;
    render->public_item_open = true;
    render->public_item_bytes = label_len != 0u;
    render->public_item_ended_lf = label_len && label[label_len - 1u] == '\n';
    render->public_trailing_newlines = 0u;
    render->public_column = label_len ? snj_term_text_width(label, label_len) : 0u;
    render->wrap_has_word = false;
    render->wrap_continuation = false;
    render->wrap_word_open = false;
    render->wrap_break_open = false;
    memset(&render->markdown_state, 0, sizeof(render->markdown_state));
    snj_buf_init(&render->markdown_state.table, SNJ_MAX_PUBLIC_ITEM);
    if (render->public_column == SIZE_MAX)
        goto fail;
    if (label_len) {
        bool colored = color_enabled(render, fd);
        const char *color = strncmp(label, "reason", 6u) == 0 ?
                            COLOR_ACTIVITY : COLOR_AGENT;
        if (colored) {
            if (output_begin(render, true) < 0)
                goto fail;
            render->public_output_open = true;
            if (write_literal(fd, color) < 0)
                goto fail;
        }
        if (public_write(render, label, label_len) < 0)
            goto fail;
        if (colored && write_literal(fd, COLOR_RESET) < 0)
            goto fail;
        if (close_public_output(render) < 0)
            goto fail;
    }
    render->markdown_rendering = render->markdown && public_terminal(render);
    render->markdown_state.line_start = true;
    return 0;
fail:
    {
        int saved_errno = errno;
        if (color_enabled(render, fd))
            (void)write_literal(fd, COLOR_RESET);
        if (render->public_output_open)
            (void)output_end(render);
        render->public_output_open = false;
        render->public_item_open = false;
        render->public_item_bytes = false;
        render->public_item_ended_lf = false;
        render->public_trailing_newlines = 0u;
        render->markdown_rendering = false;
        render->public_fd = -1;
        snj_buf_free(&render->markdown_state.table);
        snj_buf_free(&render->wrap_pending);
        errno = saved_errno;
        return -1;
    }
}

static bool
public_terminal(const struct snj_render *render)
{
    return render->public_fd == STDOUT_FILENO ? render->stdout_terminal :
                                                render->stderr_terminal;
}

static int
public_write(struct snj_render *render, const char *text, size_t len)
{
    bool terminal = public_terminal(render);
    size_t trailing = 0u;

    if (!len)
        return 0;
    if (render->markdown_measuring)
        return 0;
    if (!render->public_output_open) {
        if (output_begin(render, true) < 0)
            return -1;
        render->public_output_open = true;
        if (markdown_paint_style(render) < 0)
            return -1;
    }
    if ((terminal ? snj_term_write_safe(render->public_fd, text, len) :
                    snj_write_full(render->public_fd, text, len)) < 0)
        return -1;
    if (terminal && render->term)
        snj_term_note_output(render->term, text, len);
    render->public_item_bytes = true;
    render->public_item_ended_lf = text[len - 1u] == '\n';
    while (trailing < len && trailing < 2u &&
           text[len - trailing - 1u] == '\n')
        ++trailing;
    if (trailing == len && trailing < 2u) {
        unsigned int prior = render->public_trailing_newlines;
        unsigned int room = 2u - (unsigned int)trailing;

        trailing += prior < room ? prior : room;
    }
    render->public_trailing_newlines = (unsigned int)trailing;
    return 0;
}

static int
close_public_output(struct snj_render *render)
{
    int rc = 0;
    int saved_errno = 0;

    if (!render->public_output_open)
        return 0;
    if (markdown_clear_style(render) < 0) {
        rc = -1;
        saved_errno = errno;
    }
    render->public_output_open = false;
    if (output_end(render) < 0 && rc == 0)
        rc = -1;
    if (saved_errno)
        errno = saved_errno;
    return rc;
}

static int
flush_wrap_pending(struct snj_render *render)
{
    const char *text = (const char *)render->wrap_pending.data;
    size_t len = render->wrap_pending.len;
    size_t leading = 0u;
    size_t width;
    unsigned int columns;
    bool prompt_interposed;
    bool continued_line;

    if (!len)
        return 0;
    while (leading < len && (text[leading] == ' ' || text[leading] == '\t'))
        ++leading;
    prompt_interposed = !render->public_output_open && render->term &&
                        snj_term_typing_active(render->term);
    if (prompt_interposed) {
        continued_line = render->public_column != 0u;
        render->public_column = 0u;
        if (continued_line) {
            text += leading;
            len -= leading;
        }
        if (!len) {
            snj_buf_reset(&render->wrap_pending);
            render->wrap_has_word = false;
            render->wrap_continuation = false;
            return 0;
        }
    }
    width = snj_term_text_width(text, len);
    columns = render->markdown_measuring ? UINT_MAX :
                                           snj_term_columns(render->term);
    if (width == SIZE_MAX)
        return -1;
    if (columns >= 20u && render->public_column == columns && leading == len) {
        snj_buf_reset(&render->wrap_pending);
        render->wrap_has_word = false;
        render->wrap_continuation = false;
        return 0;
    }
    if (render->wrap_has_word && !render->wrap_continuation &&
        columns >= 20u && render->public_column != 0u &&
        (width >= columns || render->public_column > columns - width)) {
        if (public_write(render, "\n", 1u) < 0)
            return -1;
        render->public_column = 0u;
        text += leading;
        len -= leading;
        if (render->markdown_rendering && render->markdown_state.prose) {
            if (public_write(render, "  ", 2u) < 0)
                return -1;
            render->public_column = 2u;
        }
        width = snj_term_text_width(text, len);
        if (width == SIZE_MAX)
            return -1;
    }
    if (public_write(render, text, len) < 0)
        return -1;
    if (render->public_column > SIZE_MAX - width) {
        errno = EOVERFLOW;
        return -1;
    }
    if (columns >= 20u) {
        size_t total = render->public_column + width;

        render->public_column = total > columns ? total % columns : total;
        if (total > columns && render->public_column == 0u)
            render->public_column = columns;
    } else {
        render->public_column += width;
    }
    snj_buf_reset(&render->wrap_pending);
    render->wrap_has_word = false;
    render->wrap_continuation = false;
    return 0;
}

static bool
wrap_break_after(const unsigned char *text, size_t len)
{
    if (len == 1u)
        return text[0] != '\0' &&
               strchr("\"-,.;:!?/)]}", (int)text[0]) != NULL;
    if (len != 3u)
        return false;
    return memcmp(text, "\xe2\x80\x90", 3u) == 0 ||
           memcmp(text, "\xe2\x80\x92", 3u) == 0 ||
           memcmp(text, "\xe2\x80\x93", 3u) == 0 ||
           memcmp(text, "\xe2\x80\x94", 3u) == 0 ||
           memcmp(text, "\xe2\x80\xa6", 3u) == 0;
}

static int
write_wrapped(struct snj_render *render, const unsigned char *text, size_t len)
{
    size_t i = 0u;

    while (i < len) {
        size_t n = utf8_sequence_size(text[i]);

        if (!n || n > len - i) {
            errno = EILSEQ;
            return -1;
        }
        if (text[i] == '\n') {
            if (flush_wrap_pending(render) < 0 ||
                public_write(render, "\n", 1u) < 0)
                return -1;
            render->public_column = 0u;
            render->wrap_word_open = false;
            render->wrap_break_open = false;
        } else {
            bool space = text[i] == ' ' || text[i] == '\t';
            bool punctuation = !space && wrap_break_after(text + i, n);
            bool break_after = punctuation &&
                               (render->wrap_word_open ||
                                render->wrap_break_open);

            if (space && render->wrap_has_word &&
                flush_wrap_pending(render) < 0)
                return -1;
            if (!space && !render->wrap_has_word) {
                render->wrap_continuation =
                    (render->wrap_word_open || break_after) &&
                    render->wrap_pending.len == 0u;
                render->wrap_has_word = true;
            }
            if (snj_buf_append(&render->wrap_pending, text + i, n) < 0)
                return -1;
            if (break_after) {
                if (flush_wrap_pending(render) < 0)
                    return -1;
                render->wrap_word_open = false;
                render->wrap_break_open = true;
            } else {
                render->wrap_word_open = !space;
                render->wrap_break_open = false;
            }
        }
        i += n;
    }
    return flush_wrap_pending(render);
}

static bool
markdown_word(const unsigned char *text, size_t len)
{
    return len != 1u || (text[0] >= '0' && text[0] <= '9') ||
           (text[0] >= 'A' && text[0] <= 'Z') ||
           (text[0] >= 'a' && text[0] <= 'z') || text[0] == '_';
}

static int
markdown_text(struct snj_render *render, const void *text, size_t len)
{
    const unsigned char *bytes = text;
    size_t last;

    if (!len)
        return 0;
    if (write_wrapped(render, bytes, len) < 0)
        return -1;
    last = len - 1u;
    while (last && (bytes[last] & 0xc0u) == 0x80u)
        --last;
    render->markdown_state.previous_word =
        markdown_word(bytes + last, len - last);
    return 0;
}

static int
markdown_repeat(struct snj_render *render, char value, size_t count)
{
    char text[16];

    memset(text, value, sizeof(text));
    while (count) {
        size_t amount = count < sizeof(text) ? count : sizeof(text);
        if (markdown_text(render, text, amount) < 0)
            return -1;
        count -= amount;
    }
    return 0;
}

static int
markdown_style_changed(struct snj_render *render)
{
    if (flush_wrap_pending(render) < 0 || markdown_clear_style(render) < 0)
        return -1;
    return render->public_output_open ? markdown_paint_style(render) : 0;
}

static int
markdown_flush_delimiter(struct snj_render *render, bool next_word,
                         bool at_end)
{
    struct snj_markdown_state *md = &render->markdown_state;
    char delimiter = md->delimiter;
    size_t count = md->delimiter_len;

    if (!count)
        return 0;
    md->delimiter = '\0';
    md->delimiter_len = 0u;
    if (md->inline_code && delimiter != '`')
        return markdown_repeat(render, delimiter, count);
    if (delimiter == '`') {
        if (!md->inline_code) {
            if (at_end)
                return markdown_repeat(render, delimiter, count);
            md->inline_code = true;
            md->code_ticks = (unsigned int)count;
        } else if (count == md->code_ticks) {
            md->inline_code = false;
            md->code_ticks = 0u;
        } else {
            return markdown_repeat(render, delimiter, count);
        }
        return markdown_style_changed(render);
    }
    if (delimiter == '~') {
        if (at_end && (!md->strike || !((count / 2u) & 1u)))
            return markdown_repeat(render, delimiter, count);
        if (count & 1u && markdown_repeat(render, '~', 1u) < 0)
            return -1;
        if ((count / 2u) & 1u) {
            md->strike = !md->strike;
            return markdown_style_changed(render);
        }
        return 0;
    }
    if (delimiter == '_' && md->delimiter_previous_word && next_word)
        return markdown_repeat(render, delimiter, count);
    if (at_end && (((count / 2u) & 1u && !md->strong) ||
                   (count & 1u && !md->emphasis)))
        return markdown_repeat(render, delimiter, count);
    if ((count / 2u) & 1u)
        md->strong = !md->strong;
    if (count & 1u)
        md->emphasis = !md->emphasis;
    return markdown_style_changed(render);
}

static bool
markdown_escapable(unsigned char value)
{
    return strchr("\\`*{}[]()#+-.!_>~|", (int)value) != NULL;
}

static int
markdown_inline(struct snj_render *render, const unsigned char *text, size_t len)
{
    struct snj_markdown_state *md = &render->markdown_state;
    size_t i = 0u;

    while (i < len) {
        size_t n = utf8_sequence_size(text[i]);
        bool word;

        if (!n || n > len - i) {
            errno = EILSEQ;
            return -1;
        }
        word = markdown_word(text + i, n);
        if (md->escape) {
            md->escape = false;
            if (!(n == 1u && markdown_escapable(text[i])) &&
                markdown_text(render, "\\", 1u) < 0)
                return -1;
            if (markdown_text(render, text + i, n) < 0)
                return -1;
            i += n;
            continue;
        }
        if (md->link_after_label) {
            md->link_after_label = false;
            if (n == 1u && text[i] == '(') {
                if (markdown_text(render, " <", 2u) < 0)
                    return -1;
                md->link_url = true;
                if (markdown_style_changed(render) < 0)
                    return -1;
                i += n;
                continue;
            }
        }
        if (md->link_url) {
            if (n == 1u && text[i] == ')') {
                md->link_url = false;
                if (markdown_style_changed(render) < 0 ||
                    markdown_text(render, ">", 1u) < 0)
                    return -1;
            } else if (markdown_text(render, text + i, n) < 0) {
                return -1;
            }
            i += n;
            continue;
        }
        if (md->delimiter_len &&
            !(n == 1u && text[i] == (unsigned char)md->delimiter) &&
            markdown_flush_delimiter(render, word, false) < 0)
            return -1;
        if (n == 1u && text[i] == '\\' && !md->inline_code) {
            md->escape = true;
            i += n;
            continue;
        }
        if (n == 1u && (text[i] == '`' ||
            (!md->inline_code && (text[i] == '*' || text[i] == '_' ||
                                  text[i] == '~')))) {
            if (!md->delimiter_len) {
                md->delimiter = (char)text[i];
                md->delimiter_previous_word = md->previous_word;
            }
            ++md->delimiter_len;
            i += n;
            continue;
        }
        if (!md->inline_code && n == 1u && text[i] == ']') {
            if (markdown_text(render, text + i, n) < 0)
                return -1;
            md->link_after_label = true;
            i += n;
            continue;
        }
        {
            size_t start = i;
            do {
                i += n;
                if (i >= len)
                    break;
                n = utf8_sequence_size(text[i]);
                if (!n || n > len - i)
                    break;
            } while (!(n == 1u &&
                       (text[i] == '\\' || text[i] == '`' || text[i] == ']' ||
                        (!md->inline_code && (text[i] == '*' || text[i] == '_' ||
                                              text[i] == '~')))));
            if (markdown_text(render, text + start, i - start) < 0)
                return -1;
        }
    }
    return 0;
}

static void
markdown_table_trim(struct markdown_table_cell *cell)
{
    while (cell->len && (cell->text[0] == ' ' || cell->text[0] == '\t')) {
        ++cell->text;
        --cell->len;
    }
    while (cell->len && (cell->text[cell->len - 1u] == ' ' ||
                         cell->text[cell->len - 1u] == '\t'))
        --cell->len;
}

static bool
markdown_table_cells(const unsigned char *text, size_t len,
                     struct markdown_table_cell cells[MARKDOWN_TABLE_COLUMNS],
                     size_t *cell_count)
{
    size_t first = 0u;
    size_t start;
    size_t end = len;
    size_t count = 0u;
    unsigned int code_ticks = 0u;
    bool escape = false;

    while (first < len && first < 3u && text[first] == ' ')
        ++first;
    if (first >= len || text[first] != '|')
        return false;
    while (end > first && (text[end - 1u] == ' ' || text[end - 1u] == '\t'))
        --end;
    start = first + 1u;
    for (size_t i = start; i < end;) {
        if (escape) {
            escape = false;
            ++i;
            continue;
        }
        if (!code_ticks && text[i] == '\\') {
            escape = true;
            ++i;
            continue;
        }
        if (text[i] == '`') {
            size_t ticks = 1u;

            while (i + ticks < len && text[i + ticks] == '`')
                ++ticks;
            if (!code_ticks)
                code_ticks = ticks > UINT_MAX ? UINT_MAX :
                                                   (unsigned int)ticks;
            else if (ticks == code_ticks)
                code_ticks = 0u;
            i += ticks;
            continue;
        }
        if (!code_ticks && text[i] == '|') {
            if (count == MARKDOWN_TABLE_COLUMNS)
                return false;
            cells[count].text = text + start;
            cells[count].len = i - start;
            markdown_table_trim(&cells[count]);
            ++count;
            start = i + 1u;
        }
        ++i;
    }
    if (end == first + 1u || text[end - 1u] != '|') {
        if (count == MARKDOWN_TABLE_COLUMNS)
            return false;
        cells[count].text = text + start;
        cells[count].len = end - start;
        markdown_table_trim(&cells[count]);
        ++count;
    }
    if (!count)
        return false;
    *cell_count = count;
    return true;
}

static bool
markdown_table_delimiter(const struct markdown_table_cell *cells,
                         size_t count,
                         enum markdown_table_alignment *alignment)
{
    for (size_t i = 0u; i < count; ++i) {
        const unsigned char *text = cells[i].text;
        size_t begin = 0u;
        size_t end = cells[i].len;
        bool left;
        bool right;

        left = begin < end && text[begin] == ':';
        if (left)
            ++begin;
        right = begin < end && text[end - 1u] == ':';
        if (right)
            --end;
        if (end - begin < 3u)
            return false;
        for (size_t j = begin; j < end; ++j)
            if (text[j] != '-')
                return false;
        alignment[i] = left && right ? TABLE_CENTER :
                       right ? TABLE_RIGHT : TABLE_LEFT;
    }
    return true;
}

static int
markdown_inline_finish(struct snj_render *render)
{
    struct snj_markdown_state *md = &render->markdown_state;

    if (markdown_flush_delimiter(render, false, true) < 0)
        return -1;
    if (md->escape && markdown_text(render, "\\", 1u) < 0)
        return -1;
    md->escape = false;
    md->link_after_label = false;
    if (md->link_url) {
        md->link_url = false;
        if (markdown_style_changed(render) < 0 ||
            markdown_text(render, ">", 1u) < 0)
            return -1;
    }
    md->strong = false;
    md->emphasis = false;
    md->strike = false;
    md->inline_code = false;
    md->code_ticks = 0u;
    md->previous_word = false;
    return markdown_style_changed(render);
}

static int
markdown_table_cell_width(struct snj_render *render,
                          const struct markdown_table_cell *cell,
                          size_t *width)
{
    struct snj_render probe;
    int rc;

    memset(&probe, 0, sizeof(probe));
    probe.public_fd = render->public_fd;
    probe.markdown_rendering = true;
    probe.markdown_measuring = true;
    snj_buf_init(&probe.wrap_pending, SNJ_MAX_PUBLIC_ITEM);
    rc = markdown_inline(&probe, cell->text, cell->len);
    if (rc == 0)
        rc = markdown_inline_finish(&probe);
    if (rc == 0)
        rc = flush_wrap_pending(&probe);
    if (rc == 0)
        *width = probe.public_column;
    snj_buf_free(&probe.wrap_pending);
    return rc;
}

static int
markdown_table_spaces(struct snj_render *render, size_t count)
{
    static const char spaces[] = "                ";

    while (count) {
        size_t amount = count < sizeof(spaces) - 1u ? count :
                                                        sizeof(spaces) - 1u;
        if (markdown_text(render, spaces, amount) < 0)
            return -1;
        count -= amount;
    }
    return 0;
}

static int
markdown_table_rule(struct snj_render *render, const char *left,
                    const char *middle, const char *right,
                    const size_t *widths, size_t count, bool newline)
{
    if (markdown_text(render, left, strlen(left)) < 0)
        return -1;
    for (size_t i = 0u; i < count; ++i) {
        for (size_t j = 0u; j < widths[i] + 2u; ++j)
            if (markdown_text(render, "─", strlen("─")) < 0)
                return -1;
        if (i + 1u < count &&
            markdown_text(render, middle, strlen(middle)) < 0)
            return -1;
    }
    if (markdown_text(render, right, strlen(right)) < 0 ||
        (newline && markdown_text(render, "\n", 1u) < 0))
        return -1;
    return 0;
}

static int
markdown_table_cell(struct snj_render *render,
                    const struct markdown_table_cell *cell, bool strong)
{
    render->markdown_state.previous_word = false;
    render->markdown_state.table_header = strong;
    if (markdown_style_changed(render) < 0)
        return -1;
    if (markdown_inline(render, cell->text, cell->len) < 0)
        return -1;
    if (markdown_inline_finish(render) < 0)
        return -1;
    render->markdown_state.table_header = false;
    return markdown_style_changed(render);
}

static int
markdown_table_grid_row(struct snj_render *render,
                        const struct markdown_table_cell *cells,
                        size_t cell_count, const size_t *widths,
                        const enum markdown_table_alignment *alignment,
                        size_t columns, bool header)
{
    if (markdown_text(render, "│ ", strlen("│ ")) < 0)
        return -1;
    for (size_t i = 0u; i < columns; ++i) {
        struct markdown_table_cell empty = {0};
        const struct markdown_table_cell *cell = i < cell_count ?
                                                  &cells[i] : &empty;
        size_t width;
        size_t before;
        size_t after;

        if (markdown_table_cell_width(render, cell, &width) < 0 ||
            width > widths[i]) {
            errno = EINVAL;
            return -1;
        }
        if (alignment[i] == TABLE_RIGHT) {
            before = widths[i] - width;
        } else if (alignment[i] == TABLE_CENTER) {
            before = (widths[i] - width) / 2u;
        } else {
            before = 0u;
        }
        after = widths[i] - width - before;
        if (markdown_table_spaces(render, before) < 0 ||
            markdown_table_cell(render, cell, header) < 0 ||
            markdown_table_spaces(render, after) < 0 ||
            markdown_text(render, i + 1u < columns ? " │ " : " │\n",
                          i + 1u < columns ? strlen(" │ ") :
                                             strlen(" │\n")) < 0)
            return -1;
    }
    return 0;
}

static bool
markdown_table_next_line(const unsigned char *text, size_t len,
                         size_t *offset, const unsigned char **line,
                         size_t *line_len)
{
    size_t start;
    size_t end;

    if (*offset >= len)
        return false;
    start = *offset;
    end = start;
    while (end < len && text[end] != '\n')
        ++end;
    *line = text + start;
    *line_len = end - start;
    *offset = end < len ? end + 1u : end;
    return true;
}

static int
markdown_table_render(struct snj_render *render)
{
    struct snj_markdown_state *md = &render->markdown_state;
    const unsigned char *text = md->table.data;
    struct markdown_table_cell header[MARKDOWN_TABLE_COLUMNS];
    struct markdown_table_cell delimiter[MARKDOWN_TABLE_COLUMNS];
    enum markdown_table_alignment alignment[MARKDOWN_TABLE_COLUMNS];
    size_t widths[MARKDOWN_TABLE_COLUMNS] = {0};
    const unsigned char *line;
    size_t line_len;
    size_t header_count;
    size_t delimiter_count;
    size_t offset = 0u;
    size_t body_offset;
    size_t total;
    unsigned int terminal_columns = snj_term_columns(render->term);
    bool ended_lf = md->table.len && text[md->table.len - 1u] == '\n';
    bool grid;

    if (!markdown_table_next_line(text, md->table.len, &offset, &line,
                                  &line_len) ||
        !markdown_table_cells(line, line_len, header, &header_count) ||
        !markdown_table_next_line(text, md->table.len, &offset, &line,
                                  &line_len) ||
        !markdown_table_cells(line, line_len, delimiter, &delimiter_count) ||
        delimiter_count != header_count ||
        !markdown_table_delimiter(delimiter, delimiter_count, alignment)) {
        errno = EINVAL;
        return -1;
    }
    for (size_t i = 0u; i < header_count; ++i) {
        if (markdown_table_cell_width(render, &header[i], &widths[i]) < 0)
            return -1;
        if (!widths[i])
            widths[i] = 1u;
    }
    body_offset = offset;
    while (markdown_table_next_line(text, md->table.len, &offset, &line,
                                    &line_len)) {
        struct markdown_table_cell cells[MARKDOWN_TABLE_COLUMNS];
        size_t count;

        if (!markdown_table_cells(line, line_len, cells, &count)) {
            errno = EINVAL;
            return -1;
        }
        for (size_t i = 0u; i < count && i < header_count; ++i) {
            size_t width;

            if (markdown_table_cell_width(render, &cells[i], &width) < 0)
                return -1;
            if (width > widths[i])
                widths[i] = width;
        }
    }
    total = 1u;
    for (size_t i = 0u; i < header_count; ++i) {
        if (widths[i] > SIZE_MAX - total - 3u) {
            errno = EOVERFLOW;
            return -1;
        }
        total += widths[i] + 3u;
    }
    grid = terminal_columns >= 10u && total < terminal_columns;
    if (render->public_column) {
        if (markdown_text(render, "\n", 1u) < 0)
            return -1;
        render->public_column = 0u;
    }
    md->prose = false;
    if (grid) {
        if (markdown_table_rule(render, "┌", "┬", "┐", widths,
                                header_count, true) < 0 ||
            markdown_table_grid_row(render, header, header_count, widths,
                                    alignment, header_count, true) < 0 ||
            markdown_table_rule(render, "├", "┼", "┤", widths,
                                header_count, true) < 0)
            return -1;
        offset = body_offset;
        while (markdown_table_next_line(text, md->table.len, &offset, &line,
                                        &line_len)) {
            struct markdown_table_cell cells[MARKDOWN_TABLE_COLUMNS];
            size_t count;

            if (!markdown_table_cells(line, line_len, cells, &count) ||
                markdown_table_grid_row(render, cells, count, widths,
                                        alignment, header_count, false) < 0)
                return -1;
        }
        if (markdown_table_rule(render, "└", "┴", "┘", widths,
                                header_count, ended_lf) < 0)
            return -1;
    } else {
        size_t row = 0u;

        if (markdown_text(render, "┌─ table\n", strlen("┌─ table\n")) < 0)
            return -1;
        offset = body_offset;
        while (markdown_table_next_line(text, md->table.len, &offset, &line,
                                        &line_len)) {
            struct markdown_table_cell cells[MARKDOWN_TABLE_COLUMNS];
            size_t count;

            if (!markdown_table_cells(line, line_len, cells, &count) ||
                markdown_text(render, "├─ row\n", strlen("├─ row\n")) < 0)
                return -1;
            for (size_t i = 0u; i < header_count; ++i) {
                struct markdown_table_cell empty = {0};
                const struct markdown_table_cell *cell = i < count ?
                                                          &cells[i] : &empty;

                if (markdown_text(render, "│ ", strlen("│ ")) < 0 ||
                    markdown_table_cell(render, &header[i], true) < 0 ||
                    markdown_text(render, ": ", 2u) < 0 ||
                    markdown_table_cell(render, cell, false) < 0 ||
                    markdown_text(render, "\n", 1u) < 0)
                    return -1;
            }
            ++row;
        }
        if (!row) {
            for (size_t i = 0u; i < header_count; ++i)
                if (markdown_text(render, "│ ", strlen("│ ")) < 0 ||
                    markdown_table_cell(render, &header[i], true) < 0 ||
                    markdown_text(render, "\n", 1u) < 0)
                    return -1;
        }
        if (markdown_text(render, ended_lf ? "└─\n" : "└─",
                          ended_lf ? strlen("└─\n") : strlen("└─")) < 0)
            return -1;
    }
    md->line_start = ended_lf;
    md->previous_word = false;
    return 0;
}

static void
markdown_table_reset(struct snj_markdown_state *md)
{
    snj_buf_reset(&md->table);
    md->table_header_len = 0u;
    md->table_line_start = 0u;
    md->table_line = false;
    md->table_pending = false;
    md->table_active = false;
}

static int
markdown_table_replay(struct snj_render *render)
{
    struct snj_markdown_state *md = &render->markdown_state;
    struct snj_buf saved = md->table;
    int rc;

    snj_buf_init(&md->table, SNJ_MAX_PUBLIC_ITEM);
    md->table_header_len = 0u;
    md->table_line_start = 0u;
    md->table_line = false;
    md->table_pending = false;
    md->table_active = false;
    md->line_start = true;
    md->table_disabled = true;
    rc = markdown_write(render, saved.data, saved.len);
    md->table_disabled = false;
    snj_buf_free(&saved);
    return rc;
}

static int
markdown_table_finish(struct snj_render *render)
{
    struct snj_markdown_state *md = &render->markdown_state;
    int rc;

    if (md->table_line) {
        struct markdown_table_cell cells[MARKDOWN_TABLE_COLUMNS];
        size_t count;

        md->table_line = false;
        if (!markdown_table_cells(md->table.data + md->table_line_start,
                                  md->table.len - md->table_line_start,
                                  cells, &count)) {
            if (md->table_active) {
                struct snj_buf row;

                snj_buf_init(&row, SNJ_MAX_PUBLIC_ITEM);
                if (snj_buf_append(&row,
                                   md->table.data + md->table_line_start,
                                   md->table.len - md->table_line_start) < 0) {
                    snj_buf_free(&row);
                    return -1;
                }
                md->table.len = md->table_line_start;
                rc = markdown_table_render(render);
                markdown_table_reset(md);
                if (rc == 0) {
                    md->table_disabled = true;
                    rc = markdown_write(render, row.data, row.len);
                    md->table_disabled = false;
                }
                snj_buf_free(&row);
                return rc;
            }
            return markdown_table_replay(render);
        }
        if (!md->table_header_len) {
            md->table_header_len = md->table.len;
            md->table_pending = true;
        } else if (md->table_pending) {
            struct markdown_table_cell header[MARKDOWN_TABLE_COLUMNS];
            enum markdown_table_alignment alignment[MARKDOWN_TABLE_COLUMNS];
            size_t header_len = md->table_header_len;
            size_t header_count;

            if (header_len && md->table.data[header_len - 1u] == '\n')
                --header_len;
            if (!markdown_table_cells(md->table.data, header_len, header,
                                      &header_count) ||
                header_count != count ||
                !markdown_table_delimiter(cells, count, alignment))
                return markdown_table_replay(render);
            md->table_pending = false;
            md->table_active = true;
        }
    }
    if (md->table_active) {
        rc = markdown_table_render(render);
        markdown_table_reset(md);
        return rc;
    }
    if (md->table.len)
        return markdown_table_replay(render);
    return 0;
}

static int
markdown_table_line_end(struct snj_render *render)
{
    struct snj_markdown_state *md = &render->markdown_state;
    struct markdown_table_cell cells[MARKDOWN_TABLE_COLUMNS];
    size_t line_len = md->table.len - md->table_line_start;
    size_t count;

    if (line_len && md->table.data[md->table.len - 1u] == '\n')
        --line_len;
    md->table_line = false;
    md->line_start = true;
    if (!markdown_table_cells(md->table.data + md->table_line_start,
                              line_len, cells, &count)) {
        md->table_line = true;
        return markdown_table_finish(render);
    }
    if (!md->table_header_len) {
        md->table_header_len = md->table.len;
        md->table_pending = true;
        return 0;
    }
    if (md->table_pending) {
        struct markdown_table_cell header[MARKDOWN_TABLE_COLUMNS];
        enum markdown_table_alignment alignment[MARKDOWN_TABLE_COLUMNS];
        size_t header_len = md->table_header_len;
        size_t header_count;

        if (header_len && md->table.data[header_len - 1u] == '\n')
            --header_len;
        if (!markdown_table_cells(md->table.data, header_len, header,
                                  &header_count) ||
            header_count != count ||
            !markdown_table_delimiter(cells, count, alignment))
            return markdown_table_replay(render);
        md->table_pending = false;
        md->table_active = true;
    }
    return 0;
}

static int
markdown_table_start_line(struct snj_render *render)
{
    struct snj_markdown_state *md = &render->markdown_state;

    md->table_line_start = md->table.len;
    if (snj_buf_append(&md->table, md->prefix, md->prefix_len) < 0)
        return -1;
    md->prefix_len = 0u;
    md->table_line = true;
    md->line_start = false;
    return 0;
}

static size_t
markdown_prefix_spaces(const struct snj_markdown_state *md)
{
    size_t spaces = 0u;

    while (spaces < md->prefix_len && spaces < 3u &&
           md->prefix[spaces] == ' ')
        ++spaces;
    return spaces;
}

static int
markdown_prefix_literal(struct snj_render *render)
{
    struct snj_markdown_state *md = &render->markdown_state;
    size_t len = md->prefix_len;
    size_t spaces = markdown_prefix_spaces(md);
    bool prose = len != 0u && spaces != len;

    md->prefix_len = 0u;
    md->line_start = false;
    if (prose && !md->prose) {
        md->prose = true;
        if (markdown_text(render, "• ", strlen("• ")) < 0)
            return -1;
    }
    return markdown_inline(render, (const unsigned char *)md->prefix, len);
}

static int
markdown_code_prefix_literal(struct snj_render *render)
{
    struct snj_markdown_state *md = &render->markdown_state;
    size_t len = md->prefix_len;

    md->prefix_len = 0u;
    md->line_start = false;
    md->prose = false;
    if (markdown_text(render, "│ ", strlen("│ ")) < 0)
        return -1;
    return markdown_text(render, md->prefix, len);
}

static bool
markdown_fence_close(const struct snj_markdown_state *md)
{
    size_t spaces = markdown_prefix_spaces(md);
    size_t count = 0u;
    size_t i = spaces;

    while (i < md->prefix_len && md->prefix[i] == md->fence) {
        ++count;
        ++i;
    }
    while (i < md->prefix_len && md->prefix[i] == ' ')
        ++i;
    return count >= md->fence_len && i == md->prefix_len;
}

static int
markdown_open_fence(struct snj_render *render, bool newline)
{
    struct snj_markdown_state *md = &render->markdown_state;
    size_t begin = 0u;
    size_t end = md->fence_info_len;

    while (begin < end && md->fence_info[begin] == ' ')
        ++begin;
    while (end > begin && md->fence_info[end - 1u] == ' ')
        --end;
    md->fence_header = false;
    md->fence_info_len = 0u;
    if (markdown_style_changed(render) < 0 ||
        markdown_text(render, "┌─", strlen("┌─")) < 0 ||
        (end > begin &&
         (markdown_text(render, " ", 1u) < 0 ||
          markdown_text(render, md->fence_info + begin, end - begin) < 0)) ||
        (newline && markdown_text(render, "\n", 1u) < 0))
        return -1;
    md->line_start = newline;
    return 0;
}

static int
markdown_line_prefix(struct snj_render *render,
                     const unsigned char *text, size_t len)
{
    struct snj_markdown_state *md = &render->markdown_state;
    size_t spaces;
    const char *body;
    size_t body_len;

    if (len != 1u || md->prefix_len == sizeof(md->prefix)) {
        if (md->fence)
            return markdown_code_prefix_literal(render) < 0 ? -1 :
                   markdown_text(render, text, len);
        return markdown_prefix_literal(render) < 0 ? -1 :
               markdown_inline(render, text, len);
    }
    md->prefix[md->prefix_len++] = (char)text[0];
    spaces = markdown_prefix_spaces(md);
    body = md->prefix + spaces;
    body_len = md->prefix_len - spaces;
    if ((md->table_pending || md->table_active) &&
        !(body_len && body[0] == '|')) {
        size_t pending = md->prefix_len;
        char prefix[sizeof(md->prefix)];

        if (spaces == md->prefix_len && spaces <= 3u)
            return 0;
        memcpy(prefix, md->prefix, pending);
        md->prefix_len = 0u;
        if (markdown_table_finish(render) < 0)
            return -1;
        md->line_start = true;
        for (size_t i = 0u; i < pending; ++i)
            if (markdown_line_prefix(render,
                                     (const unsigned char *)prefix + i,
                                     1u) < 0)
                return -1;
        return 0;
    }
    if (spaces == md->prefix_len)
        return spaces <= 3u ? 0 : markdown_prefix_literal(render);
    if (!md->table_disabled && body[0] == '|')
        return markdown_table_start_line(render);
    if (md->fence) {
        size_t marks = 0u;
        while (marks < body_len && body[marks] == md->fence)
            ++marks;
        if (marks == body_len ||
            (marks >= md->fence_len && body[marks] == ' '))
            return 0;
        return markdown_code_prefix_literal(render);
    }
    if (body[0] == '#' && body_len >= 2u && body_len <= 7u &&
        body[body_len - 1u] == ' ') {
        for (size_t i = 0u; i + 1u < body_len; ++i)
            if (body[i] != '#')
                goto ordinary;
        md->prefix_len = 0u;
        md->line_start = false;
        md->prose = false;
        md->heading = true;
        return markdown_style_changed(render);
    }
    if (body[0] == '#' && body_len <= 6u) {
        for (size_t i = 0u; i < body_len; ++i)
            if (body[i] != '#')
                goto ordinary;
        return 0;
    }
    if ((body[0] == '`' || body[0] == '~')) {
        size_t marks = 0u;
        while (marks < body_len && body[marks] == body[0])
            ++marks;
        if (marks == body_len && marks < 3u)
            return 0;
        if (marks >= 3u) {
            md->fence = body[0];
            md->fence_len = (unsigned int)marks;
            md->fence_header = true;
            md->prefix_len = 0u;
            md->line_start = false;
            md->prose = false;
            return 0;
        }
    }
    if ((body[0] == '>' || body[0] == '-' || body[0] == '*' ||
         body[0] == '+') && body_len == 1u)
        return 0;
    if (body_len == 2u && body[1] == ' ' &&
        (body[0] == '>' || body[0] == '-' || body[0] == '*' ||
         body[0] == '+')) {
        const char *marker = body[0] == '>' ? "│ " : "• ";
        if (body[0] == '>')
            md->quote = true;
        md->prefix_len = 0u;
        md->line_start = false;
        md->prose = false;
        if (markdown_style_changed(render) < 0 ||
            markdown_text(render, md->prefix, spaces) < 0)
            return -1;
        return markdown_text(render, marker, strlen(marker));
    }
    if (body[0] >= '0' && body[0] <= '9') {
        size_t digits = 0u;
        while (digits < body_len && body[digits] >= '0' && body[digits] <= '9')
            ++digits;
        if (digits == body_len && digits <= 9u)
            return 0;
        if (digits && digits <= 9u && digits + 1u == body_len &&
            (body[digits] == '.' || body[digits] == ')'))
            return 0;
        if (digits && digits <= 9u && digits + 2u == body_len &&
            (body[digits] == '.' || body[digits] == ')') &&
            body[digits + 1u] == ' ') {
            size_t amount = md->prefix_len;
            md->prefix_len = 0u;
            md->line_start = false;
            md->prose = false;
            return markdown_text(render, md->prefix, amount);
        }
    }
ordinary:
    return markdown_prefix_literal(render);
}

static int
markdown_newline(struct snj_render *render)
{
    struct snj_markdown_state *md = &render->markdown_state;
    bool blank = md->line_start && !md->fence &&
                 markdown_prefix_spaces(md) == md->prefix_len;

    if (!md->fence && (md->table_pending || md->table_active)) {
        if (markdown_table_finish(render) < 0)
            return -1;
        md->line_start = true;
        md->prefix_len = 0u;
        return markdown_text(render, "\n", 1u);
    }
    if (blank)
        md->prose = false;

    if (md->fence_header)
        return markdown_open_fence(render, true);
    if (md->line_start) {
        if (md->fence && markdown_fence_close(md)) {
            md->prefix_len = 0u;
            if (markdown_text(render, "└─\n", strlen("└─\n")) < 0)
                return -1;
            md->fence = '\0';
            md->fence_len = 0u;
            return markdown_style_changed(render);
        }
        if ((md->fence ? markdown_code_prefix_literal(render) :
                         markdown_prefix_literal(render)) < 0)
            return -1;
    }
    if (markdown_flush_delimiter(render, false, true) < 0)
        return -1;
    if (md->escape && markdown_text(render, "\\", 1u) < 0)
        return -1;
    md->escape = false;
    md->link_after_label = false;
    if (md->link_url) {
        md->link_url = false;
        if (markdown_style_changed(render) < 0 ||
            markdown_text(render, ">", 1u) < 0)
            return -1;
    }
    if (markdown_text(render, "\n", 1u) < 0)
        return -1;
    md->heading = false;
    md->quote = false;
    md->line_start = true;
    md->prefix_len = 0u;
    return markdown_style_changed(render);
}

static int
markdown_write(struct snj_render *render, const unsigned char *text, size_t len)
{
    struct snj_markdown_state *md = &render->markdown_state;
    size_t i = 0u;

    while (i < len) {
        size_t n = utf8_sequence_size(text[i]);

        if (!n || n > len - i) {
            errno = EILSEQ;
            return -1;
        }
        if (text[i] == '\n') {
            if (md->table_line) {
                if (snj_buf_putc(&md->table, '\n') < 0 ||
                    markdown_table_line_end(render) < 0)
                    return -1;
            } else if (markdown_newline(render) < 0) {
                return -1;
            }
        } else if (md->table_line) {
            if (snj_buf_append(&md->table, text + i, n) < 0)
                return -1;
        } else if (md->fence_header) {
            if (n == 1u && text[i] == (unsigned char)md->fence &&
                md->fence_info_len == 0u) {
                if (md->fence_len == UINT_MAX) {
                    errno = EOVERFLOW;
                    return -1;
                }
                ++md->fence_len;
                i += n;
                continue;
            }
            if (md->fence_info_len < sizeof(md->fence_info)) {
                size_t room = sizeof(md->fence_info) - md->fence_info_len;
                size_t amount = n < room ? n : room;
                memcpy(md->fence_info + md->fence_info_len, text + i, amount);
                md->fence_info_len += amount;
            }
        } else if (md->line_start) {
            if (markdown_line_prefix(render, text + i, n) < 0)
                return -1;
        } else {
            size_t start = i;

            while (i + n < len && text[i + n] != '\n') {
                size_t next = utf8_sequence_size(text[i + n]);
                if (!next || next > len - i - n)
                    break;
                n += next;
            }
            if (md->fence ? markdown_text(render, text + start, n) < 0 :
                            markdown_inline(render, text + start, n) < 0)
                return -1;
            i += n;
            continue;
        }
        i += n;
    }
    return flush_wrap_pending(render);
}

static int
markdown_finish(struct snj_render *render)
{
    struct snj_markdown_state *md = &render->markdown_state;

    if (!render->markdown_rendering)
        return 0;
    if ((md->table_line || md->table_pending || md->table_active) &&
        markdown_table_finish(render) < 0)
        return -1;
    if (md->fence_header) {
        if (markdown_open_fence(render, false) < 0)
            return -1;
    } else if (md->line_start && md->prefix_len) {
        if (md->fence && markdown_fence_close(md)) {
            md->prefix_len = 0u;
            if (markdown_text(render, "└─", strlen("└─")) < 0)
                return -1;
            md->fence = '\0';
            md->fence_len = 0u;
        } else if ((md->fence ? markdown_code_prefix_literal(render) :
                                markdown_prefix_literal(render)) < 0) {
            return -1;
        }
    }
    if (markdown_flush_delimiter(render, false, true) < 0)
        return -1;
    if (md->escape && markdown_text(render, "\\", 1u) < 0)
        return -1;
    md->escape = false;
    if (md->link_url) {
        md->link_url = false;
        if (markdown_style_changed(render) < 0 ||
            markdown_text(render, ">", 1u) < 0)
            return -1;
    }
    if (md->fence && !render->markdown_preserve_fence) {
        if (!render->public_item_ended_lf &&
            markdown_text(render, "\n", 1u) < 0)
            return -1;
        if (markdown_text(render, "└─", strlen("└─")) < 0)
            return -1;
        md->fence = '\0';
        md->fence_len = 0u;
    }
    md->heading = false;
    md->quote = false;
    md->strong = false;
    md->emphasis = false;
    md->strike = false;
    md->inline_code = false;
    md->link_after_label = false;
    md->code_ticks = 0u;
    if (flush_wrap_pending(render) < 0)
        return -1;
    return markdown_clear_style(render);
}

static int
markdown_abort(struct snj_render *render)
{
    struct snj_markdown_state *md = &render->markdown_state;

    if ((md->table_line || md->table_pending || md->table_active) &&
        markdown_table_finish(render) < 0)
        return -1;
    return markdown_clear_style(render);
}

int
snj_render_public(struct snj_render *render, const char *text, size_t len,
                  struct snj_buf *delivered)
{
    const unsigned char *input = (const unsigned char *)text;
    struct snj_buf complete;
    size_t complete_max;
    int rc = -1;
    int saved_errno = 0;

    if (!render->public_item_open ||
        !snj_size_add(len, sizeof(render->utf8_pending), &complete_max)) {
        errno = EOVERFLOW;
        return -1;
    }
    snj_buf_init(&complete, complete_max);
    for (size_t i = 0; i < len; ++i) {
        size_t expected;

        if (render->utf8_pending_len >= sizeof(render->utf8_pending))
            goto invalid;
        render->utf8_pending[render->utf8_pending_len++] = input[i];
        expected = utf8_sequence_size(render->utf8_pending[0]);
        if (!expected || render->utf8_pending_len > expected)
            goto invalid;
        if (render->utf8_pending_len < expected)
            continue;
        if (!snj_utf8_valid(render->utf8_pending, expected, true))
            goto invalid;
        if (snj_buf_append(&complete, render->utf8_pending, expected) < 0)
            goto out;
        render->utf8_pending_len = 0;
    }
    if (complete.len) {
        if (delivered && snj_buf_reserve(delivered, complete.len) < 0)
            goto out;
        if (public_terminal(render) ?
            (render->markdown_rendering ?
             markdown_write(render, complete.data, complete.len) < 0 :
             write_wrapped(render, complete.data, complete.len) < 0) :
            public_write(render, (const char *)complete.data, complete.len) < 0)
            goto out;
        if (delivered && snj_buf_append(delivered, complete.data, complete.len) < 0)
            goto out;
    }
    rc = 0;
    goto out;
invalid:
    render->utf8_pending_len = 0;
    errno = EILSEQ;
out:
    if (rc < 0)
        saved_errno = errno;
    if (close_public_output(render) < 0 && rc == 0)
        rc = -1;
    snj_buf_free(&complete);
    if (saved_errno)
        errno = saved_errno;
    return rc;
}

static int
close_public_item(struct snj_render *render, bool discard_incomplete)
{
    int fd;
    bool ended_lf;
    bool had_bytes;
    bool markdown_item;
    bool terminal;
    unsigned int trailing_newlines;
    bool invalid = false;
    int rc = 0;
    int saved_errno = 0;

    if (render->utf8_pending_len) {
        render->utf8_pending_len = 0;
        invalid = !discard_incomplete;
    }
    if (!render->public_item_open) {
        if (invalid) {
            errno = EILSEQ;
            return -1;
        }
        return 0;
    }
    if (render->markdown_rendering &&
        (discard_incomplete ? markdown_abort(render) :
                              markdown_finish(render)) < 0) {
        rc = -1;
        saved_errno = errno;
    }
    if (flush_wrap_pending(render) < 0 && rc == 0) {
        rc = -1;
        saved_errno = errno;
    }
    if (close_public_output(render) < 0 && rc == 0) {
        rc = -1;
        saved_errno = errno;
    }
    fd = render->public_fd;
    ended_lf = render->public_item_ended_lf;
    had_bytes = render->public_item_bytes;
    markdown_item = render->markdown_rendering;
    terminal = public_terminal(render);
    trailing_newlines = render->public_trailing_newlines;
    render->public_item_open = false;
    render->public_output_open = false;
    render->public_item_bytes = false;
    render->public_item_ended_lf = false;
    render->public_trailing_newlines = 0u;
    render->markdown_rendering = false;
    render->markdown_preserve_fence = false;
    render->public_fd = -1;
    render->wrap_has_word = false;
    render->wrap_continuation = false;
    render->wrap_word_open = false;
    render->wrap_break_open = false;
    snj_buf_free(&render->markdown_state.table);
    snj_buf_free(&render->wrap_pending);
    if (fd == STDOUT_FILENO && had_bytes) {
        render->stdout_item_seen = true;
        render->stdout_item_ended_lf = ended_lf;
        if (!ended_lf && render->stderr_terminal &&
            write_literal(STDERR_FILENO, "\n") < 0)
            rc = -1;
        else if (!ended_lf && render->stderr_terminal && render->term)
            snj_term_note_output(render->term, "\n", 1u);
    } else if (fd == STDERR_FILENO && had_bytes && !ended_lf &&
               write_literal(STDERR_FILENO, "\n") < 0) {
        rc = -1;
    } else if (fd == STDERR_FILENO && had_bytes && !ended_lf && render->term) {
        snj_term_note_output(render->term, "\n", 1u);
    }
    if (had_bytes && terminal) {
        if (!ended_lf)
            trailing_newlines = fd == STDERR_FILENO || render->stderr_terminal ?
                                1u : 0u;
        render->previous_public_item = true;
        render->previous_public_markdown = markdown_item;
        render->previous_public_fd = fd;
        render->previous_public_newlines = trailing_newlines;
    }
    if (rc < 0 && !saved_errno)
        saved_errno = errno;
    if (saved_errno)
        errno = saved_errno;
    if (invalid && rc == 0) {
        errno = EILSEQ;
        rc = -1;
    }
    return rc;
}

int
snj_render_public_end(struct snj_render *render)
{
    return close_public_item(render, false);
}

int
snj_render_public_abort(struct snj_render *render)
{
    return close_public_item(render, true);
}

static int
rollout_physical_begin(struct snj_render *render,
                       struct snj_render_record *record)
{
    const char *label;

    if (record->physical_open)
        return 0;
    label = record->label_displayed ? NULL : record->label;
    if (snj_render_public_begin(render, record->fd, label) < 0)
        return -1;
    record->physical_open = true;
    record->label_displayed = true;
    return 0;
}

static int
rollout_physical_append(struct snj_render *render,
                        struct snj_render_record *record,
                        const char *text, size_t len)
{
    if (!len)
        return 0;
    if (rollout_physical_begin(render, record) < 0 ||
        snj_render_public(render, text, len, NULL) < 0)
        return -1;
    record->displayed += len;
    return 0;
}

int
snj_render_rollout_begin(struct snj_render *render, int fd,
                         const char *label)
{
    struct snj_render_record *record;

    if (!render->networked)
        return snj_render_public_begin(render, fd, label);
    if (render->rollout_open || (fd != STDOUT_FILENO && fd != STDERR_FILENO)) {
        errno = render->rollout_open ? EBUSY : EINVAL;
        return -1;
    }
    record = calloc(1u, sizeof(*record));
    if (!record)
        return -1;
    record->kind = SNJ_RENDER_RECORD_PUBLIC;
    record->fd = fd;
    snj_buf_init(&record->text, SNJ_MAX_PUBLIC_ITEM);
    if (label) {
        record->label = snj_strdup_checked(label, 1024u);
        if (!record->label) {
            free_record(record);
            return -1;
        }
    }
    if (render->view == SNJ_RENDER_ROLLOUT &&
        rollout_physical_begin(render, record) < 0) {
        free_record(record);
        return -1;
    }
    queue_record(render, SNJ_RENDER_ROLLOUT, record);
    render->rollout_open = record;
    return 0;
}

int
snj_render_rollout(struct snj_render *render, const char *text, size_t len,
                   struct snj_buf *delivered)
{
    struct snj_render_record *record = render->rollout_open;
    const unsigned char *input = (const unsigned char *)text;
    struct snj_buf complete;
    size_t complete_max;
    int rc = -1;

    if (!render->networked)
        return snj_render_public(render, text, len, delivered);
    if (!record || record->complete ||
        !snj_size_add(len, sizeof(record->utf8_pending), &complete_max)) {
        errno = record ? EOVERFLOW : EINVAL;
        return -1;
    }
    snj_buf_init(&complete, complete_max);
    for (size_t i = 0u; i < len; ++i) {
        size_t expected;

        if (record->utf8_pending_len >= sizeof(record->utf8_pending))
            goto invalid;
        record->utf8_pending[record->utf8_pending_len++] = input[i];
        expected = utf8_sequence_size(record->utf8_pending[0]);
        if (!expected || record->utf8_pending_len > expected)
            goto invalid;
        if (record->utf8_pending_len < expected)
            continue;
        if (!snj_utf8_valid(record->utf8_pending, expected, true))
            goto invalid;
        if (snj_buf_append(&complete, record->utf8_pending, expected) < 0)
            goto out;
        record->utf8_pending_len = 0u;
    }
    if (complete.len) {
        if (snj_buf_reserve(&record->text, complete.len) < 0 ||
            (delivered && snj_buf_reserve(delivered, complete.len) < 0) ||
            snj_buf_append(&record->text, complete.data, complete.len) < 0)
            goto out;
        if (render->view == SNJ_RENDER_ROLLOUT &&
            rollout_physical_append(render, record,
                                    (const char *)complete.data,
                                    complete.len) < 0)
            goto out;
        if (delivered &&
            snj_buf_append(delivered, complete.data, complete.len) < 0)
            goto out;
    }
    rc = 0;
    goto out;
invalid:
    record->utf8_pending_len = 0u;
    errno = EILSEQ;
out:
    snj_buf_free(&complete);
    return rc;
}

static int
close_rollout_record(struct snj_render *render, bool abort)
{
    struct snj_render_record *record = render->rollout_open;
    bool was_head;
    int rc = 0;

    if (!render->networked)
        return abort ? snj_render_public_abort(render) :
                       snj_render_public_end(render);
    if (!record)
        return 0;
    if (record->utf8_pending_len) {
        record->utf8_pending_len = 0u;
        if (!abort) {
            errno = EILSEQ;
            rc = -1;
        }
    }
    record->complete = true;
    record->aborted = abort;
    was_head = record == render->view_head[SNJ_RENDER_ROLLOUT];
    if (record->physical_open) {
        if ((abort ? snj_render_public_abort(render) :
                     snj_render_public_end(render)) < 0 && rc == 0)
            rc = -1;
        record->physical_open = false;
    }
    render->rollout_open = NULL;
    if (was_head && record->displayed == record->text.len) {
        pop_record(render, SNJ_RENDER_ROLLOUT);
        if (render->view == SNJ_RENDER_ROLLOUT &&
            flush_view(render, SNJ_RENDER_ROLLOUT) < 0 && rc == 0)
            rc = -1;
    }
    return rc;
}

int
snj_render_rollout_end(struct snj_render *render)
{
    return close_rollout_record(render, false);
}

int
snj_render_rollout_abort(struct snj_render *render)
{
    return close_rollout_record(render, true);
}

static int
render_message(struct snj_render *render, const char *message,
               const char *color)
{
    struct snj_buf line;
    int rc;

    snj_buf_init(&line, 16384u);
    rc = snj_buf_printf(&line, SNAJPAGENT_NAME ": %s\n", message);
    if (rc == 0)
        rc = write_role_block(render, STDERR_FILENO, color,
                              (char *)line.data, line.len, line.len,
                              render->stderr_terminal, true);
    snj_buf_free(&line);
    return rc;
}

int
snj_render_error_ctx(struct snj_render *render, const char *message)
{
    return render_message(render, message, COLOR_ERROR);
}

int
snj_render_warning_ctx(struct snj_render *render, const char *message)
{
    return render_message(render, message, COLOR_WARNING);
}

int
snj_render_host(struct snj_render *render, const char *text)
{
    size_t len = strlen(text);
    struct snj_buf line;
    int rc;

    snj_buf_init(&line, 4u * 1024u * 1024u);
    rc = snj_buf_append(&line, text, len);
    if (rc == 0 && (len == 0u || text[len - 1u] != '\n'))
        rc = snj_buf_putc(&line, '\n');
    if (rc == 0)
        rc = write_role_block(render, STDERR_FILENO, COLOR_HOST,
                              (char *)line.data, line.len,
                              first_line_len((char *)line.data, line.len),
                              render->stderr_terminal, true);
    snj_buf_free(&line);
    return rc;
}

int
snj_render_runtime(struct snj_render *render, const char *text)
{
    size_t len;
    struct snj_buf line;
    int rc;

    if (render->verbosity < 3u)
        return 0;
    len = strlen(text);
    snj_buf_init(&line, 4u * 1024u * 1024u);
    rc = snj_buf_append(&line, text, len);
    if (rc == 0 && (!len || text[len - 1u] != '\n'))
        rc = snj_buf_putc(&line, '\n');
    if (rc == 0)
        rc = view_block(render, SNJ_RENDER_ROLLOUT, STDERR_FILENO, COLOR_META,
                        (char *)line.data, line.len,
                        first_line_len((char *)line.data, line.len),
                        render->stderr_terminal, true);
    snj_buf_free(&line);
    return rc;
}

static const char *
irc_event_word(enum snj_irc_event_kind kind)
{
    switch (kind) {
    case SNJ_IRC_CONNECTED: return "connected";
    case SNJ_IRC_DISCONNECTED: return "disconnected";
    case SNJ_IRC_JOIN: return "joined";
    case SNJ_IRC_PART: return "left";
    case SNJ_IRC_QUIT: return "quit";
    case SNJ_IRC_NICK: return "is now known as";
    case SNJ_IRC_TOPIC: return "set topic";
    case SNJ_IRC_MODE: return "set mode";
    case SNJ_IRC_HISTORY_READY: return "history synchronized";
    case SNJ_IRC_MESSAGE: case SNJ_IRC_NOTICE: break;
    }
    return "event";
}

static int
irc_piece(struct snj_render *render, const char *text, bool safe)
{
    size_t len = strlen(text);

    if ((safe ? snj_term_write_safe(STDERR_FILENO, text, len) :
                snj_write_full(STDERR_FILENO, text, len)) < 0)
        return -1;
    if (render->term && (len == 0u || text[0] != '\033'))
        snj_term_note_output(render->term, text, len);
    return 0;
}

static struct snj_irc_markdown_state *
irc_markdown_state(struct snj_render *render, const struct snj_irc_event *event,
                   bool allocate)
{
    struct snj_irc_markdown_state *empty = NULL;

    for (size_t i = 0u; i < SNJ_RENDER_IRC_MARKDOWN_STATES; ++i) {
        struct snj_irc_markdown_state *state = &render->irc_markdown[i];
        if (!state->fence) {
            if (!empty)
                empty = state;
            continue;
        }
        if (strcmp(state->endpoint, event->endpoint) == 0 &&
            strcmp(state->nick, event->nick) == 0)
            return state;
    }
    return allocate ? empty : NULL;
}

static void
irc_markdown_lifecycle(struct snj_render *render,
                       const struct snj_irc_event *event)
{
    bool endpoint_reset = event->kind == SNJ_IRC_CONNECTED ||
                          event->kind == SNJ_IRC_DISCONNECTED;
    bool nick_reset = event->kind == SNJ_IRC_PART ||
                      event->kind == SNJ_IRC_QUIT ||
                      event->kind == SNJ_IRC_NICK;

    if (!endpoint_reset && !nick_reset)
        return;
    for (size_t i = 0u; i < SNJ_RENDER_IRC_MARKDOWN_STATES; ++i) {
        struct snj_irc_markdown_state *state = &render->irc_markdown[i];
        if (state->fence && strcmp(state->endpoint, event->endpoint) == 0 &&
            (endpoint_reset || strcmp(state->nick, event->nick) == 0))
            memset(state, 0, sizeof(*state));
    }
}

static int
render_irc_markdown(struct snj_render *render,
                    const struct snj_irc_event *event, size_t column,
                    const char *suffix)
{
    struct snj_irc_markdown_state *saved =
        irc_markdown_state(render, event, false);
    struct snj_render body;
    int rc = -1;

    snj_render_init(&body, render->verbosity);
    body.stderr_terminal = render->stderr_terminal;
    body.color_stderr = render->color_stderr;
    body.markdown = true;
    body.term = render->term;
    if (snj_render_public_begin(&body, STDERR_FILENO, NULL) < 0)
        return -1;
    body.public_column = column;
    body.markdown_preserve_fence = true;
    if (saved) {
        body.markdown_state.fence = saved->fence;
        body.markdown_state.fence_len = saved->fence_len;
    }
    if (snj_render_public(&body, event->text, strlen(event->text), NULL) < 0 ||
        markdown_finish(&body) < 0)
        goto out;
    if (body.markdown_state.fence) {
        if (!saved)
            saved = irc_markdown_state(render, event, true);
        if (saved) {
            (void)snprintf(saved->endpoint, sizeof(saved->endpoint), "%s",
                           event->endpoint);
            (void)snprintf(saved->nick, sizeof(saved->nick), "%s", event->nick);
            saved->fence = body.markdown_state.fence;
            saved->fence_len = body.markdown_state.fence_len;
        }
    } else if (saved) {
        memset(saved, 0, sizeof(*saved));
    }
    body.markdown_rendering = false;
    if (suffix[0] &&
        snj_render_public(&body, suffix, strlen(suffix), NULL) < 0)
        goto out;
    if (!body.public_item_bytes && public_write(&body, "\n", 1u) < 0)
        goto out;
    rc = snj_render_public_end(&body);
    return rc;
out:
    body.markdown_rendering = false;
    (void)snj_render_public_abort(&body);
    return -1;
}

static int
render_irc_event_now(struct snj_render *render,
                     const struct snj_irc_event *event, bool own_agent)
{
    char when[16u];
    char prefix[768u];
    char suffix[384u] = {0};
    time_t seconds;
    struct tm tm;
    const char *nick_color;
    bool colored;
    bool markdown_body;
    int n;
    int rc = -1;

    if (!render || !event) {
        errno = EINVAL;
        return -1;
    }
    irc_markdown_lifecycle(render, event);
    if (own_agent && render->verbosity < 1u)
        return 0;
    seconds = (time_t)(event->timestamp_ms / 1000u);
    if (!localtime_r(&seconds, &tm) ||
        strftime(when, sizeof(when), "%H:%M:%S", &tm) == 0)
        memcpy(when, "--:--:--", 9u);
    colored = render->color_stderr;
    nick_color = event->op ? "\033[1;35m" :
                 own_agent ?
                 "\033[1;36m" : "\033[1;34m";
    if (output_begin(render, true) < 0)
        return -1;
    if (colored && irc_piece(render, "\033[2m", false) < 0)
        goto out;
    n = snprintf(prefix, sizeof(prefix), "%s%s ", when,
                 event->historical ? " history" : "");
    if (n < 0 || (size_t)n >= sizeof(prefix) ||
        irc_piece(render, prefix, true) < 0)
        goto out;
    if (colored && (irc_piece(render, "\033[0m", false) < 0 ||
                    irc_piece(render, nick_color, false) < 0))
        goto out;
    if (event->kind == SNJ_IRC_MESSAGE || event->kind == SNJ_IRC_NOTICE) {
        n = snprintf(prefix, sizeof(prefix), "%s%s%s ",
                     event->kind == SNJ_IRC_NOTICE ? "-" : "",
                     event->op ? "@" : "", event->nick);
        if (n < 0 || (size_t)n >= sizeof(prefix) ||
            irc_piece(render, prefix, true) < 0)
            goto out;
        if (colored && irc_piece(render, "\033[0m", false) < 0)
            goto out;
        if (irc_piece(render,
                event->kind == SNJ_IRC_NOTICE ? "- " : "› ", true) < 0)
            goto out;
        markdown_body = event->kind == SNJ_IRC_MESSAGE && render->markdown &&
                        render->stderr_terminal && !event->op;
        if (markdown_body) {
            char visible[1024u];
            size_t column;

            n = snprintf(visible, sizeof(visible), "%s%s %s%s%s %s ",
                         when, event->historical ? " history" : "",
                         event->kind == SNJ_IRC_NOTICE ? "-" : "",
                         event->op ? "@" : "", event->nick,
                         event->kind == SNJ_IRC_NOTICE ? "-" : "›");
            if (n < 0 || (size_t)n >= sizeof(visible))
                goto out;
            column = snj_term_text_width(visible, (size_t)n);
            if (column == SIZE_MAX)
                goto out;
            if (render->verbosity >= 4u && event->endpoint[0]) {
                n = snprintf(suffix, sizeof(suffix), " [%s]", event->endpoint);
                if (n < 0 || (size_t)n >= sizeof(suffix))
                    goto out;
            }
            if (render_irc_markdown(render, event,
                    column % snj_term_columns(render->term), suffix) < 0)
                goto out;
            rc = 0;
            goto out;
        }
        if (irc_piece(render, event->text, true) < 0)
            goto out;
    } else {
        n = snprintf(prefix, sizeof(prefix), "· %s%s%s %s",
                     event->op ? "@" : "", event->nick,
                     event->nick[0] ? " " : "", irc_event_word(event->kind));
        if (n < 0 || (size_t)n >= sizeof(prefix) ||
            irc_piece(render, prefix, true) < 0)
            goto out;
        if (colored && irc_piece(render, "\033[0m", false) < 0)
            goto out;
        if (event->text[0] &&
            (irc_piece(render, " · ", true) < 0 ||
             irc_piece(render, event->text, true) < 0))
            goto out;
    }
    if (render->verbosity >= 4u && event->endpoint[0] &&
        (colored && irc_piece(render, "\033[2m", false) < 0))
        goto out;
    if (render->verbosity >= 4u && event->endpoint[0] &&
        (irc_piece(render, " [", true) < 0 ||
         irc_piece(render, event->endpoint, true) < 0 ||
         irc_piece(render, "]", true) < 0))
        goto out;
    if (colored && irc_piece(render, "\033[0m", false) < 0)
        goto out;
    if (irc_piece(render, "\n", false) < 0)
        goto out;
    rc = 0;
out:
    if (colored)
        (void)irc_piece(render, "\033[0m", false);
    if (output_end(render) < 0 && rc == 0)
        rc = -1;
    return rc;
}

int
snj_render_irc_event(struct snj_render *render,
                     const struct snj_irc_event *event)
{
    struct snj_render_record *record;
    bool own_agent;

    if (!render || !event) {
        errno = EINVAL;
        return -1;
    }
    own_agent = event->local && render->model_nick[0] &&
                strcmp(event->nick, render->model_nick) == 0;
    if (!render->networked || render->view == SNJ_RENDER_CHAT)
        return render_irc_event_now(render, event, own_agent);
    record = calloc(1u, sizeof(*record));
    if (!record)
        return -1;
    record->kind = SNJ_RENDER_RECORD_IRC;
    record->irc = *event;
    record->own_agent = own_agent;
    queue_record(render, SNJ_RENDER_CHAT, record);
    return 0;
}

static int
flush_view(struct snj_render *render, enum snj_render_view view)
{
    while (render->view_head[view]) {
        struct snj_render_record *record = render->view_head[view];
        int rc;

        if (record->kind == SNJ_RENDER_RECORD_BLOCK) {
            rc = write_role_block(render, record->fd, record->color,
                                  (const char *)record->text.data,
                                  record->text.len, record->colored_len,
                                  record->terminal_safe, record->persistent);
        } else if (record->kind == SNJ_RENDER_RECORD_IRC) {
            rc = render_irc_event_now(render, &record->irc, record->own_agent);
        } else {
            rc = 0;
            if (record->displayed < record->text.len)
                rc = rollout_physical_append(
                    render, record,
                    (const char *)record->text.data + record->displayed,
                    record->text.len - record->displayed);
            if (rc == 0 && record->complete && record->physical_open) {
                rc = record->aborted ? snj_render_public_abort(render) :
                                       snj_render_public_end(render);
                record->physical_open = false;
            }
            if (rc < 0)
                return -1;
            if (!record->complete)
                return 0;
        }
        if (rc < 0)
            return -1;
        pop_record(render, view);
    }
    return 0;
}

enum snj_render_view
snj_render_view(const struct snj_render *render)
{
    return render ? render->view : SNJ_RENDER_ROLLOUT;
}

int
snj_render_set_view(struct snj_render *render, enum snj_render_view view)
{
    static const char *const boundaries[SNJ_RENDER_VIEW_COUNT] = {
        "── chat ──\n", "── rollout ──\n"
    };
    struct snj_render_record *open;

    if (!render || (view != SNJ_RENDER_CHAT && view != SNJ_RENDER_ROLLOUT) ||
        (!render->networked && view != SNJ_RENDER_ROLLOUT)) {
        errno = EINVAL;
        return -1;
    }
    if (render->view == view)
        return 0;
    open = render->rollout_open;
    if (open && open->physical_open) {
        /* Finish the visible fragment so buffered Markdown/wrap text is not
         * lost when the logical stream resumes after visiting chat. */
        if (snj_render_public_end(render) < 0)
            return -1;
        open->physical_open = false;
    }
    render->view = view;
    if (write_role_block(render, STDERR_FILENO, COLOR_HOST, boundaries[view],
                         strlen(boundaries[view]), strlen(boundaries[view]),
                         render->stderr_terminal, true) < 0 ||
        flush_view(render, view) < 0)
        return -1;
    return 0;
}

static const char *
tool_label(const char *name)
{
    if (strcmp(name, "exec_command") == 0)
        return "exec";
    if (strcmp(name, "write_stdin") == 0)
        return "stdin";
    if (strcmp(name, "apply_patch") == 0)
        return "patch";
    return name;
}

static size_t
utf8_prefix(const char *text, size_t limit)
{
    size_t len = strlen(text);
    if (len <= limit)
        return len;
    while (limit && (((unsigned char)text[limit] & 0xc0u) == 0x80u))
        --limit;
    return limit;
}

static int
append_shell_quoted(struct snj_buf *line, const char *text)
{
    size_t full = strlen(text);
    size_t shown = utf8_prefix(text, 2048u);

    if (snj_buf_putc(line, '\'') < 0)
        return -1;
    for (size_t i = 0; i < shown; ++i) {
        if (text[i] == '\'') {
            if (snj_buf_append(line, "'\\''", 4u) < 0)
                return -1;
        } else if (snj_buf_putc(line, (unsigned char)text[i]) < 0) {
            return -1;
        }
    }
    if (snj_buf_putc(line, '\'') < 0)
        return -1;
    if (shown != full &&
        snj_buf_printf(line, " … <%zu bytes omitted>", full - shown) < 0)
        return -1;
    return 0;
}

int
snj_render_tool_start(struct snj_render *render,
                      const struct snj_response_item *call,
                      const char *workdir, uint32_t default_timeout_ms)
{
    struct snj_buf line;
    const char *label;
    const char *command;
    json_t *timeout_value;
    uint32_t timeout_ms = default_timeout_ms;
    size_t prefix_len = 0u;
    int rc = 0;

    if (render->verbosity < 1u)
        return 0;
    label = tool_label(call->name);
    command = snj_json_string(call->arguments, "command");
    timeout_value = json_object_get(call->arguments, "timeout_ms");
    if (json_is_integer(timeout_value)) {
        json_int_t requested = json_integer_value(timeout_value);

        if (requested >= 0 && (uint64_t)requested <= UINT32_MAX)
            timeout_ms = (uint32_t)requested;
    }
    snj_buf_init(&line, SNJ_MAX_TOOL_ARGUMENTS * 2u + 4096u);
    if (snj_buf_printf(&line, "→ %s", label) < 0)
        rc = -1;
    else if (command) {
        prefix_len = line.len;
        if ((timeout_ms &&
             snj_buf_printf(&line, "  timeout=%ums", timeout_ms) < 0) ||
            (!timeout_ms &&
             snj_buf_append(&line, "  timeout=none", 14u) < 0) ||
            snj_buf_append(&line, "  ", 2u) < 0 ||
            append_shell_quoted(&line, command) < 0)
            rc = -1;
    }
    if (rc == 0 && snj_buf_putc(&line, '\n') < 0)
        rc = -1;
    if (rc == 0) {
        struct snj_buf encoded;
        snj_buf_init(&encoded, SNJ_MAX_TOOL_ARGUMENTS + 64u);
        if (snj_json_canonical(call->arguments, &encoded) < 0 ||
            snj_buf_printf(&line, "  workdir: %s\n  arguments: ", workdir) < 0 ||
            snj_buf_append(&line, encoded.data, encoded.len) < 0 ||
            snj_buf_putc(&line, '\n') < 0)
            rc = -1;
        snj_buf_free(&encoded);
    }
    if (rc == 0)
        rc = view_block(render, SNJ_RENDER_ROLLOUT, STDERR_FILENO,
                        COLOR_ACTIVITY, (char *)line.data, line.len,
                        command ? prefix_len :
                            first_line_len((char *)line.data, line.len),
                        render->stderr_terminal, true);
    snj_buf_free(&line);
    return rc;
}

int
snj_render_tool_finish(struct snj_render *render, const char *name,
                       const json_t *result, uint32_t max_output_bytes)
{
    struct snj_buf line;
    const char *model_text;
    const char *status;
    const char *reason;
    json_t *exit_value;
    uint64_t duration = 0u;
    const char *color = COLOR_WARNING;
    int rc = 0;

    if (render->verbosity < 1u)
        return 0;
    status = snj_json_string(result, "status");
    reason = snj_json_string(result, "reason");
    exit_value = json_object_get(result, "exit_code");
    (void)snj_json_integer_u64(result, "duration_ms", &duration);
    if ((status && strcmp(status, "succeeded") == 0) ||
        (json_is_integer(exit_value) && json_integer_value(exit_value) == 0))
        color = COLOR_SUCCESS;
    else if ((status && (strcmp(status, "failed") == 0 ||
                         strcmp(status, "outcome_unknown") == 0)) ||
             (json_is_integer(exit_value) && json_integer_value(exit_value) != 0))
        color = COLOR_ERROR;
    model_text = snj_json_string(result, "model_text");
    snj_buf_init(&line, SIZE_MAX);
    if (snj_buf_printf(&line, "← %s  ", tool_label(name)) < 0)
        rc = -1;
    else if (json_is_integer(exit_value)) {
        if (snj_buf_printf(&line, "exit %lld",
                           (long long)json_integer_value(exit_value)) < 0)
            rc = -1;
    } else if (status && strcmp(status, "not_run") == 0) {
        if (snj_buf_printf(&line, "not run%s%s", reason ? " · " : "",
                           reason ? reason : "") < 0)
            rc = -1;
    } else if (status && strcmp(status, "outcome_unknown") == 0) {
        if (snj_buf_append(&line, "outcome unknown", 15u) < 0)
            rc = -1;
    } else if (snj_buf_append(&line, status ? status : "unknown",
                              strlen(status ? status : "unknown")) < 0) {
        rc = -1;
    }
    if (rc == 0) {
        if (duration < 1000u)
            rc = snj_buf_printf(&line, " · %llums\n",
                                (unsigned long long)duration);
        else
            rc = snj_buf_printf(&line, " · %llu.%llus\n",
                    (unsigned long long)(duration / 1000u),
                    (unsigned long long)((duration % 1000u) / 100u));
    }
    if (rc == 0 && model_text) {
        size_t len = json_string_length(json_object_get(result, "model_text"));
        size_t shown = len;

        if (max_output_bytes && shown > max_output_bytes) {
            shown = max_output_bytes;
            while (shown && shown < len &&
                   ((unsigned char)model_text[shown] & 0xc0u) == 0x80u)
                --shown;
        }
        if (snj_buf_append(&line, "  output:\n", 10u) < 0 ||
            snj_buf_append(&line, model_text, shown) < 0 ||
            (shown && model_text[shown - 1u] != '\n' &&
             snj_buf_putc(&line, '\n') < 0) ||
            (shown < len &&
             snj_buf_printf(&line,
                            "  <%zu output bytes hidden by max_output_bytes>\n",
                            len - shown) < 0))
            rc = -1;
    }
    if (rc == 0)
        rc = view_block(render, SNJ_RENDER_ROLLOUT, STDERR_FILENO, color,
                        (char *)line.data, line.len,
                        first_line_len((char *)line.data, line.len),
                        render->stderr_terminal, true);
    snj_buf_free(&line);
    return rc;
}

int
snj_render_event(struct snj_render *render, uint64_t seq, const char *type)
{
    const char *notice = NULL;
    struct snj_buf line;
    int rc = 0;

    if (strcmp(type, "compaction_completed") == 0)
        notice = "Compacted";
    else if (strcmp(type, "goal_started") == 0 ||
             strcmp(type, "goal_reworded") == 0)
        notice = "Goal set";
    else if (strcmp(type, "goal_completed") == 0 ||
             strcmp(type, "goal_cancelled") == 0)
        notice = "Goal cleared";
    if (!notice && render->verbosity < 4u)
        return 0;
    snj_buf_init(&line, 1024u);
    if (notice && render->verbosity < 4u) {
        if (snj_buf_printf(&line, "• %s\n", notice) < 0)
            rc = -1;
    } else if (notice) {
        if (snj_buf_printf(&line, "• %s · event › %llu %s synced\n",
                           notice, (unsigned long long)seq, type) < 0)
            rc = -1;
    } else if (snj_buf_printf(&line, "event › %llu %s synced\n",
                              (unsigned long long)seq, type) < 0) {
        rc = -1;
    }
    if (rc == 0)
        rc = view_block(render, SNJ_RENDER_ROLLOUT, STDERR_FILENO,
                        notice ? COLOR_LIFECYCLE : COLOR_DURABLE,
                        (char *)line.data, line.len, line.len,
                        render->stderr_terminal, true);
    snj_buf_free(&line);
    return rc;
}

int
snj_render_resume_hint(const struct snj_render *render, const char *command,
                       size_t command_len)
{
    static const char header[] =
        "• You can resume this session with the following command:";
    struct snj_buf block;
    size_t max = command_len;
    bool colored;
    int rc = -1;

    if (!render || !command || !command_len) {
        errno = EINVAL;
        return -1;
    }
    if (!snj_size_add(max, sizeof(header) + 1u, &max)) {
        errno = EOVERFLOW;
        return -1;
    }
    colored = render->color_stderr;
    if (colored &&
        (!snj_size_add(max, sizeof(COLOR_LIFECYCLE) - 1u, &max) ||
         !snj_size_add(max, sizeof(COLOR_RESET) - 1u, &max))) {
        errno = EOVERFLOW;
        return -1;
    }
    snj_buf_init(&block, max);
    if (colored &&
        snj_buf_append(&block, COLOR_LIFECYCLE,
                       sizeof(COLOR_LIFECYCLE) - 1u) < 0)
        goto out;
    if (snj_buf_append(&block, header, sizeof(header) - 1u) < 0)
        goto out;
    if (colored &&
        snj_buf_append(&block, COLOR_RESET, sizeof(COLOR_RESET) - 1u) < 0)
        goto out;
    if (snj_buf_putc(&block, '\n') < 0 ||
        snj_buf_append(&block, command, command_len) < 0 ||
        snj_buf_putc(&block, '\n') < 0)
        goto out;
    rc = snj_write_full(STDERR_FILENO, block.data, block.len);
out:
    snj_buf_free(&block);
    return rc;
}

static bool
diagnostic_text_valid(const char *text, size_t len, bool multiline)
{
    if (!text || !snj_utf8_valid((const unsigned char *)text, len, true))
        return false;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)text[i];
        if (c == '\n' && multiline)
            continue;
        if (c == '\t')
            continue;
        if (c < 0x20u || c == 0x7fu)
            return false;
    }
    return true;
}

static int
protocol_warning(struct snj_render *render)
{
    static const char warning[] =
        SNAJPAGENT_NAME
        ": verbosity 5 exposes prompts, source code, tool output, and model content\n";

    if (render->protocol_warning_shown)
        return 0;
    if (write_role_block(render, STDERR_FILENO, COLOR_WARNING,
                         warning, sizeof(warning) - 1u,
                         sizeof(warning) - 1u,
                         render->stderr_terminal, true) < 0)
        return -1;
    render->protocol_warning_shown = true;
    return 0;
}

int
snj_render_protocol(struct snj_render *render, const char *label,
                    const char *text, size_t len)
{
    struct snj_buf block;
    int rc = -1;

    if (render->verbosity < 5u)
        return 0;
    if (!label || !diagnostic_text_valid(label, strlen(label), false) ||
        !diagnostic_text_valid(text, len, true) ||
        len > 2u * 1024u * 1024u) {
        errno = EINVAL;
        return -1;
    }
    if (protocol_warning(render) < 0)
        return -1;
    snj_buf_init(&block, 2u * 1024u * 1024u + 4096u);
    if (snj_buf_printf(&block, "protocol › %s\n", label) < 0 ||
        snj_buf_append(&block, text, len) < 0 ||
        (len && text[len - 1u] != '\n' && snj_buf_putc(&block, '\n') < 0))
        goto out;
    rc = view_block(render, SNJ_RENDER_ROLLOUT, STDERR_FILENO, COLOR_PROTOCOL,
                    (const char *)block.data, block.len,
                    first_line_len((const char *)block.data, block.len),
                    render->stderr_terminal, true);
out:
    snj_buf_free(&block);
    return rc;
}

int
snj_render_transport(struct snj_render *render, char direction,
                     const char *text, size_t len)
{
    struct snj_buf line;
    int rc = -1;

    if (render->verbosity < 6u)
        return 0;
    if ((direction != '>' && direction != '<') ||
        !diagnostic_text_valid(text, len, false) || len > 64u * 1024u) {
        errno = EINVAL;
        return -1;
    }
    if (protocol_warning(render) < 0)
        return -1;
    snj_buf_init(&line, 64u * 1024u + 4u);
    if (snj_buf_putc(&line, (unsigned char)direction) < 0 ||
        snj_buf_putc(&line, ' ') < 0 ||
        snj_buf_append(&line, text, len) < 0 || snj_buf_putc(&line, '\n') < 0)
        goto out;
    rc = view_block(render, SNJ_RENDER_ROLLOUT, STDERR_FILENO, COLOR_TRANSPORT,
                    (const char *)line.data, line.len,
                    line.len > 2u ? 2u : line.len,
                    render->stderr_terminal, true);
out:
    snj_buf_free(&line);
    return rc;
}

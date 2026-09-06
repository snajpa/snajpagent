/* SPDX-License-Identifier: GPL-2.0-only */
#include "render.h"
#include "base.h"
#include "fs.h"
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

enum snag_render_record_kind {
    SNAG_RENDER_RECORD_BLOCK,
    SNAG_RENDER_RECORD_IRC,
    SNAG_RENDER_RECORD_TOOL,
    SNAG_RENDER_RECORD_PUBLIC
};

struct snag_render_record {
    struct snag_render_record *next;
    enum snag_render_record_kind kind;
    enum snag_presentation presentation;
    struct snag_buf text;
    const char *color;
    char *label;
    struct snag_irc_event *irc;
    enum snag_irc_event_kind irc_kind;
    size_t colored_len;
    size_t displayed;
    size_t source_item;
    int fd;
    unsigned char utf8_pending[4];
    size_t utf8_pending_len;
    bool terminal_safe;
    bool persistent;
    bool complete;
    bool aborted;
    bool physical_open;
    bool label_displayed;
    struct snag_render_source source, response;
    uint32_t timeout_ms, max_output_bytes;
    bool tool_start;
    bool omitted;
};

static int render_irc_event_now(struct snag_render *render,
                                const struct snag_irc_event *event);
static int flush_view(struct snag_render *render, enum snag_render_view view);
static int close_public_output(struct snag_render *render);
static int render_tool_record(struct snag_render *render, const struct snag_render_record *record);
static json_t *source_event(struct snag_render *render, struct snag_render_source source);

const char *
snag_verbosity_name(unsigned int level)
{
    static const char *const names[] = {
        "conversation", "tools", "previews", "full tools", "debug", "protocol", "wire"
    };
    return level <= SNAG_VERBOSITY_MAX ? names[level] : NULL;
}

bool
snag_presentation_enabled(enum snag_presentation kind, unsigned int level,
                          enum snag_render_view view)
{
    static const unsigned char minimum[] = {0, 1, 2, 2, 3, 4, 5, 6, 0, 0};
    if ((unsigned int)kind >= sizeof(minimum) || level < minimum[kind])
        return false;
    if (kind == SNAG_PRESENT_FEEDBACK)
        return true;
    return view == (kind == SNAG_PRESENT_CHAT ? SNAG_RENDER_CHAT : SNAG_RENDER_ROLLOUT);
}

size_t
snag_presentation_limit(enum snag_presentation kind, unsigned int level)
{
    if (!snag_presentation_enabled(kind, level, SNAG_RENDER_ROLLOUT))
        return 0u;
    if (level == 2u && kind == SNAG_PRESENT_ARGUMENTS)
        return 1024u;
    if (level == 2u && kind == SNAG_PRESENT_OUTPUT)
        return 512u;
    return SIZE_MAX;
}

bool
snag_render_enabled(const struct snag_render *render, enum snag_presentation kind)
{
    if (render->suppress_optional && kind >= SNAG_PRESENT_TOOL && kind <= SNAG_PRESENT_WIRE)
        return false;
    return snag_presentation_enabled(kind, render->verbosity, render->view);
}

static int
render_checkpoint(struct snag_render *render)
{
    if (!render->checkpoint)
        return 0;
    if (!render->markdown_measuring && close_public_output(render) < 0)
        return -1;
    return render->checkpoint(render->checkpoint_opaque);
}

static size_t
text_slice(const char *text, size_t len)
{
    size_t amount = len < 1024u ? len : 1024u;
    while (amount < len && amount &&
           ((unsigned char)text[amount] & 0xc0u) == 0x80u)
        --amount;
    return amount ? amount : len < 4u ? len : 4u;
}

static int
write_literal(int fd, const char *s)
{
    return snag_term_write(fd, s, strlen(s));
}

static int
output_begin(struct snag_render *render, bool persistent)
{
    return render->term ? snag_term_output_begin(render->term, persistent) : 0;
}

static int
output_end(struct snag_render *render)
{
    return render->term ? snag_term_output_end(render->term) : 0;
}

static bool
color_enabled(const struct snag_render *render, int fd)
{
    return fd == STDOUT_FILENO ? render->color_stdout : render->color_stderr;
}

static int
write_role_chunk(struct snag_render *render, int fd, const char *color,
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
    if (!len)
        return 0;
    if (output_begin(render, persistent) < 0)
        return -1;
    if (colored && write_literal(fd, color) < 0)
        goto out;
    if (colored_len &&
        (terminal_safe ? snag_term_write_safe(fd, text, colored_len) :
                         snag_term_write(fd, text, colored_len)) < 0)
        goto out;
    if (colored && write_literal(fd, COLOR_RESET) < 0)
        goto out;
    if (len > colored_len &&
        (terminal_safe ? snag_term_write_safe(fd, text + colored_len,
                                              len - colored_len) :
                         snag_term_write(fd, text + colored_len,
                                        len - colored_len)) < 0)
        goto out;
    if (render->term &&
        ((fd == STDOUT_FILENO && render->stdout_terminal) ||
         (fd == STDERR_FILENO && render->stderr_terminal)) &&
        snag_term_note_output(render->term, text, len,
                             colored && colored_len == len ? color : "") < 0)
        goto out;
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

static int
write_role_block(struct snag_render *render, int fd, const char *color,
                 const char *text, size_t len, size_t colored_len,
                 bool terminal_safe, bool persistent)
{
    bool deferred = render->term && render->term->defer_redraw;
    int rc = 0;

    if (!len)
        return write_role_chunk(render, fd, color, text, len, colored_len,
                                 terminal_safe, persistent);
    while (len) {
        size_t amount = text_slice(text, len);
        size_t colored = colored_len < amount ? colored_len : amount;
        if (render->term)
            render->term->defer_redraw = deferred || len > amount;
        if (write_role_chunk(render, fd, color, text, amount, colored,
                              terminal_safe, persistent) < 0) {
            rc = -1;
            break;
        }
        text += amount;
        len -= amount;
        colored_len -= colored;
        if (len && render_checkpoint(render) < 0) {
            rc = -1;
            break;
        }
    }
    if (render->term)
        render->term->defer_redraw = deferred;
    return rc;
}

static void
free_record(struct snag_render_record *record)
{
    if (!record)
        return;
    snag_buf_free(&record->text);
    free(record->irc);
    free(record->label);
    free(record);
}

static int
write_optional_block(struct snag_render *render, enum snag_presentation kind,
                     const char *color, const char *text, size_t len,
                     size_t colored_len)
{
    bool ended_lf = true;
    while (len) {
        if (render_checkpoint(render) < 0)
            return -1;
        if (!snag_render_enabled(render, kind)) {
            if (!ended_lf && write_literal(STDERR_FILENO, "\n") < 0)
                return -1;
            return snag_render_host(render, "… [display omitted]");
        }
        size_t amount = text_slice(text, len);
        size_t colored = colored_len < amount ? colored_len : amount;
        if (write_role_chunk(render, STDERR_FILENO, color, text, amount,
                              colored, render->stderr_terminal, true) < 0)
            return -1;
        ended_lf = text[amount - 1u] == '\n';
        text += amount;
        len -= amount;
        colored_len -= colored;
    }
    return 0;
}

static void
queue_record(struct snag_render *render, enum snag_render_view view,
             struct snag_render_record *record)
{
    if (render->view_tail[view])
        render->view_tail[view]->next = record;
    else
        render->view_head[view] = record;
    render->view_tail[view] = record;
}

static void
pop_record(struct snag_render *render, enum snag_render_view view)
{
    struct snag_render_record *record = render->view_head[view];

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
view_block(struct snag_render *render, enum snag_render_view view, int fd,
           const char *color, const char *text, size_t len,
           size_t colored_len, bool terminal_safe, bool persistent)
{
    struct snag_render_record *record;

    if (render->view == view && !render->view_head[view])
        return write_role_block(render, fd, color, text, len, colored_len,
                                terminal_safe, persistent);
    record = calloc(1u, sizeof(*record));
    if (!record)
        return -1;
    record->kind = SNAG_RENDER_RECORD_BLOCK;
    record->fd = fd;
    record->color = color;
    record->colored_len = colored_len;
    record->terminal_safe = terminal_safe;
    record->persistent = persistent;
    snag_buf_init(&record->text, len);
    if (snag_buf_append(&record->text, text, len) < 0) {
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
write_block(struct snag_render *render, int fd, const char *text, size_t len,
            bool terminal_safe, bool persistent)
{
    return write_role_block(render, fd, "", text, len, 0u,
                             terminal_safe, persistent);
}

void
snag_render_init(struct snag_render *render, unsigned int verbosity)
{
    memset(render, 0, sizeof(*render));
    render->verbosity = verbosity;
    render->history_fd = -1;
    render->markdown = true;
    render->markdown_prose_bullets = true;
    render->view = SNAG_RENDER_ROLLOUT;
    render->public_fd = -1;
    render->stdout_terminal = isatty(STDOUT_FILENO) == 1;
    render->stderr_terminal = isatty(STDERR_FILENO) == 1;
}

void
snag_render_free(struct snag_render *render)
{
    if (!render)
        return;
    if (render->public_item_open)
        (void)snag_render_public_abort(render);
    for (unsigned int view = 0u; view < SNAG_RENDER_VIEW_COUNT; ++view)
        while (render->view_head[view])
            pop_record(render, (enum snag_render_view)view);
    render->rollout_open = NULL;
    if (render->history_fd >= 0)
        (void)close(render->history_fd);
    render->history_fd = -1;
}

void
snag_render_set_markdown(struct snag_render *render, bool enabled)
{
    render->markdown = enabled;
}

void
snag_render_set_color(struct snag_render *render, enum snag_color_mode mode)
{
    const char *term = getenv("TERM");
    bool disabled = mode == SNAG_COLOR_NEVER ||
                    (mode == SNAG_COLOR_AUTO &&
                     (getenv("NO_COLOR") != NULL || !term ||
                      strcmp(term, "dumb") == 0));

    render->color_stdout = !disabled &&
        (mode == SNAG_COLOR_ALWAYS || render->stdout_terminal);
    render->color_stderr = !disabled &&
        (mode == SNAG_COLOR_ALWAYS || render->stderr_terminal);
    if (render->term)
        snag_term_set_color(render->term, render->color_stderr);
}

void
snag_render_attach_term(struct snag_render *render, struct snag_term *term)
{
    render->term = term;
    snag_term_set_color(term, render->color_stderr);
}

int
snag_render_orientation(struct snag_render *render,
                       const char *workspace, const char *id,
                       uint64_t turns, size_t queued, bool resumed)
{
    struct snag_buf line;
    int rc;

    snag_buf_init(&line, 32768u);
    if (resumed) {
        rc = snag_buf_printf(&line,
            SNAJPAGENT_IDENTITY " · resumed · %s · session id %.8s "
            "· %llu turns · %zu queued%s\n",
            workspace, id, (unsigned long long)turns, queued,
            queued ? " paused" : "");
    } else {
        rc = snag_buf_printf(&line, SNAJPAGENT_IDENTITY
                            " · %s · session id %.8s\n",
                            workspace, id);
    }
    if (rc == 0)
        rc = write_role_block(render, STDERR_FILENO, COLOR_AGENT,
                              (char *)line.data, line.len, line.len,
                              render->stderr_terminal, true);
    snag_buf_free(&line);
    return rc;
}

int
snag_render_history(struct snag_render *render,
                    const char *user, const char *assistant)
{
    struct snag_buf line;
    int rc = 0;

    if (!user && !assistant)
        return 0;
    snag_buf_init(&line, 4u * 1024u * 1024u);
    if (snag_buf_append(&line, "--- recent history ---\n", 23u) < 0 ||
        (user &&
         (snag_buf_append(&line, "user: ", 6u) < 0 ||
          snag_buf_append(&line, user, strlen(user)) < 0 ||
          snag_buf_putc(&line, '\n') < 0)))
        rc = -1;
    else if (line.len)
        rc = write_block(render, STDERR_FILENO, (char *)line.data, line.len,
                         render->stderr_terminal, true);
    if (rc == 0 && assistant) {
        if (snag_render_public_begin(render, STDERR_FILENO, "assistant: ") < 0) {
            rc = -1;
        } else if (snag_render_public(render, assistant,
                                     strlen(assistant), NULL) < 0) {
            int saved_errno = errno;
            (void)snag_render_public_abort(render);
            errno = saved_errno;
            rc = -1;
        } else {
            rc = snag_render_public_end(render);
        }
    }
    if (rc == 0)
        rc = write_block(render, STDERR_FILENO, "--- end history ---\n", 20u,
                         false, true);
    snag_buf_free(&line);
    return rc;
}

static int
render_submitted(struct snag_render *render, const char *label, const char *text,
                 bool separate)
{
    struct snag_buf line;
    int rc;

    if (render->term &&
        snag_term_consume_echoed_submission(render->term, label)) {
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
    snag_buf_init(&line, SNAG_MAX_DIRECT_PROMPT * 8u + 64u);
    if (render->public_item_open && !render->public_item_ended_lf) {
        if (snag_buf_putc(&line, '\n') < 0) {
            snag_buf_free(&line);
            return -1;
        }
        render->public_item_ended_lf = true;
    }
    rc = snag_buf_append(&line, label, strlen(label));
    if (rc == 0)
        rc = snag_buf_append(&line, text, strlen(text));
    if (rc == 0 && separate && render->stderr_terminal) {
        size_t len = strlen(text);
        size_t trailing = 0u;

        while (trailing < len && trailing < 2u &&
               text[len - trailing - 1u] == '\n')
            ++trailing;
        while (trailing++ < 2u)
            if (snag_buf_putc(&line, '\n') < 0) {
                rc = -1;
                break;
            }
    } else if (rc == 0) {
        rc = snag_buf_putc(&line, '\n');
    }
    if (rc == 0)
        rc = write_role_block(render, STDERR_FILENO, COLOR_AGENT,
                              (char *)line.data, line.len, strlen(label),
                              render->stderr_terminal, true);
    if (rc == 0 && render->stderr_terminal) {
        render->previous_public_item = false;
        if (render->public_item_open) {
            render->public_item_ended_lf = true;
            render->public_trailing_newlines = separate ? 2u : 1u;
        }
    }
    snag_buf_free(&line);
    return rc;
}

int
snag_render_submitted(struct snag_render *render, const char *label,
                     const char *text)
{
    return render_submitted(render, label, text, false);
}

int
snag_render_input_submitted(struct snag_render *render, const char *label,
                           const char *text)
{
    return render_submitted(render, label, text, true);
}

int
snag_render_before_prompt(struct snag_render *render)
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

static int public_write(struct snag_render *render, const char *text, size_t len);
static int close_public_output(struct snag_render *render);
static int markdown_finish(struct snag_render *render);
static int markdown_abort(struct snag_render *render);
static int markdown_write(struct snag_render *render,
                          const unsigned char *text, size_t len);
static int markdown_table_finish(struct snag_render *render);
static bool public_terminal(const struct snag_render *render);

static bool
markdown_has_style(const struct snag_render *render)
{
    const struct snag_markdown_state *md = &render->markdown_state;

    return render->markdown_highlight || md->heading || md->quote || md->strong ||
           md->emphasis || md->strike || md->inline_code || md->table_header ||
           md->fence != '\0' || md->link_url;
}

static int
markdown_paint_style(struct snag_render *render)
{
    const struct snag_markdown_state *md = &render->markdown_state;
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
    if (render->markdown_highlight) ADD_STYLE(";1;35");
#undef ADD_STYLE
    sequence[len++] = 'm';
    memcpy(render->public_style, sequence, len);
    render->public_style[len] = '\0';
    if (snag_term_write(render->public_fd, sequence, len) < 0)
        return -1;
    render->markdown_state.style_painted = true;
    return 0;
}

static int
markdown_clear_style(struct snag_render *render)
{
    if (!render->markdown_state.style_painted)
        return 0;
    render->markdown_state.style_painted = false;
    render->public_style[0] = '\0';
    return write_literal(render->public_fd, COLOR_RESET);
}

int
snag_render_public_begin(struct snag_render *render, int fd, const char *label)
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
    snag_buf_init(&render->wrap_pending, SNAG_MAX_PUBLIC_ITEM);
    render->public_fd = fd;
    render->public_item_open = true;
    render->public_item_bytes = label_len != 0u;
    render->public_item_ended_lf = label_len && label[label_len - 1u] == '\n';
    render->public_trailing_newlines = 0u;
    render->public_column = label_len ? snag_term_text_width(label, label_len) : 0u;
    render->wrap_has_word = false;
    render->wrap_continuation = false;
    render->wrap_word_open = false;
    render->wrap_break_open = false;
    memset(&render->markdown_state, 0, sizeof(render->markdown_state));
    snag_buf_init(&render->markdown_state.table, SNAG_MAX_PUBLIC_ITEM);
    if (render->public_column == SIZE_MAX)
        goto fail;
    if (label_len) {
        bool colored = color_enabled(render, fd);
        const char *color = COLOR_AGENT;
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
        snag_buf_free(&render->markdown_state.table);
        snag_buf_free(&render->wrap_pending);
        errno = saved_errno;
        return -1;
    }
}

static bool
public_terminal(const struct snag_render *render)
{
    return render->public_fd == STDOUT_FILENO ? render->stdout_terminal :
                                                render->stderr_terminal;
}

static int
public_write(struct snag_render *render, const char *text, size_t len)
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
    if ((terminal ? snag_term_write_safe(render->public_fd, text, len) :
                    snag_term_write(render->public_fd, text, len)) < 0)
        return -1;
    if (terminal && render->term &&
        snag_term_note_output(render->term, text, len, render->public_style) < 0)
        return -1;
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
close_public_output(struct snag_render *render)
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
flush_wrap_pending(struct snag_render *render)
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
                        snag_term_typing_active(render->term);
    if (prompt_interposed) {
        continued_line = render->public_column != 0u;
        render->public_column = 0u;
        if (continued_line) {
            text += leading;
            len -= leading;
        }
        if (!len) {
            snag_buf_reset(&render->wrap_pending);
            render->wrap_has_word = false;
            render->wrap_continuation = false;
            return 0;
        }
    }
    width = snag_term_text_width(text, len);
    columns = render->markdown_measuring ? UINT_MAX :
                                           snag_term_columns(render->term);
    if (width == SIZE_MAX)
        return -1;
    if (columns >= 20u && render->public_column == columns && leading == len) {
        snag_buf_reset(&render->wrap_pending);
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
        if (render->markdown_rendering && render->markdown_state.prose &&
            render->markdown_prose_bullets) {
            if (public_write(render, "  ", 2u) < 0)
                return -1;
            render->public_column = 2u;
        }
        width = snag_term_text_width(text, len);
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
    snag_buf_reset(&render->wrap_pending);
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
write_wrapped(struct snag_render *render, const unsigned char *text, size_t len)
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
            if (snag_buf_append(&render->wrap_pending, text + i, n) < 0)
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
markdown_text(struct snag_render *render, const void *text, size_t len)
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
markdown_repeat(struct snag_render *render, char value, size_t count)
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
markdown_style_changed(struct snag_render *render)
{
    if (flush_wrap_pending(render) < 0 || markdown_clear_style(render) < 0)
        return -1;
    return render->public_output_open ? markdown_paint_style(render) : 0;
}

static int
markdown_flush_delimiter(struct snag_render *render, bool next_word,
                         bool at_end)
{
    struct snag_markdown_state *md = &render->markdown_state;
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
markdown_inline(struct snag_render *render, const unsigned char *text, size_t len)
{
    struct snag_markdown_state *md = &render->markdown_state;
    size_t i = 0u;

    size_t checkpoint_at = 0u;
    while (i < len) {
        if (i >= checkpoint_at) {
            checkpoint_at = i + 1024u;
            if (render_checkpoint(render) < 0)
                return -1;
        }
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
                if (i >= len || i - start >= 1024u)
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
markdown_inline_finish(struct snag_render *render)
{
    struct snag_markdown_state *md = &render->markdown_state;

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
markdown_table_cell_width(struct snag_render *render,
                          const struct markdown_table_cell *cell,
                          size_t *width)
{
    struct snag_render probe;
    int rc;

    if (render_checkpoint(render) < 0)
        return -1;
    memset(&probe, 0, sizeof(probe));
    probe.public_fd = render->public_fd;
    probe.markdown_rendering = true;
    probe.markdown_measuring = true;
    probe.checkpoint = render->checkpoint;
    probe.checkpoint_opaque = render->checkpoint_opaque;
    snag_buf_init(&probe.wrap_pending, SNAG_MAX_PUBLIC_ITEM);
    rc = markdown_inline(&probe, cell->text, cell->len);
    if (rc == 0)
        rc = markdown_inline_finish(&probe);
    if (rc == 0)
        rc = flush_wrap_pending(&probe);
    if (rc == 0)
        *width = probe.public_column;
    snag_buf_free(&probe.wrap_pending);
    return rc;
}

static int
markdown_table_spaces(struct snag_render *render, size_t count)
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
markdown_table_rule(struct snag_render *render, const char *left,
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
markdown_table_cell(struct snag_render *render,
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
markdown_table_grid_row(struct snag_render *render,
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

        if (render_checkpoint(render) < 0 ||
            markdown_table_cell_width(render, cell, &width) < 0 ||
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
markdown_table_render(struct snag_render *render)
{
    struct snag_markdown_state *md = &render->markdown_state;
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
    unsigned int terminal_columns = snag_term_columns(render->term);
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
        if (render_checkpoint(render) < 0)
            return -1;
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

            if (render_checkpoint(render) < 0 ||
                !markdown_table_cells(line, line_len, cells, &count) ||
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

            if (render_checkpoint(render) < 0 ||
                !markdown_table_cells(line, line_len, cells, &count) ||
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
markdown_table_reset(struct snag_markdown_state *md)
{
    snag_buf_reset(&md->table);
    md->table_header_len = 0u;
    md->table_line_start = 0u;
    md->table_line = false;
    md->table_pending = false;
    md->table_active = false;
}

static int
markdown_table_replay(struct snag_render *render)
{
    struct snag_markdown_state *md = &render->markdown_state;
    struct snag_buf saved = md->table;
    int rc;

    snag_buf_init(&md->table, SNAG_MAX_PUBLIC_ITEM);
    md->table_header_len = 0u;
    md->table_line_start = 0u;
    md->table_line = false;
    md->table_pending = false;
    md->table_active = false;
    md->line_start = true;
    md->table_disabled = true;
    rc = markdown_write(render, saved.data, saved.len);
    md->table_disabled = false;
    snag_buf_free(&saved);
    return rc;
}

static int
markdown_table_finish(struct snag_render *render)
{
    struct snag_markdown_state *md = &render->markdown_state;
    int rc;

    if (md->table_line) {
        struct markdown_table_cell cells[MARKDOWN_TABLE_COLUMNS];
        size_t count;

        md->table_line = false;
        if (!markdown_table_cells(md->table.data + md->table_line_start,
                                  md->table.len - md->table_line_start,
                                  cells, &count)) {
            if (md->table_active) {
                struct snag_buf row;

                snag_buf_init(&row, SNAG_MAX_PUBLIC_ITEM);
                if (snag_buf_append(&row,
                                   md->table.data + md->table_line_start,
                                   md->table.len - md->table_line_start) < 0) {
                    snag_buf_free(&row);
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
                snag_buf_free(&row);
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
markdown_table_line_end(struct snag_render *render)
{
    struct snag_markdown_state *md = &render->markdown_state;
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
markdown_table_start_line(struct snag_render *render)
{
    struct snag_markdown_state *md = &render->markdown_state;

    md->table_line_start = md->table.len;
    if (snag_buf_append(&md->table, md->prefix, md->prefix_len) < 0)
        return -1;
    md->prefix_len = 0u;
    md->table_line = true;
    md->line_start = false;
    return 0;
}

static size_t
markdown_prefix_spaces(const struct snag_markdown_state *md)
{
    size_t spaces = 0u;

    while (spaces < md->prefix_len && spaces < 3u &&
           md->prefix[spaces] == ' ')
        ++spaces;
    return spaces;
}

static int
markdown_prefix_literal(struct snag_render *render)
{
    struct snag_markdown_state *md = &render->markdown_state;
    size_t len = md->prefix_len;
    size_t spaces = markdown_prefix_spaces(md);
    bool prose = len != 0u && spaces != len;

    md->prefix_len = 0u;
    md->line_start = false;
    if (prose && !md->prose) {
        md->prose = true;
        if (render->markdown_prose_bullets &&
            markdown_text(render, "• ", strlen("• ")) < 0)
            return -1;
    }
    return markdown_inline(render, (const unsigned char *)md->prefix, len);
}

static int
markdown_code_prefix_literal(struct snag_render *render)
{
    struct snag_markdown_state *md = &render->markdown_state;
    size_t len = md->prefix_len;

    md->prefix_len = 0u;
    md->line_start = false;
    md->prose = false;
    if (markdown_text(render, "│ ", strlen("│ ")) < 0)
        return -1;
    return markdown_text(render, md->prefix, len);
}

static bool
markdown_fence_close(const struct snag_markdown_state *md)
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
markdown_open_fence(struct snag_render *render, bool newline)
{
    struct snag_markdown_state *md = &render->markdown_state;
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
markdown_line_prefix(struct snag_render *render,
                     const unsigned char *text, size_t len)
{
    struct snag_markdown_state *md = &render->markdown_state;
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
markdown_newline(struct snag_render *render)
{
    struct snag_markdown_state *md = &render->markdown_state;
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
markdown_write(struct snag_render *render, const unsigned char *text, size_t len)
{
    struct snag_markdown_state *md = &render->markdown_state;
    size_t i = 0u;

    size_t checkpoint_at = 0u;
    while (i < len) {
        if (i >= checkpoint_at) {
            checkpoint_at = i + 1024u;
            if (render_checkpoint(render) < 0)
                return -1;
        }
        size_t n = utf8_sequence_size(text[i]);

        if (!n || n > len - i) {
            errno = EILSEQ;
            return -1;
        }
        if (text[i] == '\n') {
            if (md->table_line) {
                if (snag_buf_putc(&md->table, '\n') < 0 ||
                    markdown_table_line_end(render) < 0)
                    return -1;
            } else if (markdown_newline(render) < 0) {
                return -1;
            }
        } else if (md->table_line) {
            if (snag_buf_append(&md->table, text + i, n) < 0)
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

            while (i + n < len && n < 1024u && text[i + n] != '\n') {
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
markdown_finish(struct snag_render *render)
{
    struct snag_markdown_state *md = &render->markdown_state;

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
markdown_abort(struct snag_render *render)
{
    struct snag_markdown_state *md = &render->markdown_state;

    if ((md->table_line || md->table_pending || md->table_active) &&
        markdown_table_finish(render) < 0)
        return -1;
    return markdown_clear_style(render);
}

static int
render_public_chunk(struct snag_render *render, const char *text, size_t len,
                  struct snag_buf *delivered)
{
    const unsigned char *input = (const unsigned char *)text;
    struct snag_buf complete;
    size_t complete_max;
    int rc = -1;
    int saved_errno = 0;

    if (!render->public_item_open ||
        !snag_size_add(len, sizeof(render->utf8_pending), &complete_max)) {
        errno = EOVERFLOW;
        return -1;
    }
    snag_buf_init(&complete, complete_max);
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
        if (!snag_utf8_valid(render->utf8_pending, expected, true))
            goto invalid;
        if (snag_buf_append(&complete, render->utf8_pending, expected) < 0)
            goto out;
        render->utf8_pending_len = 0;
    }
    if (complete.len) {
        if (delivered && snag_buf_reserve(delivered, complete.len) < 0)
            goto out;
        if (public_terminal(render) ?
            (render->markdown_rendering ?
             markdown_write(render, complete.data, complete.len) < 0 :
             write_wrapped(render, complete.data, complete.len) < 0) :
            public_write(render, (const char *)complete.data, complete.len) < 0)
            goto out;
        if (delivered && snag_buf_append(delivered, complete.data, complete.len) < 0)
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
    snag_buf_free(&complete);
    if (saved_errno)
        errno = saved_errno;
    return rc;
}

int
snag_render_public(struct snag_render *render, const char *text, size_t len,
                   struct snag_buf *delivered)
{
    if (!len)
        return render_public_chunk(render, text, len, delivered);
    while (len) {
        size_t amount = text_slice(text, len);
        if (render_public_chunk(render, text, amount, delivered) < 0)
            return -1;
        text += amount;
        len -= amount;
        if (len && render_checkpoint(render) < 0)
            return -1;
    }
    return 0;
}

static int
close_public_item(struct snag_render *render, bool discard_incomplete)
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
    snag_buf_free(&render->markdown_state.table);
    snag_buf_free(&render->wrap_pending);
    if (fd == STDOUT_FILENO && had_bytes) {
        render->stdout_item_seen = true;
        render->stdout_item_ended_lf = ended_lf;
    }
    if (had_bytes && !ended_lf &&
        (fd == STDERR_FILENO || render->stderr_terminal) &&
        write_block(render, STDERR_FILENO, "\n", 1u, false, true) < 0)
        rc = -1;
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
snag_render_public_end(struct snag_render *render)
{
    return close_public_item(render, false);
}

int
snag_render_public_abort(struct snag_render *render)
{
    return close_public_item(render, true);
}

static int
rollout_physical_begin(struct snag_render *render,
                       struct snag_render_record *record)
{
    const char *label;

    if (record->physical_open)
        return 0;
    label = record->label_displayed ? NULL : record->label;
    if (snag_render_public_begin(render, record->fd, label) < 0)
        return -1;
    record->physical_open = true;
    record->label_displayed = true;
    return 0;
}

static int
rollout_physical_append(struct snag_render *render,
                        struct snag_render_record *record,
                        const char *text, size_t len)
{
    while (len) {
        size_t amount = text_slice(text, len);
        if (!snag_render_enabled(render, record->presentation)) {
            record->omitted = true;
            if (record->physical_open) {
                if (snag_render_public_abort(render) < 0 ||
                    snag_render_host(render, "… [display omitted]") < 0)
                    return -1;
                record->physical_open = false;
            }
        }
        if (!record->omitted &&
            (rollout_physical_begin(render, record) < 0 ||
             snag_render_public(render, text, amount, NULL) < 0))
            return -1;
        record->displayed += amount;
        text += amount;
        len -= amount;
        if (len && render_checkpoint(render) < 0)
            return -1;
    }
    return 0;
}

int
snag_render_rollout_begin(struct snag_render *render, int fd,
                         const char *label, enum snag_presentation kind)
{
    struct snag_render_record *record;

    if (render->rollout_open || render->public_item_open ||
        (fd != STDOUT_FILENO && fd != STDERR_FILENO)) {
        errno = render->rollout_open || render->public_item_open ? EBUSY : EINVAL;
        return -1;
    }
    record = calloc(1u, sizeof(*record));
    if (!record)
        return -1;
    record->kind = SNAG_RENDER_RECORD_PUBLIC;
    record->presentation = kind;
    record->fd = fd;
    snag_buf_init(&record->text, SNAG_MAX_PUBLIC_ITEM);
    if (label) {
        record->label = snag_strdup_checked(label, 1024u);
        if (!record->label) {
            free_record(record);
            return -1;
        }
    }
    queue_record(render, SNAG_RENDER_ROLLOUT, record);
    render->rollout_open = record;
    return 0;
}

int
snag_render_rollout(struct snag_render *render, const char *text, size_t len,
                   struct snag_buf *delivered)
{
    struct snag_render_record *record = render->rollout_open;
    const unsigned char *input = (const unsigned char *)text;
    struct snag_buf complete;
    size_t complete_max;
    int rc = -1;

    if (!record || record->complete ||
        !snag_size_add(len, sizeof(record->utf8_pending), &complete_max)) {
        errno = record ? EOVERFLOW : EINVAL;
        return -1;
    }
    snag_buf_init(&complete, complete_max);
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
        if (!snag_utf8_valid(record->utf8_pending, expected, true))
            goto invalid;
        if (snag_buf_append(&complete, record->utf8_pending, expected) < 0)
            goto out;
        record->utf8_pending_len = 0u;
    }
    if (complete.len) {
        if (snag_buf_reserve(&record->text, complete.len) < 0 ||
            (delivered && snag_buf_reserve(delivered, complete.len) < 0) ||
            snag_buf_append(&record->text, complete.data, complete.len) < 0)
            goto out;
        if (render->view == SNAG_RENDER_ROLLOUT &&
            rollout_physical_append(render, record,
                                    (const char *)complete.data,
                                    complete.len) < 0)
            goto out;
        if (delivered &&
            snag_buf_append(delivered, complete.data, complete.len) < 0)
            goto out;
    }
    rc = 0;
    goto out;
invalid:
    record->utf8_pending_len = 0u;
    errno = EILSEQ;
out:
    snag_buf_free(&complete);
    return rc;
}

static int
close_rollout_record(struct snag_render *render, bool abort)
{
    struct snag_render_record *record = render->rollout_open;
    bool was_head;
    int rc = 0;

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
    was_head = record == render->view_head[SNAG_RENDER_ROLLOUT];
    if (record->physical_open) {
        if ((abort ? snag_render_public_abort(render) :
                     snag_render_public_end(render)) < 0 && rc == 0)
            rc = -1;
        record->physical_open = false;
    }
    render->rollout_open = NULL;
    if (was_head && record->displayed == record->text.len) {
        pop_record(render, SNAG_RENDER_ROLLOUT);
        if (render->view == SNAG_RENDER_ROLLOUT &&
            flush_view(render, SNAG_RENDER_ROLLOUT) < 0 && rc == 0)
            rc = -1;
    }
    return rc;
}

int
snag_render_rollout_end(struct snag_render *render)
{
    return close_rollout_record(render, false);
}

int
snag_render_rollout_abort(struct snag_render *render)
{
    return close_rollout_record(render, true);
}

static int
render_message(struct snag_render *render, const char *message,
               const char *color)
{
    struct snag_buf line;
    int rc;

    snag_buf_init(&line, 16384u);
    rc = snag_buf_printf(&line, SNAJPAGENT_NAME ": %s\n", message);
    if (rc == 0)
        rc = write_role_block(render, STDERR_FILENO, color,
                              (char *)line.data, line.len, line.len,
                              render->stderr_terminal, true);
    snag_buf_free(&line);
    return rc;
}

int
snag_render_error_ctx(struct snag_render *render, const char *message)
{
    return render_message(render, message, COLOR_ERROR);
}

int
snag_render_warning_ctx(struct snag_render *render, const char *message)
{
    return render_message(render, message, COLOR_WARNING);
}

int
snag_render_host(struct snag_render *render, const char *text)
{
    size_t len = strlen(text);
    struct snag_buf line;
    int rc;

    snag_buf_init(&line, 4u * 1024u * 1024u);
    rc = snag_buf_append(&line, text, len);
    if (rc == 0 && (len == 0u || text[len - 1u] != '\n'))
        rc = snag_buf_putc(&line, '\n');
    if (rc == 0)
        rc = write_role_block(render, STDERR_FILENO, COLOR_HOST,
                              (char *)line.data, line.len,
                              first_line_len((char *)line.data, line.len),
                              render->stderr_terminal, true);
    snag_buf_free(&line);
    return rc;
}

int
snag_render_runtime(struct snag_render *render, const char *text)
{
    size_t len;
    struct snag_buf line;
    int rc;

    if (!snag_render_enabled(render, SNAG_PRESENT_DEBUG))
        return 0;
    len = strlen(text);
    snag_buf_init(&line, 4u * 1024u * 1024u);
    rc = snag_buf_append(&line, text, len);
    if (rc == 0 && (!len || text[len - 1u] != '\n'))
        rc = snag_buf_putc(&line, '\n');
    if (rc == 0)
        rc = write_optional_block(render, SNAG_PRESENT_DEBUG, COLOR_META,
                        (char *)line.data, line.len,
                        first_line_len((char *)line.data, line.len));
    snag_buf_free(&line);
    return rc;
}

static const char *
irc_event_word(enum snag_irc_event_kind kind)
{
    switch (kind) {
    case SNAG_IRC_CONNECTED: return "connected";
    case SNAG_IRC_DISCONNECTED: return "disconnected";
    case SNAG_IRC_JOIN: return "joined";
    case SNAG_IRC_PART: return "left";
    case SNAG_IRC_QUIT: return "quit";
    case SNAG_IRC_NICK: return "is now known as";
    case SNAG_IRC_TOPIC: return "set topic";
    case SNAG_IRC_MODE: return "set mode";
    case SNAG_IRC_HISTORY_READY: return "history synchronized";
    case SNAG_IRC_MESSAGE: case SNAG_IRC_NOTICE: break;
    }
    return "event";
}

static int
irc_piece(struct snag_render *render, const char *text, bool safe)
{
    size_t len = strlen(text);

    if ((safe ? snag_term_write_safe(STDERR_FILENO, text, len) :
                snag_term_write(STDERR_FILENO, text, len)) < 0)
        return -1;
    if (len && text[0] == '\033')
        (void)snag_strcpy(render->public_style, sizeof(render->public_style), text);
    else if (render->term)
        return snag_term_note_output(render->term, text, len, render->public_style);
    return 0;
}

static struct snag_irc_markdown_state *
irc_markdown_state(struct snag_render *render, const struct snag_irc_event *event,
                   bool allocate)
{
    struct snag_irc_markdown_state *empty = NULL;

    for (size_t i = 0u; i < SNAG_RENDER_IRC_MARKDOWN_STATES; ++i) {
        struct snag_irc_markdown_state *state = &render->irc_markdown[i];
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
irc_markdown_lifecycle(struct snag_render *render,
                       const struct snag_irc_event *event)
{
    bool endpoint_reset = event->kind == SNAG_IRC_CONNECTED ||
                          event->kind == SNAG_IRC_DISCONNECTED;
    bool nick_reset = event->kind == SNAG_IRC_PART ||
                      event->kind == SNAG_IRC_QUIT ||
                      event->kind == SNAG_IRC_NICK;

    if (!endpoint_reset && !nick_reset)
        return;
    for (size_t i = 0u; i < SNAG_RENDER_IRC_MARKDOWN_STATES; ++i) {
        struct snag_irc_markdown_state *state = &render->irc_markdown[i];
        if (state->fence && strcmp(state->endpoint, event->endpoint) == 0 &&
            (endpoint_reset || strcmp(state->nick, event->nick) == 0))
            memset(state, 0, sizeof(*state));
    }
}

static int
render_irc_markdown(struct snag_render *render,
                    const struct snag_irc_event *event, size_t column,
                    bool highlight)
{
    struct snag_irc_markdown_state *saved =
        irc_markdown_state(render, event, false);
    struct snag_render body;
    int rc = -1;

    snag_render_init(&body, render->verbosity);
    body.stderr_terminal = render->stderr_terminal;
    body.color_stderr = render->color_stderr;
    body.markdown = true;
    body.markdown_prose_bullets = false;
    body.markdown_highlight = highlight;
    body.term = render->term;
    body.checkpoint = render->checkpoint;
    body.checkpoint_opaque = render->checkpoint_opaque;
    if (snag_render_public_begin(&body, STDERR_FILENO, NULL) < 0)
        return -1;
    body.public_column = column;
    body.markdown_preserve_fence = true;
    if (saved) {
        body.markdown_state.fence = saved->fence;
        body.markdown_state.fence_len = saved->fence_len;
    }
    if (snag_render_public(&body, event->text, strlen(event->text), NULL) < 0 ||
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
    if (!body.public_item_bytes && public_write(&body, "\n", 1u) < 0)
        goto out;
    rc = snag_render_public_end(&body);
    return rc;
out:
    body.markdown_rendering = false;
    (void)snag_render_public_abort(&body);
    return -1;
}

static int
render_irc_event_now(struct snag_render *render,
                     const struct snag_irc_event *event)
{
    char when[16u];
    char prefix[768u];
    char source[SNAG_CONFIG_IRC_ENDPOINT_MAX + SNAG_CONFIG_IRC_ROOM_MAX + 32u] = {0};
    time_t seconds;
    struct tm tm;
    const char *nick_color;
    const char *body_color = COLOR_RESET;
    bool highlight = false;
    bool colored;
    bool markdown_body;
    int n;
    int rc = -1;

    if (!render || !event) {
        errno = EINVAL;
        return -1;
    }
    irc_markdown_lifecycle(render, event);
    if (render->term && render->term->destinations) {
        const struct snag_irc_destinations *destinations = render->term->destinations;
        const struct snag_irc_destination *origin = NULL;
        for (size_t i = 0u; i < destinations->count; ++i)
            if (strcmp(destinations->items[i].endpoint, event->endpoint) == 0 &&
                (!event->room[0] || strcmp(destinations->items[i].room, event->room) == 0))
                origin = &destinations->items[i];
        highlight = origin &&
            (event->kind == SNAG_IRC_MESSAGE || event->kind == SNAG_IRC_NOTICE) &&
            (snag_irc_nick_mentioned(event->text, origin->operator) ||
             snag_irc_nick_mentioned(event->text, origin->model));
        if (origin && destinations->count > 1u)
            (void)snprintf(source, sizeof(source), "[%u] ", origin->target.id);
        else if (!origin && strcmp(event->endpoint, "local") != 0)
            (void)snprintf(source, sizeof(source), "[%s %s] ", event->endpoint, event->room);
    }
    seconds = (time_t)(event->timestamp_ms / 1000u);
    if (!snag_localtime(&seconds, &tm) ||
        strftime(when, sizeof(when), "%H:%M:%S", &tm) == 0)
        memcpy(when, "--:--:--", 9u);
    colored = render->color_stderr;
    nick_color = event->op || highlight ? COLOR_OPERATOR : COLOR_AGENT;
    if (highlight)
        body_color = COLOR_OPERATOR;
    if (output_begin(render, true) < 0)
        return -1;
    if (colored && irc_piece(render, highlight ? COLOR_OPERATOR : COLOR_META, false) < 0)
        goto out;
    n = snprintf(prefix, sizeof(prefix), "%s%s%s ", source, when,
                 event->historical ? " history" : "");
    if (n < 0 || (size_t)n >= sizeof(prefix) ||
        irc_piece(render, prefix, true) < 0)
        goto out;
    if (colored && (irc_piece(render, COLOR_RESET, false) < 0 ||
                    irc_piece(render, nick_color, false) < 0))
        goto out;
    if (event->kind == SNAG_IRC_MESSAGE || event->kind == SNAG_IRC_NOTICE) {
        n = snprintf(prefix, sizeof(prefix), "%s%s%s ",
                     event->kind == SNAG_IRC_NOTICE ? "-" : "",
                     event->op ? "@" : "", event->nick);
        if (n < 0 || (size_t)n >= sizeof(prefix) ||
            irc_piece(render, prefix, true) < 0)
            goto out;
        if (colored && irc_piece(render, body_color, false) < 0)
            goto out;
        if (irc_piece(render,
                event->kind == SNAG_IRC_NOTICE ? "- " : "› ", true) < 0)
            goto out;
        markdown_body = event->kind == SNAG_IRC_MESSAGE && render->markdown &&
                        render->stderr_terminal && !event->op;
        if (markdown_body) {
            char visible[1024u];
            size_t column;

            n = snprintf(visible, sizeof(visible), "%s%s%s %s%s%s %s ",
                         source, when, event->historical ? " history" : "",
                         event->kind == SNAG_IRC_NOTICE ? "-" : "",
                         event->op ? "@" : "", event->nick,
                         event->kind == SNAG_IRC_NOTICE ? "-" : "›");
            if (n < 0 || (size_t)n >= sizeof(visible))
                goto out;
            column = snag_term_text_width(visible, (size_t)n);
            if (column == SIZE_MAX)
                goto out;
            if (render_irc_markdown(render, event,
                    column % snag_term_columns(render->term), highlight) < 0)
                goto out;
            rc = 0;
            goto out;
        }
        if (irc_piece(render, event->text, true) < 0)
            goto out;
    } else {
        const char *word = event->kind == SNAG_IRC_TOPIC && !event->nick[0] ?
                           "topic" : irc_event_word(event->kind);

        n = snprintf(prefix, sizeof(prefix), "· %s%s%s%s",
                     event->op ? "@" : "", event->nick,
                     event->nick[0] ? " " : "", word);
        if (n < 0 || (size_t)n >= sizeof(prefix) ||
            irc_piece(render, prefix, true) < 0)
            goto out;
        if (colored && irc_piece(render, COLOR_RESET, false) < 0)
            goto out;
        if (event->text[0] &&
            (irc_piece(render, " · ", true) < 0 ||
             irc_piece(render, event->text, true) < 0))
            goto out;
    }
    if (colored && irc_piece(render, COLOR_RESET, false) < 0)
        goto out;
    if (irc_piece(render, "\n", false) < 0)
        goto out;
    rc = 0;
out:
    if (colored)
        (void)irc_piece(render, COLOR_RESET, false);
    if (output_end(render) < 0 && rc == 0)
        rc = -1;
    return rc;
}

int
snag_render_irc_event(struct snag_render *render,
                     const struct snag_irc_event *event)
{
    struct snag_render_record *record;

    if (!render || !event) {
        errno = EINVAL;
        return -1;
    }
    struct snag_render_source source = render->irc_source;
    render->irc_source = (struct snag_render_source){0};
    if (render->view == SNAG_RENDER_CHAT)
        return render_irc_event_now(render, event);
    record = calloc(1u, sizeof(*record));
    if (!record)
        return -1;
    record->kind = SNAG_RENDER_RECORD_IRC;
    record->source = source;
    record->irc_kind = event->kind;
    if (!source.len) {
        record->irc = malloc(sizeof(*record->irc));
        if (!record->irc) {
            free_record(record);
            return -1;
        }
        *record->irc = *event;
    }
    queue_record(render, SNAG_RENDER_CHAT, record);
    return 0;
}

static int
render_irc_record(struct snag_render *render, const struct snag_render_record *record)
{
    if (record->irc)
        return render_irc_event_now(render, record->irc);
    json_t *event = source_event(render, record->source);
    json_t *data = json_object_get(event, "data");
    struct snag_irc_event irc = {.kind = record->irc_kind};
    int rc = -1;
    const char *value;
    if (!event || snag_json_integer_u64(data, "timestamp_ms", &irc.timestamp_ms) < 0)
        goto out;
#define IRC_FIELD(member) do { \
    value = snag_json_string(data, #member); \
    if (!value || strlen(value) >= sizeof(irc.member)) goto out; \
    memcpy(irc.member, value, strlen(value) + 1u); \
} while (0)
    IRC_FIELD(endpoint);
    IRC_FIELD(room);
    IRC_FIELD(nick);
    IRC_FIELD(text);
#undef IRC_FIELD
    irc.historical = json_is_true(json_object_get(data, "historical"));
    irc.op = json_is_true(json_object_get(data, "op"));
    rc = render_irc_event_now(render, &irc);
out:
    if (event)
        json_decref(event);
    return rc;
}

static int
flush_view(struct snag_render *render, enum snag_render_view view)
{
    while (render->view_head[view]) {
        struct snag_render_record *record = render->view_head[view];
        int rc;

        if (record->kind == SNAG_RENDER_RECORD_BLOCK) {
            rc = write_role_block(render, record->fd, record->color,
                                  (const char *)record->text.data,
                                  record->text.len, record->colored_len,
                                  record->terminal_safe, record->persistent);
        } else if (record->kind == SNAG_RENDER_RECORD_IRC) {
            rc = render_irc_record(render, record);
        } else if (record->kind == SNAG_RENDER_RECORD_TOOL) {
            rc = render_tool_record(render, record);
        } else {
            rc = 0;
            if (record->source.len && !record->text.data) {
                json_t *event = source_event(render, record->source);
                json_t *data = json_object_get(event, "data");
                json_t *items = json_object_get(data, "items");
                if (!items)
                    items = json_object_get(data, "partial_public");
                json_t *text = json_object_get(json_array_get(items, record->source_item), "text");
                if (!json_is_string(text)) {
                    if (event)
                        json_decref(event);
                    errno = EPROTO;
                    return -1;
                }
                snag_buf_init(&record->text, SNAG_MAX_PUBLIC_ITEM);
                rc = snag_buf_append(&record->text, json_string_value(text), json_string_length(text));
                json_decref(event);
            }
            if (rc == 0 && record->displayed < record->text.len)
                rc = rollout_physical_append(
                    render, record,
                    (const char *)record->text.data + record->displayed,
                    record->text.len - record->displayed);
            if (rc == 0 && record->complete && record->physical_open) {
                rc = record->aborted ? snag_render_public_abort(render) :
                                       snag_render_public_end(render);
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
        if (render->view_head[view] && render_checkpoint(render) < 0)
            return -1;
    }
    return 0;
}

enum snag_render_view
snag_render_view(const struct snag_render *render)
{
    return render ? render->view : SNAG_RENDER_ROLLOUT;
}

int
snag_render_set_view(struct snag_render *render, enum snag_render_view view)
{
    static const char *const boundaries[SNAG_RENDER_VIEW_COUNT] = {
        "── chat ──\n", "── rollout ──\n"
    };
    struct snag_render_record *open;

    if (!render || (view != SNAG_RENDER_CHAT && view != SNAG_RENDER_ROLLOUT)) {
        errno = EINVAL;
        return -1;
    }
    if (render->view == view)
        return 0;
    open = render->rollout_open;
    if (open && open->physical_open) {
        /* Finish the visible fragment so buffered Markdown/wrap text is not
         * lost when the logical stream resumes after visiting chat. */
        if (snag_render_public_end(render) < 0)
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

static int
tool_body(struct snag_render *render, const struct snag_render_block *block)
{
    const char *label = block->body_kind == SNAG_PRESENT_ARGUMENTS ? "arguments" : "output";
    size_t offset = 0u, count = 0u;
    bool truncated = block->truncated;

    if (!block->body.len || !snag_render_enabled(render, block->body_kind))
        return 0;
    char header[32];
    (void)snprintf(header, sizeof(header), "  %s:\n", label);
    if (write_block(render, STDERR_FILENO, header, strlen(header), true, true) < 0)
        return -1;
    while (offset < block->body.len) {
        size_t limit = snag_render_enabled(render, block->body_kind) ?
                       snag_presentation_limit(block->body_kind, render->verbosity) : 0u;
        size_t end = offset, characters = 0u;
        while (end < block->body.len && end - offset < 1024u && count + characters < limit) {
            size_t n = snag_utf8_size(block->body.data[end]);
            if (!n || n > block->body.len - end) {
                errno = EILSEQ;
                return -1;
            }
            end += n;
            ++characters;
        }
        if (end == offset) {
            truncated = true;
            break;
        }
        if (write_block(render, STDERR_FILENO, (const char *)block->body.data + offset,
                         end - offset, true, true) < 0)
            return -1;
        offset = end;
        count += characters;
        if (render_checkpoint(render) < 0)
            return -1;
    }
    if (offset && block->body.data[offset - 1u] != '\n' &&
        write_literal(STDERR_FILENO, "\n") < 0)
        return -1;
    if (truncated) {
        (void)snprintf(header, sizeof(header), "… [%s truncated]\n", label);
        return write_block(render, STDERR_FILENO, header, strlen(header), true, true);
    }
    return 0;
}

int
snag_render_tool_block(struct snag_render *render, const struct snag_render_block *block)
{
    static const char *const colors[] = {
        COLOR_ACTIVITY, COLOR_SUCCESS, COLOR_WARNING, COLOR_ERROR
    };

    if (!snag_render_enabled(render, SNAG_PRESENT_TOOL))
        return 0;
    if (write_role_block(render, STDERR_FILENO,
                      colors[block->role], (const char *)block->text.data,
                      block->text.len, block->colored_len,
                      render->stderr_terminal, true) < 0 || render_checkpoint(render) < 0)
        return -1;
    if (block->context.len && snag_render_enabled(render, SNAG_PRESENT_CONTEXT) &&
        write_optional_block(render, SNAG_PRESENT_CONTEXT, "",
                              (const char *)block->context.data,
                              block->context.len, 0u) < 0)
        return -1;
    return tool_body(render, block);
}

static json_t *
source_event(struct snag_render *render, struct snag_render_source source)
{
    struct snag_buf text;
    json_t *event = NULL;
    char error[128];

    if (source.offset < 0 || !source.len || source.len > SNAG_MAX_EVENT_LINE) {
        errno = EINVAL;
        return NULL;
    }
    snag_buf_init(&text, source.len);
    if (snag_buf_reserve(&text, source.len) < 0)
        goto out;
    while (text.len < source.len) {
        size_t want = source.len - text.len;
        if (want > 8192u)
            want = 8192u;
        ssize_t n = snag_pread(render->history_fd, text.data + text.len, want,
                          source.offset + (int64_t)text.len);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0 || render_checkpoint(render) < 0)
            goto out;
        text.len += (size_t)n;
    }
    event = snag_json_load_strict(text.data, text.len, text.max, error, sizeof(error));
out:
    snag_buf_free(&text);
    return event;
}

static int
render_process_chunks(struct snag_render *render, const json_t *ref,
                       uint32_t max_output_bytes, int64_t result_offset)
{
    const char *handle = snag_json_string(ref, "handle");
    uint64_t start, end, from[2], to[2];
    size_t displayed = 0u, characters = 0u;
    struct snag_buf line;
    bool truncated = false;
    int rc = -1;
    if (!handle || snag_json_integer_u64(ref, "log_start", &start) < 0 ||
        snag_json_integer_u64(ref, "log_end", &end) < 0 || start > end ||
        end > (uint64_t)result_offset ||
        snag_json_integer_u64(ref, "stdout_start", &from[0]) < 0 ||
        snag_json_integer_u64(ref, "stdout_end", &to[0]) < 0 ||
        snag_json_integer_u64(ref, "stderr_start", &from[1]) < 0 ||
        snag_json_integer_u64(ref, "stderr_end", &to[1]) < 0)
        return -1;
    snag_buf_init(&line, SNAG_MAX_EVENT_LINE);
    while (start < end && snag_render_enabled(render, SNAG_PRESENT_OUTPUT)) {
        unsigned char input[8192];
        size_t want = end - start > sizeof(input) ? sizeof(input) : (size_t)(end - start);
        ssize_t n = snag_pread(render->history_fd, input, want, (int64_t)start);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0 || render_checkpoint(render) < 0)
            goto out;
        start += (uint64_t)n;
        for (ssize_t i = 0; i < n; ++i) {
            if (input[i] != '\n') {
                if (snag_buf_putc(&line, input[i]) < 0)
                    goto out;
                continue;
            }
            char error[128];
            json_t *event = snag_json_load_strict(line.data, line.len, line.max, error, sizeof(error));
            if (!event)
                goto out;
            const json_t *data = json_object_get(event, "data");
            const char *type = snag_json_string(event, "type");
            const char *id = snag_json_string(data, "handle");
            if (type && !strcmp(type, "process_output") && id && !strcmp(id, handle)) {
                uint64_t stream, offset;
                const char *text = snag_json_string(data, "data");
                const char *encoding = snag_json_string(data, "encoding");
                if (!text || !encoding || snag_json_integer_u64(data, "stream", &stream) < 0 || stream > 1u ||
                    snag_json_integer_u64(data, "offset", &offset) < 0) {
                    json_decref(event);
                    goto out;
                }
                if (offset >= from[stream] && offset < to[stream]) {
                    size_t shown = 0u, len = strlen(text), count = 0u;
                    size_t limit = snag_presentation_limit(SNAG_PRESENT_OUTPUT, render->verbosity);
                    while (shown < len && characters + count < limit &&
                           (!max_output_bytes || displayed + shown < max_output_bytes)) {
                        size_t width = snag_utf8_size((unsigned char)text[shown]);
                        if (!width || width > len - shown ||
                            (max_output_bytes && width > max_output_bytes - displayed - shown))
                            break;
                        shown += width;
                        ++count;
                    }
                    struct snag_render_block block = {.body_kind = SNAG_PRESENT_OUTPUT};
                    snag_buf_init(&block.body, shown + 128u);
                    int pr = snag_buf_printf(&block.body, "[%.8s %s%s]\n", handle,
                        stream ? "stderr" : "stdout", !strcmp(encoding, "base64") ? " base64" : "");
                    if (pr == 0)
                        pr = snag_buf_append(&block.body, text, shown);
                    if (pr == 0 && shown)
                        pr = tool_body(render, &block);
                    snag_buf_free(&block.body);
                    displayed += shown;
                    characters += count;
                    truncated = shown < len;
                    if (pr < 0) {
                        json_decref(event);
                        goto out;
                    }
                }
            }
            json_decref(event);
            snag_buf_reset(&line);
            if (truncated) {
                rc = write_optional_block(render, SNAG_PRESENT_OUTPUT, "",
                    "… [output truncated; complete bytes retained in journal]\n",
                    strlen("… [output truncated; complete bytes retained in journal]\n"), 0u);
                goto out;
            }
        }
    }
    rc = 0;
out:
    snag_buf_free(&line);
    return rc;
}

static int
render_tool_record(struct snag_render *render, const struct snag_render_record *record)
{
    json_t *event = NULL, *response = NULL, *data, *items;
    struct snag_render_block block;
    int rc = -1;

    if (!snag_render_enabled(render, SNAG_PRESENT_TOOL))
        return 0;
    event = source_event(render, record->source);
    response = source_event(render, record->response);
    if (!event || !response)
        goto out;
    data = json_object_get(event, "data");
    const char *status = snag_json_string(json_object_get(data, "result"), "status");
    if (!record->tool_start && status && strcmp(status, "not_run") == 0) {
        rc = 0;
        goto out;
    }
    items = json_object_get(json_object_get(response, "data"), "items");
    const char *id = snag_json_string(data, "call_id");
    for (size_t i = 0u; id && i < json_array_size(items); ++i) {
        json_t *item = json_array_get(items, i);
        const char *call_id = snag_json_string(item, "call_id");
        const char *name = snag_json_string(item, "name");
        if (!call_id || !name || strcmp(call_id, id) != 0)
            continue;
        unsigned int columns = render->term ? render->term->columns : 0u;
        if (record->tool_start) {
            struct snag_response_item call = {
                .name = (char *)name, .arguments = json_object_get(item, "arguments")
            };
            (void)snag_strcpy(call.call_id, sizeof(call.call_id), call_id);
            const char *workdir = snag_json_string(data, "resolved_workdir");
            rc = snag_render_prepare_tool_start(&block, &call, workdir ? workdir : "?",
                  record->timeout_ms, render->verbosity, columns);
        } else {
            rc = snag_render_prepare_tool_finish(&block, name, json_object_get(data, "result"),
                  record->max_output_bytes, render->verbosity, columns);
        }
        if (rc == 0) {
            rc = snag_render_tool_block(render, &block);
            snag_render_block_free(&block);
            json_t *ref = json_object_get(json_object_get(data, "result"), "output_ref");
            if (rc == 0 && ref && !record->tool_start)
                rc = render_process_chunks(render, ref, record->max_output_bytes, record->source.offset);
        }
        goto out;
    }
    errno = EPROTO;
out:
    if (event)
        json_decref(event);
    if (response)
        json_decref(response);
    return rc;
}

int
snag_render_durable(struct snag_render *render, int fd, struct snag_render_source source,
                    const char *type, uint32_t timeout_ms, uint32_t max_output_bytes)
{
    if (render->history_fd < 0) {
        render->history_fd = snag_dup_read(fd);
        if (render->history_fd < 0)
            return -1;
    }
    if (strcmp(type, "response_completed") == 0) {
        render->response_source = source;
    }
    if (strcmp(type, "irc_event") == 0) {
        render->irc_source = source;
        return 0;
    }
    if (strcmp(type, "response_completed") == 0 ||
        strcmp(type, "response_interrupted") == 0 || strcmp(type, "response_failed") == 0) {
        json_t *event = NULL;
        int rc = 0;
        for (struct snag_render_record *record = render->view_head[SNAG_RENDER_ROLLOUT];
             record; record = record->next) {
            if (record->kind != SNAG_RENDER_RECORD_PUBLIC || record->source.len ||
                !record->complete)
                continue;
            if (!event && !(event = source_event(render, source))) {
                rc = -1;
                break;
            }
            json_t *data = json_object_get(event, "data");
            json_t *items = json_object_get(data, "items");
            if (!items)
                items = json_object_get(data, "partial_public");
            for (size_t i = 0u; i < json_array_size(items); ++i) {
                json_t *text = json_object_get(json_array_get(items, i), "text");
                if (!json_is_string(text) || json_string_length(text) != record->text.len ||
                    memcmp(json_string_value(text), record->text.data, record->text.len) != 0)
                    continue;
                record->source = source;
                record->source_item = i;
                snag_buf_free(&record->text);
                break;
            }
        }
        if (event)
            json_decref(event);
        return rc;
    }
    bool start = strcmp(type, "tool_started") == 0;
    if ((!start && strcmp(type, "tool_finished") != 0) || !render->response_source.len)
        return 0;
    struct snag_render_record *record = calloc(1u, sizeof(*record));
    if (!record)
        return -1;
    record->kind = SNAG_RENDER_RECORD_TOOL;
    record->source = source;
    record->response = render->response_source;
    record->tool_start = start;
    record->timeout_ms = timeout_ms;
    record->max_output_bytes = max_output_bytes;
    queue_record(render, SNAG_RENDER_ROLLOUT, record);
    return render->view == SNAG_RENDER_ROLLOUT ? flush_view(render, render->view) : 0;
}

int
snag_render_event(struct snag_render *render, uint64_t seq, const char *type)
{
    const char *notice = NULL;
    struct snag_buf line;
    int rc = 0;

    if (strcmp(type, "compaction_completed") == 0)
        notice = "Compacted";
    else if (strcmp(type, "goal_started") == 0 ||
             strcmp(type, "goal_reworded") == 0)
        notice = "Goal set";
    else if (strcmp(type, "goal_completed") == 0 ||
             strcmp(type, "goal_cancelled") == 0)
        notice = "Goal cleared";
    bool debug = snag_render_enabled(render, SNAG_PRESENT_DEBUG);
    if (!notice && !debug)
        return 0;
    snag_buf_init(&line, 1024u);
    if (notice) {
        if (snag_buf_printf(&line, "• %s\n", notice) < 0)
            rc = -1;
    }
    if (rc == 0 && notice)
        rc = view_block(render, SNAG_RENDER_ROLLOUT, STDERR_FILENO,
                        COLOR_LIFECYCLE,
                        (char *)line.data, line.len, line.len,
                        render->stderr_terminal, true);
    snag_buf_reset(&line);
    if (rc == 0 && debug) {
        rc = snag_buf_printf(&line, "event › %llu %s synced\n",
                            (unsigned long long)seq, type);
        if (rc == 0)
            rc = write_optional_block(render, SNAG_PRESENT_DEBUG, COLOR_DURABLE,
                                       (char *)line.data, line.len, line.len);
    }
    snag_buf_free(&line);
    return rc;
}

int
snag_render_resume_hint(const struct snag_render *render, const char *command,
                       size_t command_len)
{
    static const char header[] =
        "• You can resume this session with the following command:";
    struct snag_buf block;
    size_t max = command_len;
    bool colored;
    int rc = -1;

    if (!render || !command || !command_len) {
        errno = EINVAL;
        return -1;
    }
    if (!snag_size_add(max, sizeof(header) + 1u, &max)) {
        errno = EOVERFLOW;
        return -1;
    }
    colored = render->color_stderr;
    if (colored &&
        (!snag_size_add(max, sizeof(COLOR_LIFECYCLE) - 1u, &max) ||
         !snag_size_add(max, sizeof(COLOR_RESET) - 1u, &max))) {
        errno = EOVERFLOW;
        return -1;
    }
    snag_buf_init(&block, max);
    if (colored &&
        snag_buf_append(&block, COLOR_LIFECYCLE,
                       sizeof(COLOR_LIFECYCLE) - 1u) < 0)
        goto out;
    if (snag_buf_append(&block, header, sizeof(header) - 1u) < 0)
        goto out;
    if (colored &&
        snag_buf_append(&block, COLOR_RESET, sizeof(COLOR_RESET) - 1u) < 0)
        goto out;
    if (snag_buf_putc(&block, '\n') < 0 ||
        snag_buf_append(&block, command, command_len) < 0 ||
        snag_buf_putc(&block, '\n') < 0)
        goto out;
    rc = snag_term_write(STDERR_FILENO, block.data, block.len);
out:
    snag_buf_free(&block);
    return rc;
}

static bool
diagnostic_text_valid(const char *text, size_t len, bool multiline)
{
    if (!text || !snag_utf8_valid((const unsigned char *)text, len, true))
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
protocol_warning(struct snag_render *render)
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
snag_render_protocol(struct snag_render *render, const char *label,
                    const char *text, size_t len)
{
    struct snag_buf block;
    int rc = -1;

    if (!snag_render_enabled(render, SNAG_PRESENT_PROTOCOL))
        return 0;
    if (!label || !diagnostic_text_valid(label, strlen(label), false) ||
        !diagnostic_text_valid(text, len, true) ||
        len > 2u * 1024u * 1024u) {
        errno = EINVAL;
        return -1;
    }
    if (protocol_warning(render) < 0)
        return -1;
    snag_buf_init(&block, 2u * 1024u * 1024u + 4096u);
    if (snag_buf_printf(&block, "protocol › %s\n", label) < 0 ||
        snag_buf_append(&block, text, len) < 0 ||
        (len && text[len - 1u] != '\n' && snag_buf_putc(&block, '\n') < 0))
        goto out;
    rc = write_optional_block(render, SNAG_PRESENT_PROTOCOL, COLOR_PROTOCOL,
                    (const char *)block.data, block.len,
                    first_line_len((const char *)block.data, block.len));
out:
    snag_buf_free(&block);
    return rc;
}

int
snag_render_transport(struct snag_render *render, char direction,
                     const char *text, size_t len)
{
    struct snag_buf line;
    int rc = -1;

    if (!snag_render_enabled(render, SNAG_PRESENT_WIRE))
        return 0;
    if ((direction != '>' && direction != '<') ||
        !diagnostic_text_valid(text, len, false) || len > 64u * 1024u) {
        errno = EINVAL;
        return -1;
    }
    if (protocol_warning(render) < 0)
        return -1;
    snag_buf_init(&line, 64u * 1024u + 4u);
    if (snag_buf_putc(&line, (unsigned char)direction) < 0 ||
        snag_buf_putc(&line, ' ') < 0 ||
        snag_buf_append(&line, text, len) < 0 || snag_buf_putc(&line, '\n') < 0)
        goto out;
    rc = write_optional_block(render, SNAG_PRESENT_WIRE, COLOR_TRANSPORT,
                    (const char *)line.data, line.len,
                    line.len > 2u ? 2u : line.len);
out:
    snag_buf_free(&line);
    return rc;
}

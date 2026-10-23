/* SPDX-License-Identifier: GPL-2.0-only */
#include "vm_terminal.h"
#include "process_host.h"
#include "screen_wire.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifndef _WIN32
#include <sys/ioctl.h>
#include <termios.h>
#include <vterm.h>

#if !defined(SNAJPAGENT_VTERM_ABI) || SNAJPAGENT_VTERM_ABI != 1
#error "the snajpagent-vterm dependency is required; see DEPENDENCIES.md"
#endif

extern char **environ;

struct snag_vm_terminal {
    VTerm *terminal;
    VTermScreen *screen;
    struct snag_buf input, transaction;
    size_t written;
    struct snag_child child;
    int fd;
    bool visible, mouse, paste, checkpoint_current;
    int error;
    uint64_t checkpoint, checkpoint_deadline, frame;
    json_t *checkpoint_snapshot;
    char probe_reply[32], copy_reply[96];
    int (*emit)(void *, const void *, size_t);
    void *opaque;
};

static void
terminal_output(const char *bytes, size_t length, void *opaque)
{
    struct snag_vm_terminal *terminal = opaque;
    if (snag_buf_append(&terminal->input, bytes, length) < 0) terminal->error = errno;
}

static int
terminal_emit(struct snag_vm_terminal *terminal, const void *bytes, size_t length)
{
    if (terminal->emit && terminal->emit(terminal->opaque, bytes, length) < 0)
        terminal->error = errno;
    return 1;
}

static void
terminal_copy_begin(struct snag_vm_terminal *terminal)
{
    static const char prefix[] = "\033]2;" SNAG_SCREEN_PREFIX "BEGIN:";
    size_t length = sizeof(prefix) - 1u;
    const char *bytes = (char *)terminal->transaction.data;
    if (terminal->transaction.len < length + 42u || memcmp(bytes, prefix, length)) return;
    bytes += length;
    if (bytes[8] != ':' || bytes[41] != ':') return;
    unsigned long identity[5];
    for (size_t i = 0u; i < 5u; ++i) {
        char number[9];
        memcpy(number, bytes + i * 8u + (i ? 1u : 0u), 8u);
        number[8] = 0;
        if (strspn(number, "0123456789abcdef") != 8u) return;
        identity[i] = strtoul(number, NULL, 16);
    }
    (void)snprintf(terminal->copy_reply, sizeof(terminal->copy_reply),
        "\033[>9003;%lu;%lu;%lu;%lu;%lu;", identity[0], identity[1], identity[2],
        identity[3], identity[4]);
}

static int
terminal_osc(int command, VTermStringFragment fragment, void *opaque)
{
    struct snag_vm_terminal *terminal = opaque;
    if (command != 0 && command != 1 && command != 2 && command != 52) return 0;
    if (fragment.initial) {
        snag_buf_reset(&terminal->transaction);
        if (snag_buf_printf(&terminal->transaction, "\033]%d;", command) < 0)
            terminal->error = errno;
    }
    if (!terminal->error && snag_buf_append(&terminal->transaction, fragment.str, fragment.len) < 0)
        terminal->error = errno;
    if (fragment.final && !terminal->error) {
        if (snag_buf_append(&terminal->transaction, "\033\\", 2u) < 0) terminal->error = errno;
        else {
            static const char prefix[] = "\033]2;" SNAG_SCREEN_PREFIX "DRAFT:";
            static const char frame[] = "\033]2;" SNAG_SCREEN_PREFIX "FRAME:";
            const char *bytes = (const char *)terminal->transaction.data;
            size_t length = terminal->transaction.len;
            if (length >= sizeof(frame) + 2u &&
                !memcmp(bytes, frame, sizeof(frame) - 1u)) {
                size_t digits = length - (sizeof(frame) - 1u) - 2u;
                char number[21];
                if (digits && digits < sizeof(number)) {
                    memcpy(number, bytes + sizeof(frame) - 1u, digits);
                    number[digits] = '\0';
                    uint64_t serial;
                    if (strspn(number, "0123456789") == digits &&
                        snag_parse_count(number, &serial) == 0 && serial <= INT64_MAX &&
                        serial > terminal->frame) terminal->frame = serial;
                }
            } else if (terminal->transaction.len > sizeof(prefix) - 1u &&
                !memcmp(bytes, prefix, sizeof(prefix) - 1u)) {
                char expected[64];
                int size = snprintf(expected, sizeof(expected), "%s%llu:", prefix,
                    (unsigned long long)terminal->checkpoint);
                if (size > 0 && (size_t)size < sizeof(expected) &&
                    terminal->transaction.len > (size_t)size + 2u &&
                    !memcmp(bytes, expected, (size_t)size)) {
                    json_t *snapshot = json_loadb(bytes + size,
                        terminal->transaction.len - (size_t)size - 2u,
                        JSON_REJECT_DUPLICATES, NULL);
                    if (snapshot) {
                        json_decref(terminal->checkpoint_snapshot);
                        terminal->checkpoint_snapshot = snapshot;
                    }
                }
            } else {
                terminal_copy_begin(terminal);
                terminal_emit(terminal, terminal->transaction.data, terminal->transaction.len);
            }
        }
        snag_buf_reset(&terminal->transaction);
    }
    return 1;
}

static int
terminal_dcs(const char *command, size_t command_length,
    VTermStringFragment fragment, void *opaque)
{
    struct snag_vm_terminal *terminal = opaque;
    if (fragment.initial) {
        snag_buf_reset(&terminal->transaction);
        if (snag_buf_append(&terminal->transaction, "\033P", 2u) < 0 ||
            (command_length && snag_buf_append(&terminal->transaction,
                command, command_length) < 0)) terminal->error = errno;
    }
    if (!terminal->error && snag_buf_append(&terminal->transaction,
        fragment.str, fragment.len) < 0) terminal->error = errno;
    if (fragment.final) {
        if (!terminal->error) {
            if (snag_buf_append(&terminal->transaction, "\033\\", 2u) < 0)
                terminal->error = errno;
            else terminal_emit(terminal,
                terminal->transaction.data, terminal->transaction.len);
        }
        snag_buf_reset(&terminal->transaction);
    }
    return 1;
}

int
snag_vm_terminal_checkpoint(struct snag_vm_terminal *terminal, bool force)
{
    if (terminal->paste) return snag_errno(EAGAIN);
    if (!force && terminal->checkpoint_current) {
        if (!terminal->checkpoint_snapshot && snag_monotonic_ms() >= terminal->checkpoint_deadline)
            return snag_errno(ETIMEDOUT);
        return 0;
    }
    terminal->checkpoint_deadline = snag_monotonic_ms() + 5000u;
    terminal->checkpoint_current = true;
    json_decref(terminal->checkpoint_snapshot);
    terminal->checkpoint_snapshot = NULL;
    if (terminal->checkpoint == INT64_MAX) return snag_errno(EOVERFLOW);
    return snag_buf_printf(&terminal->input, "\033[9004;%llu~",
        (unsigned long long)++terminal->checkpoint);
}

json_t *
snag_vm_terminal_checkpoint_snapshot(struct snag_vm_terminal *terminal)
{
    json_t *snapshot = terminal->checkpoint_snapshot;
    if (snapshot) {
        terminal->checkpoint_snapshot = NULL;
        terminal->checkpoint_current = false;
    }
    return snapshot;
}

static int
terminal_csi(const char *leader, const long *arguments, int count, const char *intermediate,
    char command, void *opaque)
{
    struct snag_vm_terminal *terminal = opaque;
    bool clipboard = leader && !strcmp(leader, ">") && count &&
        CSI_ARG(arguments[0]) == 9003 && command == 'c';
    bool upload = leader && !strcmp(leader, "?") && count == 1 &&
        CSI_ARG(arguments[0]) == 9002 && (command == 'h' || command == 'l');
    bool probe = leader && !strcmp(leader, "?") && count == 2 &&
        arguments[0] == 9001 && arguments[1] >= 0 && arguments[1] < 1000000000 && command == 'n';
    if (intermediate || (!clipboard && !upload && !probe)) return 0;
    if (probe) (void)snprintf(terminal->probe_reply, sizeof(terminal->probe_reply),
        "\033[>%luS", (unsigned long)arguments[1]);
    struct snag_buf sequence = {.max = SIZE_MAX};
    int rc = snag_buf_printf(&sequence, "\033[%s", leader);
    for (int i = 0; !rc && i < count; ++i)
        /* These private transactions have uint32 fields, not SGR subarguments.
         * Bit 31 is payload, not libvterm's CSI_ARG_FLAG_MORE marker. */
        rc = snag_buf_printf(&sequence, "%s%lu", i ? ";" : "", (unsigned long)arguments[i]);
    if (!rc) rc = snag_buf_putc(&sequence, command);
    if (rc < 0) terminal->error = errno;
    else terminal_emit(terminal, sequence.data, sequence.len);
    snag_buf_free(&sequence);
    return 1;
}

static const VTermParserCallbacks parser_filter = {
    .osc = terminal_osc, .dcs = terminal_dcs, .csi = terminal_csi};

static int
terminal_property(VTermProp property, VTermValue *value, void *opaque)
{
    struct snag_vm_terminal *terminal = opaque;
    if (property == VTERM_PROP_CURSORVISIBLE) terminal->visible = value->boolean;
    if (property == VTERM_PROP_MOUSE) terminal->mouse = value->number != VTERM_PROP_MOUSE_NONE;
    return 1;
}

static const VTermScreenCallbacks screen_callbacks = {.settermprop = terminal_property};

struct snag_vm_terminal *
snag_vm_terminal_frontend(const char *program, int channel, unsigned int rows,
    unsigned int columns, int (*emit)(void *, const void *, size_t), void *opaque)
{
    if (channel <= STDERR_FILENO || !rows || !columns ||
        rows > USHRT_MAX || columns > USHRT_MAX) {
        errno = EINVAL;
        return NULL;
    }
    struct snag_vm_terminal *terminal = calloc(1u, sizeof(*terminal));
    if (!terminal) return NULL;
    terminal->fd = -1;
    terminal->emit = emit;
    terminal->opaque = opaque;
    snag_child_init(&terminal->child);
    snag_buf_init(&terminal->input, SIZE_MAX);
    snag_buf_init(&terminal->transaction, SIZE_MAX);
    terminal->terminal = vterm_new((int)rows, (int)columns);
    if (!terminal->terminal) goto fail;
    vterm_set_utf8(terminal->terminal, 1);
    vterm_parser_set_filter(terminal->terminal, &parser_filter, terminal);
    vterm_output_set_callback(terminal->terminal, terminal_output, terminal);
    terminal->screen = vterm_obtain_screen(terminal->terminal);
    if (!terminal->screen) goto fail;
    vterm_screen_set_callbacks(terminal->screen, &screen_callbacks, terminal);
    vterm_screen_enable_altscreen(terminal->screen, 1);
    vterm_screen_enable_reflow(terminal->screen, true);
    vterm_screen_enable_history(terminal->screen);
    vterm_screen_reset(terminal->screen, 1);
    int error = vterm_get_error(terminal->terminal);
    if (error) { errno = error; goto fail; }
    char descriptor[32];
    (void)snprintf(descriptor, sizeof(descriptor), "%d", channel);
    const char *frontend[] = {program, "--internal-ui-frontend", descriptor, NULL};
    /* The pane is the immediate terminal. Do not let inherited backend IDs
     * route clipboard or screen transactions around it into an outer pane. */
    size_t count = 0u;
    while (environ[count]) ++count;
    char **environment = calloc(count + 1u, sizeof(*environment));
    if (!environment) goto fail;
    size_t used = 0u;
    for (size_t i = 0u; i < count; ++i) {
        if (!strncmp(environ[i], "TMUX=", 5u) || !strncmp(environ[i], "TMUX_PANE=", 10u) ||
            !strncmp(environ[i], "STY=", 4u)) continue;
        environment[used++] = environ[i];
    }
    int spawned = snag_child_spawn_terminal_channel(&terminal->child, program,
        frontend, environment, (unsigned short)rows, (unsigned short)columns, channel);
    free(environment);
    if (spawned < 0) goto fail;
    terminal->fd = terminal->child.fd[0];
    if (snag_vm_terminal_resize(terminal, rows, columns) < 0) goto fail;
    return terminal;
fail:
    error = errno;
    snag_vm_terminal_free(terminal);
    errno = error;
    return NULL;
}

int
snag_vm_terminal_fd(const struct snag_vm_terminal *terminal)
{
    return terminal ? terminal->fd : -1;
}

bool
snag_vm_terminal_writing(const struct snag_vm_terminal *terminal)
{
    return terminal && terminal->written < terminal->input.len;
}

uint64_t
snag_vm_terminal_frame(const struct snag_vm_terminal *terminal)
{
    return terminal->frame;
}

int
snag_vm_terminal_step(struct snag_vm_terminal *terminal)
{
    if (terminal->error) return snag_errno(terminal->error);
    bool changed = false;
    if (terminal->fd >= 0 && terminal->written < terminal->input.len) {
        ssize_t count = write(terminal->fd, terminal->input.data + terminal->written,
            terminal->input.len - terminal->written);
        if (count > 0) terminal->written += (size_t)count;
        else if (count < 0 && errno != EAGAIN && errno != EINTR) return -1;
        if (terminal->written == terminal->input.len) {
            terminal->written = 0u;
            snag_buf_reset(&terminal->input);
        }
    }
    if (terminal->fd >= 0) {
        unsigned char bytes[16384];
        ssize_t count = read(terminal->fd, bytes, sizeof(bytes));
        if (count > 0) {
            size_t used = vterm_input_write(terminal->terminal, (char *)bytes, (size_t)count);
            int error = vterm_get_error(terminal->terminal);
            if (error) { terminal->error = error; return snag_errno(error); }
            if (used != (size_t)count) return snag_errno(EIO);
            vterm_screen_flush_damage(terminal->screen);
            changed = true;
        } else if (!count || (count < 0 && errno == EIO)) {
            snag_child_close_stream(&terminal->child, 0u);
            terminal->fd = -1;
            changed = true;
        } else if (errno != EAGAIN && errno != EINTR) return -1;
    }
    if (!terminal->child.reaped) {
        int result = snag_child_exited(&terminal->child);
        if (result > 0) {
            if (snag_child_reap(&terminal->child) < 0) return -1;
            changed = true;
        } else if (result < 0) return -1;
    }
    return changed ? 1 : 0;
}

int
snag_vm_terminal_resize(struct snag_vm_terminal *terminal, unsigned int rows, unsigned int columns)
{
    if (!rows || !columns || rows > USHRT_MAX || columns > USHRT_MAX)
        return snag_errno(EINVAL);
    int old_rows, old_columns;
    vterm_get_size(terminal->terminal, &old_rows, &old_columns);
    if (old_rows == (int)rows && old_columns == (int)columns &&
        terminal->child.rows == rows && terminal->child.columns == columns) return 0;
    struct winsize geometry = {.ws_row = (unsigned short)rows,
        .ws_col = (unsigned short)columns};
    if (terminal->fd >= 0 && ioctl(terminal->fd, TIOCSWINSZ, &geometry) < 0) return -1;
    vterm_set_size(terminal->terminal, (int)rows, (int)columns);
    int error = vterm_get_error(terminal->terminal);
    if (error) { terminal->error = error; return snag_errno(error); }
    terminal->child.rows = (unsigned short)rows;
    terminal->child.columns = (unsigned short)columns;
    return 0;
}

int
snag_vm_terminal_input(struct snag_vm_terminal *terminal, const struct snag_vm_input_event *event)
{
    if (terminal->fd < 0) return snag_errno(EPIPE);
    terminal->checkpoint_current = false;
    if (event->kind == SNAG_VM_PASTE_BEGIN) terminal->paste = true;
    if (event->kind == SNAG_VM_PASTE_END ||
        (event->kind == SNAG_VM_KEY && event->key == 'c' &&
            (event->modifiers & SNAG_VM_CTRL))) terminal->paste = false;
    if (event->kind == SNAG_VM_MOUSE) {
        if (!terminal->mouse) {
            if (event->release || event->button) return 0;
            VTermPos cursor;
            vterm_state_get_cursorpos(vterm_obtain_state(terminal->terminal), &cursor);
            char pointer[32];
            int length = snprintf(pointer, sizeof(pointer), "\033[9006;%d;%u~",
                (int)event->row - cursor.row, event->column);
            return snag_buf_append(&terminal->input, pointer, (size_t)length);
        }
        VTermModifier modifiers = VTERM_MOD_NONE;
        if (event->button & 4u) modifiers |= VTERM_MOD_SHIFT;
        if (event->button & 8u) modifiers |= VTERM_MOD_ALT;
        if (event->button & 16u) modifiers |= VTERM_MOD_CTRL;
        vterm_mouse_move(terminal->terminal, (int)event->row, (int)event->column, modifiers);
        if (!(event->button & 32u)) {
            int button = (int)(event->button & 3u) + (event->button & 64u ? 4 : 1);
            vterm_mouse_button(terminal->terminal, button, !event->release, modifiers);
        }
        return terminal->error ? snag_errno(terminal->error) : 0;
    }
    return snag_buf_append(&terminal->input, event->wire, event->wire_length);
}

int
snag_vm_terminal_reply(struct snag_vm_terminal *terminal, const struct snag_vm_input_event *event)
{
    size_t probe = strlen(terminal->probe_reply), copy = strlen(terminal->copy_reply);
    bool matches = (probe && event->wire_length == probe &&
        !memcmp(event->wire, terminal->probe_reply, probe)) ||
        (copy && event->wire_length > copy && !memcmp(event->wire, terminal->copy_reply, copy));
    if (!matches) return 0;
    return snag_vm_terminal_input(terminal, event) < 0 ? -1 : 1;
}

static uint32_t
terminal_color(VTermColor color)
{
    if (color.type & VTERM_COLOR_DEFAULT_MASK) return 0u;
    if (VTERM_COLOR_IS_INDEXED(&color)) return (uint32_t)color.indexed.idx + 1u;
    return 0x1000000u | (uint32_t)color.rgb.red << 16u |
        (uint32_t)color.rgb.green << 8u | color.rgb.blue;
}

bool
snag_vm_terminal_mouse(const struct snag_vm_terminal *terminal)
{
    return terminal->mouse;
}

bool
snag_vm_terminal_continued(const struct snag_vm_terminal *terminal, size_t offset, size_t row)
{
    return row <= INT_MAX && vterm_screen_view_line_continued(terminal->screen, offset, (int)row);
}

size_t
snag_vm_terminal_history_rows(const struct snag_vm_terminal *terminal)
{
    return vterm_screen_history_rows(terminal->screen);
}

int
snag_vm_terminal_draw(struct snag_vm_terminal *terminal, struct snag_vm_grid *grid,
    const struct snag_vm_rectangle *rectangle, size_t offset,
    size_t *cursor_row, size_t *cursor_column)
{
    int rows, columns;
    vterm_get_size(terminal->terminal, &rows, &columns);
    for (unsigned int row = 0u; row < rectangle->rows && row < (unsigned int)rows; ++row) {
        for (unsigned int column = 0u; column < rectangle->columns &&
            column < (unsigned int)columns; ++column) {
            VTermScreenCell cell;
            const uint32_t *characters = NULL;
            size_t count = 0u;
            if (!vterm_screen_get_view_cell(terminal->screen, offset,
                (VTermPos){(int)row, (int)column}, &cell, &characters, &count))
                return snag_errno(EIO);
            if (cell.chars[0] == UINT32_MAX) continue;
            unsigned int style = SNAG_VM_NATIVE;
            if (cell.attrs.bold) style |= SNAG_VM_BOLD;
            if (cell.attrs.faint) style |= SNAG_VM_DIM;
            if (cell.attrs.italic) style |= SNAG_VM_ITALIC;
            if (cell.attrs.underline) style |= SNAG_VM_UNDERLINE;
            if (cell.attrs.reverse) style |= SNAG_VM_REVERSE;
            if (cell.attrs.blink) style |= SNAG_VM_BLINK;
            if (cell.attrs.conceal) style |= SNAG_VM_CONCEAL;
            if (cell.attrs.strike) style |= SNAG_VM_STRIKE;
            if (cell.attrs.underline == 2u) style |= SNAG_VM_DOUBLE_UNDERLINE;
            if (snag_vm_grid_terminal_cell(grid, rectangle->row + row,
                rectangle->column + column,
                characters, count,
                cell.width > 0 ? (unsigned int)cell.width : 1u, style,
                terminal_color(cell.fg), terminal_color(cell.bg)) < 0) return -1;
        }
    }
    if (!offset && cursor_row && cursor_column && terminal->visible) {
        VTermPos cursor;
        vterm_state_get_cursorpos(vterm_obtain_state(terminal->terminal), &cursor);
        if (cursor.row >= 0 && cursor.col >= 0 && (unsigned int)cursor.row < rectangle->rows &&
            (unsigned int)cursor.col < rectangle->columns) {
            *cursor_row = rectangle->row + (size_t)cursor.row;
            *cursor_column = rectangle->column + (size_t)cursor.col;
        }
    }
    return 0;
}

void
snag_vm_terminal_free(struct snag_vm_terminal *terminal)
{
    if (!terminal) return;
    snag_child_free(&terminal->child);
    json_decref(terminal->checkpoint_snapshot);
    if (terminal->terminal) vterm_free(terminal->terminal);
    snag_buf_free(&terminal->input);
    snag_buf_free(&terminal->transaction);
    free(terminal);
}

#else
struct snag_vm_terminal *
snag_vm_terminal_frontend(const char *program, int channel, unsigned int rows,
    unsigned int columns, int (*emit)(void *, const void *, size_t), void *opaque)
{
    (void)program; (void)channel; (void)rows; (void)columns; (void)emit; (void)opaque;
    errno = ENOTSUP;
    return NULL;
}
int snag_vm_terminal_fd(const struct snag_vm_terminal *terminal) { (void)terminal; return -1; }
uint64_t snag_vm_terminal_frame(const struct snag_vm_terminal *terminal)
{ (void)terminal; return 0u; }
bool snag_vm_terminal_writing(const struct snag_vm_terminal *terminal)
{ (void)terminal; return false; }
int snag_vm_terminal_step(struct snag_vm_terminal *terminal) { (void)terminal; return 0; }
int snag_vm_terminal_checkpoint(struct snag_vm_terminal *terminal, bool force)
{ (void)terminal; (void)force; return snag_errno(ENOTSUP); }
json_t *snag_vm_terminal_checkpoint_snapshot(struct snag_vm_terminal *terminal)
{ (void)terminal; return NULL; }
int snag_vm_terminal_resize(struct snag_vm_terminal *terminal,
    unsigned int rows, unsigned int columns)
{ (void)terminal; (void)rows; (void)columns; return snag_errno(ENOTSUP); }
int snag_vm_terminal_input(struct snag_vm_terminal *terminal,
    const struct snag_vm_input_event *event)
{ (void)terminal; (void)event; return snag_errno(ENOTSUP); }
int snag_vm_terminal_reply(struct snag_vm_terminal *terminal,
    const struct snag_vm_input_event *event)
{ (void)terminal; (void)event; return 0; }
bool snag_vm_terminal_mouse(const struct snag_vm_terminal *terminal)
{ (void)terminal; return false; }
bool snag_vm_terminal_continued(const struct snag_vm_terminal *terminal, size_t offset, size_t row)
{ (void)terminal; (void)offset; (void)row; return false; }
size_t snag_vm_terminal_history_rows(const struct snag_vm_terminal *terminal)
{ (void)terminal; return 0u; }
int snag_vm_terminal_draw(struct snag_vm_terminal *terminal, struct snag_vm_grid *grid,
    const struct snag_vm_rectangle *rectangle, size_t offset, size_t *row, size_t *column)
{
    (void)terminal; (void)grid; (void)rectangle; (void)offset; (void)row; (void)column;
    return snag_errno(ENOTSUP);
}
void snag_vm_terminal_free(struct snag_vm_terminal *terminal) { (void)terminal; }
#endif

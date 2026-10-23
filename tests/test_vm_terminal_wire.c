/* SPDX-License-Identifier: GPL-2.0-only */
#include "vm_terminal.h"
#include "process_host.h"
#include "presentation.h"
#include "term.h"
#include <vterm.h>

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

static VTerm *startup_terminal(int rows, int columns);
static int startup_spawn(struct snag_child *child, const char *program,
    const char *const *argv, char **environment, unsigned short rows,
    unsigned short columns, int channel);

static size_t draw_appends, draw_copied_bytes;
static bool measuring_draw;

static int
terminal_append(struct snag_buf *buffer, const void *bytes, size_t length)
{
    if (measuring_draw && length) {
        ++draw_appends;
        draw_copied_bytes += length;
    }
    return snag_buf_append(buffer, bytes, length);
}

/* Exercise the real constructor, including its pre-spawn error boundary. */
#define vterm_new startup_terminal
#define snag_buf_append terminal_append
#define snag_child_spawn_terminal_channel startup_spawn
#include "../src/vm_terminal.c"
#undef snag_child_spawn_terminal_channel
#undef vterm_new
#undef snag_buf_append

static const char clipboard[] =
    "\033[>9003;4294967295;2147483648;123;456;789;1;2147483647;4294967295c";
static const char probe[] = "\033[?9001;987654321n";
static const char probe_reply[] = "\033[>987654321S";
static const char begin[] = "\033]2;SNAJPAGENT-SCREEN/1:BEGIN:ffffffff:"
    "800000000000007b000001c800000315:UTF8:0:0120:0\033\\";
static const char title[] = "\033]2;fragmented title\033\\";
static const char passthrough[] = "\033P#ACT:screen \033[?9001;7n payload\n\033\\";
static const char acknowledged[] = "\033]1;replies received\033\\";

static void
test_clipped_editor_preserves_the_painted_tail(void)
{
    struct snag_term term;
    snag_term_init(&term);
    term.opened = term.capable = term.prompt_wanted = true;
    term.columns = 42u;
    term.rows = 24u;
    memcpy(term.label, "EXACT-host-model-PROMPT> 07:00:00 ", 33u);
    struct snag_buf output = {.max = SIZE_MAX}, draft = {.max = SIZE_MAX};
    VTerm *vt = vterm_new(24, 42);
    assert(vt);
    vterm_set_utf8(vt, 1);
    VTermScreen *screen = vterm_obtain_screen(vt);
    assert(screen);
    vterm_screen_reset(screen, 1);
    term.capture = &output;
    snag_term_output_bind(&term);
    for (size_t i = 0u; i < 220u; ++i)
        assert(snag_buf_append(&draft, "wrapped words ", 14u) == 0);
    static const char tail[] = "tail-42-é-中-👩‍💻";
    assert(snag_buf_append(&draft, tail, sizeof(tail) - 1u) == 0);
    assert(snag_buf_terminate(&draft) == 0);
    assert(snag_term_restore_draft(&term, (char *)draft.data) == 0);
    assert(vterm_input_write(vt, (char *)output.data, output.len) == output.len);
    size_t consumed = output.len;
    static const char edit[] = "\001prefix-\005-end";
    for (size_t i = 0u; i < sizeof(edit) - 1u; ++i) {
        enum snag_term_action action = SNAG_TERM_NONE;
        char *text = NULL;
        assert(snag_term_feed_byte(&term, (unsigned char)edit[i], &action, &text) == 0);
        assert(action == SNAG_TERM_NONE && !text);
        size_t added = output.len - consumed;
        assert(vterm_input_write(vt, (char *)output.data + consumed, added) == added);
        consumed = output.len;
        VTermPos position;
        vterm_state_get_cursorpos(vterm_obtain_state(vt), &position);
        assert(position.row == (int)term.rendered_cursor_row);
        assert(position.col == (int)term.rendered_cursor_col);
    }
    assert(term.cursor == term.draft.len && term.draft.len == draft.len + 11u);
    assert(!memcmp(term.draft.data + term.draft.len - 4u, "-end", 4u));
    char cells[24u * 42u + 1u];
    for (int row = 0; row < 24; ++row) {
        for (int col = 0; col < 42; ++col) {
            VTermScreenCell cell;
            assert(vterm_screen_get_cell(screen, (VTermPos){row, col}, &cell));
            uint32_t cp = cell.chars[0];
            cells[row * 42 + col] = cp && cp < 128u ? (char)cp : ' ';
        }
    }
    cells[sizeof(cells) - 1u] = '\0';
    if (!strstr(cells, "-end")) {
        for (size_t row = 0u; row < 24u; ++row)
            fprintf(stderr, "%02zu %.42s\n", row, cells + row * 42u);
        fprintf(stderr, "painted=%.*s\ncursor=%zu,%zu\n", (int)term.painted_prompt.len,
            term.painted_prompt.data, term.rendered_cursor_row, term.rendered_cursor_col);
    }
    assert(strstr(cells, "-end"));
    vterm_free(vt);
    term.capture = NULL;
    term.opened = false;
    snag_term_output_bind(NULL);
    snag_term_close(&term);
    snag_buf_free(&draft);
    snag_buf_free(&output);
}

struct output {
    struct snag_buf bytes;
    bool partial;
};

static int
emit(void *opaque, const void *bytes, size_t length)
{
    struct output *output = opaque;
    if (length >= 2u && (!memcmp(bytes, "\033]", 2u) ||
        !memcmp(bytes, "\033P", 2u)) &&
        (length < 4u || memcmp((const char *)bytes + length - 2u, "\033\\", 2u)))
        output->partial = true;
    return snag_buf_append(&output->bytes, bytes, length);
}

struct allocation_failure {
    size_t calls, fail, live;
    bool failed;
};

static void *
failing_allocate(size_t bytes, void *opaque)
{
    struct allocation_failure *failure = opaque;
    if (++failure->calls == failure->fail) {
        failure->failed = true;
        errno = ENOMEM;
        return NULL;
    }
    void *memory = calloc(1u, bytes);
    assert(memory);
    ++failure->live;
    return memory;
}

static void
failing_free(void *memory, void *opaque)
{
    if (!memory) return;
    struct allocation_failure *failure = opaque;
    assert(failure->live);
    --failure->live;
    free(memory);
}

static struct allocation_failure startup_failure;
static bool startup_inject;
static size_t startup_spawns;

static VTerm *
startup_terminal(int rows, int columns)
{
    if (!startup_inject) return vterm_new(rows, columns);
    static VTermAllocatorFunctions allocator = {
        .malloc = failing_allocate, .free = failing_free};
    return vterm_new_with_allocator(rows, columns, &allocator, &startup_failure);
}

static int
startup_spawn(struct snag_child *child, const char *program, const char *const *argv,
    char **environment, unsigned short rows, unsigned short columns, int channel)
{
    if (!startup_inject)
        return snag_child_spawn_terminal_channel(child, program, argv, environment,
            rows, columns, channel);
    ++startup_spawns;
    return snag_errno(ECHILD);
}

struct parser_capture {
    struct snag_buf bytes;
    size_t fragments;
};

static int
capture_osc(int command, VTermStringFragment fragment, void *opaque)
{
    struct parser_capture *capture = opaque;
    assert(command == 2);
    assert(fragment.initial == (capture->fragments == 0u));
    ++capture->fragments;
    assert(snag_buf_append(&capture->bytes, fragment.str, fragment.len) == 0);
    return 1;
}

static void
test_control_bytes_in_string_header_preserve_initial_fragment(void)
{
    static const char title[] = "\033]2\0;header\033\\";
    static const VTermParserCallbacks callbacks = {.osc = capture_osc};
    for (size_t chunk = 1u; chunk < sizeof(title); ++chunk) {
        VTerm *vt = vterm_new(3, 4);
        assert(vt);
        struct parser_capture capture = {.bytes.max = SIZE_MAX};
        vterm_parser_set_callbacks(vt, &callbacks, &capture);
        for (size_t at = 0u; at < sizeof(title) - 1u;) {
            size_t length = sizeof(title) - 1u - at;
            if (length > chunk) length = chunk;
            assert(vterm_input_write(vt, title + at, length) == length);
            at += length;
        }
        assert(capture.fragments);
        assert(capture.bytes.len == 6u && !memcmp(capture.bytes.data, "header", 6u));
        snag_buf_free(&capture.bytes);
        vterm_free(vt);
    }
}

static void
test_frontend_frame_marker_follows_terminal_bytes(void)
{
    static const char bytes[] = "painted\033]2;SNAJPAGENT-SCREEN/1:FRAME:19\033\\";
    for (size_t chunk = 1u; chunk < sizeof(bytes); ++chunk) {
        struct snag_vm_terminal terminal = {.transaction.max = SIZE_MAX};
        terminal.terminal = vterm_new(3, 20);
        assert(terminal.terminal);
        terminal.screen = vterm_obtain_screen(terminal.terminal);
        assert(terminal.screen);
        vterm_screen_reset(terminal.screen, 1);
        vterm_parser_set_filter(terminal.terminal, &parser_filter, &terminal);
        for (size_t at = 0u; at < sizeof(bytes) - 1u;) {
            size_t amount = sizeof(bytes) - 1u - at;
            if (amount > chunk) amount = chunk;
            assert(vterm_input_write(terminal.terminal, bytes + at, amount) == amount);
            at += amount;
            assert(snag_vm_terminal_frame(&terminal) == (at == sizeof(bytes) - 1u ? 19u : 0u));
        }
        char text[16];
        assert(vterm_screen_get_text(terminal.screen, text, sizeof(text),
            (VTermRect){0, 1, 0, 7}) == 7u);
        assert(!memcmp(text, "painted", 7u));
        assert(!terminal.error);
        snag_buf_free(&terminal.transaction);
        vterm_free(terminal.terminal);
    }
}

static void
test_terminal_draw_encodes_borrowed_glyphs_into_the_grid(void)
{
    VTerm *vt = vterm_new(3, 12);
    assert(vt);
    vterm_set_utf8(vt, 1);
    VTermScreen *screen = vterm_obtain_screen(vt);
    assert(screen);
    vterm_screen_reset(screen, 1);
    static const char glyph[] = "é́́́́́́́";
    static const char text[] = "\033[1;2;3;4;5;7;8;9;38;2;17;34;51;48;5;201m"
        "é́́́́́́́\033[0m界👩‍💻";
    assert(vterm_input_write(vt, text, sizeof(text) - 1u) == sizeof(text) - 1u);
    struct snag_vm_terminal terminal = {.terminal = vt, .screen = screen};
    struct snag_vm_grid grid = {0};
    assert(snag_vm_grid_resize(&grid, 3u, 12u) == 0);
    struct snag_vm_rectangle rectangle = {.rows = 3u, .columns = 12u};
    draw_appends = draw_copied_bytes = 0u;
    for (size_t i = 0u; i < 10u; ++i) {
        snag_vm_grid_begin(&grid);
        measuring_draw = true;
        assert(snag_vm_terminal_draw(&terminal, &grid, &rectangle, 0u, NULL, NULL) == 0);
        measuring_draw = false;
        const struct snag_vm_cell *cell = grid.back.cells;
        assert(cell->length == sizeof(glyph) - 1u && cell->width == 1u);
        assert(!memcmp(grid.back.text.data + cell->offset, glyph, sizeof(glyph) - 1u));
        assert(cell->style == (SNAG_VM_NATIVE | SNAG_VM_BOLD | SNAG_VM_DIM |
            SNAG_VM_ITALIC | SNAG_VM_UNDERLINE | SNAG_VM_BLINK | SNAG_VM_REVERSE |
            SNAG_VM_CONCEAL | SNAG_VM_STRIKE));
        assert(cell->foreground == 0x1112233u && cell->background == 202u);
        assert(cell[1].width == 2u && cell[2].continuation);
        assert(cell[3].width == 2u && cell[4].continuation);
        assert(cell[3].length == sizeof("👩‍💻") - 1u);
        assert(!memcmp(grid.back.text.data + cell[3].offset, "👩‍💻", cell[3].length));
    }
    if (draw_appends)
        fprintf(stderr, "terminal draw scratch copies: %zu calls, %zu bytes\n",
            draw_appends, draw_copied_bytes);
    assert(!draw_appends && !draw_copied_bytes);
    snag_vm_grid_free(&grid);
    vterm_free(vt);
}

static void
test_terminal_overlapping_moves_preserve_glyph_ownership(void)
{
    static const char text[] = "Aé́́́́́́́Bé́́́́́́́C";
    static const char *const moves[] = {"\033[1;1H\033[P", "\033[1;1H\033[@"};
    static const int columns[] = {0, 2};
    for (size_t move = 0u; move < sizeof(moves) / sizeof(moves[0]); ++move) {
        struct allocation_failure allocation = {0};
        VTermAllocatorFunctions allocator = {.malloc = failing_allocate, .free = failing_free};
        VTerm *vt = vterm_new_with_allocator(3, 12, &allocator, &allocation);
        assert(vt);
        vterm_set_utf8(vt, 1);
        VTermScreen *screen = vterm_obtain_screen(vt);
        assert(screen);
        vterm_screen_reset(screen, 1);
        assert(vterm_input_write(vt, text, sizeof(text) - 1u) == sizeof(text) - 1u);
        VTermScreenCell cell;
        const uint32_t *glyph = NULL;
        size_t length = 0u;
        assert(vterm_screen_get_view_cell(screen, 0u, (VTermPos){0, 1},
            &cell, &glyph, &length));
        assert(length == 9u && glyph[0] == 'e');
        assert(vterm_input_write(vt, moves[move], strlen(moves[move])) == strlen(moves[move]));
        assert(!vterm_get_error(vt));
        assert(vterm_screen_get_view_cell(screen, 0u, (VTermPos){0, columns[move]},
            &cell, &glyph, &length));
        if (length != 9u)
            fprintf(stderr, "overlapping terminal move %zu: %zu scalars, expected 9\n",
                move, length);
        assert(length == 9u && glyph[0] == 'e');
        for (size_t i = 1u; i < length; ++i) assert(glyph[i] == 0x301u);
        vterm_free(vt);
        assert(!allocation.live);
    }
}

static void
test_terminal_history_preserves_cells_and_ownership(void)
{
    static const char text[] = "\033[2;31mé́́́́́́́\033[0m\r\nmiddle\r\nlast\r\n";
    struct allocation_failure allocation = {0};
    VTermAllocatorFunctions allocator = {.malloc = failing_allocate, .free = failing_free};
    VTerm *vt = vterm_new_with_allocator(3, 12, &allocator, &allocation);
    assert(vt);
    vterm_set_utf8(vt, 1);
    VTermScreen *screen = vterm_obtain_screen(vt);
    assert(screen);
    vterm_screen_enable_history(screen);
    vterm_screen_reset(screen, 1);
    assert(vterm_input_write(vt, text, sizeof(text) - 1u) == sizeof(text) - 1u);
    assert(!vterm_get_error(vt));
    assert(vterm_screen_history_rows(screen) == 1u);
    VTermScreenCell cell;
    const uint32_t *glyph = NULL;
    size_t length = 0u;
    assert(vterm_screen_get_view_cell(screen, 1u, (VTermPos){0, 0},
        &cell, &glyph, &length));
    assert(length == 9u && glyph[0] == 'e');
    for (size_t i = 1u; i < length; ++i) assert(glyph[i] == 0x301u);
    assert(cell.attrs.faint && VTERM_COLOR_IS_INDEXED(&cell.fg) && cell.fg.indexed.idx == 1);
    vterm_set_size(vt, 3, 20);
    assert(vterm_screen_get_view_cell(screen, 1u, (VTermPos){0, 19},
        &cell, &glyph, &length));
    assert(!length && (cell.fg.type & VTERM_COLOR_DEFAULT_FG) &&
        (cell.bg.type & VTERM_COLOR_DEFAULT_BG));
    vterm_set_size(vt, 3, 12);
    vterm_set_size(vt, 4, 12);
    assert(!vterm_get_error(vt));
    assert(vterm_screen_history_rows(screen) == 0u);
    assert(vterm_screen_get_glyph(screen, (VTermPos){0, 0}, &glyph) == 9u);
    vterm_set_size(vt, 3, 12);
    assert(!vterm_get_error(vt));
    assert(vterm_screen_history_rows(screen) == 1u);
    vterm_screen_enable_altscreen(screen, 1);
    static const char alternate[] = "\033[?1049hA\r\nB\r\nC\r\nD";
    assert(vterm_input_write(vt, alternate, sizeof(alternate) - 1u) == sizeof(alternate) - 1u);
    assert(vterm_screen_history_rows(screen) == 0u);
    assert(vterm_input_write(vt, "\033[?1049l", 8u) == 8u);
    assert(vterm_screen_history_rows(screen) == 1u);
    assert(vterm_input_write(vt, "\033[3J", 4u) == 4u);
    assert(vterm_screen_history_rows(screen) == 0u);
    vterm_free(vt);
    assert(!allocation.live);

    vt = vterm_new(2, 4);
    assert(vt);
    vterm_set_utf8(vt, 1);
    screen = vterm_obtain_screen(vt);
    assert(screen);
    vterm_screen_enable_history(screen);
    vterm_screen_enable_reflow(screen, true);
    vterm_screen_reset(screen, 1);
    assert(vterm_input_write(vt, "abcdefghijk", 11u) == 11u);
    assert(vterm_screen_history_rows(screen) == 1u);
    struct snag_vm_terminal painted = {.terminal = vt, .screen = screen};
    assert(!snag_vm_terminal_continued(&painted, 1u, 0u));
    assert(snag_vm_terminal_continued(&painted, 1u, 1u));
    assert(snag_vm_terminal_continued(&painted, 0u, 0u));
    assert(!snag_vm_terminal_continued(&painted, 0u, SIZE_MAX));
    vterm_set_size(vt, 3, 4);
    assert(vterm_screen_history_rows(screen) == 0u);
    assert(!vterm_state_get_lineinfo(vterm_obtain_state(vt), 0)->continuation);
    assert(vterm_state_get_lineinfo(vterm_obtain_state(vt), 1)->continuation);
    assert(vterm_state_get_lineinfo(vterm_obtain_state(vt), 2)->continuation);
    vterm_set_size(vt, 3, 12);
    char joined[64];
    size_t used = vterm_screen_get_text(screen, joined, sizeof(joined),
        (VTermRect){0, 1, 0, 12});
    assert(used == 11u && !memcmp(joined, "abcdefghijk", 11u));
    vterm_free(vt);

    /* Vector and row failures must surface and release partial ownership. */
    for (size_t fail = 1u; fail <= 3u; ++fail) {
        allocation = (struct allocation_failure){0};
        vt = vterm_new_with_allocator(2, 12, &allocator, &allocation);
        assert(vt);
        screen = vterm_obtain_screen(vt);
        assert(screen);
        vterm_screen_enable_history(screen);
        vterm_screen_reset(screen, 1);
        allocation.fail = allocation.calls + fail;
        static const char rows[] = "first\r\nsecond\r\nthird\r\n";
        (void)vterm_input_write(vt, rows, sizeof(rows) - 1u);
        assert(allocation.failed && vterm_get_error(vt) == ENOMEM);
        vterm_free(vt);
        assert(!allocation.live);
    }
}

static void
test_terminal_history_reflows_across_the_live_boundary(void)
{
    VTerm *vt = vterm_new(2, 4);
    assert(vt);
    VTermScreen *screen = vterm_obtain_screen(vt);
    assert(screen);
    vterm_screen_enable_history(screen);
    vterm_screen_enable_reflow(screen, 1);
    vterm_screen_reset(screen, 1);
    assert(vterm_input_write(vt, "abcdefghijk", 11u) == 11u);
    assert(vterm_screen_history_rows(screen) == 1u);
    vterm_set_size(vt, 3, 12);
    assert(!vterm_get_error(vt));
    char text[64];
    size_t used = vterm_screen_get_text(screen, text, sizeof(text),
        (VTermRect){0, 1, 0, 12});
    assert(used == 11u && !memcmp(text, "abcdefghijk", 11u));
    assert(vterm_screen_history_rows(screen) == 0u);
    assert(!vterm_state_get_lineinfo(vterm_obtain_state(vt), 0)->continuation);
    for (size_t resize = 0u; resize < 20u; ++resize) {
        vterm_set_size(vt, 2, 3);
        assert(!vterm_get_error(vt));
        /* tmux retains the other bottom blank when height shrinks by one. */
        assert(vterm_screen_history_rows(screen) == 3u);
        VTermScreenCell cell;
        const uint32_t *glyph;
        size_t length;
        assert(vterm_screen_get_view_cell(screen, 3u, (VTermPos){0, 0},
            &cell, &glyph, &length) && length == 1u && glyph[0] == 'a');
        assert(vterm_screen_get_view_cell(screen, 3u, (VTermPos){1, 0},
            &cell, &glyph, &length) && length == 1u && glyph[0] == 'd');
        vterm_set_size(vt, 3, 12);
        used = vterm_screen_get_text(screen, text, sizeof(text), (VTermRect){0, 1, 0, 12});
        assert(used == 11u && !memcmp(text, "abcdefghijk", 11u));
        assert(vterm_screen_history_rows(screen) == 0u);
        VTermPos cursor;
        vterm_state_get_cursorpos(vterm_obtain_state(vt), &cursor);
        assert(cursor.row == 0 && cursor.col == 11);
    }
    assert(vterm_input_write(vt, "!", 1u) == 1u);
    used = vterm_screen_get_text(screen, text, sizeof(text), (VTermRect){0, 1, 0, 12});
    assert(used == 12u && !memcmp(text, "abcdefghijk!", 12u));
    vterm_free(vt);

    vt = vterm_new(4, 4);
    assert(vt);
    screen = vterm_obtain_screen(vt);
    assert(screen);
    vterm_screen_enable_history(screen);
    vterm_screen_enable_reflow(screen, 1);
    vterm_screen_reset(screen, 1);
    static const char erased[] = "abcde\r\033[2K";
    assert(vterm_input_write(vt, erased, sizeof(erased) - 1u) == sizeof(erased) - 1u);
    vterm_set_size(vt, 4, 8);
    VTermPos erased_cursor;
    vterm_state_get_cursorpos(vterm_obtain_state(vt), &erased_cursor);
    assert(erased_cursor.row == 1 && erased_cursor.col == 0);
    vterm_free(vt);

    vt = vterm_new(2, 4);
    assert(vt);
    vterm_set_utf8(vt, 1);
    screen = vterm_obtain_screen(vt);
    assert(screen);
    vterm_screen_enable_history(screen);
    vterm_screen_enable_reflow(screen, 1);
    vterm_screen_reset(screen, 1);
    static const char wide[] = "\033[2;31mé́́́́́́́ab界XYz";
    assert(vterm_input_write(vt, wide, sizeof(wide) - 1u) == sizeof(wide) - 1u);
    assert(vterm_screen_history_rows(screen) == 1u);
    for (size_t resize = 0u; resize < 20u; ++resize) {
        vterm_set_size(vt, 2, 5);
        assert(!vterm_get_error(vt));
        assert(vterm_screen_history_rows(screen) == 0u);
        const uint32_t *glyph;
        VTermScreenCell cell;
        assert(vterm_screen_get_glyph(screen, (VTermPos){0, 0}, &glyph) == 9u);
        assert(glyph[0] == 'e' && glyph[8] == 0x301u);
        assert(vterm_screen_get_cell(screen, (VTermPos){0, 3}, &cell));
        assert(cell.chars[0] == 0x754cu && cell.width == 2 && cell.attrs.faint &&
            VTERM_COLOR_IS_INDEXED(&cell.fg) && cell.fg.indexed.idx == 1);
        assert(vterm_screen_get_cell(screen, (VTermPos){1, 0}, &cell) && cell.chars[0] == 'X');
        vterm_set_size(vt, 2, 3);
        assert(!vterm_get_error(vt));
        assert(vterm_screen_history_rows(screen) == 1u);
    }
    vterm_free(vt);
}

static void
test_terminal_allocation_failures(void)
{
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    assert(snag_fd_cloexec(pair[0]) == 0 && snag_fd_cloexec(pair[1]) == 0);
    startup_inject = true;
    for (size_t fail = 1u;; ++fail) {
        startup_failure = (struct allocation_failure){.fail = fail};
        startup_spawns = 0u;
        struct snag_vm_terminal *pane = snag_vm_terminal_frontend("unused", pair[1],
            3u, 8u, NULL, NULL);
        assert(!pane);
        if (startup_failure.failed) {
            assert(!startup_spawns);
            assert(errno == ENOMEM);
        } else {
            assert(startup_spawns == 1u && errno == ECHILD);
        }
        assert(!startup_failure.live);
        assert(fcntl(pair[0], F_GETFD) & FD_CLOEXEC);
        assert(fcntl(pair[1], F_GETFD) & FD_CLOEXEC);
        if (!startup_failure.failed) break;
    }
    assert(close(pair[0]) == 0 && close(pair[1]) == 0);
    startup_inject = false;
    VTermAllocatorFunctions allocator = {.malloc = failing_allocate, .free = failing_free};
    for (size_t fail = 1u;; ++fail) {
        struct allocation_failure failure = {0};
        VTerm *terminal = vterm_new_with_allocator(2, 4, &allocator, &failure);
        assert(terminal);
        vterm_set_utf8(terminal, 1);
        VTermScreen *screen = vterm_obtain_screen(terminal);
        assert(screen);
        vterm_screen_enable_history(screen);
        vterm_screen_enable_reflow(screen, 1);
        vterm_screen_reset(screen, 1);
        static const char text[] = "á́́́́́́́bcdefghijk";
        assert(vterm_input_write(terminal, text, sizeof(text) - 1u) == sizeof(text) - 1u);
        failure.fail = failure.calls + fail;
        vterm_set_size(terminal, 2, 2);
        assert(vterm_get_error(terminal) == (failure.failed ? ENOMEM : 0));
        int rows, cols;
        vterm_get_size(terminal, &rows, &cols);
        assert(rows == 2 && cols == (failure.failed ? 4 : 2));
        assert(vterm_screen_history_rows(screen) == (failure.failed ? 1u : 4u));
        const uint32_t *glyph;
        VTermScreenCell cell;
        size_t length;
        assert(vterm_screen_get_view_cell(screen, failure.failed ? 1u : 4u,
            (VTermPos){0, 0}, &cell, &glyph, &length) && length == 9u &&
            glyph[0] == 'a' && glyph[8] == 0x301u);
        vterm_free(terminal);
        assert(!failure.live);
        if (!failure.failed) break;
    }
    for (size_t fail = 1u; fail <= 12u; ++fail) {
        struct allocation_failure failure = {.fail = fail};
        VTerm *terminal = vterm_new_with_allocator(3, 8, &allocator, &failure);
        if (terminal) {
            VTermScreen *screen = vterm_obtain_screen(terminal);
            if (screen) {
                vterm_screen_reset(screen, 1);
                vterm_screen_enable_altscreen(screen, 1);
            }
            vterm_free(terminal);
        }
        assert(!failure.live);
    }
    for (int alternate = 0; alternate < 2; ++alternate) {
        for (size_t fail = 1u; fail <= 9u; ++fail) {
            struct allocation_failure failure = {0};
            VTerm *terminal = vterm_new_with_allocator(3, 8, &allocator, &failure);
            assert(terminal);
            VTermScreen *screen = vterm_obtain_screen(terminal);
            assert(screen);
            vterm_screen_reset(screen, 1);
            vterm_screen_enable_reflow(screen, true);
            vterm_screen_enable_altscreen(screen, 1);
            if (alternate) vterm_input_write(terminal, "\033[?1049h", 8u);
            vterm_input_write(terminal, "A", 1u);
            failure.calls = 0u;
            failure.fail = fail;
            vterm_set_size(terminal, 5, 12);
            int rows, cols;
            vterm_get_size(terminal, &rows, &cols);
            assert(vterm_get_error(terminal) == (failure.failed ? ENOMEM : 0));
            assert(rows == (failure.failed ? 3 : 5));
            assert(cols == (failure.failed ? 8 : 12));
            VTermScreenCell cell;
            assert(vterm_screen_get_cell(screen, (VTermPos){0, 0}, &cell));
            assert(cell.chars[0] == 'A');
            vterm_free(terminal);
            assert(!failure.live);
        }
    }
    for (size_t fail = 1u; fail <= 4u; ++fail) {
        struct allocation_failure failure = {0};
        VTerm *terminal = vterm_new_with_allocator(3, 8, &allocator, &failure);
        assert(terminal);
        vterm_set_utf8(terminal, 1);
        VTermScreen *screen = vterm_obtain_screen(terminal);
        assert(screen);
        vterm_screen_reset(screen, 1);
        failure.calls = 0u;
        failure.fail = fail;
        vterm_input_write(terminal, "A", 1u);
        for (size_t i = 0u; i < 30u && !vterm_get_error(terminal); ++i)
            vterm_input_write(terminal, "\xcc\x81", 2u);
        assert(vterm_get_error(terminal) == (failure.failed ? ENOMEM : 0));
        vterm_free(terminal);
        assert(!failure.live);
    }
}

static void
test_frontend_constructor(const char *program)
{
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    assert(snag_fd_cloexec(pair[0]) == 0 && snag_fd_cloexec(pair[1]) == 0);
    struct snag_vm_terminal *terminal = snag_vm_terminal_frontend(program, pair[1],
        17u, 53u, NULL, NULL);
    assert(terminal);
    assert(fcntl(pair[1], F_GETFD) & FD_CLOEXEC);
    assert(close(pair[1]) == 0);
    for (size_t i = 0u; i < 2u; ++i) {
        if (i) assert(snag_vm_terminal_resize(terminal, 29u, 71u) == 0);
        assert(write(pair[0], "G", 1u) == 1);
        struct pollfd ready = {.fd = pair[0], .events = POLLIN};
        assert(poll(&ready, 1u, 5000) == 1);
        struct winsize size;
        assert(recv(pair[0], &size, sizeof(size), MSG_WAITALL) == (ssize_t)sizeof(size));
        assert(size.ws_row == (i ? 29u : 17u) && size.ws_col == (i ? 71u : 53u));
    }
    assert(close(pair[0]) == 0);
    uint64_t deadline = snag_monotonic_ms() + 5000u;
    while (!terminal->child.reaped && snag_monotonic_ms() < deadline) {
        assert(snag_vm_terminal_step(terminal) >= 0);
        if (!terminal->child.reaped) assert(snag_sleep_ms(1u) == 0);
    }
    assert(terminal->child.reaped && terminal->child.exit_code == 0);
    snag_vm_terminal_free(terminal);
    errno = 0;
    assert(!snag_vm_terminal_frontend(program, STDERR_FILENO, 17u, 53u, NULL, NULL));
    assert(errno == EINVAL);
}

static void
test_terminal_private_channel(const char *program)
{
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    assert(snag_fd_cloexec(pair[0]) == 0 && snag_fd_cloexec(pair[1]) == 0);
    char channel[32], hidden[32];
    snprintf(channel, sizeof(channel), "%d", pair[1]);
    snprintf(hidden, sizeof(hidden), "%d", pair[0]);
    const char *arguments[] = {program, "channel", channel, hidden, NULL};
    struct snag_child child;
    snag_child_init(&child);
    assert(snag_child_spawn_terminal_channel(&child, program, arguments, NULL,
        17u, 53u, pair[1]) == 0);
    assert(fcntl(pair[1], F_GETFD) & FD_CLOEXEC);
    assert(close(pair[1]) == 0);
    struct pollfd ready = {.fd = pair[0], .events = POLLIN};
    assert(poll(&ready, 1u, 5000) == 1);
    char answer;
    assert(read(pair[0], &answer, 1u) == 1 && answer == 'R');
    assert(close(pair[0]) == 0);
    uint64_t deadline = snag_monotonic_ms() + 5000u;
    while (!child.reaped && snag_monotonic_ms() < deadline) {
        int exited = snag_child_exited(&child);
        assert(exited >= 0);
        if (exited) assert(snag_child_reap(&child) == 0);
        else assert(snag_sleep_ms(1u) == 0);
    }
    assert(child.reaped && child.exit_code == 0);
    snag_child_free(&child);
}

int
main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "--internal-ui-frontend")) {
        int channel = atoi(argv[2]);
        assert(channel > STDERR_FILENO && isatty(STDIN_FILENO));
        char request = 0;
        while (read(channel, &request, 1u) == 1) {
            if (request == 'T') break;
            assert(request == 'G');
            struct winsize size;
            assert(ioctl(STDIN_FILENO, TIOCGWINSZ, &size) == 0);
            assert(write(channel, &size, sizeof(size)) == (ssize_t)sizeof(size));
        }
        assert(close(channel) == 0);
        if (request != 'T') return 0;
    }
    if (argc == 4 && !strcmp(argv[1], "presentation")) {
        int fd = open(argv[2], O_RDONLY | O_CLOEXEC);
        assert(fd >= 0);
        struct snag_binary_anchor begin, tail;
        bool incomplete;
        json_t *records = NULL;
        assert(snag_presentation_read(fd, argv[3], NULL, NULL, false,
            SNAG_JOURNAL_PAGE_BYTES, &records, &begin, &tail, &incomplete, NULL, NULL) == 0);
        assert(!incomplete);
        assert(json_dumpf(records, stdout, JSON_COMPACT | JSON_ENSURE_ASCII) == 0);
        json_decref(records);
        assert(close(fd) == 0);
        return 0;
    }
    if (argc == 4 && !strcmp(argv[1], "channel")) {
        int channel = atoi(argv[2]), hidden = atoi(argv[3]);
        assert(fcntl(hidden, F_GETFD) < 0 && errno == EBADF);
        struct winsize size;
        assert(ioctl(STDIN_FILENO, TIOCGWINSZ, &size) == 0);
        assert(size.ws_row == 17u && size.ws_col == 53u);
        assert(isatty(STDIN_FILENO) && isatty(STDERR_FILENO));
        assert(fcntl(channel, F_GETFD) >= 0);
        assert(write(channel, "R", 1u) == 1);
        assert(close(channel) == 0);
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "oom")) {
        test_terminal_allocation_failures();
        puts("terminal allocation failures: ok");
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "geometry")) {
        struct winsize size;
        assert(ioctl(STDIN_FILENO, TIOCGWINSZ, &size) == 0);
        assert(size.ws_row == 7u && size.ws_col == 93u);
        return 0;
    }
    if (argc > 1) {
        struct termios mode;
        assert(tcgetattr(STDIN_FILENO, &mode) == 0);
        mode.c_iflag &= (tcflag_t)~(IGNBRK | BRKINT | PARMRK | ISTRIP |
            INLCR | IGNCR | ICRNL | IXON);
        mode.c_oflag &= (tcflag_t)~OPOST;
        mode.c_lflag &= (tcflag_t)~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
        mode.c_cflag = (mode.c_cflag & (tcflag_t)~(CSIZE | PARENB)) | CS8;
        mode.c_cc[VMIN] = 1;
        mode.c_cc[VTIME] = 0;
        assert(tcsetattr(STDIN_FILENO, TCSANOW, &mode) == 0);
        static const char mouse[] = "\033[?1002h\033[?1006h";
        assert(write(STDOUT_FILENO, mouse, sizeof(mouse) - 1u) == (ssize_t)sizeof(mouse) - 1);
        assert(write(STDOUT_FILENO, probe, sizeof(probe) - 1u) == (ssize_t)sizeof(probe) - 1);
        assert(write(STDOUT_FILENO, begin, sizeof(begin) - 1u) == (ssize_t)sizeof(begin) - 1);
        assert(write(STDOUT_FILENO, clipboard, sizeof(clipboard) - 1u) ==
            (ssize_t)sizeof(clipboard) - 1);
        assert(write(STDOUT_FILENO, "\033]2;fragmented", 14u) == 14);
        assert(snag_sleep_ms(100u) == 0);
        assert(write(STDOUT_FILENO, " title\033\\", 8u) == 8);
        assert(write(STDOUT_FILENO, passthrough, 9u) == 9);
        assert(snag_sleep_ms(100u) == 0);
        assert(write(STDOUT_FILENO, passthrough + 9u,
            sizeof(passthrough) - 10u) == (ssize_t)sizeof(passthrough) - 10);
        static const char replies[] = "\033[>987654321S"
            "\033[>9003;4294967295;2147483648;123;456;789;1;2147483647;4294967295c"
            "\033[<0;8;4M\033[<0;8;4m";
        char input[sizeof(replies)];
        size_t used = 0u;
        while (used < sizeof(replies) - 1u) {
            ssize_t count = read(STDIN_FILENO, input + used, sizeof(replies) - 1u - used);
            assert(count > 0);
            used += (size_t)count;
        }
        assert(!memcmp(input, replies, sizeof(replies) - 1u));
        assert(write(STDOUT_FILENO, acknowledged, sizeof(acknowledged) - 1u) ==
            (ssize_t)sizeof(acknowledged) - 1);
        return 0;
    }
    assert(snag_text_locale_init());
    test_clipped_editor_preserves_the_painted_tail();
    test_control_bytes_in_string_header_preserve_initial_fragment();
    test_frontend_frame_marker_follows_terminal_bytes();
    test_terminal_draw_encodes_borrowed_glyphs_into_the_grid();
    test_terminal_overlapping_moves_preserve_glyph_ownership();
    test_terminal_history_preserves_cells_and_ownership();
    test_terminal_history_reflows_across_the_live_boundary();
    /* A variation selector can widen a glyph after the previous read ended at
     * the right margin. Recombination must wrap the whole glyph, not clip it. */
    VTerm *vt = vterm_new(3, 4);
    assert(vt);
    vterm_set_utf8(vt, 1);
    VTermScreen *screen = vterm_obtain_screen(vt);
    assert(screen);
    vterm_screen_reset(screen, 1);
    static const char narrow[] = "abc\xe2\x9d\xa4";
    static const char wide[] = "\xef\xb8\x8f";
    assert(vterm_input_write(vt, narrow, sizeof(narrow) - 1u) == sizeof(narrow) - 1u);
    assert(vterm_input_write(vt, wide, sizeof(wide) - 1u) == sizeof(wide) - 1u);
    const uint32_t *glyph;
    assert(vterm_screen_get_glyph(screen, (VTermPos){1, 0}, &glyph) == 2u);
    assert(glyph[0] == 0x2764u && glyph[1] == 0xfe0fu);
    assert(vterm_screen_get_glyph(screen, (VTermPos){0, 3}, &glyph) == 0u);
    VTermPos cursor;
    vterm_state_get_cursorpos(vterm_obtain_state(vt), &cursor);
    assert(cursor.row == 1 && cursor.col == 2);
    vterm_free(vt);

    struct output output = {.bytes = {.max = SIZE_MAX}};
    test_terminal_allocation_failures();
    char program[PATH_MAX];
    assert(realpath(argv[0], program));
    test_frontend_constructor(program);
    test_terminal_private_channel(program);
    /* No post-spawn resize can rescue this contract: the child inspects its
     * initial terminal geometry immediately after exec. */
    struct snag_child child;
    snag_child_init(&child);
    const char *arguments[] = {program, "geometry", NULL};
    assert(snag_child_spawn_terminal(&child, program, arguments, NULL, 7u, 93u) == 0);
    uint64_t geometry_deadline = snag_monotonic_ms() + 5000u;
    while (!child.reaped && snag_monotonic_ms() < geometry_deadline) {
        int exited = snag_child_exited(&child);
        assert(exited >= 0);
        if (exited) assert(snag_child_reap(&child) == 0);
        else assert(snag_sleep_ms(1u) == 0);
    }
    assert(child.reaped && child.exit_code == 0);
    snag_child_free(&child);
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    assert(snag_fd_cloexec(pair[0]) == 0 && snag_fd_cloexec(pair[1]) == 0);
    struct snag_vm_terminal *terminal = snag_vm_terminal_frontend(program, pair[1],
        24u, 80u, emit, &output);
    assert(terminal);
    assert(fcntl(pair[1], F_GETFD) & FD_CLOEXEC);
    assert(close(pair[1]) == 0);
    assert(write(pair[0], "T", 1u) == 1);
    assert(close(pair[0]) == 0);
    size_t expected = sizeof(probe) + sizeof(begin) + sizeof(clipboard) +
        sizeof(title) + sizeof(passthrough) - 5u;
    bool replied = false;
    uint64_t deadline = snag_monotonic_ms() + 5000u;
    while (snag_vm_terminal_fd(terminal) >= 0 && snag_monotonic_ms() < deadline) {
        assert(snag_vm_terminal_step(terminal) >= 0);
        if (!replied && output.bytes.len >= expected) {
            static const unsigned char unrelated[] = "\033[>123456789S";
            struct snag_vm_input_event event = {.kind = SNAG_VM_TERMINAL_REPLY,
                .wire = unrelated, .wire_length = sizeof(unrelated) - 1u};
            assert(snag_vm_terminal_reply(terminal, &event) == 0);
            static const unsigned char wrong_operation[] =
                "\033[>9003;4294967295;2147483648;123;456;788;1;2147483647;4294967295c";
            event.wire = wrong_operation;
            event.wire_length = sizeof(wrong_operation) - 1u;
            assert(snag_vm_terminal_reply(terminal, &event) == 0);
            event.wire = (const unsigned char *)probe_reply;
            event.wire_length = sizeof(probe_reply) - 1u;
            assert(snag_vm_terminal_reply(terminal, &event) == 1);
            event.wire = (const unsigned char *)clipboard;
            event.wire_length = sizeof(clipboard) - 1u;
            assert(snag_vm_terminal_reply(terminal, &event) == 1);
            event = (struct snag_vm_input_event){.kind = SNAG_VM_MOUSE, .row = 3u,
                .column = 7u, .button = 0u};
            assert(snag_vm_terminal_input(terminal, &event) == 0);
            event.release = true;
            assert(snag_vm_terminal_input(terminal, &event) == 0);
            replied = true;
        }
        assert(snag_sleep_ms(1u) == 0);
    }
    if (!replied) {
        fprintf(stderr, "transaction prefix length: %zu, expected %zu, pending %zu, error %d\n",
            output.bytes.len, expected, terminal->transaction.len, terminal->error);
        for (size_t i = 0u; i < output.bytes.len; ++i)
            fprintf(stderr, "%02x ", (unsigned char)output.bytes.data[i]);
        fputc('\n', stderr);
    }
    assert(replied);
    assert(snag_vm_terminal_fd(terminal) < 0);
    snag_vm_terminal_free(terminal);
    assert(output.bytes.len >= expected);
    assert(output.bytes.len - expected == sizeof(acknowledged) - 1u);
    assert(!memcmp(output.bytes.data + expected, acknowledged, sizeof(acknowledged) - 1u));
    expected += sizeof(acknowledged) - 1u;
    if (output.bytes.len != expected) {
        fprintf(stderr, "transaction length: %zu, expected %zu\n", output.bytes.len,
            expected);
        for (size_t i = 0u; i < output.bytes.len; ++i)
            fprintf(stderr, "%02x ", (unsigned char)output.bytes.data[i]);
        fputc('\n', stderr);
    }
    assert(output.bytes.len == expected);
    assert(!memcmp(output.bytes.data, probe, sizeof(probe) - 1u));
    assert(!memcmp(output.bytes.data + sizeof(probe) - 1u, begin, sizeof(begin) - 1u));
    assert(!memcmp(output.bytes.data + sizeof(probe) + sizeof(begin) - 2u,
        clipboard, sizeof(clipboard) - 1u));
    assert(!memcmp(output.bytes.data + sizeof(probe) + sizeof(begin) + sizeof(clipboard) - 3u,
        title, sizeof(title) - 1u));
    assert(!memcmp(output.bytes.data + sizeof(probe) + sizeof(begin) +
        sizeof(clipboard) + sizeof(title) - 4u,
        passthrough, sizeof(passthrough) - 1u));
    assert(!output.partial);
    snag_buf_free(&output.bytes);
    puts("terminal transactions: ok");
    return 0;
}

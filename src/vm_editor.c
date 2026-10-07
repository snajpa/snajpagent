/* SPDX-License-Identifier: GPL-2.0-only */
#include "vm_editor.h"
#include "commands.h"
#include "term.h"
#include "unicode.h"
#include "vm_connection.h"
#include "vm_text.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct snag_vm_undo {
    struct snag_vm_undo *next;
    size_t begin, before_size, after_size, before_cursor, after_cursor;
    unsigned char text[];
};

static void
undo_free(struct snag_vm_undo *undo)
{
    while (undo) {
        struct snag_vm_undo *next = undo->next;
        memset(undo->text, 0, undo->before_size + undo->after_size);
        free(undo);
        undo = next;
    }
}

void
snag_vm_editor_reset(struct snag_vm_editor *editor)
{
    if (editor->input) {
        snag_term_close(editor->input);
        free(editor->input);
    }
    undo_free(editor->undo);
    undo_free(editor->redo);
    if (editor->original.data) memset(editor->original.data, 0, editor->original.len);
    snag_buf_free(&editor->original);
    memset(editor, 0, sizeof(*editor));
}

int
snag_vm_editor_begin(struct snag_vm_buffer *buffer)
{
    struct snag_vm_editor *editor = &buffer->editor;
    if (editor->grouping) return 0;
    editor->original.max = SNAG_MAX_DIRECT_PROMPT + 1u;
    snag_buf_reset(&editor->original);
    if (snag_buf_append(&editor->original, buffer->draft.data, buffer->draft.len) < 0)
        return -1;
    editor->original_cursor = buffer->cursor;
    editor->grouping = true;
    return 0;
}

int
snag_vm_editor_end(struct snag_vm_buffer *buffer)
{
    struct snag_vm_editor *editor = &buffer->editor;
    if (!editor->grouping) return 0;
    const unsigned char *before = editor->original.data;
    const unsigned char *after = buffer->draft.data;
    size_t old_size = editor->original.len, new_size = buffer->draft.len;
    size_t prefix = 0u, suffix = 0u;
    while (prefix < old_size && prefix < new_size && before[prefix] == after[prefix]) ++prefix;
    if (prefix != old_size || prefix != new_size) {
        /* Delta edges need UTF-8 boundaries, even when a changed codepoint
         * shares leading bytes or joins a neighboring grapheme. */
        while (prefix && prefix < old_size && (before[prefix] & 0xc0u) == 0x80u) --prefix;
        while (suffix < old_size - prefix && suffix < new_size - prefix &&
            before[old_size - suffix - 1u] == after[new_size - suffix - 1u]) ++suffix;
        while (suffix && (before[old_size - suffix] & 0xc0u) == 0x80u) --suffix;
        size_t old_length = old_size - prefix - suffix;
        size_t new_length = new_size - prefix - suffix;
        /* Each side is bounded by the existing maximum draft size. */
        struct snag_vm_undo *undo = malloc(sizeof(*undo) + old_length + new_length);
        if (!undo) return -1;
        *undo = (struct snag_vm_undo){.next = editor->undo, .begin = prefix,
            .before_size = old_length, .after_size = new_length,
            .before_cursor = editor->original_cursor, .after_cursor = buffer->cursor};
        if (old_length) memcpy(undo->text, before + prefix, old_length);
        if (new_length) memcpy(undo->text + old_length, after + prefix, new_length);
        editor->undo = undo;
        undo_free(editor->redo);
        editor->redo = NULL;
    }
    if (editor->original.data) memset(editor->original.data, 0, editor->original.len);
    snag_buf_reset(&editor->original);
    editor->grouping = false;
    return 0;
}

int
snag_vm_editor_replace(struct snag_vm_buffer *buffer, size_t begin, size_t end,
    const void *text, size_t length)
{
    if (snag_vm_editor_begin(buffer) < 0) return -1;
    buffer->editor.column_valid = false;
    return snag_vm_draft_replace(buffer, begin, end, text, length);
}

int
snag_vm_editor_undo(struct snag_vm_buffer *buffer, bool redo, size_t count)
{
    struct snag_vm_editor *editor = &buffer->editor;
    if (snag_vm_editor_end(buffer) < 0) return -1;
    struct snag_vm_undo **from = redo ? &editor->redo : &editor->undo;
    struct snag_vm_undo **to = redo ? &editor->undo : &editor->redo;
    while (count-- && *from) {
        struct snag_vm_undo *undo = *from;
        size_t remove = redo ? undo->before_size : undo->after_size;
        size_t insert = redo ? undo->after_size : undo->before_size;
        const unsigned char *text = undo->text + (redo ? undo->before_size : 0u);
        if (snag_vm_draft_replace(buffer, undo->begin, undo->begin + remove,
            text, insert) < 0) return -1;
        snag_vm_draft_cursor(buffer, redo ? undo->after_cursor : undo->before_cursor);
        *from = undo->next;
        undo->next = *to;
        *to = undo;
    }
    editor->column_valid = false;
    return 0;
}

void
snag_vm_editor_normal(struct snag_vm_buffer *buffer, bool from_insert)
{
    const char *text = (const char *)buffer->draft.data;
    size_t start = snag_vm_text_line_start(text, buffer->draft.len, buffer->cursor);
    size_t end = snag_vm_text_line_end(text, buffer->draft.len, buffer->cursor);
    if (buffer->cursor > start && (from_insert || buffer->cursor == end))
        snag_vm_draft_cursor(buffer,
            snag_vm_text_previous(text, buffer->draft.len, buffer->cursor));
    buffer->editor.count = buffer->editor.operator_count = 0u;
    buffer->editor.operator = buffer->editor.prefix = 0u;
}

static unsigned int
word_class(const char *text, size_t length, size_t at)
{
    uint32_t cp;
    if (at >= length) return 0u;
    (void)snag_utf8_decode((const unsigned char *)text + at, length - at, &cp);
    return snag_unicode_word_class(cp);
}

static size_t
first_text(const char *text, size_t length, size_t at)
{
    at = snag_vm_text_line_start(text, length, at);
    while (at < length && (text[at] == ' ' || text[at] == '\t')) ++at;
    return at;
}

static size_t
line_number(const char *text, size_t at)
{
    size_t line = 0u;
    for (size_t i = 0u; i < at; ++i) if (text[i] == '\n') ++line;
    return line;
}

static size_t
line_at(const char *text, size_t length, size_t line)
{
    size_t at = 0u;
    while (line--) {
        size_t end = snag_vm_text_line_end(text, length, at);
        if (end == length) break;
        at = end + 1u;
    }
    return at;
}

/* Scan each boundary at most twice for a counted motion. Huge counts stop at
 * the available text, including backward movement over a long Unicode line. */
static size_t
left(const char *text, size_t length, size_t at, size_t count)
{
    size_t start = snag_vm_text_line_start(text, length, at), units = 0u;
    for (size_t i = start; i < at; i = snag_vm_text_next(text, length, i)) ++units;
    units = units > count ? units - count : 0u;
    while (units--) start = snag_vm_text_next(text, length, start);
    return start;
}

static size_t
word_move(const char *text, size_t length, size_t at, size_t count, unsigned int key,
    bool change_word, bool operate)
{
    size_t wanted = count, candidate_count = 0u, previous_end = 0u;
    unsigned int previous = 0u;
    /* Backward motion selects a start from its forward ordinal. */
    if (key == 'b') {
        for (size_t i = 0u; i < at; i = snag_vm_text_next(text, length, i)) {
            unsigned int kind = word_class(text, length, i);
            if ((kind && kind != previous) || (text[i] == '\n' && (!i || text[i - 1u] == '\n')))
                ++candidate_count;
            previous = kind;
        }
        if (!candidate_count) return 0u;
        wanted = candidate_count > count ? candidate_count - count + 1u : 1u;
        previous = 0u;
    }
    for (size_t i = 0u; i < length;) {
        size_t next = snag_vm_text_next(text, length, i);
        unsigned int kind = word_class(text, length, i);
        bool start = (kind && kind != previous) ||
            (text[i] == '\n' && (!i || text[i - 1u] == '\n'));
        bool end = kind && kind != word_class(text, length, next);
        bool candidate = key == 'e' ? end && (i > at || (change_word && i == at)) :
            start && (key == 'b' || i > at);
        if (candidate && !--wanted) {
            if (operate && key == 'w' && previous_end >= at && previous_end < i &&
                memchr(text + previous_end, '\n', i - previous_end))
                return snag_vm_text_line_end(text, length, previous_end);
            return i;
        }
        if (kind) previous_end = next;
        previous = kind;
        i = next;
    }
    if (operate && key == 'w' && previous_end >= at)
        return snag_vm_text_line_end(text, length, previous_end);
    return length;
}

struct display_motion {
    size_t cursor, row, current, total, target, column, logical_column, result;
    const char *text;
    size_t length;
    bool selecting, first;
};

static int
display_row(void *opaque, const struct snag_vm_text_row *row)
{
    struct display_motion *motion = opaque;
    if (!motion->selecting) {
        if (row->begin <= motion->cursor && motion->cursor <= row->end) {
            motion->current = motion->row;
            motion->column = motion->logical_column - row->logical_column;
        }
    } else if (motion->row == motion->target) {
        if (motion->first) {
            motion->result = row->begin;
            while (motion->result < row->end && (motion->text[motion->result] == ' ' ||
                motion->text[motion->result] == '\t')) ++motion->result;
            return 1;
        }
        motion->result = snag_vm_text_at_column(motion->text, motion->length, row->begin,
            row->logical_column + motion->column, false);
        if (motion->result >= row->end && row->end > row->begin)
            motion->result = snag_vm_text_previous(motion->text, motion->length, row->end);
        return 1;
    }
    ++motion->row;
    motion->total = motion->row;
    return 0;
}

static size_t
composer_display_move(struct snag_vm_editor *editor, const char *prompt, const char *text,
    size_t length, size_t at, size_t count, unsigned int key, size_t columns,
    size_t rows, size_t top)
{
    struct snag_buf frame = {0};
    struct snag_term_composer_layout layout;
    if (snag_term_composer_frame(prompt, text, length, at, (unsigned int)columns,
        &frame, &layout) < 0) { snag_buf_free(&frame); return at; }
    size_t target, column = layout.cursor_column;
    size_t total = layout.end_row + 1u;
    bool first = key != 'j' && key != 'k';
    if (!first) {
        if (editor->column_valid && editor->column_display) column = editor->column;
        else editor->column = column;
        editor->column_valid = editor->column_display = true;
        if (key == 'j') target = count >= total - layout.cursor_row ? total - 1u :
            layout.cursor_row + count;
        else target = count > layout.cursor_row ? 0u : layout.cursor_row - count;
    } else {
        if (!rows) rows = 1u;
        if (rows > total) rows = total;
        if (top > total - rows) top = total - rows;
        size_t offset = count > rows ? rows - 1u : count - 1u;
        target = top + (key == 'H' ? offset : key == 'L' ? rows - offset - 1u :
            (rows - 1u) / 2u);
        editor->column_valid = false;
    }
    struct snag_term_prompt_row line = {0};
    for (size_t i = 0u, begin = 0u; i <= target && begin <= frame.len; ++i) {
        line = snag_term_prompt_row(&frame, begin, (unsigned int)columns);
        begin = line.next;
    }
    size_t byte = line.start;
    size_t cells = 0u;
    while (byte < line.end) {
        struct snag_vm_glyph glyph = snag_vm_glyph((const char *)frame.data + byte,
            line.end - byte, cells, false);
        if (first ? frame.data[byte] != ' ' : cells + glyph.columns > column) break;
        cells += glyph.columns;
        byte += glyph.bytes;
    }
    size_t result = at;
    if (snag_term_composer_hit(prompt, text, length, (unsigned int)columns,
        byte, &result) < 0) result = at;
    result = snag_vm_text_floor(text, length, result);
    if (result == length && result) result = snag_vm_text_previous(text, length, result);
    snag_buf_free(&frame);
    return result;
}

static size_t
display_move(struct snag_vm_editor *editor, const char *text, size_t length, size_t at,
    size_t count, unsigned int key, size_t columns, size_t rows, size_t top, const char *prompt)
{
    if (prompt) return composer_display_move(editor, prompt, text, length, at, count, key,
        columns, rows, top);
    struct display_motion motion = {.text = text, .length = length, .cursor = at, .result = at};
    motion.logical_column = snag_vm_text_column(text, length, at, false);
    if (!columns) columns = 1u;
    (void)snag_vm_text_wrap(text, length, columns, false, display_row, &motion);
    if (key == 'j' || key == 'k') {
        if (editor->column_valid && editor->column_display) motion.column = editor->column;
        else editor->column = motion.column;
        editor->column_valid = editor->column_display = true;
        if (key == 'j') motion.target = count >= motion.total - motion.current ? motion.total - 1u :
            motion.current + count;
        else motion.target = count > motion.current ? 0u : motion.current - count;
    } else {
        if (!rows) rows = 1u;
        if (rows > motion.total) rows = motion.total;
        if (top > motion.current) top = motion.current;
        if (motion.current - top >= rows) top = motion.current - rows + 1u;
        if (top > motion.total - rows) top = motion.total - rows;
        size_t offset = count > rows ? rows - 1u : count - 1u;
        motion.target = top + (key == 'H' ? offset : key == 'L' ? rows - offset - 1u :
            (rows - 1u) / 2u);
        motion.first = true;
        editor->column_valid = false;
    }
    motion.row = 0u;
    motion.selecting = true;
    (void)snag_vm_text_wrap(text, length, columns, false, display_row, &motion);
    return motion.result;
}

struct snag_vm_motion
snag_vm_editor_motion(struct snag_vm_editor *editor, const char *text, size_t length,
    size_t at, unsigned int key, size_t count, bool counted, bool prefixed,
    bool insert, size_t columns, size_t rows, size_t top, const char *prompt)
{
    struct snag_vm_motion motion = {.at = at, .valid = true};
    if (key == 'h' || key == SNAG_VM_KEY_LEFT) {
        motion.at = insert ? snag_vm_text_previous(text, length, at) :
            left(text, length, at, count);
    } else if (key == 'l' || key == SNAG_VM_KEY_RIGHT) {
        if (insert) {
            motion.at = snag_vm_text_next(text, length, at);
            editor->column_valid = false;
            return motion;
        }
        size_t end = snag_vm_text_line_end(text, length, at);
        if (!editor->operator && end > snag_vm_text_line_start(text, length, at))
            end = snag_vm_text_previous(text, length, end);
        while (count-- && motion.at < end)
            motion.at = snag_vm_text_next(text, length, motion.at);
    } else if (key == '0' || key == SNAG_VM_KEY_HOME)
        motion.at = snag_vm_text_line_start(text, length, at);
    else if (key == '^') motion.at = first_text(text, length, at);
    else if (key == '$' || key == SNAG_VM_KEY_END) {
        size_t line = line_number(text, at);
        size_t target = count - 1u > SIZE_MAX - line ? SIZE_MAX : line + count - 1u;
        motion.at = snag_vm_text_line_end(text, length, line_at(text, length, target));
        if (!insert && motion.at > snag_vm_text_line_start(text, length, motion.at))
            motion.at = snag_vm_text_previous(text, length, motion.at);
        motion.inclusive = true;
        editor->column = SIZE_MAX;
        editor->column_valid = true;
        editor->column_display = false;
        return motion;
    } else if (key == 'G' || (prefixed && key == 'g')) {
        size_t target = counted ? count - 1u : key == 'G' ? SIZE_MAX : 0u;
        motion.at = first_text(text, length, line_at(text, length, target));
        motion.lines = true;
    } else if (key == 'w' || key == 'b' || key == 'e') {
        bool change = key == 'w' && editor->operator == 'c' && word_class(text, length, at);
        motion.at = word_move(text, length, at, count, change ? 'e' : key,
            change, editor->operator != 0u);
        motion.inclusive = key == 'e' || change;
    } else if (key == 'H' || key == 'M' || key == 'L') {
        motion.at = display_move(editor, text, length, at, count, key, columns, rows, top, prompt);
        motion.lines = true;
    } else if (key == 'j' || key == 'k' || key == SNAG_VM_KEY_UP || key == SNAG_VM_KEY_DOWN ||
        key == SNAG_VM_KEY_PAGE_UP || key == SNAG_VM_KEY_PAGE_DOWN) {
        bool down = key == 'j' || key == SNAG_VM_KEY_DOWN || key == SNAG_VM_KEY_PAGE_DOWN;
        if (key == SNAG_VM_KEY_PAGE_UP || key == SNAG_VM_KEY_PAGE_DOWN)
            count = rows && count <= SIZE_MAX / rows ? count * rows : SIZE_MAX;
        if (prefixed) motion.at = display_move(editor, text, length, at, count,
            down ? 'j' : 'k', columns, rows, top, prompt);
        else {
            if (!editor->column_valid || editor->column_display) editor->column =
                snag_vm_text_column(text, length, at, false);
            editor->column_valid = true;
            editor->column_display = false;
            size_t line = line_number(text, at);
            size_t target = down ? (count > SIZE_MAX - line ? SIZE_MAX : line + count) :
                count > line ? 0u : line - count;
            motion.at = snag_vm_text_at_column(text, length,
                line_at(text, length, target), editor->column, false);
            motion.lines = true;
        }
        return motion;
    } else motion.valid = false;
    if (motion.valid) editor->column_valid = false;
    return motion;
}

static int
register_text(struct snag_vm_register *reg, const char *text, size_t length, bool lines)
{
    struct snag_buf next = {.max = SIZE_MAX};
    if (snag_buf_append(&next, text, length) < 0 ||
        (lines && (!length || text[length - 1u] != '\n') && snag_buf_putc(&next, '\n') < 0)) {
        snag_buf_free(&next);
        return -1;
    }
    snag_vm_register_free(reg);
    reg->text = next;
    reg->length = next.len;
    reg->lines = lines;
    return 0;
}

static enum snag_vm_edit_result
operate(struct snag_vm_buffer *buffer, struct snag_vm_register *reg,
    struct snag_vm_motion motion, unsigned int operator)
{
    const char *text = buffer->draft.len ? (const char *)buffer->draft.data : "";
    size_t length = buffer->draft.len, cursor = buffer->cursor;
    size_t begin = cursor < motion.at ? cursor : motion.at;
    size_t end = cursor > motion.at ? cursor : motion.at;
    if (motion.lines) {
        begin = snag_vm_text_line_start(text, length, begin);
        end = snag_vm_text_line_end(text, length, end);
        if (end < length) ++end;
    } else if (motion.inclusive && end < length && text[end] != '\n')
        end = snag_vm_text_next(text, length, end);
    if (register_text(reg, text + begin, end - begin, motion.lines) < 0)
        return SNAG_VM_EDIT_ERROR;
    if (operator == 'y') {
        if (motion.at < cursor) snag_vm_draft_cursor(buffer, motion.at);
        return end > begin ? SNAG_VM_EDIT_YANK : SNAG_VM_EDIT_DONE;
    }
    const char *replacement = "";
    size_t replacement_size = 0u;
    if (motion.lines && operator == 'c' && end && text[end - 1u] == '\n') {
        replacement = "\n";
        replacement_size = 1u;
    } else if (motion.lines && operator == 'd' && end == length && begin &&
        (begin == end || text[end - 1u] != '\n')) {
        --begin;
    }
    if (snag_vm_editor_replace(buffer, begin, end, replacement, replacement_size) < 0)
        return SNAG_VM_EDIT_ERROR;
    snag_vm_draft_cursor(buffer, begin);
    if (operator == 'c') return SNAG_VM_EDIT_INSERT;
    snag_vm_editor_normal(buffer, false);
    if (motion.lines) {
        size_t first = first_text((const char *)buffer->draft.data,
            buffer->draft.len, buffer->cursor);
        snag_vm_draft_cursor(buffer, first);
    }
    return snag_vm_editor_end(buffer) < 0 ? SNAG_VM_EDIT_ERROR : SNAG_VM_EDIT_DONE;
}

static int
put_padding(struct snag_buf *text, size_t count)
{
    static const char spaces[] = "                                ";
    if (count > text->max - text->len) return snag_errno(EOVERFLOW);
    while (count) {
        size_t size = count < sizeof(spaces) - 1u ? count : sizeof(spaces) - 1u;
        if (snag_buf_append(text, spaces, size) < 0) return -1;
        count -= size;
    }
    return 0;
}

static int
put_block_row(struct snag_buf *out, const char *draft, size_t length,
    size_t *line, size_t column, const struct snag_buf *row, size_t count,
    bool more, size_t *cursor)
{
    bool extending = *line > length;
    size_t start = extending ? length : *line;
    size_t end = extending ? length : snag_vm_text_line_end(draft, length, start);
    size_t at = extending ? length : snag_vm_text_at_column(draft, length, start, column, false);
    size_t actual = extending ? 0u : snag_vm_text_column(draft, length, at, false);
    struct snag_vm_glyph glyph = snag_vm_glyph(draft + at, end - at, actual, false);
    bool split_tab = glyph.tab && column > actual;
    if (snag_buf_append(out, draft + start, at - start) < 0) return -1;
    if ((at == end || split_tab) && column > actual &&
        put_padding(out, column - actual) < 0) return -1;
    if (*cursor == SIZE_MAX) *cursor = out->len;
    if (row->len && count > (out->max - out->len) / row->len) return snag_errno(EOVERFLOW);
    for (size_t i = 0u; row->len && i < count; ++i)
        if (snag_buf_append(out, row->data, row->len) < 0) return -1;
    if (split_tab) {
        if (put_padding(out, glyph.columns - (column - actual)) < 0) return -1;
        at += glyph.bytes;
    }
    if (snag_buf_append(out, draft + at, end - at) < 0) return -1;
    if ((end < length || more) && snag_buf_putc(out, '\n') < 0) return -1;
    *line = end < length ? end + 1u : length + 1u;
    return 0;
}

static enum snag_vm_edit_result
put_block(struct snag_vm_buffer *buffer, struct snag_vm_register *reg,
    bool after, size_t count)
{
    const char *draft = buffer->draft.len ? (const char *)buffer->draft.data : "";
    size_t length = buffer->draft.len, at = buffer->cursor;
    if (after && at < length && draft[at] != '\n') at = snag_vm_text_next(draft, length, at);
    size_t column = snag_vm_text_column(draft, length, at, false);
    size_t line = snag_vm_text_line_start(draft, length, at), cursor = SIZE_MAX;
    struct snag_buf out = {.max = SNAG_MAX_DIRECT_PROMPT};
    struct snag_buf row = {.max = SNAG_MAX_DIRECT_PROMPT};
    int rc = snag_buf_append(&out, draft, line);
    unsigned char bytes[65536];
    for (uint64_t offset = 0u; !rc && offset < reg->length;) {
        size_t size = reg->length - offset < sizeof(bytes) ?
            (size_t)(reg->length - offset) : sizeof(bytes);
        rc = snag_vm_register_read(reg, offset, bytes, size);
        for (size_t begin = 0u; !rc && begin < size;) {
            unsigned char *newline = memchr(bytes + begin, '\n', size - begin);
            size_t end = newline ? (size_t)(newline - bytes) : size;
            rc = snag_buf_append(&row, bytes + begin, end - begin);
            if (!rc && newline) {
                rc = put_block_row(&out, draft, length, &line, column, &row, count,
                    offset + end + 1u < reg->length, &cursor);
                snag_buf_reset(&row);
            }
            begin = newline ? end + 1u : end;
        }
        offset += size;
    }
    if (!rc && row.len) rc = put_block_row(&out, draft, length, &line, column,
        &row, count, false, &cursor);
    if (!rc && line < length) rc = snag_buf_append(&out, draft + line, length - line);
    if (!rc) rc = snag_vm_editor_replace(buffer, 0u, length, out.data, out.len);
    if (!rc) {
        snag_vm_draft_cursor(buffer, cursor == SIZE_MAX ? 0u : cursor);
        rc = snag_vm_editor_end(buffer);
    }
    snag_buf_free(&row);
    snag_buf_free(&out);
    return rc < 0 ? SNAG_VM_EDIT_ERROR : SNAG_VM_EDIT_DONE;
}

static enum snag_vm_edit_result
put(struct snag_vm_buffer *buffer, struct snag_vm_register *reg,
    bool after, size_t count)
{
    if (!reg->length) return SNAG_VM_EDIT_DONE;
    if (reg->block) return put_block(buffer, reg, after, count);
    const char *text = buffer->draft.len ? (const char *)buffer->draft.data : "";
    size_t at = buffer->cursor;
    bool leading_newline = false;
    if (reg->lines) {
        at = snag_vm_text_line_start(text, buffer->draft.len, at);
        if (after) {
            at = snag_vm_text_line_end(text, buffer->draft.len, at);
            if (at < buffer->draft.len) ++at;
            else leading_newline = true;
        }
    } else if (after && at < buffer->draft.len && text[at] != '\n')
        at = snag_vm_text_next(text, buffer->draft.len, at);
    size_t available = SNAG_MAX_DIRECT_PROMPT - buffer->draft.len;
    if (count > available / reg->length) return snag_errno(EOVERFLOW);
    struct snag_buf insertion = {.max = SNAG_MAX_DIRECT_PROMPT};
    int rc = leading_newline ? snag_buf_putc(&insertion, '\n') : 0;
    for (size_t i = 0u; !rc && i < count; ++i) {
        size_t length = (size_t)reg->length -
            (leading_newline && i + 1u == count ? 1u : 0u);
        unsigned char bytes[65536];
        for (size_t at = 0u; !rc && at < length;) {
            size_t size = length - at < sizeof(bytes) ? length - at : sizeof(bytes);
            rc = snag_vm_register_read(reg, at, bytes, size);
            if (!rc) rc = snag_buf_append(&insertion, bytes, size);
            at += size;
        }
    }
    if (!rc) rc = snag_vm_editor_replace(buffer, at, at, insertion.data, insertion.len);
    if (!rc) {
        if (reg->lines) snag_vm_draft_cursor(buffer,
            first_text((const char *)buffer->draft.data, buffer->draft.len,
                at + (leading_newline ? 1u : 0u)));
        else snag_vm_editor_normal(buffer, true);
        rc = snag_vm_editor_end(buffer);
    }
    snag_buf_free(&insertion);
    return rc < 0 ? SNAG_VM_EDIT_ERROR : SNAG_VM_EDIT_DONE;
}

static const char *
insert_key(const struct snag_vm_input_event *event, unsigned char *control)
{
    if ((event->modifiers & SNAG_VM_CTRL) && event->key >= '@' && event->key <= '~') {
        *control = event->key & 0x1fu;
        return (const char *)control;
    }
    bool word = (event->modifiers & (SNAG_VM_ALT | SNAG_VM_CTRL)) != 0u;
    switch (event->key) {
    case SNAG_VM_KEY_ENTER: return "\r";
    case SNAG_VM_KEY_TAB: return event->modifiers & SNAG_VM_SHIFT ? "\033[Z" : "\t";
    case SNAG_VM_KEY_BACKSPACE: return "\177";
    case SNAG_VM_KEY_UP: return "\033[A";
    case SNAG_VM_KEY_DOWN: return "\033[B";
    case SNAG_VM_KEY_LEFT: return word ? "\033[1;5D" : "\033[D";
    case SNAG_VM_KEY_RIGHT: return word ? "\033[1;5C" : "\033[C";
    case SNAG_VM_KEY_HOME: return "\033[H";
    case SNAG_VM_KEY_END: return "\033[F";
    case SNAG_VM_KEY_DELETE: return "\033[3~";
    case SNAG_VM_KEY_UPLOAD: return "\033[9002~";
    default: return NULL;
    }
}

static enum snag_vm_edit_result
insert_input(struct snag_vm_buffer *buffer, const struct snag_vm_input_event *event,
    size_t columns, size_t rows, const char *prompt)
{
    unsigned char control[2] = {0};
    const unsigned char *bytes = event->text;
    size_t length = event->length;
    bool paste = event->kind == SNAG_VM_PASTE_BEGIN ||
        event->kind == SNAG_VM_PASTE_TEXT || event->kind == SNAG_VM_PASTE_END;
    if (event->kind != SNAG_VM_TEXT && !paste) {
        if (event->kind != SNAG_VM_KEY ||
            ((event->modifiers & SNAG_VM_CTRL) && event->key == 'z'))
            return SNAG_VM_EDIT_UNUSED;
        bytes = (const unsigned char *)insert_key(event, control);
        if (!bytes) return SNAG_VM_EDIT_UNUSED;
        length = strlen((const char *)bytes);
    }
    struct snag_vm_editor *editor = &buffer->editor;
    if (!editor->input) {
        editor->input = malloc(sizeof(*editor->input));
        if (!editor->input) return SNAG_VM_EDIT_ERROR;
        snag_term_init(editor->input);
        editor->input->input_only = editor->input->defer_redraw = true;
        editor->input->capable = editor->input->blank_local = true;
        editor->input->feedback = snag_vm_buffer_feedback;
        editor->input->feedback_opaque = buffer;
        snag_term_set_commands(editor->input, snag_commands, snag_command_count());
    }
    if (snag_vm_editor_begin(buffer) < 0) return SNAG_VM_EDIT_ERROR;
    struct snag_term *term = editor->input;
    if (!term->history.global_path && buffer->connection->dotdir &&
        snag_history_snapshot_open(&term->history, buffer->connection->dotdir,
            buffer->connection->session) < 0) return SNAG_VM_EDIT_ERROR;
    term->columns = (unsigned int)columns;
    term->rows = (unsigned int)rows;
    const json_t *state = buffer->connection->state;
    const json_t *owner_prompt = json_object_get(state, "prompt");
    term->active = json_is_true(json_object_get(
        json_is_object(owner_prompt) ? owner_prompt : state, "active"));
    term->chat = json_is_object(buffer->route);
    term->conversation_tabs = buffer->connection->buffers &&
        buffer->connection->buffers->next;
    json_decref(term->irc_names);
    term->irc_names = json_incref(json_object_get(buffer->connection->state, "irc_names"));
    if (term->chat && snag_view_conversation_read(buffer->route, &term->conversation) < 0)
        return SNAG_VM_EDIT_ERROR;
    if (!snag_strcpy(term->label, sizeof(term->label), prompt)) return SNAG_VM_EDIT_ERROR;

    /* The buffer owns the draft across persistence and Vim edits. Lend it to
     * the terminal editor for this input step without a second text copy. */
    struct snag_buf spare = term->draft;
    term->draft = buffer->draft;
    term->cursor = buffer->cursor;
    enum snag_term_action action = SNAG_TERM_NONE;
    char *submitted = NULL;
    int rc = length || paste ? 0 : snag_term_history_step(term);
    if (event->kind == SNAG_VM_PASTE_BEGIN) snag_term_paste_begin(term);
    else if (event->kind == SNAG_VM_PASTE_TEXT) rc = snag_term_paste_append(term, bytes, length);
    else if (event->kind == SNAG_VM_PASTE_END) rc = snag_term_paste_end(term);
    else for (size_t i = 0u; !rc && i < length; ++i)
        rc = snag_term_feed_byte(term, bytes[i], &action, &submitted);
    if (action == SNAG_TERM_UPLOAD)
        editor->upload_directory = submitted && !strcmp(submitted, "trz -d");
    if (submitted) {
        /* Admission retains the draft until its durable request is prepared. */
        if (action != SNAG_TERM_UPLOAD) {
            rc = snag_buf_append(&term->draft, submitted, strlen(submitted));
            term->cursor = term->draft.len;
        }
        free(submitted);
    }
    buffer->draft = term->draft;
    buffer->cursor = term->cursor;
    term->draft = spare;
    buffer->draft_dirty = true;
    ++buffer->connection->revision;
    if (snag_buf_terminate(&buffer->draft) < 0 || rc < 0) return SNAG_VM_EDIT_ERROR;
    switch (action) {
    case SNAG_TERM_SUBMIT: return SNAG_VM_EDIT_SUBMIT;
    case SNAG_TERM_QUEUE: return SNAG_VM_EDIT_QUEUE;
    case SNAG_TERM_VIEW: return SNAG_VM_EDIT_VIEW;
    case SNAG_TERM_CANCEL: return SNAG_VM_EDIT_CANCEL;
    case SNAG_TERM_INTERRUPT: return SNAG_VM_EDIT_INTERRUPT;
    case SNAG_TERM_EXIT: return SNAG_VM_EDIT_EXIT;
    case SNAG_TERM_UPLOAD: return SNAG_VM_EDIT_UPLOAD;
    default: return SNAG_VM_EDIT_DONE;
    }
}

int
snag_vm_editor_poll(struct snag_vm_buffer *buffer)
{
    struct snag_term *term = buffer->editor.input;
    if (!term || !term->history_pending) return 0;
    const struct snag_vm_input_event tick = {.kind = SNAG_VM_TEXT};
    char prompt[SNAG_TERM_LABEL_BYTES];
    memcpy(prompt, term->label, sizeof(prompt));
    return insert_input(buffer, &tick, term->columns, term->rows, prompt) < 0 ? -1 : 1;
}

enum snag_vm_edit_result
snag_vm_editor_key(struct snag_vm_buffer *buffer, struct snag_vm_register *reg,
    const struct snag_vm_input_event *event, bool insert, size_t columns, size_t rows,
    size_t top, const char *prompt)
{
    struct snag_vm_editor *editor = &buffer->editor;
    unsigned int key = event->kind == SNAG_VM_TEXT && event->length == 1u ? event->text[0] :
        event->kind == SNAG_VM_KEY ? event->key : 0u;
    bool control = (event->modifiers & SNAG_VM_CTRL) != 0u;
    const char *text = buffer->draft.len ? (const char *)buffer->draft.data : "";
    size_t length = buffer->draft.len, at = buffer->cursor;
    if (key == SNAG_VM_KEY_ESCAPE || (control && key == '[')) {
        snag_vm_editor_normal(buffer, insert);
        return snag_vm_editor_end(buffer) < 0 ? SNAG_VM_EDIT_ERROR : SNAG_VM_EDIT_NORMAL;
    }
    if (insert) return insert_input(buffer, event, columns, rows, prompt);
    if (!control && key >= '0' && key <= '9' && (key != '0' || editor->count)) {
        size_t digit = key - '0';
        if (editor->count > (SIZE_MAX - digit) / 10u) {
            snag_vm_editor_normal(buffer, false);
            return snag_errno(EOVERFLOW);
        }
        editor->count = editor->count * 10u + digit;
        return SNAG_VM_EDIT_DONE;
    }
    bool counted = editor->count || editor->operator_count;
    size_t count = editor->count ? editor->count : 1u;
    if (editor->operator_count) {
        if (count > SIZE_MAX / editor->operator_count) {
            snag_vm_editor_normal(buffer, false);
            return snag_errno(EOVERFLOW);
        }
        count *= editor->operator_count;
    }
    if (!control && (key == 'g' || (!editor->operator && key == 'z')) && !editor->prefix) {
        editor->prefix = key;
        return SNAG_VM_EDIT_DONE;
    }
    if (!control && editor->prefix == 'z' && key == 'z') {
        if (counted) {
            struct snag_vm_motion motion = snag_vm_editor_motion(editor, text, length, at,
                'G', count, true, false, false, columns, rows, top, prompt);
            snag_vm_draft_cursor(buffer, motion.at);
        }
        snag_vm_editor_normal(buffer, false);
        return SNAG_VM_EDIT_CENTER;
    }
    if (!control && editor->prefix &&
        (editor->prefix != 'g' || (key != 'g' && key != 'j' && key != 'k'))) {
        snag_vm_editor_normal(buffer, false);
        return snag_errno(ENOTSUP);
    }
    if (!control && (key == 'd' || key == 'c' || key == 'y') && !editor->operator) {
        editor->operator = key;
        editor->operator_count = count;
        editor->count = 0u;
        return SNAG_VM_EDIT_DONE;
    }
    unsigned int operator = editor->operator;
    bool prefixed = editor->prefix == 'g';
    enum snag_vm_edit_result result = SNAG_VM_EDIT_DONE;
    if (!control && !operator && (key == 'i' || key == 'a' || key == 'I' || key == 'A')) {
        if (key == 'a' && at < length && text[at] != '\n') at = snag_vm_text_next(text, length, at);
        if (key == 'I') at = first_text(text, length, at);
        if (key == 'A') at = snag_vm_text_line_end(text, length, at);
        snag_vm_draft_cursor(buffer, at);
        result = SNAG_VM_EDIT_INSERT;
    } else if (!control && !operator && (key == 'o' || key == 'O')) {
        at = key == 'o' ? snag_vm_text_line_end(text, length, at) :
            snag_vm_text_line_start(text, length, at);
        if (snag_vm_editor_replace(buffer, at, at, "\n", 1u) < 0) result = SNAG_VM_EDIT_ERROR;
        else {
            snag_vm_draft_cursor(buffer, at + (key == 'o' ? 1u : 0u));
            result = SNAG_VM_EDIT_INSERT;
        }
    } else if (!operator && ((!control && key == 'u') || (control && key == 'r'))) {
        result = snag_vm_editor_undo(buffer, control, count) < 0 ?
            SNAG_VM_EDIT_ERROR : SNAG_VM_EDIT_DONE;
        snag_vm_editor_normal(buffer, false);
    } else if (!control && !operator && (key == 'p' || key == 'P'))
        result = put(buffer, reg, key == 'p', count);
    else if (!operator && ((!control && key == 'x') || key == SNAG_VM_KEY_DELETE)) {
        size_t end = snag_vm_text_line_end(text, length, at);
        size_t target = at;
        while (count-- && target < end) target = snag_vm_text_next(text, length, target);
        result = target == at ? SNAG_VM_EDIT_DONE : operate(buffer, reg,
            (struct snag_vm_motion){.at = target, .valid = true}, 'd');
    } else {
        if (control && (key == 'u' || key == 'd' || key == 'b' || key == 'f')) {
            size_t page = key == 'u' || key == 'd' ? (rows / 2u ? rows / 2u : 1u) : rows;
            count = page && count <= SIZE_MAX / page ? count * page : SIZE_MAX;
            key = key == 'u' || key == 'b' ? 'k' : 'j';
            control = false;
        }
        struct snag_vm_motion motion = {0};
        if (!control && operator && key == operator) {
            size_t line = line_number(text, at);
            size_t target = count - 1u > SIZE_MAX - line ? SIZE_MAX : line + count - 1u;
            motion = (struct snag_vm_motion){.at = line_at(text, length, target),
                .valid = true, .lines = true};
        } else if (!control) motion = snag_vm_editor_motion(editor, text, length, at,
            key, count, counted, prefixed, false, columns, rows, top, prompt);
        if (motion.valid) {
            if (operator) result = operate(buffer, reg, motion, operator);
            else {
                snag_vm_draft_cursor(buffer, motion.at);
                snag_vm_editor_normal(buffer, false);
            }
        } else if (control || key == ':' || key == '/' || key == '?' || key == SNAG_VM_KEY_TAB)
            result = SNAG_VM_EDIT_UNUSED;
        else result = snag_errno(ENOTSUP);
    }
    editor->operator = editor->prefix = 0u;
    editor->count = editor->operator_count = 0u;
    return result;
}

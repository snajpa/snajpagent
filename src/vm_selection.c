/* SPDX-License-Identifier: GPL-2.0-only */
#include "vm_selection.h"
#include "fs.h"
#include "unicode.h"
#include "vm_source.h"
#include "vm_text.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* A storage quantum, not a copy limit. Beyond it the register streams to disk. */
#define REGISTER_MEMORY 65536u

void
snag_vm_register_free(struct snag_vm_register *reg)
{
    if (!reg) return;
    if (reg->file) (void)fclose(reg->file);
    if (*reg->path) (void)snag_unlink_at(reg->directory, reg->path, false);
    snag_buf_free(&reg->text);
    memset(reg, 0, sizeof(*reg));
}

static int
spill(struct snag_vm_register *reg, int directory)
{
    char id[SNAG_ID_HEX_LEN + 1u];
    if (snag_random_id(id) < 0) return -1;
    (void)snprintf(reg->path, sizeof(reg->path), ".vm-register-%s", id);
    reg->directory = directory;
    int fd = snag_create_private_at(directory, reg->path, true);
    if (fd < 0) { reg->path[0] = 0; return -1; }
    reg->file = fdopen(fd, "w+b");
    if (!reg->file) {
        int saved = errno;
        (void)close(fd);
        (void)snag_unlink_at(directory, reg->path, false);
        reg->path[0] = 0;
        return snag_errno(saved);
    }
#ifndef _WIN32
    if (snag_unlink_at(directory, reg->path, false) == 0) reg->path[0] = 0;
#endif
    if (reg->text.len && fwrite(reg->text.data, 1u, reg->text.len, reg->file) != reg->text.len)
        return -1;
    snag_buf_free(&reg->text);
    return 0;
}

int
snag_vm_register_append(struct snag_vm_register *reg, int directory,
    const void *bytes, size_t length)
{
    if (length > INT64_MAX - reg->length) return snag_errno(EOVERFLOW);
    if (!length) return 0;
    if (!reg->file && reg->length + length > REGISTER_MEMORY && spill(reg, directory) < 0)
        return -1;
    if (reg->file) {
        if (fwrite(bytes, 1u, length, reg->file) != length) return -1;
    } else {
        reg->text.max = REGISTER_MEMORY;
        if (snag_buf_append(&reg->text, bytes, length) < 0) return -1;
    }
    reg->length += length;
    return 0;
}

int
snag_vm_register_read(struct snag_vm_register *reg, uint64_t offset, void *bytes, size_t length)
{
    if (offset > reg->length || length > reg->length - offset) return snag_errno(EINVAL);
    if (!length) return 0;
    if (!reg->file) { memcpy(bytes, reg->text.data + (size_t)offset, length); return 0; }
    if (fflush(reg->file) < 0) return -1;
    size_t done = 0u;
    while (done < length) {
        ssize_t count = snag_pread(fileno(reg->file), (char *)bytes + done,
            length - done, (int64_t)(offset + done));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return snag_errno(count < 0 ? errno : EIO);
        done += (size_t)count;
    }
    return 0;
}

static int
clear_register(struct snag_vm_register *reg)
{
    if (reg->file) {
        if (fflush(reg->file) < 0 || snag_truncate(fileno(reg->file), 0) < 0) return -1;
        rewind(reg->file);
    } else snag_buf_reset(&reg->text);
    reg->length = 0u;
    return 0;
}

int
snag_vm_anchor_compare(const struct snag_vm_anchor *a, const struct snag_vm_anchor *b)
{
    if (strcmp(a->key, b->key)) {
        if (a->seq != b->seq) return a->seq < b->seq ? -1 : 1;
        if (a->order != b->order) return a->order < b->order ? -1 : 1;
    }
    if (a->heading != b->heading) return a->heading ? -1 : 1;
    return a->byte < b->byte ? -1 : a->byte > b->byte;
}

struct snag_vm_copy {
    struct snag_vm_selection selection;
    struct snag_vm_register result, line, rectangle;
    struct snag_grapheme_state boundary;
    struct snag_grapheme_cells cells;
    uint64_t cluster_start;
    struct snag_vm_anchor field_end;
    unsigned int cluster_columns;
    bool last_cluster, cluster_tab;
    int directory;
    uint64_t first_byte, last_byte, source_end;
    bool first, last, active, done, field, pending, last_newline;
    char key[160], handle[SNAG_ID_HEX_LEN + 1u];
    unsigned int stream;
    size_t column;
};

struct snag_vm_copy *
snag_vm_copy_open(const struct snag_vm_selection *selection, int directory)
{
    if (!selection || selection->kind < SNAG_VM_SELECT_CHAR ||
        selection->kind > SNAG_VM_SELECT_BLOCK || !*selection->first.key ||
        !*selection->last.key || (selection->kind == SNAG_VM_SELECT_BLOCK &&
        selection->left >= selection->right)) { errno = EINVAL; return NULL; }
    struct snag_vm_copy *copy = calloc(1u, sizeof(*copy));
    if (!copy) return NULL;
    copy->selection = *selection;
    if (snag_vm_anchor_compare(&selection->first, &selection->last) > 0) {
        copy->selection.first = selection->last;
        copy->selection.last = selection->first;
    }
    copy->directory = directory;
    copy->result.lines = selection->kind == SNAG_VM_SELECT_LINE;
    copy->result.block = selection->kind == SNAG_VM_SELECT_BLOCK;
    return copy;
}

void
snag_vm_copy_close(struct snag_vm_copy *copy)
{
    if (!copy) return;
    snag_vm_register_free(&copy->line);
    snag_vm_register_free(&copy->rectangle);
    snag_vm_register_free(&copy->result);
    free(copy);
}

static int
append_range(struct snag_vm_copy *copy, struct snag_vm_register *from,
    struct snag_vm_register *to, uint64_t begin, uint64_t end,
    bool (*cancel)(void *), void *opaque)
{
    unsigned char bytes[REGISTER_MEMORY];
    while (begin < end) {
        if (cancel && cancel(opaque)) return snag_errno(ECANCELED);
        size_t count = end - begin < sizeof(bytes) ? (size_t)(end - begin) : sizeof(bytes);
        if (snag_vm_register_read(from, begin, bytes, count) < 0 ||
            snag_vm_register_append(to, copy->directory, bytes, count) < 0) return -1;
        begin += count;
    }
    return 0;
}

static int
spaces(struct snag_vm_copy *copy, size_t count)
{
    static const char padding[] =
        "                                                                ";
    while (count) {
        size_t bytes = count < sizeof(padding) - 1u ? count : sizeof(padding) - 1u;
        if (snag_vm_register_append(&copy->rectangle, copy->directory, padding, bytes) < 0)
            return -1;
        count -= bytes;
    }
    return 0;
}

static int
finish_cluster(struct snag_vm_copy *copy, bool (*cancel)(void *), void *opaque)
{
    if (copy->line.length == copy->cluster_start) return 0;
    int width = copy->cluster_columns ? (int)copy->cluster_columns :
        snag_grapheme_cells_width(&copy->cells);
    if (width < 0) return -1;
    if (!width) width = 1;
    if ((size_t)width > SIZE_MAX - copy->column) return snag_errno(EOVERFLOW);
    size_t end = copy->column + (size_t)width;
    if (copy->selection.kind == SNAG_VM_SELECT_BLOCK &&
        copy->column < copy->selection.right && end > copy->selection.left) {
        if (copy->cluster_tab) {
            size_t left = copy->column > copy->selection.left ?
                copy->column : copy->selection.left;
            size_t right = end < copy->selection.right ? end : copy->selection.right;
            if (spaces(copy, right - left) < 0) return -1;
        } else if (append_range(copy, &copy->line, &copy->rectangle,
            copy->cluster_start, copy->line.length, cancel, opaque) < 0) return -1;
    }
    if (copy->last_cluster) copy->last_byte = copy->line.length;
    copy->cluster_start = copy->line.length;
    copy->column = end;
    copy->cells = (struct snag_grapheme_cells){0};
    copy->cluster_columns = 0u;
    copy->cluster_tab = copy->last_cluster = false;
    return 0;
}

static int
finish_line(struct snag_vm_copy *copy, bool newline, bool (*cancel)(void *), void *opaque)
{
    if (finish_cluster(copy, cancel, opaque) < 0) return -1;
    if (copy->first) copy->active = true;
    if (copy->active && !copy->done) {
        if (copy->selection.kind == SNAG_VM_SELECT_BLOCK && copy->column < copy->selection.right) {
            size_t start = copy->column > copy->selection.left ?
                copy->column : copy->selection.left;
            if (spaces(copy, copy->selection.right - start) < 0) return -1;
        }
        uint64_t begin = copy->selection.kind == SNAG_VM_SELECT_CHAR && copy->first ?
            copy->first_byte : 0u;
        uint64_t end = copy->selection.kind == SNAG_VM_SELECT_CHAR && copy->last ?
            copy->last_byte : copy->line.length;
        struct snag_vm_register *line = &copy->line;
        if (copy->selection.kind == SNAG_VM_SELECT_BLOCK) {
            line = &copy->rectangle;
            end = line->length;
        }
        if (append_range(copy, line, &copy->result, begin, end, cancel, opaque) < 0) return -1;
        if ((copy->selection.kind != SNAG_VM_SELECT_CHAR ||
            (newline && (!copy->last || copy->last_newline))) &&
            snag_vm_register_append(&copy->result, copy->directory, "\n", 1u) < 0) return -1;
        if (copy->last) copy->done = true;
    }
    copy->first = copy->last = copy->pending = copy->last_newline = false;
    copy->column = 0u;
    copy->cluster_start = 0u;
    copy->boundary = (struct snag_grapheme_state){0};
    if (clear_register(&copy->rectangle) < 0) return -1;
    return clear_register(&copy->line);
}

static bool
endpoint(const struct snag_vm_anchor *anchor, const json_t *block, bool heading,
    uint64_t begin, uint64_t end)
{
    return anchor->heading == heading && !strcmp(anchor->key, snag_json_string(block, "key")) &&
        anchor->byte >= begin && (anchor->byte < end || (begin == end && anchor->byte == begin));
}

static int
copy_field(struct snag_vm_copy *copy, const json_t *block, bool heading,
    bool (*cancel)(void *), void *opaque)
{
    const char *text = snag_vm_block_text(block, heading);
    if (!text) return 0;
    size_t length = strlen(text);
    if (heading && length && text[length - 1u] == '\n') --length;
    const char *kind = snag_json_string(block, "kind");
    bool prose = !heading && kind && snag_string_in(kind, "assistant refusal");
    const json_t *map = json_object_get(block, "format_map");
    size_t mapped_begin = (size_t)json_integer_value(json_array_get(json_array_get(map, 0u), 0u));
    size_t mapped_end = json_array_size(map) ? (size_t)json_integer_value(json_array_get(
        json_array_get(map, json_array_size(map) - 1u), 1u)) : length;
    struct snag_vm_anchor first = copy->selection.first, last = copy->selection.last;
    if (!heading) {
        uint64_t begin = (uint64_t)json_integer_value(json_object_get(block, "source_begin"));
        uint64_t end = (uint64_t)json_integer_value(json_object_get(block, "source_end"));
        first.byte = first.byte < begin || first.byte > end ? UINT64_MAX :
            snag_vm_source_position(block, first.byte, false);
        last.byte = last.byte < begin || last.byte > end ? UINT64_MAX :
            snag_vm_source_position(block, last.byte, false);
        if (!copy->selection.exclusive && last.byte < length) {
            size_t next = (size_t)last.byte;
            while (next < length && snag_vm_source_mapped(block, next) &&
                snag_vm_source_position(block, next, true) == copy->selection.last.byte) {
                last.byte = next;
                size_t bytes = snag_utf8_size((unsigned char)text[next]);
                next += bytes ? bytes : 1u;
            }
        }
    }
    for (size_t at = 0u; at < length && !copy->done;) {
        if (cancel && cancel(opaque)) return snag_errno(ECANCELED);
        uint32_t cp;
        size_t bytes = snag_utf8_decode((const unsigned char *)text + at, length - at, &cp);
        if (!bytes) return snag_errno(EILSEQ);
        if (prose && !snag_vm_source_mapped(block, at) &&
            (copy->selection.kind == SNAG_VM_SELECT_CHAR || at >= mapped_end ||
             (json_integer_value(json_object_get(block, "source_begin")) && at < mapped_begin))) {
            at += bytes;
            continue;
        }
        if (snag_grapheme_feed(&copy->boundary, cp) && finish_cluster(copy, cancel, opaque) < 0)
            return -1;
        struct snag_vm_glyph glyph = snag_vm_glyph(text + at, bytes, copy->column, false);
        if (!copy->first && !copy->active &&
            endpoint(&first, block, heading, at, at + bytes)) {
            copy->first = true;
            copy->first_byte = copy->cluster_start;
        }
        if ((!copy->last || !copy->selection.exclusive) &&
            endpoint(&last, block, heading, at, at + bytes)) {
            copy->last = true;
            copy->last_cluster = !copy->selection.exclusive;
            copy->last_newline = glyph.newline && !copy->selection.exclusive;
            copy->last_byte = copy->selection.exclusive ? copy->cluster_start : copy->line.length;
        }
        copy->pending = true;
        if (glyph.newline) {
            if (finish_line(copy, true, cancel, opaque) < 0) return -1;
        } else {
            if (glyph.escaped[0] || glyph.tab) copy->cluster_columns = glyph.columns;
            else if (snag_grapheme_cells_feed(&copy->cells, cp, false) < 0) return -1;
            copy->cluster_tab = glyph.tab;
            const char *value = glyph.escaped[0] ? glyph.escaped : text + at;
            size_t count = glyph.escaped[0] ? strlen(glyph.escaped) : bytes;
            if (snag_vm_register_append(&copy->line, copy->directory, value, count) < 0) return -1;
        }
        at += bytes;
    }
    (void)snprintf(copy->field_end.key, sizeof(copy->field_end.key), "%s",
        snag_json_string(block, "key"));
    copy->field_end.heading = heading;
    copy->field_end.byte = heading ? length : snag_vm_source_position(block, length, true);
    return 0;
}

static int
finish_field(struct snag_vm_copy *copy, bool newline, bool (*cancel)(void *), void *opaque)
{
    if (copy->selection.exclusive &&
        copy->selection.last.heading == copy->field_end.heading &&
        !strcmp(copy->selection.last.key, copy->field_end.key) &&
        copy->selection.last.byte == copy->field_end.byte) {
        if (finish_cluster(copy, cancel, opaque) < 0) return -1;
        copy->last = true;
        copy->last_byte = copy->line.length;
    }
    return copy->pending || copy->last ? finish_line(copy, newline, cancel, opaque) : 0;
}

int
snag_vm_copy_block(struct snag_vm_copy *copy, const json_t *block,
    bool (*cancel)(void *), void *opaque)
{
    if (copy->done) return 1;
    const char *key = snag_json_string(block, "key");
    const char *handle = snag_json_string(block, "handle");
    uint64_t begin, end;
    if (!key || strlen(key) >= sizeof(copy->key) ||
        snag_json_integer_u64(block, "source_begin", &begin) < 0 ||
        snag_json_integer_u64(block, "source_end", &end) < 0) return snag_errno(EINVAL);
    unsigned int stream = (unsigned int)json_integer_value(json_object_get(block, "stream"));
    bool contiguous = copy->field && begin == copy->source_end &&
        (handle ? !strcmp(handle, copy->handle) && stream == copy->stream :
         !strcmp(key, copy->key));
    if (!contiguous) {
        if (finish_field(copy, true, cancel, opaque) < 0) return -1;
        if (copy->done) return 1;
        const char *label = snag_vm_block_text(block, true);
        if (label && *label) {
            if (copy_field(copy, block, true, cancel, opaque) < 0 ||
                finish_field(copy, true, cancel, opaque) < 0) return -1;
        }
    }
    if (!copy->done && copy_field(copy, block, false, cancel, opaque) < 0) return -1;
    (void)snprintf(copy->key, sizeof(copy->key), "%s", key);
    (void)snprintf(copy->handle, sizeof(copy->handle), "%s", handle ? handle : "");
    copy->stream = stream;
    copy->source_end = end;
    copy->field = true;
    return copy->done ? 1 : 0;
}

int
snag_vm_copy_finish(struct snag_vm_copy *copy, struct snag_vm_register *out,
    bool (*cancel)(void *), void *opaque)
{
    if (!copy->done && finish_field(copy, false, cancel, opaque) < 0) return -1;
    if (!copy->done) return snag_errno(ESTALE);
    if (copy->result.file && fflush(copy->result.file) < 0) return -1;
    snag_vm_register_free(out);
    *out = copy->result;
    memset(&copy->result, 0, sizeof(copy->result));
    return 0;
}

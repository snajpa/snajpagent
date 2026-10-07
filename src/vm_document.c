/* SPDX-License-Identifier: GPL-2.0-only */
#include "vm_document.h"
#include "vm_source.h"
#include "vm_text.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define ROW_STRIDE 128u

struct position {
    size_t block, byte, column;
    bool heading;
};

struct snag_vm_document {
    json_t *blocks;
    struct position *points;
    size_t count, capacity, rows, refs;
    unsigned int columns;
};

static const char *
field(const struct snag_vm_document *doc, const struct position *at, size_t *length)
{
    const json_t *block = json_array_get(doc->blocks, at->block);
    const json_t *value = json_object_get(block, "display");
    if (value && at->heading) { *length = 0u; return ""; }
    if (!value) value = json_object_get(block, at->heading ? "label" : "text");
    const char *text = json_string_value(value);
    *length = json_string_length(value);
    /* Native summary labels end with a line break; the next field already
     * starts on a new display row. Keep actual transcript text unchanged. */
    if (at->heading && *length && text[*length - 1u] == '\n') --*length;
    return text;
}

static void
advance(struct position *at)
{
    if (!at->heading) ++at->block;
    at->heading = !at->heading;
    at->byte = at->column = 0u;
}

static bool
next(const struct snag_vm_document *doc, struct position *at, struct snag_vm_document_row *row)
{
    size_t length = 0u;
    size_t count = json_array_size(doc->blocks);
    const char *text = NULL;
    while (at->block < count) {
        text = field(doc, at, &length);
        if (text && at->byte < length) break;
        advance(at);
    }
    if (!text || at->block >= count) return false;
    *row = (struct snag_vm_document_row){.block = at->block, .begin = at->byte,
        .column = at->column, .heading = at->heading};
    size_t cells = 0u;
    while (at->byte < length) {
        struct snag_vm_glyph glyph = snag_vm_glyph(text + at->byte, length - at->byte,
            at->column, false);
        if (!glyph.newline && cells && glyph.columns > doc->columns - cells) break;
        if (glyph.newline) {
            row->end = at->byte;
            at->byte += glyph.bytes;
            at->column = 0u;
            return true;
        }
        cells += glyph.columns > doc->columns ? doc->columns : glyph.columns;
        at->byte += glyph.bytes;
        at->column += glyph.columns;
    }
    row->end = at->byte;
    return true;
}

struct snag_vm_document *
snag_vm_document_open(json_t *blocks, unsigned int columns, bool (*cancel)(void *), void *opaque)
{
    if (!json_is_array(blocks) || !columns) { errno = EINVAL; return NULL; }
    struct snag_vm_document *doc = calloc(1u, sizeof(*doc));
    if (!doc) return NULL;
    doc->blocks = json_incref(blocks);
    doc->columns = columns;
    doc->refs = 1u;
    struct position at = {.heading = true};
    struct snag_vm_document_row row;
    for (;;) {
        if (cancel && cancel(opaque)) { errno = ECANCELED; goto failed; }
        if (!next(doc, &at, &row)) return doc;
        if (!(doc->rows % ROW_STRIDE)) {
            if (doc->count == doc->capacity) {
                size_t capacity = doc->capacity ? doc->capacity * 2u : 16u;
                if (capacity < doc->capacity || capacity > SIZE_MAX / sizeof(*doc->points)) {
                    errno = EOVERFLOW;
                    goto failed;
                }
                struct position *points = realloc(doc->points, capacity * sizeof(*points));
                if (!points) goto failed;
                doc->points = points;
                doc->capacity = capacity;
            }
            doc->points[doc->count++] = (struct position){.block = row.block,
                .byte = row.begin, .column = row.column, .heading = row.heading};
        }
        ++doc->rows;
    }
failed:
    snag_vm_document_free(doc);
    return NULL;
}

struct snag_vm_document *
snag_vm_document_ref(struct snag_vm_document *doc)
{
    if (doc) ++doc->refs;
    return doc;
}

void
snag_vm_document_free(struct snag_vm_document *doc)
{
    if (!doc || --doc->refs) return;
    json_decref(doc->blocks);
    free(doc->points);
    free(doc);
}

size_t
snag_vm_document_rows(const struct snag_vm_document *doc)
{
    return doc ? doc->rows : 0u;
}

unsigned int
snag_vm_document_columns(const struct snag_vm_document *doc)
{
    return doc ? doc->columns : 0u;
}

int
snag_vm_document_row(const struct snag_vm_document *doc, size_t index,
    struct snag_vm_document_row *row)
{
    if (!doc || index >= doc->rows) return snag_errno(EINVAL);
    struct position at = doc->points[index / ROW_STRIDE];
    for (size_t i = 0u; i <= index % ROW_STRIDE; ++i)
        if (!next(doc, &at, row)) return snag_errno(EINVAL);
    return 0;
}

const json_t *
snag_vm_document_block(const struct snag_vm_document *doc, size_t index)
{
    return doc ? json_array_get(doc->blocks, index) : NULL;
}

const char *
snag_vm_document_text(const struct snag_vm_document *doc, const struct snag_vm_document_row *row)
{
    return snag_vm_block_text(snag_vm_document_block(doc, row->block), row->heading);
}

static bool
before(const struct position *at, size_t block, bool heading, size_t byte)
{
    if (at->block != block) return at->block < block;
    if (at->heading != heading) return at->heading;
    return at->byte <= byte;
}

static size_t
locate(const struct snag_vm_document *doc, const char *key,
    uint64_t seq, size_t byte, bool heading, bool source)
{
    if (!doc || !doc->rows) return 0u;
    size_t block = json_array_size(doc->blocks), nearest = block;
    for (size_t i = 0u; i < json_array_size(doc->blocks); ++i) {
        const json_t *candidate = json_array_get(doc->blocks, i);
        const char *name = snag_json_string(candidate, "key");
        if (key && name && !strcmp(key, name)) { block = i; break; }
        uint64_t event = (uint64_t)json_integer_value(json_object_get(candidate, "seq"));
        if (nearest == json_array_size(doc->blocks) && event >= seq) nearest = i;
    }
    if (block == json_array_size(doc->blocks)) { block = nearest; byte = 0u; heading = true; }
    if (source && !heading) {
        byte = (size_t)snag_vm_source_position(json_array_get(doc->blocks, block), byte, false);
    }
    size_t low = 0u, high = doc->count;
    while (low < high) {
        size_t middle = low + (high - low) / 2u;
        if (before(&doc->points[middle], block, heading, byte)) low = middle + 1u;
        else high = middle;
    }
    size_t index = low ? (low - 1u) * ROW_STRIDE : 0u;
    struct position at = doc->points[index / ROW_STRIDE];
    struct snag_vm_document_row row;
    while (next(doc, &at, &row)) {
        bool within = row.heading == heading && (row.end > byte || row.begin == byte);
        if (row.block > block || (row.block == block && ((!row.heading && heading) || within)))
            return index;
        ++index;
    }
    return doc->rows - 1u;
}

size_t
snag_vm_document_locate(const struct snag_vm_document *doc, const char *key,
    uint64_t seq, size_t byte, bool heading)
{
    return locate(doc, key, seq, byte, heading, false);
}

size_t
snag_vm_document_locate_source(const struct snag_vm_document *doc, const char *key,
    uint64_t seq, size_t byte, bool heading)
{
    return locate(doc, key, seq, byte, heading, true);
}

size_t
snag_vm_document_source(const struct snag_vm_document *doc,
    const struct snag_vm_document_row *row, size_t display_byte)
{
    return row->heading ? display_byte : (size_t)snag_vm_source_position(
        snag_vm_document_block(doc, row->block), display_byte, true);
}

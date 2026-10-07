/* SPDX-License-Identifier: GPL-2.0-only */
#include "vm_source.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

const char *
snag_vm_block_text(const json_t *block, bool heading)
{
    const char *display = snag_json_string(block, "display");
    return display ? heading ? "" : display :
        snag_json_string(block, heading ? "label" : "text");
}

int
snag_vm_source_replace(json_t *runs, uint64_t display, uint64_t source,
    uint64_t display_bytes, uint64_t source_bytes)
{
    if (!json_is_array(runs) || !display_bytes || !source_bytes ||
        display > INT64_MAX || source > INT64_MAX || display_bytes > INT64_MAX - display ||
        source_bytes > INT64_MAX - source) return snag_errno(EOVERFLOW);
    size_t count = json_array_size(runs);
    json_t *last = count ? json_array_get(runs, count - 1u) : NULL;
    if (last) {
        uint64_t d = (uint64_t)json_integer_value(json_array_get(last, 0u));
        uint64_t s = (uint64_t)json_integer_value(json_array_get(last, 1u));
        uint64_t dw = (uint64_t)json_integer_value(json_array_get(last, 2u));
        uint64_t sw = (uint64_t)json_integer_value(json_array_get(last, 3u));
        uint64_t n = (uint64_t)json_integer_value(json_array_get(last, 4u));
        if (d + dw * n > display || s + sw * n > source) return snag_errno(EINVAL);
        if (dw == display_bytes && sw == source_bytes &&
            d + dw * n == display && s + sw * n == source) {
            return json_array_set_new(last, 4u, json_integer((json_int_t)n + 1));
        }
    }
    return json_array_append_new(runs, json_pack("[I,I,I,I,i]", (json_int_t)display,
        (json_int_t)source, (json_int_t)display_bytes, (json_int_t)source_bytes, 1));
}

static uint64_t
source_position(const json_t *block, uint64_t byte, bool display_to_source)
{
    const json_t *runs = json_object_get(block, "source_map");
    if (!json_is_array(runs)) return byte;
    uint64_t source = (uint64_t)json_integer_value(json_object_get(block, "source_begin"));
    uint64_t display = 0u;
    size_t low = 0u, high = json_array_size(runs);
    unsigned int from = display_to_source ? 0u : 1u, to = display_to_source ? 1u : 0u;
    while (low < high) {
        size_t middle = low + (high - low) / 2u;
        const json_t *run = json_array_get(runs, middle);
        uint64_t start = (uint64_t)json_integer_value(json_array_get(run, from));
        if (start <= byte) low = middle + 1u;
        else high = middle;
    }
    if (low) {
        const json_t *run = json_array_get(runs, low - 1u);
        uint64_t begin = (uint64_t)json_integer_value(json_array_get(run, from));
        uint64_t target = (uint64_t)json_integer_value(json_array_get(run, to));
        uint64_t width = (uint64_t)json_integer_value(json_array_get(run, from + 2u));
        uint64_t step = (uint64_t)json_integer_value(json_array_get(run, to + 2u));
        uint64_t count = (uint64_t)json_integer_value(json_array_get(run, 4u));
        if (byte - begin < width * count) return target + (byte - begin) / width * step;
        display = (uint64_t)json_integer_value(json_array_get(run, 0u)) +
            (uint64_t)json_integer_value(json_array_get(run, 2u)) * count;
        source = (uint64_t)json_integer_value(json_array_get(run, 1u)) +
            (uint64_t)json_integer_value(json_array_get(run, 3u)) * count;
    }
    uint64_t begin = display_to_source ? display : source;
    uint64_t target = display_to_source ? source : display;
    return byte < begin ? target : byte - begin > UINT64_MAX - target ? UINT64_MAX :
        target + byte - begin;
}

json_t *
snag_vm_source_redactions(const char *source, const char *display,
    const struct snag_wire_secrets *secrets)
{
    json_t *runs = json_array();
    if (!runs) return NULL;
    size_t length = strlen(source), shown = 0u;
    for (size_t at = 0u; at < length;) {
        size_t matched = snag_wire_secret_match((const unsigned char *)source + at,
            length - at, secrets);
        if (matched) {
            if (snag_vm_source_replace(runs, shown, at, 17u, matched) < 0) goto failed;
            shown += 17u;
            at += matched;
        } else { ++shown; ++at; }
    }
    /* Preserve the whole span when another presentation transformation has
     * changed the field. Never invent detailed offsets from byte counts. */
    if (shown != strlen(display)) {
        (void)json_array_clear(runs);
        if (length && *display &&
            snag_vm_source_replace(runs, 0u, 0u, strlen(display), length) < 0) goto failed;
    }
    return runs;
failed:
    json_decref(runs);
    return NULL;
}

static size_t
format_after(const json_t *map, uint64_t byte)
{
    size_t low = 0u, high = json_array_size(map);
    while (low < high) {
        size_t middle = low + (high - low) / 2u;
        const json_t *run = json_array_get(map, middle);
        if ((uint64_t)json_integer_value(json_array_get(run, 0u)) <= byte) low = middle + 1u;
        else high = middle;
    }
    return low;
}

bool
snag_vm_source_mapped(const json_t *block, uint64_t byte)
{
    const json_t *map = json_object_get(block, "format_map");
    if (!json_is_array(map)) return true;
    size_t after = format_after(map, byte);
    const json_t *run = after ? json_array_get(map, after - 1u) : NULL;
    return run && byte < (uint64_t)json_integer_value(json_array_get(run, 1u));
}

/* A table can place later source bytes before earlier ones on its next visual
 * row. Display runs are ordered; reverse lookup therefore searches source spans. */
static uint64_t
format_position(const json_t *block, uint64_t byte, bool forward)
{
    const json_t *map = json_object_get(block, "format_map");
    if (!json_is_array(map)) return byte;
    size_t first = 0u, last = json_array_size(map);
    if (forward) {
        size_t low = format_after(map, byte);
        first = low ? low - 1u : 0u;
        if (low < last) last = low + 1u;
    }
    uint64_t nearest = 0u, distance = UINT64_MAX;
    for (size_t i = first; i < last; ++i) {
        const json_t *run = json_array_get(map, i);
        uint64_t display = (uint64_t)json_integer_value(json_array_get(run, 0u));
        uint64_t end = (uint64_t)json_integer_value(json_array_get(run, 1u));
        uint64_t source = (uint64_t)json_integer_value(json_array_get(run, 2u));
        uint64_t length = (uint64_t)json_integer_value(json_array_get(run, 3u));
        uint64_t begin = forward ? display : source;
        uint64_t width = forward ? end - display : length;
        uint64_t target = forward ? source : display;
        uint64_t target_width = forward ? length : end - display;
        if (byte >= begin && byte < begin + width)
            return target + (width == target_width ? byte - begin : 0u);
        uint64_t gap = byte < begin ? begin - byte : byte - begin - width;
        if (gap < distance) {
            distance = gap;
            nearest = target + (byte < begin ? 0u : target_width);
        }
    }
    return nearest;
}

uint64_t
snag_vm_source_position(const json_t *block, uint64_t byte, bool display_to_source)
{
    return display_to_source ? source_position(block, format_position(block, byte, true), true) :
        format_position(block, source_position(block, byte, false), false);
}

uint64_t
snag_vm_source_unformatted(const json_t *block, uint64_t byte)
{
    return source_position(block, byte, true);
}

/* SPDX-License-Identifier: GPL-2.0-only */
#include "vm_layout.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A snapshot also nests windows/drafts around this tree. Keep it within the
 * shared strict JSON reader's nesting bound on both save and restore. */
#define SNAG_VM_LAYOUT_DEPTH 32u

struct snag_vm_layout {
    uint64_t window;
    enum snag_vm_split split;
    unsigned int weight;
    struct snag_vm_layout *first, *second;
};

struct snag_vm_layout *
snag_vm_layout_new(uint64_t window)
{
    if (!window || window > INT64_MAX) {
        errno = EINVAL;
        return NULL;
    }
    struct snag_vm_layout *layout = calloc(1u, sizeof(*layout));
    if (layout) layout->window = window;
    return layout;
}

void
snag_vm_layout_free(struct snag_vm_layout *layout)
{
    if (!layout) return;
    snag_vm_layout_free(layout->first);
    snag_vm_layout_free(layout->second);
    free(layout);
}

bool
snag_vm_layout_contains(const struct snag_vm_layout *layout, uint64_t window)
{
    return layout && window && (layout->window == window ||
        snag_vm_layout_contains(layout->first, window) ||
        snag_vm_layout_contains(layout->second, window));
}

size_t
snag_vm_layout_count(const struct snag_vm_layout *layout)
{
    if (!layout) return 0u;
    if (layout->window) return 1u;
    return snag_vm_layout_count(layout->first) + snag_vm_layout_count(layout->second);
}

static struct snag_vm_layout *
find_leaf(struct snag_vm_layout *layout, uint64_t window, unsigned int *depth)
{
    if (layout->window == window) return layout;
    if (layout->window) return NULL;
    ++*depth;
    struct snag_vm_layout *next = snag_vm_layout_contains(layout->first, window) ?
        layout->first : layout->second;
    return find_leaf(next, window, depth);
}

int
snag_vm_layout_split(struct snag_vm_layout *layout, uint64_t existing, uint64_t added,
    enum snag_vm_split split)
{
    if (!layout || !existing || !added || added > INT64_MAX ||
        (split != SNAG_VM_HORIZONTAL && split != SNAG_VM_VERTICAL) ||
        !snag_vm_layout_contains(layout, existing) || snag_vm_layout_contains(layout, added))
        return snag_errno(EINVAL);
    unsigned int depth = 0u;
    struct snag_vm_layout *leaf = find_leaf(layout, existing, &depth);
    if (depth >= SNAG_VM_LAYOUT_DEPTH) return snag_errno(EOVERFLOW);
    struct snag_vm_layout *first = snag_vm_layout_new(existing);
    struct snag_vm_layout *second = snag_vm_layout_new(added);
    if (!first || !second) {
        snag_vm_layout_free(first);
        snag_vm_layout_free(second);
        return -1;
    }
    *leaf = (struct snag_vm_layout){.split = split, .weight = 5000u,
        .first = first, .second = second};
    return 0;
}

int
snag_vm_layout_close(struct snag_vm_layout *layout, uint64_t window)
{
    if (!layout || !window || !snag_vm_layout_contains(layout, window))
        return snag_errno(EINVAL);
    if (layout->window) return snag_errno(EBUSY);
    struct snag_vm_layout *closed = NULL, *kept = NULL;
    if (layout->first->window == window) {
        closed = layout->first;
        kept = layout->second;
    } else if (layout->second->window == window) {
        closed = layout->second;
        kept = layout->first;
    }
    if (closed) {
        *layout = *kept;
        free(closed);
        free(kept);
        return 0;
    }
    return snag_vm_layout_close(snag_vm_layout_contains(layout->first, window) ?
        layout->first : layout->second, window);
}

static size_t
axis_extent(const struct snag_vm_layout *layout, enum snag_vm_split axis)
{
    if (layout->window) return 1u;
    size_t first = axis_extent(layout->first, axis);
    size_t second = axis_extent(layout->second, axis);
    return layout->split == axis ? first + second : first > second ? first : second;
}

void
snag_vm_layout_equalize(struct snag_vm_layout *layout)
{
    if (!layout || layout->window) return;
    size_t first = axis_extent(layout->first, layout->split);
    size_t second = axis_extent(layout->second, layout->split);
    layout->weight = (unsigned int)((uint64_t)first * 10000u / (first + second));
    if (!layout->weight) layout->weight = 1u;
    if (layout->weight >= 10000u) layout->weight = 9999u;
    snag_vm_layout_equalize(layout->first);
    snag_vm_layout_equalize(layout->second);
}

int
snag_vm_layout_resize(struct snag_vm_layout *layout, uint64_t window, enum snag_vm_split axis,
    int change)
{
    if (!layout || !window || !snag_vm_layout_contains(layout, window) ||
        (axis != SNAG_VM_HORIZONTAL && axis != SNAG_VM_VERTICAL)) return snag_errno(EINVAL);
    struct snag_vm_layout *candidate = NULL;
    bool in_first = false;
    while (!layout->window) {
        bool first = snag_vm_layout_contains(layout->first, window);
        if (layout->split == axis) {
            candidate = layout;
            in_first = first;
        }
        layout = first ? layout->first : layout->second;
    }
    if (!candidate) return snag_errno(ENOENT);
    int64_t weight = (int64_t)candidate->weight + (int64_t)change * (in_first ? 100 : -100);
    candidate->weight = weight < 1 ? 1u : weight > 9999 ? 9999u : (unsigned int)weight;
    return 0;
}

static int
place(const struct snag_vm_layout *layout, uint64_t focus, struct snag_vm_rectangle area,
    int (*emit)(void *, const struct snag_vm_rectangle *), void *opaque)
{
    if (layout->window) {
        area.window = layout->window;
        area.visible = area.rows && area.columns;
        return emit(opaque, &area);
    }
    struct snag_vm_rectangle first = area, second = area;
    unsigned int extent = layout->split == SNAG_VM_VERTICAL ? area.columns : area.rows;
    unsigned int minimum = layout->split == SNAG_VM_VERTICAL ? 1u : 2u;
    if (extent < minimum * 2u + 1u || !area.rows || !area.columns) {
        bool keep_first = !snag_vm_layout_contains(layout->second, focus);
        struct snag_vm_rectangle *hidden = keep_first ? &second : &first;
        hidden->rows = hidden->columns = 0u;
    } else {
        unsigned int available = extent - 1u;
        unsigned int length = (unsigned int)((uint64_t)available * layout->weight / 10000u);
        if (length < minimum) length = minimum;
        if (length > available - minimum) length = available - minimum;
        if (layout->split == SNAG_VM_VERTICAL) {
            first.columns = length;
            second.column += length + 1u;
            second.columns = available - length;
        } else {
            first.rows = length;
            second.row += length + 1u;
            second.rows = available - length;
        }
    }
    int rc = place(layout->first, focus, first, emit, opaque);
    return rc ? rc : place(layout->second, focus, second, emit, opaque);
}

int
snag_vm_layout_place(const struct snag_vm_layout *layout, uint64_t focus,
    unsigned int rows, unsigned int columns,
    int (*emit)(void *, const struct snag_vm_rectangle *), void *opaque)
{
    if (!layout || !emit || !focus || !snag_vm_layout_contains(layout, focus))
        return snag_errno(EINVAL);
    struct snag_vm_rectangle area = {.rows = rows, .columns = columns};
    return place(layout, focus, area, emit, opaque);
}

json_t *
snag_vm_layout_json(const struct snag_vm_layout *layout)
{
    if (!layout) return NULL;
    if (layout->window) return json_pack("{s:I}", "window", (json_int_t)layout->window);
    json_t *first = snag_vm_layout_json(layout->first);
    json_t *second = snag_vm_layout_json(layout->second);
    if (!first || !second) {
        json_decref(first);
        json_decref(second);
        return NULL;
    }
    return json_pack("{s:s,s:i,s:o,s:o}", "split",
        layout->split == SNAG_VM_VERTICAL ? "vertical" : "horizontal",
        "weight", (int)layout->weight, "first", first, "second", second);
}

static struct snag_vm_layout *
load(const json_t *value, unsigned int depth, json_t *seen)
{
    errno = EINVAL;
    if (depth > SNAG_VM_LAYOUT_DEPTH) return NULL;
    uint64_t window;
    if (snag_json_exact_keys(value, "window") &&
        snag_json_integer_u64(value, "window", &window) == 0 && window) {
        char key[32];
        (void)snprintf(key, sizeof(key), "%llu", (unsigned long long)window);
        if (json_object_get(seen, key) || json_object_set_new(seen, key, json_true()) < 0)
            return NULL;
        return snag_vm_layout_new(window);
    }
    const char *split = snag_json_string(value, "split");
    uint64_t weight;
    if (!snag_json_exact_keys(value, "split weight first second") ||
        !split || (strcmp(split, "horizontal") && strcmp(split, "vertical")) ||
        snag_json_integer_u64(value, "weight", &weight) < 0 || !weight || weight >= 10000u)
        return NULL;
    struct snag_vm_layout *layout = calloc(1u, sizeof(*layout));
    if (!layout) return NULL;
    layout->split = !strcmp(split, "vertical") ? SNAG_VM_VERTICAL : SNAG_VM_HORIZONTAL;
    layout->weight = (unsigned int)weight;
    layout->first = load(json_object_get(value, "first"), depth + 1u, seen);
    if (layout->first)
        layout->second = load(json_object_get(value, "second"), depth + 1u, seen);
    if (!layout->first || !layout->second) {
        int saved = errno;
        snag_vm_layout_free(layout);
        errno = saved;
        return NULL;
    }
    return layout;
}

struct snag_vm_layout *
snag_vm_layout_load(const json_t *value, char *error, size_t size)
{
    json_t *seen = json_object();
    struct snag_vm_layout *layout = seen ? load(value, 0u, seen) : NULL;
    json_decref(seen);
    if (!layout) (void)snag_fail(error, size, errno, "cannot load workspace split layout");
    return layout;
}

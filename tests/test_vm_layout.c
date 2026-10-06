/* SPDX-License-Identifier: GPL-2.0-only */
#include "vm_layout.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

struct placement {
    struct snag_vm_rectangle items[64];
    size_t count;
    unsigned int rows, columns;
};

static int
collect(void *opaque, const struct snag_vm_rectangle *area)
{
    struct placement *p = opaque;
    assert(p->count < sizeof(p->items) / sizeof(p->items[0]));
    assert(area->row <= p->rows && area->rows <= p->rows - area->row);
    assert(area->column <= p->columns && area->columns <= p->columns - area->column);
    assert(area->visible == (area->rows != 0u && area->columns != 0u));
    for (size_t i = 0u; i < p->count; ++i) {
        const struct snag_vm_rectangle *other = &p->items[i];
        assert(other->window != area->window);
        if (!other->visible || !area->visible) continue;
        assert(other->row + other->rows <= area->row ||
            area->row + area->rows <= other->row ||
            other->column + other->columns <= area->column ||
            area->column + area->columns <= other->column);
    }
    p->items[p->count++] = *area;
    return 0;
}

static struct placement
arrange(const struct snag_vm_layout *layout, uint64_t focus, unsigned int rows,
    unsigned int columns)
{
    struct placement p = {.rows = rows, .columns = columns};
    assert(snag_vm_layout_place(layout, focus, rows, columns, collect, &p) == 0);
    assert(p.count == snag_vm_layout_count(layout));
    bool focused = false;
    for (size_t i = 0u; i < p.count; ++i) {
        if (p.items[i].window == focus)
            focused = p.items[i].visible || !rows || !columns;
    }
    assert(focused);
    return p;
}

static const struct snag_vm_rectangle *
rectangle(const struct placement *p, uint64_t window)
{
    for (size_t i = 0u; i < p->count; ++i) {
        if (p->items[i].window == window) return &p->items[i];
    }
    abort();
}

static void
mouse(void)
{
    struct snag_vm_layout *layout = snag_vm_layout_new(1u);
    assert(layout && snag_vm_layout_split(layout, 1u, 2u, SNAG_VM_VERTICAL) == 0);
    assert(snag_vm_layout_split(layout, 2u, 3u, SNAG_VM_HORIZONTAL) == 0);
    struct snag_vm_separator vertical, horizontal, missed;
    assert(snag_vm_layout_separator(layout, 1u, 21u, 81u, 10u, 40u, &vertical) == 1);
    assert(snag_vm_layout_separator(layout, 1u, 21u, 81u, 10u, 70u, &horizontal) == 1);
    assert(snag_vm_layout_separator(layout, 1u, 21u, 81u, 0u, 0u, &missed) == 0);
    assert(!missed.first && !missed.second);
    assert(snag_vm_layout_separator(layout, 1u, 21u, 81u, 30u, 40u, &missed) == 0);
    assert(snag_vm_layout_separator(layout, 3u, 1u, 1u, 0u, 0u, &missed) == 0);
    assert(snag_vm_layout_drag(layout, 1u, 21u, 81u, &vertical, 0u, 57u) == 1);
    struct placement p = arrange(layout, 1u, 21u, 81u);
    assert(rectangle(&p, 1u)->columns == 57u && rectangle(&p, 2u)->column == 58u);
    assert(snag_vm_layout_drag(layout, 1u, 21u, 81u, &horizontal, 5u, 70u) == 1);
    p = arrange(layout, 1u, 21u, 81u);
    assert(rectangle(&p, 2u)->rows == 5u && rectangle(&p, 3u)->row == 6u);
    assert(snag_vm_layout_drag(layout, 1u, 21u, 81u, &horizontal, 5u, 70u) == 0);
    assert(snag_vm_layout_drag(layout, 1u, 21u, 81u, &horizontal, UINT_MAX, 0u) == 1);
    p = arrange(layout, 1u, 21u, 81u);
    assert(rectangle(&p, 3u)->rows == 2u);
    assert(snag_vm_layout_drag(layout, 1u, 21u, 81u, &vertical, 0u, 0u) == 1);
    p = arrange(layout, 1u, 21u, 81u);
    assert(rectangle(&p, 1u)->columns == 1u);
    assert(snag_vm_layout_drag(layout, 3u, 1u, 1u, &horizontal, 0u, 0u) == 0);
    assert(snag_vm_layout_close(layout, 2u) == 0);
    assert(snag_vm_layout_drag(layout, 1u, 21u, 81u, &horizontal, 0u, 0u) < 0);
    snag_vm_layout_free(layout);
}

static void
invalid(const char *text)
{
    json_error_t json_error;
    json_t *value = json_loads(text, 0u, &json_error);
    assert(value);
    char error[256] = "";
    assert(!snag_vm_layout_load(value, error, sizeof(error)) && errno == EINVAL && *error);
    json_decref(value);
}

int
main(void)
{
    mouse();
    assert(!snag_vm_layout_new(0u) && errno == EINVAL);
    assert(!snag_vm_layout_new(UINT64_MAX) && errno == EINVAL);
    struct snag_vm_layout *layout = snag_vm_layout_new(1u);
    assert(layout && snag_vm_layout_count(layout) == 1u);
    assert(snag_vm_layout_close(layout, 1u) < 0 && errno == EBUSY);
    assert(snag_vm_layout_split(layout, 1u, 2u, SNAG_VM_VERTICAL) == 0);
    assert(snag_vm_layout_split(layout, 1u, 3u, SNAG_VM_VERTICAL) == 0);
    assert(!snag_vm_layout_contains(layout, 0u));
    snag_vm_layout_equalize(layout);
    struct placement p = arrange(layout, 1u, 20u, 120u);
    for (size_t i = 0u; i < p.count; ++i)
        assert(p.items[i].columns >= 39u && p.items[i].columns <= 40u);
    json_t *saved = snag_vm_layout_json(layout);
    assert(saved);
    for (unsigned int rows = 0u; rows < 10u; ++rows) {
        for (unsigned int cols = 0u; cols < 10u; ++cols) {
            for (uint64_t focus = 1u; focus <= 3u; ++focus)
                (void)arrange(layout, focus, rows, cols);
        }
    }
    json_t *after = snag_vm_layout_json(layout);
    assert(json_equal(saved, after));
    json_decref(after);
    assert(snag_vm_layout_split(layout, 2u, 4u, SNAG_VM_HORIZONTAL) == 0);
    p = arrange(layout, 4u, 30u, 120u);
    unsigned int original = rectangle(&p, 4u)->rows;
    assert(snag_vm_layout_resize(layout, 4u, SNAG_VM_HORIZONTAL, 10) == 0);
    p = arrange(layout, 4u, 30u, 120u);
    assert(rectangle(&p, 4u)->rows > original);
    assert(snag_vm_layout_resize(layout, 4u, SNAG_VM_VERTICAL, INT_MIN) == 0);
    p = arrange(layout, 4u, 30u, 120u);
    assert(rectangle(&p, 4u)->columns == 1u);
    assert(snag_vm_layout_resize(layout, 4u, SNAG_VM_VERTICAL, INT_MAX) == 0);
    (void)arrange(layout, 4u, UINT_MAX, UINT_MAX);
    assert(snag_vm_layout_split(layout, 4u, 1u, SNAG_VM_HORIZONTAL) < 0);
    assert(snag_vm_layout_split(layout, 99u, 5u, SNAG_VM_HORIZONTAL) < 0);
    assert(snag_vm_layout_close(layout, 99u) < 0);
    assert(snag_vm_layout_close(layout, 2u) == 0);
    assert(snag_vm_layout_count(layout) == 3u && !snag_vm_layout_contains(layout, 2u));
    assert(snag_vm_layout_close(layout, 3u) == 0);
    assert(snag_vm_layout_close(layout, 1u) == 0);
    assert(snag_vm_layout_count(layout) == 1u && snag_vm_layout_contains(layout, 4u));
    assert(snag_vm_layout_resize(layout, 4u, SNAG_VM_HORIZONTAL, 10) < 0 && errno == ENOENT);
    snag_vm_layout_free(layout);
    char error[256] = "";
    layout = snag_vm_layout_load(saved, error, sizeof(error));
    assert(layout);
    after = snag_vm_layout_json(layout);
    assert(json_equal(saved, after));
    json_decref(saved);
    json_decref(after);
    snag_vm_layout_free(layout);
    invalid("{}");
    invalid("{\"window\":0}");
    invalid("{\"window\":-1}");
    invalid("{\"window\":1,\"extra\":2}");
    invalid("{\"split\":\"vertical\",\"weight\":5000,"
        "\"first\":{\"window\":1},\"second\":{\"window\":1}}");
    invalid("{\"split\":\"horizontal\",\"weight\":0,"
        "\"first\":{\"window\":1},\"second\":{\"window\":2}}");
    invalid("{\"split\":\"diagonal\",\"weight\":5000,"
        "\"first\":{\"window\":1},\"second\":{\"window\":2}}");
    layout = snag_vm_layout_new(1u);
    assert(layout);
    for (uint64_t i = 2u; i <= 33u; ++i)
        assert(snag_vm_layout_split(layout, 1u, i, SNAG_VM_HORIZONTAL) == 0);
    assert(snag_vm_layout_split(layout, 1u, 34u, SNAG_VM_HORIZONTAL) < 0 && errno == EOVERFLOW);
    saved = snag_vm_layout_json(layout);
    assert(saved);
    struct snag_vm_layout *restored = snag_vm_layout_load(saved, error, sizeof(error));
    assert(restored && snag_vm_layout_count(restored) == 33u);
    (void)arrange(restored, 1u, 1u, 1u);
    json_decref(saved);
    snag_vm_layout_free(restored);
    snag_vm_layout_free(layout);
    puts("test_vm_layout: ok");
    return 0;
}

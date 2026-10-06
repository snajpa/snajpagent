/* SPDX-License-Identifier: GPL-2.0-only */
#include "vm_navigation.h"
#include "vm_source.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static json_t *
block(uint64_t seq, uint64_t begin, const char *text)
{
    return json_pack("{s:s,s:I,s:I,s:I,s:s,s:s,s:[]}", "key", "item/0",
        "seq", (json_int_t)seq, "source_begin", (json_int_t)begin,
        "source_end", (json_int_t)(begin + strlen(text)), "text", text,
        "label", "model", "source_map");
}

static void
check(json_t *blocks, enum snag_vm_navigation_kind kind, uint64_t from, uint64_t count,
    size_t column, bool operate, uint64_t expected, bool heading)
{
    struct snag_vm_navigation_request request = {.kind = kind,
        .count = count, .column = column, .operate = operate};
    (void)snprintf(request.start.key, sizeof(request.start.key), "item/0");
    request.start.byte = from;
    struct snag_vm_navigation *nav = snag_vm_navigation_open(&request);
    assert(nav);
    struct snag_vm_anchor result = {0};
    for (unsigned int pass = 0u;; ++pass) {
        assert(pass < 2u);
        for (size_t i = 0u; i < json_array_size(blocks); ++i) {
            int rc = snag_vm_navigation_block(nav, json_array_get(blocks, i), NULL, NULL);
            assert(rc >= 0);
            if (rc) break;
        }
        int rc = snag_vm_navigation_finish(nav, &result);
        assert(rc >= 0);
        if (!rc) break;
    }
    if (result.byte != expected || result.heading != heading)
        (void)fprintf(stderr,
            "kind=%d from=%llu count=%llu: byte=%llu heading=%d, expected=%llu/%d\n",
            kind, (unsigned long long)from, (unsigned long long)count,
            (unsigned long long)result.byte, result.heading, (unsigned long long)expected, heading);
    assert(result.byte == expected && result.heading == heading && !strcmp(result.key, "item/0"));
    snag_vm_navigation_close(nav);
}

int
main(void)
{
    json_t *blocks = json_array();
    assert(json_array_append_new(blocks, block(1u, 0u, "one t")) == 0);
    assert(json_array_append_new(blocks, block(2u, 5u, "wo\n   three\nlast")) == 0);
    check(blocks, SNAG_VM_NAV_LINE, 0u, 1u, 0u, false, 0u, true);
    check(blocks, SNAG_VM_NAV_LINE, 0u, 3u, 0u, false, 11u, false);
    check(blocks, SNAG_VM_NAV_LINE, 0u, 99u, 0u, false, 17u, false);
    check(blocks, SNAG_VM_NAV_LAST, 0u, 1u, 0u, false, 17u, false);
    check(blocks, SNAG_VM_NAV_UP, 19u, 1u, 1u, false, 9u, false);
    check(blocks, SNAG_VM_NAV_UP, 19u, 99u, 1u, false, 1u, true);
    check(blocks, SNAG_VM_NAV_DOWN, 1u, 2u, 2u, false, 19u, false);
    check(blocks, SNAG_VM_NAV_DOWN, 1u, 99u, 2u, false, 19u, false);
    check(blocks, SNAG_VM_NAV_LINE_START, 14u, 1u, 6u, false, 8u, false);
    check(blocks, SNAG_VM_NAV_LINE_FIRST, 14u, 1u, 6u, false, 11u, false);
    check(blocks, SNAG_VM_NAV_LINE_END, 1u, 2u, 1u, false, 15u, false);
    check(blocks, SNAG_VM_NAV_LEFT, 5u, 3u, 0u, false, 2u, false);
    check(blocks, SNAG_VM_NAV_RIGHT, 4u, 20u, 0u, false, 6u, false);
    check(blocks, SNAG_VM_NAV_RIGHT, 4u, 20u, 0u, true, 7u, false);
    check(blocks, SNAG_VM_NAV_WORD_NEXT, 0u, 1u, 0u, false, 4u, false);
    check(blocks, SNAG_VM_NAV_WORD_NEXT, 0u, 2u, 0u, false, 11u, false);
    check(blocks, SNAG_VM_NAV_WORD_PREVIOUS, 12u, 1u, 0u, false, 11u, false);
    check(blocks, SNAG_VM_NAV_WORD_PREVIOUS, 11u, 1u, 0u, false, 4u, false);
    check(blocks, SNAG_VM_NAV_WORD_PREVIOUS, 11u, 99u, 0u, false, 0u, true);
    check(blocks, SNAG_VM_NAV_WORD_END, 0u, 1u, 0u, false, 2u, false);
    check(blocks, SNAG_VM_NAV_WORD_END, 2u, 1u, 0u, false, 6u, false);
    check(blocks, SNAG_VM_NAV_WORD_END, 0u, 3u, 0u, false, 15u, false);
    json_decref(blocks);
    blocks = json_array();
    assert(json_array_append_new(blocks, block(1u, 0u, "e")) == 0);
    assert(json_array_append_new(blocks, block(2u, 1u, "́👩")) == 0);
    assert(json_array_append_new(blocks, block(3u, 7u, "‍💻x")) == 0);
    check(blocks, SNAG_VM_NAV_RIGHT, 0u, 1u, 0u, false, 3u, false);
    check(blocks, SNAG_VM_NAV_LEFT, 14u, 1u, 0u, false, 3u, false);
    json_decref(blocks);
    blocks = json_array();
    json_t *hidden = block(1u, 0u, "<redacted:secret>x\n");
    assert(snag_vm_source_replace(json_object_get(hidden, "source_map"),
        0u, 0u, 17u, 5u) == 0);
    assert(json_array_append_new(blocks, hidden) == 0);
    check(blocks, SNAG_VM_NAV_RIGHT, 0u, 1u, 0u, false, 5u, false);
    check(blocks, SNAG_VM_NAV_LEFT, 5u, 1u, 0u, false, 0u, false);
    check(blocks, SNAG_VM_NAV_WORD_NEXT, 0u, 1u, 0u, false, 5u, false);
    json_decref(blocks);
    (void)puts("Vim navigation: PASS");
    return 0;
}

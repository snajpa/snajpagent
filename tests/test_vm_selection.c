/* SPDX-License-Identifier: GPL-2.0-only */
#include "vm_selection.h"
#include "fs.h"
#include "vm_source.h"

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static json_t *
block(const char *key, uint64_t seq, uint64_t offset, const char *text, const char *label)
{
    return json_pack("{s:s,s:I,s:I,s:I,s:s,s:s,s:[]}", "key", key, "seq", (json_int_t)seq,
        "source_begin", (json_int_t)offset, "source_end", (json_int_t)(offset + strlen(text)),
        "text", text, "label", label, "source_map");
}

static struct snag_vm_anchor
anchor(const char *key, uint64_t byte)
{
    struct snag_vm_anchor result = {.byte = byte, .seq = 1u};
    assert(snag_strcpy(result.key, sizeof(result.key), key));
    return result;
}

static void
expect(struct snag_vm_register *reg, const char *text)
{
    assert(reg->length == strlen(text));
    char *bytes = malloc(strlen(text) + 1u);
    assert(bytes && snag_vm_register_read(reg, 0u, bytes, strlen(text)) == 0);
    assert(!memcmp(bytes, text, strlen(text)));
    free(bytes);
}

static bool
cancel(void *opaque)
{
    (void)opaque;
    return true;
}

int
main(void)
{
    char root[] = "/tmp/snajpagent-selection-XXXXXX";
    assert(mkdtemp(root));
    int dir = snag_open_read(root, true);
    assert(dir >= 0);
    json_t *a = block("item/0", 1u, 0u, "alpha be", "model");
    json_t *b = block("item/0", 2u, 8u, "ta\ngamma\ndelta", "model");
    for (unsigned int reverse = 0u; reverse < 2u; ++reverse) {
        struct snag_vm_selection selection = {.kind = SNAG_VM_SELECT_CHAR,
            .first = anchor("item/0", reverse ? 12u : 6u),
            .last = anchor("item/0", reverse ? 6u : 12u)};
        struct snag_vm_copy *copy = snag_vm_copy_open(&selection, dir);
        struct snag_vm_register reg = {0};
        assert(copy && snag_vm_copy_block(copy, a, NULL, NULL) == 0);
        assert(snag_vm_copy_block(copy, b, NULL, NULL) == 1);
        assert(snag_vm_copy_finish(copy, &reg, NULL, NULL) == 0);
        expect(&reg, "beta\nga");
        snag_vm_copy_close(copy);
        snag_vm_register_free(&reg);
    }
    struct snag_vm_selection selection = {.kind = SNAG_VM_SELECT_LINE,
        .first = anchor("item/0", 6u), .last = anchor("item/0", 12u)};
    struct snag_vm_copy *copy = snag_vm_copy_open(&selection, dir);
    struct snag_vm_register reg = {0};
    assert(snag_vm_copy_block(copy, a, NULL, NULL) == 0);
    assert(snag_vm_copy_block(copy, b, NULL, NULL) == 1);
    assert(snag_vm_copy_finish(copy, &reg, NULL, NULL) == 0 && reg.lines);
    expect(&reg, "alpha beta\ngamma\n");
    snag_vm_copy_close(copy);
    snag_vm_register_free(&reg);
    json_decref(a);
    json_decref(b);

    a = block("item/0", 1u, 0u, "a界éx\n\tz\nq", "");
    selection = (struct snag_vm_selection){.kind = SNAG_VM_SELECT_BLOCK,
        .first = anchor("item/0", 1u), .last = anchor("item/0", 12u), .left = 1u, .right = 4u};
    copy = snag_vm_copy_open(&selection, dir);
    assert(copy && snag_vm_copy_block(copy, a, NULL, NULL) == 0);
    assert(snag_vm_copy_finish(copy, &reg, NULL, NULL) == 0 && reg.block);
    expect(&reg, "界é\n   \n   \n");
    snag_vm_copy_close(copy);
    snag_vm_register_free(&reg);
    json_decref(a);

    const char *pieces[] = {"e", "́👩", "‍", "💻x\n"};
    for (unsigned int rectangle = 0u; rectangle < 2u; ++rectangle) {
        selection = (struct snag_vm_selection){.kind = rectangle ? SNAG_VM_SELECT_BLOCK :
            SNAG_VM_SELECT_CHAR, .first = anchor("item/0", 0u),
            .last = anchor("item/0", 3u), .left = 0u, .right = 3u};
        copy = snag_vm_copy_open(&selection, dir);
        uint64_t offset = 0u;
        for (size_t i = 0u; i < sizeof(pieces) / sizeof(pieces[0]); ++i) {
            a = block("item/0", i + 1u, offset, pieces[i], "");
            assert(snag_vm_copy_block(copy, a, NULL, NULL) >= 0);
            offset += strlen(pieces[i]);
            json_decref(a);
        }
        assert(snag_vm_copy_finish(copy, &reg, NULL, NULL) == 0);
        expect(&reg, rectangle ? "é👩‍💻\n" : "é👩‍💻");
        snag_vm_copy_close(copy);
        snag_vm_register_free(&reg);
    }
    a = block("item/0", 1u, 0u, "<redacted:secret> safe\n", "");
    assert(snag_vm_source_replace(json_object_get(a, "source_map"), 0u, 0u, 17u, 5u) == 0);
    for (unsigned int redacted = 0u; redacted < 2u; ++redacted) {
        selection = (struct snag_vm_selection){.kind = SNAG_VM_SELECT_CHAR,
            .first = anchor("item/0", redacted ? 0u : 6u),
            .last = anchor("item/0", redacted ? 0u : 9u)};
        copy = snag_vm_copy_open(&selection, dir);
        assert(copy && snag_vm_copy_block(copy, a, NULL, NULL) == 1);
        assert(snag_vm_copy_finish(copy, &reg, NULL, NULL) == 0);
        expect(&reg, redacted ? "<redacted:secret>" : "safe");
        snag_vm_copy_close(copy);
        snag_vm_register_free(&reg);
    }
    json_decref(a);

    size_t length = 300000u;
    char *large = malloc(length + 1u);
    assert(large);
    memset(large, 'x', length);
    large[length] = 0;
    a = block("big/0", 1u, 0u, large, "");
    selection = (struct snag_vm_selection){.kind = SNAG_VM_SELECT_LINE,
        .first = anchor("big/0", 0u), .last = anchor("big/0", length - 1u)};
    copy = snag_vm_copy_open(&selection, dir);
    assert(copy && snag_vm_copy_block(copy, a, NULL, NULL) == 0);
    assert(snag_vm_copy_finish(copy, &reg, NULL, NULL) == 0);
    assert(reg.file && !reg.text.len && reg.length == length + 1u);
    struct snag_file_privacy privacy;
    assert(snag_fd_privacy(fileno(reg.file), &privacy) == 0 && privacy.private_access);
    char last[2];
    assert(snag_vm_register_read(&reg, length - 1u, last, 2u) == 0 &&
        last[0] == 'x' && last[1] == '\n');
    snag_vm_copy_close(copy);
    copy = snag_vm_copy_open(&selection, dir);
    assert(copy && snag_vm_copy_block(copy, a, cancel, NULL) < 0 && errno == ECANCELED);
    assert(snag_vm_copy_finish(copy, &reg, NULL, NULL) < 0 && reg.length == length + 1u);
    snag_vm_copy_close(copy);
    snag_vm_register_free(&reg);
    json_decref(a);
    free(large);
    (void)close(dir);
    assert(rmdir(root) == 0);
    (void)puts("Vim selection: PASS");
    return 0;
}

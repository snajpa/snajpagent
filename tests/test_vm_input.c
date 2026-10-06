/* SPDX-License-Identifier: GPL-2.0-only */
#include "vm_input.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct captured {
    struct snag_vm_input_event event;
    struct snag_buf bytes;
};

struct capture {
    struct captured items[64];
    size_t count;
};

static int
collect(void *opaque, const struct snag_vm_input_event *event)
{
    struct capture *capture = opaque;
    struct captured *item = capture->count ? &capture->items[capture->count - 1u] : NULL;
    if (!item || (event->kind != SNAG_VM_TEXT && event->kind != SNAG_VM_PASTE_TEXT) ||
        item->event.kind != event->kind || item->event.modifiers != event->modifiers) {
        assert(capture->count < sizeof(capture->items) / sizeof(capture->items[0]));
        item = &capture->items[capture->count++];
        item->event = *event;
        item->event.text = NULL;
        item->bytes.max = SIZE_MAX;
    }
    if (event->text) assert(snag_buf_append(&item->bytes, event->text, event->length) == 0);
    return 0;
}

static void
clear(struct capture *capture)
{
    for (size_t i = 0u; i < capture->count; ++i) snag_buf_free(&capture->items[i].bytes);
    *capture = (struct capture){0};
}

static void
same(const struct capture *a, const struct capture *b)
{
    assert(a->count == b->count);
    for (size_t i = 0u; i < a->count; ++i) {
        const struct snag_vm_input_event *left = &a->items[i].event;
        const struct snag_vm_input_event *right = &b->items[i].event;
        assert(left->kind == right->kind && left->key == right->key &&
            left->modifiers == right->modifiers && left->row == right->row &&
            left->column == right->column && left->button == right->button &&
            left->release == right->release && left->focused == right->focused);
        assert(a->items[i].bytes.len == b->items[i].bytes.len);
        if (a->items[i].bytes.len)
            assert(!memcmp(a->items[i].bytes.data, b->items[i].bytes.data, a->items[i].bytes.len));
    }
}

static void
bytes(const struct capture *capture, size_t at, enum snag_vm_input_kind kind, const char *value)
{
    assert(at < capture->count && capture->items[at].event.kind == kind);
    const struct snag_buf *buffer = &capture->items[at].bytes;
    assert(buffer->len == strlen(value) && !memcmp(buffer->data, value, buffer->len));
}

static void
key(const struct capture *capture, size_t at, unsigned int code, unsigned int modifiers)
{
    assert(at < capture->count && capture->items[at].event.kind == SNAG_VM_KEY);
    assert(capture->items[at].event.key == code &&
        capture->items[at].event.modifiers == modifiers);
}

static void
clipboard_replies(void)
{
    static const char reply[] = "\033[>9003;4294967295;4294967295;4294967295;4294967295;"
        "4294967295;11;4294967295;4294967295c";
    for (size_t split = 0u; split < sizeof(reply); ++split) {
        struct snag_vm_input input = {0};
        struct capture captured = {0};
        assert(snag_vm_input_feed(&input, reply, split, 10u, collect, &captured) == 0);
        assert(snag_vm_input_feed(&input, reply + split, sizeof(reply) - 1u - split,
            11u, collect, &captured) == 0);
        assert(captured.count == 1u);
        bytes(&captured, 0u, SNAG_VM_TERMINAL_REPLY, reply);
        clear(&captured);
    }
    struct snag_vm_input input = {0};
    struct capture captured = {0};
    assert(snag_vm_input_feed(&input, "\033[>123456789S", 13u, 10u, collect, &captured) == 0);
    bytes(&captured, 0u, SNAG_VM_TERMINAL_REPLY, "\033[>123456789S");
    assert(snag_vm_input_feed(&input, "\033[200~", 6u, 11u, collect, &captured) == 0);
    assert(snag_vm_input_feed(&input, reply, sizeof(reply) - 1u, 12u, collect, &captured) == 0);
    assert(snag_vm_input_feed(&input, "\033[201~", 6u, 13u, collect, &captured) == 0);
    assert(captured.count == 4u);
    bytes(&captured, 2u, SNAG_VM_PASTE_TEXT, reply);
    clear(&captured);
}

int
main(void)
{
    clipboard_replies();
    struct snag_vm_input input = {0};
    struct capture full = {0}, fragmented = {0};
    static const char script[] = "é界👩‍💻\r\n\t\177\033[A\033[1;5D\033OF\033[Z"
        "\033[200~:q\r\ngg\033[20x\033\033[201~\033[<0;80;24M\033[<0;80;24m"
        "\033[I\033[O\033b\033é";
    size_t length = sizeof(script) - 1u;
    assert(snag_vm_input_feed(&input, script, length, 1000u, collect, &full) == 0);
    assert(!input.paste && !input.sequence_length && !input.utf8_length);
    bytes(&full, 0u, SNAG_VM_TEXT, "é界👩‍💻");
    key(&full, 1u, SNAG_VM_KEY_ENTER, 0u);
    key(&full, 2u, 'j', SNAG_VM_CTRL);
    key(&full, 3u, SNAG_VM_KEY_TAB, 0u);
    key(&full, 4u, SNAG_VM_KEY_BACKSPACE, 0u);
    key(&full, 5u, SNAG_VM_KEY_UP, 0u);
    key(&full, 6u, SNAG_VM_KEY_LEFT, SNAG_VM_CTRL);
    key(&full, 7u, SNAG_VM_KEY_END, 0u);
    key(&full, 8u, SNAG_VM_KEY_TAB, SNAG_VM_SHIFT);
    assert(full.items[9].event.kind == SNAG_VM_PASTE_BEGIN);
    bytes(&full, 10u, SNAG_VM_PASTE_TEXT, ":q\r\ngg\033[20x\033");
    assert(full.items[11].event.kind == SNAG_VM_PASTE_END);
    assert(full.items[12].event.kind == SNAG_VM_MOUSE &&
        full.items[12].event.row == 23u && full.items[12].event.column == 79u &&
        !full.items[12].event.release);
    assert(full.items[13].event.kind == SNAG_VM_MOUSE && full.items[13].event.release);
    assert(full.items[14].event.kind == SNAG_VM_FOCUS && full.items[14].event.focused);
    assert(full.items[15].event.kind == SNAG_VM_FOCUS && !full.items[15].event.focused);
    bytes(&full, 16u, SNAG_VM_TEXT, "bé");
    assert(full.items[16].event.modifiers == SNAG_VM_ALT && full.count == 17u);
    for (size_t split = 0u; split <= length; ++split) {
        input = (struct snag_vm_input){0};
        assert(snag_vm_input_feed(&input, script, split, 1000u, collect, &fragmented) == 0);
        assert(snag_vm_input_feed(&input, script + split, length - split, 1001u,
            collect, &fragmented) == 0);
        same(&full, &fragmented);
        clear(&fragmented);
    }
    input = (struct snag_vm_input){0};
    for (size_t i = 0u; i < length; ++i)
        assert(snag_vm_input_feed(&input, script + i, 1u, 1000u + i / 10u,
            collect, &fragmented) == 0);
    same(&full, &fragmented);
    clear(&full);
    clear(&fragmented);
    input = (struct snag_vm_input){0};
    assert(snag_vm_input_feed(&input, "\033", 1u, 2000u, collect, &full) == 0);
    assert(snag_vm_input_wait_ms(&input, 2001u) == 34);
    assert(snag_vm_input_expire(&input, 2034u, collect, &full) == 0 && !full.count);
    assert(snag_vm_input_expire(&input, 2035u, collect, &full) == 0);
    key(&full, 0u, SNAG_VM_KEY_ESCAPE, 0u);
    assert(snag_vm_input_wait_ms(&input, 2035u) == -1);
    assert(snag_vm_input_feed(&input, "q", 1u, 2036u, collect, &full) == 0);
    bytes(&full, 1u, SNAG_VM_TEXT, "q");
    clear(&full);
    assert(snag_vm_input_feed(&input, "\033[", 2u, 3000u, collect, &full) == 0);
    assert(snag_vm_input_expire(&input, 4000u, collect, &full) == 0);
    key(&full, 0u, SNAG_VM_KEY_UNKNOWN, 0u);
    clear(&full);
    static const char malformed[] = "\xf0\x80\x80\x80" "x\xc3\r\xff"
        "\033[<0;0;1M\033[<0;9999999999999999999999;1M"
        "\033]52;c;:q\r\a";
    assert(snag_vm_input_feed(&input, malformed, sizeof(malformed) - 1u, 4001u,
        collect, &full) == 0);
    bytes(&full, 0u, SNAG_VM_TEXT, "�x�");
    key(&full, 1u, SNAG_VM_KEY_ENTER, 0u);
    bytes(&full, 2u, SNAG_VM_TEXT, "�");
    for (size_t i = 3u; i < 6u; ++i) key(&full, i, SNAG_VM_KEY_UNKNOWN, 0u);
    assert(full.count == 6u);
    clear(&full);
    char huge[1024];
    memset(huge, '1', sizeof(huge));
    huge[0] = '\033';
    huge[1] = '[';
    huge[sizeof(huge) - 2u] = 'q';
    huge[sizeof(huge) - 1u] = 'j';
    assert(snag_vm_input_feed(&input, huge, sizeof(huge), 5000u, collect, &full) == 0);
    key(&full, 0u, SNAG_VM_KEY_UNKNOWN, 0u);
    bytes(&full, 1u, SNAG_VM_TEXT, "j");
    clear(&full);
    assert(snag_vm_input_feed(&input, "\033[200~", 6u, 6000u, collect, &full) == 0);
    assert(snag_vm_input_expire(&input, 100000u, collect, &full) == 0 && input.paste);
    assert(snag_vm_input_feed(&input, "\003:q\r\033[201~", 10u, 100001u, collect, &full) == 0);
    assert(!input.paste && full.count == 3u);
    bytes(&full, 1u, SNAG_VM_PASTE_TEXT, "\003:q\r");
    clear(&full);
    puts("test_vm_input: ok");
    return 0;
}

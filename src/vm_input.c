/* SPDX-License-Identifier: GPL-2.0-only */
#include "vm_input.h"

#include <limits.h>
#include <string.h>

static int
key(snag_vm_input_emit emit, void *opaque, unsigned int code, unsigned int modifiers)
{
    struct snag_vm_input_event event = {.kind = SNAG_VM_KEY, .key = code,
        .modifiers = modifiers};
    return emit(opaque, &event);
}

static int
text(snag_vm_input_emit emit, void *opaque, enum snag_vm_input_kind kind,
    const unsigned char *bytes, size_t length, unsigned int modifiers)
{
    struct snag_vm_input_event event = {.kind = kind, .text = bytes, .length = length,
        .modifiers = modifiers};
    return length ? emit(opaque, &event) : 0;
}

static int
marker(snag_vm_input_emit emit, void *opaque, enum snag_vm_input_kind kind)
{
    struct snag_vm_input_event event = {.kind = kind};
    return emit(opaque, &event);
}

static int
replacement(snag_vm_input_emit emit, void *opaque, unsigned int modifiers)
{
    static const unsigned char value[] = {0xef, 0xbf, 0xbd};
    return text(emit, opaque, SNAG_VM_TEXT, value, sizeof(value), modifiers);
}

static int
ordinary(struct snag_vm_input *input, unsigned char byte, unsigned int modifiers,
    snag_vm_input_emit emit, void *opaque)
{
    if (input->utf8_length) {
        if ((byte & 0xc0u) == 0x80u) {
            input->utf8[input->utf8_length++] = byte;
            if (input->utf8_length < input->utf8_expected) return 0;
            size_t length = input->utf8_length;
            input->utf8_length = input->utf8_expected = 0u;
            return snag_utf8_valid(input->utf8, length, false) ?
                text(emit, opaque, SNAG_VM_TEXT, input->utf8, length, input->utf8_modifiers) :
                replacement(emit, opaque, input->utf8_modifiers);
        }
        input->utf8_length = input->utf8_expected = 0u;
        if (replacement(emit, opaque, input->utf8_modifiers)) return -1;
    }
    if (byte >= 0x80u) {
        if (byte < 0xc2u || byte > 0xf4u) return replacement(emit, opaque, modifiers);
        input->utf8[0] = byte;
        input->utf8_length = 1u;
        input->utf8_expected = byte < 0xe0u ? 2u : byte < 0xf0u ? 3u : 4u;
        input->utf8_modifiers = modifiers;
        return 0;
    }
    if (byte == '\r') return key(emit, opaque, SNAG_VM_KEY_ENTER, modifiers);
    if (byte == '\t') return key(emit, opaque, SNAG_VM_KEY_TAB, modifiers);
    if (byte == 0x7fu || byte == 0x08u)
        return key(emit, opaque, SNAG_VM_KEY_BACKSPACE, modifiers);
    if (byte == 0x1bu) return key(emit, opaque, SNAG_VM_KEY_ESCAPE, modifiers);
    if (byte < 0x20u)
        return key(emit, opaque, byte ? byte <= 26u ? 'a' + byte - 1u : '@' + byte : '@',
            modifiers | SNAG_VM_CTRL);
    return text(emit, opaque, SNAG_VM_TEXT, &byte, 1u, modifiers);
}

static unsigned int
final_key(unsigned char final)
{
    switch (final) {
    case 'A': return SNAG_VM_KEY_UP;
    case 'B': return SNAG_VM_KEY_DOWN;
    case 'C': return SNAG_VM_KEY_RIGHT;
    case 'D': return SNAG_VM_KEY_LEFT;
    case 'H': return SNAG_VM_KEY_HOME;
    case 'F': return SNAG_VM_KEY_END;
    case 'P': return SNAG_VM_KEY_F1;
    case 'Q': return SNAG_VM_KEY_F2;
    case 'R': return SNAG_VM_KEY_F3;
    case 'S': return SNAG_VM_KEY_F4;
    default: return SNAG_VM_KEY_UNKNOWN;
    }
}

static bool
parameters(const unsigned char *bytes, size_t length, unsigned int *values,
    size_t capacity, size_t *count)
{
    *count = 0u;
    if (!length) return true;
    size_t at = 0u;
    while (at < length && *count < capacity) {
        unsigned int value = 0u;
        size_t start = at;
        while (at < length && bytes[at] >= '0' && bytes[at] <= '9') {
            unsigned int digit = bytes[at++] - '0';
            if (value > (UINT_MAX - digit) / 10u) return false;
            value = value * 10u + digit;
        }
        if (at == start) return false;
        values[(*count)++] = value;
        if (at == length) return true;
        if (bytes[at++] != ';' || at == length) return false;
    }
    return false;
}

static int
sequence(struct snag_vm_input *input, snag_vm_input_emit emit, void *opaque)
{
    size_t length = input->sequence_length;
    input->sequence_length = 0u;
    const unsigned char *bytes = input->sequence;
    unsigned char final = bytes[length - 1u];
    if (input->discard) {
        input->discard = false;
        return key(emit, opaque, SNAG_VM_KEY_UNKNOWN, 0u);
    }
    if (bytes[1] == '[' && length >= 5u && bytes[2] == '>' &&
        ((final == 'S' && length <= 16u) ||
            (final == 'c' && length >= 10u && !memcmp(bytes + 3u, "9003;", 5u))))
        return text(emit, opaque, SNAG_VM_TERMINAL_REPLY, bytes, length, 0u);
    if (bytes[1] == 'O') return key(emit, opaque, final_key(final), 0u);
    if (length == 3u && (final == 'I' || final == 'O')) {
        struct snag_vm_input_event event = {.kind = SNAG_VM_FOCUS, .focused = final == 'I'};
        return emit(opaque, &event);
    }
    if (length == 3u && final == 'Z')
        return key(emit, opaque, SNAG_VM_KEY_TAB, SNAG_VM_SHIFT);
    if (length > 3u && bytes[2] == '<' && (final == 'M' || final == 'm')) {
        unsigned int values[3];
        size_t count;
        if (parameters(bytes + 3u, length - 4u, values, 3u, &count) && count == 3u &&
            values[1] && values[2]) {
            struct snag_vm_input_event event = {.kind = SNAG_VM_MOUSE,
                .button = values[0], .column = values[1] - 1u, .row = values[2] - 1u,
                .release = final == 'm', .modifiers = (values[0] >> 2u) & 7u};
            return emit(opaque, &event);
        }
        return key(emit, opaque, SNAG_VM_KEY_UNKNOWN, 0u);
    }
    unsigned int values[2] = {0};
    size_t count;
    if (!parameters(bytes + 2u, length - 3u, values, 2u, &count) ||
        (count == 2u && (!values[1] || values[1] > 8u)))
        return key(emit, opaque, SNAG_VM_KEY_UNKNOWN, 0u);
    unsigned int modifiers = count == 2u ? values[1] - 1u : 0u;
    if (final == '~') {
        unsigned int code = SNAG_VM_KEY_UNKNOWN;
        if (count == 1u && values[0] == 200u) {
            input->paste = true;
            return marker(emit, opaque, SNAG_VM_PASTE_BEGIN);
        }
        switch (values[0]) {
        case 1: case 7: code = SNAG_VM_KEY_HOME; break;
        case 2: code = SNAG_VM_KEY_INSERT; break;
        case 3: code = SNAG_VM_KEY_DELETE; break;
        case 4: case 8: code = SNAG_VM_KEY_END; break;
        case 5: code = SNAG_VM_KEY_PAGE_UP; break;
        case 6: code = SNAG_VM_KEY_PAGE_DOWN; break;
        default: break;
        }
        return key(emit, opaque, code, modifiers);
    }
    return key(emit, opaque, count && values[0] > 1u ? SNAG_VM_KEY_UNKNOWN :
        final_key(final), modifiers);
}

static int
paste(struct snag_vm_input *input, unsigned char byte, snag_vm_input_emit emit, void *opaque)
{
    static const unsigned char end[] = "\033[201~";
    if (byte == end[input->paste_match]) {
        if (++input->paste_match < sizeof(end) - 1u) return 0;
        input->paste_match = 0u;
        input->paste = false;
        return marker(emit, opaque, SNAG_VM_PASTE_END);
    }
    if (input->paste_match) {
        size_t matched = input->paste_match;
        input->paste_match = 0u;
        if (text(emit, opaque, SNAG_VM_PASTE_TEXT, end, matched, 0u)) return -1;
        if (byte == end[0]) {
            input->paste_match = 1u;
            return 0;
        }
    }
    return text(emit, opaque, SNAG_VM_PASTE_TEXT, &byte, 1u, 0u);
}

static int
escape(struct snag_vm_input *input, unsigned char byte, uint64_t now,
    snag_vm_input_emit emit, void *opaque)
{
    if (input->string) {
        if (byte == '\a' || (input->string_escape && byte == '\\')) {
            input->sequence_length = 0u;
            input->string = input->string_escape = false;
            return key(emit, opaque, SNAG_VM_KEY_UNKNOWN, 0u);
        }
        input->string_escape = byte == 0x1bu;
        return 0;
    }
    if (input->sequence_length == 1u) {
        if (byte == '[' || byte == 'O') {
            input->sequence[input->sequence_length++] = byte;
            return 0;
        }
        if (byte == ']' || byte == 'P' || byte == '_' || byte == '^' || byte == 'X') {
            input->string = true;
            input->sequence[input->sequence_length++] = byte;
            return 0;
        }
        input->sequence_length = 0u;
        if (byte == 0x1bu) {
            input->sequence_since = now;
            input->sequence_length = 1u;
            return key(emit, opaque, SNAG_VM_KEY_ESCAPE, 0u);
        }
        return ordinary(input, byte, SNAG_VM_ALT, emit, opaque);
    }
    bool final = byte >= 0x40u && byte <= 0x7eu;
    if (input->sequence_length < sizeof(input->sequence))
        input->sequence[input->sequence_length++] = byte;
    else
        input->discard = true;
    if (final) return sequence(input, emit, opaque);
    if (byte < 0x20u || byte > 0x3fu) input->discard = true;
    return 0;
}

int
snag_vm_input_feed(struct snag_vm_input *input, const void *data, size_t length, uint64_t now,
    snag_vm_input_emit emit, void *opaque)
{
    const unsigned char *bytes = data;
    int expired = snag_vm_input_expire(input, now, emit, opaque);
    if (expired) return expired;
    for (size_t i = 0u; i < length; ++i) {
        unsigned char byte = bytes[i];
        int rc;
        if (input->paste) {
            /* Deliver ordinary paste spans together; only its closing marker
             * needs bytewise matching across read boundaries. */
            size_t begin = i;
            if (!input->paste_match && byte != 0x1bu) {
                while (i + 1u < length && bytes[i + 1u] != 0x1bu) ++i;
                rc = text(emit, opaque, SNAG_VM_PASTE_TEXT, bytes + begin, i - begin + 1u, 0u);
            } else {
                rc = paste(input, byte, emit, opaque);
            }
        } else if (input->sequence_length) {
            rc = escape(input, byte, now, emit, opaque);
        } else if (byte == 0x1bu) {
            rc = 0;
            if (input->utf8_length) {
                input->utf8_length = input->utf8_expected = 0u;
                rc = replacement(emit, opaque, input->utf8_modifiers);
            }
            input->sequence[0] = byte;
            input->sequence_length = 1u;
            input->sequence_since = now;
        } else {
            rc = ordinary(input, byte, 0u, emit, opaque);
        }
        if (rc) return rc;
    }
    return 0;
}

int
snag_vm_input_wait_ms(const struct snag_vm_input *input, uint64_t now)
{
    if (!input->sequence_length) return -1;
    uint64_t elapsed = now >= input->sequence_since ? now - input->sequence_since : 0u;
    unsigned int delay = input->sequence_length == 1u ? 35u : 1000u;
    return elapsed >= delay ? 0 : (int)(delay - elapsed);
}

int
snag_vm_input_expire(struct snag_vm_input *input, uint64_t now,
    snag_vm_input_emit emit, void *opaque)
{
    if (snag_vm_input_wait_ms(input, now) != 0) return 0;
    unsigned int code = input->sequence_length == 1u ? SNAG_VM_KEY_ESCAPE : SNAG_VM_KEY_UNKNOWN;
    input->sequence_length = 0u;
    input->discard = input->string = input->string_escape = false;
    return key(emit, opaque, code, 0u);
}

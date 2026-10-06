/* SPDX-License-Identifier: GPL-2.0-only */
#include "unicode.h"
#include "base.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>

enum grapheme_property {
    G_OTHER, G_CR, G_LF, G_CONTROL, G_EXTEND, G_ZWJ, G_RI,
    G_PREPEND, G_SPACING, G_L, G_V, G_T, G_LV, G_LVT
};

enum indic_property {I_NONE, I_CONSONANT, I_EXTEND, I_LINKER};

#define PROPERTY_GCB 0x0fu
#define PROPERTY_EP 0x40u
#define PROPERTY_EMOJI 0x80u
#define PROPERTY_WIDE 0x100u
#define PROPERTY_AMBIGUOUS 0x200u
#define PROPERTY_ZERO 0x400u
#define PROPERTY_WORD 0x800u
#define PROPERTY_SPACE 0x1000u

struct unicode_range {
    uint32_t first, last;
    uint16_t properties;
};

#include "unicode_tables.inc"

static unsigned int
properties(uint32_t cp)
{
    size_t low = 0u, high = sizeof(unicode_ranges) / sizeof(unicode_ranges[0]);
    unsigned int value = 0u;
    while (low < high) {
        size_t middle = low + (high - low) / 2u;
        const struct unicode_range *range = unicode_ranges + middle;
        if (cp < range->first) high = middle;
        else if (cp > range->last) low = middle + 1u;
        else { value = range->properties; break; }
    }
    if (cp >= 0xac00u && cp <= 0xd7a3u) {
        value = (value & ~PROPERTY_GCB) | ((cp - 0xac00u) % 28u ? G_LVT : G_LV);
    }
    return value;
}

static bool
control(unsigned int value)
{
    return value == G_CR || value == G_LF || value == G_CONTROL;
}

unsigned int
snag_unicode_word_class(uint32_t cp)
{
    unsigned int value = properties(cp);
    if ((value & PROPERTY_SPACE) || (cp >= 9u && cp <= 13u) || cp == 0x85u) return 0u;
    return cp == '_' || (value & PROPERTY_WORD) ? 1u : 2u;
}

struct boundary_state {
    unsigned int previous;
    bool regional_odd, pictographic_extend, pictographic_zwj, consonant, linker;
};

static void
advance(struct boundary_state *state, unsigned int value)
{
    unsigned int current = value & PROPERTY_GCB, indic = (value >> 4u) & 3u;
    state->regional_odd = current == G_RI && !state->regional_odd;
    state->pictographic_zwj = current == G_ZWJ && state->pictographic_extend;
    if (value & PROPERTY_EP) state->pictographic_extend = true;
    else if (current != G_EXTEND) state->pictographic_extend = false;
    if (indic == I_CONSONANT) {
        state->consonant = true;
        state->linker = false;
    } else if (indic == I_LINKER && state->consonant) {
        state->linker = true;
    } else if (indic != I_EXTEND) {
        state->consonant = state->linker = false;
    }
    state->previous = current;
}

static bool
joined(const struct boundary_state *state, unsigned int value)
{
    unsigned int a = state->previous, b = value & PROPERTY_GCB;
    if (a == G_CR && b == G_LF) return true; /* GB3 */
    if (control(a) || control(b)) return false; /* GB4, GB5 */
    if (a == G_L && (b == G_L || b == G_V || b == G_LV || b == G_LVT)) return true;
    if ((a == G_LV || a == G_V) && (b == G_V || b == G_T)) return true;
    if ((a == G_LVT || a == G_T) && b == G_T) return true; /* GB6..GB8 */
    if (b == G_EXTEND || b == G_ZWJ || b == G_SPACING || a == G_PREPEND) return true;
    if (((value >> 4u) & 3u) == I_CONSONANT && state->consonant && state->linker) return true;
    if ((value & PROPERTY_EP) && state->pictographic_zwj) return true; /* GB11 */
    return a == G_RI && b == G_RI && state->regional_odd; /* GB12, GB13 */
}

size_t
snag_grapheme_next(const unsigned char *text, size_t length)
{
    uint32_t cp;
    if (!length) return 0u;
    size_t position = snag_utf8_decode(text, length, &cp);
    if (!position) { errno = EILSEQ; return 0u; }
    struct boundary_state state = {0};
    advance(&state, properties(cp));
    while (position < length) {
        size_t bytes = snag_utf8_decode(text + position, length - position, &cp);
        if (!bytes) break;
        unsigned int value = properties(cp);
        if (!joined(&state, value)) break;
        advance(&state, value);
        position += bytes;
    }
    return position;
}

int
snag_grapheme_width(const unsigned char *text, size_t length, bool ambiguous_wide)
{
    size_t position = 0u;
    int width = 0;
    bool emoji = false, pictographic = false, zwj = false, text_style = false, emoji_style = false;
    bool keycap_base = false, keycap = false, regional = false;
    while (position < length) {
        uint32_t cp;
        size_t bytes = snag_utf8_decode(text + position, length - position, &cp);
        if (!bytes) return snag_errno(EILSEQ);
        unsigned int value = properties(cp), gcb = value & PROPERTY_GCB;
        if (control(gcb)) return snag_errno(EINVAL);
        if (!position) keycap_base = cp == '#' || cp == '*' || (cp >= '0' && cp <= '9');
        if (cp == 0x20e3u) keycap = true;
        if (cp == 0xfe0eu) text_style = true;
        if (cp == 0xfe0fu) emoji_style = true;
        emoji = emoji || (value & PROPERTY_EMOJI);
        pictographic = pictographic || (value & PROPERTY_EP);
        regional = regional || gcb == G_RI;
        zwj = zwj || gcb == G_ZWJ;
        if (!(value & PROPERTY_ZERO) && gcb != G_EXTEND && gcb != G_ZWJ &&
            gcb != G_V && gcb != G_T) {
            int cells = (value & PROPERTY_WIDE) ||
                (ambiguous_wide && (value & PROPERTY_AMBIGUOUS)) ? 2 : 1;
            if (width > INT_MAX - cells) return snag_errno(EOVERFLOW);
            width += cells;
        }
        position += bytes;
    }
    if (regional || (keycap_base && keycap) ||
        (!text_style && (emoji || (pictographic && (emoji_style || zwj))))) return 2;
    return width;
}

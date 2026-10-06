/* SPDX-License-Identifier: GPL-2.0-only */
#include "unicode.h"
#include "base.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
append_codepoint(struct snag_buf *buffer, uint32_t cp)
{
    unsigned char bytes[4];
    size_t count;
    if (cp < 0x80u) { bytes[0] = (unsigned char)cp; count = 1u; }
    else if (cp < 0x800u) {
        bytes[0] = 0xc0u | (cp >> 6u);
        bytes[1] = 0x80u | (cp & 0x3fu);
        count = 2u;
    } else if (cp < 0x10000u) {
        bytes[0] = 0xe0u | (cp >> 12u);
        bytes[1] = 0x80u | ((cp >> 6u) & 0x3fu);
        bytes[2] = 0x80u | (cp & 0x3fu);
        count = 3u;
    } else {
        bytes[0] = 0xf0u | (cp >> 18u);
        bytes[1] = 0x80u | ((cp >> 12u) & 0x3fu);
        bytes[2] = 0x80u | ((cp >> 6u) & 0x3fu);
        bytes[3] = 0x80u | (cp & 0x3fu);
        count = 4u;
    }
    assert(snag_buf_append(buffer, bytes, count) == 0);
}

static void
official_boundaries(void)
{
    FILE *file = fopen("tests/fixtures/unicode-17.0.0/GraphemeBreakTest.txt", "r");
    assert(file);
    char line[8192];
    unsigned int count = 0u, line_number = 0u;
    while (fgets(line, sizeof(line), file)) {
        ++line_number;
        assert(strchr(line, '\n') || feof(file));
        char *comment = strchr(line, '#');
        if (comment) *comment = '\0';
        struct snag_buf text = {.max = SIZE_MAX};
        size_t boundaries[4096], boundary_count = 0u;
        for (char *token = strtok(line, " \t\r\n"); token; token = strtok(NULL, " \t\r\n")) {
            if (!strcmp(token, "÷")) {
                assert(boundary_count < sizeof(boundaries) / sizeof(boundaries[0]));
                boundaries[boundary_count++] = text.len;
            } else if (strcmp(token, "×")) {
                char *end;
                unsigned long cp = strtoul(token, &end, 16);
                assert(!*end && cp <= 0x10ffffu);
                append_codepoint(&text, (uint32_t)cp);
            }
        }
        if (boundary_count) {
            ++count;
            assert(boundaries[0] == 0u && boundaries[boundary_count - 1u] == text.len);
            struct snag_grapheme_state state = {0};
            size_t boundary = 0u;
            for (size_t at = 0u; at < text.len;) {
                uint32_t cp;
                size_t bytes = snag_utf8_decode(text.data + at, text.len - at, &cp);
                assert(bytes);
                bool start = snag_grapheme_feed(&state, cp);
                if (start) assert(at == boundaries[boundary++]);
                else assert(at != boundaries[boundary]);
                at += bytes;
            }
            assert(boundary + 1u == boundary_count);
            size_t position = 0u;
            for (size_t i = 1u; i < boundary_count; ++i) {
                size_t bytes = snag_grapheme_next(text.data + position, text.len - position);
                if (!bytes || position + bytes != boundaries[i]) {
                    fprintf(stderr, "Unicode boundary mismatch at fixture line %u: %zu != %zu\n",
                        line_number, position + bytes, boundaries[i]);
                    abort();
                }
                position += bytes;
            }
        }
        snag_buf_free(&text);
    }
    assert(!ferror(file) && fclose(file) == 0 && count > 700u);
    printf("Unicode 17 official boundary cases: %u\n", count);
}

static int
width(const char *text, bool ambiguous)
{
    return snag_grapheme_width((const unsigned char *)text, strlen(text), ambiguous);
}

int
main(void)
{
    official_boundaries();
    uint32_t folded[3];
    assert(snag_unicode_casefold('A', folded) == 1u && folded[0] == 'a');
    assert(snag_unicode_casefold(0xdfu, folded) == 2u && folded[0] == 's' && folded[1] == 's');
    assert(snag_unicode_casefold(0x3c2u, folded) == 1u && folded[0] == 0x3c3u);
    assert(snag_unicode_casefold(0x130u, folded) == 2u && folded[0] == 'i' && folded[1] == 0x307u);
    assert(snag_unicode_casefold(0xfb03u, folded) == 3u && folded[2] == 'i');
    assert(snag_unicode_casefold(0x4e2du, folded) == 1u && folded[0] == 0x4e2du);
    assert(snag_unicode_word_class('A') == 1u && snag_unicode_word_class('_') == 1u);
    assert(snag_unicode_word_class(0x03b2u) == 1u && snag_unicode_word_class(0x4e2du) == 1u);
    assert(snag_unicode_word_class('!') == 2u && snag_unicode_word_class(0x1f469u) == 2u);
    assert(snag_unicode_word_class('\t') == 0u && snag_unicode_word_class(0x2003u) == 0u);
    assert(width("é", false) == 1);
    assert(width("́", false) == 0);
    assert(width("界", false) == 2);
    assert(width("·", false) == 1 && width("·", true) == 2);
    assert(width("👩‍💻", false) == 2);
    assert(width("🇨🇿", false) == 2);
    assert(width("1️⃣", false) == 2);
    assert(width("☀︎", false) == 1 && width("☀️", false) == 2);
    assert(width("\033", false) < 0);
    const unsigned char invalid[] = {'x', 0xf0u, 0x9fu};
    assert(snag_grapheme_next(invalid, sizeof(invalid)) == 1u);
    assert(!snag_grapheme_next(invalid + 1u, 2u) && errno == EILSEQ);
    assert(!snag_grapheme_next(invalid, 0u));
    puts("test_unicode: ok");
    return 0;
}

/* SPDX-License-Identifier: GPL-2.0-only */
#include "base.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
test_path_join(void)
{
    const char *left[] = {"/", "/work", "/work/", ""};
    const char *expected[] = {"/file", "/work/file", "/work//file", "/file"};
    for (size_t i = 0u; i < sizeof(left) / sizeof(left[0]); ++i) {
        char *path = snag_path_join(left[i], "file");
        assert(path && strcmp(path, expected[i]) == 0);
        free(path);
    }
    char limit[SNAG_PATH_MAX_BYTES];
    memset(limit, 'x', sizeof(limit));
    limit[sizeof(limit) - 2u] = '\0';
    char *path = snag_path_join(limit, "x");
    assert(path && strlen(path) == SNAG_PATH_MAX_BYTES);
    free(path);
    limit[sizeof(limit) - 2u] = 'x';
    limit[sizeof(limit) - 1u] = '\0';
    assert(!snag_path_join(limit, "x") && errno == EOVERFLOW);
}

static void
test_irc_target_parse(void)
{
    static const struct {
        const char *text, *body;
        enum snag_irc_target_command command;
        uint32_t id;
    } cases[] = {
        {"hello", "", SNAG_IRC_TARGET_NONE, 0u},
        {"/", "", SNAG_IRC_TARGET_NONE, 0u},
        {"//2 hi", "", SNAG_IRC_TARGET_NONE, 0u},
        {"/topic x", "", SNAG_IRC_TARGET_NONE, 0u},
        {"/alligator", "", SNAG_IRC_TARGET_NONE, 0u},
        {"/1", "", SNAG_IRC_TARGET_SELECT, 1u},
        {"/2 \t", "", SNAG_IRC_TARGET_SELECT, 2u},
        {"/17 hi", "hi", SNAG_IRC_TARGET_SEND, 17u},
        {"/02 café\nnext", "café\nnext", SNAG_IRC_TARGET_SEND, 2u},
        {"/2 /all literal", "/all literal", SNAG_IRC_TARGET_SEND, 2u},
        {"/all hi", "hi", SNAG_IRC_TARGET_ALL, 0u},
        {"/all\nhi", "hi", SNAG_IRC_TARGET_ALL, 0u},
        {"/4294967295 hi", "hi", SNAG_IRC_TARGET_SEND, UINT32_MAX},
        {"/0", "", SNAG_IRC_TARGET_INVALID, 0u},
        {"/2oops", "", SNAG_IRC_TARGET_INVALID, 0u},
        {"/4294967296", "", SNAG_IRC_TARGET_INVALID, 0u},
        {"/999999999999999999999", "", SNAG_IRC_TARGET_INVALID, 0u},
        {"/all", "", SNAG_IRC_TARGET_INVALID, 0u},
        {"/all \t", "", SNAG_IRC_TARGET_INVALID, 0u}
    };

    for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        size_t body;
        uint32_t id;
        enum snag_irc_target_command command = snag_irc_target_parse(
            cases[i].text, strlen(cases[i].text), &id, &body);
        assert(command == cases[i].command);
        if (command > SNAG_IRC_TARGET_NONE) {
            assert(id == cases[i].id);
            assert(strcmp(cases[i].text + body, cases[i].body) == 0);
        }
    }
}

int
main(void)
{
    char digest[SNAG_SHA256_HEX_LEN + 1u];
    char id[SNAG_ID_HEX_LEN + 1u];
    struct snag_buf buf;
    static const unsigned char valid[] = "A\xe2\x82\xac\xf0\x9f\x98\x80";
    static const unsigned char invalid[] = {0xc0u, 0x80u};

    snag_sha256_hex("", 0u, digest);
    assert(strcmp(digest,
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0);
    snag_sha256_hex("abc", 3u, digest);
    assert(strcmp(digest,
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);
    assert(snag_utf8_valid(valid, sizeof(valid) - 1u, true));
    assert(!snag_utf8_valid(invalid, sizeof(invalid), true));
    assert(!snag_utf8_valid((const unsigned char *)"a\0b", 3u, true));
    {
        uint32_t cp = 42u;
        const unsigned char *text = (const unsigned char *)"\xf4\x8f\xbf\xbf";
        assert(snag_utf8_decode(NULL, 0u, &cp) == 0u && cp == 42u);
        assert(snag_utf8_decode(text, 4u, &cp) == 4u && cp == 0x10ffffu);
        assert(snag_utf8_decode(text, 3u, &cp) == 0u);
        assert(!snag_utf8_valid((const unsigned char *)"\xf4\x90\x80\x80", 4u, true));
        assert(!snag_utf8_valid((const unsigned char *)"\xed\xa0\x80", 3u, true));
        assert(!snag_utf8_valid((const unsigned char *)"\xe0\x80\x80", 3u, true));
        assert(!snag_utf8_valid((const unsigned char *)"\xf0\x80\x80\x80", 4u, true));
        assert(snag_utf8_decode((const unsigned char *)"", 1u, &cp) == 1u && cp == 0u);
        assert(snag_utf8_valid((const unsigned char *)"a\0b", 3u, false));
    }
    assert(snag_random_id(id) == 0);
    assert(snag_hex_is_lower(id, SNAG_ID_HEX_LEN));

    snag_buf_init(&buf, 4u);
    assert(snag_buf_append(&buf, "abcd", 4u) == 0);
    errno = 0;
    assert(snag_buf_putc(&buf, 'e') < 0 && errno == EOVERFLOW);
    snag_buf_free(&buf);
    test_irc_target_parse();
    test_path_join();
    puts("test_base: ok");
    return 0;
}

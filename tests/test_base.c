/* SPDX-License-Identifier: GPL-2.0-only */
#include "base.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

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
    puts("test_base: ok");
    return 0;
}

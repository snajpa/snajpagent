/* SPDX-License-Identifier: GPL-2.0-only */
#include "base.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

int
main(void)
{
    char digest[SNJ_SHA256_HEX_LEN + 1u];
    char id[SNJ_ID_HEX_LEN + 1u];
    struct snj_buf buf;
    static const unsigned char valid[] = "A\xe2\x82\xac\xf0\x9f\x98\x80";
    static const unsigned char invalid[] = {0xc0u, 0x80u};

    snj_sha256_hex("", 0u, digest);
    assert(strcmp(digest,
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0);
    snj_sha256_hex("abc", 3u, digest);
    assert(strcmp(digest,
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);
    assert(snj_utf8_valid(valid, sizeof(valid) - 1u, true));
    assert(!snj_utf8_valid(invalid, sizeof(invalid), true));
    assert(!snj_utf8_valid((const unsigned char *)"a\0b", 3u, true));
    assert(snj_random_id(id) == 0);
    assert(snj_hex_is_lower(id, SNJ_ID_HEX_LEN));

    snj_buf_init(&buf, 4u);
    assert(snj_buf_append(&buf, "abcd", 4u) == 0);
    errno = 0;
    assert(snj_buf_putc(&buf, 'e') < 0 && errno == EOVERFLOW);
    snj_buf_free(&buf);
    puts("test_base: ok");
    return 0;
}

/* SPDX-License-Identifier: GPL-2.0-only */
#include "base.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

static void
test_platform(void)
{
    unsigned char random[32], again[32];
    uint64_t before = snag_monotonic_ms();
    uint64_t wall = snag_time_ms();
    time_t seconds = time(NULL);
    FILE *file = tmpfile();
    char content[4] = {0};
    int fd;

    assert(seconds > 0 && wall / 1000u <= (uint64_t)seconds);
    assert(wall / 1000u + 1u >= (uint64_t)seconds);
    assert(snag_random_bytes(NULL, 0u) == 0);
    assert(snag_random_bytes(random, sizeof(random)) == 0);
    assert(snag_random_bytes(again, sizeof(again)) == 0);
    assert(memcmp(random, again, sizeof(random)) != 0);
    assert(snag_monotonic_ms() >= before);
    assert(snag_char_width('A') == 1);
    assert(snag_char_width('\n') == -1);
    assert(snag_char_width(0u) == 0);
    assert(snag_char_width(0xd800u) == -1);
    assert(snag_char_width(0x110000u) == -1);
#ifdef _WIN32
    assert(snag_char_width(0x0301u) == 0);
    assert(snag_char_width(0x4e2du) == 2);
    assert(snag_char_width(0x1f600u) == 2);
#endif
    assert(file);
    fd = fileno(file);
    assert(snag_fd_cloexec(fd) == 0);
#ifdef _WIN32
    DWORD flags;
    assert(GetHandleInformation((HANDLE)_get_osfhandle(fd), &flags));
    assert(!(flags & HANDLE_FLAG_INHERIT));
#else
    assert(fcntl(fd, F_GETFD) & FD_CLOEXEC);
#endif
    assert(snag_write_full(fd, NULL, 0u) == 0);
    assert(snag_write_full(fd, "abc", 3u) == 0);
    assert(snag_sync_file(fd) == 0);
    rewind(file);
    assert(fread(content, 1u, 3u, file) == 3u);
    assert(strcmp(content, "abc") == 0);
    assert(fclose(file) == 0);
    errno = 0;
    assert(snag_fd_cloexec(-1) == -1 && errno == EBADF);
    errno = 0;
    assert(snag_write_full(-1, "x", 1u) == -1 && errno == EBADF);
    errno = 0;
    assert(snag_sync_file(-1) == -1 && errno == EBADF);
    errno = 0;
    assert(snag_sync_dir(-1) == -1 && errno == EBADF);
}

static void
test_path_join(void)
{
    assert(snag_path_root_len(NULL) == 0u);
    assert(snag_path_root_len("") == 0u);
    assert(snag_path_root_len("relative/file") == 0u);
#ifdef _WIN32
    assert(snag_path_root_len("C:/") == 3u);
    assert(snag_path_root_len("z:\\directory\\file") == 3u);
    assert(snag_path_root_len("C:") == 0u);
    assert(snag_path_root_len("C:relative") == 0u);
    assert(snag_path_root_len("\\rooted") == 0u);
    assert(snag_path_root_len("/rooted") == 0u);
    assert(snag_path_root_len("//server/share") == 14u);
    assert(snag_path_root_len("//server/share/file") == 15u);
    assert(snag_path_root_len("\\\\server\\share\\file") == 15u);
    assert(snag_path_root_len("//server") == 0u);
    assert(snag_path_root_len("//server/") == 0u);
    assert(snag_path_root_len("//server//share") == 0u);
    assert(snag_path_root_len("///server/share") == 0u);
    assert(snag_path_root_len("\\\\?\\C:\\file") == 0u);
    assert(snag_path_root_len("\\\\.\\pipe\\name") == 0u);
#else
    assert(snag_path_root_len("/") == 1u);
    assert(snag_path_root_len("/directory/file") == 1u);
    assert(snag_path_root_len("//directory/file") == 1u);
    assert(snag_path_root_len("C:/file") == 0u);
#endif
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
    test_platform();
    puts("test_base: ok");
    return 0;
}

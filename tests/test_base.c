/* SPDX-License-Identifier: GPL-2.0-only */
#include "base.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <aclapi.h>
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifdef _WIN32
static void
test_windows_privacy(void)
{
    HANDLE token, handle;
    TOKEN_USER *user;
    DWORD size = 0;
    wchar_t temp[MAX_PATH], path[MAX_PATH], directory[MAX_PATH + 5u];
    DWORD world[SECURITY_MAX_SID_SIZE / sizeof(DWORD) + 1u];
    SID_IDENTIFIER_AUTHORITY authority = SECURITY_WORLD_SID_AUTHORITY;
    EXPLICIT_ACCESSW entries[2] = {0};
    struct snag_file_privacy privacy;
    const DWORD rights[] = {0u, FILE_GENERIC_READ, FILE_GENERIC_WRITE,
        READ_CONTROL | SYNCHRONIZE | FILE_READ_ATTRIBUTES, FILE_GENERIC_READ};
    const bool expected[] = {true, false, false, true, true};
    DWORD count = GetTempPathW(MAX_PATH, temp);
    int fd;

    assert(count && count < MAX_PATH);
    assert(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token));
    assert(!GetTokenInformation(token, TokenUser, NULL, 0, &size) && size);
    user = malloc(size);
    assert(user && GetTokenInformation(token, TokenUser, user, size, &size));
    assert(CloseHandle(token));
    assert(InitializeSid(world, &authority, 1u));
    *GetSidSubAuthority(world, 0u) = SECURITY_WORLD_RID;
    entries[0].grfAccessPermissions = FILE_ALL_ACCESS;
    entries[0].grfAccessMode = SET_ACCESS;
    entries[0].Trustee.TrusteeForm = TRUSTEE_IS_SID;
    entries[0].Trustee.ptstrName = (LPWSTR)user->User.Sid;
    entries[1].grfAccessMode = SET_ACCESS;
    entries[1].Trustee.TrusteeForm = TRUSTEE_IS_SID;
    entries[1].Trustee.ptstrName = (LPWSTR)world;
    assert(GetTempFileNameW(temp, L"snp", 0, path));
    handle = CreateFileW(path, GENERIC_READ | GENERIC_WRITE | WRITE_DAC | WRITE_OWNER,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    assert(handle != INVALID_HANDLE_VALUE);
    fd = _open_osfhandle((intptr_t)handle, _O_BINARY | _O_RDWR);
    assert(fd >= 0);
    for (size_t i = 0; i < sizeof(rights) / sizeof(rights[0]); ++i) {
        PACL acl = NULL;
        entries[1].grfAccessPermissions = rights[i];
        entries[1].grfInheritance = i == 4u ?
            SUB_CONTAINERS_AND_OBJECTS_INHERIT | INHERIT_ONLY : NO_INHERITANCE;
        assert(SetEntriesInAclW(rights[i] ? 2u : 1u, entries, NULL, &acl) == ERROR_SUCCESS);
        assert(SetSecurityInfo(handle, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION |
                               DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                               user->User.Sid, NULL, acl, NULL) == ERROR_SUCCESS);
        LocalFree(acl);
        assert(snag_fd_privacy(fd, &privacy) == 0);
        assert(privacy.real_owner && privacy.effective_owner);
        assert(privacy.private_access == expected[i]);
    }
    assert(SetSecurityInfo(handle, SE_FILE_OBJECT,
                           DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                           NULL, NULL, NULL, NULL) == ERROR_SUCCESS);
    assert(SetFileAttributesW(path, FILE_ATTRIBUTE_READONLY));
    assert(snag_fd_privacy(fd, &privacy) == 0 && !privacy.private_access);
    assert(SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL));
    assert(_close(fd) == 0);
    assert(DeleteFileW(path));

    assert(swprintf(directory, sizeof(directory) / sizeof(directory[0]), L"%ls-dir", path) > 0);
    assert(CreateDirectoryW(directory, NULL));
    handle = CreateFileW(directory, READ_CONTROL | WRITE_DAC | WRITE_OWNER,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    assert(handle != INVALID_HANDLE_VALUE);
    PACL acl = NULL;
    assert(SetEntriesInAclW(1u, entries, NULL, &acl) == ERROR_SUCCESS);
    assert(SetSecurityInfo(handle, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION |
                           DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                           user->User.Sid, NULL, acl, NULL) == ERROR_SUCCESS);
    LocalFree(acl);
    fd = _open_osfhandle((intptr_t)handle, _O_BINARY | _O_RDONLY);
    assert(fd >= 0);
    assert(snag_fd_privacy(fd, &privacy) == 0 && privacy.private_access && privacy.effective_owner);
    assert(_close(fd) == 0);
    assert(RemoveDirectoryW(directory));
    free(user);
}
#endif

static void
test_private_directory(void)
{
    char *cwd = snag_realpath("."), *after, *root, *child, *renamed;
    char *nested, *absolute;
    struct snag_file_privacy privacy;
    int fd;
#ifdef _WIN32
    wchar_t temp[MAX_PATH], wide[MAX_PATH];
    char path[MAX_PATH * 4u];
    DWORD count = GetTempPathW(MAX_PATH, temp);
    assert(count && count < MAX_PATH);
    assert(GetTempFileNameW(temp, L"snp", 0, wide));
    assert(DeleteFileW(wide));
    assert(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1,
                               path, sizeof(path), NULL, NULL));
    assert(snag_mkdir_private(path) == 0);
    root = snag_realpath(path);
    HANDLE handle = CreateFileW(wide, FILE_READ_ATTRIBUTES | READ_CONTROL,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, NULL);
    assert(handle != INVALID_HANDLE_VALUE);
    fd = _open_osfhandle((intptr_t)handle, _O_RDONLY | _O_BINARY);
#else
    const char *temp = getenv("TMPDIR");
    char *pattern = snag_path_join(temp ? temp : "/tmp", "snag-private-dir-XXXXXX");
    assert(pattern && mkdtemp(pattern));
    root = snag_realpath(pattern);
    free(pattern);
    fd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
#endif
    assert(cwd && root && fd >= 0);
    assert(snag_fd_privacy(fd, &privacy) == 0 && privacy.effective_owner && privacy.private_access);
    {
        const char bytes[] = {'a', '\n', '\0', '\x1a', 'z'};
        char received[sizeof(bytes)];
        char *data = snag_path_join(root, "data"), *alias = snag_path_join(root, "alias");
        int file = snag_create_private_at(fd, "data", true);
        assert(data && alias && file >= 0);
        assert(snag_fd_privacy(file, &privacy) == 0 && privacy.effective_owner && privacy.private_access);
        assert(snag_write_full(file, bytes, sizeof(bytes)) == 0);
        assert(snag_sync_file(file) == 0 && close(file) == 0);
        errno = 0;
        assert(snag_create_private_at(fd, "data", true) == -1 && errno == EEXIST);
        file = snag_create_private_at(fd, "data", false);
        assert(file >= 0);
        assert(read(file, received, sizeof(received)) == sizeof(received));
        assert(memcmp(bytes, received, sizeof(bytes)) == 0);
        assert(close(file) == 0);
#ifdef _WIN32
        assert(CreateHardLinkA(alias, data, NULL));
#else
        assert(link(data, alias) == 0);
#endif
        errno = 0;
        int rejected = snag_create_private_at(fd, "data", false);
        if (rejected != -1 || errno != EACCES) {
            int error = errno;
            (void)fprintf(stderr, "hardlink rejection: fd=%d errno=%d\n", rejected, error);
#ifdef _WIN32
            HANDLE inspect = CreateFileA(data, FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
            BY_HANDLE_FILE_INFORMATION info;
            if (inspect != INVALID_HANDLE_VALUE) {
                if (GetFileInformationByHandle(inspect, &info))
                    (void)fprintf(stderr, "file links=%lu attributes=%lu\n",
                                  info.nNumberOfLinks, info.dwFileAttributes);
                (void)CloseHandle(inspect);
            }
#endif
            errno = error;
        }
        assert(rejected == -1 && errno == EACCES);
        assert(unlink(alias) == 0);
#ifdef _WIN32
        assert(SetNamedSecurityInfoA(data, SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
            NULL, NULL, NULL, NULL) == ERROR_SUCCESS);
#else
        assert(chmod(data, 0644) == 0);
#endif
        errno = 0;
        assert(snag_create_private_at(fd, "data", false) == -1 && errno == EACCES);
        assert(unlink(data) == 0);
        free(data);
        free(alias);
    }
    child = snag_path_join(root, "child");
    renamed = snag_path_join(root, "renamed");
    absolute = snag_path_join(root, "absolute");
    assert(child && renamed && absolute);
    assert(snag_mkdir_private_at(fd, "child") == 0);
    errno = 0;
    assert(snag_mkdir_private_at(fd, "child") == -1 && errno == EEXIST);
    errno = 0;
    assert(snag_mkdir_private_at(-1, "relative") == -1 && errno == EBADF);
    assert(snag_mkdir_private_at(-1, absolute) == 0);
    assert(rename(child, renamed) == 0);
    assert(rmdir(renamed) == 0);
    assert(rmdir(absolute) == 0);
    free(child);
    free(renamed);
    free(absolute);
    renamed = malloc(strlen(root) + 9u);
    assert(renamed);
    (void)sprintf(renamed, "%s-renamed", root);
    assert(rename(root, renamed) == 0);
    assert(snag_mkdir_private_at(fd, "after-rename") == 0);
    nested = snag_path_join(renamed, "after-rename");
    assert(nested && rmdir(nested) == 0);
    free(nested);
    assert(close(fd) == 0);
    assert(rmdir(renamed) == 0);
    after = snag_realpath(".");
    assert(after && strcmp(cwd, after) == 0);
    free(after);
    free(cwd);
    free(root);
    free(renamed);
}

static void
test_realpath(void)
{
    char *path = snag_realpath(".");
    char *again;

    assert(path && snag_path_root_len(path));
#ifdef _WIN32
    assert(!strchr(path, '\\'));
#endif
    again = snag_realpath(path);
    assert(again && strcmp(path, again) == 0);
    free(again);
    free(path);
    errno = 0;
    assert(!snag_realpath(NULL) && errno == EINVAL);
    errno = 0;
    assert(!snag_realpath("") && errno == ENOENT);
#ifdef _WIN32
    wchar_t temp[MAX_PATH], source[MAX_PATH], target[MAX_PATH + 16];
    char input[MAX_PATH * 4u + 64u];
    DWORD count = GetTempPathW(MAX_PATH, temp);
    assert(count && count < MAX_PATH);
    assert(GetTempFileNameW(temp, L"snp", 0, source));
    assert(swprintf(target, sizeof(target) / sizeof(target[0]),
                    L"%ls-\u03b1-\U0001f600", source) > 0);
    assert(MoveFileW(source, target));
    assert(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, target, -1,
                               input, sizeof(input), NULL, NULL));
    path = snag_realpath(input);
    assert(path && strstr(path, "-\xce\xb1-\xf0\x9f\x98\x80"));
    assert(snag_utf8_valid((const unsigned char *)path, strlen(path), true));
    assert(DeleteFileW(target));
    free(path);
    errno = 0;
    assert(!snag_realpath(input) && errno == ENOENT);
#endif
}

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
    struct snag_file_privacy privacy;
#ifndef _WIN32
    assert(snag_fd_privacy(fd, &privacy) == 0);
    assert(privacy.effective_owner && privacy.private_access);
    assert(privacy.real_owner == (getuid() == geteuid()));
    assert(fchmod(fd, 0644) == 0);
    assert(snag_fd_privacy(fd, &privacy) == 0 && !privacy.private_access);
    assert(fchmod(fd, 0600) == 0);
#endif
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
    errno = 0;
    assert(snag_fd_privacy(-1, &privacy) == -1 && errno == EBADF);
    errno = 0;
    assert(snag_fd_privacy(-1, NULL) == -1 && errno == EINVAL);
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
    test_realpath();
    test_private_directory();
#ifdef _WIN32
    test_windows_privacy();
#endif
    puts("test_base: ok");
    return 0;
}

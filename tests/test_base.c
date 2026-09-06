/* SPDX-License-Identifier: GPL-2.0-only */
#include "base.h"
#include "fs.h"

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
#include <winioctl.h>
#include <aclapi.h>
#include <sddl.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/wait.h>
#endif

#ifdef _WIN32
static void
check_windows_permission_copy(int fd, const wchar_t *source)
{
    struct snag_permissions permissions = {0};
    wchar_t path[MAX_PATH + 8u];
    char utf8[(MAX_PATH + 8u) * 4u];
    assert(swprintf(path, sizeof(path) / sizeof(path[0]), L"%ls-copy", source) > 0);
    assert(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path, -1,
                               utf8, sizeof(utf8), NULL, NULL));
    int copy = snag_create_private_at(-1, utf8, true);
    assert(copy >= 0 && snag_permissions_capture(fd, &permissions) == 0);
    assert(snag_permissions_match(fd, &permissions) == 1);
    if (snag_permissions_apply(copy, &permissions) < 0) {
        (void)fprintf(stderr, "permission copy failed: errno=%d\n", errno);
        struct snag_permissions actual = {0};
        assert(snag_permissions_capture(copy, &actual) == 0);
        PSECURITY_DESCRIPTOR descriptors[] = {permissions.native, actual.native};
        for (size_t i = 0; i < 2u; ++i) {
            char *sddl = NULL;
            SECURITY_DESCRIPTOR_CONTROL control;
            DWORD revision;
            assert(ConvertSecurityDescriptorToStringSecurityDescriptorA(descriptors[i],
                SDDL_REVISION_1, OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION |
                DACL_SECURITY_INFORMATION, &sddl, NULL));
            assert(GetSecurityDescriptorControl(descriptors[i], &control, &revision));
            (void)fprintf(stderr, "%s control=%x %s\n", i ? "copy" : "source", control, sddl);
            LocalFree(sddl);
        }
        snag_permissions_free(&actual);
        abort();
    }
    assert(snag_permissions_match(copy, &permissions) == 1);
    assert((GetFileAttributesW(path) & FILE_ATTRIBUTE_READONLY) ==
           (GetFileAttributesW(source) & FILE_ATTRIBUTE_READONLY));
    snag_permissions_free(&permissions);
    assert(SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL));
    assert(close(copy) == 0 && DeleteFileW(path));
}

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
    check_windows_permission_copy(fd, path);
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
        check_windows_permission_copy(fd, path);
    }
    assert(SetSecurityInfo(handle, SE_FILE_OBJECT,
                           DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                           NULL, NULL, NULL, NULL) == ERROR_SUCCESS);
    assert(SetFileAttributesW(path, FILE_ATTRIBUTE_READONLY));
    assert(snag_fd_privacy(fd, &privacy) == 0 && !privacy.private_access);
    check_windows_permission_copy(fd, path);
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

#ifdef _WIN32
struct lock_waiter {
    int fd;
    HANDLE ready;
};

static unsigned int __stdcall
wait_for_lock(void *opaque)
{
    struct lock_waiter *waiter = opaque;

    assert(snag_lock_file(waiter->fd, false) < 0 && errno == EAGAIN);
    assert(SetEvent(waiter->ready));
    assert(snag_lock_file(waiter->fd, true) == 0);
    assert(close(waiter->fd) == 0);
    return 0u;
}

struct directory_waiter {
    int fd;
    bool abandon;
    struct snag_directory_lock lock;
};

static unsigned int __stdcall
try_directory_lock(void *opaque)
{
    struct directory_waiter *waiter = opaque;
    int rc = snag_directory_lock_acquire(waiter->fd, &waiter->lock);

    if (waiter->abandon)
        assert(rc == 0); /* Leave it owned to test abandoned-writer recovery. */
    else
        assert(rc < 0 && errno == EAGAIN && waiter->lock.fd == -1);
    return 0u;
}
#endif

static void
test_directory_lock(const char *path, int fd)
{
    struct snag_directory_lock lock = {.fd = -1}, other = {.fd = -1};
    snag_file_info info;
    int second = snag_open_read(path, true);

    assert(second >= 0 && snag_directory_lock_acquire(fd, &lock) == 0);
#ifdef _WIN32
    struct directory_waiter waiter = {.fd = second, .lock = {.fd = -1}};
    HANDLE thread = (HANDLE)_beginthreadex(NULL, 0, try_directory_lock, &waiter, 0, NULL);
    DWORD status;
    assert(thread && WaitForSingleObject(thread, 5000u) == WAIT_OBJECT_0);
    assert(GetExitCodeThread(thread, &status) && status == 0 && CloseHandle(thread));
#else
    assert(snag_directory_lock_acquire(second, &other) < 0 &&
           (errno == EAGAIN || errno == EWOULDBLOCK) && other.fd == -1);
#endif
    assert(snag_directory_lock_release(&lock) == 0);
    assert(snag_fstat(fd, &info) == 0 && S_ISDIR(info.st_mode));
    assert(snag_directory_lock_acquire(second, &other) == 0);
    assert(snag_directory_lock_release(&other) == 0);
#ifdef _WIN32
    waiter.abandon = true;
    thread = (HANDLE)_beginthreadex(NULL, 0, try_directory_lock, &waiter, 0, NULL);
    assert(thread && WaitForSingleObject(thread, 5000u) == WAIT_OBJECT_0);
    assert(GetExitCodeThread(thread, &status) && status == 0 && CloseHandle(thread));
    assert(snag_directory_lock_acquire(fd, &lock) == 0);
    assert(CloseHandle(waiter.lock.mutex));
    assert(snag_directory_lock_release(&lock) == 0);
#endif
    assert(close(second) == 0 && snag_directory_lock_release(&lock) == 0);
    assert(snag_directory_lock_acquire(-1, &lock) < 0 && errno == EBADF);
}

static void
test_file_lock(int dirfd)
{
    int fd = snag_create_private_at(dirfd, "lock-test", true);

    assert(fd >= 0 && snag_lock_file(fd, false) == 0);
#ifdef _WIN32
    struct lock_waiter waiter = {
        .fd = snag_create_private_at(dirfd, "lock-test", false),
        .ready = CreateEventW(NULL, TRUE, FALSE, NULL)
    };
    assert(waiter.fd >= 0 && waiter.ready);
    HANDLE thread = (HANDLE)_beginthreadex(NULL, 0, wait_for_lock, &waiter, 0, NULL);
    DWORD status;
    assert(thread && WaitForSingleObject(waiter.ready, 5000u) == WAIT_OBJECT_0);
    assert(WaitForSingleObject(thread, 100u) == WAIT_TIMEOUT);
    assert(close(fd) == 0);
    assert(WaitForSingleObject(thread, 5000u) == WAIT_OBJECT_0);
    assert(GetExitCodeThread(thread, &status) && status == 0u);
    assert(CloseHandle(thread) && CloseHandle(waiter.ready));
#else
    int ready[2], status;
    char byte;
    assert(pipe(ready) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        assert(close(ready[0]) == 0 && close(fd) == 0);
        fd = snag_create_private_at(dirfd, "lock-test", false);
        assert(fd >= 0 && snag_lock_file(fd, false) < 0 && errno == EAGAIN);
        assert(write(ready[1], "r", 1u) == 1);
        assert(snag_lock_file(fd, true) == 0 && close(fd) == 0);
        _exit(0);
    }
    assert(close(ready[1]) == 0 && read(ready[0], &byte, 1u) == 1 && byte == 'r');
    assert(waitpid(child, &status, WNOHANG) == 0);
    assert(close(fd) == 0 && close(ready[0]) == 0);
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
#endif
    fd = snag_create_private_at(dirfd, "lock-test", false);
    assert(fd >= 0 && snag_lock_file(fd, true) == 0 && close(fd) == 0);
    assert(snag_unlink_at(dirfd, "lock-test", false) == 0);
    assert(snag_lock_file(-1, false) < 0 && errno == EBADF);
}

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
    assert(snag_open_read(NULL, false) == -1 && errno == EINVAL);
    assert(snag_open_read("NUL", false) == -1 && errno == EACCES);
    assert(snag_open_read("C:/NUL", false) == -1 && errno == EACCES);
    assert(snag_open_read("//./pipe/snajpagent-test", false) == -1 && errno == EINVAL);
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
    snag_file_info root_info, path_info;
    assert(snag_fstat(fd, &root_info) == 0 && S_ISDIR(root_info.st_mode));
    assert(snag_stat(root, &path_info) == 0 && root_info.st_dev == path_info.st_dev &&
           root_info.st_ino == path_info.st_ino);
    assert(snag_fd_privacy(fd, &privacy) == 0 && privacy.effective_owner && privacy.private_access);
    {
        const char bytes[] = {'a', '\n', '\0', '\x1a', 'z'};
        char received[sizeof(bytes)];
        char *data = snag_path_join(root, "data"), *alias = snag_path_join(root, "alias");
        int file = snag_create_private_at(fd, "data", true);
        assert(data && alias && file >= 0);
        assert(snag_fd_privacy(file, &privacy) == 0 && privacy.effective_owner && privacy.private_access);
        assert(snag_write_full(file, bytes, sizeof(bytes)) == 0);
        snag_file_info info, linked;
        assert(snag_fstat(file, &info) == 0 && S_ISREG(info.st_mode) && info.st_size == sizeof(bytes));
        assert(snag_lstat_at(fd, "data", &linked) == 0 && linked.st_dev == info.st_dev &&
               linked.st_ino == info.st_ino && linked.st_size == sizeof(bytes));
        assert(snag_sync_file(file) == 0 && close(file) == 0);
        errno = 0;
        assert(snag_create_private_at(fd, "data", true) == -1 && errno == EEXIST);
        file = snag_create_private_at(fd, "data", false);
        assert(file >= 0);
        assert(read(file, received, sizeof(received)) == sizeof(received));
        assert(memcmp(bytes, received, sizeof(bytes)) == 0);
#ifdef _WIN32
        LARGE_INTEGER offset;
        DWORD returned;
        offset.QuadPart = INT64_C(5368709120);
        assert(DeviceIoControl((HANDLE)_get_osfhandle(file), FSCTL_SET_SPARSE,
                               NULL, 0, NULL, 0, &returned, NULL));
        assert(SetFilePointerEx((HANDLE)_get_osfhandle(file), offset, NULL, FILE_BEGIN));
        assert(SetEndOfFile((HANDLE)_get_osfhandle(file)));
        assert(snag_fstat(file, &linked) == 0 && linked.st_size == offset.QuadPart);
        offset.QuadPart = sizeof(bytes);
        assert(SetFilePointerEx((HANDLE)_get_osfhandle(file), offset, NULL, FILE_BEGIN));
        assert(SetEndOfFile((HANDLE)_get_osfhandle(file)));
#else
        assert(ftruncate(file, (off_t)INT64_C(5368709120)) == 0);
        assert(snag_fstat(file, &linked) == 0 && linked.st_size == INT64_C(5368709120));
        assert(ftruncate(file, sizeof(bytes)) == 0);
#endif
        assert(close(file) == 0);
        file = snag_open_read_at(fd, "data", false);
        assert(file >= 0 && read(file, received, sizeof(received)) == sizeof(received));
        assert(memcmp(bytes, received, sizeof(bytes)) == 0);
        assert(!snag_directory_open(file) && errno == ENOTDIR);
        assert(snag_fstat(file, &info) == 0);
        assert(close(file) == 0);
        file = snag_open_read_security_at(fd, "data", false);
        assert(file >= 0 && snag_fd_privacy(file, &privacy) == 0 && privacy.private_access);
        assert(read(file, received, sizeof(received)) == sizeof(received));
        assert(memcmp(bytes, received, sizeof(bytes)) == 0 && close(file) == 0);
#ifdef _WIN32
        assert(CreateHardLinkA(alias, data, NULL));
#else
        assert(link(data, alias) == 0);
#endif
        assert(snag_stat(data, &info) == 0 && snag_stat(alias, &linked) == 0);
        assert(info.st_ino == linked.st_ino && info.st_dev == linked.st_dev && info.st_nlink == 2u);
        assert(snag_open_private_append_at(fd, "data", false) == -1 && errno == EACCES);
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
        assert(snag_unlink_at(fd, "alias", false) == 0);
#ifdef _WIN32
        assert(CreateSymbolicLinkA(alias, data, 0));
#else
        assert(symlink(data, alias) == 0);
#endif
        assert(snag_lstat(alias, &linked) == 0 && S_ISLNK(linked.st_mode));
        assert(snag_open_read_at(fd, "alias", false) == -1);
        assert(snag_open_private_append_at(fd, "alias", false) == -1);
        assert(snag_stat(alias, &linked) == 0 && S_ISREG(linked.st_mode) &&
               linked.st_dev == info.st_dev && linked.st_ino == info.st_ino);
        assert(snag_unlink_at(fd, "alias", false) == 0);
        assert(snag_stat(data, &linked) == 0 && S_ISREG(linked.st_mode));
#ifdef _WIN32
        assert(SetNamedSecurityInfoA(data, SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
            NULL, NULL, NULL, NULL) == ERROR_SUCCESS);
#else
        assert(chmod(data, 0644) == 0);
#endif
        errno = 0;
        assert(snag_create_private_at(fd, "data", false) == -1 && errno == EACCES);
        assert(snag_unlink_at(fd, "data", false) == 0);
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
    errno = 0;
    assert(snag_unlink_at(fd, "child", false) == -1 && errno == EISDIR);
    assert(snag_rename_at(fd, "child", fd, "renamed") == 0);
    assert(snag_unlink_at(fd, "renamed", true) == 0);
    assert(snag_unlink_at(-1, absolute, true) == 0);
    {
        int source = snag_create_private_at(fd, "source", true);
        int target = snag_create_private_at(fd, "target", true);
        assert(source >= 0 && target >= 0);
        assert(snag_write_full(source, "original", 8u) == 0);
        assert(close(source) == 0 && close(target) == 0);
        assert(snag_rename_at(fd, "source", fd, "target") == 0);
        assert(snag_lstat_at(fd, "target", &path_info) == 0 && path_info.st_size == 8);
        errno = 0;
        assert(snag_rename_at(fd, "missing", fd, "target") == -1 && errno == ENOENT);
        assert(snag_lstat_at(fd, "target", &path_info) == 0 && path_info.st_size == 8);
        assert(snag_unlink_at(fd, "target", false) == 0);
    }
    free(child);
    free(renamed);
    free(absolute);
    renamed = malloc(strlen(root) + 9u);
    assert(renamed);
    (void)sprintf(renamed, "%s-renamed", root);
    {
        unsigned char received[5];
        const unsigned char expected[] = {'a', '\r', '\n', '\0', 'b'};
        assert(snag_open_private_append_at(fd, "journal", false) < 0 && errno == ENOENT);
        int file = snag_open_private_append_at(fd, "journal", true);
        assert(file >= 0 && snag_write_full(file, expected, 3u) == 0 && close(file) == 0);
        assert(snag_open_private_append_at(fd, "journal", true) < 0 && errno == EEXIST);
        file = snag_open_private_append_at(fd, "journal", false);
        assert(file >= 0 && lseek(file, 0, SEEK_SET) == 0);
        assert(snag_write_full(file, expected + 3u, 2u) == 0 && close(file) == 0);
        file = snag_open_read_security_at(fd, "journal", false);
        assert(file >= 0 && read(file, received, sizeof(received)) == sizeof(received));
        assert(!memcmp(expected, received, sizeof(expected)) && close(file) == 0);
        assert(snag_unlink_at(fd, "journal", false) == 0);
    }
    test_file_lock(fd);
    test_directory_lock(root, fd);
#ifndef _WIN32
    {
        struct snag_permissions permissions = {0};
        mode_t mask = umask(0777);
        int file = snag_create_private_at(fd, "permissions", true);
        (void)umask(mask);
        assert(file >= 0 && snag_fstat(file, &path_info) == 0 &&
               (path_info.st_mode & 0777u) == 0600u);
        const mode_t modes[] = {0u, 0400u, 0640u, 0751u};
        for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i) {
            assert(fchmod(file, modes[i]) == 0);
            assert(snag_permissions_capture(file, &permissions) == 0);
            assert(snag_permissions_match(file, &permissions) == 1);
            assert(fchmod(file, modes[i] ^ 0100u) == 0);
            assert(snag_permissions_match(file, &permissions) == 0);
            assert(snag_permissions_apply(file, &permissions) == 0);
            assert(snag_permissions_match(file, &permissions) == 1);
            snag_permissions_free(&permissions);
        }
        assert(close(file) == 0 && snag_unlink_at(fd, "permissions", false) == 0);
    }
#endif
    assert(rename(root, renamed) == 0);
    assert(snag_fstat(fd, &path_info) == 0 && path_info.st_ino == root_info.st_ino);
    {
        char long_name[512] = "unicode-";
#ifdef _WIN32
        size_t repetitions = 130u;
#else
        size_t repetitions = 20u;
#endif
        for (size_t i = 0u; i < repetitions; ++i)
            strcat(long_name, "\xe4\xb8\xad");
        strcat(long_name, "-\xf0\x9f\x98\x80");
        int file = snag_create_private_at(fd, long_name, true);
        assert(file >= 0 && close(file) == 0);
        int read_fd = snag_open_read(renamed, true);
        struct snag_directory *dir = snag_directory_open(read_fd);
        const char *entry;
        size_t found = 0u;
        assert(read_fd >= 0 && dir);
        errno = 0;
        while ((entry = snag_directory_next(dir)))
            if (strcmp(entry, ".") && strcmp(entry, "..")) {
                assert(strcmp(entry, long_name) == 0);
                ++found;
            }
        assert(errno == 0 && found == 1u);
        assert(snag_directory_close(dir) == 0);
        errno = 0;
        assert(snag_fstat(read_fd, &path_info) == -1 && errno == EBADF);
        assert(snag_unlink_at(fd, long_name, false) == 0);
        read_fd = snag_open_read_security_at(-1, renamed, true);
        assert(read_fd >= 0 && snag_fd_privacy(read_fd, &privacy) == 0 && privacy.private_access);
        dir = snag_directory_open(read_fd);
        assert(dir);
        while ((entry = snag_directory_next(dir)))
            assert(!strcmp(entry, ".") || !strcmp(entry, ".."));
        assert(errno == 0 && snag_directory_close(dir) == 0);
    }
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

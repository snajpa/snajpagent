/* SPDX-License-Identifier: GPL-2.0-only */
#include "base.h"
#include "fs.h"
#include "wake.h"
#include "net.h"
#include "term_host.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>
#include <locale.h>
#include <regex.h>
#include <signal.h>
#include <stdatomic.h>
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
#include <pthread.h>
#endif

static atomic_int shutdown_signal_seen;

static void
test_shutdown_signal(int number)
{
    atomic_store(&shutdown_signal_seen, number);
}

#ifdef _WIN32
static int
editor_test_child(void)
{
    const WCHAR *command = GetCommandLineW();
    const WCHAR *end = wcsrchr(command, L'"'), *start;
    assert(end && !end[1]);
    start = end;
    while (start > command && start[-1] != L'"')
        --start;
    assert(start > command);
    size_t len = (size_t)(end - start);
    WCHAR *path = calloc(len + 1u, sizeof(*path));
    assert(path);
    memcpy(path, start, len * sizeof(*path));
    HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    free(path);
    assert(file != INVALID_HANDLE_VALUE);
    DWORD written;
    assert(WriteFile(file, "edited", 6u, &written, NULL) && written == 6u && CloseHandle(file));
    return 0;
}

static void
test_native_editor(void)
{
    WCHAR temp[MAX_PATH], original[MAX_PATH], path[MAX_PATH], program[32768], editor[32768];
    DWORD old_len = GetEnvironmentVariableW(L"EDITOR", NULL, 0);
    WCHAR *old = old_len ? calloc(old_len, sizeof(*old)) : NULL;
    if (old_len)
        assert(old && GetEnvironmentVariableW(L"EDITOR", old, old_len) < old_len);
    assert(GetTempPathW(MAX_PATH, temp) && GetTempFileNameW(temp, L"edt", 0, original));
    assert(swprintf(path, MAX_PATH, L"%ls - \x4e2d & %%.ini", original) > 0);
    assert(MoveFileW(original, path));
    char utf8[MAX_PATH * 4u];
    assert(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path, -1, utf8, sizeof(utf8), NULL, NULL));
    assert(GetModuleFileNameW(NULL, program, 32768u));
    assert(swprintf(editor, 32768u, L"\"%ls\" --editor-test-child", program) > 0);
    assert(SetEnvironmentVariableW(L"EDITOR", editor));
    bool success;
    assert(snag_editor_run(utf8, &success) == 0 && success);
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    char data[6];
    DWORD got;
    assert(file != INVALID_HANDLE_VALUE && ReadFile(file, data, sizeof(data), &got, NULL));
    assert(got == sizeof(data) && !memcmp(data, "edited", sizeof(data)) && CloseHandle(file));
    assert(swprintf(editor, 32768u, L"\"%ls\" --editor-test-fail", program) > 0);
    assert(SetEnvironmentVariableW(L"EDITOR", editor));
    assert(snag_editor_run(utf8, &success) == 0 && !success);
    assert(SetEnvironmentVariableW(L"EDITOR", NULL));
    assert(snag_editor_run(utf8, &success) < 0 && errno == ENOENT);
    assert(SetEnvironmentVariableW(L"EDITOR", old));
    free(old);
    assert(DeleteFileW(path));
}

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
    assert(snag_fsync(fd) == 0);
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
        DWORD returned;
        assert(DeviceIoControl((HANDLE)_get_osfhandle(file), FSCTL_SET_SPARSE,
                               NULL, 0, NULL, 0, &returned, NULL));
#endif
        assert(snag_truncate(file, INT64_C(5368709120)) == 0);
        assert(snag_fstat(file, &linked) == 0 && linked.st_size == INT64_C(5368709120));
        assert(snag_seek(file, INT64_C(5368709119), SEEK_SET) == INT64_C(5368709119));
        assert(snag_write_full(file, "z", 1u) == 0);
        assert(snag_pread(file, received, 1u, INT64_C(5368709119)) == 1 && received[0] == 'z');
        assert(snag_seek(file, 0, SEEK_CUR) == INT64_C(5368709120));
        assert(snag_truncate(file, sizeof(bytes)) == 0);
        assert(snag_seek(file, 2, SEEK_SET) == 2);
        assert(snag_pread(file, received, sizeof(received), 0) == sizeof(received));
        assert(!memcmp(bytes, received, sizeof(bytes)) && snag_seek(file, 0, SEEK_CUR) == 2);
        assert(snag_pread(file, received, 1u, INT64_C(5368709120)) == 0);
        assert(snag_pread(file, received, 1u, -1) == -1 && errno == EINVAL);
        assert(snag_truncate(file, -1) == -1 && errno == EINVAL);
        assert(snag_seek(file, 0, SEEK_CUR) == 2);
        assert(snag_pread(-1, received, 1u, 0) == -1 && errno == EBADF);
        assert(snag_truncate(-1, 0) == -1 && errno == EBADF);
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
        assert(snag_link_at(fd, "data", fd, "alias") == 0);
        assert(snag_link_at(fd, "data", fd, "alias") == -1 && errno == EEXIST);
        assert(snag_stat(data, &info) == 0 && snag_stat(alias, &linked) == 0);
        assert(info.st_ino == linked.st_ino && info.st_dev == linked.st_dev && info.st_nlink == 2u);
        assert(snag_open_private_append_at(fd, "data", false) == -1 && errno == EACCES);
        assert(snag_open_history(data) == -1 && errno == EACCES);
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
        assert(snag_open_history(alias) == -1);
        file = snag_open_secret_file(alias);
        assert(file >= 0 && read(file, received, sizeof(received)) == sizeof(received));
        assert(!memcmp(bytes, received, sizeof(bytes)) && close(file) == 0);
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
        file = snag_open_history(data);
        assert(file >= 0 && snag_fd_privacy(file, &privacy) == 0 &&
               privacy.effective_owner && privacy.private_access);
        assert(read(file, received, sizeof(received)) == sizeof(received));
        assert(!memcmp(bytes, received, sizeof(bytes)));
        assert(lseek(file, 0, SEEK_SET) == 0 && snag_write_full(file, "z", 1u) == 0);
        assert(snag_fstat(file, &linked) == 0 && linked.st_size == sizeof(bytes) + 1u);
        assert(close(file) == 0);
#ifndef _WIN32
        if (geteuid() == 0) {
            assert(chown(data, 1u, (gid_t)-1) == 0);
            assert(snag_open_history(data) == -1 && errno == EACCES);
            assert(chown(data, 0u, (gid_t)-1) == 0);
        }
#endif
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
    {
        unsigned char bytes[] = {'x', '\r', '\n', 0x1au};
        unsigned char received[sizeof(bytes)];
#ifndef _WIN32
        mode_t mask = umask(0027);
#endif
        int file = snag_create_output_at(fd, "output");
        assert(file >= 0 && snag_write_full(file, bytes, sizeof(bytes)) == 0);
        assert(snag_fstat(file, &path_info) == 0 && close(file) == 0);
#ifndef _WIN32
        assert((path_info.st_mode & 0777u) == 0640u);
        (void)umask(mask);
#endif
        assert(snag_create_output_at(fd, "output") < 0 && errno == EEXIST);
        file = snag_open_read_at(fd, "output", false);
        assert(file >= 0 && read(file, received, sizeof(received)) == sizeof(received));
        assert(!memcmp(bytes, received, sizeof(bytes)) && close(file) == 0);
        assert(snag_unlink_at(fd, "output", false) == 0);
    }
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

#ifdef _WIN32
static int
cancel_console_output(void *opaque)
{
    unsigned int *calls = opaque;
    ++*calls;
    if (*calls < 2u)
        return 0;
    errno = ECANCELED;
    return -1;
}

static unsigned int __stdcall
interrupt_hidden_console(void *opaque)
{
    (void)opaque;
    assert(snag_sleep_ms(40u) == 0);
    assert(GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, GetCurrentProcessId()));
    return 0;
}

static void
test_hidden_console(void)
{
    HANDLE input = (HANDLE)_get_osfhandle(0);
    struct snag_term_host host = {0};
    DWORD mode, written;
    assert(snag_term_input_capture(&host) == 0);
    assert(snag_term_input_hidden(&host) == 0);
    assert(GetConsoleMode(input, &mode) && !(mode & ENABLE_ECHO_INPUT) && (mode & ENABLE_LINE_INPUT));
    const WCHAR chars[] = {0x4e2du, 0xd83du, 0xde00u, L'\r'};
    INPUT_RECORD keys[4] = {0};
    for (size_t i = 0; i < 4u; ++i) {
        keys[i].EventType = KEY_EVENT;
        keys[i].Event.KeyEvent.bKeyDown = TRUE;
        keys[i].Event.KeyEvent.wRepeatCount = 1;
        keys[i].Event.KeyEvent.uChar.UnicodeChar = chars[i];
    }
    keys[3].Event.KeyEvent.wVirtualKeyCode = VK_RETURN;
    assert(WriteConsoleInputW(input, keys, 4u, &written) && written == 4u);
    const char expected[] = "\xe4\xb8\xad\xf0\x9f\x98\x80\n";
    size_t used = 0;
    while (used < sizeof(expected) - 1u) {
        char bytes[4];
        assert(snag_term_input_wait(&host, SNAG_WAKE_INVALID, 0) & SNAG_TERM_WAIT_INPUT);
        ssize_t n = snag_term_input_read(&host, bytes, sizeof(bytes));
        if (n < 0 && errno == EAGAIN)
            continue;
        assert(n > 0 && (size_t)n <= sizeof(expected) - 1u - used);
        assert(!memcmp(bytes, expected + used, (size_t)n));
        used += (size_t)n;
        if (host.line_input) {
            DWORD flags;
            assert(GetHandleInformation(host.line_input, &flags) && !(flags & HANDLE_FLAG_INHERIT));
        }
    }
    assert(!host.line_input);
    assert(snag_term_input_restore(&host, true) == 0);
    assert(GetConsoleMode(input, &mode) && mode == host.input_mode);
    /* A following prompt must not receive a leftover CRLF as an empty line. */
    memset(&host, 0, sizeof(host));
    keys[0].Event.KeyEvent.uChar.UnicodeChar = L'x';
    assert(WriteConsoleInputW(input, keys, 1u, &written) && written == 1u);
    assert(WriteConsoleInputW(input, keys + 3u, 1u, &written) && written == 1u);
    char bytes[4];
    assert(snag_term_input_read(&host, bytes, sizeof(bytes)) == 1 && bytes[0] == 'x');
    assert(snag_term_input_read(&host, bytes, sizeof(bytes)) < 0 && errno == EAGAIN);
    assert(snag_term_input_read(&host, bytes, sizeof(bytes)) == 1 && bytes[0] == '\n');
    assert(FlushConsoleInputBuffer(input));
    struct snag_shutdown shutdown;
    atomic_store(&shutdown_signal_seen, 0);
    assert(snag_shutdown_install(&shutdown, test_shutdown_signal, false) == 0);
    assert(snag_term_input_capture(&host) == 0 && snag_term_input_hidden(&host) == 0);
    HANDLE interrupter = (HANDLE)_beginthreadex(NULL, 0, interrupt_hidden_console, NULL, 0, NULL);
    assert(interrupter);
    uint64_t cancel_start = snag_monotonic_ms();
    ssize_t n = snag_term_input_read(&host, bytes, sizeof(bytes));
    uint64_t cancel_elapsed = snag_monotonic_ms() - cancel_start;
    assert(snag_term_input_restore(&host, true) == 0);
    assert(WaitForSingleObject(interrupter, 1000u) == WAIT_OBJECT_0 && CloseHandle(interrupter));
    snag_shutdown_detach(&shutdown);
    snag_shutdown_finish(&shutdown);
    assert(n <= 0 && atomic_load(&shutdown_signal_seen) == SIGINT && cancel_elapsed < 1000u);
}

static void
test_console_output(void)
{
    struct snag_term_host host = {0};
    int pair[2];
    assert(_pipe(pair, 4096u, _O_BINARY | _O_NOINHERIT) == 0);
    char output[65536];
    memset(output, 'x', sizeof(output));
    unsigned int calls = 0;
    uint64_t start = snag_monotonic_ms();
    assert(snag_term_output_write(&host, pair[1], output, sizeof(output), false,
                                  cancel_console_output, &calls) < 0 && errno == ECANCELED);
    assert(calls == 2u && snag_monotonic_ms() - start < 2000u);
    assert(close(pair[1]) == 0);
    DWORD got;
    assert(ReadFile((HANDLE)_get_osfhandle(pair[0]), output, sizeof(output), &got, NULL) && got);
    for (DWORD i = 0; i < got; ++i)
        assert(output[i] == 'x');
    assert(close(pair[0]) == 0);
    int sink = _open("NUL", _O_WRONLY | _O_BINARY | _O_NOINHERIT);
    assert(sink >= 0);
    assert(snag_term_output_write(&host, sink, "ok", 2u, false, cancel_console_output, &calls) == 0);
    assert(close(sink) == 0);
    snag_term_host_close(&host);
    assert(!host.writer);

    HANDLE screen = CreateConsoleScreenBuffer(GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CONSOLE_TEXTMODE_BUFFER, NULL);
    assert(screen != INVALID_HANDLE_VALUE);
    int fd = _open_osfhandle((intptr_t)screen, _O_WRONLY | _O_BINARY | _O_NOINHERIT);
    assert(fd >= 0);
    assert(snag_term_output_write(NULL, fd, "A\xe4\xb8\xad\xf0\x9f\x98\x80Z", 9u,
                                  false, NULL, NULL) == 0);
    WCHAR result[16] = {0};
    BOOL read_ok = ReadConsoleOutputCharacterW(screen, result, 10u, (COORD){0, 0}, &got);
    const WCHAR expected[] = {L'A', 0x4e2du, 0xd83du, 0xde00u, L'Z'};
    assert(SetConsoleCursorPosition(screen, (COORD){0, 1}));
    DWORD direct_count;
    assert(WriteConsoleW(screen, expected, 5u, &direct_count, NULL) && direct_count == 5u);
    WCHAR direct[16] = {0};
    assert(ReadConsoleOutputCharacterW(screen, direct, 10u, (COORD){0, 1}, &direct_count));
    /* The classic console cell APIs may replace supplementary glyphs even
     * for direct WriteConsoleW. Compare identical native screen projections. */
    if (!read_ok || got != direct_count || memcmp(result, direct, got * sizeof(*result))) {
        (void)fprintf(stderr, "console output ok=%u got=%lu error=%lu units:",
                       (unsigned int)read_ok, (unsigned long)got, (unsigned long)GetLastError());
        for (DWORD i = 0; i < got; ++i)
            (void)fprintf(stderr, " %04x", (unsigned int)result[i]);
        (void)fprintf(stderr, "\n");
        (void)fprintf(stderr, "direct wide output got=%lu units:", (unsigned long)direct_count);
        for (DWORD i = 0; i < direct_count; ++i)
            (void)fprintf(stderr, " %04x", (unsigned int)direct[i]);
        (void)fprintf(stderr, "\n");
        CONSOLE_SCREEN_BUFFER_INFO info;
        if (GetConsoleScreenBufferInfo(screen, &info))
            (void)fprintf(stderr, "console cursor %d,%d\n", info.dwCursorPosition.X, info.dwCursorPosition.Y);
        CHAR_INFO cells[10] = {0};
        SMALL_RECT rect = {0, 0, 9, 0};
        if (ReadConsoleOutputW(screen, cells, (COORD){10, 1}, (COORD){0, 0}, &rect)) {
            (void)fprintf(stderr, "console cells:");
            for (size_t i = 0; i < 10u; ++i)
                (void)fprintf(stderr, " %04x/%04x", (unsigned int)cells[i].Char.UnicodeChar,
                               (unsigned int)cells[i].Attributes);
            (void)fprintf(stderr, "\n");
        }
        abort();
    }
    assert(result[0] == L'A' && result[1] == 0x4e2du);
    bool suffix = false;
    for (DWORD i = 0; i < got; ++i)
        suffix |= result[i] == L'Z';
    assert(suffix);
    assert(close(fd) == 0);
}

static void
test_console_keys(struct snag_term_host *host)
{
    static const struct {
        WORD key;
        WCHAR c;
        DWORD modifiers;
        WORD repeats;
        const char *bytes;
        size_t size;
    } cases[] = {
        {VK_UP, 0, 0, 1, "\033[A", 3},
        {VK_LEFT, 0, LEFT_CTRL_PRESSED, 1, "\033[1;5D", 6},
        {VK_HOME, 0, SHIFT_PRESSED | LEFT_ALT_PRESSED, 1, "\033[1;4H", 6},
        {VK_DELETE, 0, LEFT_ALT_PRESSED, 1, "\033[3;3~", 6},
        {VK_TAB, L'\t', SHIFT_PRESSED, 1, "\033[Z", 3},
        {'X', L'x', LEFT_ALT_PRESSED, 1, "\033x", 2},
        {'Q', L'@', RIGHT_ALT_PRESSED | LEFT_CTRL_PRESSED, 1, "@", 1},
        {VK_SPACE, 0, LEFT_CTRL_PRESSED, 1, "\0", 1},
        {'C', 3, LEFT_CTRL_PRESSED, 1, "\003", 1},
        {'X', L'x', 0, 10, "xxxxxxxxxx", 10},
        {VK_UP, 0, 0, 3, "\033[A\033[A\033[A", 9}
    };
    HANDLE input = (HANDLE)_get_osfhandle(0);
    DWORD written;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        INPUT_RECORD event = {.EventType = KEY_EVENT};
        event.Event.KeyEvent = (KEY_EVENT_RECORD){
            .bKeyDown = TRUE, .wRepeatCount = cases[i].repeats,
            .wVirtualKeyCode = cases[i].key, .uChar.UnicodeChar = cases[i].c,
            .dwControlKeyState = cases[i].modifiers
        };
        assert(WriteConsoleInputW(input, &event, 1u, &written) && written == 1u);
        size_t used = 0;
        while (used < cases[i].size) {
            char bytes[4];
            assert(snag_term_input_wait(host, SNAG_WAKE_INVALID, 0) & SNAG_TERM_WAIT_INPUT);
            ssize_t n = snag_term_input_read(host, bytes, sizeof(bytes));
            assert(n > 0 && (size_t)n <= cases[i].size - used);
            assert(!memcmp(bytes, cases[i].bytes + used, (size_t)n));
            used += (size_t)n;
        }
    }
    INPUT_RECORD events[20] = {0};
    for (size_t i = 0; i < 20u; ++i) {
        events[i].EventType = KEY_EVENT;
        events[i].Event.KeyEvent.bKeyDown = TRUE;
        events[i].Event.KeyEvent.wRepeatCount = 1;
        events[i].Event.KeyEvent.uChar.UnicodeChar = L'a';
    }
    assert(WriteConsoleInputW(input, events, 20u, &written) && written == 20u);
    for (size_t i = 0; i < 5u; ++i) {
        char bytes[4];
        assert(snag_term_input_wait(host, SNAG_WAKE_INVALID, 0) & SNAG_TERM_WAIT_INPUT);
        assert(snag_term_input_read(host, bytes, sizeof(bytes)) == 4);
        assert(!memcmp(bytes, "aaaa", 4u));
    }
    events[0].Event.KeyEvent.bKeyDown = FALSE;
    events[1].EventType = FOCUS_EVENT;
    events[2].EventType = WINDOW_BUFFER_SIZE_EVENT;
    events[2].Event.WindowBufferSizeEvent.dwSize = (COORD){80, 25};
    assert(WriteConsoleInputW(input, events, 3u, &written) && written == 3u);
    char bytes[4];
    assert(snag_term_input_read(host, bytes, sizeof(bytes)) < 0 && errno == EAGAIN);
    assert(snag_term_input_resized(host) && !snag_term_input_resized(host));
    assert(snag_term_input_wait(host, SNAG_WAKE_INVALID, 0) == 0);
}
#endif

static atomic_uint console_interrupts;
static void
test_control_signal(int number)
{
    if (number == SIGINT)
        (void)atomic_fetch_add(&console_interrupts, 1u);
}

static void
test_input_mode(void)
{
    assert(snag_isatty(-1) == 0 && errno == EBADF);
#ifdef _WIN32
    int sink = _open("NUL", _O_WRONLY | _O_BINARY);
    assert(sink >= 0 && !snag_isatty(sink) && close(sink) == 0);
#endif
    if (snag_isatty(2)) {
        struct snag_term_host output_host = {0};
#ifdef _WIN32
        DWORD original_mode, changed_mode;
        assert(GetConsoleMode((HANDLE)_get_osfhandle(2), &original_mode));
#endif
        int copy = snag_term_output_open(&output_host, 2);
        assert(copy >= 0 && snag_isatty(copy));
#ifdef _WIN32
        DWORD flags;
        assert(GetConsoleMode((HANDLE)_get_osfhandle(copy), &changed_mode) &&
               (changed_mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING));
        assert(snag_term_output_mode(&output_host, false) == 0);
        assert(GetConsoleMode((HANDLE)_get_osfhandle(copy), &changed_mode) && changed_mode == original_mode);
        assert(snag_term_output_mode(&output_host, true) == 0);
        assert(GetConsoleMode((HANDLE)_get_osfhandle(copy), &changed_mode) &&
               (changed_mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING));
        assert(GetHandleInformation((HANDLE)_get_osfhandle(copy), &flags) &&
               !(flags & HANDLE_FLAG_INHERIT));
#else
        assert(fcntl(copy, F_GETFD) & FD_CLOEXEC);
#endif
        snag_term_host_close(&output_host);
#ifdef _WIN32
        assert(GetConsoleMode((HANDLE)_get_osfhandle(2), &changed_mode) && changed_mode == original_mode);
#endif
        assert(close(copy) == 0 && snag_isatty(2));
    }
    if (!snag_isatty(0))
        return;
#ifdef _WIN32
    test_native_editor();
    test_hidden_console();
    test_console_output();
#endif
    struct snag_term_host host = {0};
#ifdef _WIN32
    UINT codepage = GetConsoleCP();
#endif
    assert(snag_term_input_capture(&host) == 0);
    assert(snag_term_input_raw(&host) == 0);
#ifdef _WIN32
    DWORD mode;
    assert(GetConsoleMode((HANDLE)_get_osfhandle(0), &mode));
    assert(!(mode & (ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT)));
    assert(GetConsoleCP() == codepage);
#else
    struct termios raw;
    assert(tcgetattr(0, &raw) == 0 && !(raw.c_lflag & (ECHO | ICANON | ISIG)));
#endif
    assert(snag_term_input_flush(&host) == 0);
    assert(snag_term_controls_install(&host, test_control_signal, test_control_signal) == 0);
#ifdef _WIN32
    assert(GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, GetCurrentProcessId()));
    uint64_t deadline = snag_monotonic_ms() + 1000u;
    while (!atomic_load(&console_interrupts) && snag_monotonic_ms() < deadline)
        assert(snag_sleep_ms(1u) == 0);
    assert(atomic_load(&console_interrupts) == 1u);
    assert(snag_term_input_wait(&host, SNAG_WAKE_INVALID, 1000) == SNAG_TERM_WAIT_WAKE);
    char ignored[4];
    assert(snag_term_input_read(&host, ignored, sizeof(ignored)) < 0 && errno == EAGAIN);
#else
    assert(raise(SIGINT) == 0 && atomic_load(&console_interrupts) == 1u);
#endif
    snag_term_controls_restore(&host);
#ifndef _WIN32
    struct sigaction restored_control;
    assert(sigaction(SIGINT, NULL, &restored_control) == 0 &&
           restored_control.sa_handler == host.sigint.sa_handler);
#endif
    assert(snag_term_input_wait(&host, SNAG_WAKE_INVALID, -2) < 0 && errno == EINVAL);
    snag_wake_fd wake[2];
    assert(snag_wakeup_create(wake) == 0);
    snag_wakeup_send(wake[1]);
    assert(snag_term_input_wait(&host, wake[0], 1000) == SNAG_TERM_WAIT_WAKE);
    /* Readiness observation must neither consume bytes nor lose registration. */
    assert(snag_term_input_wait(&host, wake[0], 1000) == SNAG_TERM_WAIT_WAKE);
    snag_wakeup_drain(wake[0]);
    assert(snag_term_input_wait(&host, wake[0], 0) == 0);
    snag_wakeup_send(wake[1]);
    assert(snag_term_input_wait(&host, wake[0], 1000) == SNAG_TERM_WAIT_WAKE);
    snag_wakeup_drain(wake[0]);
    snag_wakeup_close(wake);
#ifdef _WIN32
    INPUT_RECORD keys[3] = {0};
    const WCHAR characters[] = {0x4e2du, 0xd83du, 0xde00u};
    const char expected[] = "\xe4\xb8\xad\xf0\x9f\x98\x80";
    char received[sizeof(expected) - 1u];
    DWORD written;
    HANDLE input = (HANDLE)_get_osfhandle(0);
    for (size_t i = 0; i < 3u; ++i) {
        keys[i].EventType = KEY_EVENT;
        keys[i].Event.KeyEvent.bKeyDown = TRUE;
        keys[i].Event.KeyEvent.wRepeatCount = 1;
        keys[i].Event.KeyEvent.uChar.UnicodeChar = characters[i];
    }
    assert(WriteConsoleInputW(input, keys, 3u, &written) && written == 3u);
    size_t used = 0;
    while (used < sizeof(received)) {
        int ready = snag_term_input_wait(&host, SNAG_WAKE_INVALID, 1000);
        if (ready < 0 || !(ready & SNAG_TERM_WAIT_INPUT)) {
            DWORD available = 0;
            (void)GetNumberOfConsoleInputEvents(input, &available);
            (void)fprintf(stderr, "console ready=%d used=%zu queued=%lu cache=%u/%u key=%u/%u repeat=%u\n",
                           ready, used, (unsigned long)available, host.input_next, host.input_count,
                           host.input_key_at, host.input_key_len, host.input_repeats);
            (void)snag_term_input_restore(&host, true);
            abort();
        }
        ssize_t got = snag_term_input_read(&host, received + used, sizeof(received) - used);
        if (got < 0 && errno == EAGAIN)
            continue;
        assert(got > 0);
        used += (size_t)got;
    }
    if (memcmp(received, expected, sizeof(received))) {
        (void)fprintf(stderr, "console input bytes:");
        for (size_t i = 0; i < sizeof(received); ++i)
            (void)fprintf(stderr, " %02x", (unsigned char)received[i]);
        (void)fprintf(stderr, "\n");
        assert(snag_term_input_restore(&host, true) == 0);
        abort();
    }
    assert(WriteConsoleInputW(input, keys + 1u, 1u, &written) && written == 1u);
    assert(snag_term_input_read(&host, received, sizeof(received)) < 0 && errno == EAGAIN);
    assert(WriteConsoleInputW(input, keys + 2u, 1u, &written) && written == 1u);
    assert(snag_term_input_read(&host, received, sizeof(received)) == 4);
    assert(!memcmp(received, expected + 3u, 4u));
    assert(WriteConsoleInputW(input, keys + 1u, 1u, &written) && written == 1u);
    assert(snag_term_input_read(&host, received, sizeof(received)) < 0 && errno == EAGAIN);
    assert(snag_term_input_flush(&host) == 0 && host.input_high == 0);
    /* Malformed UTF-16 must not lose subsequent text or end the session. */
    assert(WriteConsoleInputW(input, keys + 2u, 1u, &written) && written == 1u);
    assert(snag_term_input_read(&host, received, sizeof(received)) == 3);
    assert(!memcmp(received, "\xef\xbf\xbd", 3u));
    assert(WriteConsoleInputW(input, keys + 1u, 1u, &written) && written == 1u);
    assert(snag_term_input_read(&host, received, sizeof(received)) < 0 && errno == EAGAIN);
    assert(WriteConsoleInputW(input, keys + 1u, 1u, &written) && written == 1u);
    assert(snag_term_input_read(&host, received, sizeof(received)) == 3 && host.input_high);
    assert(!memcmp(received, "\xef\xbf\xbd", 3u));
    keys[0].Event.KeyEvent.uChar.UnicodeChar = L'x';
    assert(WriteConsoleInputW(input, keys, 1u, &written) && written == 1u);
    assert(snag_term_input_read(&host, received, sizeof(received)) == 4 && !host.input_high);
    assert(!memcmp(received, "\xef\xbf\xbdx", 4u));
    test_console_keys(&host);
#endif
    assert(snag_term_input_restore(&host, false) == 0);
#ifdef _WIN32
    assert(GetConsoleMode((HANDLE)_get_osfhandle(0), &mode) && mode == host.input_mode);
    assert(GetConsoleCP() == codepage);
#else
    struct termios restored;
    assert(tcgetattr(0, &restored) == 0 && restored.c_lflag == host.input_mode.c_lflag &&
           restored.c_iflag == host.input_mode.c_iflag);
#endif
    assert(snag_term_input_raw(&host) == 0 && snag_term_input_restore(&host, true) == 0);
    if (snag_isatty(2))
        assert(snag_term_host_columns() > 0u);
    else
        assert(snag_term_host_columns() == 0u);
}

static void
test_platform(void)
{
    char *shell = snag_default_shell();
    assert(shell && snag_file_executable(shell) == 0);
    free(shell);
    char hostname[1024];
    assert(snag_hostname(hostname, sizeof(hostname)) == 0 && hostname[0]);
    assert(snag_utf8_valid((unsigned char *)hostname, strlen(hostname), true));
    struct snag_shutdown shutdown;
#ifndef _WIN32
    struct sigaction before_shutdown, after_shutdown;
    assert(sigaction(SIGTERM, NULL, &before_shutdown) == 0);
#endif
    assert(snag_shutdown_install(&shutdown, test_shutdown_signal, true) == 0);
    assert(raise(SIGTERM) == 0 && atomic_load(&shutdown_signal_seen) == SIGTERM);
#ifndef _WIN32
    assert(raise(SIGHUP) == 0 && atomic_load(&shutdown_signal_seen) == SIGHUP);
#endif
    snag_shutdown_detach(&shutdown);
    snag_shutdown_finish(&shutdown);
#ifndef _WIN32
    assert(sigaction(SIGTERM, NULL, &after_shutdown) == 0 &&
           before_shutdown.sa_handler == after_shutdown.sa_handler);
#endif
    struct snag_signal_mask saved;
    assert(snag_term_signals_block(&saved) == 0);
#ifndef _WIN32
    sigset_t current;
    assert(pthread_sigmask(SIG_SETMASK, NULL, &current) == 0);
    assert(sigismember(&current, SIGINT) && sigismember(&current, SIGWINCH));
#endif
    assert(snag_term_signals_unblock() == 0);
    assert(snag_term_signals_restore(&saved) == 0);
#ifndef _WIN32
    assert(pthread_sigmask(SIG_SETMASK, NULL, &current) == 0);
    assert(sigismember(&current, SIGINT) == sigismember(&saved.native, SIGINT));
    assert(sigismember(&current, SIGWINCH) == sigismember(&saved.native, SIGWINCH));
#endif
    unsigned char random[32], again[32];
    uint64_t before = snag_monotonic_ms();
    uint64_t wall = snag_time_ms();
    time_t seconds = time(NULL);
    FILE *file = tmpfile();
    char content[4] = {0};
    int fd;

    assert(snag_sleep_ms(0u) == 0);
    before = snag_monotonic_ms();
    assert(snag_sleep_ms(20u) == 0 && snag_monotonic_ms() >= before + 1u);
    assert(snag_sleep_ms(UINT_MAX) == -1 && errno == EINVAL);
    assert(snag_text_locale_init());

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
    int copy = snag_dup_read(fd);
    assert(copy >= 0);
#ifdef _WIN32
    DWORD inherited;
    assert(GetHandleInformation((HANDLE)_get_osfhandle(copy), &inherited) &&
           !(inherited & HANDLE_FLAG_INHERIT));
#else
    assert(fcntl(copy, F_GETFD) & FD_CLOEXEC);
#endif
    assert(close(copy) == 0);
    assert(snag_dup_read(-1) < 0 && errno == EBADF);
    {
        time_t seconds = 1709164800; /* 2024-02-29, UTC. */
        struct tm utc, local;
        assert(snag_gmtime(&seconds, &utc) == &utc && utc.tm_year == 124 &&
               utc.tm_mon == 1 && utc.tm_mday == 29 && utc.tm_hour == 0);
        assert(snag_localtime(&seconds, &local) == &local && mktime(&local) == seconds);
    }
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
    assert(snag_fsync(fd) == 0);
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
test_wakeup(void)
{
    snag_wake_fd pair[2];
    assert(snag_wakeup_create(pair) == 0);
    for (size_t i = 0; i < 2u; ++i) {
#ifdef _WIN32
        DWORD flags;
        assert(GetHandleInformation((HANDLE)pair[i], &flags) && !(flags & HANDLE_FLAG_INHERIT));
#else
        assert(fcntl(pair[i], F_GETFD) & FD_CLOEXEC);
#endif
    }
    assert(snag_wakeup_wait(pair[0], 0) == 0);
    snag_wakeup_send(pair[1]);
    snag_wakeup_send(pair[1]);
    assert(snag_wakeup_wait(pair[0], 1000) == 1);
    snag_wakeup_drain(pair[0]);
    assert(snag_wakeup_wait(pair[0], 1) == 0);
    snag_wakeup_send(pair[1]);
    assert(snag_wakeup_wait(pair[0], -1) == 1);
    snag_wakeup_drain(pair[0]);
    snag_wakeup_close(pair);
    assert(pair[0] == SNAG_WAKE_INVALID && pair[1] == SNAG_WAKE_INVALID);
    snag_wakeup_close(pair);
    assert(snag_wakeup_wait(SNAG_WAKE_INVALID, 0) == 0);
    assert(snag_wakeup_wait(SNAG_WAKE_INVALID, -1) == -1 && errno == EINVAL);
}

static void
test_sockets(void)
{
    struct sockaddr_in address = {0};
#ifdef _WIN32
    int address_size = sizeof(address);
#else
    socklen_t address_size = sizeof(address);
#endif
    assert(snag_network_init() == 0);
    struct addrinfo hints = {0}, *addresses = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    assert(snag_socket_addresses("localhost", "80", &hints, &addresses) == 0 && addresses);
    for (struct addrinfo *item = addresses; item; item = item->ai_next) {
        if (item->ai_family == AF_INET)
            assert(((struct sockaddr_in *)item->ai_addr)->sin_addr.s_addr == htonl(INADDR_LOOPBACK));
        else {
            assert(item->ai_family == AF_INET6);
            assert(IN6_IS_ADDR_LOOPBACK(&((struct sockaddr_in6 *)item->ai_addr)->sin6_addr));
        }
    }
    freeaddrinfo(addresses);
    snag_socket listener = snag_socket_open(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    assert(listener != SNAG_SOCKET_INVALID);
    assert(snag_socket_reuse(listener) == 0);
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(snag_socket_bind(listener, (struct sockaddr *)&address, sizeof(address)) == 0);
    assert(getsockname(listener, (struct sockaddr *)&address, &address_size) == 0);
    assert(snag_socket_listen(listener, 2) == 0);
    snag_socket occupied = snag_socket_open(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    assert(occupied != SNAG_SOCKET_INVALID && snag_socket_reuse(occupied) == 0);
    assert(snag_socket_bind(occupied, (struct sockaddr *)&address, sizeof(address)) < 0);
    assert(errno == EADDRINUSE || errno == EACCES);
    assert(snag_socket_close(occupied) == 0);
    assert(snag_socket_accept(listener) == SNAG_SOCKET_INVALID &&
           (errno == EAGAIN || errno == EWOULDBLOCK));
    snag_socket client = snag_socket_open(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    assert(client != SNAG_SOCKET_INVALID);
    int rc = snag_socket_connect(client, (struct sockaddr *)&address, sizeof(address));
    assert(rc == 0 || (rc < 0 && errno == EINPROGRESS));
    snag_socket_event ready = {listener, SNAG_NET_READ, 0};
    assert(snag_socket_poll(&ready, 1u, 1000) == 1 && (ready.revents & SNAG_NET_READ));
    snag_socket server = snag_socket_accept(listener);
    assert(server != SNAG_SOCKET_INVALID);
    ready = (snag_socket_event){client, SNAG_NET_WRITE, 0};
    assert(snag_socket_poll(&ready, 1u, 1000) == 1 && snag_socket_connected(client) == 0);
    snag_socket_nodelay(client);
    const char bytes[] = {'a', '\r', '\n', '\0'};
    char received[sizeof(bytes)];
    assert(snag_socket_recv(server, received, sizeof(received)) < 0 &&
           (errno == EAGAIN || errno == EWOULDBLOCK));
    assert(snag_socket_send(client, bytes, sizeof(bytes)) == sizeof(bytes));
    size_t total = 0;
    while (total < sizeof(bytes)) {
        ready = (snag_socket_event){server, SNAG_NET_READ, 0};
        assert(snag_socket_poll(&ready, 1u, 1000) == 1);
        ssize_t got = snag_socket_recv(server, received + total, sizeof(received) - total);
        assert(got > 0);
        total += (size_t)got;
    }
    assert(!memcmp(bytes, received, sizeof(bytes)));
    snag_socket_event many[80];
    for (size_t i = 0; i < 80u; ++i)
        many[i] = (snag_socket_event){SNAG_SOCKET_INVALID, SNAG_NET_READ, 0};
    many[79] = (snag_socket_event){client, SNAG_NET_WRITE, 0};
    assert(snag_socket_poll(many, 80u, 1000) == 1 && (many[79].revents & SNAG_NET_WRITE));
    assert(snag_socket_close(client) == 0);
    ready = (snag_socket_event){server, SNAG_NET_READ, 0};
    assert(snag_socket_poll(&ready, 1u, 1000) == 1);
    assert(snag_socket_recv(server, received, sizeof(received)) == 0);
    assert(snag_socket_close(server) == 0 && snag_socket_close(listener) == 0);
    snag_network_free();
}

static void
test_regex(void)
{
    static const struct {
        const char *pattern, *text;
        int flags;
        bool match;
    } cases[] = {
        {"^alpha", "Alpha beta", REG_ICASE, true},
        {"^(ab|cd){2}$", "abcd", 0, true},
        {"^(ab|cd){2}$", "abcde", 0, false},
        {"[[:digit:]]+", "value 42", 0, true},
        {"^.$", "\xc3\xa9", 0, true},
        {"^.$", "\xf0\x9f\x98\x80", 0, true},
        {"^..$", "\xf0\x9f\x98\x80", 0, false},
        {"^\xc3\x89$", "\xc3\xa9", REG_ICASE, true},
        {"^[[:alpha:]]+$", "\xc3\xa9", 0, true},
        {"^[[:alpha:]]+$", "\xf0\x9f\x98\x80", 0, false}
    };
#ifdef _WIN32
    /* The static engine is UTF-8 even when the legacy CRT is in the C locale. */
    assert(setlocale(LC_CTYPE, "C"));
#else
    assert(setlocale(LC_CTYPE, ""));
#endif
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        regex_t regex;
        assert(regcomp(&regex, cases[i].pattern, REG_EXTENDED | REG_NOSUB | cases[i].flags) == 0);
        int rc = regexec(&regex, cases[i].text, 0, NULL, 0);
        if (rc != (cases[i].match ? 0 : REG_NOMATCH)) {
            (void)fprintf(stderr, "regex case %zu: rc=%d expected match=%d\n", i, rc, cases[i].match);
            abort();
        }
        regfree(&regex);
    }
    regex_t invalid;
    char message[128];
    int rc = regcomp(&invalid, "[", REG_EXTENDED | REG_NOSUB);
    assert(rc != 0 && regerror(rc, &invalid, message, sizeof(message)) > 1u);
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
main(int argc, char **argv)
{
#ifdef _WIN32
    if (argc == 3 && !strcmp(argv[1], "--editor-test-child"))
        return editor_test_child();
    if (argc == 3 && !strcmp(argv[1], "--editor-test-fail"))
        return 7;
    if (argc == 1) {
        WCHAR program[32768], command[32768];
        DWORD size = GetModuleFileNameW(NULL, program, 32768u);
        assert(size && size < 32768u);
        assert(swprintf(command, 32768u, L"\"%ls\" --console-test-child", program) > 0);
        STARTUPINFOW startup = {.cb = sizeof(startup)};
        PROCESS_INFORMATION child;
        /* Generated control events must never interrupt the parent shell. */
        assert(CreateProcessW(program, command, NULL, NULL, FALSE, CREATE_NEW_PROCESS_GROUP,
                               NULL, NULL, &startup, &child));
        assert(CloseHandle(child.hThread));
        assert(WaitForSingleObject(child.hProcess, INFINITE) == WAIT_OBJECT_0);
        DWORD status;
        assert(GetExitCodeProcess(child.hProcess, &status) && CloseHandle(child.hProcess));
        return (int)status;
    }
    assert(argc == 2 && !strcmp(argv[1], "--console-test-child"));
#else
    (void)argc;
    (void)argv;
#endif
    assert(snag_irc_nick_mentioned("@ALICE: hi", "alice"));
    assert(snag_irc_nick_mentioned("{op}: hi", "[OP]"));
    assert(snag_irc_nick_mentioned("hello alice", "alice"));
    assert(!snag_irc_nick_mentioned("hello", ""));
    assert(!snag_irc_nick_mentioned("malice alice2 alice-other", "alice"));
    assert(!snag_irc_nick_mentioned("éalice aliceé", "alice"));
    assert(!snag_irc_nick_mentioned("alice", "alice2"));
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
    test_regex();
    test_wakeup();
    test_sockets();
    test_input_mode();
#ifdef _WIN32
    test_windows_privacy();
#endif
    puts("test_base: ok");
    return 0;
}

/* SPDX-License-Identifier: GPL-2.0-only */
#include "base.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <aclapi.h>
#include <wincrypt.h>
#include <io.h>
#include <uniwidth.h>
#include <wchar.h>

static int
path_error(DWORD error)
{
    switch (error) {
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND:
        errno = ENOENT; break;
    case ERROR_ACCESS_DENIED: case ERROR_PRIVILEGE_NOT_HELD:
        errno = EACCES; break;
    case ERROR_INVALID_HANDLE:
        errno = EBADF; break;
    case ERROR_NOT_ENOUGH_MEMORY: case ERROR_OUTOFMEMORY:
        errno = ENOMEM; break;
    case ERROR_FILENAME_EXCED_RANGE:
        errno = ENAMETOOLONG; break;
    case ERROR_NO_UNICODE_TRANSLATION:
        errno = EILSEQ; break;
    case ERROR_DIRECTORY:
        errno = ENOTDIR; break;
    case ERROR_NOT_SUPPORTED: case ERROR_INVALID_FUNCTION:
        errno = ENOTSUP; break;
    case ERROR_CANT_RESOLVE_FILENAME:
        errno = ELOOP; break;
    case ERROR_INVALID_NAME: case ERROR_INVALID_PARAMETER:
        errno = EINVAL; break;
    default:
        errno = EIO; break;
    }
    return -1;
}

static TOKEN_USER *
token_user(HANDLE token)
{
    DWORD size = 0;
    TOKEN_USER *user;

    (void)GetTokenInformation(token, TokenUser, NULL, 0, &size);
    if (!size) {
        path_error(GetLastError());
        return NULL;
    }
    user = malloc(size);
    if (user && !GetTokenInformation(token, TokenUser, user, size, &size)) {
        DWORD error = GetLastError();
        free(user);
        path_error(error);
        return NULL;
    }
    return user;
}

static bool
private_dacl(PACL dacl, PSID owner)
{
    SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
    DWORD system_sid[SECURITY_MAX_SID_SIZE / sizeof(DWORD) + 1u];
    DWORD admin_sid[SECURITY_MAX_SID_SIZE / sizeof(DWORD) + 1u];
    GENERIC_MAPPING mapping = {
        FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS
    };

    if (!dacl || !IsValidAcl(dacl) || !owner || !IsValidSid(owner) ||
        !InitializeSid(system_sid, &authority, 1u) ||
        !InitializeSid(admin_sid, &authority, 2u))
        return false;
    *GetSidSubAuthority(system_sid, 0u) = SECURITY_LOCAL_SYSTEM_RID;
    *GetSidSubAuthority(admin_sid, 0u) = SECURITY_BUILTIN_DOMAIN_RID;
    *GetSidSubAuthority(admin_sid, 1u) = DOMAIN_ALIAS_RID_ADMINS;
    for (DWORD i = 0u; i < dacl->AceCount; ++i) {
        ACE_HEADER *header;
        ACCESS_ALLOWED_ACE *ace;
        PSID sid;
        DWORD mask;
        size_t offset = offsetof(ACCESS_ALLOWED_ACE, SidStart);

        if (!GetAce(dacl, i, (void **)&header))
            return false;
        if ((header->AceFlags & INHERIT_ONLY_ACE) ||
            header->AceType == ACCESS_DENIED_ACE_TYPE)
            continue;
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE ||
            header->AceSize < offset + 8u)
            return false;
        ace = (ACCESS_ALLOWED_ACE *)header;
        mask = ace->Mask;
        MapGenericMask(&mask, &mapping);
        if (!(mask & ~(READ_CONTROL | SYNCHRONIZE | FILE_READ_ATTRIBUTES)))
            continue;
        sid = &ace->SidStart;
        if (GetSidLengthRequired(*GetSidSubAuthorityCount(sid)) > header->AceSize - offset ||
            !IsValidSid(sid))
            return false;
        if (!EqualSid(sid, owner) && !EqualSid(sid, system_sid) && !EqualSid(sid, admin_sid))
            return false;
    }
    return true;
}

int
snag_fd_privacy(int fd, struct snag_file_privacy *out)
{
    struct snag_file_privacy privacy = {0};
    HANDLE process = NULL, thread = NULL;
    TOKEN_USER *real = NULL, *effective = NULL;
    PSECURITY_DESCRIPTOR descriptor = NULL;
    PSID owner = NULL;
    PACL dacl = NULL;
    intptr_t handle;
    DWORD code, close_error = 0;
    int rc = -1, error;

    if (!out) {
        errno = EINVAL;
        return -1;
    }
    handle = _get_osfhandle(fd);
    if (handle == -1) {
        errno = EBADF;
        return -1;
    }
    code = GetSecurityInfo((HANDLE)handle, SE_FILE_OBJECT,
                           OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                           &owner, NULL, &dacl, NULL, &descriptor);
    if (code != ERROR_SUCCESS) {
        path_error(code);
        goto out;
    }
    if (!owner || !IsValidSid(owner)) {
        errno = EACCES;
        goto out;
    }
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &process)) {
        path_error(GetLastError());
        goto out;
    }
    if (!(real = token_user(process)))
        goto out;
    privacy.real_owner = EqualSid(owner, real->User.Sid) != 0;
    if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &thread)) {
        if (!(effective = token_user(thread)))
            goto out;
        privacy.effective_owner = EqualSid(owner, effective->User.Sid) != 0;
    } else if (GetLastError() == ERROR_NO_TOKEN) {
        privacy.effective_owner = privacy.real_owner;
    } else {
        path_error(GetLastError());
        goto out;
    }
    privacy.private_access = private_dacl(dacl, owner);
    rc = 0;
out:
    error = errno;
    if (thread && !CloseHandle(thread))
        close_error = GetLastError();
    if (process && !CloseHandle(process) && !close_error)
        close_error = GetLastError();
    free(real);
    free(effective);
    if (descriptor)
        LocalFree(descriptor);
    errno = error;
    if (rc == 0 && close_error)
        return path_error(close_error);
    if (rc == 0)
        *out = privacy;
    return rc;
}

char *
snag_realpath(const char *path)
{
    HANDLE handle = INVALID_HANDLE_VALUE;
    wchar_t *wide = NULL, *final = NULL;
    const wchar_t *body;
    char *result = NULL;
    size_t len, prefix = 0u;
    DWORD capacity, got;
    int chars, bytes, error;

    if (!path) {
        errno = EINVAL;
        return NULL;
    }
    len = strlen(path);
    if (len > SNAG_PATH_MAX_BYTES) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    if (!snag_utf8_valid((const unsigned char *)path, len, true)) {
        errno = EILSEQ;
        return NULL;
    }
    chars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    if (!chars) {
        path_error(GetLastError());
        return NULL;
    }
    wide = malloc((size_t)chars * sizeof(*wide));
    if (!wide)
        return NULL;
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, chars))
        goto native_error;
    handle = CreateFileW(wide, FILE_READ_ATTRIBUTES,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (handle == INVALID_HANDLE_VALUE)
        goto native_error;
    capacity = GetFinalPathNameByHandleW(handle, NULL, 0, FILE_NAME_NORMALIZED);
    if (!capacity)
        goto native_error;
    if (capacity > SNAG_PATH_MAX_BYTES + 9u) {
        errno = ENAMETOOLONG;
        goto out;
    }
    final = malloc(((size_t)capacity + 1u) * sizeof(*final));
    if (!final)
        goto out;
    got = GetFinalPathNameByHandleW(handle, final, capacity + 1u, FILE_NAME_NORMALIZED);
    if (!got)
        goto native_error;
    if (got > capacity) {
        errno = ENAMETOOLONG;
        goto out;
    }
    body = final;
    if (wcsncmp(body, L"\\\\?\\UNC\\", 8u) == 0) {
        body += 8u;
        prefix = 2u;
    } else if (wcsncmp(body, L"\\\\?\\", 4u) == 0) {
        body += 4u;
    }
    bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, body, -1,
                                NULL, 0, NULL, NULL);
    if (!bytes)
        goto native_error;
    if ((size_t)bytes + prefix > SNAG_PATH_MAX_BYTES + 1u) {
        errno = ENAMETOOLONG;
        goto out;
    }
    result = malloc((size_t)bytes + prefix);
    if (!result)
        goto out;
    if (prefix)
        result[0] = result[1] = '/';
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, body, -1,
                             result + prefix, bytes, NULL, NULL)) {
        DWORD native_error = GetLastError();
        free(result);
        result = NULL;
        path_error(native_error);
        goto out;
    }
    for (char *p = result; *p; ++p)
        if (*p == '\\')
            *p = '/';
    if (!snag_path_root_len(result)) {
        free(result);
        result = NULL;
        errno = EIO;
    }
    goto out;
native_error:
    path_error(GetLastError());
out:
    error = errno;
    if (handle != INVALID_HANDLE_VALUE && !CloseHandle(handle) && result) {
        DWORD native_error = GetLastError();
        free(result);
        result = NULL;
        path_error(native_error);
        error = errno;
    }
    free(final);
    free(wide);
    errno = error;
    return result;
}

static bool
path_separator(unsigned char c)
{
    return c == '/' || c == '\\';
}

size_t
snag_path_root_len(const char *path)
{
    const char *p;

    if (!path || !*path)
        return 0u;
    if (((path[0] >= 'A' && path[0] <= 'Z') ||
         (path[0] >= 'a' && path[0] <= 'z')) &&
        path[1] == ':' && path_separator((unsigned char)path[2]))
        return 3u;
    if (!path_separator((unsigned char)path[0]) ||
        !path_separator((unsigned char)path[1]) || !path[2] ||
        path_separator((unsigned char)path[2]))
        return 0u;
    /* Device namespaces are not ordinary UNC server/share roots. */
    if ((path[2] == '.' || path[2] == '?') &&
        path_separator((unsigned char)path[3]))
        return 0u;
    p = path + 2u;
    while (*p && !path_separator((unsigned char)*p))
        ++p;
    if (!*p || !*++p || path_separator((unsigned char)*p))
        return 0u;
    while (*p && !path_separator((unsigned char)*p))
        ++p;
    return (size_t)(p - path) + (*p != '\0');
}

int
snag_char_width(uint32_t cp)
{
    if (cp > 0x10ffffu || (cp >= 0xd800u && cp <= 0xdfffu))
        return -1;
    return uc_width(cp, "UTF-8");
}

int
snag_fd_cloexec(int fd)
{
    intptr_t handle = _get_osfhandle(fd);

    if (handle == -1) {
        errno = EBADF;
        return -1;
    }
    if (SetHandleInformation((HANDLE)handle, HANDLE_FLAG_INHERIT, 0))
        return 0;
    errno = GetLastError() == ERROR_INVALID_HANDLE ? EBADF : EIO;
    return -1;
}

int
snag_random_bytes(unsigned char *out, size_t len)
{
    HCRYPTPROV provider;
    bool ok = true;

    if (!len)
        return 0;
    if (!CryptAcquireContextW(&provider, NULL, NULL, PROV_RSA_FULL,
                             CRYPT_VERIFYCONTEXT | CRYPT_SILENT)) {
        errno = EIO;
        return -1;
    }
    while (len) {
        DWORD take = len > UINT32_MAX ? UINT32_MAX : (DWORD)len;
        if (!CryptGenRandom(provider, take, out)) {
            ok = false;
            break;
        }
        out += take;
        len -= take;
    }
    if (!CryptReleaseContext(provider, 0))
        ok = false;
    if (!ok)
        errno = EIO;
    return ok ? 0 : -1;
}

uint64_t
snag_time_ms(void)
{
    FILETIME now;
    uint64_t ticks;

    GetSystemTimeAsFileTime(&now);
    ticks = ((uint64_t)now.dwHighDateTime << 32u) | now.dwLowDateTime;
    /* FILETIME counts 100 ns from 1601; callers use milliseconds from 1970. */
    return ticks < UINT64_C(116444736000000000) ? 0u :
           (ticks - UINT64_C(116444736000000000)) / 10000u;
}

uint64_t
snag_monotonic_ms(void)
{
    LARGE_INTEGER counter, frequency;
    uint64_t ticks, hz;

    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0 ||
        !QueryPerformanceCounter(&counter) || counter.QuadPart < 0)
        return 0u;
    ticks = (uint64_t)counter.QuadPart;
    hz = (uint64_t)frequency.QuadPart;
    if (ticks / hz > UINT64_MAX / 1000u)
        return UINT64_MAX;
    return ticks / hz * 1000u +
           (uint64_t)((long double)(ticks % hz) * 1000.0L / (long double)hz);
}

int
snag_sync_file(int fd)
{
    return _commit(fd);
}

int
snag_sync_dir(int fd)
{
    intptr_t handle = _get_osfhandle(fd);
    BY_HANDLE_FILE_INFORMATION info;
    DWORD error;

    if (handle == -1) {
        errno = EBADF;
        return -1;
    }
    if (FlushFileBuffers((HANDLE)handle))
        return 0;
    error = GetLastError();
    if ((error == ERROR_INVALID_FUNCTION || error == ERROR_ACCESS_DENIED ||
         error == ERROR_INVALID_HANDLE) &&
        GetFileInformationByHandle((HANDLE)handle, &info) &&
        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        errno = ENOTSUP;
        return 1; /* Valid directory, but this OS cannot flush it this way. */
    }
    errno = error == ERROR_INVALID_HANDLE ? EBADF : EIO;
    return -1;
}

#else
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <sys/stat.h>

int
snag_fd_privacy(int fd, struct snag_file_privacy *out)
{
    struct stat st;

    if (!out) {
        errno = EINVAL;
        return -1;
    }
    if (fstat(fd, &st) < 0)
        return -1;
    out->real_owner = st.st_uid == getuid();
    out->effective_owner = st.st_uid == geteuid();
    out->private_access = !(st.st_mode & 077u);
    return 0;
}

char *
snag_realpath(const char *path)
{
    if (!path) {
        errno = EINVAL;
        return NULL;
    }
    return realpath(path, NULL);
}

size_t
snag_path_root_len(const char *path)
{
    return path && path[0] == '/' ? 1u : 0u;
}

int
snag_char_width(uint32_t cp)
{
    return cp <= (uint32_t)WCHAR_MAX ? wcwidth((wchar_t)cp) : -1;
}

int
snag_fd_cloexec(int fd)
{
    int flags = fcntl(fd, F_GETFD);

    return flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0 ? -1 : 0;
}

int
snag_random_bytes(unsigned char *out, size_t len)
{
    size_t done = 0;
    int fd;

    if (!len)
        return 0;
    fd = open("/dev/urandom", O_RDONLY
#ifdef O_CLOEXEC
        | O_CLOEXEC
#endif
#ifdef O_NOFOLLOW
        | O_NOFOLLOW
#endif
    );
    if (fd < 0)
        return -1;
    while (done < len) {
        size_t take = len - done > (size_t)SSIZE_MAX ? (size_t)SSIZE_MAX : len - done;
        ssize_t n = read(fd, out + done, take);
        if (n > 0) {
            done += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        int error = n == 0 ? EIO : errno;
        (void)close(fd);
        errno = error;
        return -1;
    }
    return close(fd);
}

uint64_t
snag_time_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_REALTIME, &ts) < 0)
        return 0;
    if ((uint64_t)ts.tv_sec > UINT64_MAX / 1000u)
        return UINT64_MAX;
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

uint64_t
snag_monotonic_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return 0u;
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

int
snag_sync_file(int fd)
{
#if defined(__APPLE__)
    return fsync(fd);
#else
    if (fdatasync(fd) == 0)
        return 0;
    if (errno != EINVAL && errno != ENOSYS)
        return -1;
    return fsync(fd);
#endif
}

int
snag_sync_dir(int fd)
{
    if (fsync(fd) == 0)
        return 0;
    if (errno == EINVAL || errno == ENOTSUP || errno == EROFS)
        return 1;
    return -1;
}
#endif

int
snag_write_full(int fd, const void *data, size_t len)
{
    const unsigned char *p = data;
    size_t done = 0;

    while (done < len) {
#ifdef _WIN32
        unsigned int take = len - done > INT_MAX ? INT_MAX : (unsigned int)(len - done);
        int n = _write(fd, p + done, take);
#else
        size_t take = len - done > (size_t)SSIZE_MAX ? (size_t)SSIZE_MAX : len - done;
        ssize_t n = write(fd, p + done, take);
#endif
        if (n > 0) {
            done += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n == 0)
            errno = EIO;
        return -1;
    }
    return 0;
}

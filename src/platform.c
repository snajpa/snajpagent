/* SPDX-License-Identifier: GPL-2.0-only */
#include "base.h"

#include <errno.h>
#include <limits.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>
#include <io.h>
#include <uniwidth.h>

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

/* SPDX-License-Identifier: GPL-2.0-only */
#include "net.h"
#include "base.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

int
snag_socket_addresses(const char *host, const char *service,
                       const struct addrinfo *hints, struct addrinfo **out)
{
    int rc = getaddrinfo(host, service, hints, out);
#ifdef _WIN32
    /* Minimal/old Windows environments may lack localhost resolver entries. */
    if (rc && host && !_stricmp(host, "localhost")) {
        struct addrinfo loopback = hints ? *hints : (struct addrinfo){0};
        loopback.ai_flags &= ~AI_PASSIVE;
        rc = getaddrinfo(NULL, service, &loopback, out);
    }
#endif
    return rc;
}

#ifdef _WIN32
int
snag_socket_error(int code)
{
    switch (code) {
    case WSAEWOULDBLOCK: errno = EAGAIN; break;
    case WSAEINPROGRESS: errno = EINPROGRESS; break;
    case WSAEINTR: errno = EINTR; break;
    case WSAEINVAL: errno = EINVAL; break;
    case WSAENOTSOCK: errno = EBADF; break;
    case WSAEMFILE: errno = EMFILE; break;
    case WSAENOBUFS: errno = ENOMEM; break;
    case WSAEACCES: errno = EACCES; break;
    case WSAEADDRINUSE: errno = EADDRINUSE; break;
    case WSAEADDRNOTAVAIL: errno = EADDRNOTAVAIL; break;
    case WSAECONNREFUSED: errno = ECONNREFUSED; break;
    case WSAECONNRESET: errno = ECONNRESET; break;
    case WSAECONNABORTED: errno = ECONNABORTED; break;
    case WSAETIMEDOUT: errno = ETIMEDOUT; break;
    case WSAEHOSTUNREACH: errno = EHOSTUNREACH; break;
    case WSAENETUNREACH: errno = ENETUNREACH; break;
    case WSAENOTCONN: errno = ENOTCONN; break;
    default: errno = EIO; break;
    }
    return -1;
}

int
snag_network_init(void)
{
    WSADATA data;
    int error = WSAStartup(MAKEWORD(2, 2), &data);

    return error ? snag_socket_error(error) : 0;
}

void
snag_network_free(void)
{
    (void)WSACleanup();
}

static int
nonblocking(snag_socket fd)
{
    unsigned long yes = 1;
    DWORD flags;

    if (!GetHandleInformation((HANDLE)fd, &flags) || (flags & HANDLE_FLAG_INHERIT)) {
        errno = EACCES;
        return -1;
    }
    return ioctlsocket(fd, FIONBIO, &yes) < 0 ? snag_socket_error(WSAGetLastError()) : 0;
}

int
snag_socket_close(snag_socket fd)
{
    return closesocket(fd) < 0 ? snag_socket_error(WSAGetLastError()) : 0;
}

static int
option(snag_socket fd, int level, int key)
{
    int one = 1;

    return setsockopt(fd, level, key, (char *)&one, sizeof(one)) < 0 ?
           snag_socket_error(WSAGetLastError()) : 0;
}

int
snag_socket_reuse(snag_socket fd)
{
    return option(fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE);
}

#else
#include <fcntl.h>
#include <unistd.h>

int
snag_socket_error(int code)
{
    errno = code;
    return -1;
}

int
snag_network_init(void)
{
    return 0;
}

void
snag_network_free(void)
{
}

static int
nonblocking(snag_socket fd)
{
    int flags = fcntl(fd, F_GETFL);

    return flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ||
           snag_fd_cloexec(fd) < 0 ? -1 : 0;
}

int
snag_socket_close(snag_socket fd)
{
    return close(fd);
}

static int
option(snag_socket fd, int level, int key)
{
    int one = 1;

    return setsockopt(fd, level, key, &one, sizeof(one));
}

int
snag_socket_reuse(snag_socket fd)
{
    return option(fd, SOL_SOCKET, SO_REUSEADDR);
}
#endif

static int
failed(void)
{
#ifdef _WIN32
    return snag_socket_error(WSAGetLastError());
#else
    return -1;
#endif
}

static snag_socket
prepare_socket(snag_socket fd)
{
    if (fd == SNAG_SOCKET_INVALID) {
        (void)failed();
        return fd;
    }
    if (nonblocking(fd) < 0) {
        int saved = errno;
        (void)snag_socket_close(fd);
        errno = saved;
        return SNAG_SOCKET_INVALID;
    }
    return fd;
}

snag_socket
snag_socket_open(int family, int type, int protocol)
{
#ifdef _WIN32
    return prepare_socket(WSASocketW(family, type, protocol, NULL, 0,
                                     WSA_FLAG_NO_HANDLE_INHERIT));
#else
    return prepare_socket(socket(family, type, protocol));
#endif
}

snag_socket
snag_socket_accept(snag_socket listener)
{
    return prepare_socket(accept(listener, NULL, NULL));
}

void
snag_socket_nodelay(snag_socket fd)
{
    (void)option(fd, IPPROTO_TCP, TCP_NODELAY);
}

int
snag_socket_bind(snag_socket fd, const struct sockaddr *address, size_t size)
{
    if (size > INT_MAX) {
        errno = EINVAL;
        return -1;
    }
    return bind(fd, address, (int)size) < 0 ? failed() : 0;
}

int
snag_socket_listen(snag_socket fd, int backlog)
{
    return listen(fd, backlog) < 0 ? failed() : 0;
}

int
snag_socket_connect(snag_socket fd, const struct sockaddr *address, size_t size)
{
    if (size > INT_MAX) {
        errno = EINVAL;
        return -1;
    }
    if (connect(fd, address, (int)size) == 0)
        return 0;
#ifdef _WIN32
    if (WSAGetLastError() == WSAEWOULDBLOCK) {
        errno = EINPROGRESS;
        return -1;
    }
#endif
    return failed();
}

int
snag_socket_connected(snag_socket fd)
{
    int error = 0;
#ifdef _WIN32
    int size = sizeof(error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&error, &size) < 0)
#else
    socklen_t size = sizeof(error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) < 0)
#endif
        return failed();
    return error ? snag_socket_error(error) : 0;
}

ssize_t
snag_socket_send(snag_socket fd, const void *data, size_t size)
{
    if (size > INT_MAX) {
        errno = EINVAL;
        return -1;
    }
    ssize_t n = send(fd, data, (int)size, 0);
    return n < 0 ? failed() : n;
}

ssize_t
snag_socket_recv(snag_socket fd, void *data, size_t size)
{
    if (size > INT_MAX) {
        errno = EINVAL;
        return -1;
    }
    ssize_t n = recv(fd, data, (int)size, 0);
    return n < 0 ? failed() : n;
}

int
snag_socket_poll(snag_socket_event *events, size_t count, int timeout_ms)
{
#ifdef _WIN32
    fd_set reads, writes, errors;
    struct timeval timeout;
    size_t live = 0;

    if (count > FD_SETSIZE || timeout_ms < -1) {
        errno = EINVAL;
        return -1;
    }
    FD_ZERO(&reads);
    FD_ZERO(&writes);
    FD_ZERO(&errors);
    for (size_t i = 0; i < count; ++i) {
        events[i].revents = 0;
        if (events[i].fd == SNAG_SOCKET_INVALID)
            continue;
        if (events[i].events & SNAG_NET_READ)
            FD_SET(events[i].fd, &reads);
        if (events[i].events & SNAG_NET_WRITE)
            FD_SET(events[i].fd, &writes);
        FD_SET(events[i].fd, &errors);
        ++live;
    }
    if (!live)
        return snag_wakeup_wait(SNAG_WAKE_INVALID, timeout_ms);
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = timeout_ms % 1000 * 1000;
    int rc = select(0, &reads, &writes, &errors, timeout_ms < 0 ? NULL : &timeout);
    if (rc <= 0)
        return rc < 0 ? failed() : 0;
    rc = 0;
    for (size_t i = 0; i < count; ++i) {
        if (events[i].fd == SNAG_SOCKET_INVALID)
            continue;
        if (FD_ISSET(events[i].fd, &reads))
            events[i].revents |= SNAG_NET_READ;
        if (FD_ISSET(events[i].fd, &writes))
            events[i].revents |= SNAG_NET_WRITE;
        if (FD_ISSET(events[i].fd, &errors))
            events[i].revents |= SNAG_NET_ERROR;
        rc += events[i].revents != 0;
    }
    return rc;
#else
    return poll(events, count, timeout_ms);
#endif
}

/* SPDX-License-Identifier: GPL-2.0-only */
#include "wake.h"
#include "base.h"

#include <errno.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>

static int
socket_error(int code)
{
    switch (code) {
    case WSAEWOULDBLOCK: errno = EAGAIN; break;
    case WSAEINTR: errno = EINTR; break;
    case WSAEINVAL: errno = EINVAL; break;
    case WSAENOTSOCK: errno = EBADF; break;
    case WSAEMFILE: errno = EMFILE; break;
    case WSAENOBUFS: errno = ENOMEM; break;
    case WSAEACCES: errno = EACCES; break;
    default: errno = EIO; break;
    }
    return -1;
}

void
snag_wakeup_close(snag_wake_fd pair[2])
{
    bool owned = pair[0] != SNAG_WAKE_INVALID || pair[1] != SNAG_WAKE_INVALID;
    int saved = errno;

    for (size_t i = 0; i < 2u; ++i) {
        if (pair[i] != SNAG_WAKE_INVALID)
            (void)closesocket(pair[i]);
        pair[i] = SNAG_WAKE_INVALID;
    }
    if (owned)
        (void)WSACleanup();
    errno = saved;
}

int
snag_wakeup_create(snag_wake_fd pair[2])
{
    WSADATA data;
    SOCKET listener = INVALID_SOCKET, writer = INVALID_SOCKET, reader = INVALID_SOCKET;
    struct sockaddr_in address = {0}, peer = {0}, local = {0};
    int size = sizeof(address), yes = 1, error;
    unsigned long nonblocking = 1;

    pair[0] = pair[1] = SNAG_WAKE_INVALID;
    error = WSAStartup(MAKEWORD(2, 2), &data);
    if (error)
        return socket_error(error);
    listener = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_NO_HANDLE_INHERIT);
    writer = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_NO_HANDLE_INHERIT);
    if (listener == INVALID_SOCKET || writer == INVALID_SOCKET)
        goto fail;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (char *)&yes, sizeof(yes)) < 0 ||
        bind(listener, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        getsockname(listener, (struct sockaddr *)&address, &size) < 0 ||
        listen(listener, 1) < 0 || connect(writer, (struct sockaddr *)&address, sizeof(address)) < 0)
        goto fail;
    size = sizeof(peer);
    reader = accept(listener, (struct sockaddr *)&peer, &size);
    if (reader == INVALID_SOCKET)
        goto fail;
    size = sizeof(local);
    if (getsockname(writer, (struct sockaddr *)&local, &size) < 0)
        goto fail;
    if (peer.sin_family != AF_INET || peer.sin_addr.s_addr != local.sin_addr.s_addr ||
        peer.sin_port != local.sin_port) {
        WSASetLastError(WSAEACCES);
        goto fail;
    }
    DWORD inherited;
    if (!GetHandleInformation((HANDLE)reader, &inherited) ||
        (inherited & HANDLE_FLAG_INHERIT)) {
        WSASetLastError(WSAEACCES);
        goto fail;
    }
    if (setsockopt(writer, IPPROTO_TCP, TCP_NODELAY, (char *)&yes, sizeof(yes)) < 0 ||
        ioctlsocket(reader, FIONBIO, &nonblocking) < 0 ||
        ioctlsocket(writer, FIONBIO, &nonblocking) < 0)
        goto fail;
    (void)closesocket(listener);
    pair[0] = reader;
    pair[1] = writer;
    return 0;
fail:
    error = WSAGetLastError();
    if (listener != INVALID_SOCKET)
        (void)closesocket(listener);
    if (writer != INVALID_SOCKET)
        (void)closesocket(writer);
    if (reader != INVALID_SOCKET)
        (void)closesocket(reader);
    (void)WSACleanup();
    return socket_error(error);
}

void
snag_wakeup_send(snag_wake_fd writer)
{
    int saved = errno;
    while (send(writer, "", 1, 0) < 0 && WSAGetLastError() == WSAEINTR)
        ;
    errno = saved;
}

void
snag_wakeup_drain(snag_wake_fd reader)
{
    int saved = errno;
    char bytes[64];

    while (recv(reader, bytes, sizeof(bytes), 0) > 0)
        ;
    errno = saved;
}

int
snag_wakeup_wait(snag_wake_fd reader, int timeout_ms)
{
    fd_set ready;
    struct timeval timeout;

    if (timeout_ms < -1 || (reader == SNAG_WAKE_INVALID && timeout_ms < 0)) {
        errno = EINVAL;
        return -1;
    }
    if (reader == SNAG_WAKE_INVALID)
        return snag_sleep_ms((unsigned int)timeout_ms);
    FD_ZERO(&ready);
    FD_SET(reader, &ready);
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;
    int rc = select(0, &ready, NULL, NULL, timeout_ms < 0 ? NULL : &timeout);
    return rc < 0 ? socket_error(WSAGetLastError()) : rc;
}
#else
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

void
snag_wakeup_close(snag_wake_fd pair[2])
{
    int saved = errno;

    for (size_t i = 0; i < 2u; ++i) {
        if (pair[i] != SNAG_WAKE_INVALID)
            (void)close(pair[i]);
        pair[i] = SNAG_WAKE_INVALID;
    }
    errno = saved;
}

int
snag_wakeup_create(snag_wake_fd pair[2])
{
    pair[0] = pair[1] = SNAG_WAKE_INVALID;
    if (pipe(pair) < 0)
        return -1;
    for (size_t i = 0; i < 2u; ++i) {
        int flags = fcntl(pair[i], F_GETFL);
        if (flags < 0 || fcntl(pair[i], F_SETFL, flags | O_NONBLOCK) < 0 ||
            snag_fd_cloexec(pair[i]) < 0) {
            snag_wakeup_close(pair);
            return -1;
        }
    }
    return 0;
}

void
snag_wakeup_send(snag_wake_fd writer)
{
    int saved = errno;
    char byte = 0;

    while (write(writer, &byte, 1u) < 0 && errno == EINTR)
        ;
    errno = saved;
}

void
snag_wakeup_drain(snag_wake_fd reader)
{
    int saved = errno;
    char bytes[64];

    while (read(reader, bytes, sizeof(bytes)) > 0)
        ;
    errno = saved;
}

int
snag_wakeup_wait(snag_wake_fd reader, int timeout_ms)
{
    struct pollfd ready = {reader, POLLIN, 0};

    if (timeout_ms < -1 || (reader == SNAG_WAKE_INVALID && timeout_ms < 0)) {
        errno = EINVAL;
        return -1;
    }
    int rc = poll(&ready, 1u, timeout_ms);
    if (rc > 0 && (ready.revents & POLLNVAL)) {
        errno = EBADF;
        return -1;
    }
    return rc;
}
#endif

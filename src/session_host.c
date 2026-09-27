/* SPDX-License-Identifier: GPL-2.0-only */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "session_host.h"
#include "base.h"
#include "fs.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#if defined(__linux__) && !defined(_WIN32)
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#define SNAG_SESSION_NATIVE 1
#endif

bool
snag_session_host_supported(void)
{
#ifdef SNAG_SESSION_NATIVE
    return true;
#else
    return false;
#endif
}

size_t
snag_session_packet_length(const struct snag_session_packet *packet)
{
    const unsigned char *p = packet->bytes + 4u;
    return (size_t)p[0] | (size_t)p[1] << 8u | (size_t)p[2] << 16u | (size_t)p[3] << 24u;
}

enum snag_session_message
snag_session_packet_type(const struct snag_session_packet *packet)
{
    return (enum snag_session_message)packet->bytes[3];
}

int
snag_session_packet_set(struct snag_session_packet *packet, enum snag_session_message type,
                         const void *data, size_t length)
{
    if (type < SNAG_SESSION_RESERVE || type > SNAG_SESSION_SUSPEND ||
        length > SNAG_SESSION_FRAME_MAX || (length && !data)) return snag_errno(EINVAL);
    packet->bytes[0] = 'S';
    packet->bytes[1] = 'A';
    packet->bytes[2] = 1u;
    packet->bytes[3] = (unsigned char)type;
    for (size_t i = 0u; i < 4u; ++i) packet->bytes[4u + i] = (unsigned char)(length >> (8u * i));
    if (length) memcpy(packet->bytes + SNAG_SESSION_HEADER, data, length);
    packet->used = SNAG_SESSION_HEADER + length;
    packet->offset = 0u;
    return 0;
}

#ifdef SNAG_SESSION_NATIVE
static int
private_directory(int fd)
{
    struct stat st;
    if (fstat(fd, &st) < 0) return -1;
    if (!S_ISDIR(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 0077u))
        return snag_errno(EACCES);
    return 0;
}

static int
stream_prepare(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 || snag_fd_cloexec(fd) < 0)
        return -1;
#ifdef SO_NOSIGPIPE
    int yes = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes)) < 0) return -1;
#endif
    return 0;
}

static int
stream_socket(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (stream_prepare(fd) < 0) {
        int saved = errno;
        (void)close(fd);
        return snag_errno(saved);
    }
    return fd;
}

static int
same_user(int fd)
{
    struct ucred peer;
    socklen_t size = sizeof(peer);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) < 0) return -1;
    if (size != sizeof(peer) || peer.uid != geteuid()) return snag_errno(EACCES);
    return 0;
}

static int
endpoint_address(int dir_fd, const char *dir_path, struct sockaddr_un *address)
{
    int count;
    if (private_directory(dir_fd) < 0) return -1;
    memset(address, 0, sizeof(*address));
    address->sun_family = AF_UNIX;
    /* A held directory avoids both sun_path's pathname limit and races through
     * parent names. No process-wide chdir in the multithreaded owner. */
    (void)dir_path;
    count = snprintf(address->sun_path, sizeof(address->sun_path),
                      "/proc/self/fd/%d/%s", dir_fd, SNAG_SESSION_ENDPOINT);
    if (count < 0 || (size_t)count >= sizeof(address->sun_path)) return snag_errno(ENAMETOOLONG);
    return 0;
}

static int
endpoint_stat(int dir_fd, struct stat *st)
{
    if (fstatat(dir_fd, SNAG_SESSION_ENDPOINT, st, AT_SYMLINK_NOFOLLOW) < 0) return -1;
    if (!S_ISSOCK(st->st_mode) || st->st_uid != geteuid() || (st->st_mode & 0077u))
        return snag_errno(EACCES);
    return 0;
}

int
snag_session_endpoint_connect(int dir_fd, const char *dir_path)
{
    struct sockaddr_un address;
    struct stat st;
    int fd, saved;
    if (endpoint_address(dir_fd, dir_path, &address) < 0 || endpoint_stat(dir_fd, &st) < 0)
        return -1;
    fd = stream_socket();
    if (fd < 0) return -1;
    /* A private local endpoint connects immediately. A full listen backlog is
     * a refusal for this attempt, not an unbounded engine-side wait. */
    if (connect(fd, (const struct sockaddr *)&address, sizeof(address)) == 0 && same_user(fd) == 0)
        return fd;
    saved = errno;
    (void)close(fd);
    return snag_errno(saved);
}

static int
remove_stale_endpoint(int dir_fd, const char *dir_path)
{
    struct stat before, after;
    int probe;
    if (endpoint_stat(dir_fd, &before) < 0) return errno == ENOENT ? 0 : -1;
    probe = snag_session_endpoint_connect(dir_fd, dir_path);
    if (probe >= 0) {
        (void)close(probe);
        return snag_errno(EADDRINUSE);
    }
    if (errno != ECONNREFUSED) return -1;
    if (endpoint_stat(dir_fd, &after) < 0) return -1;
    if (before.st_dev != after.st_dev || before.st_ino != after.st_ino) return snag_errno(ESTALE);
    return unlinkat(dir_fd, SNAG_SESSION_ENDPOINT, 0);
}

int
snag_session_listener_open(struct snag_session_listener *listener, int dir_fd,
                           const char *dir_path, int lock_fd)
{
    struct sockaddr_un address;
    struct stat st;
    int fd, saved;
    *listener = (struct snag_session_listener){.fd = -1, .dir_fd = -1};
    /* The writer lock is borrowed, never duplicated or closed. Its ownership
     * is established by the session store before entering this host boundary. */
    if (lock_fd < 0 || fstat(lock_fd, &st) < 0) return snag_errno(EBADF);
    if (!S_ISREG(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 0077u))
        return snag_errno(EACCES);
    if (endpoint_address(dir_fd, dir_path, &address) < 0 ||
        remove_stale_endpoint(dir_fd, dir_path) < 0) return -1;
    listener->dir_fd = dup(dir_fd);
    if (listener->dir_fd < 0) return -1;
    if (snag_fd_cloexec(listener->dir_fd) < 0) goto fail;
    fd = stream_socket();
    if (fd < 0) goto fail;
    listener->fd = fd;
    if (bind(fd, (const struct sockaddr *)&address, sizeof(address)) < 0) goto fail;
    if (fstatat(dir_fd, SNAG_SESSION_ENDPOINT, &st, AT_SYMLINK_NOFOLLOW) < 0) goto fail;
    listener->device = (uint64_t)st.st_dev;
    listener->inode = (uint64_t)st.st_ino;
    if (fchmodat(dir_fd, SNAG_SESSION_ENDPOINT, 0600, 0) < 0 || listen(fd, 8) < 0) goto fail;
    return 0;
fail:
    saved = errno;
    snag_session_listener_close(listener);
    return snag_errno(saved);
}

void
snag_session_listener_close(struct snag_session_listener *listener)
{
    struct stat st;
    if (listener->dir_fd >= 0 &&
        fstatat(listener->dir_fd, SNAG_SESSION_ENDPOINT, &st, AT_SYMLINK_NOFOLLOW) == 0 &&
        (uint64_t)st.st_dev == listener->device && (uint64_t)st.st_ino == listener->inode)
        (void)unlinkat(listener->dir_fd, SNAG_SESSION_ENDPOINT, 0);
    if (listener->fd >= 0) (void)close(listener->fd);
    if (listener->dir_fd >= 0) (void)close(listener->dir_fd);
    *listener = (struct snag_session_listener){.fd = -1, .dir_fd = -1};
}

int
snag_session_listener_accept(const struct snag_session_listener *listener)
{
    int fd = accept(listener->fd, NULL, NULL);
    if (fd < 0) return -1;
    if (stream_prepare(fd) < 0 || same_user(fd) < 0) {
        int saved = errno;
        (void)close(fd);
        return snag_errno(saved);
    }
    return fd;
}

int
snag_session_stream_pair(int fds[2])
{
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) < 0) return -1;
    if (stream_prepare(pair[0]) < 0 || stream_prepare(pair[1]) < 0) {
        int saved = errno;
        (void)close(pair[0]);
        (void)close(pair[1]);
        return snag_errno(saved);
    }
    fds[0] = pair[0];
    fds[1] = pair[1];
    return 0;
}

int
snag_session_packet_read(int fd, struct snag_session_packet *packet)
{
    size_t target = SNAG_SESSION_HEADER;
    for (;;) {
        if (packet->used >= SNAG_SESSION_HEADER) {
            if (packet->bytes[0] != 'S' || packet->bytes[1] != 'A' || packet->bytes[2] != 1u ||
                packet->bytes[3] < SNAG_SESSION_RESERVE ||
                packet->bytes[3] > SNAG_SESSION_SUSPEND ||
                snag_session_packet_length(packet) > SNAG_SESSION_FRAME_MAX)
                return snag_errno(EPROTO);
            target += snag_session_packet_length(packet);
        }
        if (packet->used == target) return 1;
        if (packet->used > target) return snag_errno(EPROTO);
        ssize_t count = recv(fd, packet->bytes + packet->used, target - packet->used, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        if (count <= 0) return count ? -1 : snag_errno(ECONNRESET);
        packet->used += (size_t)count;
        target = SNAG_SESSION_HEADER;
    }
}

int
snag_session_packet_write(int fd, struct snag_session_packet *packet)
{
    if (packet->used < SNAG_SESSION_HEADER || packet->used > sizeof(packet->bytes) ||
        packet->offset > packet->used) return snag_errno(EINVAL);
    while (packet->offset < packet->used) {
        int flags = 0;
#ifdef MSG_NOSIGNAL
        flags = MSG_NOSIGNAL;
#endif
        ssize_t count = send(fd, packet->bytes + packet->offset,
                             packet->used - packet->offset, flags);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        if (count <= 0) return count ? -1 : snag_errno(EPIPE);
        packet->offset += (size_t)count;
    }
    return 1;
}
#else
int
snag_session_listener_open(struct snag_session_listener *listener, int dir_fd,
                           const char *dir_path, int lock_fd)
{
    (void)dir_fd;
    (void)dir_path;
    (void)lock_fd;
    *listener = (struct snag_session_listener){.fd = -1, .dir_fd = -1};
    return snag_errno(ENOTSUP);
}

void
snag_session_listener_close(struct snag_session_listener *listener)
{
    (void)listener;
}

int
snag_session_listener_accept(const struct snag_session_listener *listener)
{
    (void)listener;
    return snag_errno(ENOTSUP);
}

int
snag_session_endpoint_connect(int dir_fd, const char *dir_path)
{
    (void)dir_fd;
    (void)dir_path;
    return snag_errno(ENOTSUP);
}

int
snag_session_stream_pair(int fds[2])
{
    (void)fds;
    return snag_errno(ENOTSUP);
}

int
snag_session_packet_read(int fd, struct snag_session_packet *packet)
{
    (void)fd;
    (void)packet;
    return snag_errno(ENOTSUP);
}

int
snag_session_packet_write(int fd, struct snag_session_packet *packet)
{
    (void)fd;
    (void)packet;
    return snag_errno(ENOTSUP);
}
#endif /* SNAG_SESSION_NATIVE */

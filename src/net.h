/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_NET_H
#define SNAJPAGENT_NET_H

#include "wake.h"
#include <stddef.h>
#include <sys/types.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef FD_SETSIZE
#define FD_SETSIZE 256
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
typedef struct {
    snag_wake_fd fd;
    short events, revents;
} snag_socket_event;
#define SNAG_NET_READ 1
#define SNAG_NET_WRITE 4
#define SNAG_NET_ERROR 8
#define SNAG_NET_HUP 16
#define SNAG_NET_INVALID 32
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
typedef struct pollfd snag_socket_event;
#define SNAG_NET_READ POLLIN
#define SNAG_NET_WRITE POLLOUT
#define SNAG_NET_ERROR POLLERR
#define SNAG_NET_HUP POLLHUP
#define SNAG_NET_INVALID POLLNVAL
#endif

typedef snag_wake_fd snag_socket;
#define SNAG_SOCKET_INVALID ((snag_socket)-1)
int snag_network_init(void);
void snag_network_free(void);
int snag_socket_error(int code);
snag_socket snag_socket_open(int family, int type, int protocol);
snag_socket snag_socket_accept(snag_socket listener);
int snag_socket_close(snag_socket fd);
void snag_socket_nodelay(snag_socket fd);
int snag_socket_reuse(snag_socket fd);
int snag_socket_bind(snag_socket fd, const struct sockaddr *address, size_t size);
int snag_socket_listen(snag_socket fd, int backlog);
int snag_socket_connect(snag_socket fd, const struct sockaddr *address, size_t size);
int snag_socket_connected(snag_socket fd);
ssize_t snag_socket_send(snag_socket fd, const void *data, size_t size);
ssize_t snag_socket_recv(snag_socket fd, void *data, size_t size);
int snag_socket_poll(snag_socket_event *events, size_t count, int timeout_ms);

#endif

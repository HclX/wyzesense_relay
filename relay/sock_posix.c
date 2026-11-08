#include "sock_posix.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0  // lwIP has no SIGPIPE
#endif

void sock_link_init(sock_link_t *s, uint32_t connect_timeout_ms)
{
    memset(s, 0, sizeof *s);
    s->fd = -1;
    s->connect_timeout_ms = connect_timeout_ms;
}

void sock_link_set_nonblocking(sock_link_t *s, bool nonblocking)
{
    s->nonblocking = nonblocking;
}

static bool connect_one(sock_link_t *s, const struct addrinfo *a)
{
    int fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
    if (fd < 0) {
        return false;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    if (connect(fd, a->ai_addr, a->ai_addrlen) == 0) {
        s->fd = fd;
        s->connecting = false;
        return true;
    }
    if (errno != EINPROGRESS) {
        close(fd);
        return false;
    }
    if (s->nonblocking) {
        if (a->ai_next != NULL) {
            fd_set w;
            FD_ZERO(&w);
            FD_SET(fd, &w);
            struct timeval tv = {0, 5000};
            int err = 0;
            socklen_t len = sizeof err;
            if (select(fd + 1, NULL, &w, NULL, &tv) == 1) {
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
                    close(fd);
                    return false;
                }
                s->fd = fd;
                s->connecting = false;
                return true;
            }
        }
        s->fd = fd;
        s->connecting = true;
        s->tx_len = 0;
        return true;
    }
    fd_set w;
    FD_ZERO(&w);
    FD_SET(fd, &w);
    struct timeval tv = {(long)(s->connect_timeout_ms / 1000), (long)(s->connect_timeout_ms % 1000) * 1000};
    int err = 0;
    socklen_t len = sizeof err;
    if (select(fd + 1, NULL, &w, NULL, &tv) != 1 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err) {
        close(fd);
        return false;
    }
    s->fd = fd;
    s->connecting = false;
    return true;
}

int sock_link_finish_connect(sock_link_t *s)
{
    if (s->fd < 0) {
        return -1;
    }
    if (!s->connecting) {
        return 0;
    }
    int err = 0;
    socklen_t len = sizeof err;
    if (getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
        sock_link_close(s);
        return -1;
    }
    s->connecting = false;
    size_t queued = s->tx_len;
    s->tx_len = 0;
    if (queued && !sock_link_send(s, s->tx_buf, queued)) {
        sock_link_close(s);
        return -1;
    }
    return 0;
}

bool sock_link_connect(sock_link_t *s, const char *host, uint16_t port)
{
    char p[8];
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(p, sizeof p, "%u", port);
    sock_link_close(s);
    if (getaddrinfo(host, p, &hints, &res) != 0) {
        return false;
    }
    for (const struct addrinfo *a = res; a && s->fd < 0; a = a->ai_next) {
        connect_one(s, a);
    }
    freeaddrinfo(res);
    return s->fd >= 0;
}

bool sock_link_send(sock_link_t *s, const uint8_t *data, size_t len)
{
    if (s->connecting) {
        if (s->fd < 0 || s->tx_len + len > sizeof s->tx_buf) {
            return false;
        }
        memcpy(s->tx_buf + s->tx_len, data, len);
        s->tx_len += len;
        return true;
    }
    while (len && s->fd >= 0) {
        ssize_t n = send(s->fd, data, len, MSG_NOSIGNAL);
        if (n > 0) {
            data += n, len -= (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            fd_set w;
            FD_ZERO(&w);
            FD_SET(s->fd, &w);
            struct timeval tv = {5, 0};
            if (select(s->fd + 1, NULL, &w, NULL, &tv) == 1) {
                continue;
            }
        }
        return false;
    }
    return len == 0;
}

int sock_link_recv(sock_link_t *s, uint8_t *buf, size_t cap)
{
    if (s->fd < 0) {
        return -1;
    }
    ssize_t n = recv(s->fd, buf, cap, 0);
    if (n > 0) {
        return (int)n;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        return 0;
    }
    return -1;
}

void sock_link_close(sock_link_t *s)
{
    if (s->fd >= 0) {
        close(s->fd);
    }
    s->fd = -1;
    s->connecting = false;
    s->tx_len = 0;
}

bool sock_link_relay_connect(void *ctx, const char *host, uint16_t port)
{
    return sock_link_connect(ctx, host, port);
}

bool sock_link_relay_send(void *ctx, const uint8_t *data, size_t len)
{
    return sock_link_send(ctx, data, len);
}

void sock_link_relay_close(void *ctx)
{
    sock_link_close(ctx);
}

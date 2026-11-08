// The relay's TCP link over BSD sockets: the PC (esp32/host) and ESP-IDF (lwIP) share it. A connect that does not
// complete in its timeout fails (a server that is down is retried by the relay later), sends are complete or fail.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int fd;  // -1 when closed
    uint32_t connect_timeout_ms;
    bool nonblocking;
    bool connecting;
    uint8_t tx_buf[600];
    size_t tx_len;
} sock_link_t;

void sock_link_init(sock_link_t *s, uint32_t connect_timeout_ms);
void sock_link_set_nonblocking(sock_link_t *s, bool nonblocking);
bool sock_link_connect(sock_link_t *s, const char *host, uint16_t port);
// Completes an async EINPROGRESS connect when fd becomes writable: 0 ready (or still connecting), -1 failed.
int sock_link_finish_connect(sock_link_t *s);
bool sock_link_send(sock_link_t *s, const uint8_t *data, size_t len);
// what has come, up to cap: > 0 bytes, 0 nothing yet, -1 closed or failed
int sock_link_recv(sock_link_t *s, uint8_t *buf, size_t cap);
void sock_link_close(sock_link_t *s);

// relay_io_t's TCP callbacks on a sock_link_t (ctx)
bool sock_link_relay_connect(void *ctx, const char *host, uint16_t port);
bool sock_link_relay_send(void *ctx, const uint8_t *data, size_t len);
void sock_link_relay_close(void *ctx);

// The relay between a gateway's host link (a UART) and wyzesense2mqtt-rs's dongle bridge (a WebSocket client of its
// /ws/bridge, as tools/ws_bridge.py and its dongle_bridge are): each binary message carries host-protocol bytes as
// they are. Portable C with no I/O of its own (issue #22): the platform (the host program in esp32/host, the
// ESP-IDF firmware later) owns the loop, the socket and the UART, and calls in on each event; the relay calls back
// through relay_io_t. A thin relay: while the connection is down, bytes from the UART are dropped (the gateway's own
// resends cover a short gap; a longer one loses those alarms).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RELAY_RECONNECT_MS 5000   // as dongle_bridge and ws_bridge.py
#define RELAY_HANDSHAKE_MS 10000  // max wait for HTTP 101 Switching Protocols
#define RELAY_PING_MS      30000  // idle interval before sending a client OP_PING
#define RELAY_PONG_MS      10000  // max wait after OP_PING before declaring the link dead
#define RELAY_RX_MAX       2048   // the longest frame from the server we take (host packets are far shorter)

typedef struct {
    // open a TCP connection; false if it cannot start (the relay retries later)
    bool (*tcp_connect)(void *ctx, const char *host, uint16_t port);
    bool (*tcp_send)(void *ctx, const uint8_t *data, size_t len);  // all of it, or false
    void (*tcp_close)(void *ctx);
    void (*uart_write)(void *ctx, const uint8_t *data, size_t len);
    uint32_t (*random32)(void *ctx);  // the handshake key and the frames' masks
    void (*log)(void *ctx, const char *msg);
    void *ctx;
} relay_io_t;

typedef enum { RELAY_DOWN, RELAY_HANDSHAKE, RELAY_OPEN } relay_state_t;

typedef struct {
    relay_io_t io;
    char host[64];
    uint16_t port;
    char request[512];  // the handshake's GET line and headers
    relay_state_t state;
    uint32_t retry_at_ms;
    uint8_t rx[RELAY_RX_MAX + 14];  // what came from the server and is not handled yet
    size_t rx_len;
    uint32_t connects;        // handshakes completed (for the platform's status)
    uint32_t last_rx_ms;      // when the last server bytes arrived (relay_on_tcp)
    uint32_t state_since_ms;  // when the current state began
    uint32_t ping_sent_ms;    // when the last unanswered client OP_PING was sent
    bool awaiting_pong;
} relay_t;

// url: ws://host[:port][/path]; device: a label for the server's log (?device=); token: NULL or the bridge's token.
// False if the URL is not ws:// or does not fit.
bool relay_init(relay_t *r, const char *url, const char *device, const char *token, const relay_io_t *io);

// Time passes: (re)connects when due.
void relay_tick(relay_t *r, uint32_t now_ms);

// Bytes from the server.
void relay_on_tcp(relay_t *r, const uint8_t *data, size_t len, uint32_t now_ms);

// The connection closed (the server, the network): the relay reconnects RELAY_RECONNECT_MS later.
void relay_on_tcp_closed(relay_t *r, uint32_t now_ms);

// Bytes from the gateway's UART: one binary message while open, dropped otherwise.
void relay_on_uart(relay_t *r, const uint8_t *data, size_t len, uint32_t now_ms);

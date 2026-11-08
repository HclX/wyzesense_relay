#include "relay.h"

#include <stdio.h>
#include <string.h>

// RFC 6455 opcodes
#define OP_CONT   0x0
#define OP_BINARY 0x2
#define OP_CLOSE  0x8
#define OP_PING   0x9
#define OP_PONG   0xA

static void say(relay_t *r, const char *msg)
{
    if (r->io.log) {
        r->io.log(r->io.ctx, msg);
    }
}

// URL encoding of a query value (RFC 3986 unreserved characters kept)
static size_t urlenc(char *out, size_t cap, const char *s)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t n = 0;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        bool keep = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
                    c == '_' || c == '.' || c == '~';
        if (n + (keep ? 1 : 3) >= cap) {
            return cap;
        }
        if (keep) {
            out[n++] = (char)c;
        } else {
            out[n++] = '%', out[n++] = hex[c >> 4], out[n++] = hex[c & 15];
        }
    }
    out[n] = 0;
    return n;
}

static void base64(char *out, const uint8_t *in, size_t len)
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v =
            (uint32_t)in[i] << 16 | (i + 1 < len ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < len ? in[i + 2] : 0);
        out[o++] = t[v >> 18 & 63];
        out[o++] = t[v >> 12 & 63];
        out[o++] = i + 1 < len ? t[v >> 6 & 63] : '=';
        out[o++] = i + 2 < len ? t[v & 63] : '=';
    }
    out[o] = 0;
}

bool relay_init(relay_t *r, const char *url, const char *device, const char *token, const relay_io_t *io)
{
    char path[192] = "/", dev[96], tok[96], hostport[80];
    memset(r, 0, sizeof *r);
    r->io = *io;
    r->port = 80;
    if (strncmp(url, "ws://", 5) != 0) {
        return false;
    }
    const char *h = url + 5, *slash = strchr(h, '/');
    size_t hl = slash ? (size_t)(slash - h) : strlen(h);
    if (hl == 0 || hl >= sizeof hostport || (slash && strlen(slash) >= sizeof path)) {
        return false;
    }
    memcpy(hostport, h, hl);
    hostport[hl] = 0;
    if (slash) {
        memcpy(path, slash, strlen(slash) + 1);  // fits: checked above
    }
    char *colon = strchr(hostport, ':');
    if (colon) {
        long p = 0;
        for (const char *c = colon + 1; *c; c++) {
            if (*c < '0' || *c > '9') {
                return false;
            }
            p = p * 10 + (*c - '0');
            if (p > 65535) {
                return false;
            }
        }
        r->port = (uint16_t)p;
        *colon = 0;
    }
    if (!hostport[0] || strlen(hostport) >= sizeof r->host || urlenc(dev, sizeof dev, device) >= sizeof dev ||
        (token && urlenc(tok, sizeof tok, token) >= sizeof tok)) {
        return false;
    }
    memcpy(r->host, hostport, strlen(hostport) + 1);  // fits: checked above
    // the key: 16 random bytes, base64; the server's Sec-WebSocket-Accept is not checked (as ws_bridge.py)
    uint8_t key[16];
    char key64[25];
    for (int i = 0; i < 16; i += 4) {
        uint32_t v = io->random32(io->ctx);
        memcpy(key + i, &v, 4);
    }
    base64(key64, key, sizeof key);
    int n =
        snprintf(r->request, sizeof r->request,
                 "GET %s%cdevice=%s%s%s HTTP/1.1\r\nHost: %.*s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                 "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n",
                 path, strchr(path, '?') ? '&' : '?', dev, token ? "&token=" : "", token ? tok : "", (int)hl, h, key64);
    return n > 0 && (size_t)n < sizeof r->request;
}

static bool send_frame(relay_t *r, uint8_t op, const uint8_t *data, size_t len);

static void go_down(relay_t *r, uint32_t now_ms, const char *why)
{
    if (r->state != RELAY_DOWN) {
        r->io.tcp_close(r->io.ctx);
        say(r, why);
    }
    r->state = RELAY_DOWN;
    r->rx_len = 0;
    r->awaiting_pong = false;
    r->state_since_ms = now_ms;
    r->retry_at_ms = now_ms + RELAY_RECONNECT_MS;
}

void relay_tick(relay_t *r, uint32_t now_ms)
{
    if (r->state == RELAY_HANDSHAKE) {
        if (now_ms - r->state_since_ms >= RELAY_HANDSHAKE_MS) {
            go_down(r, now_ms, "relay: handshake timed out");
        }
        return;
    }
    if (r->state == RELAY_OPEN) {
        if (r->awaiting_pong) {
            if (now_ms - r->ping_sent_ms >= RELAY_PONG_MS) {
                go_down(r, now_ms, "relay: ping timed out");
            }
        } else if (now_ms - r->last_rx_ms >= RELAY_PING_MS) {
            if (!send_frame(r, OP_PING, NULL, 0)) {
                go_down(r, now_ms, "relay: sending a ping failed");
                return;
            }
            r->awaiting_pong = true;
            r->ping_sent_ms = now_ms;
        }
        return;
    }
    if ((int32_t)(now_ms - r->retry_at_ms) < 0) {
        return;
    }
    if (!r->io.tcp_connect(r->io.ctx, r->host, r->port)) {
        r->retry_at_ms = now_ms + RELAY_RECONNECT_MS;
        return;
    }
    r->state = RELAY_HANDSHAKE;
    r->state_since_ms = now_ms;
    r->rx_len = 0;
    r->awaiting_pong = false;
    if (!r->io.tcp_send(r->io.ctx, (const uint8_t *)r->request, strlen(r->request))) {
        go_down(r, now_ms, "relay: the handshake could not be sent");
    }
}

// A client frame: final, masked (RFC 6455 §5.3). Short frames (<= 256 B) go out in a single tcp_send call so
// TCP_NODELAY emits one Wi-Fi packet instead of two.
static bool send_frame(relay_t *r, uint8_t op, const uint8_t *data, size_t len)
{
    uint8_t pkt[14 + 256];
    size_t n = 0;
    uint32_t m = r->io.random32(r->io.ctx);
    uint8_t mask[4] = {(uint8_t)m, (uint8_t)(m >> 8), (uint8_t)(m >> 16), (uint8_t)(m >> 24)};
    pkt[n++] = (uint8_t)(0x80 | op);
    if (len < 126) {
        pkt[n++] = (uint8_t)(0x80 | len);
    } else if (len < 65536) {
        pkt[n++] = 0x80 | 126, pkt[n++] = (uint8_t)(len >> 8), pkt[n++] = (uint8_t)len;
    } else {
        return false;  // never needed: UART reads and pongs are short
    }
    memcpy(pkt + n, mask, 4);
    n += 4;
    if (len <= 256) {
        for (size_t i = 0; i < len; i++) {
            pkt[n + i] = data[i] ^ mask[i & 3];
        }
        return r->io.tcp_send(r->io.ctx, pkt, n + len);
    }
    if (!r->io.tcp_send(r->io.ctx, pkt, n)) {
        return false;
    }
    uint8_t *buf = pkt + 14;
    for (size_t off = 0; off < len; off += 256) {
        size_t k = len - off < 256 ? len - off : 256;
        for (size_t i = 0; i < k; i++) {
            buf[i] = data[off + i] ^ mask[(off + i) & 3];
        }
        if (!r->io.tcp_send(r->io.ctx, buf, k)) {
            return false;
        }
    }
    return true;
}

// The handshake reply: complete once "\r\n\r\n" is in; accepts both "HTTP/1.1 101 Switching..." and bare
// "HTTP/1.1 101\r\n" (RFC 9112 empty reason-phrase), then keeps any trailing bytes as WebSocket frames.
static void handshake(relay_t *r, uint32_t now_ms)
{
    for (size_t i = 3; i < r->rx_len; i++) {
        if (memcmp(r->rx + i - 3, "\r\n\r\n", 4) != 0) {
            continue;
        }
        if (r->rx_len < 13 || memcmp(r->rx, "HTTP/1.1 101", 12) != 0 ||
            (r->rx[12] != ' ' && r->rx[12] != '\r' && r->rx[12] != '\t')) {
            go_down(r, now_ms, "relay: the server refused the WebSocket (no 101)");
            return;
        }
        memmove(r->rx, r->rx + i + 1, r->rx_len - i - 1);  // the server's first message can come with the reply
        r->rx_len -= i + 1;
        r->state = RELAY_OPEN;
        r->state_since_ms = now_ms;
        r->last_rx_ms = now_ms;
        r->awaiting_pong = false;
        r->connects++;
        say(r, "relay: connected");
        return;
    }
    if (r->rx_len == sizeof r->rx) {
        go_down(r, now_ms, "relay: handshake reply too long");
    }
}

// The complete frames at the start of rx
static void frames(relay_t *r, uint32_t now_ms)
{
    while (r->state == RELAY_OPEN && r->rx_len >= 2) {
        const uint8_t *b = r->rx;
        uint8_t op = b[0] & 15;
        size_t n = b[1] & 127, at = 2;
        if (op >= 0x8 && (!(b[0] & 0x80) || n > 125)) {
            go_down(r, now_ms, "relay: malformed control frame from the server");
            return;
        }
        if (n == 126) {
            if (r->rx_len < 4) {
                return;
            }
            n = (size_t)b[2] << 8 | b[3], at = 4;
        } else if (n == 127) {
            go_down(r, now_ms, "relay: a frame over 64 KB from the server");
            return;
        }
        bool masked = b[1] & 0x80;  // a server should not mask; taken if it does
        if (n > RELAY_RX_MAX) {
            go_down(r, now_ms, "relay: a frame longer than RELAY_RX_MAX from the server");
            return;
        }
        if (r->rx_len < at + (masked ? 4 : 0) + n) {
            return;
        }
        uint8_t mask[4] = {0};
        if (masked) {
            memcpy(mask, b + at, 4);
            at += 4;
        }
        uint8_t *p = r->rx + at;
        for (size_t i = 0; masked && i < n; i++) {
            p[i] ^= mask[i & 3];
        }
        if (op == OP_BINARY || op == OP_CONT) {
            r->io.uart_write(r->io.ctx, p, n);
        } else if (op == OP_PING) {
            if (!send_frame(r, OP_PONG, p, n)) {
                go_down(r, now_ms, "relay: sending a pong failed");
                return;
            }
        } else if (op == OP_PONG) {
            r->awaiting_pong = false;
        } else if (op == OP_CLOSE) {
            send_frame(r, OP_CLOSE, p, n < 2 ? n : 2);
            go_down(r, now_ms, "relay: closed by the server");
            return;
        }
        memmove(r->rx, r->rx + at + n, r->rx_len - at - n);
        r->rx_len -= at + n;
    }
}

void relay_on_tcp(relay_t *r, const uint8_t *data, size_t len, uint32_t now_ms)
{
    if (len) {
        r->last_rx_ms = now_ms;
        r->awaiting_pong = false;
    }
    while (len && r->state != RELAY_DOWN) {
        size_t k = sizeof r->rx - r->rx_len < len ? sizeof r->rx - r->rx_len : len;
        memcpy(r->rx + r->rx_len, data, k);
        r->rx_len += k, data += k, len -= k;
        if (r->state == RELAY_HANDSHAKE) {
            handshake(r, now_ms);
        }
        frames(r, now_ms);
        if (k == 0) {
            go_down(r, now_ms, "relay: receive buffer full");
        }
    }
}

void relay_on_tcp_closed(relay_t *r, uint32_t now_ms)
{
    go_down(r, now_ms, "relay: connection closed");
}

void relay_on_uart(relay_t *r, const uint8_t *data, size_t len, uint32_t now_ms)
{
    if (r->state != RELAY_OPEN || !len) {
        return;  // a thin relay: nothing is kept while the connection is down
    }
    if (!send_frame(r, OP_BINARY, data, len)) {
        go_down(r, now_ms, "relay: sending failed");
    }
}

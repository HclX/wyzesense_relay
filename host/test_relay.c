// Native tests of the relay core (esp32/relay) with a fake socket and UART: the handshake request, the server's 101
// (and a refusal), frames of each length encoding split at every byte, masking, ping / pong, close, reconnects, and
// UART bytes dropped while down.
#include <stdio.h>
#include <string.h>

#include "relay.h"

static int failures, checks;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        checks++;                                                                    \
        if (!(cond)) {                                                               \
            failures++;                                                              \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                            \
    } while (0)

static struct {
    bool up, refuse;
    int connects, closes;
    char host[64];
    uint16_t port;
    uint8_t sent[8192];
    size_t sent_len;
    uint8_t uart[8192];
    size_t uart_len;
    uint32_t rnd;
} f;

static bool t_connect(void *c, const char *host, uint16_t port)
{
    (void)c;
    f.connects++;
    snprintf(f.host, sizeof f.host, "%s", host);
    f.port = port;
    f.up = !f.refuse;
    return f.up;
}

static bool t_send(void *c, const uint8_t *d, size_t n)
{
    (void)c;
    if (!f.up || f.sent_len + n > sizeof f.sent) {
        return false;
    }
    memcpy(f.sent + f.sent_len, d, n);
    f.sent_len += n;
    return true;
}

static void t_close(void *c)
{
    (void)c;
    f.up = false;
    f.closes++;
}

static void t_uart(void *c, const uint8_t *d, size_t n)
{
    (void)c;
    memcpy(f.uart + f.uart_len, d, n);
    f.uart_len += n;
}

static uint32_t t_rand(void *c)
{
    (void)c;
    return f.rnd = f.rnd * 1103515245u + 12345u;
}

static const relay_io_t io = {t_connect, t_send, t_close, t_uart, t_rand, NULL, NULL};

static const char ok101[] = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                            "Sec-WebSocket-Accept: x\r\n\r\n";

static void reset_fake(void)
{
    memset(&f, 0, sizeof f);
    f.rnd = 7;
}

static void open_relay(relay_t *r)
{
    reset_fake();
    CHECK(relay_init(r, "ws://gw.local:8080/ws/bridge", "/dev/ttyS1", "s3cr t", &io));
    relay_tick(r, 0);
    relay_on_tcp(r, (const uint8_t *)ok101, strlen(ok101), 0);
    CHECK(r->state == RELAY_OPEN);
    f.sent_len = 0;
}

// a server frame (unmasked), with a length of n
static size_t server_frame(uint8_t *out, uint8_t op, const uint8_t *p, size_t n)
{
    size_t h = 0;
    out[h++] = (uint8_t)(0x80 | op);
    if (n < 126) {
        out[h++] = (uint8_t)n;
    } else {
        out[h++] = 126, out[h++] = (uint8_t)(n >> 8), out[h++] = (uint8_t)n;
    }
    if (n) {
        memcpy(out + h, p, n);
    }
    return h + n;
}

// the client frame at f.sent + at: its opcode and unmasked payload; returns the next offset (0 if not a frame)
static size_t client_frame(size_t at, uint8_t *op, uint8_t *payload, size_t *n)
{
    const uint8_t *b = f.sent + at;
    if (f.sent_len < at + 6 || !(b[1] & 0x80)) {
        return 0;
    }
    size_t len = b[1] & 127, h = 2;
    if (len == 126) {
        len = (size_t)b[2] << 8 | b[3], h = 4;
    }
    const uint8_t *mask = b + h;
    for (size_t i = 0; i < len; i++) {
        payload[i] = b[h + 4 + i] ^ mask[i & 3];
    }
    *op = b[0] & 15, *n = len;
    return at + h + 4 + len;
}

static void test_url_and_handshake(void)
{
    relay_t r;
    reset_fake();
    CHECK(!relay_init(&r, "wss://x/y", "d", NULL, &io));
    CHECK(!relay_init(&r, "ws://:80/", "d", NULL, &io));
    CHECK(!relay_init(&r, "ws://h:99999/", "d", NULL, &io));
    CHECK(relay_init(&r, "ws://gw.local:8080/ws/bridge", "/dev/ttyS1", "s3cr t", &io));
    CHECK(!strcmp(r.host, "gw.local") && r.port == 8080);
    relay_tick(&r, 0);
    CHECK(f.connects == 1 && !strcmp(f.host, "gw.local") && f.port == 8080 && r.state == RELAY_HANDSHAKE);
    f.sent[f.sent_len] = 0;
    CHECK(strstr((char *)f.sent, "GET /ws/bridge?device=%2Fdev%2FttyS1&token=s3cr%20t HTTP/1.1\r\n") == (char *)f.sent);
    CHECK(strstr((char *)f.sent, "\r\nHost: gw.local:8080\r\n") && strstr((char *)f.sent, "Sec-WebSocket-Key: "));
    CHECK(strstr((char *)f.sent, "\r\nSec-WebSocket-Version: 13\r\n\r\n"));
    CHECK(relay_init(&r, "ws://10.0.0.2", "d", NULL, &io) && r.port == 80);  // no port, no path
    relay_tick(&r, 0);
    f.sent[f.sent_len] = 0;
    CHECK(strstr((char *)f.sent, "GET /?device=d HTTP/1.1\r\nHost: 10.0.0.2\r\n"));
    reset_fake();
    CHECK(relay_init(&r, "ws://gw.local:8080/ws/bridge?room=lab", "d1", "tok", &io));
    relay_tick(&r, 0);
    f.sent[f.sent_len] = 0;
    CHECK(strstr((char *)f.sent, "GET /ws/bridge?room=lab&device=d1&token=tok HTTP/1.1\r\n") == (char *)f.sent);
}

static void test_refused_and_reconnect(void)
{
    relay_t r;
    reset_fake();
    CHECK(relay_init(&r, "ws://h:1/b", "d", NULL, &io));
    relay_tick(&r, 0);
    const char *no = "HTTP/1.1 401 Unauthorized\r\n\r\n";
    relay_on_tcp(&r, (const uint8_t *)no, strlen(no), 10);
    CHECK(r.state == RELAY_DOWN && f.closes == 1);
    relay_tick(&r, 10 + RELAY_RECONNECT_MS - 1);
    CHECK(f.connects == 1);  // not yet
    relay_tick(&r, 10 + RELAY_RECONNECT_MS);
    CHECK(f.connects == 2 && r.state == RELAY_HANDSHAKE);
    relay_on_tcp_closed(&r, 20000);
    f.refuse = true;
    relay_tick(&r, 20000 + RELAY_RECONNECT_MS);
    CHECK(f.connects == 3 && r.state == RELAY_DOWN);  // could not connect: again later
    f.refuse = false;
    relay_tick(&r, 20000 + 2 * RELAY_RECONNECT_MS);
    CHECK(f.connects == 4 && r.state == RELAY_HANDSHAKE);
}

// the server's first message in the same segment as the 101 (wyzesense2mqtt-rs sends its first command at once)
static void test_first_message_with_the_reply(void)
{
    relay_t r;
    uint8_t seg[512], p[3] = {0xAA, 0x55, 0x43};
    reset_fake();
    CHECK(relay_init(&r, "ws://h/b", "d", NULL, &io));
    relay_tick(&r, 0);
    size_t n = strlen(ok101);
    memcpy(seg, ok101, n);
    n += server_frame(seg + n, 0x2, p, 3);
    relay_on_tcp(&r, seg, n, 0);
    CHECK(r.state == RELAY_OPEN && r.connects == 1 && f.uart_len == 3 && !memcmp(f.uart, p, 3));
}

// frames of 0, 125, 126 and 1000 bytes, fed one byte at a time and all at once
static void test_frames_split_anywhere(void)
{
    static const size_t sizes[] = {0, 1, 125, 126, 1000};
    for (int mode = 0; mode < 2; mode++) {
        relay_t r;
        open_relay(&r);
        static uint8_t all[8192], want[4096];
        size_t n = 0, w = 0;
        for (size_t k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
            uint8_t p[1000];
            for (size_t i = 0; i < sizes[k]; i++) {
                p[i] = (uint8_t)(i * 7 + k);
            }
            n += server_frame(all + n, 0x2, p, sizes[k]);
            memcpy(want + w, p, sizes[k]);
            w += sizes[k];
        }
        if (mode == 0) {
            for (size_t i = 0; i < n; i++) {
                relay_on_tcp(&r, all + i, 1, 0);
            }
        } else {
            relay_on_tcp(&r, all, n, 0);
        }
        CHECK(r.state == RELAY_OPEN && f.uart_len == w && !memcmp(f.uart, want, w));
    }
}

static void test_uart_to_server_masked(void)
{
    relay_t r;
    uint8_t data[300], op = 0, got[400];
    size_t n = 0;
    open_relay(&r);
    for (size_t i = 0; i < sizeof data; i++) {
        data[i] = (uint8_t)i;
    }
    relay_on_uart(&r, data, 5, 0);
    relay_on_uart(&r, data, sizeof data, 0);  // 300: the 126 length form
    size_t at = client_frame(0, &op, got, &n);
    CHECK(at && op == 0x2 && n == 5 && !memcmp(got, data, 5));
    at = client_frame(at, &op, got, &n);
    CHECK(at == f.sent_len && op == 0x2 && n == sizeof data && !memcmp(got, data, sizeof data));
}

static void test_ping_close_and_down(void)
{
    relay_t r;
    uint8_t seg[64], op = 0, got[64], data[4] = {1, 2, 3, 4};
    size_t n = 0;
    open_relay(&r);
    relay_on_tcp(&r, seg, server_frame(seg, 0x9, (const uint8_t *)"hi", 2), 0);  // ping -> pong "hi"
    CHECK(client_frame(0, &op, got, &n) == f.sent_len && op == 0xA && n == 2 && !memcmp(got, "hi", 2));
    f.sent_len = 0;
    relay_on_tcp(&r, seg, server_frame(seg, 0x8, (const uint8_t *)"\x03\xe8", 2), 100);  // close 1000
    CHECK(client_frame(0, &op, got, &n) && op == 0x8 && n == 2 && r.state == RELAY_DOWN && f.closes == 1);
    f.sent_len = 0;
    relay_on_uart(&r, data, 4, 200);  // down: dropped, nothing sent
    CHECK(f.sent_len == 0);
    relay_tick(&r, 100 + RELAY_RECONNECT_MS);
    CHECK(r.state == RELAY_HANDSHAKE);
}

static void test_oversized_frame_drops_the_connection(void)
{
    relay_t r;
    uint8_t h[4] = {0x82, 126, (uint8_t)((RELAY_RX_MAX + 1) >> 8), (uint8_t)(RELAY_RX_MAX + 1)};
    open_relay(&r);
    relay_on_tcp(&r, h, sizeof h, 0);
    CHECK(r.state == RELAY_DOWN && f.closes == 1);
}

static void test_keepalive_bare_101_and_control_bounds(void)
{
    relay_t r;
    uint8_t seg[256], op = 0, got[256];
    size_t n = 0;

    // 1. Bare "HTTP/1.1 101\r\n" without trailing space succeeds; "HTTP/1.1 1010\r\n" is rejected.
    reset_fake();
    CHECK(relay_init(&r, "ws://h/b", "d", NULL, &io));
    relay_tick(&r, 100);
    const char *bare101 = "HTTP/1.1 101\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\r\n";
    relay_on_tcp(&r, (const uint8_t *)bare101, strlen(bare101), 120);
    CHECK(r.state == RELAY_OPEN && r.connects == 1);

    reset_fake();
    CHECK(relay_init(&r, "ws://h/b", "d", NULL, &io));
    relay_tick(&r, 0);
    const char *bad1010 = "HTTP/1.1 1010 Bogus\r\n\r\n";
    relay_on_tcp(&r, (const uint8_t *)bad1010, strlen(bad1010), 10);
    CHECK(r.state == RELAY_DOWN && f.closes == 1);

    // 2. Handshake timeout if server accepts TCP but never sends HTTP 101.
    reset_fake();
    CHECK(relay_init(&r, "ws://h/b", "d", NULL, &io));
    relay_tick(&r, 1000);
    CHECK(r.state == RELAY_HANDSHAKE);
    relay_tick(&r, 1000 + RELAY_HANDSHAKE_MS - 1);
    CHECK(r.state == RELAY_HANDSHAKE);
    relay_tick(&r, 1000 + RELAY_HANDSHAKE_MS);
    CHECK(r.state == RELAY_DOWN && f.closes == 1);

    // 3. Periodic client OP_PING after RELAY_PING_MS, answered by server OP_PONG; then dead socket times out after
    // RELAY_PONG_MS and drops to RELAY_DOWN so buffer.c resumes local buffering.
    open_relay(&r);
    relay_tick(&r, RELAY_PING_MS - 1);
    CHECK(f.sent_len == 0 && !r.awaiting_pong);
    relay_tick(&r, RELAY_PING_MS);
    CHECK(r.awaiting_pong && client_frame(0, &op, got, &n) == f.sent_len && op == 0x9 && n == 0);
    f.sent_len = 0;
    relay_on_tcp(&r, seg, server_frame(seg, 0xA, NULL, 0), RELAY_PING_MS + 50);  // server PONG
    CHECK(!r.awaiting_pong && r.state == RELAY_OPEN);

    uint32_t t_ping2 = RELAY_PING_MS + 50 + RELAY_PING_MS;
    relay_tick(&r, t_ping2);
    CHECK(r.awaiting_pong && f.sent_len == 6);
    relay_tick(&r, t_ping2 + RELAY_PONG_MS - 1);
    CHECK(r.state == RELAY_OPEN);
    relay_tick(&r, t_ping2 + RELAY_PONG_MS);
    CHECK(r.state == RELAY_DOWN && f.closes == 1);

    // 4. Max-length 125-byte server PING is echoed cleanly; >125-byte control frame is rejected per RFC 6455 §5.5.
    open_relay(&r);
    uint8_t p125[125];
    memset(p125, 0x5A, sizeof p125);
    relay_on_tcp(&r, seg, server_frame(seg, 0x9, p125, sizeof p125), 10);
    CHECK(r.state == RELAY_OPEN && client_frame(0, &op, got, &n) == f.sent_len && op == 0xA && n == 125 &&
          !memcmp(got, p125, 125));
    uint8_t bad_ping[4] = {0x89, 126, 0x00, 126};
    relay_on_tcp(&r, bad_ping, sizeof bad_ping, 20);
    CHECK(r.state == RELAY_DOWN && f.closes == 1);
}

int main(void)
{
    test_url_and_handshake();
    test_refused_and_reconnect();
    test_first_message_with_the_reply();
    test_frames_split_anywhere();
    test_uart_to_server_masked();
    test_ping_close_and_down();
    test_oversized_frame_drops_the_connection();
    test_keepalive_bare_101_and_control_bounds();
    printf("%d checks, %d failed\n", checks, failures);
    return failures != 0;
}

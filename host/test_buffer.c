// Native tests of the outage buffer (esp32/relay/buffer.c) around the relay, with a fake socket and UART: alarms ACKed
// and queued while down (nothing else), sent after the reconnect and the handshake delay, their server ACKs kept from
// the gateway, other server packets passed on, alarms passed straight through while up, the oldest dropped when full.
#include <stdio.h>
#include <string.h>

#include "buffer.h"
#include "host_proto.h"
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
    bool up;
    uint8_t sent[16384];
    size_t sent_len;
    uint8_t uart[4096];
    size_t uart_len;
    uint32_t rnd;
} f;

static buffer_t buf;

static bool t_connect(void *c, const char *h, uint16_t p)
{
    (void)c, (void)h, (void)p;
    return f.up = true;
}

static bool t_send(void *c, const uint8_t *d, size_t n)
{
    (void)c;
    memcpy(f.sent + f.sent_len, d, n);
    f.sent_len += n;
    return f.up;
}

static void t_close(void *c)
{
    (void)c;
    f.up = false;
}

static void real_uart(void *c, const uint8_t *d, size_t n)
{
    (void)c;
    memcpy(f.uart + f.uart_len, d, n);
    f.uart_len += n;
}

static void relay_uart(void *c, const uint8_t *d, size_t n)  // the relay's uart_write: through the buffer
{
    (void)c;
    buffer_from_server(&buf, d, n);
}

static uint32_t t_rand(void *c)
{
    (void)c;
    return f.rnd = f.rnd * 1103515245u + 12345u;
}

static const relay_io_t io = {t_connect, t_send, t_close, relay_uart, t_rand, NULL, NULL};
static const char ok101[] = "HTTP/1.1 101 Switching Protocols\r\n\r\n";

static void handshake(relay_t *r, uint32_t now)
{
    relay_tick(r, now);
    relay_on_tcp(r, (const uint8_t *)ok101, strlen(ok101), now);
    buffer_tick(&buf, now);
    f.sent_len = 0;
}

static size_t alarm_at(uint8_t *out, uint8_t n, uint64_t time_ms)
{
    uint8_t pl[33] = {0};
    for (int i = 0; i < 8; i++) {
        pl[i] = (uint8_t)(time_ms >> (56 - 8 * i));
    }
    pl[8] = 0xA2, pl[30] = n;
    return host_build(out, HOST_ALARM, pl, sizeof pl, true);
}

static size_t alarm(uint8_t *out, uint8_t n)  // a gateway alarm (0x5319) with counter n
{
    return alarm_at(out, n, 0);
}

static uint64_t be64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v = (v << 8) | p[i];
    }
    return v;
}

static void server_says(relay_t *r, const uint8_t *p, size_t n, uint32_t now)  // one unmasked binary frame
{
    uint8_t fr[300] = {0x82, (uint8_t)n};
    memcpy(fr + 2, p, n);
    relay_on_tcp(r, fr, n + 2, now);
}

// the payloads of the binary frames the relay sent (unmasked, concatenated)
static size_t to_server(uint8_t *out)
{
    size_t n = 0;
    for (size_t at = 0; at + 6 <= f.sent_len;) {
        const uint8_t *b = f.sent + at;
        size_t len = b[1] & 127, h = 2;
        if (len == 126) {
            len = (size_t)b[2] << 8 | b[3], h = 4;
        }
        for (size_t i = 0; i < len; i++) {
            out[n++] = b[h + 4 + i] ^ b[h + (i & 3)];
        }
        at += h + 4 + len;
    }
    return n;
}

int main(void)
{
    relay_t r;
    uint8_t a[80], ack[16], got[4096], other[16];
    size_t an, ackn = host_build_ack(ack, HOST_ALARM, false);
    f.rnd = 3;
    CHECK(relay_init(&r, "ws://h:1/b", "d", NULL, &io));
    buffer_init(&buf, &r, real_uart, NULL);
    handshake(&r, 0);

    // up (past post-connect settle): an alarm goes straight on, no ACK from the relay, nothing queued
    an = alarm(a, 1);
    buffer_from_gateway(&buf, a, an, BUFFER_REPLAY_AFTER + 10);
    CHECK(to_server(got) == an && !memcmp(got, a, an) && f.uart_len == 0 && buf.count == 0);

    // down: alarms ACKed in the host's place and queued; a scan result is neither
    relay_on_tcp_closed(&r, 3100);
    f.sent_len = 0;
    for (uint8_t k = 2; k <= 4; k++) {
        an = alarm(a, k);
        buffer_from_gateway(&buf, a, an, 3200);
    }
    size_t sn = host_build(other, HOST_SCAN_RESULT, (const uint8_t *)"\xa3", 1, true);
    buffer_from_gateway(&buf, other, sn, 3210);
    CHECK(buf.count == 3 && f.uart_len == 3 * ackn && !memcmp(f.uart, ack, ackn) && f.sent_len == 0);

    // up again: nothing for BUFFER_REPLAY_AFTER, then one alarm each BUFFER_REPLAY_EVERY, in order
    handshake(&r, 3100 + RELAY_RECONNECT_MS);
    uint32_t t0 = 3100 + RELAY_RECONNECT_MS;
    buffer_tick(&buf, t0 + BUFFER_REPLAY_AFTER - 1);
    CHECK(f.sent_len == 0);
    buffer_tick(&buf, t0 + BUFFER_REPLAY_AFTER);
    an = alarm(a, 2);
    CHECK(to_server(got) == an && !memcmp(got, a, an) && buf.count == 2);
    buffer_tick(&buf, t0 + BUFFER_REPLAY_AFTER + 1);  // not yet
    CHECK(to_server(got) == an);
    buffer_tick(&buf, t0 + BUFFER_REPLAY_AFTER + BUFFER_REPLAY_EVERY);
    buffer_tick(&buf, t0 + BUFFER_REPLAY_AFTER + 2 * BUFFER_REPLAY_EVERY);
    CHECK(buf.count == 0 && buf.replayed_total == 3 && to_server(got) == 3 * an && got[2 * an + 35] == 4);

    // the server's ACKs of the 3 replays are kept from the gateway; a time sync (0x5333) passes; then ACKs pass again
    f.uart_len = 0;
    for (int k = 0; k < 3; k++) {
        server_says(&r, ack, ackn, t0 + 4000);
    }
    CHECK(f.uart_len == 0 && buf.acks_to_keep == 0);
    size_t tn = host_build(other, HOST_TIME_SYNC, (const uint8_t *)"\0\0\0\0\0\0\0\1", 8, false);
    server_says(&r, other, tn, t0 + 4000);
    CHECK(f.uart_len == tn && !memcmp(f.uart, other, tn));
    server_says(&r, ack, ackn, t0 + 4000);
    CHECK(f.uart_len == tn + ackn);  // an ACK of a live alarm: the gateway's

    // full: the oldest go
    relay_on_tcp_closed(&r, t0 + 5000);
    for (uint8_t k = 0; k < BUFFER_ALARMS + 3; k++) {
        an = alarm(a, k);
        buffer_from_gateway(&buf, a, an, t0 + 5000);
    }
    CHECK(buf.count == BUFFER_ALARMS && buf.dropped_total == 3 && buf.queue[buf.head][5 + 30] == 3);

    // FIFO ordering during post-connect settle and mid-drain, plus acks_to_keep expiry (TASK-02)
    buffer_init(&buf, &r, real_uart, NULL);
    relay_on_tcp_closed(&r, 10000);
    f.sent_len = f.uart_len = 0;
    an = alarm(a, 10);
    buffer_from_gateway(&buf, a, an, 10100);
    an = alarm(a, 11);
    buffer_from_gateway(&buf, a, an, 10200);
    CHECK(buf.count == 2 && f.uart_len == 2 * ackn);

    uint32_t t1 = 10000 + RELAY_RECONNECT_MS;
    handshake(&r, t1);
    f.uart_len = 0;
    // during the 3 s settle window: handshake replies pass straight through, while live alarms queue at the tail
    size_t inqn = host_build(other, 0x4328, (const uint8_t *)"\x01", 1, true);
    buffer_from_gateway(&buf, other, inqn, t1 + 500);
    CHECK(to_server(got) == inqn && !memcmp(got, other, inqn));
    f.sent_len = 0;
    an = alarm(a, 12);
    buffer_from_gateway(&buf, a, an, t1 + 1000);
    CHECK(f.sent_len == 0 && buf.count == 3 && f.uart_len == ackn);

    // start draining #10, then #13 arrives mid-drain (50 ms later) and must queue behind #11 and #12
    buffer_tick(&buf, t1 + BUFFER_REPLAY_AFTER);
    CHECK(buf.count == 2 && to_server(got) == an && got[35] == 10);
    an = alarm(a, 13);
    buffer_from_gateway(&buf, a, an, t1 + BUFFER_REPLAY_AFTER + 50);
    CHECK(buf.count == 3 && f.uart_len == 2 * ackn && to_server(got) == an);

    // finish draining #11, #12, #13 in strict chronological FIFO order
    for (int step = 1; step <= 3; step++) {
        buffer_tick(&buf, t1 + BUFFER_REPLAY_AFTER + (uint32_t)step * BUFFER_REPLAY_EVERY);
    }
    CHECK(buf.count == 0 && buf.acks_to_keep == 4 && to_server(got) == 4 * an);
    CHECK(got[0 * an + 35] == 10 && got[1 * an + 35] == 11 && got[2 * an + 35] == 12 && got[3 * an + 35] == 13);

    // once count == 0, live alarm #14 passes straight through with no local ACK
    f.sent_len = f.uart_len = 0;
    an = alarm(a, 14);
    uint32_t last_rep = t1 + BUFFER_REPLAY_AFTER + 3 * BUFFER_REPLAY_EVERY;
    buffer_from_gateway(&buf, a, an, last_rep + 50);
    CHECK(to_server(got) == an && got[35] == 14 && f.uart_len == 0 && buf.count == 0);

    // server drops 2 of the 4 replay ACKs; after BUFFER_ACK_TIMEOUT, acks_to_keep expires so live ACKs reach UART
    server_says(&r, ack, ackn, last_rep + 100);
    server_says(&r, ack, ackn, last_rep + 100);
    CHECK(buf.acks_to_keep == 2 && f.uart_len == 0);
    buffer_tick(&buf, last_rep + BUFFER_ACK_TIMEOUT - 1);
    CHECK(buf.acks_to_keep == 2);
    buffer_tick(&buf, last_rep + BUFFER_ACK_TIMEOUT);
    CHECK(buf.acks_to_keep == 0);
    server_says(&r, ack, ackn, last_rep + BUFFER_ACK_TIMEOUT + 10);
    CHECK(f.uart_len == ackn && !memcmp(f.uart, ack, ackn));

    // cold-boot 1970 timestamps rebased on 0x5333 time sync; post-2020 timestamps untouched; checksums valid
    buffer_init(&buf, &r, real_uart, NULL);
    relay_on_tcp_closed(&r, 0);
    f.sent_len = f.uart_len = 0;
    an = alarm_at(a, 20, 200);
    buffer_from_gateway(&buf, a, an, 200);
    an = alarm_at(a, 21, 1200);
    buffer_from_gateway(&buf, a, an, 1200);
    an = alarm_at(a, 22, 1700000001500ULL);
    buffer_from_gateway(&buf, a, an, 1500);

    handshake(&r, 6200);
    uint8_t ts_pl[8];
    uint64_t srv_epoch = 1700000006700ULL;
    for (int i = 0; i < 8; i++) {
        ts_pl[i] = (uint8_t)(srv_epoch >> (56 - 8 * i));
    }
    tn = host_build(other, HOST_TIME_SYNC, ts_pl, 8, false);
    server_says(&r, other, tn, 6700);

    for (int step = 0; step < 3; step++) {
        buffer_tick(&buf, 6200 + BUFFER_REPLAY_AFTER + (uint32_t)step * BUFFER_REPLAY_EVERY);
    }
    CHECK(to_server(got) == 3 * an);
    host_pkt_t p0, p1, p2;
    CHECK(host_parse(got + 0 * an, an, &p0) && be64(p0.payload) == 1700000000200ULL);
    CHECK(host_parse(got + 1 * an, an, &p1) && be64(p1.payload) == 1700000001200ULL);
    CHECK(host_parse(got + 2 * an, an, &p2) && be64(p2.payload) == 1700000001500ULL);

    // remote /ws/bridge SBL filter (TASK-11): 0x4312 and OWS_SUB_ENTER_SBL are dropped (whole or split byte-by-byte),
    // while benign commands (Inquiry 0x4327, OWS_SUB_FLAGS) pass straight through to UART.
    buffer_init(&buf, &r, real_uart, NULL);
    relay_on_tcp_closed(&r, 15000);
    handshake(&r, 15000 + RELAY_RECONNECT_MS);
    f.uart_len = 0;
    uint8_t upd[16], sbl_cmd[16], inq[16], flg[16];
    size_t updn = host_build(upd, HOST_UPDATE_MODE, NULL, 0, false);
    static const uint8_t sbl_pl[5] = {OWS_SUB_ENTER_SBL, 'O', 'S', 'B', 'L'};
    size_t sbln = host_build(sbl_cmd, HOST_OWS_CMD, sbl_pl, sizeof sbl_pl, false);
    size_t inq_req_n = host_build(inq, HOST_INQUIRY, NULL, 0, false);
    static const uint8_t flg_pl[1] = {OWS_SUB_FLAGS};
    size_t flgn = host_build(flg, HOST_OWS_CMD, flg_pl, sizeof flg_pl, false);

    server_says(&r, upd, updn, 20100);
    server_says(&r, sbl_cmd, sbln, 20110);
    CHECK(f.uart_len == 0);
    for (size_t i = 0; i < updn; i++) {
        server_says(&r, upd + i, 1, 20200 + (uint32_t)i);
    }
    for (size_t i = 0; i < sbln; i++) {
        server_says(&r, sbl_cmd + i, 1, 20300 + (uint32_t)i);
    }
    CHECK(f.uart_len == 0);
    server_says(&r, inq, inq_req_n, 20400);
    server_says(&r, flg, flgn, 20410);
    CHECK(f.uart_len == inq_req_n + flgn && !memcmp(f.uart, inq, inq_req_n) && !memcmp(f.uart + inq_req_n, flg, flgn));

    // Passive sensor telemetry (TASK-17): tracks MAC, type, RSSI, battery mV, and last_seen_ms on both fast & queued
    // paths; preserves prior battery across 0xAB/0xAD (bat_raw=0); LRU-evicts oldest when BUFFER_SENSORS is full.
    buffer_init(&buf, &r, real_uart, NULL);
    relay_on_tcp_closed(&r, 30000);
    handshake(&r, 30000 + RELAY_RECONNECT_MS);

    uint8_t pt_v1[17] = {0xA2, '7', '7', '7', 'B', '4', '2', 'D', 'F', 0x01, 24, 96, 0, 1, 1, 0, 1};
    uint8_t pl_s[40];
    size_t pl_len = host_alarm_payload(pl_s, pt_v1, sizeof pt_v1, 1700000030000ULL, 65);
    an = host_build(a, HOST_ALARM, pl_s, pl_len, true);
    buffer_from_gateway(&buf, a, an, 40000);
    CHECK(buf.sensor_count == 1 && !strcmp(buf.sensors[0].mac, "777B42DF") && buf.sensors[0].type == 0x01);
    CHECK(buf.sensors[0].last_rssi == -65 && buf.sensors[0].last_bat_mv == 3000 &&
          buf.sensors[0].last_seen_ms == 40000);

    // 0xAB motion summary (no battery byte) and 0xAD replay (bat_raw=0) while offline update RSSI/seen, keep bat_mv
    relay_on_tcp_closed(&r, 40500);
    uint8_t pt_ab[23] = {0xAB, '7', '7', '7', 'B', '4', '2', 'D', 'F', 0x02};
    pt_ab[22] = 0x01;
    pl_len = host_alarm_payload(pl_s, pt_ab, sizeof pt_ab, 1700000031000ULL, 78);
    an = host_build(a, HOST_ALARM, pl_s, pl_len, true);
    buffer_from_gateway(&buf, a, an, 41000);
    CHECK(buf.sensor_count == 1 && buf.sensors[0].type == 0x02 && buf.sensors[0].last_rssi == -78);
    CHECK(buf.sensors[0].last_bat_mv == 3000 && buf.sensors[0].last_seen_ms == 41000);

    uint8_t pt_ad[17] = {0xA2, '7', '7', '7', 'B', '4', '2', 'D', 'F', 0x02, 0, 0, 0, 1, 0, 0, 2};
    pl_len = host_alarm_payload(pl_s, pt_ad, sizeof pt_ad, 1700000032000ULL, 81);
    an = host_build(a, HOST_ALARM, pl_s, pl_len, true);
    buffer_from_gateway(&buf, a, an, 42000);
    CHECK(buf.sensors[0].last_rssi == -81 && buf.sensors[0].last_bat_mv == 3000 &&
          buf.sensors[0].last_seen_ms == 42000);

    // Fill remaining 31 slots + 1 overflow: oldest (42000 ms vs 43000+ ms) is evicted unless refreshed first
    for (unsigned idx = 1; idx < BUFFER_SENSORS; idx++) {
        uint8_t pt_k[17] = {0xA1, '7', '7', 'C', 'C', '0', '0', 0, 0, 0x0E, 22, 41, 0, 1, 0, 0, 1};
        pt_k[7] = (uint8_t)("0123456789ABCDEF"[(idx >> 4) & 15]);
        pt_k[8] = (uint8_t)("0123456789ABCDEF"[idx & 15]);
        pl_len = host_alarm_payload(pl_s, pt_k, sizeof pt_k, 1700000040000ULL, 55);
        an = host_build(a, HOST_ALARM, pl_s, pl_len, true);
        buffer_from_gateway(&buf, a, an, 43000 + idx * 10);
    }
    CHECK(buf.sensor_count == BUFFER_SENSORS && buf.sensors[1].last_bat_mv == 1281);
    // Refresh 777B42DF at 50000 ms so slot 1 ("77CC0001" at 43010 ms) becomes the LRU victim
    pl_len = host_alarm_payload(pl_s, pt_v1, sizeof pt_v1, 1700000050000ULL, 60);
    an = host_build(a, HOST_ALARM, pl_s, pl_len, true);
    buffer_from_gateway(&buf, a, an, 50000);

    uint8_t pt_new[17] = {0xE8, '7', '7', 'D', 'D', '9', '9', '9', '9', 0x07, 25, 48, 0, 3, 21, 50, 45};
    uint8_t pt_e8[19];
    memcpy(pt_e8, pt_new, 17);
    pt_e8[17] = 0;
    pt_e8[18] = 5;
    pl_len = host_alarm_payload(pl_s, pt_e8, sizeof pt_e8, 1700000051000ULL, 70);
    an = host_build(a, HOST_ALARM, pl_s, pl_len, true);
    buffer_from_gateway(&buf, a, an, 51000);
    CHECK(buf.sensor_count == BUFFER_SENSORS && !strcmp(buf.sensors[0].mac, "777B42DF"));
    CHECK(!strcmp(buf.sensors[1].mac, "77DD9999") && buf.sensors[1].type == 0x07 && buf.sensors[1].last_bat_mv == 1500);

    // Empty-queue post-connect settle window: when count == 0 at RELAY_OPEN, 0x5315 (FinishAuth) at open + 200 ms
    // passes straight through, while 0x5319 at open + 500 ms is locally ACKed and held until open + 3000 ms.
    buffer_init(&buf, &r, real_uart, NULL);
    relay_on_tcp_closed(&r, 60000);
    uint32_t t_open = 60000 + RELAY_RECONNECT_MS;
    handshake(&r, t_open);
    f.sent_len = f.uart_len = 0;
    CHECK(buf.count == 0);

    size_t authn = host_build(other, 0x5315, (const uint8_t *)"\x01", 1, true);
    buffer_from_gateway(&buf, other, authn, t_open + 200);
    CHECK(to_server(got) == authn && !memcmp(got, other, authn) && f.uart_len == 0 && buf.count == 0);
    f.sent_len = 0;

    size_t macn = host_build(other, 0x4305, (const uint8_t *)"777B42DF", 8, true);
    buffer_from_gateway(&buf, other, macn, t_open + 250);
    CHECK(to_server(got) == macn && !strcmp(buf.gateway_mac, "777B42DF"));
    f.sent_len = 0;

    an = alarm(a, 99);
    buffer_from_gateway(&buf, a, an, t_open + 500);
    CHECK(f.sent_len == 0 && buf.count == 1 && f.uart_len == ackn && !memcmp(f.uart, ack, ackn));

    buffer_tick(&buf, t_open + BUFFER_REPLAY_AFTER - 1);
    CHECK(f.sent_len == 0 && buf.count == 1);
    buffer_tick(&buf, t_open + BUFFER_REPLAY_AFTER);
    CHECK(to_server(got) == an && !memcmp(got, a, an) && buf.count == 0 && buf.acks_to_keep == 1);

    printf("%d checks, %d failed\n", checks, failures);
    return failures != 0;
}

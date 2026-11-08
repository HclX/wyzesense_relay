#include "buffer.h"

#include <string.h>

void buffer_init(buffer_t *b, relay_t *r, void (*uart_write)(void *, const uint8_t *, size_t), void *ctx)
{
    memset(b, 0, sizeof *b);
    b->relay = r;
    b->uart_write = uart_write;
    b->ctx = ctx;
    host_rx_init(&b->up);
    host_rx_init(&b->down);
}

static void enqueue(buffer_t *b, const uint8_t *p, size_t n, uint32_t now_ms)
{
    if (n > BUFFER_ALARM_MAX) {
        b->dropped_total++;
        return;
    }
    if (b->count == BUFFER_ALARMS) {  // full: the oldest goes
        b->head = (b->head + 1) % BUFFER_ALARMS;
        b->count--;
        b->dropped_total++;
    }
    unsigned at = (b->head + b->count) % BUFFER_ALARMS;
    memcpy(b->queue[at], p, n);
    b->queue_len[at] = (uint8_t)n;
    b->queue_at_ms[at] = now_ms;
    b->count++;
    b->queued_total++;
}

static uint64_t read_be64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v = (v << 8) | p[i];
    }
    return v;
}

static void rebase_queued_alarms(buffer_t *b, uint64_t server_epoch_ms, uint32_t sync_rx_ms)
{
    if (server_epoch_ms < BUFFER_MIN_VALID_EPOCH_MS) {
        return;
    }
    for (unsigned i = 0; i < b->count; i++) {
        unsigned at = (b->head + i) % BUFFER_ALARMS;
        uint8_t *q = b->queue[at];
        size_t qlen = b->queue_len[at];
        if (qlen < 15 || read_be64(q + 5) >= BUFFER_MIN_VALID_EPOCH_MS) {
            continue;
        }
        uint32_t age_ms = sync_rx_ms - b->queue_at_ms[at];
        uint64_t rebased = server_epoch_ms - (uint64_t)age_ms;
        for (int k = 0; k < 8; k++) {
            q[5 + k] = (uint8_t)(rebased >> (56 - 8 * k));
        }
        uint16_t sum = 0;
        for (size_t k = 0; k < qlen - 2; k++) {
            sum += q[k];
        }
        q[qlen - 2] = (uint8_t)(sum >> 8);
        q[qlen - 1] = (uint8_t)sum;
    }
}

static void sync_state(buffer_t *b, uint32_t now_ms)
{
    b->now_ms = now_ms;
    if (b->relay->state != RELAY_OPEN) {
        b->acks_to_keep = 0;  // a new connection: no ACKs of the last one's replays will come
        b->up_len = 0;
        b->sync_pending = false;
        return;
    }
    if (b->relay->connects != b->connects_seen) {  // just connected: the server's handshake first
        b->connects_seen = b->relay->connects;
        b->open_since_ms = now_ms;
        b->next_replay_ms = now_ms + BUFFER_REPLAY_AFTER;
        b->acks_to_keep = 0;
        b->up_len = 0;
        b->sync_pending = false;
    }
    if (b->acks_to_keep && (int32_t)(now_ms - (b->last_replay_ms + BUFFER_ACK_TIMEOUT)) >= 0) {
        b->acks_to_keep = 0;
    }
}

static bool settled(const buffer_t *b, uint32_t now_ms)
{
    return b->relay->state == RELAY_OPEN && (uint32_t)(now_ms - b->open_since_ms) >= BUFFER_REPLAY_AFTER;
}

static bool is_hex_mac(const uint8_t mac[8])
{
    for (int i = 0; i < 8; i++) {
        uint8_t c = mac[i];
        bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
        if (!ok) {
            return false;
        }
    }
    return true;
}

static void record_gateway_mac(buffer_t *b, const host_pkt_t *p)
{
    if (p->len < 8 || !is_hex_mac(p->payload)) {
        return;
    }
    char tmp[9];
    for (int i = 0; i < 8; i++) {
        uint8_t c = p->payload[i];
        tmp[i] = (char)(c >= 'a' && c <= 'f' ? c - 'a' + 'A' : c);
    }
    tmp[8] = '\0';
    if (memcmp(tmp, "00000000", 8) != 0) {
        memcpy(b->gateway_mac, tmp, 9);
    }
}

static void record_sensor(buffer_t *b, const host_pkt_t *p, uint32_t now_ms)
{
    if (p->len < 26) {  // time_ms(8) + min pt(17) + rssi(1)
        return;
    }
    const uint8_t *pt = p->payload + 8;
    uint8_t op = pt[0];
    if (op != 0xA1 && op != 0xA2 && op != 0xE8 && op != 0xAB) {
        return;
    }
    if (op == 0xAB && p->len < 32) {
        return;
    }
    if (!is_hex_mac(pt + 1)) {
        return;
    }
    unsigned idx = b->sensor_count;
    for (unsigned i = 0; i < b->sensor_count; i++) {
        if (!memcmp(b->sensors[i].mac, pt + 1, 8)) {
            idx = i;
            break;
        }
    }
    if (idx == b->sensor_count) {
        if (b->sensor_count < BUFFER_SENSORS) {
            idx = b->sensor_count++;
        } else {
            idx = 0;
            for (unsigned i = 1; i < BUFFER_SENSORS; i++) {
                if ((int32_t)(b->sensors[i].last_seen_ms - b->sensors[idx].last_seen_ms) < 0) {
                    idx = i;
                }
            }
        }
        memset(&b->sensors[idx], 0, sizeof b->sensors[idx]);
        memcpy(b->sensors[idx].mac, pt + 1, 8);
        b->sensors[idx].mac[8] = '\0';
    }
    uint8_t rssi_mag = p->payload[p->len - 1];
    b->sensors[idx].type = pt[9];
    b->sensors[idx].last_rssi = rssi_mag > 127 ? (int8_t)-128 : (int8_t)-(int)rssi_mag;
    if (op != 0xAB && pt[11] != 0) {
        b->sensors[idx].last_bat_mv = (uint16_t)(((uint32_t)pt[11] * 1000u + 16u) / 32u);
    }
    b->sensors[idx].last_seen_ms = now_ms;
}

static bool is_sync_req_ack(const buffer_t *b, uint16_t acked)
{
    if ((acked >> 8) != HOST_ASYNC) {
        return false;
    }
    // only the request this relay translated: an async one the host sent itself keeps its ACK
    return b->sync_pending && (uint8_t)acked == b->sync_cmd;
}

static bool is_sync_rsp(const buffer_t *b, uint16_t pkt_cmd)
{
    if ((pkt_cmd >> 8) != HOST_ASYNC) {
        return false;
    }
    // only the reply to the request this relay translated: the same reply ID to an async request stays async
    return b->sync_pending && (uint8_t)pkt_cmd == (uint8_t)(b->sync_cmd + 1);
}

void buffer_from_gateway(buffer_t *b, const uint8_t *data, size_t len, uint32_t now_ms)
{
    sync_state(b, now_ms);
    if (settled(b, now_ms) && !b->count && !b->up_len && !b->translate_sync) {
        relay_on_uart(b->relay, data, len, now_ms);
        bool sent = b->relay->state == RELAY_OPEN;
        if (!sent) {
            b->acks_to_keep = 0;
        }
        for (size_t i = 0; i < len; i++) {  // keep the gateway parser in step; queue if send just closed the link
            host_pkt_t p;
            if (!host_rx_feed(&b->up, data[i], &p) || !p.to_host) {
                continue;
            }
            if (p.cmd == 0x4305) {
                record_gateway_mac(b, &p);
                continue;
            }
            if (p.cmd != HOST_ALARM) {
                continue;
            }
            record_sensor(b, &p, now_ms);
            if (sent) {
                continue;
            }
            uint8_t ack[16];
            enqueue(b, p.payload - 5, p.len + 7, now_ms);
            b->uart_write(b->ctx, ack, host_build_ack(ack, HOST_ALARM, false));
        }
        return;
    }
    for (size_t i = 0; i < len; i++) {
        host_pkt_t p;
        if (b->up_len == sizeof b->up_pending) {
            if (b->relay->state == RELAY_OPEN) {
                relay_on_uart(b->relay, b->up_pending, b->up_len, now_ms);
            }
            b->up_len = 0;
        }
        if (b->relay->state == RELAY_OPEN) {
            b->up_pending[b->up_len++] = data[i];
        }
        if (!host_rx_feed(&b->up, data[i], &p)) {
            continue;
        }
        size_t n = p.len + 7;  // magic 2, type, b2, cmd, payload, checksum 2
        if (b->translate_sync && p.to_host) {
            if (p.cmd == HOST_ACK && is_sync_req_ack(b, p.acked)) {
                if (b->relay->state == RELAY_OPEN && n <= b->up_len) {
                    b->up_len -= n;
                    if (b->up_len) {
                        relay_on_uart(b->relay, b->up_pending, b->up_len, now_ms);
                    }
                }
                b->up_len = 0;
                continue;
            }
            if (p.cmd != HOST_ACK && is_sync_rsp(b, p.cmd)) {
                uint8_t ack[16];
                b->uart_write(b->ctx, ack, host_build_ack(ack, p.cmd, false));
                p.cmd = (uint16_t)((uint16_t)HOST_SYNC << 8 | (p.cmd & 0xFF));
                b->sync_pending = false;
                if (b->relay->state == RELAY_OPEN && n <= b->up_len) {
                    host_build(b->up_pending + b->up_len - n, p.cmd, p.payload, p.len, true);
                }
            }
        }
        if (p.to_host && p.cmd == 0x4305) {
            record_gateway_mac(b, &p);
        }
        if (p.to_host && p.cmd == HOST_ALARM) {
            record_sensor(b, &p, now_ms);
        }
        if (p.to_host && p.cmd == HOST_ALARM && (!settled(b, now_ms) || b->count > 0)) {
            if (b->relay->state == RELAY_OPEN && n < b->up_len) {
                relay_on_uart(b->relay, b->up_pending, b->up_len - n, now_ms);
            }
            uint8_t ack[16];
            enqueue(b, p.payload - 5, n, now_ms);
            b->uart_write(b->ctx, ack, host_build_ack(ack, HOST_ALARM, false));  // in the host's place
        } else if (b->relay->state == RELAY_OPEN) {
            relay_on_uart(b->relay, b->up_pending, b->up_len, now_ms);
            if (b->relay->state != RELAY_OPEN && p.to_host && p.cmd == HOST_ALARM) {
                b->acks_to_keep = 0;
                uint8_t ack[16];
                enqueue(b, p.payload - 5, n, now_ms);
                b->uart_write(b->ctx, ack, host_build_ack(ack, HOST_ALARM, false));
            }
        }
        b->up_len = 0;
    }
}

static bool packet_in_progress(const host_rx_t *rx)
{
    if (!rx->n) {
        return false;
    }
    if (rx->buf[rx->n - 1] == 0xAA) {
        return true;
    }
    for (size_t s = 0; s + 2 <= rx->n; s++) {
        const uint8_t *b = rx->buf + s;
        if (b[0] != 0xAA || b[1] != 0x55) {
            continue;
        }
        if (rx->n - s < 5) {
            return true;
        }
        size_t need = b[4] == 0xFF ? 7 : (b[3] >= 3 ? (size_t)b[3] + 4 : 0);
        if (need >= 7 && need <= HOST_MAX_PACKET && rx->n - s < need) {
            return true;
        }
    }
    return false;
}

static bool drop_from_server(const host_pkt_t *p)
{
    if (p->to_host) {
        return false;
    }
    if (p->cmd == HOST_UPDATE_MODE) {
        return true;
    }
    return p->cmd == HOST_OWS_CMD && p->len >= 1 && p->payload[0] == OWS_SUB_ENTER_SBL;
}

void buffer_from_server(buffer_t *b, const uint8_t *data, size_t len)
{
    uint32_t rx_ms = b->relay ? b->relay->last_rx_ms : b->now_ms;
    if ((int32_t)(rx_ms - b->now_ms) > 0) {
        b->now_ms = rx_ms;
    }
    if (b->acks_to_keep && (int32_t)(b->now_ms - (b->last_replay_ms + BUFFER_ACK_TIMEOUT)) >= 0) {
        b->acks_to_keep = 0;
    }
    for (size_t i = 0; i < len; i++) {
        host_pkt_t p;
        if (b->down_len == sizeof b->down_pending) {  // not packets: pass them on
            b->uart_write(b->ctx, b->down_pending, b->down_len);
            b->down_len = 0;
        }
        b->down_pending[b->down_len++] = data[i];
        if (!host_rx_feed(&b->down, data[i], &p)) {
            continue;
        }
        size_t n = p.len + 7;
        if (!p.to_host && p.cmd == HOST_TIME_SYNC && p.len == 8 && b->count) {
            rebase_queued_alarms(b, read_be64(p.payload), b->now_ms);
        }
        if (drop_from_server(&p) && n <= b->down_len) {
            b->down_len -= n;  // remote /ws/bridge clients may never drop the attached CC1310 into raw SBL
        } else if (b->acks_to_keep && !p.to_host && p.cmd == HOST_ACK && p.acked == HOST_ALARM && n <= b->down_len) {
            b->acks_to_keep--;  // the ACK of a replayed alarm: the gateway had its ACK already
            b->down_len -= n;
        } else if (b->translate_sync && !p.to_host && (p.cmd >> 8) == HOST_SYNC && n <= b->down_len) {
            uint8_t cmd_lo = (uint8_t)p.cmd;
            if (cmd_lo == 0xFF) {
                b->down_len -= n;  // 0x43FF ACK: already ACKed as 0x53FF on the UART when the reply arrived
            } else {
                host_build(b->down_pending + b->down_len - n, (uint16_t)((uint16_t)HOST_ASYNC << 8 | cmd_lo), p.payload,
                           p.len, false);
                b->sync_cmd = cmd_lo;
                b->sync_pending = true;
            }
        }
        b->uart_write(b->ctx, b->down_pending, b->down_len);
        b->down_len = 0;
    }
    if (b->down_len && !b->acks_to_keep && !b->translate_sync && !packet_in_progress(&b->down)) {
        b->uart_write(b->ctx, b->down_pending, b->down_len);
        b->down_len = 0;
    }
}

void buffer_tick(buffer_t *b, uint32_t now_ms)
{
    sync_state(b, now_ms);
    if (b->relay->state != RELAY_OPEN || !b->count || (int32_t)(now_ms - b->next_replay_ms) < 0) {
        return;
    }
    relay_on_uart(b->relay, b->queue[b->head], b->queue_len[b->head], now_ms);
    if (b->relay->state == RELAY_OPEN) {  // sent: off the queue
        b->head = (b->head + 1) % BUFFER_ALARMS;
        b->count--;
        b->acks_to_keep++;
        b->last_replay_ms = now_ms;
        b->replayed_total++;
    }
    b->next_replay_ms = now_ms + BUFFER_REPLAY_EVERY;
}

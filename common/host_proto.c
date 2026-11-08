#include "host_proto.h"

#include <string.h>

static size_t finish(uint8_t *out, size_t n)
{
    uint16_t sum = 0;
    for (size_t i = 0; i < n; i++) {
        sum += out[i];
    }
    out[n] = (uint8_t)(sum >> 8);
    out[n + 1] = (uint8_t)sum;
    return n + 2;
}

static void magic(uint8_t *out, bool to_host)
{
    out[0] = to_host ? 0x55 : 0xAA;
    out[1] = to_host ? 0xAA : 0x55;
}

size_t host_build(uint8_t *out, uint16_t cmd, const uint8_t *payload, size_t n, bool to_host)
{
    if (n > 252 || n > HOST_MAX_PACKET - 7) {
        return 0;
    }
    magic(out, to_host);
    out[2] = (uint8_t)(cmd >> 8);
    out[3] = (uint8_t)(n + 3);
    out[4] = (uint8_t)cmd;
    if (n) {
        memcpy(out + 5, payload, n);
    }
    return finish(out, 5 + n);
}

size_t host_build_ack(uint8_t *out, uint16_t cmd, bool to_host)
{
    magic(out, to_host);
    out[2] = (uint8_t)(cmd >> 8);
    out[3] = (uint8_t)cmd;
    out[4] = 0xFF;
    return finish(out, 5);
}

static size_t expected_len(const uint8_t *p)
{
    if (p[4] == 0xFF) {
        return 7;
    }
    return p[3] >= 3 ? (size_t)p[3] + 4 : 0;
}

bool host_parse(const uint8_t *p, size_t n, host_pkt_t *out)
{
    uint16_t sum = 0;
    if (n < 7 || !((p[0] == 0xAA && p[1] == 0x55) || (p[0] == 0x55 && p[1] == 0xAA))) {
        return false;
    }
    if (p[4] != 0xFF && p[3] < 3) {
        return false;
    }
    if (n != expected_len(p)) {
        return false;
    }
    for (size_t i = 0; i < n - 2; i++) {
        sum += p[i];
    }
    if (sum != (uint16_t)(p[n - 2] << 8 | p[n - 1])) {
        return false;
    }
    out->cmd = (uint16_t)(p[2] << 8 | p[4]);
    out->to_host = p[0] == 0x55;
    if (p[4] == 0xFF) {
        out->acked = (uint16_t)(p[2] << 8 | p[3]);
        out->payload = p + 5;
        out->len = 0;
    } else {
        out->acked = 0;
        out->payload = p + 5;
        out->len = n - 7;
    }
    return true;
}

void host_rx_init(host_rx_t *rx)
{
    rx->n = 0;
}

bool host_rx_feed(host_rx_t *rx, uint8_t byte, host_pkt_t *out)
{
    // Accepts a valid packet that ends at this byte, wherever it starts in the buffer, so a false magic (or a
    // packet cut short) before it cannot hold it back.
    if (rx->n == sizeof rx->buf) {
        memmove(rx->buf, rx->buf + 1, --rx->n);
    }
    rx->buf[rx->n++] = byte;
    for (size_t s = 0; s + 7 <= rx->n; s++) {
        const uint8_t *b = rx->buf + s;
        if (s + expected_len(b) == rx->n && host_parse(b, rx->n - s, out)) {
            rx->n = 0;  // the packet stays in buf until the next feed
            return true;
        }
    }
    return false;
}

size_t host_alarm_payload(uint8_t *out, const uint8_t *pt, size_t pt_len, uint64_t time_ms, uint8_t rssi)
{
    size_t n;
    if (pt_len < 14) {
        return 0;
    }
    n = pt[0] == 0xAB ? 23 : 16 + (size_t)pt[13];  // 13: the data length (12: the low-battery flag, FACTS.md)
    if (n > pt_len) {
        return 0;
    }
    for (int i = 0; i < 8; i++) {
        out[i] = (uint8_t)(time_ms >> (56 - 8 * i));
    }
    memcpy(out + 8, pt, n);
    out[8 + n] = rssi;
    return 9 + n;
}

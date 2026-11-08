// Native tests for esp32/web/mdns_pkt.c (TASK-17): RFC 6762 / DNS-SD queries, announcements, compression pointers,
// negative AAAA (NSEC), QU/legacy unicast flags, and malformed/looping input rejection under ASan/UBSan.
#include <stdio.h>
#include <string.h>

#include "mdns_pkt.h"

static int checks, failures;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        checks++;                                                                    \
        if (!(cond)) {                                                               \
            failures++;                                                              \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                            \
    } while (0)

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

static size_t put_qname(uint8_t *out, const char *dotted)
{
    size_t pos = 0;
    const char *p = dotted;
    while (*p) {
        const char *dot = strchr(p, '.');
        size_t len = dot ? (size_t)(dot - p) : strlen(p);
        out[pos++] = (uint8_t)len;
        memcpy(out + pos, p, len);
        pos += len;
        if (!dot) {
            break;
        }
        p = dot + 1;
    }
    out[pos++] = 0;
    return pos;
}

static size_t make_query(uint8_t *out, uint16_t id, const char *qname, uint16_t qtype, uint16_t qclass)
{
    memset(out, 0, 12);
    out[0] = (uint8_t)(id >> 8);
    out[1] = (uint8_t)id;
    out[5] = 1;  // QDCOUNT = 1
    size_t pos = 12 + put_qname(out + 12, qname);
    out[pos++] = (uint8_t)(qtype >> 8);
    out[pos++] = (uint8_t)qtype;
    out[pos++] = (uint8_t)(qclass >> 8);
    out[pos++] = (uint8_t)qclass;
    return pos;
}

static bool has_bytes(const uint8_t *hay, size_t hlen, const uint8_t *needle, size_t nlen)
{
    if (nlen > hlen) {
        return false;
    }
    for (size_t i = 0; i + nlen <= hlen; i++) {
        if (!memcmp(hay + i, needle, nlen)) {
            return true;
        }
    }
    return false;
}

int main(void)
{
    static const uint8_t ip[4] = {192, 168, 4, 1};
    uint8_t q[256], resp[MDNS_MAX_PKT];
    bool unicast = false;

    // 1. Gratuitous announce carries 5 answers (A, PTR, SD_PTR, SRV, TXT) and 1 additional (NSEC)
    size_t n = mdns_build_announce("ows-relay-a1b2", ip, 80, resp, sizeof resp);
    CHECK(n > 120 && be16(resp) == 0 && be16(resp + 2) == 0x8400 && be16(resp + 6) == 5 && be16(resp + 10) == 1);
    CHECK(has_bytes(resp, n, ip, 4));

    // 2. Dual-question A + AAAA query using an RFC 1035 compression pointer to offset 12 (mixed case)
    size_t qlen = make_query(q, 0x1234, "OWS-Relay-A1B2.LOCAL", 1, 1);  // A, IN
    q[5] = 2;                                                           // QDCOUNT = 2
    q[qlen++] = 0xC0;
    q[qlen++] = 12;  // pointer to "OWS-Relay-A1B2.LOCAL"
    q[qlen++] = 0x00;
    q[qlen++] = 28;  // AAAA
    q[qlen++] = 0x80;
    q[qlen++] = 0x01;  // QU | IN
    n = mdns_handle_query("ows-relay-a1b2", ip, 80, q, qlen, MDNS_PORT, resp, sizeof resp, &unicast);
    CHECK(n > 40 && unicast && be16(resp) == 0 && be16(resp + 6) == 2 && be16(resp + 10) == 0);
    CHECK(has_bytes(resp, n, ip, 4));
    static const uint8_t nsec_tail[3] = {0x00, 0x01, 0x40};
    CHECK(has_bytes(resp, n, nsec_tail, sizeof nsec_tail));

    // 3. _http._tcp.local PTR browse returns PTR answer + SRV, TXT, A, NSEC additionals; legacy src_port echoes ID
    qlen = make_query(q, 0xBEEF, "_http._tcp.local", 12, 1);
    n = mdns_handle_query("ows-relay-a1b2", ip, 80, q, qlen, 54321, resp, sizeof resp, &unicast);
    CHECK(n > 100 && unicast && be16(resp) == 0xBEEF && be16(resp + 6) == 1 && be16(resp + 10) == 4);
    static const uint8_t txt_path[] = {6, 'p', 'a', 't', 'h', '=', '/'};
    CHECK(has_bytes(resp, n, txt_path, sizeof txt_path));

    // 4. DNS-SD service enumeration (_services._dns-sd._udp.local)
    qlen = make_query(q, 0, "_services._dns-sd._udp.local", 12, 1);
    n = mdns_handle_query("ows-relay-a1b2", ip, 80, q, qlen, MDNS_PORT, resp, sizeof resp, &unicast);
    CHECK(n > 40 && !unicast && be16(resp + 6) == 1);

    // 5. Reject unrelated names, DNS responses (QR=1), self-looping pointers, and truncated frames
    qlen = make_query(q, 0, "other-device.local", 1, 1);
    CHECK(mdns_handle_query("ows-relay-a1b2", ip, 80, q, qlen, MDNS_PORT, resp, sizeof resp, &unicast) == 0);

    qlen = make_query(q, 0, "ows-relay-a1b2.local", 1, 1);
    q[2] = 0x84;  // QR=1 response
    CHECK(mdns_handle_query("ows-relay-a1b2", ip, 80, q, qlen, MDNS_PORT, resp, sizeof resp, &unicast) == 0);

    // Self-looping compression pointer at offset 12 -> 12
    memset(q, 0, 18);
    q[5] = 1;
    q[12] = 0xC0;
    q[13] = 12;
    q[15] = 1;
    q[17] = 1;
    CHECK(mdns_handle_query("ows-relay-a1b2", ip, 80, q, 18, MDNS_PORT, resp, sizeof resp, &unicast) == 0);

    // Truncated packet checks
    for (size_t trunc = 0; trunc < 16; trunc++) {
        CHECK(mdns_handle_query("ows-relay-a1b2", ip, 80, q, trunc, MDNS_PORT, resp, sizeof resp, &unicast) == 0);
    }

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}

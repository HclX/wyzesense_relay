#include "mdns_pkt.h"

#include <stdio.h>
#include <string.h>

enum {
    DNS_TYPE_A = 1,
    DNS_TYPE_PTR = 12,
    DNS_TYPE_TXT = 16,
    DNS_TYPE_AAAA = 28,
    DNS_TYPE_SRV = 33,
    DNS_TYPE_NSEC = 47,
    DNS_TYPE_ANY = 255,
    DNS_CLASS_IN = 1,
    DNS_CLASS_ANY = 255,
    DNS_CACHE_FLUSH = 0x8000u,
};

enum {
    RR_A = 1u << 0,
    RR_NSEC = 1u << 1,
    RR_PTR = 1u << 2,
    RR_SRV = 1u << 3,
    RR_TXT = 1u << 4,
    RR_SD_PTR = 1u << 5,
};

static char to_lower_ascii(uint8_t c)
{
    if (c >= 'A' && c <= 'Z') {
        return (char)(c + ('a' - 'A'));
    }
    return (char)c;
}

static bool valid_host(const char *host)
{
    if (!host || !host[0]) {
        return false;
    }
    size_t n = strlen(host);
    if (n > 63) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (host[i] == '.') {
            return false;
        }
    }
    return true;
}

static void put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint16_t get_be16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

// Writes an uncompressed dotted domain name (e.g. "ows-relay-a1b2.local") as DNS labels + trailing 0x00.
static size_t write_name(uint8_t *out, size_t cap, const char *dotted)
{
    size_t pos = 0;
    const char *p = dotted;
    while (*p) {
        const char *dot = strchr(p, '.');
        size_t len = dot ? (size_t)(dot - p) : strlen(p);
        if (!len || len > 63 || pos + 1 + len + 1 > cap) {
            return 0;
        }
        out[pos++] = (uint8_t)len;
        memcpy(out + pos, p, len);
        pos += len;
        if (!dot) {
            break;
        }
        p = dot + 1;
    }
    if (pos + 1 > cap) {
        return 0;
    }
    out[pos++] = 0;
    return pos;
}

// Decodes a DNS name at `pkt[*off]` (following up to 8 RFC 1035 compression pointers) into lowercase dotted ASCII.
static bool read_name(const uint8_t *pkt, size_t pkt_len, size_t *off, char *out, size_t out_cap)
{
    size_t pos = *off, next_off = 0, out_len = 0;
    bool jumped = false;
    unsigned jumps = 0;

    for (;;) {
        if (pos >= pkt_len) {
            return false;
        }
        uint8_t b = pkt[pos];
        if (b == 0) {
            if (!jumped) {
                next_off = pos + 1;
            }
            out[out_len] = '\0';
            *off = next_off;
            return true;
        }
        if ((b & 0xC0u) == 0xC0u) {
            if (pos + 1 >= pkt_len || ++jumps > 8) {
                return false;
            }
            size_t ptr = (size_t)(b & 0x3Fu) << 8 | pkt[pos + 1];
            if (ptr >= pkt_len || ptr == pos) {
                return false;
            }
            if (!jumped) {
                next_off = pos + 2;
                jumped = true;
            }
            pos = ptr;
            continue;
        }
        if ((b & 0xC0u) != 0) {
            return false;  // reserved label encoding
        }
        pos++;
        if (pos + b > pkt_len) {
            return false;
        }
        if (out_len > 0) {
            if (out_len + 1 >= out_cap) {
                return false;
            }
            out[out_len++] = '.';
        }
        if (out_len + b >= out_cap) {
            return false;
        }
        for (uint8_t i = 0; i < b; i++) {
            out[out_len++] = to_lower_ascii(pkt[pos + i]);
        }
        pos += b;
    }
}

static size_t write_rr_header(uint8_t *out, size_t cap, const char *name, uint16_t type, uint16_t cls, uint32_t ttl,
                              uint16_t rdlen)
{
    size_t n = write_name(out, cap, name);
    if (!n || n + 10 + rdlen > cap) {
        return 0;
    }
    put_be16(out + n, type);
    put_be16(out + n + 2, cls);
    put_be32(out + n + 4, ttl);
    put_be16(out + n + 8, rdlen);
    return n + 10;
}

static bool append_rr(uint8_t *out, size_t cap, size_t *pos, uint16_t *count, unsigned rr, const char *host_fqdn,
                      const char *inst_fqdn, const uint8_t ip[4], uint16_t port)
{
    static const char svc_fqdn[] = "_http._tcp.local";
    static const char sd_fqdn[] = "_services._dns-sd._udp.local";
    uint8_t rdata[192];
    size_t n = 0, rlen = 0;

    if (rr == RR_A) {
        n = write_rr_header(out + *pos, cap - *pos, host_fqdn, DNS_TYPE_A, DNS_CLASS_IN | DNS_CACHE_FLUSH, 120, 4);
        if (!n) {
            return false;
        }
        memcpy(out + *pos + n, ip, 4);
        *pos += n + 4;
    } else if (rr == RR_NSEC) {
        // RFC 6762 §6.1 negative response for AAAA: Next Domain = host_fqdn, Type Bit Map = window 0, len 1, A(0x40).
        rlen = write_name(rdata, sizeof rdata - 3, host_fqdn);
        if (!rlen) {
            return false;
        }
        rdata[rlen++] = 0x00;
        rdata[rlen++] = 0x01;
        rdata[rlen++] = 0x40;
        n = write_rr_header(out + *pos, cap - *pos, host_fqdn, DNS_TYPE_NSEC, DNS_CLASS_IN | DNS_CACHE_FLUSH, 120,
                            (uint16_t)rlen);
        if (!n) {
            return false;
        }
        memcpy(out + *pos + n, rdata, rlen);
        *pos += n + rlen;
    } else if (rr == RR_PTR) {
        rlen = write_name(rdata, sizeof rdata, inst_fqdn);
        n = rlen ? write_rr_header(out + *pos, cap - *pos, svc_fqdn, DNS_TYPE_PTR, DNS_CLASS_IN, 1200, (uint16_t)rlen)
                 : 0;
        if (!n) {
            return false;
        }
        memcpy(out + *pos + n, rdata, rlen);
        *pos += n + rlen;
    } else if (rr == RR_SD_PTR) {
        rlen = write_name(rdata, sizeof rdata, svc_fqdn);
        n = rlen ? write_rr_header(out + *pos, cap - *pos, sd_fqdn, DNS_TYPE_PTR, DNS_CLASS_IN, 1200, (uint16_t)rlen)
                 : 0;
        if (!n) {
            return false;
        }
        memcpy(out + *pos + n, rdata, rlen);
        *pos += n + rlen;
    } else if (rr == RR_SRV) {
        put_be16(rdata, 0);
        put_be16(rdata + 2, 0);
        put_be16(rdata + 4, port);
        size_t tlen = write_name(rdata + 6, sizeof rdata - 6, host_fqdn);
        rlen = tlen ? 6 + tlen : 0;
        n = rlen ? write_rr_header(out + *pos, cap - *pos, inst_fqdn, DNS_TYPE_SRV, DNS_CLASS_IN | DNS_CACHE_FLUSH, 120,
                                   (uint16_t)rlen)
                 : 0;
        if (!n) {
            return false;
        }
        memcpy(out + *pos + n, rdata, rlen);
        *pos += n + rlen;
    } else if (rr == RR_TXT) {
        static const uint8_t txt[] = {6, 'p', 'a', 't', 'h', '=', '/'};
        n = write_rr_header(out + *pos, cap - *pos, inst_fqdn, DNS_TYPE_TXT, DNS_CLASS_IN | DNS_CACHE_FLUSH, 1200,
                            sizeof txt);
        if (!n) {
            return false;
        }
        memcpy(out + *pos + n, txt, sizeof txt);
        *pos += n + sizeof txt;
    } else {
        return false;
    }
    (*count)++;
    return true;
}

static size_t build_packet(const char *host, const uint8_t ip[4], uint16_t port, uint16_t id, unsigned ans_mask,
                           unsigned add_mask, uint8_t *out, size_t cap)
{
    if (!valid_host(host) || !ip || !out || cap < 64) {
        return 0;
    }
    char host_low[64], host_fqdn[80], inst_fqdn[96];
    size_t hlen = strlen(host);
    for (size_t i = 0; i < hlen; i++) {
        host_low[i] = to_lower_ascii((uint8_t)host[i]);
    }
    host_low[hlen] = '\0';
    snprintf(host_fqdn, sizeof host_fqdn, "%s.local", host_low);
    snprintf(inst_fqdn, sizeof inst_fqdn, "%s._http._tcp.local", host_low);

    memset(out, 0, 12);
    put_be16(out, id);
    put_be16(out + 2, 0x8400u);  // QR=1, AA=1
    size_t pos = 12;
    uint16_t ancount = 0, arcount = 0;
    static const unsigned order[] = {RR_A, RR_PTR, RR_SD_PTR, RR_SRV, RR_TXT, RR_NSEC};

    for (size_t i = 0; i < sizeof order / sizeof order[0]; i++) {
        if (ans_mask & order[i]) {
            if (!append_rr(out, cap, &pos, &ancount, order[i], host_fqdn, inst_fqdn, ip, port)) {
                return 0;
            }
        }
    }
    for (size_t i = 0; i < sizeof order / sizeof order[0]; i++) {
        if (add_mask & order[i]) {
            if (!append_rr(out, cap, &pos, &arcount, order[i], host_fqdn, inst_fqdn, ip, port)) {
                return 0;
            }
        }
    }
    if (!ancount) {
        return 0;
    }
    put_be16(out + 6, ancount);
    put_be16(out + 10, arcount);
    return pos;
}

size_t mdns_build_announce(const char *host, const uint8_t ip[4], uint16_t port, uint8_t *out, size_t cap)
{
    unsigned ans = RR_A | RR_PTR | RR_SRV | RR_TXT | RR_SD_PTR;
    return build_packet(host, ip, port, 0, ans, RR_NSEC, out, cap);
}

size_t mdns_handle_query(const char *host, const uint8_t ip[4], uint16_t port, const uint8_t *pkt, size_t pkt_len,
                         uint16_t src_port, uint8_t *out, size_t cap, bool *unicast_out)
{
    if (unicast_out) {
        *unicast_out = false;
    }
    if (!valid_host(host) || !ip || !pkt || pkt_len < 12) {
        return 0;
    }
    if ((pkt[2] & 0x80u) != 0 || (pkt[2] & 0x78u) != 0) {
        return 0;  // ignore DNS responses and non-standard opcodes
    }
    uint16_t qdcount = get_be16(pkt + 4);
    if (!qdcount || qdcount > 32) {
        return 0;
    }

    char host_low[64], host_fqdn[80], inst_fqdn[96];
    size_t hlen = strlen(host);
    for (size_t i = 0; i < hlen; i++) {
        host_low[i] = to_lower_ascii((uint8_t)host[i]);
    }
    host_low[hlen] = '\0';
    snprintf(host_fqdn, sizeof host_fqdn, "%s.local", host_low);
    snprintf(inst_fqdn, sizeof inst_fqdn, "%s._http._tcp.local", host_low);

    size_t off = 12;
    unsigned ans = 0, add = 0;
    bool unicast = src_port != MDNS_PORT;

    for (uint16_t q = 0; q < qdcount; q++) {
        char qname[128];
        if (!read_name(pkt, pkt_len, &off, qname, sizeof qname) || off + 4 > pkt_len) {
            break;
        }
        uint16_t qtype = get_be16(pkt + off);
        uint16_t qclass_raw = get_be16(pkt + off + 2);
        off += 4;
        uint16_t qclass = qclass_raw & 0x7FFFu;
        if (qclass != DNS_CLASS_IN && qclass != DNS_CLASS_ANY) {
            continue;
        }
        bool qu = (qclass_raw & 0x8000u) != 0;
        unsigned prev_ans = ans;
        if (!strcmp(qname, host_fqdn)) {
            if (qtype == DNS_TYPE_A || qtype == DNS_TYPE_ANY) {
                ans |= RR_A;
                add |= RR_NSEC;
            } else if (qtype == DNS_TYPE_AAAA) {
                ans |= RR_NSEC;
            }
        } else if (!strcmp(qname, "_http._tcp.local")) {
            if (qtype == DNS_TYPE_PTR || qtype == DNS_TYPE_ANY) {
                ans |= RR_PTR;
                add |= RR_SRV | RR_TXT | RR_A | RR_NSEC;
            }
        } else if (!strcmp(qname, inst_fqdn)) {
            if (qtype == DNS_TYPE_SRV) {
                ans |= RR_SRV;
                add |= RR_A | RR_NSEC;
            } else if (qtype == DNS_TYPE_TXT) {
                ans |= RR_TXT;
            } else if (qtype == DNS_TYPE_ANY) {
                ans |= RR_SRV | RR_TXT;
                add |= RR_A | RR_NSEC;
            }
        } else if (!strcmp(qname, "_services._dns-sd._udp.local")) {
            if (qtype == DNS_TYPE_PTR || qtype == DNS_TYPE_ANY) {
                ans |= RR_SD_PTR;
            }
        }
        if (ans != prev_ans && qu) {
            unicast = true;
        }
    }

    add &= ~ans;
    if (!ans) {
        return 0;
    }
    if (unicast_out) {
        *unicast_out = unicast;
    }
    uint16_t resp_id = src_port == MDNS_PORT ? 0 : get_be16(pkt);
    return build_packet(host, ip, port, resp_id, ans, add, out, cap);
}

// Minimal RFC 6762 (mDNS) and RFC 6763 (DNS-SD) packet responder for ESP-IDF v5.5.6 and host tests (TASK-17).
// Zero external dependencies: answers A / negative-AAAA (NSEC) queries for <host>.local and _http._tcp.local DNS-SD
// browse / SRV / TXT queries, and builds gratuitous startup announcements on 224.0.0.251:5353.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MDNS_MCAST_IPV4 "224.0.0.251"
#define MDNS_PORT       5353
#define MDNS_MAX_PKT    512

// Gratuitous mDNS announcement (A + _http._tcp PTR + SRV + TXT + _services._dns-sd._udp PTR + NSEC).
// `host` is the bare hostname without ".local" (e.g. "ows-relay-a1b2"); `ip` is 4 bytes in network order.
// Returns the packet length written to `out`, or 0 if inputs or `cap` are invalid.
size_t mdns_build_announce(const char *host, const uint8_t ip[4], uint16_t port, uint8_t *out, size_t cap);

// Evaluates an incoming UDP mDNS query `pkt[0..pkt_len)` from UDP port `src_port` and writes a DNS response into
// `out[0..cap)`. Sets `*unicast_out = true` when a unicast reply was requested (QU bit or legacy source port != 5353).
// Returns 0 when the packet is a response, malformed, or asks for names/types not owned by `host`.
size_t mdns_handle_query(const char *host, const uint8_t ip[4], uint16_t port, const uint8_t *pkt, size_t pkt_len,
                         uint16_t src_port, uint8_t *out, size_t cap, bool *unicast_out);

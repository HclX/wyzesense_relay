// Wi-Fi: a station on the configured network; the setup access point "OWS-relay-XXXX" (open, 192.168.4.1) when no
// network is configured or it cannot be joined (the station keeps trying alongside).
#pragma once

#include <stdbool.h>

void net_start(void);
bool net_connected(void);         // the station has an IP address
const char *net_ip(void);         // the station's IP address, or ""
bool net_setup_ap(void);          // the setup access point is up
const char *net_name(void);       // OWS-relay-XXXX: the access point's SSID and the host name
const char *net_mdns_host(void);  // ows-relay-xxxx.local: advertised over RFC 6762 mDNS / DNS-SD

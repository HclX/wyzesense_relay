#include "net.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mdns_pkt.h"
#include "settings.h"

#define AP_DELAY_MS     120000
#define AP_TIMEOUT_MS   900000
#define BACKOFF_BASE_MS 1000
#define BACKOFF_MAX_MS  30000

static const char *TAG = "net";
static bool connected, ap_up, ap_timed_out;
static int fails, mdns_sock = -1;
static unsigned mdns_announces;
static char ip[16], name[24], mdns_bare[24], mdns_fqdn[32];
static uint8_t sta_ip[4], mdns_rx[MDNS_MAX_PKT], mdns_tx[MDNS_MAX_PKT];
static esp_timer_handle_t reconnect_timer, ap_timer, mdns_timer;

static void mdns_send_announce(void)
{
    if (mdns_sock < 0 || !connected) {
        return;
    }
    size_t n = mdns_build_announce(mdns_bare, sta_ip, 80, mdns_tx, sizeof mdns_tx);
    if (!n) {
        return;
    }
    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(MDNS_PORT);
    dst.sin_addr.s_addr = inet_addr(MDNS_MCAST_IPV4);
    sendto(mdns_sock, mdns_tx, n, 0, (struct sockaddr *)&dst, sizeof dst);
}

static void on_mdns_timer(void *arg)
{
    (void)arg;
    if (!connected || mdns_sock < 0) {
        return;
    }
    if (mdns_announces > 0 && (--mdns_announces == 10 || mdns_announces == 0)) {
        mdns_send_announce();
    }
    for (int i = 0; i < 8; i++) {
        struct sockaddr_in from = {0};
        socklen_t flen = sizeof from;
        ssize_t r = recvfrom(mdns_sock, mdns_rx, sizeof mdns_rx, MSG_DONTWAIT, (struct sockaddr *)&from, &flen);
        if (r <= 0) {
            break;
        }
        bool unicast = false;
        size_t n = mdns_handle_query(mdns_bare, sta_ip, 80, mdns_rx, (size_t)r, ntohs(from.sin_port), mdns_tx,
                                     sizeof mdns_tx, &unicast);
        if (!n) {
            continue;
        }
        struct sockaddr_in dst = from;
        if (!unicast) {
            memset(&dst, 0, sizeof dst);
            dst.sin_family = AF_INET;
            dst.sin_port = htons(MDNS_PORT);
            dst.sin_addr.s_addr = inet_addr(MDNS_MCAST_IPV4);
        }
        sendto(mdns_sock, mdns_tx, n, 0, (struct sockaddr *)&dst, sizeof dst);
    }
}

static void mdns_init(void)
{
    mdns_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (mdns_sock < 0) {
        ESP_LOGW(TAG, "mDNS socket failed");
        return;
    }
    int yes = 1, ttl = 255;
    uint8_t loop = 0;
    setsockopt(mdns_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    setsockopt(mdns_sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
    setsockopt(mdns_sock, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof loop);
    fcntl(mdns_sock, F_SETFL, O_NONBLOCK);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(MDNS_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(mdns_sock, (struct sockaddr *)&addr, sizeof addr) != 0) {
        ESP_LOGW(TAG, "mDNS bind :5353 failed");
        close(mdns_sock);
        mdns_sock = -1;
        return;
    }
    const esp_timer_create_args_t targs = {.callback = on_mdns_timer, .name = "mdns"};
    if (esp_timer_create(&targs, &mdns_timer) == ESP_OK) {
        esp_timer_start_periodic(mdns_timer, 100000);
    }
}

static void start_ap(void)
{
    if (ap_up) {
        return;
    }
    wifi_config_t ap = {0};
    strncpy((char *)ap.ap.ssid, name, sizeof ap.ap.ssid);
    ap.ap.ssid_len = (uint8_t)strlen(name);
    ap.ap.channel = 1;
    ap.ap.max_connection = 2;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    ap_up = true;
    ESP_LOGW(TAG, "setup access point %s up: http://192.168.4.1/", name);
}

static void stop_ap(void)
{
    if (!ap_up) {
        return;
    }
    esp_wifi_set_mode(WIFI_MODE_STA);
    ap_up = false;
    ESP_LOGI(TAG, "setup access point %s stopped", name);
}

static void on_reconnect_timer(void *arg)
{
    (void)arg;
    if (!connected && settings.ssid[0]) {
        esp_wifi_connect();
    }
}

static void on_ap_timer(void *arg)
{
    (void)arg;
    if (!settings.ssid[0] || connected) {
        return;
    }
    if (!ap_up && !ap_timed_out) {
        start_ap();
        esp_timer_start_once(ap_timer, (uint64_t)AP_TIMEOUT_MS * 1000);
    } else if (ap_up) {
        stop_ap();
        ap_timed_out = true;
    }
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START && settings.ssid[0]) {
        esp_timer_start_once(ap_timer, (uint64_t)AP_DELAY_MS * 1000);
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        ESP_LOGW(TAG, "not connected to %s: reason %u (201 no such network, 15 or 204 wrong password)", settings.ssid,
                 d->reason);
        connected = false;
        ip[0] = 0;
        if (settings.ssid[0]) {
            if (!ap_up && !ap_timed_out && !esp_timer_is_active(ap_timer)) {
                esp_timer_start_once(ap_timer, (uint64_t)AP_DELAY_MS * 1000);
            }
            uint32_t delay_ms = (uint32_t)BACKOFF_BASE_MS << (fails < 5 ? fails : 5);
            if (delay_ms > BACKOFF_MAX_MS) {
                delay_ms = BACKOFF_MAX_MS;
            }
            if (fails < 30) {
                fails++;
            }
            esp_timer_stop(reconnect_timer);
            esp_timer_start_once(reconnect_timer, (uint64_t)delay_ms * 1000);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(ip, sizeof ip, IPSTR, IP2STR(&e->ip_info.ip));
        sta_ip[0] = esp_ip4_addr1(&e->ip_info.ip);
        sta_ip[1] = esp_ip4_addr2(&e->ip_info.ip);
        sta_ip[2] = esp_ip4_addr3(&e->ip_info.ip);
        sta_ip[3] = esp_ip4_addr4(&e->ip_info.ip);
        connected = true;
        fails = 0;
        ap_timed_out = false;
        esp_timer_stop(reconnect_timer);
        esp_timer_stop(ap_timer);
        stop_ap();
        if (mdns_sock >= 0) {
            struct ip_mreq mreq = {0};
            mreq.imr_multiaddr.s_addr = inet_addr(MDNS_MCAST_IPV4);
            mreq.imr_interface.s_addr = htonl(INADDR_ANY);
            setsockopt(mdns_sock, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq, sizeof mreq);
            mreq.imr_interface.s_addr = e->ip_info.ip.addr;
            setsockopt(mdns_sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof mreq);
            mdns_send_announce();
            mdns_announces = 20;
        }
        ESP_LOGI(TAG, "connected to %s as %s (http://%s/)", settings.ssid, ip, mdns_fqdn);
    }
}

void net_start(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(name, sizeof name, "OWS-relay-%02X%02X", mac[4], mac[5]);
    snprintf(mdns_bare, sizeof mdns_bare, "ows-relay-%02x%02x", mac[4], mac[5]);
    snprintf(mdns_fqdn, sizeof mdns_fqdn, "%s.local", mdns_bare);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta = esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(sta, mdns_bare);
    mdns_init();
    const esp_timer_create_args_t rec_args = {.callback = on_reconnect_timer, .name = "wifi_rec"};
    const esp_timer_create_args_t ap_args = {.callback = on_ap_timer, .name = "wifi_ap"};
    ESP_ERROR_CHECK(esp_timer_create(&rec_args, &reconnect_timer));
    ESP_ERROR_CHECK(esp_timer_create(&ap_args, &ap_timer));
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));
    wifi_config_t sta_cfg = {0};
    strncpy((char *)sta_cfg.sta.ssid, settings.ssid, sizeof sta_cfg.sta.ssid);
    strncpy((char *)sta_cfg.sta.password, settings.pass, sizeof sta_cfg.sta.password);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    if (!settings.ssid[0]) {
        start_ap();
    }
}

bool net_connected(void)
{
    return connected;
}

const char *net_ip(void)
{
    return ip;
}

bool net_setup_ap(void)
{
    return ap_up;
}

const char *net_name(void)
{
    return name;
}

const char *net_mdns_host(void)
{
    return mdns_fqdn;
}

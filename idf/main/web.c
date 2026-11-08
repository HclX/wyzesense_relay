#include "web.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cjtag_task.h"
#include "contact.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "http_auth.h"
#include "multipart.h"
#include "net.h"
#include "relay_task.h"
#include "rig_task.h"
#include "settings.h"
#include "pins.h"

#define MAX_CONTACT_IMAGE 0x1E000  // pages 0-29

static const char *TAG = "web";
#if CONFIG_IDF_TARGET_ESP32S3
#define CHIP    "ESP32-S3"
#define FW_FILE "idf/build/ows_relay.bin"
#else
#define CHIP    "ESP32"
#define FW_FILE "idf/build-esp32/ows_relay.bin"
#endif
// found by the web page in an uploaded image (web/index.html): the relay's version
static const char relay_tag[] = "OWS-RELAY-VERSION:" OWS_RELAY_VERSION;
static char last_flash[160] = "none since start";

// the relays' shared page (web/index.html, EMBED_TXTFILES: NUL-terminated)
extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

static esp_err_t page_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);
}

// a JSON string of s into out: quotes, backslashes and control characters escaped
static void js(char *out, size_t cap, const char *s)
{
    size_t n = 0;
    out[n++] = '"';
    for (; *s && n + 8 < cap; s++) {
        if (*s == '"' || *s == '\\') {
            out[n++] = '\\', out[n++] = *s;
        } else if ((unsigned char)*s < 0x20) {
            n += (size_t)snprintf(out + n, cap - n, "\\u%04x", (unsigned char)*s);
        } else {
            out[n++] = *s;
        }
    }
    out[n++] = '"';
    out[n] = 0;
}

// what the page shows (matching the ESP8266 build's /api/status)
static esp_err_t api_status(httpd_req_t *req)
{
    static const char *states[] = {"down", "handshake", "connected"};
    static const char *results[] = {"same", "updated", "failed", "refused"};
    char *body = malloc(6144);
    if (!body) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }
    char name[48], mdns[48], ssid[80], url[300], ip[40], flash[340], build[80], gwmsg[340], carried[120], running[120];
    char log_raw[128] = "", log_js[280], gw_mac[9] = "", cmac_js[24];
    const gwsync_report_t *g = (settings.role == ROLE_STATION) ? NULL : contact_gwsync_report();
    char steps_raw[760] = "", steps[900];
    if (settings.role != ROLE_STATION) {
        contact_gwsync_steps(steps_raw, sizeof steps_raw);
    }
    js(steps, sizeof steps, steps_raw);
    const esp_partition_t *run = esp_ota_get_running_partition();
    wifi_ap_record_t ap;
    int rssi = net_connected() && esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
    uint32_t queued = 0, replayed = 0, dropped = 0, sns_now = 0, uart_tx = 0, uart_rx = 0;
    unsigned waiting = 0, n_sns = 0;
    buffer_sensor_t sns[BUFFER_SENSORS];
    if (settings.role != ROLE_STATION) {
        n_sns = relay_task_sensors(sns, BUFFER_SENSORS, &sns_now);
        relay_task_buffer(&queued, &replayed, &dropped, &waiting);
        contact_uart_counts(&uart_tx, &uart_rx);
        relay_task_last_log(log_raw, sizeof log_raw);
        relay_task_gateway_mac(gw_mac);
    } else {
        snprintf(log_raw, sizeof log_raw, "station-only mode");
    }
    snprintf(build, sizeof build, "%s (%s)", relay_tag + 18, run ? run->label : "?");
    js(gwmsg, sizeof gwmsg,
       g ? g->msg : (settings.role == ROLE_STATION ? "provisioning station (the relay is off)" : ""));
    carried[0] = running[0] = 0;
    if (g) {
        char v[96];
        js(v, sizeof v, g->carried.version);
        snprintf(carried, sizeof carried, "{\"version\":%.40s,\"id\":\"%02x%02x%02x%02x%02x%02x%02x%02x\"}", v,
                 g->carried.id[0], g->carried.id[1], g->carried.id[2], g->carried.id[3], g->carried.id[4],
                 g->carried.id[5], g->carried.id[6], g->carried.id[7]);
        js(v, sizeof v, g->running.version);
        snprintf(running, sizeof running, "{\"version\":%.40s,\"id\":\"%02x%02x%02x%02x%02x%02x%02x%02x\"}", v,
                 g->running.id[0], g->running.id[1], g->running.id[2], g->running.id[3], g->running.id[4],
                 g->running.id[5], g->running.id[6], g->running.id[7]);
    }
    js(name, sizeof name, net_name());
    js(mdns, sizeof mdns, net_mdns_host());
    js(ssid, sizeof ssid, settings.ssid);
    js(url, sizeof url, settings.url);
    js(ip, sizeof ip, net_connected() ? net_ip() : "");
    js(log_js, sizeof log_js, log_raw);
    js(flash, sizeof flash, last_flash);
    char wr_js[40], wt_js[40];
    js(wr_js, sizeof wr_js, settings.wire_r);
    js(wt_js, sizeof wt_js, settings.wire_t);
    js(cmac_js, sizeof cmac_js, g && g->mac[0] ? g->mac : gw_mac);
    static const char *vias[] = {"auto", "cjtag", "sbl"};  // GWSYNC_VIA_*
    bool gw_checked = (g != NULL) || (settings.role == ROLE_STATION);
    const char *relay_state = (settings.role == ROLE_STATION) ? "down" : states[relay_task_state()];
    int len = snprintf(
        body, 6144,
        "{\"name\":%s,\"mdns\":%s,\"ssid\":%s,\"url\":%s,\"ip\":%s,\"ap\":%s,\"auth\":%s,"
        "\"role\":%u,\"skip_gwsync\":%s,\"gw_fw\":\"%s\",\"stock_available\":%s,\"contact_flash\":\"%s\",\"rssi\":%d,"
        "\"relay\":\"%s\",\"connects\":%"
        "lu,\"queued\":%lu,"
        "\"replayed\":%lu,\"dropped\":%lu,\"waiting\":%u,\"uart_tx\":%lu,\"uart_rx\":%lu,"
        "\"uptime\":%lu,\"heap\":%lu,\"log\":%s,\"flash\":%s,\"contact_mac\":%s,"
        "\"wires\":{\"r\":%s,\"t\":%s},\"pins\":{\"power\":%d,\"r\":%d,\"t\":%d,\"m\":%d,\"c\":%d,\"s\":%d,\"w\":%d},"
        "\"build\":\"%s\",\"chip\":\"" CHIP "\",\"fw_file\":\"" FW_FILE "\","
        "\"gw\":{\"checked\":%s,\"result\":\"%s\",\"via_cjtag\":%s,\"msg\":%s,\"steps\":%s,\"carried\":%s,"
        "\"running\":%s},\"sensors\":[",
        name, mdns, ssid, url, ip, net_setup_ap() ? "true" : "false",
        http_auth_secret(settings.admin_pass, settings.token) ? "true" : "false", (unsigned)settings.role,
        settings.skip_gwsync ? "true" : "false", settings.stock_gw ? "stock" : "ows",
        contact_stock_gw_available() ? "true" : "false", vias[settings.contact_flash % 3], rssi, relay_state,
        (unsigned long)(settings.role == ROLE_STATION ? 0 : relay_task_connects()), (unsigned long)queued,
        (unsigned long)replayed, (unsigned long)dropped, waiting, (unsigned long)uart_tx, (unsigned long)uart_rx,
        (unsigned long)(esp_timer_get_time() / 1000000), (unsigned long)esp_get_free_heap_size(), log_js, flash,
        cmac_js, wr_js, wt_js, OWS_PIN_CONTACT_POWER, OWS_PIN_CONTACT_TX, OWS_PIN_CONTACT_RX, OWS_PIN_CONTACT_TMS,
        OWS_PIN_CONTACT_TCK, OWS_PIN_CONTACT_RESET, OWS_PIN_CONTACT_BOOT, build, gw_checked ? "true" : "false",
        g ? results[g->result] : "same", g && g->via_cjtag ? "true" : "false", gwmsg, steps, g ? carried : "null",
        g && (g->answered || g->result == GWSYNC_UPDATED) ? running : "null");
    size_t pos = len > 0 && len < 6140 ? (size_t)len : 0;
    for (unsigned i = 0; i < n_sns && pos + 96 < 6144; i++) {
        uint32_t seen_s = (uint32_t)(sns_now - sns[i].last_seen_ms) / 1000u;
        pos += (size_t)snprintf(body + pos, 6144 - pos,
                                "%s{\"mac\":\"%.8s\",\"type\":%u,\"rssi\":%d,\"bat_mv\":%u,\"seen_s\":%lu}",
                                i ? "," : "", sns[i].mac, (unsigned)sns[i].type, (int)sns[i].last_rssi,
                                (unsigned)sns[i].last_bat_mv, (unsigned long)seen_s);
    }
    snprintf(body + pos, 6144 - pos, "]}");
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, body);
    free(body);
    return err;
}

// a form field's value, URL-decoded, from an application/x-www-form-urlencoded body
static bool form_value(const char *body, const char *key, char *out, size_t cap)
{
    size_t kl = strlen(key);
    for (const char *p = body; p && *p; p = strchr(p, '&') ? strchr(p, '&') + 1 : NULL) {
        if (strncmp(p, key, kl) != 0 || p[kl] != '=') {
            continue;
        }
        size_t n = 0;
        for (p += kl + 1; *p && *p != '&' && n + 1 < cap; p++) {
            if (*p == '+') {
                out[n++] = ' ';
            } else if (*p == '%' && p[1] && p[2]) {
                char h[3] = {p[1], p[2], 0};
                out[n++] = (char)strtol(h, NULL, 16);
                p += 2;
            } else {
                out[n++] = *p;
            }
        }
        out[n] = 0;
        return true;
    }
    return false;
}

static void restart_soon(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

// a setting from the form, decoded to at most its size; an empty or missing field leaves it as it was (a script can
// change the bridge alone)
static void set_field(const char *body, const char *key, char *field, size_t cap, bool trim)
{
    char v[160];
    if (!form_value(body, key, v, cap < sizeof v ? cap : sizeof v)) {
        return;
    }
    char *s = v;
    if (trim) {  // a phone keyboard's trailing space makes an SSID nobody has
        while (*s == ' ' || *s == '\t') {
            s++;
        }
        for (size_t n = strlen(s); n && (s[n - 1] == ' ' || s[n - 1] == '\t'); n--) {
            s[n - 1] = 0;
        }
    }
    if (s[0]) {
        memmove(field, s, strlen(s) + 1);
    }
}

// while the start-up gateway check runs (it may be writing the contact), nothing that restarts the relay
static bool refused_busy(httpd_req_t *req)
{
    if (settings.role == ROLE_STATION || contact_gwsync_report()) {
        return false;
    }
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_sendstr(req, "the start-up gateway check is running: try again in a moment");
    return true;
}

static bool refused_relay_disabled(httpd_req_t *req)
{
    if (settings.role != ROLE_STATION) {
        return false;
    }
    httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "the relay: set the role to relay (setup)");
    return true;
}

static bool refused_station_disabled(httpd_req_t *req)
{
    if (settings.role == ROLE_STATION) {
        return false;
    }
    httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "the provisioning station: set the role to station (setup)");
    return true;
}

static bool require_auth(httpd_req_t *req)
{
    char hdr[256] = "";
    httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof hdr);
    if (http_auth_ok(hdr, settings.admin_pass, settings.token)) {
        return true;
    }
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"OWS Relay\"");
    httpd_resp_sendstr(req, "authentication required");
    return false;
}

static esp_err_t setup_post(httpd_req_t *req)
{
    if (!require_auth(req) || refused_busy(req)) {
        return ESP_OK;
    }
    char body[768];
    int n = req->content_len < (int)sizeof body - 1 ? httpd_req_recv(req, body, req->content_len) : -1;
    if (n <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "form too long");
    }
    body[n] = 0;
    set_field(body, "ssid", settings.ssid, sizeof settings.ssid, true);
    set_field(body, "pass", settings.pass, sizeof settings.pass, false);
    set_field(body, "url", settings.url, sizeof settings.url, true);
    set_field(body, "token", settings.token, sizeof settings.token, true);
    set_field(body, "admin_pass", settings.admin_pass, sizeof settings.admin_pass, false);
    char skip[16] = "", via[8] = "", gw_fw[16] = "";
    if (form_value(body, "skip_gwsync", skip, sizeof skip)) {
        settings.skip_gwsync = skip[0] == '1' || !strcmp(skip, "true") || !strcmp(skip, "on");
    }
    if (form_value(body, "gw_fw", gw_fw, sizeof gw_fw) && gw_fw[0]) {
        settings.stock_gw = !strcmp(gw_fw, "stock") || gw_fw[0] == '1';
    }
    if (form_value(body, "contact_flash", via, sizeof via)) {  // how gwsync reflashes the contact (#81)
        settings.contact_flash = !strcmp(via, "cjtag") ? GWSYNC_VIA_CJTAG
                                 : !strcmp(via, "sbl") ? GWSYNC_VIA_SBL
                                                       : GWSYNC_VIA_AUTO;
    }
    char role_s[16] = "";
    if (form_value(body, "role", role_s, sizeof role_s) && role_s[0]) {
        settings.role = !strcmp(role_s, "2") || !strcmp(role_s, "station") ? ROLE_STATION : ROLE_RELAY;
    }
    if (!settings_save()) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "saving failed");
    }
    httpd_resp_sendstr(req, "saved; restarting");
    xTaskCreate(restart_soon, "restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static esp_err_t contact_gwsync_post(httpd_req_t *req)
{
    if (!require_auth(req) || refused_relay_disabled(req)) {
        return ESP_OK;
    }
    char query[64] = "", body[64] = "", skip[16] = "";
    httpd_req_get_url_query_str(req, query, sizeof query);
    if (httpd_query_key_value(query, "skip", skip, sizeof skip) != ESP_OK && req->content_len > 0 &&
        req->content_len < (int)sizeof body) {
        int n = httpd_req_recv(req, body, req->content_len);
        if (n > 0) {
            body[n] = 0;
            form_value(body, "skip", skip, sizeof skip);
        }
    }
    settings.skip_gwsync = skip[0] == '1' || !strcmp(skip, "true") || !strcmp(skip, "on");
    if (!settings_save()) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "saving failed");
    }
    return httpd_resp_sendstr(req, settings.skip_gwsync ? "startup auto-sync paused" : "startup auto-sync enabled");
}

// The battery rig (#95, rig_task.c). GET /api/rig: state and results. POST /rig (form): enable=1|0 (kept in the
// settings; restarts: the contact's + pad must then be on OWS_PIN_RIG_SUPPLY, esp32/README.md), measure=1
// (fresh counters, with any of cap_nf v_low_mv v_high_mv v_busy_mv v_hold_mv v_release_mv pulse_us hold_us) or 0.
static esp_err_t rig_get(httpd_req_t *req)
{
    char out[768];
    size_t n = rig_task_json(out, sizeof out);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, out, (ssize_t)n);
}

static void rig_field(const char *body, const char *key, uint32_t *v)
{
    char s[16];
    if (form_value(body, key, s, sizeof s) && s[0]) {
        *v = (uint32_t)strtoul(s, NULL, 10);
    }
}

// The colours of the harness's R and T wires (POST /wires r=..&t=..; no restart): the page names a probe by its colour
static esp_err_t wires_post(httpd_req_t *req)
{
    if (!require_auth(req)) {
        return ESP_OK;
    }
    char body[128];
    int n =
        req->content_len > 0 && req->content_len < (int)sizeof body ? httpd_req_recv(req, body, req->content_len) : -1;
    if (n <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "r=<colour>&t=<colour>");
    }
    body[n] = 0;
    set_field(body, "r", settings.wire_r, sizeof settings.wire_r, true);
    set_field(body, "t", settings.wire_t, sizeof settings.wire_t, true);
    if (!strcmp(settings.wire_r, "-")) {  // "-": back to the default (an empty field is left unchanged)
        settings.wire_r[0] = 0;
    }
    if (!strcmp(settings.wire_t, "-")) {
        settings.wire_t[0] = 0;
    }
    if (!settings_save()) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "saving failed");
    }
    return httpd_resp_sendstr(req, "wire colours saved");
}

static esp_err_t rig_post(httpd_req_t *req)
{
    if (!require_auth(req) || refused_relay_disabled(req)) {
        return ESP_OK;
    }
    char body[400];
    int n = req->content_len < (int)sizeof body - 1 ? httpd_req_recv(req, body, req->content_len) : -1;
    if (n <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "form missing or too long");
    }
    body[n] = 0;
    char v[8] = "";
    if (form_value(body, "enable", v, sizeof v) && v[0]) {
        settings.rig = v[0] == '1';
        if (!settings_save()) {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "saving failed");
        }
        httpd_resp_sendstr(req, settings.rig ? "rig enabled; restarting" : "rig disabled; restarting");
        xTaskCreate(restart_soon, "restart", 2048, NULL, 5, NULL);
        return ESP_OK;
    }
    if (!form_value(body, "measure", v, sizeof v) || !v[0]) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "enable=0|1 or measure=0|1");
    }
    if (!rig_task_available()) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "the rig is not enabled (enable=1)");
    }
    rig_cfg_t cfg;
    rig_task_cfg(&cfg);
    uint32_t lo = cfg.v_low_mv, hi = cfg.v_high_mv, busy = cfg.v_busy_mv, hold = cfg.v_hold_mv, rel = cfg.v_release_mv;
    rig_field(body, "cap_nf", &cfg.cap_nf);
    rig_field(body, "v_low_mv", &lo);
    rig_field(body, "v_high_mv", &hi);
    rig_field(body, "v_busy_mv", &busy);
    rig_field(body, "v_hold_mv", &hold);
    rig_field(body, "v_release_mv", &rel);
    rig_field(body, "pulse_us", &cfg.pulse_us);
    rig_field(body, "hold_us", &cfg.hold_us);
    // the CC1310 browns out near 1.8 V and our firmware refuses flash writes below 2.5 V: keep the board above
    if (hold < 2000 || hold >= busy || busy >= lo || lo >= hi || hi > 3300 || rel > 3300 || !cfg.pulse_us ||
        cfg.pulse_us > 1000 || !cfg.hold_us || cfg.hold_us > 5000 || !cfg.cap_nf) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "need 2000 <= v_hold < v_busy < v_low < v_high <= 3300 mV, pulse 1-1000 us, "
                                   "hold 1-5000 us, cap_nf > 0");
    }
    cfg.v_low_mv = (uint16_t)lo, cfg.v_high_mv = (uint16_t)hi, cfg.v_busy_mv = (uint16_t)busy;
    cfg.v_hold_mv = (uint16_t)hold, cfg.v_release_mv = (uint16_t)rel;
    rig_task_measure(v[0] == '1', &cfg);
    return rig_get(req);
}

// POST /contact/console?wait_ms=N (body: one line, sent with a newline; empty: just listen): what the contact prints
// within N ms (default 800, at most 5000), raw. For a contact running our sensor firmware, whose console text the
// relay does not forward (only host-protocol packets): the battery rig's sensor (#95). Two newlines go first: a
// sensor whose console window ended wakes on the first byte and may drop it.
static esp_err_t contact_console_post(httpd_req_t *req)
{
    if (!require_auth(req) || refused_relay_disabled(req) || refused_busy(req)) {
        return ESP_OK;
    }
    char query[32] = "", w[8] = "";
    httpd_req_get_url_query_str(req, query, sizeof query);
    uint32_t wait_ms = 800;
    if (httpd_query_key_value(query, "wait_ms", w, sizeof w) == ESP_OK) {
        wait_ms = (uint32_t)strtoul(w, NULL, 10);
        wait_ms = wait_ms > 5000 ? 5000 : wait_ms;
    }
    char line[96];
    int n = req->content_len > 0 && req->content_len < (int)sizeof line - 1
                ? httpd_req_recv(req, line, req->content_len)
                : 0;
    n = n < 0 ? 0 : n;
    if (!contact_uart_take(2000)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "the contact is busy");
    }
    contact_uart_flush_input();
    contact_uart_write((const uint8_t *)"\n", 1);  // wakes a sleeping sensor's UART (this byte may be lost)
    vTaskDelay(pdMS_TO_TICKS(150));
    contact_uart_write((const uint8_t *)"\n", 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    if (n > 0) {
        contact_uart_write((const uint8_t *)line, (size_t)n);
        contact_uart_write((const uint8_t *)"\n", 1);
    }
    static char out[2048];
    size_t got = 0;
    int64_t end = esp_timer_get_time() + (int64_t)wait_ms * 1000;
    while (got < sizeof out && esp_timer_get_time() < end) {
        got += contact_uart_read((uint8_t *)out + got, sizeof out - got, 50);
    }
    contact_uart_give();
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, out, (ssize_t)got);
}

// Bring-up (the stock-dongle-firmware experiment): POST /debug/cjtag_page?page=N, body: 4096 bytes -> that page written
// through the debug port (pages 0-30; page 31 never). POST /debug/capture?ms=M&at=A, body: bytes sent A ms after the
// start -> the contact restarted and its UART for M ms (at most 10000), as hex.
static esp_err_t debug_cjtag_page_post(httpd_req_t *req)
{
    if (!require_auth(req) || refused_relay_disabled(req) || refused_busy(req)) {
        return ESP_OK;
    }
    static uint8_t page[4096];
    char query[32] = "", v[8] = "", err[120] = "", mac[9] = "", out[200];
    httpd_req_get_url_query_str(req, query, sizeof query);
    int n = httpd_query_key_value(query, "page", v, sizeof v) == ESP_OK ? atoi(v) : -1;
    if (n < 0 || n > 30 || req->content_len != (int)sizeof page) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "page=0..30 and a 4096-byte body");
    }
    for (int got = 0; got < (int)sizeof page;) {
        int r = httpd_req_recv(req, (char *)page + got, sizeof page - got);
        if (r <= 0) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "short body");
        }
        got += r;
    }
    if (!contact_uart_take(2000)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "the contact is busy");
    }
    int r = contact_cjtag_page((uint32_t)n, page, mac, err, sizeof err);
    contact_uart_give();
    snprintf(out, sizeof out, r > 0 ? "page %d written and verified (contact %s)\n" : "page %d failed: %s\n", n,
             r > 0 ? mac : err);
    return httpd_resp_sendstr(req, out);
}

static esp_err_t debug_capture_post(httpd_req_t *req)
{
    if (!require_auth(req) || refused_relay_disabled(req) || refused_busy(req)) {
        return ESP_OK;
    }
    static uint8_t send[256], got[4096];
    static char hex[2 * sizeof got + 64];
    char query[48] = "", v[8] = "";
    httpd_req_get_url_query_str(req, query, sizeof query);
    uint32_t ms = httpd_query_key_value(query, "ms", v, sizeof v) == ESP_OK ? (uint32_t)atoi(v) : 2000;
    uint32_t at = httpd_query_key_value(query, "at", v, sizeof v) == ESP_OK ? (uint32_t)atoi(v) : 0;
    ms = ms > 10000 ? 10000 : ms;
    int n = req->content_len > 0 && req->content_len <= (int)sizeof send
                ? httpd_req_recv(req, (char *)send, req->content_len)
                : 0;
    size_t g = contact_capture(ms, send, n > 0 ? (size_t)n : 0, at, got, sizeof got);
    size_t k = (size_t)snprintf(hex, sizeof hex, "%u bytes:", (unsigned)g);
    for (size_t i = 0; i < g; i++) {
        k += (size_t)snprintf(hex + k, sizeof hex - k, " %02x", got[i]);
    }
    hex[k++] = '\n';
    return httpd_resp_send(req, hex, (ssize_t)k);
}

// Bring-up: POST /debug/sample?wait=W&n=N&gap=G&addr=0x..&len=L&restart=0|1 (see contact_sample)
static esp_err_t debug_sample_post(httpd_req_t *req)
{
    if (!require_auth(req) || refused_relay_disabled(req) || refused_busy(req)) {
        return ESP_OK;
    }
    static char out[3072];
    char query[128] = "", v[16] = "";
    httpd_req_get_url_query_str(req, query, sizeof query);
#define QV(name, def) (httpd_query_key_value(query, name, v, sizeof v) == ESP_OK ? strtoul(v, NULL, 0) : (def))
    uint32_t wait = QV("wait", 2000), gap = QV("gap", 50), addr = QV("addr", 0), len = QV("len", 0);
    int n = (int)QV("n", 4);
    bool rst = QV("restart", 1) != 0;
#undef QV
    size_t k = contact_sample(wait > 20000 ? 20000 : wait, n > 20 ? 20 : n, gap, addr, len, rst, out, sizeof out);
    return httpd_resp_send(req, out, (ssize_t)k);
}

// Bring-up: GET /debug/page31 -> the contact's page 31 (4096 bytes, read twice). POST /debug/page31 (body: 0xFA8
// bytes) -> its code area written, the CCFG kept (cjtag_write_page31_code); the page as it was comes back.
static esp_err_t debug_page31_handler(httpd_req_t *req)
{
    if (!require_auth(req) || refused_relay_disabled(req) || refused_busy(req)) {
        return ESP_OK;
    }
    static uint8_t code[0xFA8], page[4096];
    char err[120] = "";
    int op = req->method == HTTP_POST;
    if (op) {
        if (req->content_len != (int)sizeof code) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "a 4008-byte body (0x1F000-0x1FFA7)");
        }
        for (int got = 0; got < (int)sizeof code;) {
            int r = httpd_req_recv(req, (char *)code + got, sizeof code - got);
            if (r <= 0) {
                return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "short body");
            }
            got += r;
        }
    }
    if (!contact_uart_take(2000)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "the contact is busy");
    }
    int r = contact_page31(op, code, page, err, sizeof err);
    contact_uart_give();
    if (r < 0) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, err);
    }
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "X-Result", r > 0 ? (op ? "written" : "read") : "already");
    return httpd_resp_send(req, (const char *)page, sizeof page);
}

static esp_err_t contact_reset_post(httpd_req_t *req)
{
    if (!require_auth(req) || refused_relay_disabled(req)) {
        return ESP_OK;
    }
    if (!contact_uart_take(2000)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "the contact is busy");
    }
    contact_reset();
    contact_uart_give();
    return httpd_resp_sendstr(req, "contact reset");
}

typedef bool (*sink_fn)(void *ctx, const uint8_t *data, size_t len);

// The request body into sink as it comes: the named field of a multipart form (the web page), or the body itself
// (a script: curl --data-binary). False if receiving failed, the sink stopped, or a form lacked the field.
static bool receive(httpd_req_t *req, const char *field, sink_fn sink, void *ctx)
{
    static mp_t mp;  // one upload at a time (the server has one task)
    char ctype[120] = "", buf[1024];
    httpd_req_get_hdr_value_str(req, "Content-Type", ctype, sizeof ctype);
    bool form = !strncmp(ctype, "multipart/form-data", 19);
    if (form && !mp_init(&mp, ctype, field, sink, ctx)) {
        return false;
    }
    for (int left = req->content_len; left > 0;) {
        int n = httpd_req_recv(req, buf, left < (int)sizeof buf ? (size_t)left : sizeof buf);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0 ||
            !(form ? mp_feed(&mp, (const uint8_t *)buf, (size_t)n) : sink(ctx, (const uint8_t *)buf, (size_t)n))) {
            return false;
        }
        left -= n;
    }
    return !form || mp_complete(&mp);
}

static bool to_contact_flash(void *ctx, const uint8_t *data, size_t len)
{
    (void)ctx;
    return contact_flash_write(data, len);
}

static esp_err_t contact_flash_post(httpd_req_t *req)
{
    if (!require_auth(req) || refused_relay_disabled(req) || refused_busy(req)) {
        return ESP_OK;
    }
    if (req->content_len <= 0 || req->content_len > MAX_CONTACT_IMAGE + 2048) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "an image for pages 0-29, a multiple of 4 bytes (a form field image, or the body)");
    }
    char query[96] = "", mac[12] = "", nv[8] = "", keep[8] = "";
    httpd_req_get_url_query_str(req, query, sizeof query);
    httpd_query_key_value(query, "mac", mac, sizeof mac);
    httpd_query_key_value(query, "nv", nv, sizeof nv);
    httpd_query_key_value(query, "keep", keep, sizeof keep);
    if (!contact_flash_begin(mac[0] ? mac : NULL, !strcmp(nv, "wipe"), last_flash, sizeof last_flash)) {
        ESP_LOGW(TAG, "%s", last_flash);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, last_flash);
    }
    bool rx_ok = receive(req, "image", to_contact_flash, NULL);
    bool ok = contact_flash_end(rx_ok, last_flash, sizeof last_flash);
    if (ok && (keep[0] == '1' || !strcmp(keep, "true")) && !settings.skip_gwsync) {
        settings.skip_gwsync = true;
        settings_save();
    }
    relay_task_reconnect();
    ESP_LOGI(TAG, "%s", last_flash);
    if (!ok) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, last_flash);
    }
    return httpd_resp_sendstr(req, last_flash);
}

typedef struct {
    char *buf;
    size_t len, cap;
} mem_sink_t;

static bool to_mem(void *ctx, const uint8_t *data, size_t len)
{
    mem_sink_t *m = ctx;
    if (m->len + len >= m->cap) {
        return false;
    }
    memcpy(m->buf + m->len, data, len);
    m->len += len;
    m->buf[m->len] = 0;
    return true;
}

static esp_err_t contact_nv_get(httpd_req_t *req)
{
    if (!require_auth(req) || refused_relay_disabled(req) || refused_busy(req)) {
        return ESP_OK;
    }
    char msg[128] = "";
    char *json = malloc(4608);
    if (!json) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }
    if (!contact_nv_export(json, 4608, msg, sizeof msg)) {
        free(json);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, msg);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"nv.json\"");
    esp_err_t err = httpd_resp_sendstr(req, json);
    free(json);
    return err;
}

static esp_err_t contact_nv_post(httpd_req_t *req)
{
    if (!require_auth(req) || refused_relay_disabled(req) || refused_busy(req)) {
        return ESP_OK;
    }
    if (req->content_len <= 0 || req->content_len > 12288) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "expected an openwyzesense-nv-json v1 document");
    }
    char msg[160] = "";
    char *raw = malloc(12289);
    gwnv_doc_t *doc = malloc(sizeof *doc);
    if (!raw || !doc) {
        free(raw);
        free(doc);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }
    mem_sink_t sink = {raw, 0, 12289};
    bool rx_ok = receive(req, "nv", to_mem, &sink);
    bool parsed = rx_ok && gwnv_parse_json(raw, sink.len, doc, msg, sizeof msg);
    free(raw);
    if (!parsed) {
        free(doc);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, rx_ok ? msg : "receiving nv.json failed");
    }
    bool ok = contact_nv_import(doc, msg, sizeof msg);
    free(doc);
    if (!ok) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, msg);
    }
    relay_task_reconnect();
    return httpd_resp_sendstr(req, msg);
}

static bool to_cjtag_stage(void *ctx, const uint8_t *data, size_t len)
{
    (void)ctx;
    return cjtag_task_stage_feed(data, len);
}

static cjtag_transport_t parse_transport(const char *s)  // the station's three modes, all on the two probes
{
    if (!strcmp(s, "sbl2")) {
        return CJTAG_TRANSPORT_SBL2;
    }
    if (!strcmp(s, "ws")) {
        return CJTAG_TRANSPORT_WS;
    }
    return CJTAG_TRANSPORT_CJTAG;
}

static esp_err_t cjtag_get(httpd_req_t *req)
{
    if (refused_station_disabled(req)) {
        return ESP_OK;
    }
    static char out[2048];  // the station's status: a long message and the identification (one request at a time)
    size_t n = cjtag_task_json(out, sizeof out);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, out, (ssize_t)n);
}

static esp_err_t cjtag_arm_post(httpd_req_t *req)
{
    if (!require_auth(req) || refused_station_disabled(req)) {
        return ESP_OK;
    }
    char query[160] = "", body[160] = "", mode_s[16] = "", tr_s[16] = "";
    httpd_req_get_url_query_str(req, query, sizeof query);
    httpd_query_key_value(query, "mode", mode_s, sizeof mode_s);
    httpd_query_key_value(query, "transport", tr_s, sizeof tr_s);
    if (!mode_s[0] && req->content_len > 0 && req->content_len < (int)sizeof body) {
        int n = httpd_req_recv(req, body, req->content_len);
        if (n > 0) {
            body[n] = 0;
            form_value(body, "mode", mode_s, sizeof mode_s);
            form_value(body, "transport", tr_s, sizeof tr_s);
        }
    }
    cjtag_mode_t m = CJTAG_MODE_NONE;
    if (!strcmp(mode_s, "dump")) {
        m = CJTAG_MODE_DUMP;
    } else if (!strcmp(mode_s, "flash")) {
        m = CJTAG_MODE_FLASH;
    } else if (!strcmp(mode_s, "identify")) {
        m = CJTAG_MODE_IDENTIFY;
    }
    cjtag_transport_t tr = parse_transport(tr_s);
    char msg[160] = "";
    if (!cjtag_task_arm(m, tr, msg, sizeof msg)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, msg);
    }
    return httpd_resp_sendstr(req, msg);
}

static esp_err_t cjtag_stage_post(httpd_req_t *req)
{
    if (!require_auth(req) || refused_station_disabled(req)) {
        return ESP_OK;
    }
    if (req->content_len <= 0 || req->content_len > MAX_CONTACT_IMAGE + 2048) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "an image for pages 0-29, a multiple of 4 bytes (a form field image, or the body)");
    }
    char query[160] = "", nv[8] = "", tr_s[16] = "", msg[160] = "";
    httpd_req_get_url_query_str(req, query, sizeof query);
    httpd_query_key_value(query, "nv", nv, sizeof nv);
    httpd_query_key_value(query, "transport", tr_s, sizeof tr_s);
    cjtag_transport_t tr = parse_transport(tr_s);
    if (!cjtag_task_stage_begin(!strcmp(nv, "wipe"), msg, sizeof msg)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, msg);
    }
    bool rx_ok = receive(req, "image", to_cjtag_stage, NULL);
    if (!cjtag_task_stage_end(rx_ok, tr, msg, sizeof msg)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, msg);
    }
    return httpd_resp_sendstr(req, msg);
}

static esp_err_t cjtag_dump_get(httpd_req_t *req)
{
    if (!require_auth(req) || refused_station_disabled(req)) {
        return ESP_OK;
    }
    uint8_t *page = malloc(CJTAG_PAGE);
    if (!page) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }
    if (!cjtag_task_get_dump_page(0, page)) {
        free(page);
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no completed station flash dump available");
    }
    char disp[80];
    snprintf(disp, sizeof disp, "attachment; filename=\"sensor_%s_flash.bin\"", cjtag_task_mac());
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    for (uint32_t p = 0; p < CJTAG_TOTAL_PAGES; p++) {
        if (!cjtag_task_get_dump_page(p, page) ||
            httpd_resp_send_chunk(req, (const char *)page, CJTAG_PAGE) != ESP_OK) {
            free(page);
            return ESP_FAIL;
        }
    }
    free(page);
    cjtag_task_note_dump_downloaded();
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t cjtag_fcfg1_get(httpd_req_t *req)
{
    if (!require_auth(req) || refused_station_disabled(req)) {
        return ESP_OK;
    }
    uint8_t *fcfg1 = malloc(CJTAG_FCFG1_SIZE);
    if (!fcfg1) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }
    if (!cjtag_task_get_fcfg1(fcfg1)) {
        free(fcfg1);
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no completed cJTAG FCFG1 dump available");
    }
    char disp[80];
    snprintf(disp, sizeof disp, "attachment; filename=\"sensor_%s_fcfg1.bin\"", cjtag_task_mac());
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    esp_err_t err = httpd_resp_send(req, (const char *)fcfg1, CJTAG_FCFG1_SIZE);
    free(fcfg1);
    return err;
}

static bool to_ota(void *ctx, const uint8_t *data, size_t len)
{
    return esp_ota_write(*(esp_ota_handle_t *)ctx, data, len) == ESP_OK;
}

static esp_err_t ota_post(httpd_req_t *req)
{
    if (!require_auth(req) || refused_busy(req)) {
        return ESP_OK;
    }
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    esp_ota_handle_t h;
    if (!next || esp_ota_begin(next, OTA_SIZE_UNKNOWN, &h) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no OTA partition");
    }
    if (!receive(req, "image", to_ota, &h)) {
        esp_ota_abort(h);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "receiving or writing failed");
    }
    if (esp_ota_end(h) != ESP_OK || esp_ota_set_boot_partition(next) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "not a valid image");
    }
    httpd_resp_sendstr(req, "new firmware written; restarting (it rolls back unless it joins Wi-Fi)");
    xTaskCreate(restart_soon, "restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

void web_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.recv_wait_timeout = 30;
    cfg.max_uri_handlers = 32;
    httpd_handle_t h;
    if (httpd_start(&h, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "the web server did not start");
        return;
    }
    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = page_get},
        {.uri = "/setup", .method = HTTP_GET, .handler = page_get},  // the setup access point's address
        {.uri = "/api/status", .method = HTTP_GET, .handler = api_status},
        {.uri = "/setup", .method = HTTP_POST, .handler = setup_post},
        {.uri = "/contact/reset", .method = HTTP_POST, .handler = contact_reset_post},
        {.uri = "/contact/gwsync", .method = HTTP_POST, .handler = contact_gwsync_post},
        {.uri = "/contact/flash", .method = HTTP_POST, .handler = contact_flash_post},
        {.uri = "/contact/nv", .method = HTTP_GET, .handler = contact_nv_get},
        {.uri = "/contact/nv", .method = HTTP_POST, .handler = contact_nv_post},
        {.uri = "/ota", .method = HTTP_POST, .handler = ota_post},
        {.uri = "/api/rig", .method = HTTP_GET, .handler = rig_get},
        {.uri = "/rig", .method = HTTP_POST, .handler = rig_post},
        {.uri = "/contact/console", .method = HTTP_POST, .handler = contact_console_post},
        {.uri = "/debug/cjtag_page", .method = HTTP_POST, .handler = debug_cjtag_page_post},
        {.uri = "/debug/capture", .method = HTTP_POST, .handler = debug_capture_post},
        {.uri = "/debug/sample", .method = HTTP_POST, .handler = debug_sample_post},
        {.uri = "/debug/page31", .method = HTTP_GET, .handler = debug_page31_handler},
        {.uri = "/debug/page31", .method = HTTP_POST, .handler = debug_page31_handler},
        {.uri = "/api/cjtag", .method = HTTP_GET, .handler = cjtag_get},
        {.uri = "/cjtag/arm", .method = HTTP_POST, .handler = cjtag_arm_post},
        {.uri = "/cjtag/stage", .method = HTTP_POST, .handler = cjtag_stage_post},
        {.uri = "/cjtag/dump.bin", .method = HTTP_GET, .handler = cjtag_dump_get},
        {.uri = "/cjtag/fcfg1.bin", .method = HTTP_GET, .handler = cjtag_fcfg1_get},
        {.uri = "/wires", .method = HTTP_POST, .handler = wires_post},
    };
    for (size_t i = 0; i < sizeof uris / sizeof uris[0]; i++) {
        httpd_register_uri_handler(h, &uris[i]);
    }
}

// The ESP8266 relay (issue #22): a v1 contact board running our gateway, relayed over Wi-Fi to wyzesense2mqtt-rs,
// as the ESP32-S3 firmware (esp32/idf) does, in an Arduino shell around the same cores (esp32/relay and
// esp32/contact: the relay, the outage buffer, streamed SBL reflashing, start-up gateway sync, and NV export/import).
// From the contact: UART0 swapped to GPIO13 (D7); to the contact: UART1's TX on GPIO2 (D4), not UART0's swapped TX on
// GPIO15 (GPIO15 must be low when the ESP8266 starts, and both of the contact's UART pads idle high: it would not
// boot) nor a software serial (with Wi-Fi on, one Inquiry in five went unanswered; 2026-10-25). So no log UART: the
// last log line is on the status page.
#include <EEPROM.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <Updater.h>

#include "gateway_image.h"  // generated (embed.py): the gateway image the relay carries (#76)
#include "index_html.h"     // generated from esp32/web/index.html (embed.py)
#include "relay_version.h"  // generated (embed.py): OWS_RELAY_VERSION

extern "C" {
#include "buffer.h"
#include "cjtag.h"
#include "gwnv.h"
#include "gwsync.h"
#include "http_auth.h"
#include "relay.h"
#include "sbl.h"
#include "stock_image.h"

extern uint32_t _FS_start;
extern uint32_t _FS_end;
}

namespace
{

struct Settings {
    uint32_t magic;
    char ssid[33], pass[65], url[128], token[64], admin_pass[64];
    uint8_t skip_gwsync;
    uint8_t stock_gw;
};
constexpr uint32_t kMagicV1 = 0x4F575331;  // "OWS1" (without admin_pass, skip_gwsync)
constexpr uint32_t kMagicV2 = 0x4F575332;  // "OWS2" (without skip_gwsync)
constexpr uint32_t kMagicV3 = 0x4F575333;  // "OWS3" (without stock_gw)
constexpr uint32_t kMagic = 0x4F575334;    // "OWS4"
constexpr uint32_t kApDelayMs = 120000;
constexpr uint32_t kApTimeoutMs = 900000;
constexpr uint32_t kBackoffBaseMs = 1000;
constexpr uint32_t kBackoffMaxMs = 30000;

Settings settings;
ESP8266WebServer server(80);
WiFiClient client;
relay_t relay;
buffer_t buffer;
bool ready, ap_up, down_tracking, ap_timed_out, mdns_up;
uint32_t down_since_ms, ap_since_ms, next_reconnect_ms;
uint32_t bytes_to_contact, bytes_from_contact;  // for the status page
char last_log[180];
char last_flash[160] = "none since the start";
// the start-up gateway check runs (gw_sync): the web pages are served from its waits meanwhile, and what would touch
// the contact's UART or restart the relay is refused
bool gw_busy, upload_refused, upload_unauth;
bool ota_started = false;
bool ota_ok = false;
bool flash_started = false;
char gw_steps[6][120];
unsigned gw_nsteps;
int fails;
char name[24], mdns_bare[24], mdns_fqdn[32];

uint32_t now_ms()
{
    return millis();
}

// --- the relay's I/O on the Arduino core ---

bool tcp_connect(void *, const char *host, uint16_t port)
{
    client.setTimeout(5000);
    if (!client.connect(host, port)) {
        return false;
    }
    client.setNoDelay(true);
    return true;
}

bool tcp_send(void *, const uint8_t *data, size_t len)
{
    return client.write(data, len) == len;
}

void tcp_close(void *)
{
    client.stop();
}

void uart_out(void *, const uint8_t *data, size_t len)  // to the contact (the buffer's)
{
    Serial1.write(data, len);  // GPIO2, to the contact
    bytes_to_contact += len;
}

void to_gateway(void *, const uint8_t *data, size_t len)  // the relay's bytes for the gateway: through the buffer
{
    buffer_from_server(&buffer, data, len);
}

uint32_t random32(void *)
{
    return ESP.random();
}

void log_line(void *, const char *msg)
{
    snprintf(last_log, sizeof last_log, "%s", msg);
}

// --- the contact's S pad, open-drain; its R pad on UART1's TX ---

constexpr int CONTACT_R_GPIO = 2;  // D4

void pad(int gpio, bool low)
{
    if (low) {
        digitalWrite(gpio, LOW);
        pinMode(gpio, OUTPUT);
    } else {
        pinMode(gpio, INPUT);  // released: the contact pulls it up
    }
}

void contact_reset()
{
    pad(OWS_CONTACT_RESET, true);
    delay(20);
    pad(OWS_CONTACT_RESET, false);
}

// R low through a reset: the ROM bootloader (R / DIO1, the stock BL_CONFIG 0xC5FE01C5, as the dongle's CH554 and the
// ESP32 relay do it; docs/design/v1-contact.md). R is UART1's TX on GPIO2: taken from the UART meanwhile and driven
// low (GPIO2 is a boot strap of the ESP8266's own start only), then given back, idling high.
void contact_enter_bootloader()
{
    Serial1.flush();
    digitalWrite(CONTACT_R_GPIO, LOW);
    pinMode(CONTACT_R_GPIO, OUTPUT);  // a plain GPIO now, out of UART1
    delay(5);
    pad(OWS_CONTACT_RESET, true);
    delay(20);
    pad(OWS_CONTACT_RESET, false);
    delay(50);  // the ROM has looked at its backdoor pin by now
    digitalWrite(CONTACT_R_GPIO, HIGH);
    Serial1.begin(115200);  // GPIO2 back to UART1's TX
}

// --- settings (EEPROM) and Wi-Fi ---

void settings_load()
{
    EEPROM.begin(sizeof(Settings));
    EEPROM.get(0, settings);
    if (settings.magic == kMagicV1) {
        memset(settings.admin_pass, 0, sizeof settings.admin_pass);
        settings.skip_gwsync = 0;
        settings.stock_gw = 0;
        settings.magic = kMagic;
    } else if (settings.magic == kMagicV2) {
        settings.skip_gwsync = 0;
        settings.stock_gw = 0;
        settings.magic = kMagic;
    } else if (settings.magic == kMagicV3) {
        settings.stock_gw = 0;
        settings.magic = kMagic;
    } else if (settings.magic != kMagic) {
        memset(&settings, 0, sizeof settings);
    }
    settings.ssid[sizeof settings.ssid - 1] = settings.pass[sizeof settings.pass - 1] = 0;
    settings.url[sizeof settings.url - 1] = settings.token[sizeof settings.token - 1] = 0;
    settings.admin_pass[sizeof settings.admin_pass - 1] = 0;
    for (char *f : {settings.ssid, settings.url, settings.token}) {  // saved before the page trimmed them
        String v(f);
        v.trim();
        memmove(f, v.c_str(), v.length() + 1);
    }
}

bool settings_save()
{
    settings.magic = kMagic;
    EEPROM.put(0, settings);
    return EEPROM.commit();
}

void start_ap()
{
    if (ap_up) {
        return;
    }
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(name);
    ap_up = true;
    ap_since_ms = now_ms();
}

void stop_ap()
{
    if (!ap_up) {
        return;
    }
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    ap_up = false;
}

void start_mdns()
{
    if (!mdns_up && WiFi.isConnected() && MDNS.begin(mdns_bare)) {
        MDNS.addService("http", "tcp", 80);
        mdns_up = true;
    }
}

// --- the web: the page (esp32/web/index.html) and its endpoints; OTA and the contact's reflash take multipart uploads
// (curl -F image=@firmware.bin) ---

void set_field(const char *key, char *field, size_t cap, bool trim)
{
    String v = server.arg(key);
    if (trim) {
        v.trim();  // a phone keyboard's trailing space makes an SSID nobody has
    }
    if (v.length() && v.length() < cap) {
        memcpy(field, v.c_str(), v.length() + 1);
    }
}

bool auth_ok()
{
    String hdr = server.header("Authorization");
    return http_auth_ok(hdr.c_str(), settings.admin_pass, settings.token);
}

void send_unauth()
{
    server.sendHeader("WWW-Authenticate", "Basic realm=\"OWS Relay\"");
    server.send(401, "text/plain", "authentication required");
}

bool require_auth()
{
    if (auth_ok()) {
        return true;
    }
    send_unauth();
    return false;
}

// 503 while the start-up check runs
bool refused_busy()
{
    if (gw_busy) {
        server.send(503, "text/plain", "the start-up gateway check is running: try again in a moment");
    }
    return gw_busy;
}

void setup_post()
{
    if (!require_auth() || refused_busy()) {
        return;
    }
    set_field("ssid", settings.ssid, sizeof settings.ssid, true);
    set_field("pass", settings.pass, sizeof settings.pass, false);
    set_field("url", settings.url, sizeof settings.url, true);
    set_field("token", settings.token, sizeof settings.token, true);
    set_field("admin_pass", settings.admin_pass, sizeof settings.admin_pass, false);
    if (server.hasArg("skip_gwsync")) {
        String s = server.arg("skip_gwsync");
        settings.skip_gwsync = (s == "1" || s == "true" || s == "on") ? 1 : 0;
    }
    if (server.hasArg("gw_fw")) {
        String g = server.arg("gw_fw");
        if (g.length()) {
            settings.stock_gw = (g == "stock" || g == "1") ? 1 : 0;
        }
    }
    if (!settings_save()) {
        server.send(500, "text/plain", "saving failed");
        return;
    }
    server.send(200, "text/plain", "saved; restarting");
    delay(500);
    ESP.restart();
}

void contact_gwsync_post()
{
    if (!require_auth()) {
        return;
    }
    String s = server.arg("skip");
    settings.skip_gwsync = (s == "1" || s == "true" || s == "on") ? 1 : 0;
    if (!settings_save()) {
        server.send(500, "text/plain", "saving failed");
        return;
    }
    server.send(200, "text/plain", settings.skip_gwsync ? "startup auto-sync paused" : "startup auto-sync enabled");
}

void contact_reset_post()
{
    if (!require_auth() || refused_busy()) {
        return;
    }
    contact_reset();
    server.send(200, "text/plain", "contact reset");
}

#ifdef OWS_DEBUG_UART
// Bring-up: GET /debug/boot: only the reset into the contact's bootloader (R held low across S)
void debug_boot()
{
    contact_enter_bootloader();
    server.send(200, "text/plain", "reset into the bootloader\n");
}

// Bring-up: GET /debug/sdrive: into the bootloader with S driven high after the reset (not just released), and left so
void debug_sdrive()
{
    Serial1.flush();
    digitalWrite(CONTACT_R_GPIO, LOW);
    pinMode(CONTACT_R_GPIO, OUTPUT);
    delay(5);
    pad(OWS_CONTACT_RESET, true);
    delay(20);
    digitalWrite(OWS_CONTACT_RESET, HIGH);
    pinMode(OWS_CONTACT_RESET, OUTPUT);
    delay(50);
    digitalWrite(CONTACT_R_GPIO, HIGH);
    Serial1.begin(115200);
    server.send(200, "text/plain", "reset into the bootloader, S driven high\n");
}

// Bring-up: GET /debug/follow: drives D2 and D4 low then high in turn and reports D7 (the line from T) each time
void debug_follow()
{
    String out;
    const int pins[] = {OWS_CONTACT_RESET, 2};
    const char *names[] = {"D2", "D4"};
    Serial1.flush();
    for (int i = 0; i < 2; i++) {
        int g = pins[i];
        digitalWrite(g, LOW);
        pinMode(g, OUTPUT);
        delay(2);
        int lo = digitalRead(13);
        digitalWrite(g, HIGH);
        delay(2);
        int hi = digitalRead(13);
        pinMode(g, INPUT);
        out += String(names[i]) + " low->D7=" + lo + " high->D7=" + hi + "\n";
    }
    Serial1.begin(115200);
    pad(OWS_CONTACT_RESET, false);
    out += String("released: D7=") + digitalRead(13) + "\n";
    server.send(200, "text/plain", out);
}

// Bring-up: GET /debug/t: D7 as a plain input with its pull-up (out of the UART), read 20 times, then a restart
void debug_t()
{
    pinMode(13, INPUT_PULLUP);
    delay(5);
    String out = "D7 as GPIO input, pull-up:";
    for (int i = 0; i < 20; i++) {
        out += digitalRead(13) ? " 1" : " 0";
        delay(5);
    }
    server.send(200, "text/plain", out + "\nrestarting\n");
    delay(200);
    ESP.restart();
}

// Bring-up: GET /debug/pins: the levels on the contact's lines, released (S read as input; R is UART1's TX)
void debug_pins()
{
    char out[96];
    snprintf(out, sizeof out, "S(D2)=%d T(D7)=%d R(D4)=%d\n", digitalRead(OWS_CONTACT_RESET), digitalRead(13),
             digitalRead(2));
    server.send(200, "text/plain", out);
}

// Bring-up: GET /debug/uart?send=<hex>[&ms=300]: the bytes to the contact, and what comes back within ms, as hex
// (the relay is not reading meanwhile: this runs inside loop())
void debug_uart()
{
    if (!require_auth() || refused_busy()) {
        return;
    }
    String hex = server.arg("send"), out;
    uint8_t b[128];
    size_t n = 0;
    for (size_t i = 0; i + 1 < hex.length() && n < sizeof b; i += 2) {
        b[n++] = (uint8_t)strtol(hex.substring(i, i + 2).c_str(), nullptr, 16);
    }
    while (Serial.available()) {
        Serial.read();
    }
    if (n) {
        uart_out(nullptr, b, n);
    }
    uint32_t end = millis() + (server.hasArg("ms") ? (uint32_t)server.arg("ms").toInt() : 300);
    while ((int32_t)(millis() - end) < 0) {
        while (Serial.available()) {
            char h[4];
            snprintf(h, sizeof h, "%02x", Serial.read());
            out += h;
        }
        delay(1);
    }
    server.send(200, "text/plain", "sent " + String(n) + ", got " + String(out.length() / 2) + ": " + out + "\n");
}
#endif

// --- reflashing the contact through its ROM bootloader (esp32/contact/sbl.c), streamed: the ESP8266 cannot hold the
// image, so each page is written as the upload brings it. Meanwhile loop() does not run: the relay pauses, and its
// connection is dropped afterwards, so wyzesense2mqtt-rs handshakes with the new firmware ---

sbl_t sbl;
uint8_t sbl_page[SBL_PAGE];
size_t sbl_fill;
bool sbl_ok, sbl_wipe_nv;

bool sbl_out(void *, const uint8_t *data, size_t len)
{
    Serial1.write(data, len);
    Serial1.flush();
    return true;
}

// up to cap bytes: waits up to timeout_ms for the first, then until the line is quiet for 20 ms
size_t sbl_in(void *, uint8_t *buf, size_t cap, uint32_t timeout_ms)
{
    size_t n = 0;
    uint32_t start = millis(), last = start;
    while (n < cap) {
        if (Serial.available()) {
            buf[n++] = (uint8_t)Serial.read();
            last = millis();
        } else if (n ? millis() - last >= 20 : millis() - start >= timeout_ms) {
            break;
        } else if (gw_busy) {
            server.handleClient();  // the page answers while the start-up check waits
        } else {
            yield();
        }
    }
    return n;
}

void sbl_drain(void *)
{
    while (Serial.available()) {
        Serial.read();
    }
}

bool sbl_put_page()
{
    sbl_ok = sbl_ok && sbl_stream_page(&sbl, sbl_page, sbl_fill);
    sbl_fill = 0;
    return sbl_ok;
}

// POST /contact/flash?mac=<MAC>&nv=wipe (both optional), multipart, the image for pages 0-29 (the -app.bin)
void contact_flash_upload()
{
    HTTPUpload &u = server.upload();
    if (u.status == UPLOAD_FILE_START) {
        flash_started = false;
        upload_unauth = !auth_ok();
        upload_refused = !upload_unauth && gw_busy;
        if (upload_unauth || upload_refused) {
            sbl_ok = false;
            return;
        }
        flash_started = true;
        sbl_io_t io = {sbl_out, sbl_in, sbl_drain, nullptr};
        String mac = server.arg("mac");
        sbl_wipe_nv = server.arg("nv") == "wipe";
        sbl_init(&sbl, &io);
        const char *expect = mac.length() ? mac.c_str() : nullptr;
        bool ok = sbl_connect(&sbl, expect);
        if (!ok) {
            sbl_init(&sbl, &io);
            contact_enter_bootloader();
            ok = sbl_connect(&sbl, expect);
        }
        sbl_ok = ok && sbl_stream_begin(&sbl);
        sbl_fill = 0;
        return;
    }
    if (upload_unauth || upload_refused || !flash_started) {
        if (u.status == UPLOAD_FILE_ABORTED) {
            upload_unauth = false;
            upload_refused = false;
        }
        return;
    }
    if (u.status == UPLOAD_FILE_WRITE) {
        for (size_t at = 0; sbl_ok && at < u.currentSize;) {
            size_t n = u.currentSize - at < SBL_PAGE - sbl_fill ? u.currentSize - at : SBL_PAGE - sbl_fill;
            memcpy(sbl_page + sbl_fill, u.buf + at, n);
            sbl_fill += n;
            at += n;
            if (sbl_fill == SBL_PAGE) {
                sbl_put_page();
            }
        }
    } else if (u.status == UPLOAD_FILE_END) {
        if (sbl_fill) {
            sbl_put_page();
        }
        sbl_ok = sbl_ok && sbl_stream_end(&sbl);
        if (sbl_ok && sbl_wipe_nv) {
            memset(sbl_page, 0xFF, sizeof sbl_page);
            sbl_ok = sbl_write_nv(&sbl, sbl_page);
        }
        if (sbl_ok) {
            snprintf(last_flash, sizeof last_flash, "contact %s (backdoor DIO%u): %lu bytes written and verified%s",
                     sbl.mac, sbl.backdoor_dio, (unsigned long)sbl.stream_at, sbl_wipe_nv ? ", NV wiped" : "");
        } else {
            snprintf(last_flash, sizeof last_flash, "not flashed: %s", sbl.error);
        }
    } else if (u.status == UPLOAD_FILE_ABORTED) {
        flash_started = false;
        upload_unauth = false;
        upload_refused = false;
        sbl_ok = false;
        contact_reset();
        sbl_drain(nullptr);
        snprintf(last_flash, sizeof last_flash, "not flashed: the upload broke off (pages 0-29 part written)");
    }
}

void contact_flash_done()
{
    bool started = flash_started;
    bool unauth = upload_unauth;
    bool refused = upload_refused;
    flash_started = false;
    upload_unauth = false;
    upload_refused = false;
    if (unauth || !auth_ok()) {
        send_unauth();
        return;
    }
    if (refused || (!started && gw_busy)) {
        server.send(503, "text/plain", "the start-up gateway check is running: try again in a moment");
        return;
    }
    if (!started) {
        server.send(400, "text/plain", "missing firmware upload\n");
        return;
    }
    contact_reset();  // into its firmware, new or old (the bootloader's own reset would hang it)
    if (sbl_ok && (server.arg("keep") == "1" || server.arg("keep") == "true") && !settings.skip_gwsync) {
        settings.skip_gwsync = 1;
        settings_save();
    }
    delay(100);
    sbl_drain(nullptr);
    client.stop();  // the relay reconnects: a handshake with what now runs
    server.send(sbl_ok ? 200 : 500, "text/plain", String(last_flash) + "\n");
}

gwnv_doc_t nv_doc;
bool nv_multipart, nv_in_str, nv_esc, nv_overflow;
size_t nv_len;

void contact_nv_get()
{
    if (!require_auth() || refused_busy()) {
        return;
    }
    gwnv_io_t io = {sbl_out, sbl_in, sbl_drain, nullptr};
    char err[128] = "";
    bool ok = gwnv_export(&io, &nv_doc, err, sizeof err);
    sbl_drain(nullptr);
    if (!ok || !gwnv_format_json(&nv_doc, (char *)sbl_page, sizeof sbl_page)) {
        server.send(500, "text/plain", String(ok ? "NV JSON buffer too small" : err) + "\n");
        return;
    }
    server.sendHeader("Content-Disposition", "attachment; filename=\"nv.json\"");
    server.send(200, "application/json", (const char *)sbl_page);
}

void contact_nv_upload()
{
    HTTPUpload &u = server.upload();
    if (u.status == UPLOAD_FILE_START) {
        nv_multipart = true;
        upload_unauth = !auth_ok();
        upload_refused = !upload_unauth && gw_busy;
        nv_len = 0;
        nv_in_str = nv_esc = nv_overflow = false;
    }
    if (upload_unauth || upload_refused || nv_overflow) {
        if (u.status == UPLOAD_FILE_ABORTED) {
            nv_multipart = false;
            upload_unauth = false;
            upload_refused = false;
        }
        return;
    }
    if (u.status == UPLOAD_FILE_WRITE) {
        for (size_t i = 0; i < u.currentSize; i++) {
            uint8_t c = u.buf[i];
            if (!nv_in_str && (c == ' ' || c == '\t' || c == '\r' || c == '\n')) {
                continue;  // strip insignificant JSON whitespace so 64-sensor pretty nv.json fits in sbl_page[4096]
            }
            if (nv_len + 1 >= sizeof sbl_page) {
                nv_overflow = true;
                return;
            }
            sbl_page[nv_len++] = c;
            if (nv_in_str) {
                if (nv_esc) {
                    nv_esc = false;
                } else if (c == '\\') {
                    nv_esc = true;
                } else if (c == '"') {
                    nv_in_str = false;
                }
            } else if (c == '"') {
                nv_in_str = true;
            }
        }
        sbl_page[nv_len] = 0;
    } else if (u.status == UPLOAD_FILE_ABORTED) {
        nv_multipart = false;
        upload_unauth = false;
        upload_refused = false;
        nv_overflow = true;
    }
}

void contact_nv_post()
{
    bool mp = nv_multipart;
    bool unauth = upload_unauth;
    bool refused = upload_refused;
    nv_multipart = false;
    upload_unauth = false;
    upload_refused = false;
    if (mp) {
        if (unauth || !auth_ok()) {
            send_unauth();
            return;
        }
        if (refused) {
            server.send(503, "text/plain", "the start-up gateway check is running: try again in a moment");
            return;
        }
        if (nv_overflow) {
            server.send(400, "text/plain", "nv.json upload too large or aborted\n");
            return;
        }
    } else if (!require_auth() || refused_busy()) {
        return;
    }
    String plain;
    const char *src = (const char *)sbl_page;
    size_t slen = nv_len;
    if (!mp) {
        plain = server.arg("plain");
        src = plain.c_str();
        slen = plain.length();
    }
    char msg[160] = "";
    if (!gwnv_parse_json(src, slen, &nv_doc, msg, sizeof msg)) {
        server.send(400, "text/plain", String(msg) + "\n");
        return;
    }
    gwnv_io_t io = {sbl_out, sbl_in, sbl_drain, nullptr};
    bool ok = gwnv_import(&io, &nv_doc, msg, sizeof msg);
    sbl_drain(nullptr);
    if (!ok) {
        server.send(500, "text/plain", String(msg) + "\n");
        return;
    }
    client.stop();  // reconnect so wyzesense2mqtt-rs re-handshakes and refreshes GetSensorList
    snprintf(msg, sizeof msg, "imported network %04X/%04X and %u sensors\n", nv_doc.pan, nv_doc.short_addr,
             (unsigned)nv_doc.n_sensors);
    server.send(200, "text/plain", msg);
}

// --- at the start: the contact on the gateway image this firmware carries (esp32/contact/gwsync.c, #76) ---

gwsync_report_t gw_report;
// found by the web page in an uploaded image (esp32/web/index.html): the relay's version
const char kRelayTag[] = "OWS-RELAY-VERSION:" OWS_RELAY_VERSION;
bool gw_checked;

void gw_reset(void *)
{
    contact_reset();
}

void gw_bootloader(void *)
{
    contact_enter_bootloader();
}

void gw_sleep(void *, uint32_t ms)
{
    for (uint32_t t = millis(); millis() - t < ms;) {
        server.handleClient();
        delay(1);
    }
}

void gw_progress(void *, const char *step)
{
    snprintf(gw_steps[gw_nsteps % 6], sizeof gw_steps[0], "%lu ms %s", (unsigned long)millis(), step);
    gw_nsteps++;
}

constexpr uint32_t kStockStageOffset = 0x20000u;  // staging region after the 128 KB stock partition in _FS_start
bool stock_probed = false;
bool stock_valid = false;
bool stock_started = false;
bool stock_ok = false;
uint32_t stock_erased_up_to = 0;
stock_pack_t stock_pack;
char stock_msg[160];

uint32_t stock_part_addr()
{
    return (uint32_t)((uintptr_t)&_FS_start - 0x40200000u);
}

uint32_t stock_part_cap()
{
    uintptr_t s = (uintptr_t)&_FS_start, e = (uintptr_t)&_FS_end;
    return e > s ? (uint32_t)(e - s) : 0;
}

bool stock_part_read(void *, uint32_t off, uint8_t *buf, size_t len)
{
    yield();
    return ESP.flashRead(stock_part_addr() + off, buf, len);
}

bool stock_gw_available()
{
    if (!stock_probed) {
        uint8_t buf[256];
        char err[96];
        stock_probed = true;
        stock_valid =
            stock_part_cap() >= STOCK_PART_LEN &&
            stock_verify_partition(stock_part_read, nullptr, k_stock_image_sha256, buf, sizeof buf, err, sizeof err);
    }
    return stock_valid;
}

const uint8_t *active_gw_image(uint32_t *len_out)
{
    if (settings.stock_gw && stock_gw_available()) {
        *len_out = STOCK_IMAGE_LEN;
        return nullptr;  // stored in the _FS_start flash region, read via ESP.flashRead in gw_image()
    }
    *len_out = kGatewayImageLen;
    return kGatewayImage;
}

bool gw_image(void *, uint32_t off, uint8_t *buf, size_t len)
{
    uint32_t img_len = 0;
    const uint8_t *img = active_gw_image(&img_len);
    if (off + len > img_len) {
        return false;
    }
    if (!img) {
        yield();
        return ESP.flashRead(stock_part_addr() + STOCK_HDR_LEN + off, buf, len);
    }
    memcpy_P(buf, img + off, len);
    return true;
}

bool stock_stage_write(void *, uint32_t off, const uint8_t *data, size_t len)
{
    if (off >= STOCK_APP_LEN) {
        return off == STOCK_APP_LEN && len == STOCK_PAGE31_CODE_LEN;
    }
    if (off + len > STOCK_APP_LEN) {
        return false;
    }
    uint32_t stage = stock_part_addr() + kStockStageOffset;
    while (stock_erased_up_to < off + len) {
        yield();
        if (!ESP.flashEraseSector((stage + stock_erased_up_to) >> 12)) {
            return false;
        }
        stock_erased_up_to += SBL_PAGE;
    }
    yield();
    return ESP.flashWrite(stage + off, data, len);
}

bool stock_commit(const uint8_t hdr[STOCK_HDR_LEN])
{
    uint32_t base = stock_part_addr();
    uint32_t stage = base + kStockStageOffset;
    for (uint32_t off = 0; off < STOCK_PART_LEN; off += SBL_PAGE) {
        yield();
        if (!ESP.flashEraseSector((base + off) >> 12)) {
            snprintf(stock_msg, sizeof stock_msg, "erasing stock flash area failed");
            return false;
        }
    }
    yield();
    // Write the 48-byte header and the 0xFA8-byte page-31 code (currently in sbl_page) before reusing sbl_page to
    // copy pages 0-29 from the staging area.
    if (!ESP.flashWrite(base, hdr, STOCK_HDR_LEN) ||
        !ESP.flashWrite(base + STOCK_HDR_LEN + STOCK_APP_LEN, sbl_page, STOCK_PAGE31_CODE_LEN)) {
        snprintf(stock_msg, sizeof stock_msg, "writing stock flash header or page 31 code failed");
        return false;
    }
    for (uint32_t off = 0; off < STOCK_APP_LEN; off += SBL_PAGE) {
        yield();
        if (!ESP.flashRead(stage + off, sbl_page, SBL_PAGE) ||
            !ESP.flashWrite(base + STOCK_HDR_LEN + off, sbl_page, SBL_PAGE)) {
            snprintf(stock_msg, sizeof stock_msg, "writing stock flash page %lu failed",
                     (unsigned long)(off / SBL_PAGE));
            return false;
        }
    }
    stock_probed = true;
    stock_valid = stock_verify_partition(stock_part_read, nullptr, k_stock_image_sha256, sbl_page, sizeof sbl_page,
                                         stock_msg, sizeof stock_msg);
    if (stock_valid) {
        snprintf(stock_msg, sizeof stock_msg,
                 "stock v1.8 gateway image stored and verified (%u bytes, SHA-256 %.16s...)", (unsigned)STOCK_IMAGE_LEN,
                 STOCK_IMAGE_SHA256_HEX);
    }
    return stock_valid;
}

void contact_stock_upload()
{
    HTTPUpload &u = server.upload();
    if (u.status == UPLOAD_FILE_START) {
        stock_started = false;
        stock_ok = false;
        upload_unauth = !auth_ok();
        upload_refused = !upload_unauth && gw_busy;
        if (upload_unauth || upload_refused) {
            return;
        }
        stock_started = true;
        if (stock_part_cap() < kStockStageOffset + STOCK_APP_LEN) {
            snprintf(stock_msg, sizeof stock_msg, "flash filesystem area too small for stock image");
            return;
        }
        stock_erased_up_to = 0;
        stock_pack_init(&stock_pack, sbl_page, stock_stage_write, nullptr);
        stock_ok = true;
        return;
    }
    if (upload_unauth || upload_refused || !stock_started) {
        if (u.status == UPLOAD_FILE_ABORTED) {
            upload_unauth = false;
            upload_refused = false;
        }
        return;
    }
    if (u.status == UPLOAD_FILE_WRITE) {
        if (stock_ok && !stock_pack_feed(&stock_pack, u.buf, u.currentSize)) {
            stock_ok = false;
        }
    } else if (u.status == UPLOAD_FILE_END) {
        uint8_t hdr[STOCK_HDR_LEN];
        if (!stock_pack_finish(&stock_pack, k_stock_image_sha256, hdr, nullptr, stock_msg, sizeof stock_msg)) {
            stock_ok = false;
            return;
        }
        stock_ok = stock_commit(hdr);
    } else if (u.status == UPLOAD_FILE_ABORTED) {
        stock_started = false;
        stock_ok = false;
        upload_unauth = false;
        upload_refused = false;
        snprintf(stock_msg, sizeof stock_msg, "stock image upload broke off");
    }
}

void contact_stock_done()
{
    bool started = stock_started;
    bool unauth = upload_unauth;
    bool refused = upload_refused;
    bool ok = stock_ok;
    stock_started = false;
    stock_ok = false;
    upload_unauth = false;
    upload_refused = false;
    if (unauth || !auth_ok()) {
        send_unauth();
        return;
    }
    if (refused || (!started && gw_busy)) {
        server.send(503, "text/plain", "the start-up gateway check is running: try again in a moment");
        return;
    }
    if (!started) {
        server.send(400, "text/plain", "missing stock image upload\n");
        return;
    }
    server.send(ok ? 200 : 400, "text/plain", String(stock_msg) + "\n");
}

// --- the contact's debug port (2-pin cJTAG on M and C, esp32/contact/cjtag.c, #81) ---

constexpr int CONTACT_TMS_GPIO = 14;  // D5: M (TMSC, both ways)
constexpr int CONTACT_TCK_GPIO = 12;  // D6: C (TCKC)
uint32_t cj_yield_at;

void cj_set_pins(void *, bool tck, bool tms)  // TMSC changes while TCKC is low
{
    if (!tck) {
        digitalWrite(CONTACT_TCK_GPIO, LOW);
        digitalWrite(CONTACT_TMS_GPIO, tms);
    } else {
        digitalWrite(CONTACT_TMS_GPIO, tms);
        digitalWrite(CONTACT_TCK_GPIO, HIGH);
    }
}

void cj_set_tms_dir(void *, bool output)
{
    pinMode(CONTACT_TMS_GPIO, output ? OUTPUT : INPUT_PULLUP);
}

bool cj_get_tms(void *)
{
    return digitalRead(CONTACT_TMS_GPIO) != 0;
}

void cj_delay(void *, uint32_t us)
{
    if (us > 1) {
        delayMicroseconds(us);
    }
    if (millis() - cj_yield_at > 20) {  // the clock is ours: pausing only slows it; Wi-Fi and the watchdog need it
        cj_yield_at = millis();
        yield();
    }
}

void cj_release_pins()
{
    pinMode(CONTACT_TMS_GPIO, INPUT);
    pinMode(CONTACT_TCK_GPIO, INPUT);
}

cjtag_t cj;

bool cj_connect()
{
    static const cjtag_io_t io = {cj_set_pins, cj_set_tms_dir, cj_get_tms, cj_delay, nullptr};
    digitalWrite(CONTACT_TCK_GPIO, HIGH);
    pinMode(CONTACT_TCK_GPIO, OUTPUT);
    digitalWrite(CONTACT_TMS_GPIO, HIGH);
    pinMode(CONTACT_TMS_GPIO, OUTPUT);
    cj_yield_at = millis();
    for (int i = 0; i < 3; i++) {
        cjtag_init(&cj, &io);
        if (cjtag_connect(&cj, true)) {
            return true;
        }
        delay(50);
    }
    cj_release_pins();
    return false;
}

void cj_done(bool restart)
{
    cjtag_release_and_reset(&cj);
    cj_release_pins();
    if (restart) {
        contact_reset();  // a clean start (VECTRESET restarts the CPU only)
    }
}

// gwsync's debug-port reflash: pages 0-29 of the carried image (sbl_page is free while gwsync calls this)
int gw_cjtag_flash(void *, uint32_t image_len, char *mac, char *err, size_t cap)
{
    if (!cj_connect()) {
        snprintf(err, cap, "%s", cj.error);
        return -1;
    }
    memcpy(mac, cj.mac, 9);
    int written = cjtag_write_image(&cj, gw_image, nullptr, image_len, sbl_page);
    if (written < 0) {
        snprintf(err, cap, "%s", cj.error);
    }
    cj_done(written > 0);
    return written;
}

const char *backdoor_name(uint32_t bl)
{
    if ((bl & 0xFFu) != 0xC5u || ((bl >> 24) & 0xFFu) != 0xC5u || ((bl >> 16) & 0xFFu) != 0xFEu) {
        return "off";  // bootloader or backdoor disabled
    }
    switch ((bl >> 8) & 0xFFu) {
        case 1:
            return "r";
        case 10:
            return "w";
        case 11:
            return "button";
        default:
            return "other";
    }
}

String js(const char *v);  // below

// GET /contact/bootloader: the contact's BL_CONFIG through the debug port (read only; the contact restarts).
// POST /contact/bootloader backdoor=r mac=<its MAC>: the one CCFG write allowed (cjtag_set_backdoor).
void contact_bootloader()
{
    bool write = server.method() == HTTP_POST;
    if (write && (!require_auth() || refused_busy())) {
        return;
    }
    if (!write && gw_busy) {
        server.send(409, "text/plain", "busy: the gateway check runs\n");
        return;
    }
    String want = server.arg("backdoor");
    // only the pin this relay drives (docs/design/ccfg-rule.md: never a backdoor nothing can reach): R (D4)
    int pin = want == "r" ? 1 : -1;
    if (write && pin < 0) {
        server.send(400, "text/plain", "backdoor=r (the pin this relay drives)\n");
        return;
    }
    gw_busy = true;
    if (!cj_connect()) {
        gw_busy = false;
        contact_reset();
        server.send(502, "text/plain", String("debug port: ") + cj.error + "\n");
        return;
    }
    uint32_t before = cj.bl_config;
    int r = 0;
    String msg;
    if (write && server.arg("mac") != cj.mac) {
        r = -1;
        msg = String("refused: mac=") + server.arg("mac") + " is not this contact (" + cj.mac + ")";
    } else if (write) {
        r = cjtag_set_backdoor(&cj, (uint8_t)pin, sbl_page);
        msg = r > 0 ? "written" : r == 0 ? "already so" : String("failed: ") + cj.error;
    }
    char out[200];
    snprintf(out, sizeof out,
             "{\"mac\":\"%s\",\"factory_mac\":%s,\"bl_config\":\"%08lx\",\"backdoor\":\"%s\",\"before\":\"%08lx\"",
             cj.mac, cj.has_ccfg_mac ? "true" : "false", (unsigned long)cj.bl_config, backdoor_name(cj.bl_config),
             (unsigned long)before);
    cj_done(true);
    gw_busy = false;
    server.send(r < 0 ? 500 : 200, "application/json", String(out) + ",\"result\":" + js(msg.c_str()) + "}\n");
}

// POST /contact/ccfg_mac mac=<MAC>: fills a blank CCFG MAC slot with the unit's own MAC from page 29 (newer v1
// contacts; cjtag_fill_ccfg_mac: program only, no erase). Then the gateway check can write it.
void contact_ccfg_mac()
{
    if (!require_auth() || refused_busy()) {
        return;
    }
    gw_busy = true;
    if (!cj_connect()) {
        gw_busy = false;
        contact_reset();
        server.send(502, "text/plain", String("debug port: ") + cj.error + "\n");
        return;
    }
    String mac = server.arg("mac");
    int r = cjtag_fill_ccfg_mac(&cj, mac.c_str(), sbl_page);
    String msg = r > 0    ? String("written: CCFG MAC ") + cj.mac
                 : r == 0 ? String("already so: ") + cj.mac
                          : String("refused/failed: ") + cj.error;
    cj_done(true);
    gw_busy = false;
    server.send(r < 0 ? 500 : 200, "text/plain", msg + "\n");
}

void gw_sync()
{
    gwsync_io_t io = {sbl_out,  sbl_in,      sbl_drain, gw_reset,       gw_bootloader,  gw_sleep,
                      gw_image, gw_progress, nullptr,   gw_cjtag_flash, GWSYNC_VIA_AUTO};  // the debug port first (#81)
    uint32_t img_len = 0;
    active_gw_image(&img_len);
    gw_busy = true;
    if (settings.skip_gwsync) {
        gwsync_probe(&io, img_len, sbl_page, &gw_report);
    } else {
        gwsync_run(&io, img_len, &sbl, sbl_page, &gw_report);
    }
    gw_busy = false;
    gw_checked = true;
    buffer.translate_sync = gw_report.stock || settings.stock_gw;
    if (buffer.translate_sync && strlen(gw_report.mac) == 8) {
        memcpy(buffer.ch554_mac, gw_report.mac, sizeof buffer.ch554_mac);  // the contact's MAC, as Get MAC answer
    }
    snprintf(last_log, sizeof last_log, "gateway: %s", gw_report.msg);
    sbl_drain(nullptr);
}

String gw_steps_text()  // the last steps, oldest first, a line each
{
    String t;
    for (unsigned i = gw_nsteps > 6 ? gw_nsteps - 6 : 0; i < gw_nsteps; i++) {
        t += gw_steps[i % 6];
        t += "\n";
    }
    return t;
}

String id_hex(const uint8_t id[8])
{
    char h[17];
    for (int i = 0; i < 8; i++) {
        snprintf(h + 2 * i, 3, "%02x", id[i]);
    }
    return h;
}

// JSON string: quotes, backslashes and control characters escaped
String js(const char *v)
{
    String o = "\"";
    for (; *v; v++) {
        if (*v == '"' || *v == '\\') {
            o += '\\';
            o += *v;
        } else if ((unsigned char)*v < 0x20) {
            char u[8];
            snprintf(u, sizeof u, "\\u%04x", (unsigned char)*v);
            o += u;
        } else {
            o += *v;
        }
    }
    return o + "\"";
}

void api_status()
{
    static const char *states[] = {"down", "handshake", "connected"};
    char n[320];  // the fields below: ~250 bytes since gw_fw / stock_available (220 cut "heap" off)
    uint32_t now = now_ms();
    String b;
    b.reserve(2560);
    b = "{\"name\":" + js(name) + ",\"mdns\":" + js(mdns_fqdn) + ",\"ssid\":" + js(settings.ssid) +
        ",\"url\":" + js(settings.url);
    b += ",\"ip\":" + js(WiFi.isConnected() ? WiFi.localIP().toString().c_str() : "");
    snprintf(n, sizeof n,
             ",\"ap\":%s,\"auth\":%s,\"skip_gwsync\":%s,\"gw_fw\":\"%s\",\"stock_stored\":%s,\"rssi\":%d,\"relay\":"
             "\"%s\",\"connects\":%lu,\"queued\":%lu,"
             "\"replayed\":%lu,\"dropped\":%lu,\"waiting\":%u,\"uart_tx\":%lu,\"uart_rx\":%lu,\"uptime\":%lu,"
             "\"heap\":%lu",
             ap_up ? "true" : "false", http_auth_secret(settings.admin_pass, settings.token) ? "true" : "false",
             settings.skip_gwsync ? "true" : "false", settings.stock_gw ? "stock" : "ows",
             stock_gw_available() ? "true" : "false", WiFi.isConnected() ? (int)WiFi.RSSI() : 0, states[relay.state],
             (unsigned long)relay.connects, (unsigned long)buffer.queued_total, (unsigned long)buffer.replayed_total,
             (unsigned long)buffer.dropped_total, buffer.count, (unsigned long)bytes_to_contact,
             (unsigned long)bytes_from_contact, (unsigned long)(now / 1000), (unsigned long)ESP.getFreeHeap());
    b += n;
    static const char *results[] = {"same", "updated", "failed", "refused"};
    const char *cmac = sbl.mac[0] ? sbl.mac : (gw_report.mac[0] ? gw_report.mac : buffer.gateway_mac);
    b += ",\"log\":" + js(last_log) + ",\"flash\":" + js(last_flash) + ",\"contact_mac\":" + js(cmac);
    b += ",\"build\":" + js(kRelayTag + 18) + ",\"chip\":\"ESP8266\",\"fw_file\":\".pio/build/d1_mini/firmware.bin\"";
    b += ",\"gw\":{\"checked\":" + String(gw_checked ? "true" : "false") + ",\"result\":\"" +
         results[gw_report.result] + "\",\"msg\":" + js(gw_report.msg) + ",\"steps\":" + js(gw_steps_text().c_str()) +
         ",\"carried\":{\"version\":" + js(gw_report.carried.version) + ",\"id\":\"" + id_hex(gw_report.carried.id) +
         "\"},\"running\":" +
         (gw_report.answered || gw_report.result == GWSYNC_UPDATED
              ? "{\"version\":" + js(gw_report.running.version) + ",\"id\":\"" + id_hex(gw_report.running.id) + "\"}"
              : String("null")) +
         "},\"sensors\":[";
    for (unsigned i = 0; i < buffer.sensor_count; i++) {
        const buffer_sensor_t &s = buffer.sensors[i];
        uint32_t seen_s = (uint32_t)(now - s.last_seen_ms) / 1000u;
        snprintf(n, sizeof n, "%s{\"mac\":\"%.8s\",\"type\":%u,\"rssi\":%d,\"bat_mv\":%u,\"seen_s\":%lu}", i ? "," : "",
                 s.mac, (unsigned)s.type, (int)s.last_rssi, (unsigned)s.last_bat_mv, (unsigned long)seen_s);
        b += n;
    }
    b += "]}";
    server.send(200, "application/json", b);
}

void page_index()
{
    server.send_P(200, "text/html", kIndexHtml);
}

void ota_done()
{
    bool started = ota_started;
    bool unauth = upload_unauth;
    bool refused = upload_refused;
    bool finished = ota_ok;
    ota_started = false;
    ota_ok = false;
    upload_unauth = false;
    upload_refused = false;
    if (unauth || !auth_ok()) {
        send_unauth();
        return;
    }
    if (refused || (!started && gw_busy)) {
        server.send(503, "text/plain", "the start-up gateway check is running: try again in a moment");
        return;
    }
    if (!started) {
        server.send(400, "text/plain", "missing firmware upload\n");
        return;
    }
    bool ok = finished && !Update.hasError();
    server.send(ok ? 200 : 500, "text/plain", ok ? "new firmware written; restarting" : "OTA failed");
    if (ok) {
        delay(500);
        ESP.restart();
    }
}

void ota_upload()
{
    HTTPUpload &u = server.upload();
    if (u.status == UPLOAD_FILE_START) {
        ota_started = false;
        ota_ok = false;
        upload_unauth = !auth_ok();
        upload_refused = !upload_unauth && gw_busy;
        if (upload_unauth || upload_refused) {
            return;
        }
        ota_started = true;
        ota_ok = Update.begin((ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000);
        return;
    }
    if (upload_unauth || upload_refused || !ota_started) {
        if (u.status == UPLOAD_FILE_ABORTED) {
            upload_unauth = false;
            upload_refused = false;
        }
        return;
    }
    if (u.status == UPLOAD_FILE_WRITE) {
        if (ota_ok && Update.write(u.buf, u.currentSize) != u.currentSize) {
            ota_ok = false;
        }
    } else if (u.status == UPLOAD_FILE_END) {
        ota_ok = ota_ok && Update.end(true);
    } else if (u.status == UPLOAD_FILE_ABORTED) {
        Update.end(false);
        ota_started = false;
        ota_ok = false;
        upload_unauth = false;
        upload_refused = false;
    }
}

}  // namespace

void setup()
{
    Serial1.begin(115200);  // TX GPIO2: to the contact
    Serial.begin(115200);   // on the USB serial chip until the swap below: the start-up report goes there
    pad(OWS_CONTACT_RESET, false);
    contact_reset();
    settings_load();
    snprintf(name, sizeof name, "OWS-relay-%04X", (unsigned)(ESP.getChipId() & 0xFFFF));
    snprintf(mdns_bare, sizeof mdns_bare, "ows-relay-%04x", (unsigned)(ESP.getChipId() & 0xFFFF));
    snprintf(mdns_fqdn, sizeof mdns_fqdn, "%s.local", mdns_bare);
    WiFi.hostname(mdns_bare);
    WiFi.setAutoReconnect(false);
    WiFi.mode(WIFI_STA);
    WiFi.setSleepMode(WIFI_NONE_SLEEP);
    if (settings.ssid[0]) {
        down_tracking = true;
        down_since_ms = now_ms();
        next_reconnect_ms = down_since_ms + 15000;
        WiFi.begin(settings.ssid, settings.pass);
    } else {
        start_ap();
    }
    server.collectHeaders();  // enables collecting the Authorization header
    server.on("/", HTTP_GET, page_index);
    server.on("/setup", HTTP_GET, page_index);  // the address the setup access point's report gives
    server.on("/api/status", HTTP_GET, api_status);
    server.on("/setup", HTTP_POST, setup_post);
    server.on("/contact/reset", HTTP_POST, contact_reset_post);
    server.on("/contact/bootloader", HTTP_GET, contact_bootloader);
    server.on("/contact/bootloader", HTTP_POST, contact_bootloader);
    server.on("/contact/ccfg_mac", HTTP_POST, contact_ccfg_mac);
    server.on("/contact/gwsync", HTTP_POST, contact_gwsync_post);
    server.on("/ota", HTTP_POST, ota_done, ota_upload);
    server.on("/contact/flash", HTTP_POST, contact_flash_done, contact_flash_upload);
    server.on("/contact/stock", HTTP_POST, contact_stock_done, contact_stock_upload);
    server.on("/contact/nv", HTTP_GET, contact_nv_get);
    server.on("/contact/nv", HTTP_POST, contact_nv_post, contact_nv_upload);
#ifdef OWS_DEBUG_UART
    server.on("/debug/uart", HTTP_GET, debug_uart);
    server.on("/debug/boot", HTTP_GET, debug_boot);
    server.on("/debug/pins", HTTP_GET, debug_pins);
    server.on("/debug/t", HTTP_GET, debug_t);
    server.on("/debug/follow", HTTP_GET, debug_follow);
    server.on("/debug/sdrive", HTTP_GET, debug_sdrive);
#endif
    server.begin();
    char device[24];
    snprintf(device, sizeof device, "esp8266-%06x", (unsigned)ESP.getChipId());
    relay_io_t io = {tcp_connect, tcp_send, tcp_close, to_gateway, random32, log_line, nullptr};
    ready =
        settings.url[0] && relay_init(&relay, settings.url, device, settings.token[0] ? settings.token : nullptr, &io);
    buffer_init(&buffer, &relay, uart_out, nullptr);
    // the start-up report on USB (up to 15 s for Wi-Fi), then UART0 goes to the contact: GPIO13 RX, GPIO15 TX
    Serial.printf("\nOpenWyzeSense relay %s, bridge %s\n", name, settings.url[0] ? settings.url : "(not set)");
    for (int i = 0; settings.ssid[0] && i < 150 && !WiFi.isConnected(); i++) {
        delay(100);
    }
    if (WiFi.isConnected()) {
        down_tracking = false;
        start_mdns();
        Serial.printf("on %s as http://%s/ (http://%s/)\n", settings.ssid, WiFi.localIP().toString().c_str(),
                      mdns_fqdn);
    } else if (!settings.ssid[0]) {
        start_ap();
        Serial.printf("no Wi-Fi set up: setup access point %s, http://192.168.4.1/setup\n", name);
    } else {
        Serial.printf("no Wi-Fi yet: fallback access point %s after 120 s if still offline\n", name);
    }
    Serial.println("UART0 RX now comes from the contact (GPIO13); this port goes quiet");
    Serial.flush();
    Serial.swap();  // RX GPIO13 from the contact; its TX moves to GPIO15, which stays unconnected
    // the line idles high: a contact that does not drive its T pad (a half-written image) leaves it floating, and
    // noise comes in as bytes (seen on the LOLIN32, 2026-10-25)
    PIN_PULLUP_EN(PERIPHS_IO_MUX_MTCK_U);  // GPIO13
    gw_sync();                             // before anything is relayed: the contact on the carried gateway image
}

void loop()
{
    server.handleClient();
    uint32_t now = now_ms();
    bool connected = WiFi.isConnected();
    if (connected) {
        if (down_tracking || ap_up || fails) {
            down_tracking = false;
            ap_timed_out = false;
            fails = 0;
            stop_ap();
        }
        start_mdns();
        if (mdns_up) {
            MDNS.update();
        }
    } else if (settings.ssid[0]) {
        mdns_up = false;
        if (!down_tracking) {
            down_tracking = true;
            down_since_ms = now;
            fails = 0;
            next_reconnect_ms = now + kBackoffBaseMs;
        }
        if (!ap_up && !ap_timed_out && (int32_t)(now - (down_since_ms + kApDelayMs)) >= 0) {
            start_ap();
        } else if (ap_up && (int32_t)(now - (ap_since_ms + kApTimeoutMs)) >= 0) {
            stop_ap();
            ap_timed_out = true;
        }
        if ((int32_t)(now - next_reconnect_ms) >= 0) {
            uint32_t wait = kBackoffBaseMs << (fails < 5 ? fails : 5);
            if (wait > kBackoffMaxMs) {
                wait = kBackoffMaxMs;
            }
            if (fails < 30) {
                fails++;
            }
            next_reconnect_ms = now + (wait < 10000 ? 10000 : wait);
            WiFi.begin(settings.ssid, settings.pass);
        }
    }
    if (ready && connected) {
        relay_tick(&relay, now);
    }
    buffer_tick(&buffer, now);
    uint8_t buf[256];
    if (relay.state != RELAY_DOWN && !client.connected() && !client.available()) {
        relay_on_tcp_closed(&relay, now);
    }
    while (client.available()) {
        int n = client.read(buf, sizeof buf);
        if (n <= 0) {
            break;
        }
        relay_on_tcp(&relay, buf, (size_t)n, now_ms());
    }
    // the gateway, read also while Wi-Fi or the server is down: its alarms are ACKed and kept (buffer.c)
    size_t n = 0;
    while (Serial.available() && n < sizeof buf) {
        buf[n++] = (uint8_t)Serial.read();
    }
    if (n) {
        bytes_from_contact += n;
        buffer_from_gateway(&buffer, buf, n, now_ms());
    }
}

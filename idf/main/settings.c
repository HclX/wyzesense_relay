#include "settings.h"

#include <string.h>

#include "nvs.h"

settings_t settings;

static void get(nvs_handle_t h, const char *key, char *out, size_t cap)
{
    size_t len = cap;
    if (nvs_get_str(h, key, out, &len) != ESP_OK) {
        out[0] = 0;
    }
}

// spaces around a setting (saved before the setup page trimmed them: a phone keyboard's trailing space)
static void trim(char *s)
{
    size_t n = strlen(s), i = 0;
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) {
        s[--n] = 0;
    }
    while (s[i] == ' ' || s[i] == '\t') {
        i++;
    }
    memmove(s, s + i, n - i + 1);
}

void settings_load(void)
{
    nvs_handle_t h;
    memset(&settings, 0, sizeof settings);
    if (nvs_open("ows", NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    get(h, "ssid", settings.ssid, sizeof settings.ssid);
    get(h, "pass", settings.pass, sizeof settings.pass);
    get(h, "url", settings.url, sizeof settings.url);
    get(h, "token", settings.token, sizeof settings.token);
    get(h, "admin_pass", settings.admin_pass, sizeof settings.admin_pass);
    get(h, "wire_power", settings.wire_power, sizeof settings.wire_power);
    get(h, "wire_minus", settings.wire_minus, sizeof settings.wire_minus);
    get(h, "wire_r", settings.wire_r, sizeof settings.wire_r);
    get(h, "wire_t", settings.wire_t, sizeof settings.wire_t);
    get(h, "wire_m", settings.wire_m, sizeof settings.wire_m);
    get(h, "wire_c", settings.wire_c, sizeof settings.wire_c);
    get(h, "wire_s", settings.wire_s, sizeof settings.wire_s);
    uint8_t skip = 0;
    if (nvs_get_u8(h, "skip_gwsync", &skip) == ESP_OK) {
        settings.skip_gwsync = skip != 0;
    }
    uint8_t stock_gw = 0;
    if (nvs_get_u8(h, "stock_gw", &stock_gw) == ESP_OK) {
        settings.stock_gw = stock_gw != 0;
    }
    uint8_t rig = 0;
    if (nvs_get_u8(h, "rig", &rig) == ESP_OK) {
        settings.rig = rig != 0;
    }
    uint8_t cflash = 0;  // GWSYNC_VIA_AUTO when never set
    if (nvs_get_u8(h, "cflash", &cflash) == ESP_OK && cflash <= 2) {
        settings.contact_flash = cflash;
    }
    uint8_t role = 0;
    if (nvs_get_u8(h, "role", &role) == ESP_OK) {
        settings.role = role == ROLE_STATION ? ROLE_STATION : ROLE_RELAY;
    }
    nvs_close(h);
    trim(settings.ssid);
    trim(settings.url);
    trim(settings.token);
}

bool settings_save(void)
{
    nvs_handle_t h;
    if (nvs_open("ows", NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    bool ok =
        nvs_set_str(h, "ssid", settings.ssid) == ESP_OK && nvs_set_str(h, "pass", settings.pass) == ESP_OK &&
        nvs_set_str(h, "url", settings.url) == ESP_OK && nvs_set_str(h, "token", settings.token) == ESP_OK &&
        nvs_set_str(h, "admin_pass", settings.admin_pass) == ESP_OK &&
        nvs_set_str(h, "wire_power", settings.wire_power) == ESP_OK &&
        nvs_set_str(h, "wire_minus", settings.wire_minus) == ESP_OK &&
        nvs_set_str(h, "wire_r", settings.wire_r) == ESP_OK && nvs_set_str(h, "wire_t", settings.wire_t) == ESP_OK &&
        nvs_set_str(h, "wire_m", settings.wire_m) == ESP_OK && nvs_set_str(h, "wire_c", settings.wire_c) == ESP_OK &&
        nvs_set_str(h, "wire_s", settings.wire_s) == ESP_OK &&
        nvs_set_u8(h, "skip_gwsync", settings.skip_gwsync ? 1 : 0) == ESP_OK &&
        nvs_set_u8(h, "stock_gw", settings.stock_gw ? 1 : 0) == ESP_OK &&
        nvs_set_u8(h, "rig", settings.rig ? 1 : 0) == ESP_OK && nvs_set_u8(h, "role", settings.role) == ESP_OK &&
        nvs_set_u8(h, "cflash", settings.contact_flash) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

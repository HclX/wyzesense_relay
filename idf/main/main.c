// The ESP32-S3 relay (issue #22): a v1 contact board running our gateway, relayed over Wi-Fi to wyzesense2mqtt-rs.
// Wiring and use: esp32/README.md.
#include "cjtag_task.h"
#include "contact.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net.h"
#include "nvs_flash.h"
#include "relay_task.h"
#include "rig_task.h"
#include "settings.h"
#include "web.h"

static const char *TAG = "ows";

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    settings_load();
    if (settings.role != ROLE_STATION) {
        rig_task_start();  // first: when the rig is the contact's supply, power it before anything talks to it
    }
    if (settings.role == ROLE_STATION) {
        cjtag_task_start();
    }
    if (settings.role != ROLE_STATION) {
        contact_init();
        contact_reset();  // a known start: the contact's firmware, W released
    }
    net_start();
    web_start();
    if (settings.role != ROLE_STATION) {
        relay_task_start();
    }
    ESP_LOGI(TAG, "OpenWyzeSense relay %s up", net_name());
    // a new firmware from /ota stays only if it joins the configured Wi-Fi (or brings up the setup AP when
    // unprovisioned); otherwise the bootloader rolls back to the previous one at the next reset
    while (!net_connected() && (settings.ssid[0] != '\0' || !net_setup_ap())) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    esp_ota_mark_app_valid_cancel_rollback();
}

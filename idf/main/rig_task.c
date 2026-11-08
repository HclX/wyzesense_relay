#include "rig_task.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "driver/rtc_io.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "settings.h"
#include "pins.h"

#define HOLD_MAX_US \
    50000  // a hold ends after this even if the driven supply never reads above v_release_mv: near
           // 3.3 V the classic ESP32's ADC saturates, so a threshold up there may never be reached

static const char *TAG = "rig";

static adc_oneshot_unit_handle_t adc;
static adc_cali_handle_t cali;
static adc_channel_t chan;
static bool available;
static volatile bool want_measure, measuring;
static rig_cfg_t next_cfg;
static rig_t rig;  // written by the rig task only; read for the status (a torn read shows one odd value at worst)

static void supply(bool on)
{
    if (on) {
        rtc_gpio_set_level(OWS_PIN_RIG_SUPPLY, 1);
        rtc_gpio_set_direction(OWS_PIN_RIG_SUPPLY, RTC_GPIO_MODE_OUTPUT_ONLY);
    } else {
        rtc_gpio_set_direction(OWS_PIN_RIG_SUPPLY, RTC_GPIO_MODE_DISABLED);  // high impedance: the ADC reads C
    }
}

static uint16_t read_mv(void)
{
    int raw = 0, mv = 0;
    if (adc_oneshot_read(adc, chan, &raw) != ESP_OK) {
        return 0;
    }
    if (cali && adc_cali_raw_to_voltage(cali, raw, &mv) == ESP_OK) {
        return (uint16_t)mv;
    }
    return (uint16_t)(raw * 3300 / 4095);
}

static void task(void *arg)
{
    (void)arg;
    for (;;) {
        if (!want_measure) {
            supply(true);
            measuring = false;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        // measuring: a busy loop that must answer a fall within tens of µs, so no yielding; CPU1's idle task gets
        // no time meanwhile, so it leaves the task watchdog until the measurement ends
        TaskHandle_t idle1 = xTaskGetIdleTaskHandleForCore(1);
        esp_task_wdt_delete(idle1);
        rig_start(&rig, &next_cfg);
        measuring = true;
        rig_kind_t last = RIG_WAIT;
        uint64_t hold_from = 0;
        while (want_measure) {
            uint64_t now = (uint64_t)esp_timer_get_time();
            uint16_t v = read_mv();
            bool driven = last == RIG_HOLD;
            rig_action_t a = rig_step(&rig, now, v, driven);
            if (a.kind == RIG_HOLD) {
                if (last != RIG_HOLD) {
                    hold_from = now;
                }
                supply(true);
                if (now - hold_from > HOLD_MAX_US) {  // see HOLD_MAX_US: let go; it holds again if it must
                    supply(false);
                    a.kind = RIG_WAIT;
                } else {
                    esp_rom_delay_us(a.us);
                }
            } else if (a.kind == RIG_PULSE) {
                supply(true);
                esp_rom_delay_us(a.us);
                supply(false);
            } else {
                supply(false);
            }
            last = a.kind;
        }
        supply(true);
        measuring = false;
        esp_task_wdt_add(idle1);
    }
}

void rig_task_start(void)
{
    rig_default_cfg(&next_cfg);
#if OWS_PIN_RIG_SUPPLY >= 0
    if (!settings.rig) {
        return;
    }
    const int gpio = OWS_PIN_RIG_SUPPLY;
    adc_unit_t unit;
    adc_oneshot_unit_init_cfg_t ucfg = {.ulp_mode = ADC_ULP_MODE_DISABLE};
    if (adc_oneshot_io_to_channel(gpio, &unit, &chan) != ESP_OK || unit != ADC_UNIT_1) {  // ADC2 is Wi-Fi's
        ESP_LOGE(TAG, "GPIO%d is not an ADC1 pin: no rig", gpio);
        return;
    }
    ucfg.unit_id = unit;
    adc_oneshot_chan_cfg_t ccfg = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12};
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&ucfg, &adc));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc, chan, &ccfg));
    adc_cali_line_fitting_config_t cc = {.unit_id = unit, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12};
    if (adc_cali_create_scheme_line_fitting(&cc, &cali) != ESP_OK) {
        cali = NULL;
    }
    // the pad stays on the RTC mux (the ADC's): its RTC output drives the supply, released it is high impedance
    rtc_gpio_init(gpio);
    rtc_gpio_pullup_dis(gpio);
    rtc_gpio_pulldown_dis(gpio);
    rtc_gpio_set_drive_capability(gpio, GPIO_DRIVE_CAP_3);
    supply(true);
    available = true;
    xTaskCreatePinnedToCore(task, "rig", 4096, NULL, 20, NULL, 1);
    ESP_LOGI(TAG, "rig on GPIO%d (ADC1 channel %d): supplying the board", gpio, (int)chan);
#endif
}

bool rig_task_available(void)
{
    return available;
}

bool rig_task_measure(bool on, const rig_cfg_t *cfg)
{
    if (!available) {
        return false;
    }
    if (on) {
        want_measure = false;  // a fresh start
        while (measuring) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (cfg) {
            next_cfg = *cfg;
        }
    }
    want_measure = on;
    return true;
}

void rig_task_cfg(rig_cfg_t *cfg)
{
    *cfg = next_cfg;
}

size_t rig_task_json(char *out, size_t cap)
{
    const rig_t *r = &rig;
    double secs = r->started ? (double)(r->now_us - r->t0_us) / 1e6 : 0;
    int n =
        snprintf(out, cap,
                 "{\"available\":%s,\"mode\":\"%s\",\"seconds\":%.1f,\"v_mv\":%u,\"v_min_mv\":%u,\"v_max_mv\":%u,"
                 "\"sleep_na\":%" PRId64 ",\"decays\":%" PRIu32 ",\"refills\":%" PRIu32 ",\"pulses\":%" PRIu32
                 ",\"wakes\":%" PRIu32 ",\"wake_ms\":%.1f,\"wake_gap_s\":[%.2f,%.2f,%.2f],\"holds\":%" PRIu32
                 ",\"hold_ms\":%.1f,"
                 "\"cfg\":{\"cap_nf\":%" PRIu32 ",\"v_low_mv\":%u,\"v_high_mv\":%u,\"v_busy_mv\":%u,"
                 "\"v_hold_mv\":%u,\"v_release_mv\":%u,\"pulse_us\":%" PRIu32 ",\"hold_us\":%" PRIu32 "}}",
                 available ? "true" : "false", measuring ? "measure" : (available ? "supply" : "off"), secs, r->v_mv,
                 r->v_min_mv, r->v_max_mv, r->started ? rig_sleep_na(r) : (int64_t)-1, r->quiet_segments, r->refills,
                 r->pulses, r->wakes, r->wake_time_us / 1000.0, r->wake_gap_min_us / 1e6,
                 r->wake_gaps ? (double)r->wake_gap_sum_us / r->wake_gaps / 1e6 : 0.0, r->wake_gap_max_us / 1e6,
                 r->holds, r->hold_time_us / 1000.0, next_cfg.cap_nf, next_cfg.v_low_mv, next_cfg.v_high_mv,
                 next_cfg.v_busy_mv, next_cfg.v_hold_mv, next_cfg.v_release_mv, next_cfg.pulse_us, next_cfg.hold_us);
    return n < 0 ? 0 : ((size_t)n < cap ? (size_t)n : cap - 1);
}

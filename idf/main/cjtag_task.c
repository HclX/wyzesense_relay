#include "cjtag_task.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_private/gpio.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sbl.h"
#include "sblprobe.h"
#include "settings.h"
#include "pins.h"

#define SBL_UART UART_NUM_2

static const char *TAG = "station";

typedef enum {
    STATE_IDLE = 0,
    STATE_ARMED,
    STATE_RUNNING,
    STATE_SLIPPED,
    STATE_DONE,
    STATE_ERROR,
} task_state_t;

typedef struct {
    int tck_pin;
    int tms_pin;
} esp_pin_ctx_t;

static SemaphoreHandle_t lock;
static uint8_t *page_bufs[CJTAG_TOTAL_PAGES];
static uint8_t fcfg1_buf[CJTAG_FCFG1_SIZE];

static volatile task_state_t state = STATE_IDLE;
static volatile cjtag_mode_t mode = CJTAG_MODE_NONE;
static volatile cjtag_transport_t cfg_transport = CJTAG_TRANSPORT_CJTAG;
static volatile uint8_t active_transport;  // 0 = none, 1 = cjtag, 2 = sbl
static volatile bool task_busy;

static const int cfg_pin_a = OWS_PIN_STATION_A;  // fixed: pins.h (the relay's harness)
static const int cfg_pin_b = OWS_PIN_STATION_B;
static int active_tck = -1;
static int active_tms = -1;

static const int cfg_sbl_pins[SBLPROBE_PADS] = {
    OWS_PIN_STATION_SBL_0,
    OWS_PIN_STATION_SBL_1,
    OWS_PIN_STATION_SBL_2,
    OWS_PIN_STATION_SBL_3,
};
static const int cfg_power_pin = OWS_PIN_STATION_POWER;
static bool sbl_uart_installed;
static bool sbl_uart_is_open;
static int sbl_active_rx_pad = -1;
static int sbl_active_tx_pad = -1;
static bool sbl_powered;
static bool sbl_pad_released[SBLPROBE_PADS] = {true, true, true, true};
// The identification (the user, 2026-11-06: identify, then dump or flash as separate steps): what Identify found and
// confirmed, the only pins Dump and Flash use; they assume the operator puts a sensor back on the same pads.
static bool identified;
static uint8_t id_transport;  // 1 = cjtag, 2 = sbl
static int id_tck = -1, id_tms = -1;
static char id_mac[9];
static bool have_sbl_map;  // id_transport == 2: saved_sbl_map holds the pads
static int id_pads = 4;    // SBL: 4 pads (a fixture on R, T, S, W) or 2 (hand-held probes on R, T)

// Two hand-held probes (the user, 2026-11-07: + and - clipped to the battery holder, two hands on the sensor's TX and
// RX): the station tries only once both probes have been on the sensor for 200 ms, and after an attempt that found
// nothing, only once they have been lifted and put back. "On the sensor" is a 20 us pull-up pulse on the probe with
// the sensor unpowered (its supply pin driven low): in the air the line reads high at once; on a pad, that pin's
// protection diode into the unpowered supply holds it near 0.5 V, low. About 1 nC a pulse: nothing is powered by it.
#define TOUCH_POLL_MS 50
#define TOUCH_STEADY  4  // polls: 200 ms
static int touch_streak;
static bool need_lift;
static sblprobe_result_t saved_sbl_map;

static size_t staged_bytes;
static uint32_t staged_pages;
static bool staged_wipe_nv;

static uint32_t pages_done_mask;
static uint32_t pages_done_count;
static uint32_t total_pages_target;
static uint32_t current_page;
static uint32_t slip_count;
static uint32_t done_seq;
static bool dump_ready;
static bool fcfg1_ready;

static char active_mac[9];
static char backed_up_mac[9];
static uint32_t active_user_id;
static uint32_t active_bl_config;
static uint64_t active_ieee_mac;
static char status_msg[256] = "Disarmed — select Dump or Flash to arm hands-free probing.";

static void json_str(char *out, size_t cap, const char *s)
{
    size_t n = 0;
    out[n++] = '"';
    for (; *s && n + 8 < cap; s++) {
        if (*s == '"' || *s == '\\') {
            out[n++] = '\\';
            out[n++] = *s;
        } else if ((unsigned char)*s < 0x20) {
            n += (size_t)snprintf(out + n, cap - n, "\\u%04x", (unsigned char)*s);
        } else {
            out[n++] = *s;
        }
    }
    out[n++] = '"';
    out[n] = 0;
}

static void free_page_bufs(void)
{
    for (uint32_t i = 0; i < CJTAG_TOTAL_PAGES; i++) {
        free(page_bufs[i]);
        page_bufs[i] = NULL;
    }
}

static bool ensure_page_bufs(void)
{
    for (uint32_t i = 0; i < CJTAG_TOTAL_PAGES; i++) {
        if (!page_bufs[i]) {
            page_bufs[i] = malloc(CJTAG_PAGE);
            if (!page_bufs[i]) {
                free_page_bufs();
                return false;
            }
        }
    }
    return true;
}

// Released: floating, no pull-up, so a probed sensor whose supply the station switches off is never fed through them
static void release_cjtag_pins(int pin_a, int pin_b)
{
    if (pin_a >= 0) {
        gpio_reset_pin((gpio_num_t)pin_a);
        gpio_set_direction((gpio_num_t)pin_a, GPIO_MODE_INPUT);
        gpio_set_pull_mode((gpio_num_t)pin_a, GPIO_FLOATING);
    }
    if (pin_b >= 0) {
        gpio_reset_pin((gpio_num_t)pin_b);
        gpio_set_direction((gpio_num_t)pin_b, GPIO_MODE_INPUT);
        gpio_set_pull_mode((gpio_num_t)pin_b, GPIO_FLOATING);
    }
}

static void prepare_cjtag_pins(int tck_pin, int tms_pin)
{
    gpio_reset_pin((gpio_num_t)tck_pin);
    gpio_set_direction((gpio_num_t)tck_pin, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)tck_pin, 1);

    gpio_reset_pin((gpio_num_t)tms_pin);
    gpio_set_pull_mode((gpio_num_t)tms_pin, GPIO_PULLUP_ONLY);
    gpio_set_direction((gpio_num_t)tms_pin, GPIO_MODE_INPUT_OUTPUT);
    gpio_set_level((gpio_num_t)tms_pin, 1);
}

static void io_set_pins(void *ctx, bool tck, bool tms)
{
    const esp_pin_ctx_t *p = (const esp_pin_ctx_t *)ctx;
    if (!tck) {
        gpio_set_level((gpio_num_t)p->tck_pin, 0);
        gpio_set_level((gpio_num_t)p->tms_pin, tms ? 1 : 0);
    } else {
        gpio_set_level((gpio_num_t)p->tms_pin, tms ? 1 : 0);
        gpio_set_level((gpio_num_t)p->tck_pin, 1);
    }
}

static void io_set_tms_dir(void *ctx, bool output)
{
    const esp_pin_ctx_t *p = (const esp_pin_ctx_t *)ctx;
    gpio_set_direction((gpio_num_t)p->tms_pin, output ? GPIO_MODE_INPUT_OUTPUT : GPIO_MODE_INPUT);
}

static bool io_get_tms(void *ctx)
{
    const esp_pin_ctx_t *p = (const esp_pin_ctx_t *)ctx;
    return gpio_get_level((gpio_num_t)p->tms_pin) != 0;
}

static void io_delay_us(void *ctx, uint32_t us)
{
    (void)ctx;
    if (us > 1) {
        esp_rom_delay_us(us);
    }
}

// --- SBL station hardware callbacks ---

static bool ensure_sbl_uart(void)
{
    if (sbl_uart_installed) {
        return true;
    }
    uart_config_t u = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    if (uart_driver_install(SBL_UART, 2048, 0, 0, NULL, 0) != ESP_OK) {
        return false;
    }
    if (uart_param_config(SBL_UART, &u) != ESP_OK) {
        return false;
    }
    sbl_uart_installed = true;
    return true;
}

static void sbl_pin_low(void *ctx, int pad)
{
    (void)ctx;
    if (pad < 0 || pad >= SBLPROBE_PADS || cfg_sbl_pins[pad] < 0) {
        return;
    }
    sbl_pad_released[pad] = false;
    gpio_num_t pin = (gpio_num_t)cfg_sbl_pins[pad];
    gpio_reset_pin(pin);
    gpio_set_level(pin, 0);
    gpio_set_direction(pin, GPIO_MODE_INPUT_OUTPUT_OD);
    gpio_set_pull_mode(pin, sbl_powered ? GPIO_PULLUP_ONLY : GPIO_FLOATING);
}

static void sbl_pin_release(void *ctx, int pad)
{
    (void)ctx;
    if (pad < 0 || pad >= SBLPROBE_PADS || cfg_sbl_pins[pad] < 0) {
        return;
    }
    sbl_pad_released[pad] = true;
    gpio_num_t pin = (gpio_num_t)cfg_sbl_pins[pad];
    gpio_reset_pin(pin);
    gpio_set_direction(pin, GPIO_MODE_INPUT);
    gpio_set_pull_mode(pin, sbl_powered ? GPIO_PULLUP_ONLY : GPIO_FLOATING);
}

static bool sbl_pin_read(void *ctx, int pad)
{
    (void)ctx;
    if (pad < 0 || pad >= SBLPROBE_PADS || cfg_sbl_pins[pad] < 0) {
        return false;
    }
    return gpio_get_level((gpio_num_t)cfg_sbl_pins[pad]) != 0;
}

static bool sbl_pin_touching(void *ctx, int pad)
{
    (void)ctx;
    if (pad < 0 || pad >= SBLPROBE_PADS || cfg_sbl_pins[pad] < 0 || sbl_powered || sbl_uart_is_open) {
        return false;
    }
    gpio_num_t pin = (gpio_num_t)cfg_sbl_pins[pad];
    gpio_set_direction(pin, GPIO_MODE_INPUT);
    gpio_pullup_en(pin);
    esp_rom_delay_us(20);
    bool high = gpio_get_level(pin) != 0;
    gpio_pullup_dis(pin);
    return !high;
}

static void release_sbl_pins(bool power_off);
static void sbl_power(void *ctx, bool on);

// Whether to try the two hand-held probes now (see TOUCH_POLL_MS): waits (one poll) otherwise. The sensor unpowered.
static bool touch_gate(const sblprobe_io_t *io)
{
    if (sbl_powered || sbl_uart_is_open) {
        release_sbl_pins(true);
    } else {
        // the supply driven low, not left floating: a sensor whose capacitors still hold charge (the relay powered it
        // before a switch to the station, bench 2026-11-07) reads high on every pad, as if no probe touched it
        sbl_power(NULL, false);
    }
    bool on = sblprobe_touched(io, 2);
    if (need_lift) {
        touch_streak = on ? 0 : touch_streak + 1;
        if (touch_streak >= TOUCH_STEADY) {
            need_lift = false;
            touch_streak = 0;
        }
    } else {
        touch_streak = on ? touch_streak + 1 : 0;
        if (touch_streak >= TOUCH_STEADY) {
            touch_streak = 0;
            return true;
        }
    }
    vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS));
    return false;
}

static void sbl_power(void *ctx, bool on)
{
    (void)ctx;
    sbl_powered = on;
    for (int i = 0; i < SBLPROBE_PADS; i++) {
        if (cfg_sbl_pins[i] >= 0 && sbl_pad_released[i]) {
            gpio_set_pull_mode((gpio_num_t)cfg_sbl_pins[i], on ? GPIO_PULLUP_ONLY : GPIO_FLOATING);
        }
    }
    if (cfg_power_pin >= 0) {
        gpio_set_direction((gpio_num_t)cfg_power_pin, GPIO_MODE_OUTPUT);
        gpio_set_drive_capability((gpio_num_t)cfg_power_pin, GPIO_DRIVE_CAP_3);  // a sensor's radio bursts
        gpio_set_level((gpio_num_t)cfg_power_pin, on ? 1 : 0);
    }
}

static void sbl_uart_close(void *ctx)
{
    (void)ctx;
    if (!sbl_uart_is_open) {
        return;
    }
    uart_set_pin(SBL_UART, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    int rx = sbl_active_rx_pad;
    int tx = sbl_active_tx_pad;
    sbl_uart_is_open = false;
    sbl_active_rx_pad = -1;
    sbl_active_tx_pad = -1;
    if (rx >= 0) {
        sbl_pin_release(NULL, rx);
    }
    if (tx >= 0) {
        sbl_pin_release(NULL, tx);
    }
}

static bool sbl_uart_open(void *ctx, int rx_pad, int tx_pad, bool tx_open_drain)
{
    (void)ctx;
    if (state != STATE_ARMED && state != STATE_SLIPPED && state != STATE_RUNNING) {
        return false;
    }
    if (rx_pad < 0 || rx_pad >= SBLPROBE_PADS || tx_pad < 0 || tx_pad >= SBLPROBE_PADS || rx_pad == tx_pad) {
        return false;
    }
    int rx_gpio = cfg_sbl_pins[rx_pad];
    int tx_gpio = cfg_sbl_pins[tx_pad];
    if (rx_gpio < 0 || tx_gpio < 0 || !ensure_sbl_uart()) {
        return false;
    }
    gpio_reset_pin((gpio_num_t)rx_gpio);
    gpio_reset_pin((gpio_num_t)tx_gpio);
    if (uart_set_pin(SBL_UART, tx_gpio, rx_gpio, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
        return false;
    }
    gpio_pullup_en((gpio_num_t)rx_gpio);
    if (tx_open_drain) {
        gpio_pullup_en((gpio_num_t)tx_gpio);
        gpio_od_enable((gpio_num_t)tx_gpio);
    }
    sbl_uart_is_open = true;
    sbl_active_rx_pad = rx_pad;
    sbl_active_tx_pad = tx_pad;
    sbl_pad_released[rx_pad] = false;
    sbl_pad_released[tx_pad] = false;
    return true;
}

static bool sbl_uart_write(void *ctx, const uint8_t *data, size_t len)
{
    (void)ctx;
    if (!sbl_uart_is_open) {
        return false;
    }
    int n = uart_write_bytes(SBL_UART, data, len);
    return n == (int)len && uart_wait_tx_done(SBL_UART, pdMS_TO_TICKS(1000)) == ESP_OK;
}

static size_t sbl_uart_read(void *ctx, uint8_t *buf, size_t cap, uint32_t timeout_ms)
{
    (void)ctx;
    if (!sbl_uart_is_open || !cap || uart_read_bytes(SBL_UART, buf, 1, pdMS_TO_TICKS(timeout_ms)) != 1) {
        return 0;
    }
    size_t n = 1;
    while (n < cap) {
        size_t more = 0;
        uart_get_buffered_data_len(SBL_UART, &more);
        if (more > 0) {
            more = more < cap - n ? more : cap - n;
            int k = uart_read_bytes(SBL_UART, buf + n, more, 0);
            if (k > 0) {
                n += (size_t)k;
                continue;
            }
        }
        if (uart_read_bytes(SBL_UART, buf + n, 1, pdMS_TO_TICKS(15)) == 1) {
            n++;
        } else {
            break;
        }
    }
    return n;
}

static void sbl_uart_flush(void *ctx)
{
    (void)ctx;
    if (sbl_uart_is_open) {
        uart_flush_input(SBL_UART);
    }
}

static void sbl_sleep_ms(void *ctx, uint32_t ms)
{
    (void)ctx;
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static void release_sbl_pins(bool power_off)
{
    sbl_uart_close(NULL);
    for (int i = 0; i < SBLPROBE_PADS; i++) {
        sbl_pin_release(NULL, i);
    }
    if (power_off) {
        sbl_power(NULL, false);
    }
}

static uint32_t popcount32(uint32_t x)
{
    uint32_t c = 0;
    while (x) {
        c += x & 1u;
        x >>= 1;
    }
    return c;
}

static bool is_ascii_hex_mac(const uint8_t *p)
{
    for (int i = 0; i < 8; i++) {
        if (!((p[i] >= '0' && p[i] <= '9') || (p[i] >= 'A' && p[i] <= 'F'))) {
            return false;
        }
    }
    return memcmp(p, "00000000", 8) != 0;
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

__attribute__((format(printf, 1, 2))) static void note(const char *fmt, ...)  // a step, shown as it happens
{
    char line[sizeof status_msg];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    xSemaphoreTake(lock, portMAX_DELAY);
    bool same = !strcmp(status_msg, line);  // a waiting line repeated every poll: logged once
    if (state == STATE_ARMED || state == STATE_SLIPPED) {
        memcpy(status_msg, line, sizeof status_msg);
    }
    xSemaphoreGive(lock);
    if (!same) {
        ESP_LOGI(TAG, "%s", line);
    }
}

// Step 1 and 2 of every sensor (the user, 2026-11-06: hands-free, the pins identified first, flash read or written
// only once they are confirmed): identify (a connect: the pins, and who answers: MAC, IEEE MAC, user ID, CCFG CRC),
// then confirm (the pins released, 100 ms, a second connect from scratch that must find the same chip on the same
// pins). A bouncing probe, a half-made contact or a second sensor never gets to a read or a write.
// pin_a is tried as TCKC first, then (both) the other way round.
static bool probe_with_polarity(cjtag_t *cj, esp_pin_ctx_t *pctx, int pin_a, int pin_b, bool both)
{
    if (pin_a < 0 || pin_b < 0) {
        return false;
    }
    int pairs[2][2] = {{pin_a, pin_b}, {pin_b, pin_a}};
    for (int i = 0; i < (both ? 2 : 1); i++) {
        pctx->tck_pin = pairs[i][0];
        pctx->tms_pin = pairs[i][1];
        prepare_cjtag_pins(pctx->tck_pin, pctx->tms_pin);
        cjtag_io_t io = {
            .set_pins = io_set_pins,
            .set_tms_dir = io_set_tms_dir,
            .get_tms = io_get_tms,
            .delay_us = io_delay_us,
            .ctx = pctx,
        };
        cjtag_init(cj, &io);
        if (cjtag_connect(cj, true)) {
            char mac[9];
            memcpy(mac, cj->mac, sizeof mac);
            uint64_t ieee = cj->ieee_mac;
            uint32_t uid = cj->user_id, crc = cj->page31_crc;
            note("Found %s on cJTAG (TCKC GPIO%d, TMSC GPIO%d): confirming", mac, pctx->tck_pin, pctx->tms_pin);
            release_cjtag_pins(pin_a, pin_b);
            vTaskDelay(pdMS_TO_TICKS(100));
            prepare_cjtag_pins(pctx->tck_pin, pctx->tms_pin);
            cjtag_init(cj, &io);
            if (cjtag_connect(cj, true) && !strcmp(cj->mac, mac) && cj->ieee_mac == ieee && cj->user_id == uid &&
                cj->page31_crc == crc) {
                note("Confirmed %s on cJTAG (TCKC GPIO%d, TMSC GPIO%d)", mac, pctx->tck_pin, pctx->tms_pin);
                return true;
            }
            note("Not confirmed (%s answered once, then not the same): hold the probes still", mac);
        }
        release_cjtag_pins(pin_a, pin_b);
    }
    return false;
}

static void run_cjtag_session(cjtag_t *cj, const esp_pin_ctx_t *pctx, int pin_a, int pin_b)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    if (state != STATE_ARMED && state != STATE_SLIPPED) {
        xSemaphoreGive(lock);
        cjtag_release_and_reset(cj);
        release_cjtag_pins(pin_a, pin_b);
        return;
    }

    // If resuming from a slip, ensure the user re-touched the same sensor MAC
    if (state == STATE_SLIPPED && active_mac[0] != '\0' && strcmp(active_mac, cj->mac) != 0) {
        snprintf(status_msg, sizeof status_msg, "Sensor MAC changed (%s != %s) — re-touch %s or re-arm", cj->mac,
                 active_mac, active_mac);
        xSemaphoreGive(lock);
        cjtag_release_and_reset(cj);
        release_cjtag_pins(pin_a, pin_b);
        vTaskDelay(pdMS_TO_TICKS(500));
        return;
    }

    active_transport = 1;
    active_tck = pctx->tck_pin;
    active_tms = pctx->tms_pin;
    memcpy(active_mac, cj->mac, sizeof active_mac);
    active_user_id = cj->user_id;
    active_bl_config = cj->bl_config;
    active_ieee_mac = cj->ieee_mac;

    if (mode == CJTAG_MODE_FLASH && !cj->has_ccfg_mac) {
        state = STATE_ERROR;
        snprintf(status_msg, sizeof status_msg, "Refused: sensor %s has no factory MAC in CCFG (page 31)", cj->mac);
        xSemaphoreGive(lock);
        cjtag_release_and_reset(cj);
        release_cjtag_pins(pin_a, pin_b);
        return;
    }

    if (mode == CJTAG_MODE_FLASH && strcmp(backed_up_mac, cj->mac) != 0) {
        state = STATE_ERROR;
        snprintf(status_msg, sizeof status_msg,
                 "Refused: dump & download %s's stock flash first before flashing (CLAUDE.md backup rule)", cj->mac);
        xSemaphoreGive(lock);
        cjtag_release_and_reset(cj);
        release_cjtag_pins(pin_a, pin_b);
        return;
    }

    if (!ensure_page_bufs()) {
        state = STATE_ERROR;
        snprintf(status_msg, sizeof status_msg, "out of memory allocating 128 KB page buffers");
        xSemaphoreGive(lock);
        cjtag_release_and_reset(cj);
        release_cjtag_pins(pin_a, pin_b);
        return;
    }

    state = STATE_RUNNING;
    task_busy = true;
    snprintf(status_msg, sizeof status_msg, "Locked %s (TCKC=GPIO%d, TMSC=GPIO%d) — %s…", active_mac, active_tck,
             active_tms, mode == CJTAG_MODE_DUMP ? "dumping" : "flashing");
    xSemaphoreGive(lock);
    ESP_LOGI(TAG, "%s", status_msg);

    bool slipped = false;
    if (mode == CJTAG_MODE_DUMP) {
        if (!cjtag_dump_fcfg1(cj, fcfg1_buf)) {
            slipped = true;
        }
        for (uint32_t p = 0; !slipped && p < CJTAG_TOTAL_PAGES; p++) {
            if (state != STATE_RUNNING) {
                break;
            }
            if ((pages_done_mask & (1u << p)) != 0) {
                continue;
            }
            current_page = p;
            if (!cjtag_dump_page(cj, p, page_bufs[p])) {
                slipped = true;
                break;
            }
            xSemaphoreTake(lock, portMAX_DELAY);
            pages_done_mask |= (1u << p);
            pages_done_count = popcount32(pages_done_mask);
            snprintf(status_msg, sizeof status_msg, "Dumping %s: page %" PRIu32 " / 32 verified", active_mac,
                     pages_done_count);
            xSemaphoreGive(lock);
            ESP_LOGI(TAG, "%s", status_msg);
            vTaskDelay(1);
        }
    } else if (mode == CJTAG_MODE_FLASH) {
        for (uint32_t p = 0; p < total_pages_target; p++) {
            if (state != STATE_RUNNING) {
                break;
            }
            if ((pages_done_mask & (1u << p)) != 0) {
                continue;
            }
            uint32_t page_idx = (p == staged_pages) ? CJTAG_NV_PAGE_IDX : p;
            current_page = page_idx;
            bool allow_nv = (page_idx == CJTAG_NV_PAGE_IDX);
            if (!cjtag_flash_page(cj, page_idx, page_bufs[page_idx], allow_nv)) {
                slipped = true;
                break;
            }
            xSemaphoreTake(lock, portMAX_DELAY);
            pages_done_mask |= (1u << p);
            pages_done_count = popcount32(pages_done_mask);
            snprintf(status_msg, sizeof status_msg, "Flashing %s: page %" PRIu32 " / %" PRIu32 " verified", active_mac,
                     pages_done_count, total_pages_target);
            xSemaphoreGive(lock);
            ESP_LOGI(TAG, "%s", status_msg);
            vTaskDelay(1);
        }
    }

    cjtag_release_and_reset(cj);
    release_cjtag_pins(pin_a, pin_b);

    xSemaphoreTake(lock, portMAX_DELAY);
    task_busy = false;
    if (state != STATE_RUNNING) {
        xSemaphoreGive(lock);
        return;
    }

    if (slipped) {
        slip_count++;
        state = STATE_SLIPPED;
        snprintf(status_msg, sizeof status_msg,
                 "Probe slipped at page %" PRIu32 " (%s) — touch M/C again to resume (%" PRIu32 "/%" PRIu32 " done)",
                 current_page, cj->error, pages_done_count, total_pages_target);
        xSemaphoreGive(lock);
        ESP_LOGW(TAG, "%s", status_msg);
        vTaskDelay(pdMS_TO_TICKS(250));
        return;
    }

    if (mode == CJTAG_MODE_DUMP) {
        dump_ready = true;
        fcfg1_ready = true;
        snprintf(status_msg, sizeof status_msg,
                 "Dump complete for %s (128 KB flash + 1 KB FCFG1 verified). Lift probes!", active_mac);
    } else {
        staged_bytes = 0;
        staged_pages = 0;
        free_page_bufs();
        snprintf(status_msg, sizeof status_msg,
                 "Flash complete for %s (%" PRIu32 " pages verified, page 31 intact). Lift probes!", active_mac,
                 pages_done_count);
    }
    done_seq++;
    state = STATE_DONE;
    xSemaphoreGive(lock);
    ESP_LOGI(TAG, "%s", status_msg);
}

static bool sbl_dump_fcfg1(sbl_t *sbl, uint8_t *out_1k)
{
    uint8_t verify_buf[256];
    if (!sbl_read_memory(sbl, CJTAG_FCFG1_ADDR, out_1k, CJTAG_FCFG1_SIZE)) {
        return false;
    }
    for (size_t off = 0; off < CJTAG_FCFG1_SIZE; off += sizeof verify_buf) {
        if (!sbl_read_memory(sbl, CJTAG_FCFG1_ADDR + (uint32_t)off, verify_buf, sizeof verify_buf) ||
            memcmp(out_1k + off, verify_buf, sizeof verify_buf) != 0) {
            return false;
        }
    }
    return true;
}

// A lost contact during an SBL dump or flash (lock held): slipped, resumed hands-free when the same sensor is back
// (a dump from the page it stopped at, a flash from the start); at the 10th of a run, an error, so a fault that is not a
// slip does not retry forever.
static void sbl_slipped(const char *where, const char *why)
{
    slip_count++;
    if (slip_count >= 10) {
        state = STATE_ERROR;
        snprintf(status_msg, sizeof status_msg, "SBL %s failed %" PRIu32 " times at %s: %s",
                 mode == CJTAG_MODE_DUMP ? "dump" : "flash", slip_count, where, why);
        return;
    }
    state = STATE_SLIPPED;
    snprintf(status_msg, sizeof status_msg,
             "Contact lost at %s (%s): put the sensor back on the pads to resume (%" PRIu32 "/%" PRIu32 " done)", where,
             why, pages_done_count, total_pages_target);
}

static void run_sbl_session(const sblprobe_result_t *map)
{
    sbl_t sbl;
    sbl_io_t sio = {
        .write = sbl_uart_write,
        .read = sbl_uart_read,
        .flush_input = sbl_uart_flush,
        .ctx = NULL,
    };
    sbl_init(&sbl, &sio);
    sbl.chip_id = map->chip_id;

    xSemaphoreTake(lock, portMAX_DELAY);
    if ((state != STATE_ARMED && state != STATE_SLIPPED) || !ensure_page_bufs()) {
        if (state == STATE_ARMED || state == STATE_SLIPPED) {
            state = STATE_ERROR;
            snprintf(status_msg, sizeof status_msg, "out of memory allocating 128 KB page buffers");
        }
        xSemaphoreGive(lock);
        release_sbl_pins(true);
        return;
    }
    task_busy = true;
    xSemaphoreGive(lock);

    // Read FCFG1 USER_ID (0x50001294) and 64-bit IEEE MAC (0x500012F0) + Page 31 (CCFG, read twice and compared)
    uint8_t id_buf[12] = {0};
    uint8_t *p31 = page_bufs[CJTAG_CCFG_PAGE_IDX];
    if (!sbl_read_memory(&sbl, 0x50001294u, id_buf, 4) || !sbl_read_memory(&sbl, 0x500012F0u, id_buf + 4, 8) ||
        !sbl_dump_page(&sbl, CJTAG_CCFG_PAGE_IDX, p31)) {
        xSemaphoreTake(lock, portMAX_DELAY);
        task_busy = false;
        sbl_slipped("reading FCFG1/CCFG", sbl.error);
        xSemaphoreGive(lock);
        release_sbl_pins(true);
        return;
    }

    uint32_t user_id = le32(id_buf);
    uint32_t mac_lo = le32(id_buf + 4);
    uint32_t mac_hi = le32(id_buf + 8);
    uint64_t ieee_mac = ((uint64_t)mac_hi << 32) | mac_lo;

    const uint8_t *ccfg = p31 + (CJTAG_CCFG_ADDR - (CJTAG_CCFG_PAGE_IDX * CJTAG_PAGE));
    uint32_t bl_config = le32(ccfg + 0x30);
    const uint8_t *ccfg_mac = ccfg + 0x24;
    bool has_ccfg_mac = false;
    char found_mac[9] = {0};
    if (is_ascii_hex_mac(ccfg_mac)) {
        memcpy(found_mac, ccfg_mac, 8);
        found_mac[8] = '\0';
        has_ccfg_mac = true;
    } else {
        uint8_t p29_mac[8] = {0};
        if (sbl_read_memory(&sbl, 0x0001D000u, p29_mac, sizeof p29_mac) && is_ascii_hex_mac(p29_mac)) {
            memcpy(found_mac, p29_mac, 8);
            found_mac[8] = '\0';
        } else {
            snprintf(found_mac, sizeof found_mac, "%08X", (unsigned)mac_lo);
        }
    }

    xSemaphoreTake(lock, portMAX_DELAY);
    if (state != STATE_ARMED && state != STATE_SLIPPED) {
        task_busy = false;
        xSemaphoreGive(lock);
        release_sbl_pins(true);
        return;
    }

    // resuming after a slip: only the same sensor (hands-free: the operator put it back)
    if (state == STATE_SLIPPED && active_mac[0] != '\0' && strcmp(active_mac, found_mac) != 0) {
        task_busy = false;
        snprintf(status_msg, sizeof status_msg, "Another sensor (%s, not %s): put %s back on the pads, or Disarm",
                 found_mac, active_mac, active_mac);
        xSemaphoreGive(lock);
        release_sbl_pins(true);
        vTaskDelay(pdMS_TO_TICKS(500));
        return;
    }
    if (mode == CJTAG_MODE_FLASH) {  // a flash starts over after a slip (the bootloader writes it as one stream)
        pages_done_mask = 0;
        pages_done_count = 0;
    }
    active_transport = 2;
    memcpy(active_mac, found_mac, sizeof active_mac);
    active_user_id = user_id;
    active_bl_config = bl_config;
    active_ieee_mac = ieee_mac;

    if (mode == CJTAG_MODE_FLASH && !has_ccfg_mac) {
        task_busy = false;
        state = STATE_ERROR;
        snprintf(status_msg, sizeof status_msg, "Refused: sensor %s has no factory MAC in CCFG (page 31)", found_mac);
        xSemaphoreGive(lock);
        release_sbl_pins(true);
        return;
    }

    if (mode == CJTAG_MODE_FLASH && strcmp(backed_up_mac, found_mac) != 0) {
        task_busy = false;
        state = STATE_ERROR;
        snprintf(status_msg, sizeof status_msg,
                 "Refused: dump & download %s's stock flash first before flashing (CLAUDE.md backup rule)", found_mac);
        xSemaphoreGive(lock);
        release_sbl_pins(true);
        return;
    }

    state = STATE_RUNNING;
    snprintf(status_msg, sizeof status_msg,
             "Locked %s over SBL (R=pad %d/GPIO%d, T=pad %d/GPIO%d, BD=pad %d/GPIO%d, chip 0x%08" PRIX32 ") — %s…",
             active_mac, map->r_pad, cfg_sbl_pins[map->r_pad], map->t_pad, cfg_sbl_pins[map->t_pad], map->backdoor_pad,
             cfg_sbl_pins[map->backdoor_pad], map->chip_id, mode == CJTAG_MODE_DUMP ? "dumping" : "flashing");
    xSemaphoreGive(lock);
    ESP_LOGI(TAG, "%s", status_msg);

    bool ok = true;
    if (mode == CJTAG_MODE_DUMP) {
        if (!sbl_dump_fcfg1(&sbl, fcfg1_buf)) {
            ok = false;
        }
        for (uint32_t p = 0; ok && p < CJTAG_TOTAL_PAGES; p++) {
            if (state != STATE_RUNNING) {
                break;
            }
            if ((pages_done_mask & (1u << p)) != 0) {
                continue;
            }
            current_page = p;
            if (p != CJTAG_CCFG_PAGE_IDX && !sbl_dump_page(&sbl, p, page_bufs[p])) {
                ok = false;
                break;
            }
            xSemaphoreTake(lock, portMAX_DELAY);
            pages_done_mask |= (1u << p);
            pages_done_count = popcount32(pages_done_mask);
            snprintf(status_msg, sizeof status_msg, "Dumping %s (SBL): page %" PRIu32 " / 32 verified", active_mac,
                     pages_done_count);
            xSemaphoreGive(lock);
            ESP_LOGI(TAG, "%s", status_msg);
            vTaskDelay(1);
        }
    } else if (mode == CJTAG_MODE_FLASH) {
        if (!sbl_stream_begin(&sbl)) {
            ok = false;
        }
        for (uint32_t p = 0; ok && p < staged_pages; p++) {
            if (state != STATE_RUNNING) {
                ok = false;
                break;
            }
            current_page = p;
            if (!sbl_stream_page(&sbl, page_bufs[p], CJTAG_PAGE) || !sbl_verify_page(&sbl, p, page_bufs[p])) {
                ok = false;
                break;
            }
            xSemaphoreTake(lock, portMAX_DELAY);
            pages_done_mask |= (1u << p);
            pages_done_count = popcount32(pages_done_mask);
            snprintf(status_msg, sizeof status_msg, "Flashing %s (SBL): page %" PRIu32 " / %" PRIu32 " verified",
                     active_mac, pages_done_count, total_pages_target);
            xSemaphoreGive(lock);
            ESP_LOGI(TAG, "%s", status_msg);
            vTaskDelay(1);
        }
        if (ok && !sbl_stream_end(&sbl)) {
            ok = false;
        }
        if (ok && staged_wipe_nv) {
            current_page = CJTAG_NV_PAGE_IDX;
            if (!sbl_write_nv(&sbl, page_bufs[CJTAG_NV_PAGE_IDX]) ||
                !sbl_verify_page(&sbl, CJTAG_NV_PAGE_IDX, page_bufs[CJTAG_NV_PAGE_IDX])) {
                ok = false;
            } else {
                xSemaphoreTake(lock, portMAX_DELAY);
                pages_done_mask |= (1u << staged_pages);
                pages_done_count = popcount32(pages_done_mask);
                snprintf(status_msg, sizeof status_msg, "Flashing %s (SBL): page %" PRIu32 " / %" PRIu32 " verified",
                         active_mac, pages_done_count, total_pages_target);
                xSemaphoreGive(lock);
            }
        }
    }

    // Power-cycle cleanly into the sensor firmware
    release_sbl_pins(true);
    vTaskDelay(pdMS_TO_TICKS(50));
    sbl_power(NULL, true);

    xSemaphoreTake(lock, portMAX_DELAY);
    task_busy = false;
    if (state != STATE_RUNNING) {
        xSemaphoreGive(lock);
        return;
    }
    if (!ok) {
        char where[32];
        snprintf(where, sizeof where, "page %" PRIu32, current_page);
        sbl_slipped(where, sbl.error[0] ? sbl.error : "verification error");
        xSemaphoreGive(lock);
        ESP_LOGW(TAG, "%s", status_msg);
        return;
    }

    if (mode == CJTAG_MODE_DUMP) {
        dump_ready = true;
        fcfg1_ready = true;
        snprintf(status_msg, sizeof status_msg,
                 "SBL dump complete for %s (128 KB flash + 1 KB FCFG1 verified; R=pad %d, T=pad %d, BD=pad %d).",
                 active_mac, map->r_pad, map->t_pad, map->backdoor_pad);
    } else {
        staged_bytes = 0;
        staged_pages = 0;
        free_page_bufs();
        snprintf(status_msg, sizeof status_msg,
                 "SBL flash complete for %s (%" PRIu32 " pages verified, page 31 intact).", active_mac,
                 pages_done_count);
    }
    done_seq++;
    state = STATE_DONE;
    xSemaphoreGive(lock);
    ESP_LOGI(TAG, "%s", status_msg);
}

// Whether the running sensor drives (or pulls up) the pad under a probe: high against the ESP's pull-down at some
// point within a second (a UART's T idles high; a firmware may pull its R up).
static bool witness_alive(int gpio)
{
    gpio_num_t pin = (gpio_num_t)gpio;
    gpio_set_direction(pin, GPIO_MODE_INPUT);
    gpio_pullup_dis(pin);
    gpio_pulldown_en(pin);
    bool alive = false;
    for (int i = 0; i < 50 && !alive; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
        alive = gpio_get_level(pin) != 0;
    }
    gpio_pulldown_dis(pin);
    return alive;
}

// One power-up of the sensor (probe 2's pad held low across it, or released): is probe 1's pad alive?
static bool ws_run(bool hold)
{
    release_sbl_pins(true);
    vTaskDelay(pdMS_TO_TICKS(200));
    if (hold) {
        sbl_pin_low(NULL, 1);
    }
    sbl_power(NULL, true);
    bool alive = witness_alive(cfg_sbl_pins[0]);
    release_sbl_pins(true);  // released, then powered off
    return alive;
}

// W/S check (the user, 2026-11-07: after T/R; probe 1 on a known pad, T preferably, probe 2 on the pad to test): a
// power-up with probe 2 released, then one with its pad held low. Holding S (RESET_N) low keeps the chip in reset, so
// probe 1's pad stays dead; holding W changes nothing (as the backdoor it would start the bootloader, which drives T as
// well). Each twice, the same both times. Needs a firmware that drives (or pulls up) probe 1's pad while it runs.
static void ws_check(const sblprobe_io_t *io)
{
    if (!touch_gate(io)) {
        if (!need_lift) {
            note("Waiting: probe 1 on T (or R), probe 2 on the pad to test");
        }
        return;
    }
    bool free0 = ws_run(false), held0 = ws_run(true), free1 = ws_run(false), held1 = ws_run(true);
    xSemaphoreTake(lock, portMAX_DELAY);
    if (state == STATE_ARMED) {
        int p1 = cfg_sbl_pins[0], p2 = cfg_sbl_pins[1];
        if (free0 != free1 || held0 != held1) {
            snprintf(status_msg, sizeof status_msg,
                     "W/S: not the same twice (released: %s/%s, held: %s/%s): hold the probes still and try again",
                     free0 ? "alive" : "dead", free1 ? "alive" : "dead", held0 ? "alive" : "dead",
                     held1 ? "alive" : "dead");
        } else if (!free0) {
            snprintf(
                status_msg, sizeof status_msg,
                "W/S: can't tell: the running sensor does not drive the pad under GPIO%d. Put probe 1 on the other "
                "of T/R and try again.",
                p1);
        } else if (!held0) {
            snprintf(
                status_msg, sizeof status_msg,
                "W/S: the pad under GPIO%d is S (held low, it kept the sensor in reset). The T/R identification is "
                "kept.",
                p2);
        } else {
            snprintf(
                status_msg, sizeof status_msg,
                "W/S: the pad under GPIO%d is W (holding it low changed nothing; not S). The T/R identification is "
                "kept.",
                p2);
        }
        done_seq++;
        state = STATE_DONE;
    }
    xSemaphoreGive(lock);
    ESP_LOGI(TAG, "%s", status_msg);
}

static void identified_done(const char *how)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    if (state == STATE_ARMED) {
        identified = true;
        done_seq++;
        state = STATE_DONE;
        snprintf(status_msg, sizeof status_msg,
                 "Identified %s: %s. Lift it; then Dump or Flash, with a sensor on the same pads.", id_mac, how);
    }
    xSemaphoreGive(lock);
    ESP_LOGI(TAG, "%s", status_msg);
}

// Identify: find the pins with the chosen transport (cJTAG, SBL, or cJTAG then SBL), confirm them, remember them,
// and let the sensor go (released, unpowered). Reads nothing but what identifies it; never writes.
static void identify(cjtag_t *cj, esp_pin_ctx_t *pctx, const sblprobe_io_t *sbl_io)
{
    cjtag_transport_t tr = cfg_transport;
    if (tr == CJTAG_TRANSPORT_WS) {
        ws_check(sbl_io);
        return;
    }
    if (tr == CJTAG_TRANSPORT_CJTAG) {
        if (!sbl_powered) {
            sbl_power(NULL, true);
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (probe_with_polarity(cj, pctx, cfg_pin_a, cfg_pin_b, true)) {
            id_transport = 1;
            id_tck = pctx->tck_pin;
            id_tms = pctx->tms_pin;
            memcpy(id_mac, cj->mac, sizeof id_mac);
            memcpy(active_mac, cj->mac, sizeof active_mac);
            active_user_id = cj->user_id;
            active_bl_config = cj->bl_config;
            active_ieee_mac = cj->ieee_mac;
            active_transport = 1;
            active_tck = id_tck;
            active_tms = id_tms;
            cjtag_release_and_reset(cj);  // not left halted
            release_cjtag_pins(cfg_pin_a, cfg_pin_b);
            release_sbl_pins(true);
            char how[64];
            snprintf(how, sizeof how, "cJTAG, TCKC (C) GPIO%d, TMSC (M) GPIO%d", id_tck, id_tms);
            identified_done(how);
            return;
        }
    }
    if (tr == CJTAG_TRANSPORT_SBL2) {
        const int pads = 2;  // the two probes
        if (!touch_gate(sbl_io)) {
            if (!need_lift) {
                note("Waiting: hold the two probes on the sensor's TX and RX pads");
            }
            return;
        }
        sblprobe_result_t found = {0};
        if (sblprobe_find_n(sbl_io, pads, &found)) {
            char where[24];
            snprintf(where, sizeof where, "R=GPIO%d T=GPIO%d", cfg_sbl_pins[found.r_pad], cfg_sbl_pins[found.t_pad]);
            note("Found the bootloader on %s, backdoor GPIO%d: confirming", where, cfg_sbl_pins[found.backdoor_pad]);
            uint32_t cid = 0;
            char err[96] = "";
            release_sbl_pins(true);
            if (sblprobe_enter(sbl_io, &found, &cid, err, sizeof err) && cid == found.chip_id) {
                // who it is: the CCFG MAC (or the FCFG1 one), read through the bootloader just entered
                sbl_t sbl;
                sbl_io_t sio = {.write = sbl_uart_write, .read = sbl_uart_read, .flush_input = sbl_uart_flush};
                sbl_init(&sbl, &sio);
                uint8_t mac[8] = {0}, ieee[8] = {0};
                bool ccfg = sbl_read_memory(&sbl, CJTAG_CCFG_ADDR + 0x24u, mac, sizeof mac) && is_ascii_hex_mac(mac);
                sbl_read_memory(&sbl, 0x500012F0u, ieee, sizeof ieee);
                if (ccfg) {
                    memcpy(id_mac, mac, 8);
                } else {
                    snprintf(id_mac, sizeof id_mac, "%08" PRIX32, le32(ieee));
                }
                id_mac[8] = 0;
                memcpy(active_mac, id_mac, sizeof active_mac);
                active_ieee_mac = ((uint64_t)le32(ieee + 4) << 32) | le32(ieee);
                active_transport = 2;
                xSemaphoreTake(lock, portMAX_DELAY);
                saved_sbl_map = found;
                have_sbl_map = true;
                id_transport = 2;
                id_pads = pads;
                xSemaphoreGive(lock);
                release_sbl_pins(true);
                char how[96];
                snprintf(how, sizeof how, "SBL, %s, backdoor GPIO%d, chip 0x%08" PRIX32, where,
                         cfg_sbl_pins[found.backdoor_pad], found.chip_id);
                identified_done(how);
                return;
            }
            note("Not confirmed (the bootloader on %s answered once, then not the same): hold the probes still", where);
        } else {
            note("Nothing answered on these two pads (%s): lift the probes, then try again", found.error);
        }
        release_sbl_pins(true);
        need_lift = true;  // not again while they stay on
        touch_streak = 0;
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(150));
}

static void task(void *arg)
{
    (void)arg;
    static cjtag_t cj;
    esp_pin_ctx_t pctx = {0};
    sblprobe_io_t sbl_io = {
        .pin_low = sbl_pin_low,
        .pin_release = sbl_pin_release,
        .pin_read = sbl_pin_read,
        .power = sbl_power,
        .uart_open = sbl_uart_open,
        .uart_close = sbl_uart_close,
        .uart_write = sbl_uart_write,
        .uart_read = sbl_uart_read,
        .uart_flush = sbl_uart_flush,
        .sleep_ms = sbl_sleep_ms,
        .touching = sbl_pin_touching,
        .ctx = NULL,
    };

    for (;;) {
        if (state != STATE_ARMED && state != STATE_SLIPPED) {
            vTaskDelay(pdMS_TO_TICKS(150));
            continue;
        }

        if (mode == CJTAG_MODE_IDENTIFY) {
            identify(&cj, &pctx, &sbl_io);
            continue;
        }
        // Dump or Flash: only the identified transport and pins, no search (the operator put the sensor back)
        if (id_transport == 1) {
            if (!sbl_powered) {
                sbl_power(NULL, true);
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            if (probe_with_polarity(&cj, &pctx, id_tck, id_tms, false)) {
                run_cjtag_session(&cj, &pctx, id_tck, id_tms);
                continue;
            }
        } else if (id_transport == 2) {
            if (id_pads == 2 && !touch_gate(&sbl_io)) {
                if (!need_lift) {
                    note("Waiting: hold the two probes on the identified TX and RX pads");
                }
                continue;
            }
            uint32_t cid = 0;
            char err[96] = "";
            if (sblprobe_enter(&sbl_io, &saved_sbl_map, &cid, err, sizeof err) && cid == saved_sbl_map.chip_id) {
                run_sbl_session(&saved_sbl_map);
                continue;
            }
            release_sbl_pins(true);
            if (id_pads == 2) {
                note("Nothing answered on the identified pads (%s): lift the probes, then try again", err);
                need_lift = true;
                touch_streak = 0;
                continue;
            }
            note("Waiting for the sensor on the identified pads (%s)", err);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(150));
    }
}

void cjtag_task_start(void)
{
    if (!lock) {
        lock = xSemaphoreCreateMutex();
    }
    xTaskCreate(task, "cjtag", 8192, NULL, 6, NULL);
}

bool cjtag_task_stage_begin(bool wipe_nv, char *err, size_t err_cap)
{
    if (!lock) {
        lock = xSemaphoreCreateMutex();
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    if (state == STATE_RUNNING || task_busy) {
        snprintf(err, err_cap, "station operation in progress");
        xSemaphoreGive(lock);
        return false;
    }
    if (!identified) {
        snprintf(err, err_cap, "identify the sensor's pins first (Identify)");
        xSemaphoreGive(lock);
        return false;
    }
    if (!ensure_page_bufs()) {
        snprintf(err, err_cap, "out of memory allocating 128 KB page buffers");
        xSemaphoreGive(lock);
        return false;
    }
    state = STATE_IDLE;
    mode = CJTAG_MODE_NONE;
    dump_ready = false;
    fcfg1_ready = false;
    staged_bytes = 0;
    staged_pages = 0;
    staged_wipe_nv = wipe_nv;
    for (uint32_t i = 0; i < CJTAG_TOTAL_PAGES; i++) {
        memset(page_bufs[i], 0xFF, CJTAG_PAGE);
    }
    xSemaphoreGive(lock);
    return true;
}

bool cjtag_task_stage_feed(const uint8_t *data, size_t len)
{
    if (staged_bytes + len > CJTAG_APP_END) {
        return false;
    }
    size_t rem = len;
    const uint8_t *p = data;
    while (rem > 0) {
        uint32_t pg = (uint32_t)(staged_bytes / CJTAG_PAGE);
        size_t off = staged_bytes % CJTAG_PAGE;
        size_t chunk = (rem < CJTAG_PAGE - off) ? rem : (CJTAG_PAGE - off);
        if (!page_bufs[pg]) {
            return false;
        }
        memcpy(page_bufs[pg] + off, p, chunk);
        staged_bytes += chunk;
        p += chunk;
        rem -= chunk;
    }
    return true;
}

bool cjtag_task_stage_end(bool rx_ok, cjtag_transport_t transport, char *msg, size_t msg_cap)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    if (!rx_ok || staged_bytes == 0 || (staged_bytes % 4u) != 0 || !page_bufs[0]) {
        staged_bytes = 0;
        staged_pages = 0;
        free_page_bufs();
        snprintf(msg, msg_cap, "invalid CC1310 image (must be 1..122880 bytes, multiple of 4)");
        xSemaphoreGive(lock);
        return false;
    }
    // Verify Cortex-M3 vector table at start of page 0
    const uint8_t *p0 = page_bufs[0];
    uint32_t sp = le32(p0);
    uint32_t reset = le32(p0 + 4);
    if (!(sp > 0x20000000u && sp <= 0x20005000u && (reset & 1u) != 0 && reset < CJTAG_APP_END)) {
        staged_bytes = 0;
        staged_pages = 0;
        free_page_bufs();
        snprintf(msg, msg_cap, "refused: not a CC1310 image (invalid vector table in page 0)");
        xSemaphoreGive(lock);
        return false;
    }

    staged_pages = (uint32_t)((staged_bytes + CJTAG_PAGE - 1u) / CJTAG_PAGE);
    total_pages_target = staged_pages + (staged_wipe_nv ? 1u : 0u);
    pages_done_mask = 0;
    pages_done_count = 0;
    current_page = 0;
    slip_count = 0;
    active_mac[0] = '\0';
    active_transport = 0;
    (void)transport;  // the identification's
    mode = CJTAG_MODE_FLASH;
    state = STATE_ARMED;
    const char *tr_label = id_transport != 2 ? "cJTAG, identified pins"
                           : id_pads == 2    ? "SBL, two hand probes on the identified TX/RX"
                                             : "SBL, identified pads";
    snprintf(status_msg, sizeof status_msg, "Armed Station Flash [%s] (%u B, %" PRIu32 " pages%s) — connect sensor!",
             tr_label, (unsigned)staged_bytes, staged_pages, staged_wipe_nv ? " + NV wipe" : "");
    snprintf(msg, msg_cap, "%s", status_msg);
    xSemaphoreGive(lock);
    return true;
}

bool cjtag_task_arm(cjtag_mode_t new_mode, cjtag_transport_t transport, char *msg, size_t msg_cap)
{
    if (!lock) {
        lock = xSemaphoreCreateMutex();
    }
    xSemaphoreTake(lock, portMAX_DELAY);

    if (new_mode == CJTAG_MODE_NONE) {
        state = STATE_IDLE;
        mode = CJTAG_MODE_NONE;
        active_transport = 0;
        xSemaphoreGive(lock);
        while (task_busy) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        xSemaphoreTake(lock, portMAX_DELAY);
        release_cjtag_pins(cfg_pin_a, cfg_pin_b);
        release_sbl_pins(true);
        dump_ready = false;
        fcfg1_ready = false;
        staged_bytes = 0;
        staged_pages = 0;
        free_page_bufs();
        snprintf(status_msg, sizeof status_msg, "Disarmed.");
        snprintf(msg, msg_cap, "%s", status_msg);
        xSemaphoreGive(lock);
        return true;
    }

    touch_streak = 0;  // two hand-held probes: a fresh start
    need_lift = false;
    if (new_mode == CJTAG_MODE_IDENTIFY && transport != CJTAG_TRANSPORT_WS) {  // a new one replaces the last one
        identified = false;
        id_transport = 0;
        have_sbl_map = false;
        id_mac[0] = '\0';
        active_mac[0] = '\0';
        active_tck = -1;
        active_tms = -1;
        active_transport = 0;
    }
    if (new_mode == CJTAG_MODE_IDENTIFY) {  // the W/S check keeps the T/R identification
        cfg_transport = transport;
        mode = new_mode;
        const char *label = (transport == CJTAG_TRANSPORT_SBL2) ? "SBL T/R, two probes"
                            : (transport == CJTAG_TRANSPORT_WS) ? "W/S check (experimental), two probes"
                                                                : "cJTAG M/C, two probes";
        snprintf(status_msg, sizeof status_msg, "Identify armed [%s]: connect the sensor", label);
        state = STATE_ARMED;
        snprintf(msg, msg_cap, "%s", status_msg);
        xSemaphoreGive(lock);
        return true;
    }
    if (!identified) {
        snprintf(msg, msg_cap, "identify the sensor's pins first (Identify)");
        xSemaphoreGive(lock);
        return false;
    }

    if (!ensure_page_bufs()) {
        snprintf(msg, msg_cap, "out of memory allocating 128 KB page buffers");
        xSemaphoreGive(lock);
        return false;
    }

    if (new_mode == CJTAG_MODE_FLASH && staged_pages == 0) {
        snprintf(msg, msg_cap, "upload/stage a CC1310 .bin image first");
        xSemaphoreGive(lock);
        return false;
    }

    pages_done_mask = 0;
    pages_done_count = 0;
    current_page = 0;
    slip_count = 0;
    active_mac[0] = '\0';
    active_tck = -1;
    active_tms = -1;
    active_transport = 0;
    (void)transport;  // Dump and Flash use the identification's transport and pins
    mode = new_mode;

    const char *tr_label = id_transport != 2 ? "cJTAG, identified pins"
                           : id_pads == 2    ? "SBL, two hand probes on the identified TX/RX"
                                             : "SBL, identified pads";
    if (new_mode == CJTAG_MODE_DUMP) {
        dump_ready = false;
        fcfg1_ready = false;
        staged_bytes = 0;
        staged_pages = 0;
        total_pages_target = CJTAG_TOTAL_PAGES;
        snprintf(status_msg, sizeof status_msg, "Armed Station Dump [%s] — connect sensor!", tr_label);
    } else {
        total_pages_target = staged_pages + (staged_wipe_nv ? 1u : 0u);
        snprintf(status_msg, sizeof status_msg, "Armed Station Flash [%s] (%" PRIu32 " pages) — connect sensor!",
                 tr_label, total_pages_target);
    }
    state = STATE_ARMED;
    snprintf(msg, msg_cap, "%s", status_msg);
    xSemaphoreGive(lock);
    return true;
}

bool cjtag_task_get_dump_page(uint32_t page_idx, uint8_t *out_4k)
{
    if (page_idx >= CJTAG_TOTAL_PAGES || !lock) {
        return false;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    bool ok = dump_ready && page_bufs[page_idx] != NULL;
    if (ok) {
        memcpy(out_4k, page_bufs[page_idx], CJTAG_PAGE);
    }
    xSemaphoreGive(lock);
    return ok;
}

bool cjtag_task_get_fcfg1(uint8_t *out_1k)
{
    if (!lock) {
        return false;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    bool ok = fcfg1_ready;
    if (ok) {
        memcpy(out_1k, fcfg1_buf, CJTAG_FCFG1_SIZE);
    }
    xSemaphoreGive(lock);
    return ok;
}

void cjtag_task_note_dump_downloaded(void)
{
    if (!lock) {
        return;
    }
    // The dump stays (until the next Dump, a staged image, or Disarm): a download sent is not a file saved (2026-11-07:
    // Chrome held the 128 KB file as an unconfirmed download, and the dump was already gone), so it can be saved again
    xSemaphoreTake(lock, portMAX_DELAY);
    if (dump_ready && active_mac[0] != '\0') {
        memcpy(backed_up_mac, active_mac, sizeof backed_up_mac);
    }
    xSemaphoreGive(lock);
}

const char *cjtag_task_mac(void)
{
    return active_mac[0] ? active_mac : "UNKNOWN";
}

size_t cjtag_task_json(char *out, size_t cap)
{
    static const char *state_names[] = {"idle", "armed", "running", "slipped", "done", "error"};
    static const char *mode_names[] = {"none", "dump", "flash", "identify"};
    static const char *tr_names[] = {"cjtag", "sbl", "auto", "sbl2", "ws"};
    static const char *act_tr_names[] = {"none", "cjtag", "sbl"};

    if (!lock) {
        lock = xSemaphoreCreateMutex();
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    char mac_js[24], msg_js[480];
    json_str(mac_js, sizeof mac_js, active_mac);
    json_str(msg_js, sizeof msg_js, status_msg);
    int r_gpio = (have_sbl_map && saved_sbl_map.r_pad >= 0) ? cfg_sbl_pins[saved_sbl_map.r_pad] : -1;
    int t_gpio = (have_sbl_map && saved_sbl_map.t_pad >= 0) ? cfg_sbl_pins[saved_sbl_map.t_pad] : -1;
    int bd_gpio = (have_sbl_map && saved_sbl_map.backdoor_pad >= 0) ? cfg_sbl_pins[saved_sbl_map.backdoor_pad] : -1;
    int n = snprintf(out, cap,
                     "{\"state\":\"%s\",\"mode\":\"%s\",\"transport\":\"%s\",\"active_transport\":\"%s\","
                     "\"pin_a\":%d,\"pin_b\":%d,\"tck_pin\":%d,\"tms_pin\":%d,"
                     "\"sbl_pins\":[%d,%d,%d,%d],\"power_pin\":%d,"
                     "\"sbl_map\":{\"found\":%s,\"r_pad\":%d,\"t_pad\":%d,\"backdoor_pad\":%d,"
                     "\"r_gpio\":%d,\"t_gpio\":%d,\"backdoor_gpio\":%d,\"chip_id\":\"0x%08" PRIX32 "\"},"
                     "\"mac\":%s,\"user_id\":\"0x%08" PRIX32 "\",\"bl_config\":\"0x%08" PRIX32 "\","
                     "\"ieee_mac\":\"%016" PRIX64 "\",\"pages_done\":%" PRIu32 ",\"total_pages\":%" PRIu32 ","
                     "\"current_page\":%" PRIu32 ",\"slips\":%" PRIu32 ",\"done_seq\":%" PRIu32 ",\"dump_ready\":%s,"
                     "\"fcfg1_ready\":%s,\"staged_bytes\":%u,\"identified\":%s,\"id_transport\":\"%s\","
                     "\"id_tck\":%d,\"id_tms\":%d,\"msg\":%s}",
                     state_names[state], mode_names[mode], tr_names[cfg_transport], act_tr_names[active_transport],
                     cfg_pin_a, cfg_pin_b, active_tck, active_tms, cfg_sbl_pins[0], cfg_sbl_pins[1], cfg_sbl_pins[2],
                     cfg_sbl_pins[3], cfg_power_pin, have_sbl_map ? "true" : "false",
                     have_sbl_map ? saved_sbl_map.r_pad : -1, have_sbl_map ? saved_sbl_map.t_pad : -1,
                     have_sbl_map ? saved_sbl_map.backdoor_pad : -1, r_gpio, t_gpio, bd_gpio,
                     have_sbl_map ? saved_sbl_map.chip_id : 0u, mac_js, active_user_id, active_bl_config,
                     active_ieee_mac, pages_done_count, total_pages_target, current_page, slip_count, done_seq,
                     dump_ready ? "true" : "false", fcfg1_ready ? "true" : "false", (unsigned)staged_bytes,
                     identified ? "true" : "false", act_tr_names[id_transport], id_tck, id_tms, msg_js);
    xSemaphoreGive(lock);
    return n < 0 ? 0 : ((size_t)n < cap ? (size_t)n : cap - 1);
}

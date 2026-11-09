#include "contact.h"

#include "cjtag.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "driver/uart.h"
#include "esp_private/gpio.h"  // gpio_od_enable
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "settings.h"

#define UART   UART_NUM_1
#define OFF_MS 300  // a power cycle's off time: the board's capacitors drain, the chip's power-on reset fires

static SemaphoreHandle_t uart_lock;
static volatile uint32_t s_uart_tx, s_uart_rx;

static void pin_init(int gpio)
{
    if (gpio < 0) {
        return;
    }
    gpio_config_t c = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_OUTPUT_OD,  // the contact pulls its RESET_N pin up internally
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_set_level(gpio, 1);
    gpio_config(&c);
}

// The contact's + pad on a GPIO (#81): a reset is a power cycle. Not with the battery rig, which owns the supply.
static bool power_ctl(void)
{
    return OWS_PIN_CONTACT_POWER >= 0 && !settings.rig;
}

static void line_free(int gpio)  // high impedance, no pull: nothing driven into the contact
{
    if (gpio >= 0) {
        gpio_reset_pin(gpio);
        gpio_set_direction(gpio, GPIO_MODE_DISABLE);
        gpio_pullup_dis(gpio);
        gpio_pulldown_dis(gpio);
    }
}

void contact_init(void)
{
    uart_lock = xSemaphoreCreateMutex();
    if (OWS_PIN_CONTACT_RESET >= 0) {
        pin_init(OWS_PIN_CONTACT_RESET);
    }
    if (OWS_PIN_CONTACT_BOOT >= 0) {
        pin_init(OWS_PIN_CONTACT_BOOT);
    }
    line_free(OWS_PIN_CONTACT_TMS);  // the debug port (M, C) only when gwsync uses it
    line_free(OWS_PIN_CONTACT_TCK);
    if (power_ctl()) {  // supplied from the start: the UART below sets R high (idle) before anything looks at it
        gpio_reset_pin(OWS_PIN_CONTACT_POWER);
        gpio_set_direction(OWS_PIN_CONTACT_POWER, GPIO_MODE_OUTPUT);
        gpio_set_drive_capability(OWS_PIN_CONTACT_POWER, GPIO_DRIVE_CAP_3);
        gpio_set_level(OWS_PIN_CONTACT_POWER, 1);
    }
    uart_config_t u = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(UART, 4096, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART, &u));
    ESP_ERROR_CHECK(uart_set_pin(UART, OWS_PIN_CONTACT_TX, OWS_PIN_CONTACT_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    // the line idles high: a contact that does not drive its T pad (a half-written image, a chip that is starting)
    // leaves it floating, and noise came in as bytes (00 ff 00 fe ..., 2026-10-25)
    gpio_pullup_en(OWS_PIN_CONTACT_RX);
    if (settings.rig) {
        // the battery rig (#95, docs/design/low-battery.md §3.1): the contact's supply sits below 3.3 V and is measured, so
        // nothing may feed it through its pins: no pull-up on the line from T, and the line to R open-drain (the
        // contact pulls its R pin up itself, to its own supply)
        gpio_pullup_dis(OWS_PIN_CONTACT_RX);
        // the pad's open-drain bit only: gpio_set_direction() would detach the UART and leave a plain output, low
        // (2026-11-01: R held low, the console dead and the contact's pull-up and UART drawing all the time)
        gpio_od_enable(OWS_PIN_CONTACT_TX);
    }
}

// A restart of the contact, into its firmware or (bootloader) into its ROM bootloader (#81): a power cycle when its
// + pad is on a GPIO (a cold reset: it also ends a brown-out lock-up, TI errata Advisory 12), else S. Into the
// bootloader, R (DIO1: the stock backdoor, BL_CONFIG 0xC5FE01C5, as the dongle's CH554 does it) is held low across
// the start. While the power is off nothing may feed the chip through its pins: R low, the line from T unpulled, S
// released (open-drain).
static void restart(bool bootloader)
{
    const int tx = OWS_PIN_CONTACT_TX, rx = OWS_PIN_CONTACT_RX;
    const int s = OWS_PIN_CONTACT_RESET, w = OWS_PIN_CONTACT_BOOT;
    bool cycle = power_ctl();
    if (!cycle && s < 0) {
        ESP_LOGW("contact", "no way to reset it: neither a power GPIO nor the S pad");
        return;
    }
    // R as a plain output (the UART detached): low; open-drain on the rig, whose supply nothing may feed
    gpio_reset_pin(tx);
    gpio_pullup_dis(tx);
    gpio_set_direction(tx, settings.rig ? GPIO_MODE_OUTPUT_OD : GPIO_MODE_OUTPUT);
    gpio_set_level(tx, 0);
    if (bootloader && w >= 0) {
        gpio_set_level(w, 0);
    }
    if (s >= 0) {
        gpio_set_level(s, 0);  // held in reset
    }
    if (cycle) {
        gpio_pullup_dis(rx);
        gpio_set_level(OWS_PIN_CONTACT_POWER, 0);
        vTaskDelay(pdMS_TO_TICKS(OFF_MS));
        gpio_set_level(OWS_PIN_CONTACT_POWER, 1);
        if (!bootloader) {
            // R high right after the power, not before (it would feed the unpowered chip, which then missed its
            // power-on reset without S: bench, #81), and long before the ROM looks at its backdoor pin
            gpio_set_level(tx, 1);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    } else {
        if (!bootloader) {
            gpio_set_level(tx, 1);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (s >= 0) {
        gpio_set_level(s, 1);  // it starts here (or at the power-up, S released then)
    }
    if (bootloader) {
        vTaskDelay(pdMS_TO_TICKS(50));  // the ROM has looked at its backdoor pin by now
        gpio_set_level(tx, 1);
        if (w >= 0) {
            gpio_set_level(w, 1);
        }
    }
    // R back to the UART (idles high), the line from T pulled up again (not on the rig: contact_init)
    uart_set_pin(UART, tx, rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (settings.rig) {
        gpio_od_enable(tx);
    } else {
        gpio_pullup_en(rx);
    }
}

void contact_reset(void)
{
    restart(false);
}

void contact_enter_bootloader(void)
{
    restart(true);
}

bool contact_uart_take(uint32_t wait_ms)
{
    return xSemaphoreTake(uart_lock, pdMS_TO_TICKS(wait_ms)) == pdTRUE;
}

void contact_uart_give(void)
{
    xSemaphoreGive(uart_lock);
}

// up to cap bytes: waits up to timeout_ms for the first, then takes what is buffered. Not uart_read_bytes(cap) alone:
// it waits for all cap bytes or the whole timeout, so every short bootloader reply cost its full timeout (a page took
// 49 s on the LOLIN32, 2026-10-25)
size_t contact_uart_read(uint8_t *buf, size_t cap, uint32_t timeout_ms)
{
    if (!cap || uart_read_bytes(UART, buf, 1, pdMS_TO_TICKS(timeout_ms)) != 1) {
        return 0;
    }
    size_t more = 0;
    uart_get_buffered_data_len(UART, &more);
    more = more < cap - 1 ? more : cap - 1;
    int k = more ? uart_read_bytes(UART, buf + 1, more, 0) : 0;
    size_t n = 1 + (k > 0 ? (size_t)k : 0);
    s_uart_rx += (uint32_t)n;
    return n;
}

bool contact_uart_write(const uint8_t *data, size_t len)
{
    int n = uart_write_bytes(UART, data, len);
    if (n > 0) {
        s_uart_tx += (uint32_t)n;
    }
    return n == (int)len && uart_wait_tx_done(UART, pdMS_TO_TICKS(1000)) == ESP_OK;
}

void contact_uart_flush_input(void)
{
    uart_flush_input(UART);
}

void contact_uart_counts(uint32_t *tx, uint32_t *rx)
{
    *tx = s_uart_tx;
    *rx = s_uart_rx;
}

// --- reflashing through the ROM bootloader ---

#include <stdio.h>
#include <string.h>

#include "sbl.h"

static bool sbl_write(void *ctx, const uint8_t *d, size_t n)
{
    (void)ctx;
    return contact_uart_write(d, n);
}

static size_t sbl_read(void *ctx, uint8_t *buf, size_t cap, uint32_t timeout_ms)
{
    (void)ctx;
    size_t n = contact_uart_read(buf, cap, timeout_ms);
    while (n && n < cap) {  // what follows, until the line is quiet for 20 ms
        size_t k = contact_uart_read(buf + n, cap - n, 20);
        if (!k) {
            break;
        }
        n += k;
    }
    return n;
}

static void sbl_flush(void *ctx)
{
    (void)ctx;
    contact_uart_flush_input();
}

static sbl_t s_sbl;  // shared across contact_gwsync() and contact_flash_*() under uart_lock
static uint8_t s_page[SBL_PAGE];
static size_t s_fill;
static bool s_wipe_nv;

bool contact_flash_begin(const char *expect_mac, bool wipe_nv, char *msg, size_t cap)
{
    sbl_io_t io = {sbl_write, sbl_read, sbl_flush, NULL};
    if (!contact_uart_take(5000)) {
        snprintf(msg, cap, "the UART is busy");
        return false;
    }
    s_fill = 0;
    s_wipe_nv = wipe_nv;
    sbl_init(&s_sbl, &io);
    bool ok = sbl_connect(&s_sbl, expect_mac);
    if (!ok) {
        sbl_init(&s_sbl, &io);
        contact_enter_bootloader();
        ok = sbl_connect(&s_sbl, expect_mac);
    }
    if (!ok || !sbl_stream_begin(&s_sbl)) {
        snprintf(msg, cap, "not flashed: %s", s_sbl.error);
        contact_reset();
        contact_uart_flush_input();
        contact_uart_give();
        return false;
    }
    return true;
}

bool contact_flash_write(const uint8_t *data, size_t len)
{
    for (size_t at = 0; at < len;) {
        size_t n = len - at < SBL_PAGE - s_fill ? len - at : SBL_PAGE - s_fill;
        memcpy(s_page + s_fill, data + at, n);
        s_fill += n;
        at += n;
        if (s_fill == SBL_PAGE) {
            if (!sbl_stream_page(&s_sbl, s_page, SBL_PAGE)) {
                s_fill = 0;
                return false;
            }
            s_fill = 0;
        }
    }
    return true;
}

bool contact_flash_end(bool rx_ok, char *msg, size_t cap)
{
    bool ok = rx_ok;
    if (ok && s_fill > 0) {
        ok = sbl_stream_page(&s_sbl, s_page, s_fill);
        s_fill = 0;
    }
    if (ok) {
        ok = sbl_stream_end(&s_sbl);
    }
    if (ok && s_wipe_nv) {
        memset(s_page, 0xFF, sizeof s_page);
        ok = sbl_write_nv(&s_sbl, s_page);
    }
    if (ok) {
        snprintf(msg, cap, "contact %s (chip %08lx, backdoor DIO%u): %lu bytes written and verified%s; reset",
                 s_sbl.mac, (unsigned long)s_sbl.chip_id, s_sbl.backdoor_dio, (unsigned long)s_sbl.stream_at,
                 s_wipe_nv ? ", NV wiped" : "");
    } else if (!rx_ok && !s_sbl.error[0]) {
        snprintf(msg, cap, "not flashed: the upload broke off or was not a valid image");
    } else {
        snprintf(msg, cap, "not flashed: %s", s_sbl.error);
    }
    contact_reset();  // into its firmware, new or old (the bootloader's own reset would hang it)
    contact_uart_flush_input();
    contact_uart_give();
    return ok;
}

bool contact_flash(const uint8_t *image, size_t len, const char *expect_mac, bool wipe_nv, char *msg, size_t cap)
{
    if (!contact_flash_begin(expect_mac, wipe_nv, msg, cap)) {
        return false;
    }
    bool ok = contact_flash_write(image, len);
    return contact_flash_end(ok, msg, cap);
}

// --- at the start: the contact on the gateway image this firmware carries (esp32/contact/gwsync.c, #76) ---

#include "esp_log.h"
#include "esp_timer.h"
#include "gwsync.h"
#include "settings.h"
#include "pins.h"

extern const uint8_t gw_image_start[] asm("_binary_gateway_contact_v1_app_bin_start");
extern const uint8_t gw_image_end[] asm("_binary_gateway_contact_v1_app_bin_end");
extern const uint8_t gw_stock_start[] asm("_binary_gateway_stock_v1_8_app_bin_start");
extern const uint8_t gw_stock_end[] asm("_binary_gateway_stock_v1_8_app_bin_end");

bool contact_stock_gw_available(void)
{
    return (size_t)(gw_stock_end - gw_stock_start) > GWSYNC_PAGE31_CODE_LEN;
}

static const uint8_t *active_gw_image(uint32_t *len_out)
{
    if (settings.stock_gw && contact_stock_gw_available()) {
        *len_out = (uint32_t)(gw_stock_end - gw_stock_start);
        return gw_stock_start;
    }
    *len_out = (uint32_t)(gw_image_end - gw_image_start);
    return gw_image_start;
}

static gwsync_report_t gw_report;
static bool gw_checked;
static char gw_steps[6][120];  // the last steps, for the status page while the check runs and after
static unsigned gw_nsteps;

static void gw_progress(void *ctx, const char *step)
{
    (void)ctx;
    snprintf(gw_steps[gw_nsteps % 6], sizeof gw_steps[0], "%lu ms %s", (unsigned long)(esp_timer_get_time() / 1000),
             step);
    gw_nsteps++;
    ESP_LOGI("contact", "gateway check: %s", step);
}

size_t contact_gwsync_steps(char *out, size_t cap)  // the last steps, oldest first, one per line
{
    size_t n = 0;
    out[0] = 0;
    for (unsigned i = gw_nsteps > 6 ? gw_nsteps - 6 : 0; i < gw_nsteps && n + 1 < cap; i++) {
        n += (size_t)snprintf(out + n, cap - n, "%s\n", gw_steps[i % 6]);
    }
    return n < cap ? n : cap - 1;
}

static void gw_reset(void *ctx)
{
    (void)ctx;
    contact_reset();
}

static void gw_bootloader(void *ctx)
{
    (void)ctx;
    contact_enter_bootloader();
}

static void gw_sleep(void *ctx, uint32_t ms)
{
    (void)ctx;
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static bool gw_image(void *ctx, uint32_t off, uint8_t *buf, size_t len)
{
    (void)ctx;
    uint32_t img_len = 0;
    const uint8_t *img = active_gw_image(&img_len);
    if (off + len > img_len) {
        return false;
    }
    memcpy(buf, img + off, len);
    return true;
}

// --- the contact's debug port (cJTAG on M and C, #81) for gwsync ---

typedef struct {
    int tck, tms;
} cj_pins_t;

static void cj_set_pins(void *ctx, bool tck, bool tms)  // TMSC changes while TCKC is low (cjtag_task.c's order)
{
    const cj_pins_t *p = ctx;
    if (!tck) {
        gpio_set_level(p->tck, 0);
        gpio_set_level(p->tms, tms);
    } else {
        gpio_set_level(p->tms, tms);
        gpio_set_level(p->tck, 1);
    }
}

static void cj_set_tms_dir(void *ctx, bool output)
{
    const cj_pins_t *p = ctx;
    gpio_set_direction(p->tms, output ? GPIO_MODE_INPUT_OUTPUT : GPIO_MODE_INPUT);
}

static bool cj_get_tms(void *ctx)
{
    const cj_pins_t *p = ctx;
    return gpio_get_level(p->tms) != 0;
}

static void cj_delay(void *ctx, uint32_t us)
{
    (void)ctx;
    if (us > 1) {
        esp_rom_delay_us(us);
    }
}

// The carried image's pages 0-29 through the debug port: pages already equal skipped, the CCFG's factory MAC checked
// first (cjtag_flash_page refuses pages 30 and 31 and verifies each page), then a restart into the new firmware.
// Returns the pages written, or -1 (err says why: not wired, not answering, no MAC, a write failed).
static int cjtag_flash_now(uint32_t image_len, char *mac, char *err, size_t cap)
{
    static cjtag_t cj;
    static uint8_t want[CJTAG_PAGE];
    const int tck = OWS_PIN_CONTACT_TCK, tms = OWS_PIN_CONTACT_TMS;
    if (tck < 0 || tms < 0) {
        snprintf(err, cap, "no debug port wired (M, C)");
        return -1;
    }
    cj_pins_t pins = {tck, tms};
    gpio_reset_pin(tck);
    gpio_set_direction(tck, GPIO_MODE_OUTPUT);
    gpio_set_level(tck, 1);
    gpio_reset_pin(tms);
    gpio_set_pull_mode(tms, GPIO_PULLUP_ONLY);
    gpio_set_direction(tms, GPIO_MODE_INPUT_OUTPUT);
    gpio_set_level(tms, 1);
    cjtag_io_t io = {cj_set_pins, cj_set_tms_dir, cj_get_tms, cj_delay, &pins};
    bool connected = false;
    for (int i = 0; i < 3 && !connected; i++) {
        cjtag_init(&cj, &io);
        connected = cjtag_connect(&cj, true);
        if (!connected) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    int written = -1;
    if (!connected) {
        snprintf(err, cap, "%s", cj.error);
        goto out;
    }
    memcpy(mac, cj.mac, 9);
    written = cjtag_write_image(&cj, gw_image, NULL, image_len, want);
    if (written < 0) {
        snprintf(err, cap, "%s", cj.error);
    } else if (written > 0) {
        ESP_LOGI("contact", "debug port: pages written: %s", cj.written_pages);
        snprintf(err, cap, "pages %s", cj.written_pages);  // the caller logs it (gwsync step)
    }
    cjtag_release_and_reset(&cj);
out:
    line_free(tck);
    line_free(tms);
    if (written > 0) {
        contact_reset();  // a clean start of the new firmware (VECTRESET restarts the CPU only)
    }
    return written;
}

// Bring-up: one page (0-30; page 31 never: cjtag_flash_page refuses it) through the debug port, the CCFG's factory MAC
// checked first and the page verified; the contact is left halted-and-released (VECTRESET), not restarted.
int contact_cjtag_page(uint32_t page, const uint8_t *data, char *mac, char *err, size_t cap)
{
    static cjtag_t cj;
    const int tck = OWS_PIN_CONTACT_TCK, tms = OWS_PIN_CONTACT_TMS;
    if (tck < 0 || tms < 0) {
        snprintf(err, cap, "no debug port wired (M, C)");
        return -1;
    }
    cj_pins_t pins = {tck, tms};
    gpio_reset_pin(tck);
    gpio_set_direction(tck, GPIO_MODE_OUTPUT);
    gpio_set_level(tck, 1);
    gpio_reset_pin(tms);
    gpio_set_pull_mode(tms, GPIO_PULLUP_ONLY);
    gpio_set_direction(tms, GPIO_MODE_INPUT_OUTPUT);
    gpio_set_level(tms, 1);
    cjtag_io_t io = {cj_set_pins, cj_set_tms_dir, cj_get_tms, cj_delay, &pins};
    bool connected = false;
    for (int i = 0; i < 3 && !connected; i++) {
        cjtag_init(&cj, &io);
        connected = cjtag_connect(&cj, true);
        if (!connected) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    int r = -1;
    if (connected) {
        memcpy(mac, cj.mac, 9);
        r = cjtag_flash_page(&cj, page, data, true) ? 1 : -1;
        if (r < 0) {
            snprintf(err, cap, "%s", cj.error);
        }
        cjtag_release_and_reset(&cj);
    } else {
        snprintf(err, cap, "%s", cj.error);
    }
    line_free(tck);
    line_free(tms);
    return r;
}

// The VDDR boost through the debug port (cjtag_set_vddr_boost; on: 1 on, 0 off, -1 only read): want_mac must be the
// contact's CCFG MAC, else nothing is written. Its state after it in *state (1 on, 0 off, -1 other). The contact
// restarts after a write. 1 written, 0 already so / read, -1 refused or failed (err).
int contact_vddr(int on, const char *want_mac, int *state, char *mac, char *err, size_t cap)
{
    static cjtag_t cj;
    static uint8_t buf[CJTAG_PAGE];
    const int tck = OWS_PIN_CONTACT_TCK, tms = OWS_PIN_CONTACT_TMS;
    *state = -1;
    if (tck < 0 || tms < 0) {
        snprintf(err, cap, "no debug port wired (M, C)");
        return -1;
    }
    cj_pins_t pins = {tck, tms};
    gpio_reset_pin(tck);
    gpio_set_direction(tck, GPIO_MODE_OUTPUT);
    gpio_set_level(tck, 1);
    gpio_reset_pin(tms);
    gpio_set_pull_mode(tms, GPIO_PULLUP_ONLY);
    gpio_set_direction(tms, GPIO_MODE_INPUT_OUTPUT);
    gpio_set_level(tms, 1);
    cjtag_io_t io = {cj_set_pins, cj_set_tms_dir, cj_get_tms, cj_delay, &pins};
    bool connected = false;
    for (int i = 0; i < 3 && !connected; i++) {
        cjtag_init(&cj, &io);
        connected = cjtag_connect(&cj, true);
        if (!connected) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    int r = -1;
    if (!connected) {
        snprintf(err, cap, "%s", cj.error);
    } else {
        memcpy(mac, cj.mac, 9);
        if (on >= 0 && (!want_mac || strcmp(want_mac, cj.mac) != 0)) {
            snprintf(err, cap, "the contact is %s, not %s: not written", cj.mac, want_mac ? want_mac : "(none)");
        } else {
            r = on >= 0 ? cjtag_set_vddr_boost(&cj, on != 0, buf) : 0;
            if (r < 0) {
                snprintf(err, cap, "%s", cj.error);
            }
            uint8_t cc[16];
            if (cjtag_mem_read_block(&cj, CJTAG_CCFG_ADDR, cc, sizeof cc)) {
                uint32_t mc1 = (uint32_t)cc[4] | (uint32_t)cc[5] << 8 | (uint32_t)cc[6] << 16 | (uint32_t)cc[7] << 24;
                uint32_t mc =
                    (uint32_t)cc[12] | (uint32_t)cc[13] << 8 | (uint32_t)cc[14] << 16 | (uint32_t)cc[15] << 24;
                bool b_on = ((mc1 >> 20) & 0xF) == 0xC && !(mc & 0x02000000u);
                bool b_off = ((mc1 >> 20) & 0xF) == 0x8 && (mc & 0x02000000u);
                *state = b_on ? 1 : b_off ? 0 : -1;
            }
        }
        cjtag_release_and_reset(&cj);
    }
    line_free(tck);
    line_free(tms);
    if (r > 0) {
        contact_reset();  // the new CCFG takes effect at a start
    }
    return r;
}

// Bring-up: page 31 read (op 0, into page) or its code area written from code (op 1; the CCFG kept, cjtag.c), page
// receiving the page as it was. 1/0/-1 as cjtag_write_page31_code (a read: 1).
int contact_page31(int op, const uint8_t *code, uint8_t *page, char *err, size_t cap)
{
    static cjtag_t cj;
    static uint8_t buf[CJTAG_PAGE];
    const int tck = OWS_PIN_CONTACT_TCK, tms = OWS_PIN_CONTACT_TMS;
    cj_pins_t pins = {tck, tms};
    gpio_reset_pin(tck);
    gpio_set_direction(tck, GPIO_MODE_OUTPUT);
    gpio_set_level(tck, 1);
    gpio_reset_pin(tms);
    gpio_set_pull_mode(tms, GPIO_PULLUP_ONLY);
    gpio_set_direction(tms, GPIO_MODE_INPUT_OUTPUT);
    gpio_set_level(tms, 1);
    cjtag_io_t io = {cj_set_pins, cj_set_tms_dir, cj_get_tms, cj_delay, &pins};
    bool connected = false;
    for (int i = 0; i < 8 && !connected; i++) {
        cjtag_init(&cj, &io);
        connected = cjtag_connect(&cj, true);
        if (!connected) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    int r = -1;
    if (!connected) {
        snprintf(err, cap, "%s", cj.error);
    } else {
        r = op == 0 ? (cjtag_dump_page(&cj, CJTAG_CCFG_PAGE_IDX, page) ? 1 : -1)
                    : cjtag_write_page31_code(&cj, code, buf, page);
        if (r < 0) {
            snprintf(err, cap, "%s", cj.error);
        }
        cjtag_release_and_reset(&cj);
    }
    line_free(tck);
    line_free(tms);
    return r;
}

// Bring-up: a restart, then after wait_ms the debug port attached without a halt; n samples of the CPU's registers
// gap_ms apart, then len bytes of memory at addr; the CPU left running. Text into out.
size_t contact_sample(uint32_t wait_ms, int n, uint32_t gap_ms, uint32_t addr, uint32_t len, bool restart_first,
                      char *out, size_t cap)
{
    static cjtag_t cj;
    static uint8_t mem[512];
    size_t k = 0;
    const int tck = OWS_PIN_CONTACT_TCK, tms = OWS_PIN_CONTACT_TMS;
    if (!contact_uart_take(2000)) {
        return (size_t)snprintf(out, cap, "busy\n");
    }
    if (restart_first) {
        restart(false);
    }
    vTaskDelay(pdMS_TO_TICKS(wait_ms));
    cj_pins_t pins = {tck, tms};
    gpio_reset_pin(tck);
    gpio_set_direction(tck, GPIO_MODE_OUTPUT);
    gpio_set_level(tck, 1);
    gpio_reset_pin(tms);
    gpio_set_pull_mode(tms, GPIO_PULLUP_ONLY);
    gpio_set_direction(tms, GPIO_MODE_INPUT_OUTPUT);
    gpio_set_level(tms, 1);
    cjtag_io_t io = {cj_set_pins, cj_set_tms_dir, cj_get_tms, cj_delay, &pins};
    bool connected = false;
    for (int i = 0; i < 5 && !connected; i++) {
        cjtag_init(&cj, &io);
        connected = cjtag_connect(&cj, false);
        if (!connected) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    if (!connected) {
        k += (size_t)snprintf(out + k, cap - k, "no connect: %s\n", cj.error);
    } else {
        for (int i = 0; i < n && k < cap - 200; i++) {
            uint32_t r[CJTAG_SAMPLE_REGS];
            if (cjtag_sample(&cj, r)) {
                k +=
                    (size_t)snprintf(out + k, cap - k,
                                     "pc=%08lx lr=%08lx sp=%08lx xpsr=%08lx r0=%08lx r1=%08lx r2=%08lx r3=%08lx "
                                     "r12=%08lx msp=%08lx psp=%08lx\n",
                                     (unsigned long)r[7], (unsigned long)r[6], (unsigned long)r[5], (unsigned long)r[8],
                                     (unsigned long)r[0], (unsigned long)r[1], (unsigned long)r[2], (unsigned long)r[3],
                                     (unsigned long)r[4], (unsigned long)r[9], (unsigned long)r[10]);
            } else {
                k += (size_t)snprintf(out + k, cap - k, "sample failed: %s\n", cj.error);
            }
            vTaskDelay(pdMS_TO_TICKS(gap_ms));
        }
        len = len > sizeof mem ? sizeof mem : len & ~3u;
        if (len && cjtag_mem_read_block(&cj, addr & ~3u, mem, len)) {
            for (uint32_t i = 0; i < len && k < cap - 64; i += 16) {
                k += (size_t)snprintf(out + k, cap - k, "%08lx:", (unsigned long)((addr & ~3u) + i));
                for (uint32_t j = 0; j < 16 && i + j < len; j += 4) {
                    uint32_t w = (uint32_t)mem[i + j] | (uint32_t)mem[i + j + 1] << 8 | (uint32_t)mem[i + j + 2] << 16 |
                                 (uint32_t)mem[i + j + 3] << 24;
                    k += (size_t)snprintf(out + k, cap - k, " %08lx", (unsigned long)w);
                }
                k += (size_t)snprintf(out + k, cap - k, "\n");
            }
        } else if (len) {
            k += (size_t)snprintf(out + k, cap - k, "memory read failed: %s\n", cj.error);
        }
        cjtag_release_running(&cj);
    }
    line_free(tck);
    line_free(tms);
    contact_uart_give();
    return k;
}

// Bring-up: a power cycle (or an S reset), then the contact's UART from its start for ms, send (n bytes) written at
// send_at_ms; returns the bytes received (into out). The UART is taken from the relay meanwhile.
size_t contact_capture(uint32_t ms, const uint8_t *send, size_t n, uint32_t send_at_ms, uint8_t *out, size_t cap)
{
    if (!contact_uart_take(2000)) {
        return 0;
    }
    contact_uart_flush_input();
    restart(false);
    size_t got = 0;
    bool sent = n == 0;
    int64_t t0 = esp_timer_get_time(), end = t0 + (int64_t)ms * 1000;
    while (got < cap && esp_timer_get_time() < end) {
        if (!sent && esp_timer_get_time() - t0 >= (int64_t)send_at_ms * 1000) {
            contact_uart_write(send, n);
            sent = true;
        }
        got += contact_uart_read(out + got, cap - got, 10);
    }
    contact_uart_give();
    return got;
}

// The debug-port work runs in its own task, with room to spare, only for as long as it takes (#81: the first bench
// run, when cjtag.c still kept 4 KB page buffers on the stack, overflowed the relay task's 6 KB stack into the Wi-Fi
// driver's memory; it now reads in 256-byte chunks, but the task costs nothing to keep).
typedef struct {
    uint32_t image_len;
    char *mac, *err;
    size_t cap;
    int result;
    TaskHandle_t caller;
} cj_job_t;

static void cj_worker(void *arg)
{
    cj_job_t *job = arg;
    job->result = cjtag_flash_now(job->image_len, job->mac, job->err, job->cap);
    xTaskNotifyGive(job->caller);
    vTaskDelete(NULL);
}

static int gw_cjtag_flash(void *ctx, uint32_t image_len, char *mac, char *err, size_t cap)
{
    (void)ctx;
    cj_job_t job = {image_len, mac, err, cap, -1, xTaskGetCurrentTaskHandle()};
    if (xTaskCreate(cj_worker, "gw_cjtag", 12288, &job, uxTaskPriorityGet(NULL), NULL) != pdPASS) {
        snprintf(err, cap, "out of memory for the debug-port task");
        return -1;
    }
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    return job.result;
}

void contact_gwsync(void)
{
    gwsync_io_t io = {sbl_write, sbl_read,    sbl_flush, gw_reset,       gw_bootloader,         gw_sleep,
                      gw_image,  gw_progress, NULL,      gw_cjtag_flash, settings.contact_flash};
    uint32_t img_len = 0;
    active_gw_image(&img_len);
    contact_uart_take(portMAX_DELAY);
    if (settings.skip_gwsync) {
        gwsync_probe(&io, img_len, s_page, &gw_report);
    } else {
        gwsync_run(&io, img_len, &s_sbl, s_page, &gw_report);
    }
    contact_uart_flush_input();
    contact_uart_give();
    gw_checked = true;
    ESP_LOGI("contact", "gateway: %s", gw_report.msg);
}

const gwsync_report_t *contact_gwsync_report(void)
{
    if (gw_checked && !gw_report.mac[0] && s_sbl.mac[0]) {
        memcpy(gw_report.mac, s_sbl.mac, sizeof gw_report.mac);
    }
    return gw_checked ? &gw_report : NULL;
}

bool contact_nv_export(char *json_out, size_t json_cap, char *msg, size_t msg_cap)
{
    gwnv_io_t io = {sbl_write, sbl_read, sbl_flush, NULL};
    if (!contact_uart_take(5000)) {
        snprintf(msg, msg_cap, "the UART is busy");
        return false;
    }
    gwnv_doc_t doc;
    bool ok = gwnv_export(&io, &doc, msg, msg_cap);
    contact_uart_flush_input();
    contact_uart_give();
    if (!ok) {
        return false;
    }
    if (!gwnv_format_json(&doc, json_out, json_cap)) {
        snprintf(msg, msg_cap, "NV JSON buffer too small");
        return false;
    }
    return true;
}

bool contact_nv_import(const gwnv_doc_t *doc, char *msg, size_t msg_cap)
{
    gwnv_io_t io = {sbl_write, sbl_read, sbl_flush, NULL};
    if (!contact_uart_take(5000)) {
        snprintf(msg, msg_cap, "the UART is busy");
        return false;
    }
    bool ok = gwnv_import(&io, doc, msg, msg_cap);
    contact_uart_flush_input();
    contact_uart_give();
    if (ok) {
        snprintf(msg, msg_cap, "imported network %04X/%04X and %u sensors", doc->pan, doc->short_addr,
                 (unsigned)doc->n_sensors);
    }
    return ok;
}

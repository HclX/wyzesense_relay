// Native unit tests of esp32/contact/sblprobe.c and sbl_read_memory/sbl_dump_page against a simulated CC1310 ROM
// bootloader behind 4 unmarked pads (issue #81).
#include "sblprobe.h"

#include <stdio.h>
#include <string.h>

#include "sbl.h"

static int checks, failures;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        checks++;                                                                    \
        if (!(cond)) {                                                               \
            failures++;                                                              \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                            \
    } while (0)

typedef struct {
    // Pad permutation: which pad index (0..3) corresponds to R, T, W, S
    int pad_r;
    int pad_t;
    int pad_w;
    int pad_s;
    int backdoor_pad;  // pad_r, pad_w, or -1 (bootloader disabled)
    uint32_t chip_id;

    // Physical pin & power state
    bool powered;
    bool in_reset;
    bool in_sbl;
    bool sbl_synced;
    bool pin_low[SBLPROBE_PADS];
    bool uart_open;
    int uart_rx_pad;
    int uart_tx_pad;
    bool uart_tx_od;

    // Safety tracking
    bool backpower_violation;
    uint32_t uart_driven_mask;  // bitmask of pads ever driven push-pull by UART TX

    // SBL packet parser & response queue
    uint8_t rx_buf[300];
    size_t rx_len;
    uint8_t tx_queue[1024];
    size_t tx_len;

    // Simulated 128 KB flash for sbl_read_memory / sbl_dump_page tests
    uint8_t flash[32 * SBL_PAGE];
    int read_cmd_count;
    int glitch_on_read_cmd;

    unsigned touch_mask;  // probes on the sensor (the touching callback)
} sim_sbl_target_t;

static void sim_queue_bytes(sim_sbl_target_t *s, const uint8_t *data, size_t len)
{
    if (s->tx_len + len <= sizeof s->tx_queue) {
        memcpy(s->tx_queue + s->tx_len, data, len);
        s->tx_len += len;
    }
}

static void sim_queue_packet(sim_sbl_target_t *s, const uint8_t *payload, size_t len)
{
    uint8_t hdr[2];
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum = (uint8_t)(sum + payload[i]);
    }
    hdr[0] = (uint8_t)(len + 2u);
    hdr[1] = sum;
    sim_queue_bytes(s, hdr, 2);
    sim_queue_bytes(s, payload, len);
}

static void sim_eval_boot(sim_sbl_target_t *s)
{
    if (!s->powered) {
        s->in_reset = true;
        s->in_sbl = false;
        s->sbl_synced = false;
        s->rx_len = 0;
        s->tx_len = 0;
        return;
    }
    if (s->pin_low[s->pad_s]) {
        // S (RESET_N) held low holds the chip in reset
        s->in_reset = true;
        s->in_sbl = false;
        s->sbl_synced = false;
        s->rx_len = 0;
        s->tx_len = 0;
        return;
    }
    if (s->in_reset) {
        // Rising edge of reset / power-on: sample backdoor pad
        s->in_reset = false;
        if (s->backdoor_pad >= 0 && s->pin_low[s->backdoor_pad]) {
            s->in_sbl = true;
            s->sbl_synced = false;
        } else {
            s->in_sbl = false;
            s->sbl_synced = false;
        }
        s->rx_len = 0;
        s->tx_len = 0;
    }
}

static void sim_pin_low(void *ctx, int pad)
{
    sim_sbl_target_t *s = ctx;
    if (pad >= 0 && pad < SBLPROBE_PADS) {
        s->pin_low[pad] = true;
        sim_eval_boot(s);
    }
}

static void sim_pin_release(void *ctx, int pad)
{
    sim_sbl_target_t *s = ctx;
    if (pad >= 0 && pad < SBLPROBE_PADS) {
        s->pin_low[pad] = false;
        sim_eval_boot(s);
    }
}

static bool sim_pin_read(void *ctx, int pad)
{
    sim_sbl_target_t *s = ctx;
    if (pad < 0 || pad >= SBLPROBE_PADS) {
        return false;
    }
    return !s->pin_low[pad];
}

static void sim_power(void *ctx, bool on)
{
    sim_sbl_target_t *s = ctx;
    if (!on) {
        // When powering off, all pins must be released and UART closed so nothing back-powers the chip
        if (s->uart_open) {
            s->backpower_violation = true;
        }
        for (int i = 0; i < SBLPROBE_PADS; i++) {
            if (s->pin_low[i]) {
                s->backpower_violation = true;
            }
        }
    }
    s->powered = on;
    sim_eval_boot(s);
}

static bool sim_uart_open(void *ctx, int rx_pad, int tx_pad, bool tx_open_drain)
{
    sim_sbl_target_t *s = ctx;
    s->uart_open = true;
    s->uart_rx_pad = rx_pad;
    s->uart_tx_pad = tx_pad;
    s->uart_tx_od = tx_open_drain;
    if (!tx_open_drain && tx_pad >= 0 && tx_pad < SBLPROBE_PADS) {
        s->uart_driven_mask |= (1u << (unsigned)tx_pad);
    }
    return true;
}

static void sim_uart_close(void *ctx)
{
    sim_sbl_target_t *s = ctx;
    s->uart_open = false;
    s->uart_rx_pad = -1;
    s->uart_tx_pad = -1;
}

static void sim_handle_sbl_rx(sim_sbl_target_t *s)
{
    static const uint8_t ack[2] = {0x00, 0xCC};
    static const uint8_t nak[2] = {0x00, 0x33};

    if (!s->sbl_synced) {
        while (s->rx_len >= 2) {
            if (s->rx_buf[0] == 0x55 && s->rx_buf[1] == 0x55) {
                s->sbl_synced = true;
                memmove(s->rx_buf, s->rx_buf + 2, s->rx_len - 2);
                s->rx_len -= 2;
                sim_queue_bytes(s, ack, 2);
                break;
            }
            memmove(s->rx_buf, s->rx_buf + 1, --s->rx_len);
        }
    }

    while (s->sbl_synced && s->rx_len > 0) {
        // Skip host ACK/NAK (00 CC / 00 33) or zero padding bytes
        if (s->rx_buf[0] == 0x00) {
            if (s->rx_len >= 2 && (s->rx_buf[1] == 0xCC || s->rx_buf[1] == 0x33)) {
                memmove(s->rx_buf, s->rx_buf + 2, s->rx_len - 2);
                s->rx_len -= 2;
                continue;
            }
            memmove(s->rx_buf, s->rx_buf + 1, --s->rx_len);
            continue;
        }
        uint8_t size = s->rx_buf[0];
        if (size < 3) {
            memmove(s->rx_buf, s->rx_buf + 1, --s->rx_len);
            continue;
        }
        if (s->rx_len < size) {
            break;
        }
        uint8_t want_sum = s->rx_buf[1];
        uint8_t cmd = s->rx_buf[2];
        const uint8_t *data = s->rx_buf + 3;
        size_t data_len = size - 3u;
        uint8_t sum = cmd;
        for (size_t i = 0; i < data_len; i++) {
            sum = (uint8_t)(sum + data[i]);
        }
        if (sum != want_sum) {
            sim_queue_bytes(s, nak, 2);
            memmove(s->rx_buf, s->rx_buf + size, s->rx_len - size);
            s->rx_len -= size;
            continue;
        }
        sim_queue_bytes(s, ack, 2);
        if (cmd == 0x20) {
            // CMD_PING: ACK only
        } else if (cmd == 0x28) {
            // CMD_GET_CHIP_ID: 4-byte big-endian chip_id
            uint8_t id[4] = {
                (uint8_t)(s->chip_id >> 24),
                (uint8_t)(s->chip_id >> 16),
                (uint8_t)(s->chip_id >> 8),
                (uint8_t)s->chip_id,
            };
            sim_queue_packet(s, id, 4);
        } else if (cmd == 0x2A && data_len == 6) {
            // CMD_MEMORY_READ: addr (be32), type (1 = 32-bit), count (1..63 words)
            uint32_t addr = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) | ((uint32_t)data[2] << 8) | data[3];
            uint8_t type = data[4];
            uint8_t count = data[5];
            size_t nbytes = (size_t)count * 4u;
            if (type == 1 && count > 0 && count <= 63 && addr + nbytes <= sizeof s->flash) {
                uint8_t out[63 * 4];
                memcpy(out, s->flash + addr, nbytes);
                s->read_cmd_count++;
                if (s->glitch_on_read_cmd > 0 && s->read_cmd_count == s->glitch_on_read_cmd) {
                    out[0] ^= 0x01u;
                }
                sim_queue_packet(s, out, nbytes);
            }
        }
        memmove(s->rx_buf, s->rx_buf + size, s->rx_len - size);
        s->rx_len -= size;
    }
}

static bool sim_uart_write(void *ctx, const uint8_t *data, size_t len)
{
    sim_sbl_target_t *s = ctx;
    if (!s->uart_open) {
        return false;
    }
    if (!s->uart_tx_od && s->uart_tx_pad >= 0 && s->uart_tx_pad < SBLPROBE_PADS) {
        s->uart_driven_mask |= (1u << (unsigned)s->uart_tx_pad);
    }
    // Only reaches the chip's UART RX if uart_tx_pad == pad_r and chip is in SBL
    if (s->powered && !s->in_reset && s->in_sbl && s->uart_tx_pad == s->pad_r && s->pad_r >= 0 &&
        s->pad_r < SBLPROBE_PADS && !s->pin_low[s->pad_r]) {
        if (s->rx_len + len <= sizeof s->rx_buf) {
            memcpy(s->rx_buf + s->rx_len, data, len);
            s->rx_len += len;
            sim_handle_sbl_rx(s);
        }
    }
    return true;
}

static size_t sim_uart_read(void *ctx, uint8_t *buf, size_t cap, uint32_t timeout_ms)
{
    (void)timeout_ms;
    sim_sbl_target_t *s = ctx;
    if (!s->uart_open || s->uart_rx_pad != s->pad_t || s->tx_len == 0) {
        return 0;
    }
    size_t n = (s->tx_len < cap) ? s->tx_len : cap;
    memcpy(buf, s->tx_queue, n);
    memmove(s->tx_queue, s->tx_queue + n, s->tx_len - n);
    s->tx_len -= n;
    return n;
}

static void sim_uart_flush(void *ctx)
{
    sim_sbl_target_t *s = ctx;
    s->tx_len = 0;
}

static bool sim_touching(void *ctx, int pad)
{
    const sim_sbl_target_t *s = ctx;
    return pad >= 0 && pad < SBLPROBE_PADS && (s->touch_mask & (1u << (unsigned)pad));
}

static void sim_sleep_ms(void *ctx, uint32_t ms)
{
    (void)ctx;
    (void)ms;
}

static void sim_init(sim_sbl_target_t *s, int r, int t, int w, int rst, int bd_pad, uint32_t chip_id)
{
    memset(s, 0, sizeof *s);
    s->pad_r = r;
    s->pad_t = t;
    s->pad_w = w;
    s->pad_s = rst;
    s->backdoor_pad = bd_pad;
    s->chip_id = chip_id;
    s->in_reset = true;
    s->uart_rx_pad = -1;
    s->uart_tx_pad = -1;
    for (size_t i = 0; i < sizeof s->flash; i++) {
        s->flash[i] = (uint8_t)((i * 41u + 7u) & 0xFFu);
    }
}

int main(void)
{
    static sim_sbl_target_t sim;
    sblprobe_io_t io = {
        .pin_low = sim_pin_low,
        .pin_release = sim_pin_release,
        .pin_read = sim_pin_read,
        .power = sim_power,
        .uart_open = sim_uart_open,
        .uart_close = sim_uart_close,
        .uart_write = sim_uart_write,
        .uart_read = sim_uart_read,
        .uart_flush = sim_uart_flush,
        .sleep_ms = sim_sleep_ms,
        .touching = sim_touching,
        .ctx = &sim,
    };

    // 1. Test all 24 permutations of (R, T, W, S) across the 4 pads, with backdoor on R and with backdoor on W
    int perms = 0;
    for (int r = 0; r < 4; r++) {
        for (int t = 0; t < 4; t++) {
            if (t == r) {
                continue;
            }
            for (int w = 0; w < 4; w++) {
                if (w == r || w == t) {
                    continue;
                }
                int rst = 6 - r - t - w;
                perms++;

                // Case A: backdoor pad is R itself (e.g. Wyze dongle / v1 motion DIO1)
                uint32_t want_id_r = 0x2082F000u + (uint32_t)perms;
                sim_init(&sim, r, t, w, rst, r, want_id_r);
                sblprobe_result_t res;
                CHECK(sblprobe_find(&io, &res));
                CHECK(res.r_pad == r);
                CHECK(res.t_pad == t);
                CHECK(res.backdoor_pad == r);
                CHECK(res.chip_id == want_id_r);
                CHECK(!sim.backpower_violation);
                // No pad other than R is ever driven push-pull by the UART
                CHECK(sim.uart_driven_mask == (1u << (unsigned)r));

                // Case B: backdoor pad is W (another pad, e.g. v1 contact DIO10)
                uint32_t want_id_w = 0x10029000u + (uint32_t)perms;
                sim_init(&sim, r, t, w, rst, w, want_id_w);
                CHECK(sblprobe_find(&io, &res));
                CHECK(res.r_pad == r);
                CHECK(res.t_pad == t);
                CHECK(res.backdoor_pad == w);
                CHECK(res.chip_id == want_id_w);
                CHECK(!sim.backpower_violation);
                CHECK(sim.uart_driven_mask == (1u << (unsigned)r));

                // Re-entering with the remembered mapping succeeds in one attempt and drives only R
                sim.uart_driven_mask = 0;
                uint32_t re_id = 0;
                char err[96] = "";
                CHECK(sblprobe_enter(&io, &res, &re_id, err, sizeof err));
                CHECK(re_id == want_id_w);
                CHECK(sim.uart_driven_mask == (1u << (unsigned)r));
            }
        }
    }
    CHECK(perms == 24);

    // 2. Disabled bootloader case: reports the exact error and never drives any pad push-pull with the UART
    sim_init(&sim, 2, 0, 3, 1, -1, 0x2082F000u);
    sblprobe_result_t res;
    CHECK(!sblprobe_find(&io, &res));
    CHECK(strcmp(res.error, "no TI ROM bootloader answered: bootloader disabled, or not a TI CC13xx/CC26xx") == 0);
    CHECK(!sim.backpower_violation);
    CHECK(sim.uart_driven_mask == 0);

    // 3. Test sbl_read_memory and sbl_dump_page (double-read comparison & glitch rejection)
    sim_init(&sim, 1, 3, 0, 2, 0, 0x2082F000u);
    CHECK(sblprobe_find(&io, &res));

    sbl_t sbl;
    sbl_io_t sio = {
        .write = sim_uart_write,
        .read = sim_uart_read,
        .flush_input = sim_uart_flush,
        .ctx = &sim,
    };
    sbl_init(&sbl, &sio);

    // Reject unaligned or zero-length reads
    uint8_t page_buf[SBL_PAGE];
    CHECK(!sbl_read_memory(&sbl, 0, page_buf, 0));
    CHECK(!sbl_read_memory(&sbl, 2, page_buf, 16));
    CHECK(!sbl_read_memory(&sbl, 0, page_buf, 14));

    // Multi-packet read (512 bytes > 252 bytes max per SBL packet)
    CHECK(sbl_read_memory(&sbl, 0x1000, page_buf, 512));
    CHECK(memcmp(page_buf, sim.flash + 0x1000, 512) == 0);

    // sbl_dump_page reads 4 KB twice and compares: inject a 1-bit glitch on pass 2 and verify rejection
    sim.read_cmd_count = 0;
    sim.glitch_on_read_cmd = 20;  // inside pass 2 (pass 1 is 17 packets of <=252 bytes)
    CHECK(!sbl_dump_page(&sbl, 3, page_buf));

    // Without glitch, sbl_dump_page succeeds and matches flash
    sim.glitch_on_read_cmd = 0;
    CHECK(sbl_dump_page(&sbl, 3, page_buf));
    CHECK(memcmp(page_buf, sim.flash + (size_t)3 * SBL_PAGE, SBL_PAGE) == 0);

    // 2 pads: two hand-held probes on the sensor's TX and RX, + and - clipped (#81): both orders, the backdoor on R (the
    // stock DIO1) or on T; S and W (pads 2, 3) are not probed, and only R is ever driven by the UART
    for (int r = 0; r < 2; r++) {
        int t = 1 - r;
        for (int b = 0; b < 2; b++) {
            int bd = b ? t : r;
            uint32_t want = 0x20018000u + (uint32_t)(r * 2 + b);
            sim_init(&sim, r, t, 2, 3, bd, want);
            sblprobe_result_t res2;
            CHECK(sblprobe_find_n(&io, 2, &res2));
            CHECK(res2.r_pad == r && res2.t_pad == t && res2.backdoor_pad == bd && res2.chip_id == want);
            CHECK(!sim.backpower_violation);
            CHECK(sim.uart_driven_mask == (1u << (unsigned)r));
        }
    }
    sim_init(&sim, 0, 1, 2, 3, -1, 0x20018000u);  // bootloader disabled: nothing answers, the sensor left unpowered
    sblprobe_result_t none;
    CHECK(!sblprobe_find_n(&io, 2, &none) && none.error[0] && !sim.powered && !sim.backpower_violation);
    sim_init(&sim, 0, 1, 2, 3, 0, 0x20018000u);  // the backdoor on a pad that is not probed (W): not found
    sim.backdoor_pad = 2;
    CHECK(!sblprobe_find_n(&io, 2, &none));
    CHECK(!sblprobe_find_n(&io, 3, &none));  // only 2 or 4

    // touch: all of the first n probes on the sensor
    sim.touch_mask = 0x3;
    CHECK(sblprobe_touched(&io, 2));
    sim.touch_mask = 0x1;
    CHECK(!sblprobe_touched(&io, 2));
    CHECK(sblprobe_touched(&io, 1));
    sblprobe_io_t no_touch = io;
    no_touch.touching = NULL;
    sim.touch_mask = 0x3;
    CHECK(!sblprobe_touched(&no_touch, 2));

    if (failures) {
        fprintf(stderr, "%d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("test_sblprobe: %d checks passed\n", checks);
    return 0;
}

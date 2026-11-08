// Portable UART ROM bootloader (SBL) auto-detection for unmarked CC13xx/CC26xx sensors (issue #81).
// Given 4 probe pads (S, W, R, T in unknown order) + a switched power rail, sweeps all 12 ordered (R, T) pairs and
// the 3 backdoor candidates (R first, then the two remaining pads). With 2 pads (two hand-held probes on the sensor's
// TX and RX, + and - clipped: the user, 2026-11-07): both orders, the backdoor on R (the stock DIO1) or on the other. All probe pads are only ever driven low or
// released with pull-up (open-drain); the UART TX pin is only driven push-pull once R is confirmed by the SBL
// autobaud ACK (0x00 0xCC).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SBLPROBE_PADS 4

typedef struct {
    void (*pin_low)(void *ctx, int pad);
    void (*pin_release)(void *ctx, int pad);
    bool (*pin_read)(void *ctx, int pad);
    void (*power)(void *ctx, bool on);
    bool (*uart_open)(void *ctx, int rx_pad, int tx_pad, bool tx_open_drain);
    void (*uart_close)(void *ctx);
    bool (*uart_write)(void *ctx, const uint8_t *data, size_t len);
    size_t (*uart_read)(void *ctx, uint8_t *buf, size_t cap, uint32_t timeout_ms);
    void (*uart_flush)(void *ctx);
    void (*sleep_ms)(void *ctx, uint32_t ms);
    // optional: is this probe on a pad of the (unpowered) sensor? (the ESP: a 20 us pull-up pulse, read back)
    bool (*touching)(void *ctx, int pad);
    void *ctx;
} sblprobe_io_t;

typedef struct {
    int r_pad;         // 0..3: target UART RX input (ESP UART TX)
    int t_pad;         // 0..3: target UART TX output (ESP UART RX)
    int backdoor_pad;  // 0..3: backdoor pad held low at power-on (may equal r_pad)
    uint32_t chip_id;  // 32-bit chip ID from SBL COMMAND_GET_CHIP_ID (0x28)
    char error[96];
} sblprobe_result_t;

// Sweep all 12 ordered (R, T) pairs and 3 backdoor candidates per pair to find the sensor's pad roles and confirm
// with GET_CHIP_ID through sbl.c. On success returns true with the UART open on (t_pad, r_pad) and the bootloader
// synced.
bool sblprobe_find(const sblprobe_io_t *io, sblprobe_result_t *res);

// The same over the first `pads` probes (2 or 4).
bool sblprobe_find_n(const sblprobe_io_t *io, int pads, sblprobe_result_t *res);

// Whether all of the first `pads` probes are on the sensor (false without a touching callback).
bool sblprobe_touched(const sblprobe_io_t *io, int pads);

// Enter the ROM bootloader using a known (r_pad, t_pad, backdoor_pad) mapping, sync autobaud, and confirm GET_CHIP_ID.
bool sblprobe_enter(const sblprobe_io_t *io, const sblprobe_result_t *map, uint32_t *chip_id, char *err,
                    size_t err_cap);

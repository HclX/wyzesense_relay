#include "sblprobe.h"

#include <stdio.h>
#include <string.h>

#include "sbl.h"

static void release_all(const sblprobe_io_t *io)
{
    for (int i = 0; i < SBLPROBE_PADS; i++) {
        io->pin_release(io->ctx, i);
    }
}

static bool wait_autobaud_ack(const sblprobe_io_t *io, uint32_t timeout_ms)
{
    uint8_t buf[16];
    bool saw_zero = false;
    for (int tries = 0; tries < 8; tries++) {
        size_t n = io->uart_read(io->ctx, buf, sizeof buf, timeout_ms);
        if (n == 0) {
            return false;
        }
        for (size_t i = 0; i < n; i++) {
            if (buf[i] == 0x00) {
                saw_zero = true;
            } else if (buf[i] == 0xCC && saw_zero) {
                return true;
            } else {
                return false;
            }
        }
    }
    return false;
}

static bool try_candidate(const sblprobe_io_t *io, int r, int t, int bd, uint32_t *chip_id_out)
{
    static const uint8_t autobaud[2] = {0x55, 0x55};

    io->uart_close(io->ctx);
    release_all(io);
    io->power(io->ctx, false);
    io->sleep_ms(io->ctx, 200);

    io->pin_low(io->ctx, bd);
    io->power(io->ctx, true);
    io->sleep_ms(io->ctx, 50);
    io->pin_release(io->ctx, bd);

    // Keep TX open-drain while probing so no unknown pad is ever driven high before R is confirmed
    if (!io->uart_open(io->ctx, t, r, true)) {
        return false;
    }
    io->uart_flush(io->ctx);
    if (!io->uart_write(io->ctx, autobaud, sizeof autobaud) || !wait_autobaud_ack(io, 100)) {
        io->uart_close(io->ctx);
        release_all(io);
        return false;
    }

    // Autobaud ACK confirmed R is the chip's UART input: enable normal UART TX on R and verify GET_CHIP_ID via sbl.c
    if (!io->uart_open(io->ctx, t, r, false)) {
        io->uart_close(io->ctx);
        release_all(io);
        return false;
    }
    sbl_t sbl;
    sbl_io_t sio = {
        .write = io->uart_write,
        .read = io->uart_read,
        .flush_input = io->uart_flush,
        .ctx = io->ctx,
    };
    sbl_init(&sbl, &sio);
    if (!sbl_get_chip_id(&sbl, chip_id_out)) {
        io->uart_close(io->ctx);
        release_all(io);
        return false;
    }
    return true;
}

bool sblprobe_touched(const sblprobe_io_t *io, int pads)
{
    if (!io->touching || pads < 1 || pads > SBLPROBE_PADS) {
        return false;
    }
    for (int i = 0; i < pads; i++) {
        if (!io->touching(io->ctx, i)) {
            return false;
        }
    }
    return true;
}

bool sblprobe_find(const sblprobe_io_t *io, sblprobe_result_t *res)
{
    return sblprobe_find_n(io, SBLPROBE_PADS, res);
}

bool sblprobe_find_n(const sblprobe_io_t *io, int pads, sblprobe_result_t *res)
{
    memset(res, 0, sizeof *res);
    res->r_pad = -1;
    res->t_pad = -1;
    res->backdoor_pad = -1;
    if (pads != 2 && pads != SBLPROBE_PADS) {
        snprintf(res->error, sizeof res->error, "2 or 4 pads");
        return false;
    }

    for (int r = 0; r < pads; r++) {
        for (int t = 0; t < pads; t++) {
            if (t == r) {
                continue;
            }
            int candidates[3];  // R first (the stock backdoor, DIO1), then the others (with 2 pads: T)
            candidates[0] = r;
            int k = 1;
            for (int p = 0; p < pads; p++) {
                if (p != r && (p != t || pads == 2)) {
                    candidates[k++] = p;
                }
            }
            for (int c = 0; c < k; c++) {
                uint32_t chip_id = 0;
                if (try_candidate(io, r, t, candidates[c], &chip_id)) {
                    res->r_pad = r;
                    res->t_pad = t;
                    res->backdoor_pad = candidates[c];
                    res->chip_id = chip_id;
                    return true;
                }
            }
        }
    }

    io->uart_close(io->ctx);
    release_all(io);
    io->power(io->ctx, false);
    snprintf(res->error, sizeof res->error,
             "no TI ROM bootloader answered: bootloader disabled, or not a TI CC13xx/CC26xx");
    return false;
}

bool sblprobe_enter(const sblprobe_io_t *io, const sblprobe_result_t *map, uint32_t *chip_id, char *err, size_t err_cap)
{
    if (!map || map->r_pad < 0 || map->r_pad >= SBLPROBE_PADS || map->t_pad < 0 || map->t_pad >= SBLPROBE_PADS ||
        map->r_pad == map->t_pad || map->backdoor_pad < 0 || map->backdoor_pad >= SBLPROBE_PADS) {
        if (err && err_cap) {
            snprintf(err, err_cap, "invalid SBL pad mapping");
        }
        return false;
    }
    uint32_t id = 0;
    if (!try_candidate(io, map->r_pad, map->t_pad, map->backdoor_pad, &id)) {
        if (err && err_cap) {
            snprintf(err, err_cap, "no TI ROM bootloader answered: bootloader disabled, or not a TI CC13xx/CC26xx");
        }
        return false;
    }
    if (chip_id) {
        *chip_id = id;
    }
    return true;
}

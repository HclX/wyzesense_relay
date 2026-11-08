// TI's CC13x0 ROM serial bootloader over a UART, in portable C (issue #22): how the ESP32 reflashes the v1 contact
// board it relays for (R held low at a reset puts it in the bootloader: the stock backdoor, DIO1). The C counterpart of tools/uart_sbl.py
// and of tools/sbl_common.py's write path, with the same rules: pages 0-29 are written, page 30 (the NV page) only
// through sbl_write_nv(), page 31 (stock code and the CCFG with the unit's MAC) never; before writing, the CCFG must
// hold a MAC (and the expected one, if given) and the bootloader enabled; after, the chip's CRC32 of what was
// written and of page 31 is checked. The chip is not reset here: the bootloader's own reset leaves it running
// nothing (FACTS §5); the caller resets it through S (RESET_N).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SBL_PAGE      0x1000
#define SBL_APP_END   0x1E000  // pages 0-29
#define SBL_NV_PAGE   0x1E000  // page 30
#define SBL_PAGE31    0x1F000
#define SBL_CCFG      0x1FFA8
#define SBL_SLOT_A    0x01000
#define SBL_SLOT_B    0x0F000
#define SBL_SLOTS_LEN (SBL_SLOT_B + SBL_PAGE)  // selector + slot A + slot B's first page (blank)

typedef struct {
    bool (*write)(void *ctx, const uint8_t *data, size_t len);
    // up to cap bytes; waits up to timeout_ms for the first, then returns what has come; 0 on timeout
    size_t (*read)(void *ctx, uint8_t *buf, size_t cap, uint32_t timeout_ms);
    void (*flush_input)(void *ctx);
    void *ctx;
} sbl_io_t;

typedef struct {
    sbl_io_t io;
    uint8_t pending[300];  // bytes read and not used yet
    size_t pending_len;
    char error[96];  // why the last call failed
    uint32_t chip_id;
    char mac[9];  // the CCFG's ASCII MAC
    uint8_t backdoor_dio;
    bool already_synced;  // the last connect found a bootloader synced by an earlier session
    // a flash in pieces (sbl_stream_*)
    uint32_t stream_at;   // bytes written so far
    uint32_t stream_crc;  // CRC32 of them, running (pre-inverted)
    uint32_t stream_page31;
} sbl_t;

void sbl_init(sbl_t *s, const sbl_io_t *io);

// Auto-baud, ping, chip ID (a bootloader synced already is found with a ping); then page 31 read and its CCFG
// checked (a MAC; expect_mac if not NULL; the bootloader enabled).
bool sbl_connect(sbl_t *s, const char *expect_mac);

// Read 32-bit chip ID (COMMAND_GET_CHIP_ID, 0x28) into s->chip_id (and *chip_id if non-NULL).
bool sbl_get_chip_id(sbl_t *s, uint32_t *chip_id);

// Read 32-bit aligned memory (COMMAND_MEMORY_READ, 0x2A, access type 1 = 32-bit, up to 63 words per packet).
bool sbl_read_memory(sbl_t *s, uint32_t addr, uint8_t *out, size_t len);

// Read a 4 KB flash page (0..31) twice via sbl_read_memory() and verify both passes match byte-for-byte.
bool sbl_dump_page(sbl_t *s, uint32_t page_idx, uint8_t *out_4k);

// Read back a 4 KB flash page (0..31) via sbl_read_memory() and verify it matches expect_4k byte-for-byte.
bool sbl_verify_page(sbl_t *s, uint32_t page_idx, const uint8_t *expect_4k);

// Read all 32 4 KB flash pages (128 KB) via sbl_dump_page().
bool sbl_dump_flash(sbl_t *s, uint8_t *out_128k);

// image at 0x0 (pages 0-29 only), verified by the chip's CRC32, page 31 checked unchanged.
bool sbl_flash(sbl_t *s, const uint8_t *image, size_t len);

// The same in pieces, for a caller that cannot hold the image (the ESP8266 receives it over HTTP): begin, then the
// image a page at a time in order (SBL_PAGE bytes, the last one shorter, a multiple of 4), then end, which checks the
// chip's CRC32 of all of it and page 31. An image whose first page does not start with a vector table is refused
// before anything is written. A failure on the way leaves pages 0-29 part written (the bootloader, the CCFG
// and page 30 untouched): flash again.
bool sbl_stream_begin(sbl_t *s);
bool sbl_stream_page(sbl_t *s, const uint8_t *data, size_t len);
bool sbl_stream_end(sbl_t *s);

// The chip's CRC32 (IEEE) of flash [addr, addr + len), from the bootloader
bool sbl_chip_crc32(sbl_t *s, uint32_t addr, uint32_t len, uint32_t *crc);

// The same CRC on this side, in pieces: c = 0xFFFFFFFF to start, the CRC is ~c at the end
uint32_t sbl_crc32_update(uint32_t c, const uint8_t *p, size_t n);

// Page 30 <- page (4096 bytes: our NV format, or all FF), verified; page 31 checked unchanged.
bool sbl_write_nv(sbl_t *s, const uint8_t *page);

// The OTA layout (docs/design/ota.md) in out (SBL_SLOTS_LEN bytes): the boot selector at 0, slot A at 0x1000, the rest FF.
// False if the selector is over a page or the slot image is not one (magic OWSI) or too long.
bool sbl_slots_image(uint8_t *out, const uint8_t *boot, size_t boot_len, const uint8_t *slot, size_t slot_len);

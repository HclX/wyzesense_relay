// 2-pin IEEE 1149.7 cJTAG (OScan1) + TI ICEPick-C + ARM CoreSight JTAG-DP/AHB-AP + CC1310 flash engine (issue #69).
// Operates over two GPIOs only (M = TMSC, C = TCKC; no RESET_N or W strap needed), with auto-polarity detection on
// the two probe pins and page-by-page dump/flash support so hand-held probes can slip and resume without restarting.
// Hard rule (CLAUDE.md / AGENTS.md): page 31 (0x0001F000..0x0001FFFF, CCFG + factory MAC) is never erased or written,
// except BL_CONFIG through cjtag_set_backdoor (the one CCFG write allowed, #14).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CJTAG_PAGE             0x1000u
#define CJTAG_TOTAL_PAGES      32u
#define CJTAG_FLASH_SIZE       (CJTAG_PAGE * CJTAG_TOTAL_PAGES)  // 128 KB
#define CJTAG_APP_PAGES        30u                               // pages 0-29 (0x00000..0x1DFFF)
#define CJTAG_APP_END          (CJTAG_PAGE * CJTAG_APP_PAGES)    // 0x1E000
#define CJTAG_NV_PAGE_IDX      30u                               // page 30 (0x1E000)
#define CJTAG_CCFG_PAGE_IDX    31u                               // page 31 (0x1F000): never erased or written
#define CJTAG_CCFG_ADDR        0x0001FFA8u
#define CJTAG_PAGE31_CODE_LEN  (CJTAG_CCFG_ADDR - CJTAG_CCFG_PAGE_IDX * CJTAG_PAGE)  // 0xFA8 (4008 B)
#define CJTAG_FCFG1_ADDR       0x50001000u
#define CJTAG_FCFG1_SIZE       0x00000400u  // 1 KB factory configuration
#define CJTAG_CCFG_SIZE        88u          // 0x0001FFA8..0x0001FFFF
#define CJTAG_CCFG_MAC         0x24u        // offsets in the CCFG: Wyze's ASCII MAC (8 bytes)
#define CJTAG_CCFG_BL_CONFIG   0x30u
#define CJTAG_CCFG_TAP_DAP_0   0x3Cu
#define CJTAG_CCFG_IMAGE_VALID 0x44u
#define CJTAG_PAGE29_MAC_ADDR  0x0001D000u  // the MAC of the newer units (v1 contacts too), CCFG slot blank
#define CJTAG_MAX_BL_PIN       14u          // 5x5 package: DIO0-14 (TI errata SWRZ062F advisory 13)
#define CJTAG_ICEPICK_IDCODE   0x0B9BE02Fu  // CC13x0 ICEPick-C IDCODE (ignoring 4-bit version)
#define CJTAG_DAP_IDCODE       0x4BA00477u  // ARM Cortex-M3 CoreSight JTAG-DP IDCODE

typedef struct {
    void (*set_pins)(void *ctx, bool tck, bool tms);
    void (*set_tms_dir)(void *ctx, bool output);
    bool (*get_tms)(void *ctx);
    void (*delay_us)(void *ctx, uint32_t us);
    void *ctx;
} cjtag_io_t;

typedef struct {
    cjtag_io_t io;
    uint8_t cjtag_mode;  // 7 = OScan1, 8 = OScan0 (MScan)
    bool dap_at_tdo;     // true if CS_DAP is closest to TDO, false if ICEPick-C is closest to TDO
    uint8_t cur_dap_ir;  // cached 4-bit JTAG-DP IR (0xFF when unknown)
    uint32_t ap_csw;     // cached AHB-AP CSW register
    uint32_t icepick_id;
    uint32_t dap_id;
    uint32_t user_id;     // FCFG1 USER_ID (0x50001294)
    uint64_t ieee_mac;    // FCFG1 64-bit IEEE 802.15.4 MAC (0x500012F0)
    char mac[9];          // 8-char uppercase hex sensor MAC + NUL
    bool has_ccfg_mac;    // true if page 31 CCFG + 0x24 holds a valid 8-char ASCII hex MAC
    uint32_t bl_config;   // CCFG BL_CONFIG word at 0x0001FFD8
    uint32_t page31_crc;  // IEEE CRC32 of page 31 read at connect time
    char error[96];       // human-readable reason for the last error
} cjtag_t;

void cjtag_init(cjtag_t *c, const cjtag_io_t *io);

// Reset the target's IEEE 1149.7 adapter back to offline JScan0 mode (>=8 TMSC toggles while TCKC=1).
void cjtag_reset_escape(cjtag_t *c);

// Enter 2-pin IEEE 1149.7 OScan1 mode using the exact XDS110 non-adaptive OScan1 sequence, unlock ICEPick-C,
// link the Cortex-M3 CS_DAP into the scan chain, power up the debug domain, and halt the CPU.
// Fast-fails if no CC1310 is on the pins (used for 150 ms hands-free polling and auto-polarity detection).
bool cjtag_connect(cjtag_t *c, bool halt_cpu);

// Read/write a single 32-bit aligned memory word through the Cortex-M3 AHB-AP.
bool cjtag_mem_read32(cjtag_t *c, uint32_t addr, uint32_t *val);
bool cjtag_mem_write32(cjtag_t *c, uint32_t addr, uint32_t val);

// Read/write a block of 32-bit aligned bytes through AHB-AP auto-increment (len must be a multiple of 4).
bool cjtag_mem_read_block(cjtag_t *c, uint32_t addr, uint8_t *out, size_t len);
bool cjtag_mem_write_block(cjtag_t *c, uint32_t addr, const uint8_t *data, size_t len);

// Read a 4 KB flash page (0..31) twice and verify both passes match byte-for-byte so a hand-held probe glitch
// can never corrupt a dump page.
bool cjtag_dump_page(cjtag_t *c, uint32_t page_idx, uint8_t *out_4k);

// Read the 1 KB FCFG1 factory configuration region (0x50001000..0x500013FF) twice and verify both passes match.
bool cjtag_dump_fcfg1(cjtag_t *c, uint8_t *out_1k);

// Erase and program a single 4 KB flash page via the CC1310 ROM Hard-API in SRAM, then read back and verify all
// 4096 bytes and confirm page 31's CRC is unchanged.
// Hard rule: page_idx >= 31 is unconditionally refused; page_idx == 30 requires allow_nv == true.
bool cjtag_flash_page(cjtag_t *c, uint32_t page_idx, const uint8_t *data_4k, bool allow_nv);

// Writes the image (image(ctx, off, buf, n) fills buf from the image) to pages 0-29 page by page, pages already equal
// skipped, through cjtag_flash_page (its checks). buf_4k: the caller's 4 KB buffer. Returns the pages written, or
// -1 (c->error says why).
typedef bool (*cjtag_image_fn)(void *ctx, uint32_t off, uint8_t *buf, size_t n);
int cjtag_write_image(cjtag_t *c, cjtag_image_fn image, void *ctx, uint32_t image_len, uint8_t *buf_4k);
// The one page-31 write allowed (docs/design/v1-contact.md, #14): the ROM bootloader on, its backdoor DIO<pin>
// active low, nothing else changed. Page 31 is read twice and compared and must match the connect's CRC; the CCFG must
// hold a factory MAC, the CPU debug port enabled and a valid image; after the erase the page is written and verified
// whole, and on a failure written back as it was. Returns 1 written, 0 already so, -1 refused or failed (c->error).
int cjtag_set_backdoor(cjtag_t *c, uint8_t pin, uint8_t *buf_4k);
// Fills a blank CCFG MAC slot (0x1FFCC, 8 bytes, all FF) with the unit's own MAC, the one in page 29: newer units keep
// it there only (the user's decision, 2026-11-07, for v1 contact 77A9B885). Program only, never an erase (FF -> the
// MAC needs no 0 -> 1 bit); page 31 read twice before and verified whole after. Returns 1 written, 0 already so, -1
// refused (slot not blank, MAC not the page-29 one) or failed (c->error).
int cjtag_fill_ccfg_mac(cjtag_t *c, const char *mac, uint8_t *buf_4k);
// Bring-up: halts the CPU, reads r0-r3, r12, sp, lr, pc, xpsr, msp, psp into regs, and lets it run on
#define CJTAG_SAMPLE_REGS 11
bool cjtag_sample(cjtag_t *c, uint32_t regs[CJTAG_SAMPLE_REGS]);
// Bring-up: lets go of a running CPU without a reset (debug off, the debug port powered down, the adapter offline)
void cjtag_release_running(cjtag_t *c);
// Bring-up (the stock dongle firmware on a contact board, approved by the user 2026-11-07): page 31's code area
// (0x1F000-0x1FFA7: code is linked there by the stock products) replaced by code (0xFA8 bytes), the unit's own CCFG
// kept byte for byte. Requires the CCFG's MAC, debug port and bootloader on, image valid; page 31 read twice, verified
// whole after, the original put back on a failure. orig_4k receives the page as it was. 1 written, 0 already, -1.
int cjtag_write_page31_code(cjtag_t *c, const uint8_t *code, uint8_t *buf_4k, uint8_t *orig_4k);
// Release the debug domain, trigger a Cortex-M3 VECTRESET so the chip boots cleanly without hanging in ROM
// Halt-In-Boot (HIB), and return the cJTAG adapter to offline mode.
void cjtag_release_and_reset(cjtag_t *c);

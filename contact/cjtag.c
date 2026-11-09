#include "cjtag.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

// ICEPick-C 6-bit IR instructions (ti/icepick.cfg)
#define ICEPICK_IR_ROUTER  0x02u
#define ICEPICK_IR_IDCODE  0x04u
#define ICEPICK_IR_CONNECT 0x07u
#define ICEPICK_IR_BYPASS  0x3Fu

// ARM CoreSight JTAG-DP 4-bit IR instructions
#define DAP_IR_ABORT  0x08u
#define DAP_IR_DPACC  0x0Au
#define DAP_IR_APACC  0x0Bu
#define DAP_IR_IDCODE 0x0Eu

// JTAG-DP 3-bit ACK responses (shifted out in bits [2:0] of 35-bit DPACC/APACC DR)
#define DAP_ACK_WAIT 0x01u
#define DAP_ACK_OK   0x02u

// DP register addresses (A[3:2])
#define DP_CTRL_STAT 0x04u
#define DP_SELECT    0x08u
#define DP_RDBUFF    0x0Cu

// AHB-AP register addresses (bank 0, A[3:2])
#define AP_CSW 0x00u
#define AP_TAR 0x04u
#define AP_DRW 0x0Cu

// AHB-AP CSW values: 32-bit access, master debug, auto-increment off (0x00) or single (0x10)
#define AP_CSW_32BIT_NOINC 0x23000002u
#define AP_CSW_32BIT_INC   0x23000012u

// Cortex-M3 SCS debug registers
#define CM3_DHCSR   0xE000EDF0u
#define CM3_DCRSR   0xE000EDF4u
#define CM3_DCRDR   0xE000EDF8u
#define CM3_DEMCR   0xE000EDFCu
#define CM3_AIRCR   0xE000ED0Cu
#define DHCSR_HALT  0xA05F0003u  // DBGKEY | C_HALT | C_DEBUGEN
#define DHCSR_RUN   0xA05F0001u  // DBGKEY | C_DEBUGEN (C_HALT cleared)
#define DHCSR_SHALT 0x00020000u  // S_HALT bit
#define DHCSR_SRDY  0x00010000u  // S_REGRDY bit

// CC1310 memory map for flash & ROM Hard-API
#define CC1310_ROM_HAPI_CRC32 0x10000048u  // HARD_API->Crc32
#define CC1310_ROM_HAPI_ERASE 0x10000054u  // HARD_API->ReservedLocation1 (HapiSectorErase)
#define CC1310_ROM_HAPI_PROG  0x1000005Cu  // HARD_API->ReservedLocation3 (HapiProgramFlash)
#define CC1310_VIMS_CTL       0x40034004u
#define CC1310_VIMS_STAT      0x40034000u
#define CC1310_FLASH_CFG      0x40030024u
#define CC1310_FLASH_FWLOCK   0x4003003Cu
#define CC1310_FLASH_FWFLAG   0x40030040u
#define CC1310_SRAM_TRAMP     0x20000000u  // bkpt #0 trampoline
#define CC1310_SRAM_BUF       0x20001000u  // 4 KB page buffer in SRAM
#define CC1310_SRAM_SP        0x20004000u  // stack pointer for ROM Hard-API calls

static bool fail(cjtag_t *c, const char *why)
{
    snprintf(c->error, sizeof c->error, "%s", why);
    return false;
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n)  // start at 0xFFFFFFFF, end with ~
{
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return crc;
}

// Reads and checks go in chunks of this size, so that no call needs more than a few hundred bytes of stack (the
// ESP8266's loop has 4 KB in all; the callers hand in the one 4 KB page buffer an operation needs)
#define CHUNK 256u

static bool page_crc(cjtag_t *c, uint32_t addr, uint32_t *out)
{
    uint8_t b[CHUNK];
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t off = 0; off < CJTAG_PAGE; off += CHUNK) {
        if (!cjtag_mem_read_block(c, addr + off, b, CHUNK)) {
            return false;
        }
        crc = crc32_update(crc, b, CHUNK);
    }
    *out = ~crc;
    return true;
}

// Flash at addr equals want (len bytes, a multiple of 4)?
static bool flash_equals(cjtag_t *c, uint32_t addr, const uint8_t *want, size_t len, bool *equal)
{
    uint8_t b[CHUNK];
    *equal = true;
    for (size_t off = 0; off < len; off += CHUNK) {
        size_t n = len - off < CHUNK ? len - off : CHUNK;
        if (!cjtag_mem_read_block(c, addr + (uint32_t)off, b, n)) {
            return false;
        }
        if (memcmp(b, want + off, n) != 0) {
            *equal = false;
            return true;
        }
    }
    return true;
}

void cjtag_init(cjtag_t *c, const cjtag_io_t *io)
{
    memset(c, 0, sizeof *c);
    c->io = *io;
    c->cur_dap_ir = 0xFFu;
}

// --- Layer 1: IEEE 1149.7 OScan1 physical signaling (decoded from XDS110 firmware_3.0.0.45.bin) ---

// Online-to-offline escape (0x151ec in XDS110): >=8 TMSC transitions while TCKC is held high resets the
// IEEE 1149.7 TAP adapter back to standard JScan0 mode.
void cjtag_reset_escape(cjtag_t *c)
{
    c->io.set_tms_dir(c->io.ctx, true);
    c->io.set_pins(c->io.ctx, true, false);
    c->io.delay_us(c->io.ctx, 2);
    for (int i = 0; i < 10; i++) {
        c->io.set_pins(c->io.ctx, true, true);
        c->io.delay_us(c->io.ctx, 1);
        c->io.set_pins(c->io.ctx, true, false);
        c->io.delay_us(c->io.ctx, 1);
    }
    c->io.delay_us(c->io.ctx, 5);
}

// Standard 2-pin JScan0 TMS clock sequence (0xf184 in XDS110): 1 TCKC clock per TMS bit (LSB-first).
static void jscan0_tms(cjtag_t *c, uint64_t bits, unsigned count)
{
    for (unsigned i = 0; i < count; i++) {
        bool tms = ((bits >> i) & 1u) != 0;
        c->io.set_pins(c->io.ctx, false, tms);
        if (i == 0) {
            c->io.set_tms_dir(c->io.ctx, true);
        }
        c->io.delay_us(c->io.ctx, 1);
        c->io.set_pins(c->io.ctx, true, tms);
        c->io.delay_us(c->io.ctx, 1);
    }
}

// OScan1 TMS-only navigation packet (0xf1c8 in XDS110, mode=7): 3 TCKC clocks per bit (!TDI=0, TMS, TDO).
// All TMSC level and direction changes occur strictly while TCKC = 0 so no escape edges occur while TCKC = 1.
static void oscan1_tms(cjtag_t *c, uint32_t bits, unsigned count)
{
    for (unsigned i = 0; i < count; i++) {
        bool tms = ((bits >> i) & 1u) != 0;
        // Clock 1: !TDI = 0 (TDI = 1)
        c->io.set_pins(c->io.ctx, false, false);
        c->io.set_tms_dir(c->io.ctx, true);
        c->io.delay_us(c->io.ctx, 1);
        c->io.set_pins(c->io.ctx, true, false);
        c->io.delay_us(c->io.ctx, 1);
        // Clock 2: TMS
        c->io.set_pins(c->io.ctx, false, tms);
        c->io.delay_us(c->io.ctx, 1);
        c->io.set_pins(c->io.ctx, true, tms);
        c->io.delay_us(c->io.ctx, 1);
        // Clock 3: TDO (tri-state TMSC while TCKC is low and keep tri-stated while TCKC is high)
        c->io.set_pins(c->io.ctx, false, tms);
        c->io.set_tms_dir(c->io.ctx, false);
        c->io.delay_us(c->io.ctx, 1);
        c->io.set_pins(c->io.ctx, true, tms);
        c->io.delay_us(c->io.ctx, 1);
    }
}

// OScan1 Shift-IR / Shift-DR packet (0xfd70 in XDS110): shifts count bits (1..64) LSB-first.
// When exit_tms is true, TMS=1 on the final bit to transition Shift-xR -> Exit1-xR.
static uint64_t oscan1_shift(cjtag_t *c, uint64_t tdi_bits, unsigned count, bool exit_tms)
{
    uint64_t rx = 0;
    for (unsigned i = 0; i < count; i++) {
        bool ntdi = ((tdi_bits >> i) & 1u) == 0;
        bool tms = (i + 1 == count) && exit_tms;
        // Clock 1: !TDI (inverted TDI)
        c->io.set_pins(c->io.ctx, false, ntdi);
        c->io.set_tms_dir(c->io.ctx, true);
        c->io.delay_us(c->io.ctx, 1);
        c->io.set_pins(c->io.ctx, true, ntdi);
        c->io.delay_us(c->io.ctx, 1);
        // Clock 2: TMS
        c->io.set_pins(c->io.ctx, false, tms);
        c->io.delay_us(c->io.ctx, 1);
        c->io.set_pins(c->io.ctx, true, tms);
        c->io.delay_us(c->io.ctx, 1);
        // Clock 3: TDO (target drives TMSC while TCKC is low; sample before rising edge)
        c->io.set_pins(c->io.ctx, false, tms);
        c->io.set_tms_dir(c->io.ctx, false);
        c->io.delay_us(c->io.ctx, 1);
        if (c->io.get_tms(c->io.ctx)) {
            rx |= (1ULL << i);
        }
        c->io.set_pins(c->io.ctx, true, tms);
        c->io.delay_us(c->io.ctx, 1);
    }
    return rx;
}

// XDS110 0x1113c (Dormant-to-JTAG wakeup sequence) before 0x10db2.
static void xds110_cjtag_reset_1113c(cjtag_t *c)
{
    static const uint8_t alert[16] = {
        0x92, 0xf3, 0x09, 0x62, 0x95, 0x2d, 0x85, 0x86, 0xe9, 0xaf, 0xdd, 0xe3, 0xa2, 0x0e, 0xbc, 0x19,
    };
    c->io.set_pins(c->io.ctx, false, true);
    c->io.set_tms_dir(c->io.ctx, false);
    for (int i = 0; i < 60; i++) {
        c->io.set_pins(c->io.ctx, false, true);
        c->io.delay_us(c->io.ctx, 1);
        c->io.set_pins(c->io.ctx, true, true);
        c->io.delay_us(c->io.ctx, 1);
    }
    c->io.set_pins(c->io.ctx, false, true);
    c->io.set_tms_dir(c->io.ctx, true);
    jscan0_tms(c, ~0ULL, 60);
    jscan0_tms(c, 0x33BBBBBAULL, 32);
    for (int i = 0; i < 16; i++) {
        jscan0_tms(c, alert[i], 8);
    }
    jscan0_tms(c, 0, 4);
    jscan0_tms(c, 0x0Au, 8);
    jscan0_tms(c, 0x00u, 8);
    jscan0_tms(c, ~0ULL, 60);
    jscan0_tms(c, 0, 8);
}

// XDS110 CJTAG_Connect(mode) (0x10db2..0x10fa4 in firmware_3.0.0.45.bin), supporting mode=7 (OScan1) and mode=8 (OScan0).
static void xds110_enter_mode(cjtag_t *c, int mode, bool with_escape)
{
    c->cjtag_mode = (uint8_t)mode;
    if (with_escape) {
        cjtag_reset_escape(c);
    }

    jscan0_tms(c, 0xFFu, 8);
    jscan0_tms(c, 0x00u, 1);
    jscan0_tms(c, 0x03u, 3);
    jscan0_tms(c, 0x00u, 64);
    jscan0_tms(c, 0x03u, 3);
    jscan0_tms(c, 0x05u, 4);
    jscan0_tms(c, 0x03u, 2);
    jscan0_tms(c, 0x05u, 4);
    jscan0_tms(c, 0x03u, 2);
    jscan0_tms(c, 0x05u, 4);
    jscan0_tms(c, 0x0Du, 4);
    jscan0_tms(c, 0x05u, 4);
    jscan0_tms(c, 0x0Du, 4);
    jscan0_tms(c, 0x05u, 4);
    jscan0_tms(c, 0x0Du, 4);
    jscan0_tms(c, 0x05u, 4);
    jscan0_tms(c, 0x31u, 6);
    jscan0_tms(c, 0x05u, 4);
    if (mode == 8) {
        jscan0_tms(c, 0x3801u, 14);
    } else {
        jscan0_tms(c, 0x1C01u, 13);
    }
    jscan0_tms(c, 0x00u, 4);

    if (mode == 8) {
        jscan0_tms(c, 0x02u, 3);
        jscan0_tms(c, 0x03u, 2);
        jscan0_tms(c, 0x15u, 6);
        oscan1_tms(c, 0x08u, 4);
        jscan0_tms(c, 0x01u, 2);
        jscan0_tms(c, 0x00u, 7);
        jscan0_tms(c, 0x05u, 4);
        jscan0_tms(c, 0x01u, 2);
        oscan1_tms(c, 0x40u, 7);
        jscan0_tms(c, 0x01u, 1);
        jscan0_tms(c, 0x05u, 4);
        jscan0_tms(c, 0x03u, 3);
        jscan0_tms(c, 0x00u, 7);
        jscan0_tms(c, 0x05u, 4);
        jscan0_tms(c, 0x03u, 2);
        jscan0_tms(c, 0x05u, 4);
        jscan0_tms(c, 0x01u, 2);
        oscan1_tms(c, 0x01u, 1);
        jscan0_tms(c, 0x01u, 2);
        jscan0_tms(c, 0x00u, 7);
    } else {
        oscan1_tms(c, 0x02u, 3);
        oscan1_tms(c, 0x03u, 2);
        oscan1_tms(c, 0x15u, 6);
        oscan1_tms(c, 0x08u, 4);
        oscan1_tms(c, 0x01u, 2);
        jscan0_tms(c, 0x00u, 7);
        oscan1_tms(c, 0x05u, 4);
        oscan1_tms(c, 0x01u, 2);
        oscan1_tms(c, 0x40u, 7);
        oscan1_tms(c, 0x01u, 1);
        oscan1_tms(c, 0x05u, 4);
        oscan1_tms(c, 0x03u, 3);
        jscan0_tms(c, 0x00u, 7);
        oscan1_tms(c, 0x05u, 4);
        oscan1_tms(c, 0x03u, 2);
        oscan1_tms(c, 0x05u, 4);
        oscan1_tms(c, 0x01u, 2);
        oscan1_tms(c, 0x01u, 1);
        oscan1_tms(c, 0x01u, 2);
        jscan0_tms(c, 0x00u, 7);
    }
}

// Exact XDS110 CJTAG_Connect(7) OScan1 activation sequence (0x10db2..0x10fa4 in firmware_3.0.0.45.bin).
static void oscan1_enter(cjtag_t *c)
{
    xds110_enter_mode(c, 7, true);
}

// Navigate TAP state machine outside Shift-DR/Shift-IR:
// mode=8 (OScan0/MScan) uses 1-clock JScan0 TMS packets outside Shift-xR; mode=7 (OScan1) uses 3-clock OScan1 TMS.
static void nav_tms(cjtag_t *c, uint32_t bits, unsigned count)
{
    if (c->cjtag_mode == 8) {
        jscan0_tms(c, bits, count);
    } else {
        oscan1_tms(c, bits, count);
    }
}

// --- Layer 2: TI ICEPick-C Router Unlock ---

// Shift 6-bit IR and N-bit DR on ICEPick-C alone without entering Run-Test/Idle between router commands
// (as required by OpenOCD target/ti/icepick.cfg lines 103-105).
static void icepick_ir_dr_no_idle(cjtag_t *c, bool from_idle, uint8_t ir6, uint32_t dr, unsigned dr_bits)
{
    // Enter Shift-IR from either Run-Test/Idle (1,1,0,0) or Update-DR (1,1,0,0)
    (void)from_idle;
    nav_tms(c, 0x03u, 4);
    oscan1_shift(c, ir6 & 0x3Fu, 6, true);
    // Exit1-IR -> Pause-IR (0) -> Exit2-IR (1) -> Update-IR (1) -> Select-DR (1) -> Capture-DR (0) -> Shift-DR (0)
    nav_tms(c, 0x0Eu, 6);
    oscan1_shift(c, dr, dr_bits, true);
    // Exit1-DR -> Pause-DR (0) -> Exit2-DR (1) -> Update-DR (1)
    nav_tms(c, 0x06u, 3);
}

// Shift 10-bit combined IR (CS_DAP 4 bits + ICEPick-C 6 bits in BYPASS) from Run-Test/Idle -> Run-Test/Idle.
static void chain_shift_ir(cjtag_t *c, uint8_t dap_ir)
{
    if (c->cur_dap_ir == dap_ir) {
        return;
    }
    uint32_t tx_ir = c->dap_at_tdo ? ((ICEPICK_IR_BYPASS << 4) | (dap_ir & 0x0Fu))
                                   : (((uint32_t)(dap_ir & 0x0Fu) << 6) | ICEPICK_IR_BYPASS);
    nav_tms(c, 0x03u, 4);  // Idle -> Select-DR (1), Select-IR (1), Capture-IR (0), Shift-IR (0)
    oscan1_shift(c, tx_ir, 10, true);
    nav_tms(c, 0x01u, 2);  // Exit1-IR -> Update-IR (1), Run-Test/Idle (0)
    c->cur_dap_ir = dap_ir;
}

// Shift (dap_bits + 1) combined DR (CS_DAP + 1-bit ICEPick-C bypass) from Run-Test/Idle -> Run-Test/Idle.
static uint64_t chain_shift_dr(cjtag_t *c, uint64_t dap_tx, unsigned dap_bits)
{
    uint64_t tx_dr = c->dap_at_tdo ? dap_tx : (dap_tx << 1);
    nav_tms(c, 0x01u, 3);  // Idle -> Select-DR (1), Capture-DR (0), Shift-DR (0)
    uint64_t rx_dr = oscan1_shift(c, tx_dr, dap_bits + 1, true);
    nav_tms(c, 0x01u, 2);  // Exit1-DR -> Update-DR (1), Run-Test/Idle (0)
    uint64_t mask = (dap_bits == 64) ? ~0ULL : ((1ULL << dap_bits) - 1ULL);
    return c->dap_at_tdo ? (rx_dr & mask) : ((rx_dr >> 1) & mask);
}

static bool icepick_connect_dap(cjtag_t *c)
{
    // Read 32-bit ICEPick-C IDCODE (if xds110_enter_mode left IR=BYPASS, shift ICEPICK_IR_IDCODE=0x04 first)
    nav_tms(c, 0x01u, 3);  // Idle -> Select-DR, Capture-DR, Shift-DR
    uint32_t idcode = (uint32_t)oscan1_shift(c, 0, 32, true);
    nav_tms(c, 0x01u, 2);  // Exit1-DR -> Update-DR, Idle

    if ((idcode & 0x0FFFFFFFu) != CJTAG_ICEPICK_IDCODE) {
        nav_tms(c, 0x03u, 4);  // Idle -> Select-DR, Select-IR, Capture-IR, Shift-IR
        oscan1_shift(c, ICEPICK_IR_IDCODE, 6, true);
        nav_tms(c, 0x01u, 2);  // Exit1-IR -> Update-IR, Idle
        nav_tms(c, 0x01u, 3);  // Idle -> Select-DR, Capture-DR, Shift-DR
        idcode = (uint32_t)oscan1_shift(c, 0, 32, true);
        nav_tms(c, 0x01u, 2);  // Exit1-DR -> Update-DR, Idle
    }

    c->icepick_id = idcode;
    if ((idcode & 0x0FFFFFFFu) != CJTAG_ICEPICK_IDCODE) {
        snprintf(c->error, sizeof c->error, "no CC1310 ICEPick-C IDCODE on probe pins (got 0x%08" PRIX32 ")", idcode);
        return false;
    }

    // Unlock ICEPick-C and link Debug TAP 0 (Cortex-M3 CS_DAP) without touching Run-Test/Idle on the way
    icepick_ir_dr_no_idle(c, true, ICEPICK_IR_CONNECT, 0x89u, 8);
    icepick_ir_dr_no_idle(c, false, ICEPICK_IR_ROUTER, 0x81002100u, 32);  // SysCtrl: force power/clock active
    icepick_ir_dr_no_idle(c, false, ICEPICK_IR_ROUTER, 0xA0110048u, 32);  // TAP 0: power & clock enable
    icepick_ir_dr_no_idle(c, false, ICEPICK_IR_ROUTER, 0xA0112048u, 32);  // TAP 0: debug enable
    icepick_ir_dr_no_idle(c, false, ICEPICK_IR_ROUTER, 0xA0112148u, 32);  // TAP 0: select & link into chain

    // From Update-DR, load ICEPick BYPASS (0x3F) and enter Run-Test/Idle for 10 clocks
    nav_tms(c, 0x03u, 4);  // Update-DR -> Select-DR (1), Select-IR (1), Capture-IR (0), Shift-IR (0)
    oscan1_shift(c, ICEPICK_IR_BYPASS, 6, true);
    nav_tms(c, 0x01u, 2);   // Exit1-IR -> Update-IR (1), Run-Test/Idle (0)
    nav_tms(c, 0x00u, 10);  // 10 Run-Test/Idle clocks

    // Auto-detect whether CS_DAP (32-bit IDCODE = 0x4BA00477) or ICEPick-C (1-bit bypass) is closest to TDO
    nav_tms(c, 0x01u, 3);
    uint64_t raw33 = oscan1_shift(c, 0, 33, true);
    nav_tms(c, 0x01u, 2);

    if ((uint32_t)((raw33 >> 1) & 0xFFFFFFFFULL) == CJTAG_DAP_IDCODE) {
        c->dap_at_tdo = false;
        c->dap_id = CJTAG_DAP_IDCODE;
        c->cur_dap_ir = DAP_IR_IDCODE;
        return true;
    }
    if ((uint32_t)(raw33 & 0xFFFFFFFFULL) == CJTAG_DAP_IDCODE) {
        c->dap_at_tdo = true;
        c->dap_id = CJTAG_DAP_IDCODE;
        c->cur_dap_ir = DAP_IR_IDCODE;
        return true;
    }

    // Try explicit DAP_IR_IDCODE under both chain orders
    for (int order = 0; order < 2; order++) {
        c->dap_at_tdo = (order == 1);
        c->cur_dap_ir = 0xFFu;
        chain_shift_ir(c, DAP_IR_IDCODE);
        uint32_t id = (uint32_t)chain_shift_dr(c, 0, 32);
        if (id == CJTAG_DAP_IDCODE) {
            c->dap_id = id;
            return true;
        }
    }
    snprintf(c->error, sizeof c->error,
             "ICEPick (0x%08" PRIX32 ") connected, but CS_DAP IDCODE not found (raw33=0x%09" PRIX64 ")", idcode, raw33);
    return false;
}

// --- Layer 3: ARM CoreSight JTAG-DP & AHB-AP ---

// Perform one 35-bit DPACC or APACC scan with WAIT retry.
// addr is byte address (bits [3:2] placed in DR[2:1], rnw in DR[0], 32-bit data in DR[34:3]).
static bool dap_scan35(cjtag_t *c, uint8_t ir, uint8_t addr, bool rnw, uint32_t tx_data, uint32_t *rx_data)
{
    chain_shift_ir(c, ir);
    uint64_t req = ((uint64_t)tx_data << 3) | (uint64_t)(addr & 0x0Cu) >> 1 | (rnw ? 1ULL : 0ULL);
    for (int tries = 0; tries < 64; tries++) {
        uint64_t resp = chain_shift_dr(c, req, 35);
        uint8_t ack = (uint8_t)(resp & 0x07u);
        if (ack == DAP_ACK_OK) {
            if (rx_data) {
                *rx_data = (uint32_t)(resp >> 3);
            }
            return true;
        }
        if (ack != DAP_ACK_WAIT) {
            // Clear sticky error bits via ABORT (DAPABORT=1, STKERRCLR=1)
            chain_shift_ir(c, DAP_IR_ABORT);
            chain_shift_dr(c, 0x04u, 35);
            return fail(c, "CoreSight JTAG-DP ACK fault");
        }
    }
    return fail(c, "CoreSight JTAG-DP WAIT timeout");
}

static bool dp_write(cjtag_t *c, uint8_t reg_addr, uint32_t val)
{
    return dap_scan35(c, DAP_IR_DPACC, reg_addr, false, val, NULL);
}

static bool dp_read(cjtag_t *c, uint8_t reg_addr, uint32_t *val)
{
    // Pipelined JTAG-DP read: first scan initiates the read, second scan (RDBUFF) shifts out the result
    return dap_scan35(c, DAP_IR_DPACC, reg_addr, true, 0, NULL) && dap_scan35(c, DAP_IR_DPACC, DP_RDBUFF, true, 0, val);
}

static bool ap_write(cjtag_t *c, uint8_t reg_addr, uint32_t val)
{
    return dap_scan35(c, DAP_IR_APACC, reg_addr, false, val, NULL);
}

static bool ap_read(cjtag_t *c, uint8_t reg_addr, uint32_t *val)
{
    return dap_scan35(c, DAP_IR_APACC, reg_addr, true, 0, NULL) && dap_scan35(c, DAP_IR_DPACC, DP_RDBUFF, true, 0, val);
}

static bool set_ap_csw(cjtag_t *c, uint32_t csw)
{
    if (c->ap_csw == csw) {
        return true;
    }
    if (!ap_write(c, AP_CSW, csw)) {
        return false;
    }
    c->ap_csw = csw;
    return true;
}

static bool dap_power_up(cjtag_t *c)
{
    // Clear any sticky errors in CTRL/STAT
    chain_shift_ir(c, DAP_IR_ABORT);
    chain_shift_dr(c, 0x1Eu, 35);

    // Request debug and system power-up (CDBGPWRUPREQ | CSYSPWRUPREQ = 0x50000000)
    if (!dp_write(c, DP_CTRL_STAT, 0x50000000u)) {
        return false;
    }
    uint32_t stat = 0;
    for (int i = 0; i < 50; i++) {
        if (!dp_read(c, DP_CTRL_STAT, &stat)) {
            return false;
        }
        if ((stat & 0xF0000000u) == 0xF0000000u) {
            break;
        }
        c->io.delay_us(c->io.ctx, 100);
    }
    if ((stat & 0xF0000000u) != 0xF0000000u) {
        return fail(c, "JTAG-DP debug/system power-up ACK timeout");
    }

    // Select AP 0 (Cortex-M3 AHB-AP), bank 0
    if (!dp_write(c, DP_SELECT, 0x00000000u)) {
        return false;
    }
    c->ap_csw = 0;
    return set_ap_csw(c, AP_CSW_32BIT_NOINC);
}

bool cjtag_mem_read32(cjtag_t *c, uint32_t addr, uint32_t *val)
{
    return set_ap_csw(c, AP_CSW_32BIT_NOINC) && ap_write(c, AP_TAR, addr) && ap_read(c, AP_DRW, val);
}

bool cjtag_mem_write32(cjtag_t *c, uint32_t addr, uint32_t val)
{
    // Follow AP_DRW write with a dummy DP_RDBUFF read to ensure the AHB write completes before returning
    uint32_t dummy = 0;
    return set_ap_csw(c, AP_CSW_32BIT_NOINC) && ap_write(c, AP_TAR, addr) && ap_write(c, AP_DRW, val) &&
           dap_scan35(c, DAP_IR_DPACC, DP_RDBUFF, true, 0, &dummy);
}

// Pipelined block read using AHB-AP single auto-increment (wraps at 1 KB TAR boundaries).
bool cjtag_mem_read_block(cjtag_t *c, uint32_t addr, uint8_t *out, size_t len)
{
    if ((addr & 3u) != 0 || (len & 3u) != 0) {
        return fail(c, "unaligned block read");
    }
    if (!set_ap_csw(c, AP_CSW_32BIT_INC)) {
        return false;
    }
    size_t off = 0;
    while (off < len) {
        uint32_t cur = addr + (uint32_t)off;
        // Do not cross a 1 KB (0x400) TAR auto-increment boundary in a single burst
        size_t chunk_max = 0x400u - (cur & 0x3FFu);
        size_t chunk = (len - off < chunk_max) ? (len - off) : chunk_max;
        size_t words = chunk / 4u;

        if (!ap_write(c, AP_TAR, cur)) {
            return false;
        }
        // Prime the pipeline with the first AP_DRW read
        if (!dap_scan35(c, DAP_IR_APACC, AP_DRW, true, 0, NULL)) {
            return false;
        }
        for (size_t w = 0; w < words; w++) {
            uint32_t word = 0;
            bool ok = (w + 1 < words) ? dap_scan35(c, DAP_IR_APACC, AP_DRW, true, 0, &word)
                                      : dap_scan35(c, DAP_IR_DPACC, DP_RDBUFF, true, 0, &word);
            if (!ok) {
                return false;
            }
            out[off + w * 4 + 0] = (uint8_t)word;
            out[off + w * 4 + 1] = (uint8_t)(word >> 8);
            out[off + w * 4 + 2] = (uint8_t)(word >> 16);
            out[off + w * 4 + 3] = (uint8_t)(word >> 24);
        }
        off += chunk;
    }
    return true;
}

bool cjtag_mem_write_block(cjtag_t *c, uint32_t addr, const uint8_t *data, size_t len)
{
    if ((addr & 3u) != 0 || (len & 3u) != 0) {
        return fail(c, "unaligned block write");
    }
    if (!set_ap_csw(c, AP_CSW_32BIT_INC)) {
        return false;
    }
    size_t off = 0;
    while (off < len) {
        uint32_t cur = addr + (uint32_t)off;
        size_t chunk_max = 0x400u - (cur & 0x3FFu);
        size_t chunk = (len - off < chunk_max) ? (len - off) : chunk_max;
        size_t words = chunk / 4u;

        if (!ap_write(c, AP_TAR, cur)) {
            return false;
        }
        for (size_t w = 0; w < words; w++) {
            const uint8_t *p = data + off + w * 4;
            uint32_t word = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
            if (!ap_write(c, AP_DRW, word)) {
                return false;
            }
        }
        uint32_t dummy = 0;
        if (!dap_scan35(c, DAP_IR_DPACC, DP_RDBUFF, true, 0, &dummy)) {
            return false;
        }
        off += chunk;
    }
    return true;
}

// --- Layer 4: Cortex-M3 Halt, Core Registers & CC1310 Flash Engine ---

static bool cpu_halt(cjtag_t *c)
{
    if (!cjtag_mem_write32(c, CM3_DHCSR, DHCSR_HALT)) {
        return false;
    }
    for (int i = 0; i < 50; i++) {
        uint32_t dhcsr = 0;
        if (!cjtag_mem_read32(c, CM3_DHCSR, &dhcsr)) {
            return false;
        }
        if ((dhcsr & DHCSR_SHALT) != 0) {
            return true;
        }
        c->io.delay_us(c->io.ctx, 50);
    }
    return fail(c, "Cortex-M3 failed to halt");
}

static bool cpu_write_reg(cjtag_t *c, uint8_t reg, uint32_t val)
{
    if (!cjtag_mem_write32(c, CM3_DCRDR, val) || !cjtag_mem_write32(c, CM3_DCRSR, 0x00010000u | reg)) {
        return false;
    }
    for (int i = 0; i < 50; i++) {
        uint32_t dhcsr = 0;
        if (!cjtag_mem_read32(c, CM3_DHCSR, &dhcsr)) {
            return false;
        }
        if ((dhcsr & DHCSR_SRDY) != 0) {
            return true;
        }
    }
    return fail(c, "Cortex-M3 DCRSR write timeout");
}

static bool cpu_read_reg(cjtag_t *c, uint8_t reg, uint32_t *val)
{
    if (!cjtag_mem_write32(c, CM3_DCRSR, reg)) {
        return false;
    }
    for (int i = 0; i < 50; i++) {
        uint32_t dhcsr = 0;
        if (!cjtag_mem_read32(c, CM3_DHCSR, &dhcsr)) {
            return false;
        }
        if ((dhcsr & DHCSR_SRDY) != 0) {
            return cjtag_mem_read32(c, CM3_DCRDR, val);
        }
    }
    return fail(c, "Cortex-M3 DCRSR read timeout");
}

// Call a CC1310 ROM Hard-API function on the halted Cortex-M3 with interrupts disabled and a bkpt #0 trampoline
// at 0x20000000 in SRAM, then wait for the CPU to hit the breakpoint and return R0.
static bool rom_hapi_call(cjtag_t *c, uint32_t ptr_addr, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t timeout_ms,
                          uint32_t *ret_r0)
{
    uint32_t func_addr = 0;
    if (!cjtag_mem_read32(c, ptr_addr, &func_addr) || (func_addr & 0xFFF00001u) != 0x10000001u) {
        return fail(c, "invalid CC1310 ROM Hard-API pointer");
    }
    // Write `bkpt #0; nop` (0xBE00, 0xBF00 -> 0xBF00BE00 little-endian) at 0x20000000
    if (!cjtag_mem_write32(c, CC1310_SRAM_TRAMP, 0xBF00BE00u)) {
        return false;
    }
    // Set PRIMASK=1 (reg 20), SP, LR=0x20000001 (bkpt #0 in Thumb), xPSR=0x01000000 (Thumb), R0..R2, PC=func_addr
    if (!cpu_write_reg(c, 20, 0x00000001u) || !cpu_write_reg(c, 13, CC1310_SRAM_SP) ||
        !cpu_write_reg(c, 14, CC1310_SRAM_TRAMP | 1u) || !cpu_write_reg(c, 16, 0x01000000u) ||
        !cpu_write_reg(c, 0, r0) || !cpu_write_reg(c, 1, r1) || !cpu_write_reg(c, 2, r2) ||
        !cpu_write_reg(c, 15, func_addr)) {
        return false;
    }
    // Resume Cortex-M3 (C_DEBUGEN=1, C_HALT=0)
    if (!cjtag_mem_write32(c, CM3_DHCSR, DHCSR_RUN)) {
        return false;
    }
    uint32_t loops = timeout_ms * 10u;
    for (uint32_t i = 0; i <= loops; i++) {
        uint32_t dhcsr = 0;
        if (!cjtag_mem_read32(c, CM3_DHCSR, &dhcsr)) {
            return false;
        }
        if ((dhcsr & DHCSR_SHALT) != 0) {
            return cpu_read_reg(c, 0, ret_r0);
        }
        c->io.delay_us(c->io.ctx, 100);
    }
    (void)cpu_halt(c);
    return fail(c, "CC1310 ROM Hard-API call timed out");
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

bool cjtag_connect(cjtag_t *c, bool halt_cpu)
{
    c->error[0] = 0;
    c->cur_dap_ir = 0xFFu;
    c->ap_csw = 0;
    uint32_t ids[4] = {0};
    char icepick_err[96] = {0};
    oscan1_enter(c);
    bool connected = icepick_connect_dap(c);
    ids[0] = c->icepick_id;
    if (!connected && (ids[0] & 0x0FFFFFFFu) == CJTAG_ICEPICK_IDCODE) {
        snprintf(icepick_err, sizeof icepick_err, "%s", c->error);
    }
    if (!connected) {
        xds110_cjtag_reset_1113c(c);
        xds110_enter_mode(c, 7, false);
        connected = icepick_connect_dap(c);
        ids[1] = c->icepick_id;
        if (!connected && (ids[1] & 0x0FFFFFFFu) == CJTAG_ICEPICK_IDCODE) {
            snprintf(icepick_err, sizeof icepick_err, "%s", c->error);
        }
    }
    if (!connected) {
        xds110_enter_mode(c, 8, true);
        connected = icepick_connect_dap(c);
        ids[2] = c->icepick_id;
        if (!connected && (ids[2] & 0x0FFFFFFFu) == CJTAG_ICEPICK_IDCODE) {
            snprintf(icepick_err, sizeof icepick_err, "%s", c->error);
        }
    }
    if (!connected) {
        xds110_cjtag_reset_1113c(c);
        xds110_enter_mode(c, 8, false);
        connected = icepick_connect_dap(c);
        ids[3] = c->icepick_id;
        if (!connected && (ids[3] & 0x0FFFFFFFu) == CJTAG_ICEPICK_IDCODE) {
            snprintf(icepick_err, sizeof icepick_err, "%s", c->error);
        }
    }
    if (!connected) {
        if (icepick_err[0] != 0) {
            snprintf(c->error, sizeof c->error, "%s", icepick_err);
        } else {
            snprintf(c->error, sizeof c->error,
                     "no IDCODE (m7=0x%08" PRIX32 " r7=0x%08" PRIX32 " m8=0x%08" PRIX32 " r8=0x%08" PRIX32 ")", ids[0],
                     ids[1], ids[2], ids[3]);
        }
        return false;
    }
    if (!dap_power_up(c)) {
        return false;
    }
    if (halt_cpu && !cpu_halt(c)) {
        return false;
    }

    // Read FCFG1 USER_ID (0x50001294) and 64-bit IEEE 802.15.4 MAC (0x500012F0..0x500012F4)
    uint32_t mac_lo = 0, mac_hi = 0;
    if (!cjtag_mem_read32(c, 0x50001294u, &c->user_id) || !cjtag_mem_read32(c, 0x500012F0u, &mac_lo) ||
        !cjtag_mem_read32(c, 0x500012F4u, &mac_hi)) {
        return fail(c, "failed to read FCFG1 identification registers");
    }
    c->ieee_mac = ((uint64_t)mac_hi << 32) | mac_lo;

    // Page 31 (CCFG): its baseline CRC32, BL_CONFIG and the CCFG's ASCII MAC
    if (!page_crc(c, CJTAG_CCFG_PAGE_IDX * CJTAG_PAGE, &c->page31_crc)) {
        return fail(c, "failed to read page 31 (CCFG)");
    }
    uint8_t ccfg[CJTAG_CCFG_SIZE];
    if (!cjtag_mem_read_block(c, CJTAG_CCFG_ADDR, ccfg, sizeof ccfg)) {
        return fail(c, "failed to read the CCFG");
    }
    const uint8_t *bl = ccfg + CJTAG_CCFG_BL_CONFIG;
    c->bl_config = (uint32_t)bl[0] | ((uint32_t)bl[1] << 8) | ((uint32_t)bl[2] << 16) | ((uint32_t)bl[3] << 24);

    const uint8_t *ccfg_mac = ccfg + 0x24;  // 0x0001FFCC
    if (is_ascii_hex_mac(ccfg_mac)) {
        memcpy(c->mac, ccfg_mac, 8);
        c->mac[8] = 0;
        c->has_ccfg_mac = true;
    } else {
        c->has_ccfg_mac = false;
        // Stock v2 climate/motion sensors store their 8-char ASCII MAC at 0x0001D000
        uint8_t p29_mac[8];
        if (cjtag_mem_read_block(c, CJTAG_PAGE29_MAC_ADDR, p29_mac, sizeof p29_mac) && is_ascii_hex_mac(p29_mac)) {
            memcpy(c->mac, p29_mac, 8);
            c->mac[8] = 0;
        } else {
            snprintf(c->mac, sizeof c->mac, "%08X", (unsigned)mac_lo);
        }
    }
    return true;
}

bool cjtag_dump_page(cjtag_t *c, uint32_t page_idx, uint8_t *out_4k)
{
    if (page_idx >= CJTAG_TOTAL_PAGES) {
        return fail(c, "page index out of range (0..31)");
    }
    uint32_t addr = page_idx * CJTAG_PAGE;
    bool equal = false;
    if (!cjtag_mem_read_block(c, addr, out_4k, CJTAG_PAGE) || !flash_equals(c, addr, out_4k, CJTAG_PAGE, &equal)) {
        return false;
    }
    if (!equal) {
        return fail(c, "page read mismatch between pass 1 and pass 2 (probe glitch)");
    }
    return true;
}

bool cjtag_dump_fcfg1(cjtag_t *c, uint8_t *out_1k)
{
    bool equal = false;
    if (!cjtag_mem_read_block(c, CJTAG_FCFG1_ADDR, out_1k, CJTAG_FCFG1_SIZE) ||
        !flash_equals(c, CJTAG_FCFG1_ADDR, out_1k, CJTAG_FCFG1_SIZE, &equal)) {
        return false;
    }
    if (!equal) {
        return fail(c, "FCFG1 read mismatch between pass 1 and pass 2");
    }
    return true;
}

static bool vector_table_ok(const uint8_t *p)
{
    uint32_t sp = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    uint32_t reset = (uint32_t)p[4] | ((uint32_t)p[5] << 8) | ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
    return sp > 0x20000000u && sp <= 0x20005000u && (reset & 1u) != 0 && reset < CJTAG_APP_END;
}

static bool is_all_ff(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (p[i] != 0xFFu) {
            return false;
        }
    }
    return true;
}

// Erase and program one page and verify it whole. No refusals here: cjtag_flash_page (pages 0-30) and
// cjtag_set_backdoor (page 31, BL_CONFIG only) decide what may be written.
static bool flash_raw(cjtag_t *c, uint32_t page_idx, const uint8_t *data_4k)
{
    if (!cpu_halt(c)) {
        return false;
    }

    // Disable VIMS flash cache (MODE = OFF = 0x3) so flash reads and ROM flash operations see raw flash state
    uint32_t orig_vims = 0;
    if (!cjtag_mem_read32(c, CC1310_VIMS_CTL, &orig_vims) ||
        !cjtag_mem_write32(c, CC1310_VIMS_CTL, (orig_vims & ~3u) | 3u)) {
        return false;
    }

    // Force flash trim before erase/program in case the chip was halted early in boot (cc26x0_xds.gel)
    uint32_t fwflag = 0;
    if (!cjtag_mem_write32(c, CC1310_FLASH_FWLOCK, 0x00000005u) || !cjtag_mem_read32(c, CC1310_FLASH_FWFLAG, &fwflag) ||
        !cjtag_mem_write32(c, CC1310_FLASH_FWFLAG, fwflag & ~1u) ||
        !cjtag_mem_write32(c, CC1310_FLASH_FWLOCK, 0x00000000u)) {
        (void)cjtag_mem_write32(c, CC1310_VIMS_CTL, orig_vims);
        return false;
    }

    uint32_t sector_addr = page_idx * CJTAG_PAGE;
    uint32_t status = 0xFFFFFFFFu;
    if (!rom_hapi_call(c, CC1310_ROM_HAPI_ERASE, sector_addr, 0, 0, 500, &status) || status != 0) {
        (void)cjtag_mem_write32(c, CC1310_VIMS_CTL, orig_vims);
        return fail(c, "CC1310 HapiSectorErase failed");
    }

    if (!is_all_ff(data_4k, CJTAG_PAGE)) {
        if (!cjtag_mem_write_block(c, CC1310_SRAM_BUF, data_4k, CJTAG_PAGE)) {
            (void)cjtag_mem_write32(c, CC1310_VIMS_CTL, orig_vims);
            return fail(c, "failed to load 4 KB page buffer into CC1310 SRAM");
        }
        status = 0xFFFFFFFFu;
        if (!rom_hapi_call(c, CC1310_ROM_HAPI_PROG, CC1310_SRAM_BUF, sector_addr, CJTAG_PAGE, 500, &status) ||
            status != 0) {
            (void)cjtag_mem_write32(c, CC1310_VIMS_CTL, orig_vims);
            return fail(c, "CC1310 HapiProgramFlash failed");
        }
    }

    // Clear FLASH.CFG.DIS_STANDBY (bit 1) as driverlib FlashSectorErase/FlashProgram do
    uint32_t fcfg = 0;
    if (cjtag_mem_read32(c, CC1310_FLASH_CFG, &fcfg)) {
        (void)cjtag_mem_write32(c, CC1310_FLASH_CFG, fcfg & ~0x02u);
    }

    // Read back the page while the VIMS cache is still OFF and verify every byte matches
    bool equal = false;
    if (!flash_equals(c, sector_addr, data_4k, CJTAG_PAGE, &equal) || !equal) {
        (void)cjtag_mem_write32(c, CC1310_VIMS_CTL, orig_vims);
        return fail(c, "flash readback verification failed");
    }

    // Restore original VIMS cache mode
    (void)cjtag_mem_write32(c, CC1310_VIMS_CTL, orig_vims);
    return true;
}

bool cjtag_flash_page(cjtag_t *c, uint32_t page_idx, const uint8_t *data_4k, bool allow_nv)
{
    // Hard safety rule: NEVER erase or write page 31 (CCFG + factory MAC) here
    if (page_idx >= CJTAG_CCFG_PAGE_IDX) {
        return fail(c, "refused: page 31 (CCFG) may never be erased or written");
    }
    if (page_idx == CJTAG_NV_PAGE_IDX && !allow_nv) {
        return fail(c, "refused: page 30 (NV) requires allow_nv");
    }
    if (!c->has_ccfg_mac) {
        return fail(c, "no valid ASCII MAC in CCFG (page 31): refusing to flash");
    }
    if (page_idx == 0 && !vector_table_ok(data_4k)) {
        return fail(c, "page 0 does not start with a valid CC1310 Cortex-M3 vector table");
    }
    if (!flash_raw(c, page_idx, data_4k)) {
        return false;
    }
    // Verify page 31 (CCFG) is completely untouched
    uint32_t crc = 0;
    if (!page_crc(c, CJTAG_CCFG_PAGE_IDX * CJTAG_PAGE, &crc) || crc != c->page31_crc) {
        return fail(c, "page 31 CRC changed!");
    }
    return true;
}

int cjtag_write_image(cjtag_t *c, cjtag_image_fn image, void *ctx, uint32_t image_len, uint8_t *buf_4k)
{
    int written = 0;
    c->written_pages[0] = 0;
    if (!c->has_ccfg_mac) {
        fail(c, "no factory MAC in its CCFG: not written");
        return -1;
    }
    bool has_p31_code = image_len > CJTAG_PAGE31_CODE_LEN && (image_len - CJTAG_PAGE31_CODE_LEN) % CJTAG_PAGE == 0 &&
                        (image_len - CJTAG_PAGE31_CODE_LEN) <= CJTAG_APP_END;
    uint32_t app_len = has_p31_code ? image_len - CJTAG_PAGE31_CODE_LEN : image_len;
    if (!app_len || app_len > CJTAG_APP_END) {
        fail(c, "image size out of range (pages 0-29 only)");
        return -1;
    }
    for (uint32_t p = 0; p * CJTAG_PAGE < app_len; p++) {
        uint32_t n = app_len - p * CJTAG_PAGE < CJTAG_PAGE ? app_len - p * CJTAG_PAGE : CJTAG_PAGE;
        bool equal = false;
        memset(buf_4k, 0xFF, CJTAG_PAGE);
        if (!image(ctx, p * CJTAG_PAGE, buf_4k, n)) {
            snprintf(c->error, sizeof c->error, "page %lu: image read failed", (unsigned long)p);
            return -1;
        }
        if (!flash_equals(c, p * CJTAG_PAGE, buf_4k, CJTAG_PAGE, &equal)) {
            return -1;
        }
        if (equal) {
            continue;  // already there
        }
        // the pages written, for the caller's log ("3 5-7"): c->error holds them when the write succeeds
        size_t el = strlen(c->written_pages);
        if (el + 6 < sizeof c->written_pages) {
            snprintf(c->written_pages + el, sizeof c->written_pages - el, "%s%lu", el ? " " : "", (unsigned long)p);
        }
        if (!cjtag_flash_page(c, p, buf_4k, false)) {
            char why[sizeof c->error];
            snprintf(why, sizeof why, "%s", c->error);
            snprintf(c->error, sizeof c->error, "page %lu: %.70s", (unsigned long)p, why);
            return -1;
        }
        written++;
    }
    if (has_p31_code) {
        // the page-31 code (a stock image's TI-RTOS part) through the one guarded writer: the unit's CCFG kept byte
        // for byte, ccfg_safe, page 31 put back as it was if the write fails
        static uint8_t orig[CJTAG_PAGE];
        if (!image(ctx, app_len, buf_4k, CJTAG_PAGE31_CODE_LEN)) {
            fail(c, "page 31 code: image read failed");
            return -1;
        }
        int w = cjtag_write_page31_code(c, buf_4k, buf_4k, orig);
        if (w < 0) {
            return -1;
        }
        size_t el = strlen(c->written_pages);
        if (w > 0 && el + 4 < sizeof c->written_pages) {
            snprintf(c->written_pages + el, sizeof c->written_pages - el, "%s31", el ? " " : "");
        }
        written += w;
    }
    return written;
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put_le32_(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

// Programs n bytes (a multiple of 4) at addr from data, with no erase: flash bits only go from 1 to 0
static bool program_only(cjtag_t *c, uint32_t addr, const uint8_t *data, uint32_t n)
{
    if (!cpu_halt(c)) {
        return false;
    }
    uint32_t orig_vims = 0;
    if (!cjtag_mem_read32(c, CC1310_VIMS_CTL, &orig_vims) ||
        !cjtag_mem_write32(c, CC1310_VIMS_CTL, (orig_vims & ~3u) | 3u)) {
        return false;
    }
    uint32_t fwflag = 0;
    bool ok =
        cjtag_mem_write32(c, CC1310_FLASH_FWLOCK, 0x00000005u) && cjtag_mem_read32(c, CC1310_FLASH_FWFLAG, &fwflag) &&
        cjtag_mem_write32(c, CC1310_FLASH_FWFLAG, fwflag & ~1u) &&
        cjtag_mem_write32(c, CC1310_FLASH_FWLOCK, 0x00000000u) && cjtag_mem_write_block(c, CC1310_SRAM_BUF, data, n);
    uint32_t status = 0xFFFFFFFFu;
    if (ok && (!rom_hapi_call(c, CC1310_ROM_HAPI_PROG, CC1310_SRAM_BUF, addr, n, 500, &status) || status != 0)) {
        ok = fail(c, "CC1310 HapiProgramFlash failed");
    }
    uint32_t fcfg = 0;
    if (cjtag_mem_read32(c, CC1310_FLASH_CFG, &fcfg)) {
        (void)cjtag_mem_write32(c, CC1310_FLASH_CFG, fcfg & ~0x02u);
    }
    (void)cjtag_mem_write32(c, CC1310_VIMS_CTL, orig_vims);
    return ok;
}

// docs/design/ccfg-rule.md: the CCFG about to be written (now) against the chip's (was). Never: a debug port off
// (CCFG_TAP_DAP_0/1 unchanged, the CPU's on), IMAGE_VALID_CONF other than 0 or changed, a protection bit or an erase
// lock (CCFG_PROT_*, ERASE_CONF unchanged), a bootloader that was on turned off. Only what the caller may change
// (CCFG_MAY_*) may differ at all.
enum { CCFG_MAY_BL = 1, CCFG_MAY_MAC = 2 };

static bool ccfg_safe(cjtag_t *c, const uint8_t *was, const uint8_t *now, unsigned may)
{
    for (uint32_t o = 0; o < CJTAG_CCFG_SIZE; o++) {
        bool bl = o >= CJTAG_CCFG_BL_CONFIG && o < CJTAG_CCFG_BL_CONFIG + 4;
        bool mac = o >= CJTAG_CCFG_MAC && o < CJTAG_CCFG_MAC + 8;
        if (now[o] != was[o] && !(bl && (may & CCFG_MAY_BL)) && !(mac && (may & CCFG_MAY_MAC))) {
            return fail(c, "CCFG rule: only the field being written may change");
        }
    }
    if (((le32(now + CJTAG_CCFG_TAP_DAP_0) >> 16) & 0xFFu) != 0xC5u) {
        return fail(c, "CCFG rule: the CPU debug port must stay on");
    }
    if (le32(now + CJTAG_CCFG_IMAGE_VALID) != 0) {
        return fail(c, "CCFG rule: IMAGE_VALID_CONF must be 0");
    }
    uint32_t bw = le32(was + CJTAG_CCFG_BL_CONFIG), bn = le32(now + CJTAG_CCFG_BL_CONFIG);
    if ((bw & 0xFF0000FFu) == 0xC50000C5u && (bn & 0xFF0000FFu) != 0xC50000C5u) {
        return fail(c, "CCFG rule: a bootloader that is on stays on");
    }
    return true;
}

int cjtag_fill_ccfg_mac(cjtag_t *c, const char *mac, uint8_t *buf_4k)
{
    const uint32_t slot = CJTAG_CCFG_ADDR + CJTAG_CCFG_MAC;
    const uint32_t slot_off = slot - CJTAG_CCFG_PAGE_IDX * CJTAG_PAGE;
    uint8_t m[8], page29[8];
    if (!mac || strlen(mac) != 8) {
        fail(c, "the MAC must be 8 hex digits");
        return -1;
    }
    for (int i = 0; i < 8; i++) {
        m[i] = (uint8_t)(mac[i] >= 'a' && mac[i] <= 'f' ? mac[i] - 32 : mac[i]);
    }
    if (!is_ascii_hex_mac(m)) {
        fail(c, "the MAC must be 8 hex digits");
        return -1;
    }
    if (!cjtag_dump_page(c, CJTAG_CCFG_PAGE_IDX, buf_4k)) {
        return -1;
    }
    if (!memcmp(buf_4k + slot_off, m, 8)) {
        return 0;  // already there
    }
    if (!is_all_ff(buf_4k + slot_off, 8)) {
        fail(c, "the CCFG MAC slot is not blank: not writing (only a blank slot is filled, never erased)");
        return -1;
    }
    if (!cjtag_mem_read_block(c, CJTAG_PAGE29_MAC_ADDR, page29, sizeof page29) || memcmp(page29, m, 8) != 0) {
        fail(c, "the MAC is not the one in page 29 (0x1D000): not writing");
        return -1;
    }
    const uint32_t ccfg_off = CJTAG_CCFG_ADDR - CJTAG_CCFG_PAGE_IDX * CJTAG_PAGE;
    uint8_t now[CJTAG_CCFG_SIZE];
    memcpy(now, buf_4k + ccfg_off, sizeof now);
    memcpy(now + CJTAG_CCFG_MAC, m, 8);
    if (!ccfg_safe(c, buf_4k + ccfg_off, now, CCFG_MAY_MAC)) {
        return -1;
    }
    if (!program_only(c, slot, m, 8)) {
        return -1;
    }
    memcpy(buf_4k + slot_off, m, 8);  // the page as it must be now: only the slot changed
    bool equal = false;
    if (!flash_equals(c, CJTAG_CCFG_PAGE_IDX * CJTAG_PAGE, buf_4k, CJTAG_PAGE, &equal) || !equal) {
        fail(c, "page 31 read-back after the MAC fill differs");
        return -1;
    }
    memcpy(c->mac, m, 8);
    c->mac[8] = 0;
    c->has_ccfg_mac = true;
    c->page31_crc = ~crc32_update(0xFFFFFFFFu, buf_4k, CJTAG_PAGE);
    return 1;
}

int cjtag_set_backdoor(cjtag_t *c, uint8_t pin, uint8_t *buf_4k)
{
    const uint32_t ccfg_off = CJTAG_CCFG_ADDR - CJTAG_CCFG_PAGE_IDX * CJTAG_PAGE;
    if (pin > CJTAG_MAX_BL_PIN) {
        fail(c, "backdoor pin out of range (DIO0-14 on the 5x5 package)");
        return -1;
    }
    if (!c->has_ccfg_mac) {
        fail(c, "no factory MAC in its CCFG: not touching page 31");
        return -1;
    }
    // the page read twice (compared), and the same as at the connect
    uint32_t crc = 0;
    if (!cjtag_dump_page(c, CJTAG_CCFG_PAGE_IDX, buf_4k)) {
        return -1;
    }
    crc = ~crc32_update(0xFFFFFFFFu, buf_4k, CJTAG_PAGE);
    if (crc != c->page31_crc) {
        fail(c, "page 31 changed since the connect: not writing");
        return -1;
    }
    uint8_t *ccfg = buf_4k + ccfg_off, was[CJTAG_CCFG_SIZE];
    memcpy(was, ccfg, sizeof was);
    uint32_t old = le32(ccfg + CJTAG_CCFG_BL_CONFIG);
    uint32_t want = 0xC5FE00C5u | (uint32_t)pin << 8;  // bootloader on, backdoor DIO<pin>, active low, enabled
    if (old == want) {
        return 0;
    }
    put_le32_(ccfg + CJTAG_CCFG_BL_CONFIG, want);
    if (!ccfg_safe(c, was, ccfg, CCFG_MAY_BL)) {
        return -1;
    }
    if (!flash_raw(c, CJTAG_CCFG_PAGE_IDX, buf_4k)) {
        // the page as it was, at once: page 31 erased and not written leaves the chip without its CCFG and MAC
        char why[sizeof c->error];
        snprintf(why, sizeof why, "%s", c->error);
        put_le32_(ccfg + CJTAG_CCFG_BL_CONFIG, old);
        for (int i = 0; i < 3; i++) {
            if (flash_raw(c, CJTAG_CCFG_PAGE_IDX, buf_4k)) {
                snprintf(c->error, sizeof c->error, "page 31 restored; BL_CONFIG write failed: %.40s", why);
                return -1;
            }
        }
        snprintf(c->error, sizeof c->error, "BL_CONFIG write failed and page 31 NOT restored: restore the backup");
        return -1;
    }
    c->bl_config = want;
    c->page31_crc = ~crc32_update(0xFFFFFFFFu, buf_4k, CJTAG_PAGE);
    return 1;
}

void cjtag_release_and_reset(cjtag_t *c)
{
    // Clear DEMCR vector catch, unhalt CPU, and trigger Cortex-M3 VECTRESET (0x05FA0001) so the ROM boot code
    // does not enter the Halt-In-Boot (HIB) wait loop (chip.c line 84 / ti_cc26x0.cfg line 57).
    (void)cjtag_mem_write32(c, CM3_DEMCR, 0x00000000u);
    (void)cjtag_mem_write32(c, CM3_DHCSR, DHCSR_RUN);
    (void)cjtag_mem_write32(c, CM3_AIRCR, 0x05FA0001u);
    (void)dp_write(c, DP_CTRL_STAT, 0x00000000u);
    cjtag_reset_escape(c);
}

bool cjtag_sample(cjtag_t *c, uint32_t regs[CJTAG_SAMPLE_REGS])
{
    static const uint8_t which[CJTAG_SAMPLE_REGS] = {0,  1,  2,  3,  12, 13,
                                                     14, 15, 16, 17, 18};  // r0-3 r12 sp lr pc xpsr msp psp
    if (!cpu_halt(c)) {
        return false;
    }
    for (int i = 0; i < CJTAG_SAMPLE_REGS; i++) {
        if (!cpu_read_reg(c, which[i], &regs[i])) {
            return false;
        }
    }
    return cjtag_mem_write32(c, CM3_DHCSR, DHCSR_RUN);
}

void cjtag_release_running(cjtag_t *c)
{
    (void)cjtag_mem_write32(c, CM3_DEMCR, 0x00000000u);
    (void)cjtag_mem_write32(c, CM3_DHCSR, 0xA05F0000u);  // C_DEBUGEN off: the CPU runs on, undebugged
    (void)dp_write(c, DP_CTRL_STAT, 0x00000000u);
    cjtag_reset_escape(c);
}

int cjtag_write_page31_code(cjtag_t *c, const uint8_t *code, uint8_t *buf_4k, uint8_t *orig_4k)
{
    const uint32_t ccfg_off = CJTAG_CCFG_ADDR - CJTAG_CCFG_PAGE_IDX * CJTAG_PAGE;
    if (!c->has_ccfg_mac) {
        fail(c, "no factory MAC in its CCFG: not touching page 31");
        return -1;
    }
    if (!cjtag_dump_page(c, CJTAG_CCFG_PAGE_IDX, orig_4k)) {
        return -1;
    }
    if (~crc32_update(0xFFFFFFFFu, orig_4k, CJTAG_PAGE) != c->page31_crc) {
        fail(c, "page 31 changed since the connect: not writing");
        return -1;
    }
    const uint8_t *ccfg = orig_4k + ccfg_off;
    memmove(buf_4k, code, ccfg_off);                         // code may be buf_4k itself
    memcpy(buf_4k + ccfg_off, ccfg, CJTAG_PAGE - ccfg_off);  // the unit's own CCFG, byte for byte
    if (!ccfg_safe(c, ccfg, buf_4k + ccfg_off, 0)) {
        return -1;
    }
    if (!memcmp(buf_4k, orig_4k, CJTAG_PAGE)) {
        return 0;
    }
    if (!flash_raw(c, CJTAG_CCFG_PAGE_IDX, buf_4k)) {
        char why[sizeof c->error];
        snprintf(why, sizeof why, "%s", c->error);
        for (int i = 0; i < 3; i++) {
            if (flash_raw(c, CJTAG_CCFG_PAGE_IDX, orig_4k)) {
                snprintf(c->error, sizeof c->error, "page 31 restored; the code write failed: %.40s", why);
                return -1;
            }
        }
        snprintf(c->error, sizeof c->error, "page 31 write failed and NOT restored: restore the backup");
        return -1;
    }
    c->page31_crc = ~crc32_update(0xFFFFFFFFu, buf_4k, CJTAG_PAGE);
    return 1;
}

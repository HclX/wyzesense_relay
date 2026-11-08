// Native unit tests of esp32/contact/cjtag.c against a simulated CC1310 IEEE 1149.7 OScan1 + ICEPick-C +
// ARM CoreSight JTAG-DP/AHB-AP + Cortex-M3 ROM Hard-API target (issue #69).
#include "cjtag.h"

#include <stdio.h>
#include <string.h>

static int checks, failures;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        checks++;                                                                    \
        if (!(cond)) {                                                               \
            failures++;                                                              \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                            \
    } while (0)

typedef enum {
    TAP_RESET = 0,
    TAP_IDLE,
    TAP_SELECT_DR,
    TAP_CAPTURE_DR,
    TAP_SHIFT_DR,
    TAP_EXIT1_DR,
    TAP_PAUSE_DR,
    TAP_EXIT2_DR,
    TAP_UPDATE_DR,
    TAP_SELECT_IR,
    TAP_CAPTURE_IR,
    TAP_SHIFT_IR,
    TAP_EXIT1_IR,
    TAP_PAUSE_IR,
    TAP_EXIT2_IR,
    TAP_UPDATE_IR,
} tap_state_t;

typedef struct {
    // Physical pin state
    bool disconnected;
    bool swap_pins;
    bool tck;
    bool tms_out;
    bool tms_dir_out;
    bool tdo_line;
    unsigned high_tck_toggles;

    // IEEE 1149.7 OScan1 framing state
    bool in_oscan1;
    unsigned jscan0_clocks;
    unsigned oscan1_phase;
    bool latched_tdi;
    bool latched_tms;

    // JTAG TAP & ICEPick-C / CS_DAP state
    bool dap_at_tdo;
    tap_state_t tap;
    uint64_t shift_reg;
    unsigned shift_len;
    uint8_t icepick_ir;
    bool icepick_connected;
    bool dap_linked;
    uint8_t dap_ir;

    // CoreSight JTAG-DP / AHB-AP state
    uint32_t dp_ctrl_stat;
    uint32_t dp_select;
    uint32_t dp_rdbuff;
    uint32_t ap_csw;
    uint32_t ap_tar;

    // Cortex-M3 debug & core registers
    uint32_t dhcsr;
    uint32_t dcrsr;
    uint32_t dcrdr;
    uint32_t demcr;
    uint32_t aircr;
    uint32_t core_regs[21];
    uint32_t vims_ctl;
    uint32_t flash_cfg;
    uint32_t flash_fwlock;
    uint32_t flash_fwflag;

    // Glitch injection for testing double-read verification & hands-free resume
    int glitch_on_read_count;
    int read_count;
    int fail_programs;  // the next n HapiProgramFlash calls fail, after the erase (a write gone wrong)
    int erases;         // HapiSectorErase calls so far

    // Simulated CC1310 memories
    uint8_t flash[CJTAG_FLASH_SIZE];
    uint8_t sram[0x5000];
    uint8_t fcfg1[CJTAG_FCFG1_SIZE];
} sim_cc1310_t;

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t sim_bus_read32(sim_cc1310_t *s, uint32_t addr)
{
    s->read_count++;
    uint32_t flip = (s->glitch_on_read_count > 0 && s->read_count == s->glitch_on_read_count) ? 0x00000001u : 0u;
    if (addr + 4 <= sizeof s->flash) {
        return le32(s->flash + addr) ^ flip;
    }
    if (addr >= 0x20000000u && addr + 4 <= 0x20000000u + sizeof s->sram) {
        return le32(s->sram + (addr - 0x20000000u));
    }
    if (addr >= CJTAG_FCFG1_ADDR && addr + 4 <= CJTAG_FCFG1_ADDR + sizeof s->fcfg1) {
        return le32(s->fcfg1 + (addr - CJTAG_FCFG1_ADDR)) ^ flip;
    }
    if (addr == 0x10000054u) {
        return 0x10001101u;  // simulated HapiSectorErase Thumb entry point
    }
    if (addr == 0x1000005Cu) {
        return 0x10001201u;  // simulated HapiProgramFlash Thumb entry point
    }
    if (addr == 0xE000EDF0u) {
        return s->dhcsr;
    }
    if (addr == 0xE000EDF8u) {
        return s->dcrdr;
    }
    if (addr == 0x40034004u) {
        return s->vims_ctl;
    }
    if (addr == 0x40030024u) {
        return s->flash_cfg;
    }
    if (addr == 0x40030040u) {
        return s->flash_fwflag;
    }
    return 0;
}

static void sim_bus_write32(sim_cc1310_t *s, uint32_t addr, uint32_t val)
{
    if (addr >= 0x20000000u && addr + 4 <= 0x20000000u + sizeof s->sram) {
        put_le32(s->sram + (addr - 0x20000000u), val);
        return;
    }
    if (addr == 0xE000EDF0u) {
        if ((val & 0xFFFF0000u) == 0xA05F0000u) {
            if ((val & 0x02u) != 0) {
                s->dhcsr = 0x00030003u;  // S_HALT | S_REGRDY | C_HALT | C_DEBUGEN
            } else {
                // CPU resumed: execute simulated ROM Hard-API function at PC
                uint32_t pc = s->core_regs[15];
                if (pc == 0x10001101u) {
                    // HapiSectorErase(r0 = sector_addr)
                    uint32_t sec = s->core_regs[0];
                    if (sec < sizeof s->flash && (sec % CJTAG_PAGE) == 0) {
                        s->erases++;
                        memset(s->flash + sec, 0xFF, CJTAG_PAGE);
                        s->core_regs[0] = 0;  // FAPI_STATUS_SUCCESS
                    } else {
                        s->core_regs[0] = 1;
                    }
                } else if (pc == 0x10001201u) {
                    // HapiProgramFlash(r0 = sram_buf, r1 = flash_addr, r2 = count)
                    uint32_t src = s->core_regs[0];
                    uint32_t dst = s->core_regs[1];
                    uint32_t cnt = s->core_regs[2];
                    if (s->fail_programs > 0) {
                        s->fail_programs--;
                        s->core_regs[0] = 1;
                    } else if (src >= 0x20000000u && src + cnt <= 0x20000000u + sizeof s->sram &&
                               dst + cnt <= sizeof s->flash) {
                        for (uint32_t i = 0; i < cnt; i++) {
                            s->flash[dst + i] &= s->sram[(src - 0x20000000u) + i];
                        }
                        s->core_regs[0] = 0;
                    } else {
                        s->core_regs[0] = 1;
                    }
                }
                // Hit bkpt #0 at LR (0x20000001) -> halt CPU
                if (le32(s->sram) == 0xBF00BE00u && s->core_regs[14] == 0x20000001u) {
                    s->dhcsr = 0x00030003u;
                }
            }
        }
        return;
    }
    if (addr == 0xE000EDF4u) {
        s->dcrsr = val;
        uint8_t reg = (uint8_t)(val & 0x1Fu);
        if (reg < 21) {
            if ((val & 0x00010000u) != 0) {
                s->core_regs[reg] = s->dcrdr;
            } else {
                s->dcrdr = s->core_regs[reg];
            }
        }
        s->dhcsr |= 0x00010000u;  // S_REGRDY
        return;
    }
    if (addr == 0xE000EDF8u) {
        s->dcrdr = val;
        return;
    }
    if (addr == 0xE000EDFCu) {
        s->demcr = val;
        return;
    }
    if (addr == 0xE000ED0Cu) {
        s->aircr = val;
        return;
    }
    if (addr == 0x40034004u) {
        s->vims_ctl = val;
        return;
    }
    if (addr == 0x40030024u) {
        s->flash_cfg = val;
        return;
    }
    if (addr == 0x4003003Cu) {
        s->flash_fwlock = val;
        return;
    }
    if (addr == 0x40030040u && s->flash_fwlock == 0x05u) {
        s->flash_fwflag = val;
        return;
    }
}

static void sim_capture_ir(sim_cc1310_t *s)
{
    if (!s->dap_linked) {
        s->shift_len = 6;
        s->shift_reg = 0x01u;
    } else {
        s->shift_len = 10;
        s->shift_reg = s->dap_at_tdo ? ((0x01u << 4) | 0x01u) : ((0x01u << 6) | 0x01u);
    }
}

static void sim_update_ir(sim_cc1310_t *s)
{
    if (!s->dap_linked) {
        s->icepick_ir = (uint8_t)(s->shift_reg & 0x3Fu);
    } else if (s->dap_at_tdo) {
        s->dap_ir = (uint8_t)(s->shift_reg & 0x0Fu);
        s->icepick_ir = (uint8_t)((s->shift_reg >> 4) & 0x3Fu);
    } else {
        s->icepick_ir = (uint8_t)(s->shift_reg & 0x3Fu);
        s->dap_ir = (uint8_t)((s->shift_reg >> 6) & 0x0Fu);
    }
}

static void sim_capture_dr(sim_cc1310_t *s)
{
    if (!s->dap_linked) {
        if (s->icepick_ir == 0x07u) {
            s->shift_len = 8;
            s->shift_reg = s->icepick_connected ? 0x89u : 0x00u;
        } else if (s->icepick_ir == 0x02u) {
            s->shift_len = 32;
            s->shift_reg = 0;
        } else if (s->icepick_ir == 0x3Fu) {
            s->shift_len = 1;
            s->shift_reg = 0;
        } else {
            s->shift_len = 32;
            s->shift_reg = 0x3B9BE02Fu;  // CC1310 ICEPick-C IDCODE (version 3)
        }
        return;
    }

    uint64_t dap_dr = 0;
    unsigned dap_bits = 35;
    if (s->dap_ir == 0x0Eu) {
        dap_bits = 32;
        dap_dr = CJTAG_DAP_IDCODE;
    } else if (s->dap_ir == 0x0Au || s->dap_ir == 0x0Bu) {
        dap_bits = 35;
        dap_dr = ((uint64_t)s->dp_rdbuff << 3) | 0x02ULL;  // ACK = OK (0b010)
    }
    s->shift_len = dap_bits + 1;
    s->shift_reg = s->dap_at_tdo ? dap_dr : (dap_dr << 1);
}

static void sim_update_dr(sim_cc1310_t *s)
{
    if (!s->dap_linked) {
        if (s->icepick_ir == 0x07u && (s->shift_reg & 0xFFu) == 0x89u) {
            s->icepick_connected = true;
        } else if (s->icepick_ir == 0x02u && s->icepick_connected) {
            uint32_t cmd = (uint32_t)s->shift_reg;
            if (cmd == 0xA0112148u) {
                s->dap_linked = true;
                s->dap_ir = 0x0Eu;  // default DAP IR = IDCODE
            }
        }
        return;
    }

    unsigned dap_bits = (s->dap_ir == 0x0Eu) ? 32 : 35;
    uint64_t mask = (dap_bits == 64) ? ~0ULL : ((1ULL << dap_bits) - 1ULL);
    uint64_t dap_val = s->dap_at_tdo ? (s->shift_reg & mask) : ((s->shift_reg >> 1) & mask);

    if (s->dap_ir == 0x0Au) {
        // DPACC
        bool rnw = (dap_val & 1u) != 0;
        uint8_t addr = (uint8_t)((dap_val & 0x06u) << 1);
        uint32_t data = (uint32_t)(dap_val >> 3);
        if (rnw) {
            if (addr == 0x04u) {
                s->dp_rdbuff = s->dp_ctrl_stat;
            }
        } else {
            if (addr == 0x04u) {
                s->dp_ctrl_stat = data;
                if ((data & 0x50000000u) == 0x50000000u) {
                    s->dp_ctrl_stat |= 0xF0000000u;
                }
            } else if (addr == 0x08u) {
                s->dp_select = data;
            }
        }
    } else if (s->dap_ir == 0x0Bu) {
        // APACC
        bool rnw = (dap_val & 1u) != 0;
        uint8_t addr = (uint8_t)((dap_val & 0x06u) << 1);
        uint32_t data = (uint32_t)(dap_val >> 3);
        if (rnw) {
            if (addr == 0x00u) {
                s->dp_rdbuff = s->ap_csw;
            } else if (addr == 0x04u) {
                s->dp_rdbuff = s->ap_tar;
            } else if (addr == 0x0Cu) {
                s->dp_rdbuff = sim_bus_read32(s, s->ap_tar);
                if ((s->ap_csw & 0x10u) != 0) {
                    s->ap_tar = (s->ap_tar & ~0x3FFu) | ((s->ap_tar + 4u) & 0x3FFu);
                }
            }
        } else {
            if (addr == 0x00u) {
                s->ap_csw = data;
            } else if (addr == 0x04u) {
                s->ap_tar = data;
            } else if (addr == 0x0Cu) {
                sim_bus_write32(s, s->ap_tar, data);
                if ((s->ap_csw & 0x10u) != 0) {
                    s->ap_tar = (s->ap_tar & ~0x3FFu) | ((s->ap_tar + 4u) & 0x3FFu);
                }
            }
        }
    }
}

static void sim_step_tap(sim_cc1310_t *s, bool tms, bool tdi)
{
    switch (s->tap) {
        case TAP_RESET:
            s->tap = tms ? TAP_RESET : TAP_IDLE;
            break;
        case TAP_IDLE:
            s->tap = tms ? TAP_SELECT_DR : TAP_IDLE;
            break;
        case TAP_SELECT_DR:
            s->tap = tms ? TAP_SELECT_IR : TAP_CAPTURE_DR;
            if (s->tap == TAP_CAPTURE_DR) {
                sim_capture_dr(s);
            }
            break;
        case TAP_CAPTURE_DR:
            s->tap = tms ? TAP_EXIT1_DR : TAP_SHIFT_DR;
            break;
        case TAP_SHIFT_DR:
            if (s->shift_len > 0) {
                s->shift_reg = (s->shift_reg >> 1) | ((tdi ? 1ULL : 0ULL) << (s->shift_len - 1));
            }
            s->tap = tms ? TAP_EXIT1_DR : TAP_SHIFT_DR;
            break;
        case TAP_EXIT1_DR:
            s->tap = tms ? TAP_UPDATE_DR : TAP_PAUSE_DR;
            if (s->tap == TAP_UPDATE_DR) {
                sim_update_dr(s);
            }
            break;
        case TAP_PAUSE_DR:
            s->tap = tms ? TAP_EXIT2_DR : TAP_PAUSE_DR;
            break;
        case TAP_EXIT2_DR:
            s->tap = tms ? TAP_UPDATE_DR : TAP_SHIFT_DR;
            if (s->tap == TAP_UPDATE_DR) {
                sim_update_dr(s);
            }
            break;
        case TAP_UPDATE_DR:
            s->tap = tms ? TAP_SELECT_DR : TAP_IDLE;
            break;
        case TAP_SELECT_IR:
            s->tap = tms ? TAP_RESET : TAP_CAPTURE_IR;
            if (s->tap == TAP_CAPTURE_IR) {
                sim_capture_ir(s);
            }
            break;
        case TAP_CAPTURE_IR:
            s->tap = tms ? TAP_EXIT1_IR : TAP_SHIFT_IR;
            break;
        case TAP_SHIFT_IR:
            if (s->shift_len > 0) {
                s->shift_reg = (s->shift_reg >> 1) | ((tdi ? 1ULL : 0ULL) << (s->shift_len - 1));
            }
            s->tap = tms ? TAP_EXIT1_IR : TAP_SHIFT_IR;
            break;
        case TAP_EXIT1_IR:
            s->tap = tms ? TAP_UPDATE_IR : TAP_PAUSE_IR;
            if (s->tap == TAP_UPDATE_IR) {
                sim_update_ir(s);
            }
            break;
        case TAP_PAUSE_IR:
            s->tap = tms ? TAP_EXIT2_IR : TAP_PAUSE_IR;
            break;
        case TAP_EXIT2_IR:
            s->tap = tms ? TAP_UPDATE_IR : TAP_SHIFT_IR;
            if (s->tap == TAP_UPDATE_IR) {
                sim_update_ir(s);
            }
            break;
        case TAP_UPDATE_IR:
            s->tap = tms ? TAP_SELECT_DR : TAP_IDLE;
            break;
    }
}

static void sim_set_pins(void *ctx, bool tck, bool tms)
{
    sim_cc1310_t *s = ctx;
    if (s->disconnected) {
        return;
    }
    bool real_tck = s->swap_pins ? tms : tck;
    bool real_tms = s->swap_pins ? tck : tms;

    // Detect online-to-offline escape: TMSC transitions while TCKC is held high
    if (s->tck && real_tck && s->tms_dir_out && real_tms != s->tms_out) {
        s->high_tck_toggles++;
        if (s->high_tck_toggles >= 8) {
            s->in_oscan1 = false;
            s->jscan0_clocks = 0;
            s->oscan1_phase = 0;
            s->tap = TAP_RESET;
            s->icepick_ir = 0x04u;
            s->icepick_connected = false;
            s->dap_linked = false;
        }
    }
    if (!real_tck) {
        s->high_tck_toggles = 0;
        // In OScan1 Clock 3 (phase 2), target drives TDO onto TMSC while TCKC is low
        if (s->in_oscan1 && s->oscan1_phase == 2 && !s->tms_dir_out) {
            s->tdo_line = (s->shift_reg & 1ULL) != 0;
        }
    }

    // Rising edge of TCKC
    if (!s->tck && real_tck) {
        if (!s->in_oscan1) {
            s->jscan0_clocks++;
            // The XDS110 OScan1 activation sequence ends after Step B (146 clocks) + Step C (180 clocks) = 326 clocks
            if (s->jscan0_clocks == 326) {
                s->in_oscan1 = true;
                s->oscan1_phase = 0;
                s->tap = TAP_IDLE;
                s->icepick_ir = 0x04u;
            }
        } else {
            if (s->oscan1_phase == 0) {
                s->latched_tdi = !real_tms;
                s->oscan1_phase = 1;
            } else if (s->oscan1_phase == 1) {
                s->latched_tms = real_tms;
                s->oscan1_phase = 2;
            } else {
                sim_step_tap(s, s->latched_tms, s->latched_tdi);
                s->oscan1_phase = 0;
            }
        }
    }

    s->tck = real_tck;
    s->tms_out = real_tms;
}

static void sim_set_tms_dir(void *ctx, bool output)
{
    sim_cc1310_t *s = ctx;
    if (s->disconnected) {
        s->tms_dir_out = output;
        return;
    }
    // Changing TMSC direction while TCKC is high when TMSC is low causes a pull-up edge on TMSC
    if (s->tck && !s->swap_pins && output != s->tms_dir_out && !s->tms_out) {
        s->high_tck_toggles++;
        if (s->high_tck_toggles >= 8) {
            s->in_oscan1 = false;
            s->jscan0_clocks = 0;
            s->oscan1_phase = 0;
            s->tap = TAP_RESET;
            s->icepick_ir = 0x04u;
            s->icepick_connected = false;
            s->dap_linked = false;
        }
    }
    s->tms_dir_out = output;
    if (!s->tck && s->in_oscan1 && s->oscan1_phase == 2 && !output) {
        s->tdo_line = (s->shift_reg & 1ULL) != 0;
    }
}

static bool sim_get_tms(void *ctx)
{
    sim_cc1310_t *s = ctx;
    if (s->disconnected || s->swap_pins) {
        return true;  // pull-up on floating / swapped input pin
    }
    return s->tdo_line;
}

static void sim_delay_us(void *ctx, uint32_t us)
{
    (void)ctx;
    (void)us;
}

static bool image_fn(void *ctx, uint32_t off, uint8_t *buf, size_t n)
{
    memcpy(buf, (const uint8_t *)ctx + off, n);
    return true;
}

static void sim_init(sim_cc1310_t *s, const char *ccfg_mac)
{
    memset(s, 0, sizeof *s);
    memset(s->flash, 0xFF, sizeof s->flash);
    memset(s->fcfg1, 0xFF, sizeof s->fcfg1);

    // Populate FCFG1 USER_ID (0x50001294) and 64-bit IEEE MAC (0x500012F0)
    put_le32(s->fcfg1 + 0x294, 0x2082F000u);
    put_le32(s->fcfg1 + 0x2F0, 0x77A1B2C3u);
    put_le32(s->fcfg1 + 0x2F4, 0x00124B00u);

    // Populate valid Cortex-M3 vector table in page 0
    put_le32(s->flash + 0x00, 0x20004000u);  // initial SP
    put_le32(s->flash + 0x04, 0x00000101u);  // reset handler (Thumb)
    for (size_t i = 8; i < (size_t)CJTAG_APP_END; i++) {
        s->flash[i] = (uint8_t)((i * 37u + 13u) & 0xFFu);
    }

    // Populate CCFG in page 31 (0x0001FFA8)
    uint8_t *ccfg = s->flash + CJTAG_CCFG_ADDR;
    if (ccfg_mac) {
        memcpy(ccfg + 0x24, ccfg_mac, 8);
    }
    put_le32(ccfg + 0x30, 0xC5FE0EC5u);  // BL_CONFIG enabled on DIO14, active low
}

int main(void)
{
    static sim_cc1310_t sim;
    cjtag_io_t io = {
        .set_pins = sim_set_pins,
        .set_tms_dir = sim_set_tms_dir,
        .get_tms = sim_get_tms,
        .delay_us = sim_delay_us,
        .ctx = &sim,
    };
    cjtag_t cj;

    // 1. Disconnected / swapped probes must fast-fail cleanly (for hands-free auto-polarity polling)
    sim_init(&sim, "77A1B2C3");
    sim.disconnected = true;
    cjtag_init(&cj, &io);
    CHECK(!cjtag_connect(&cj, true));

    sim.disconnected = false;
    sim.swap_pins = true;
    cjtag_init(&cj, &io);
    CHECK(!cjtag_connect(&cj, true));

    // 2. Connect with both scan-chain orders (dap_at_tdo = false and dap_at_tdo = true)
    for (int order = 0; order < 2; order++) {
        sim_init(&sim, "77A1B2C3");
        sim.dap_at_tdo = (order == 1);
        cjtag_init(&cj, &io);
        CHECK(cjtag_connect(&cj, true));
        CHECK((cj.icepick_id & 0x0FFFFFFFu) == CJTAG_ICEPICK_IDCODE);
        CHECK(cj.dap_id == CJTAG_DAP_IDCODE);
        CHECK(cj.dap_at_tdo == sim.dap_at_tdo);
        CHECK(cj.user_id == 0x2082F000u);
        CHECK(cj.ieee_mac == 0x00124B0077A1B2C3ULL);
        CHECK(cj.has_ccfg_mac);
        CHECK(strcmp(cj.mac, "77A1B2C3") == 0);
        CHECK(cj.bl_config == 0xC5FE0EC5u);
    }

    // 3. Climate / v2 motion sensor fallback MAC at 0x0001D000 when CCFG MAC is all FF
    sim_init(&sim, NULL);
    memcpy(sim.flash + 0x0001D000u, "77C68193", 8);
    cjtag_init(&cj, &io);
    CHECK(cjtag_connect(&cj, true));
    CHECK(!cj.has_ccfg_mac);
    CHECK(strcmp(cj.mac, "77C68193") == 0);

    // 4. Full 32-page dump + FCFG1 dump + glitch detection & page-level resume
    sim_init(&sim, "77A1B2C3");
    cjtag_init(&cj, &io);
    CHECK(cjtag_connect(&cj, true));

    uint8_t fcfg1_dump[CJTAG_FCFG1_SIZE];
    CHECK(cjtag_dump_fcfg1(&cj, fcfg1_dump));
    CHECK(memcmp(fcfg1_dump, sim.fcfg1, CJTAG_FCFG1_SIZE) == 0);

    uint8_t page_buf[CJTAG_PAGE];
    // Inject a 1-bit glitch on the second read pass of page 5: cjtag_dump_page must reject it!
    sim.read_count = 0;
    sim.glitch_on_read_count = 1500;
    CHECK(!cjtag_dump_page(&cj, 5, page_buf));
    sim.glitch_on_read_count = 0;
    // Retry page 5 after the glitch: must succeed and match simulated flash byte-for-byte
    CHECK(cjtag_dump_page(&cj, 5, page_buf));
    CHECK(memcmp(page_buf, sim.flash + (size_t)5 * CJTAG_PAGE, CJTAG_PAGE) == 0);

    for (uint32_t p = 0; p < CJTAG_TOTAL_PAGES; p++) {
        CHECK(cjtag_dump_page(&cj, p, page_buf));
        CHECK(memcmp(page_buf, sim.flash + (size_t)p * CJTAG_PAGE, CJTAG_PAGE) == 0);
    }

    // 5. Flash safety rules: Page 31 (CCFG) is ALWAYS refused; Page 30 requires allow_nv; Page 0 checks vector table
    uint8_t new_page[CJTAG_PAGE];
    memset(new_page, 0xA5, sizeof new_page);
    put_le32(new_page + 0, 0x20005000u);  // valid SP
    put_le32(new_page + 4, 0x00001001u);  // valid Thumb reset handler

    CHECK(!cjtag_flash_page(&cj, CJTAG_CCFG_PAGE_IDX, new_page, true));
    CHECK(!cjtag_flash_page(&cj, CJTAG_NV_PAGE_IDX, new_page, false));

    uint8_t bad_page0[CJTAG_PAGE];
    memset(bad_page0, 0x11, sizeof bad_page0);
    CHECK(!cjtag_flash_page(&cj, 0, bad_page0, false));

    // 6. Flash pages 0 and 1, verify readback, VIMS_CTL restoration, and confirm page 31 is untouched
    uint8_t orig_page31[CJTAG_PAGE];
    memcpy(orig_page31, sim.flash + (size_t)CJTAG_CCFG_PAGE_IDX * CJTAG_PAGE, CJTAG_PAGE);
    sim.vims_ctl = 0x00000001u;  // CACHE mode (0x1)

    CHECK(cjtag_flash_page(&cj, 0, new_page, false));
    CHECK(memcmp(sim.flash, new_page, CJTAG_PAGE) == 0);
    CHECK(sim.vims_ctl == 0x00000001u);

    memset(new_page, 0x5A, sizeof new_page);
    CHECK(cjtag_flash_page(&cj, 1, new_page, false));
    CHECK(memcmp(sim.flash + CJTAG_PAGE, new_page, CJTAG_PAGE) == 0);
    CHECK(memcmp(sim.flash + (size_t)CJTAG_CCFG_PAGE_IDX * CJTAG_PAGE, orig_page31, CJTAG_PAGE) == 0);
    CHECK(sim.vims_ctl == 0x00000001u);

    // 7. The image writer: pages already equal skipped, the rest written through cjtag_flash_page's checks
    static uint8_t image[3 * CJTAG_PAGE - 100];
    memcpy(image, sim.flash, sizeof image);  // pages 0 and 1 as now, page 2 different
    memset(image + (size_t)2 * CJTAG_PAGE, 0x3C, sizeof image - (size_t)2 * CJTAG_PAGE);
    int erases0 = sim.erases;
    CHECK(cjtag_write_image(&cj, image_fn, image, sizeof image, page_buf) == 1);
    CHECK(sim.erases == erases0 + 1 && memcmp(sim.flash, image, sizeof image) == 0);
    CHECK(sim.flash[(size_t)3 * CJTAG_PAGE - 100] == 0xFF);                       // the rest of the last page: erased
    CHECK(cjtag_write_image(&cj, image_fn, image, sizeof image, page_buf) == 0);  // nothing to do now
    CHECK(cjtag_write_image(&cj, image_fn, image, CJTAG_APP_END + 4, page_buf) == -1);

    // 8. BL_CONFIG (the one page-31 write allowed): only that word changes; refusals; a failed write put back
    uint8_t *ccfg = sim.flash + CJTAG_CCFG_ADDR;
    put_le32(ccfg + CJTAG_CCFG_TAP_DAP_0, 0xFFC500C5u);
    put_le32(ccfg + CJTAG_CCFG_IMAGE_VALID, 0);
    memset(sim.flash + (size_t)CJTAG_CCFG_PAGE_IDX * CJTAG_PAGE, 0x6B, 0x100);  // stock code before the CCFG
    cjtag_init(&cj, &io);
    CHECK(cjtag_connect(&cj, true));
    memcpy(orig_page31, sim.flash + (size_t)CJTAG_CCFG_PAGE_IDX * CJTAG_PAGE, CJTAG_PAGE);
    CHECK(cjtag_set_backdoor(&cj, 15, page_buf) == -1);  // no DIO15 on the 5x5 package
    CHECK(cjtag_set_backdoor(&cj, 1, page_buf) == 1);
    CHECK(le32(ccfg + CJTAG_CCFG_BL_CONFIG) == 0xC5FE01C5u && cj.bl_config == 0xC5FE01C5u);
    put_le32(orig_page31 + (CJTAG_CCFG_ADDR - CJTAG_CCFG_PAGE_IDX * CJTAG_PAGE) + CJTAG_CCFG_BL_CONFIG, 0xC5FE01C5u);
    CHECK(memcmp(sim.flash + (size_t)CJTAG_CCFG_PAGE_IDX * CJTAG_PAGE, orig_page31, CJTAG_PAGE) == 0);
    erases0 = sim.erases;
    CHECK(cjtag_set_backdoor(&cj, 1, page_buf) == 0 && sim.erases == erases0);  // already so: no erase
    CHECK(cjtag_flash_page(&cj, 3, new_page, false));  // the new page-31 CRC is the one checked now

    sim.fail_programs = 1;  // erased, then the write fails: the page goes back as it was
    CHECK(cjtag_set_backdoor(&cj, 10, page_buf) == -1 && strstr(cj.error, "restored"));
    CHECK(memcmp(sim.flash + (size_t)CJTAG_CCFG_PAGE_IDX * CJTAG_PAGE, orig_page31, CJTAG_PAGE) == 0);
    CHECK(cj.bl_config == 0xC5FE01C5u);
    sim.fail_programs = 10;  // and if that fails too, it says so
    CHECK(cjtag_set_backdoor(&cj, 10, page_buf) == -1 && strstr(cj.error, "NOT restored"));
    sim.fail_programs = 0;
    memcpy(sim.flash + (size_t)CJTAG_CCFG_PAGE_IDX * CJTAG_PAGE, orig_page31, CJTAG_PAGE);

    put_le32(ccfg + CJTAG_CCFG_TAP_DAP_0, 0xFF0000C5u);  // the CPU debug port off (ccfg-rule.md): refused, untouched
    cjtag_init(&cj, &io);
    CHECK(cjtag_connect(&cj, true));
    erases0 = sim.erases;
    CHECK(cjtag_set_backdoor(&cj, 10, page_buf) == -1 && sim.erases == erases0);
    put_le32(ccfg + CJTAG_CCFG_TAP_DAP_0, 0xFFC500C5u);
    memset(ccfg + 0x24, 0xFF, 8);  // no factory MAC: refused
    cjtag_init(&cj, &io);
    CHECK(cjtag_connect(&cj, true));
    CHECK(cjtag_set_backdoor(&cj, 10, page_buf) == -1 && sim.erases == erases0);
    CHECK(cjtag_write_image(&cj, image_fn, image, sizeof image, page_buf) == -1);

    // 8b. ccfg-rule.md: a disabled bootloader may be turned on (an erase-and-rewrite, BL_CONFIG only)
    sim_init(&sim, "77A1B2C3");
    put_le32(sim.flash + CJTAG_CCFG_ADDR + CJTAG_CCFG_TAP_DAP_0, 0xFFC500C5u);
    put_le32(sim.flash + CJTAG_CCFG_ADDR + CJTAG_CCFG_IMAGE_VALID, 0);
    put_le32(sim.flash + CJTAG_CCFG_ADDR + CJTAG_CCFG_BL_CONFIG, 0x00FE0100u);
    cjtag_init(&cj, &io);
    CHECK(cjtag_connect(&cj, true));
    CHECK(cjtag_set_backdoor(&cj, 1, page_buf) == 1 && cj.bl_config == 0xC5FE01C5u);

    // 9. The CCFG MAC fill (newer units: MAC in page 29, the CCFG slot blank): program only, never an erase
    sim_init(&sim, NULL);
    memcpy(sim.flash + CJTAG_PAGE29_MAC_ADDR, "77A9B885", 8);
    put_le32(sim.flash + CJTAG_CCFG_ADDR + CJTAG_CCFG_TAP_DAP_0, 0xFFC500C5u);  // a real unit's CCFG (77A9B885)
    put_le32(sim.flash + CJTAG_CCFG_ADDR + CJTAG_CCFG_IMAGE_VALID, 0);
    memset(sim.flash + (size_t)CJTAG_CCFG_PAGE_IDX * CJTAG_PAGE, 0x6B, 0x100);
    cjtag_init(&cj, &io);
    CHECK(cjtag_connect(&cj, true) && !cj.has_ccfg_mac && strcmp(cj.mac, "77A9B885") == 0);
    CHECK(cjtag_write_image(&cj, image_fn, image, sizeof image, page_buf) == -1);  // refused without a CCFG MAC
    memcpy(orig_page31, sim.flash + (size_t)CJTAG_CCFG_PAGE_IDX * CJTAG_PAGE, CJTAG_PAGE);
    erases0 = sim.erases;
    CHECK(cjtag_fill_ccfg_mac(&cj, "77A9B886", page_buf) == -1);  // not the page-29 MAC
    CHECK(cjtag_fill_ccfg_mac(&cj, "77A9B88", page_buf) == -1);
    CHECK(cjtag_fill_ccfg_mac(&cj, "77A9B8G5", page_buf) == -1);
    CHECK(memcmp(sim.flash + (size_t)CJTAG_CCFG_PAGE_IDX * CJTAG_PAGE, orig_page31, CJTAG_PAGE) == 0);
    CHECK(cjtag_fill_ccfg_mac(&cj, "77a9b885", page_buf) == 1);
    CHECK(sim.erases == erases0 && memcmp(sim.flash + CJTAG_CCFG_ADDR + CJTAG_CCFG_MAC, "77A9B885", 8) == 0);
    memcpy(orig_page31 + (CJTAG_CCFG_ADDR + CJTAG_CCFG_MAC - CJTAG_CCFG_PAGE_IDX * CJTAG_PAGE), "77A9B885", 8);
    CHECK(memcmp(sim.flash + (size_t)CJTAG_CCFG_PAGE_IDX * CJTAG_PAGE, orig_page31, CJTAG_PAGE) == 0);
    CHECK(cj.has_ccfg_mac && strcmp(cj.mac, "77A9B885") == 0);
    CHECK(cjtag_fill_ccfg_mac(&cj, "77A9B885", page_buf) == 0);                   // already so
    CHECK(cjtag_write_image(&cj, image_fn, image, sizeof image, page_buf) >= 1);  // and the unit can be flashed now
    cjtag_init(&cj, &io);
    CHECK(cjtag_connect(&cj, true) && cj.has_ccfg_mac && strcmp(cj.mac, "77A9B885") == 0);
    memcpy(sim.flash + CJTAG_PAGE29_MAC_ADDR, "77A9B886", 8);  // a slot already holding another MAC: never touched
    memcpy(orig_page31, sim.flash + (size_t)CJTAG_CCFG_PAGE_IDX * CJTAG_PAGE, CJTAG_PAGE);
    erases0 = sim.erases;
    CHECK(cjtag_fill_ccfg_mac(&cj, "77A9B886", page_buf) == -1 && sim.erases == erases0);
    CHECK(memcmp(sim.flash + (size_t)CJTAG_CCFG_PAGE_IDX * CJTAG_PAGE, orig_page31, CJTAG_PAGE) == 0);

    // 10. Page 31's code area replaced, the unit's CCFG kept byte for byte; a failed write put back; refusals
    {
        static uint8_t code[0xFA8], orig[CJTAG_PAGE], before[CJTAG_PAGE];
        uint8_t *p31 = sim.flash + (size_t)CJTAG_CCFG_PAGE_IDX * CJTAG_PAGE;
        sim_init(&sim, "77A1B2C3");
        uint8_t *cc = sim.flash + CJTAG_CCFG_ADDR;
        put_le32(cc + CJTAG_CCFG_TAP_DAP_0, 0xFFC500C5u);
        put_le32(cc + CJTAG_CCFG_IMAGE_VALID, 0);
        memset(p31, 0x6B, sizeof code);   // the contact's own code there
        memset(code, 0x4D, sizeof code);  // the dongle's
        memcpy(before, p31, CJTAG_PAGE);
        cjtag_init(&cj, &io);
        CHECK(cjtag_connect(&cj, true));
        sim.fail_programs = 1;
        CHECK(cjtag_write_page31_code(&cj, code, page_buf, orig) == -1 && strstr(cj.error, "restored"));
        CHECK(memcmp(p31, before, CJTAG_PAGE) == 0);
        CHECK(cjtag_write_page31_code(&cj, code, page_buf, orig) == 1 && memcmp(orig, before, CJTAG_PAGE) == 0);
        CHECK(memcmp(p31, code, sizeof code) == 0 && memcmp(p31 + sizeof code, before + sizeof code, 88) == 0);
        CHECK(cjtag_write_page31_code(&cj, code, page_buf, orig) == 0);  // already so
        CHECK(cjtag_flash_page(&cj, 2, new_page, false));                // the new CRC is the one checked now
        put_le32(cc + CJTAG_CCFG_IMAGE_VALID, 0x00000001u);  // image not valid (ccfg-rule.md): refused, untouched
        cjtag_init(&cj, &io);
        CHECK(cjtag_connect(&cj, true));
        memcpy(before, p31, CJTAG_PAGE);
        memset(code, 0x4E, sizeof code);
        CHECK(cjtag_write_page31_code(&cj, code, page_buf, orig) == -1 && memcmp(p31, before, CJTAG_PAGE) == 0);
    }

    // 11. Clean release and VECTRESET (0x05FA0001)
    cjtag_release_and_reset(&cj);
    CHECK(sim.aircr == 0x05FA0001u);
    CHECK(!sim.in_oscan1);

    if (failures) {
        fprintf(stderr, "%d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("test_cjtag: %d checks passed\n", checks);
    return 0;
}

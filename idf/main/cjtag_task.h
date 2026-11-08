// Hands-free provisioning station task for dumping and flashing CC1310 sensors over 2-pin cJTAG (M/C, issue #69)
// or 4-pad UART ROM bootloader (SBL with pad auto-detection, issue #81).
// Hard rule (CLAUDE.md / AGENTS.md): page 31 (CCFG + factory MAC) is never erased or written.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cjtag.h"

typedef enum {
    CJTAG_MODE_NONE = 0,
    CJTAG_MODE_DUMP = 1,
    CJTAG_MODE_FLASH = 2,
    CJTAG_MODE_IDENTIFY = 3,  // find and confirm the pins only; dump and flash then use what it found (#81)
} cjtag_mode_t;

typedef enum {
    CJTAG_TRANSPORT_CJTAG = 0,
    CJTAG_TRANSPORT_SBL = 1,
    CJTAG_TRANSPORT_AUTO = 2,
    CJTAG_TRANSPORT_SBL2 = 3,  // SBL with two hand-held probes on the sensor's TX/RX (R, T wires), + and - clipped
    CJTAG_TRANSPORT_WS = 4,    // after T/R: probe 1 on T (or R), probe 2 on a pad: is that pad S (reset) or W
} cjtag_transport_t;

void cjtag_task_start(void);

// Stage a CC1310 image (up to 120 KB, pages 0-29) into the 4 KB page buffers for station flashing.
bool cjtag_task_stage_begin(bool wipe_nv, char *err, size_t err_cap);
bool cjtag_task_stage_feed(const uint8_t *data, size_t len);
bool cjtag_task_stage_end(bool rx_ok, cjtag_transport_t transport, char *msg, size_t msg_cap);

// Arm or disarm the station state machine (its pins are fixed: pins.h).
bool cjtag_task_arm(cjtag_mode_t mode, cjtag_transport_t transport, char *msg, size_t msg_cap);

// Copy out a 4 KB page from the completed dump buffer, or the 1 KB FCFG1 buffer.
bool cjtag_task_get_dump_page(uint32_t page_idx, uint8_t *out_4k);
bool cjtag_task_get_fcfg1(uint8_t *out_1k);
void cjtag_task_note_dump_downloaded(void);
const char *cjtag_task_mac(void);

// Format current station state, transport, and progress as JSON for GET /api/cjtag.
size_t cjtag_task_json(char *out, size_t cap);

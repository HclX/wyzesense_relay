// The relay keeps its contact board on the gateway image it carries (#76): at its start, before it relays anything,
// it asks the gateway for page 0 of its build info block (OWS_SUB_BUILD_INFO) and compares the image IDs. On a
// difference it reflashes the contact (sbl.c: pages 0-29 only, the NV page with the sensors and the CCFG with the MAC
// kept). When the gateway does not answer (older firmware, stock, a hung board), the bootloader's CRC32 of the image's
// range decides, so an image already there is not written again. Once per call; the caller calls it once per boot.
// Portable C: the platform gives the UART, the S pad and the carried image's bytes, and optionally the
// contact's debug port (cJTAG on its M and C pads, #81): then the reflash can go that way instead (transport).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "build_info.h"
#include "sbl.h"

typedef struct {
    bool (*write)(void *ctx, const uint8_t *data, size_t len);
    // up to cap bytes: waits up to timeout_ms for the first, then returns what has come; 0 on timeout
    size_t (*read)(void *ctx, uint8_t *buf, size_t cap, uint32_t timeout_ms);
    void (*flush_input)(void *ctx);
    void (*reset)(void *ctx);             // S pulsed (or power-cycled): the contact starts its firmware
    void (*enter_bootloader)(void *ctx);  // R held low through a reset: its ROM bootloader
    void (*sleep_ms)(void *ctx, uint32_t ms);
    // len bytes of the carried image from off (it may live in flash the CPU cannot read directly)
    bool (*image)(void *ctx, uint32_t off, uint8_t *buf, size_t len);
    // each step as it starts, for a status page while the check runs (may be NULL)
    void (*progress)(void *ctx, const char *step);
    void *ctx;
    // The contact's debug port (cJTAG, #81; may be NULL: none wired): write the carried image's pages 0-29 (pages
    // already equal are skipped; never page 30 or 31), the CCFG's factory MAC checked first, every page verified; then
    // reset it into its firmware. Returns the pages written (0: all already equal), or -1 when it failed (not
    // connected, no MAC, a write or a verify failed). mac: its CCFG MAC (9 bytes). err: why it failed.
    int (*cjtag_flash)(void *ctx, uint32_t image_len, char *mac, char *err, size_t err_cap);
    uint8_t transport;  // GWSYNC_VIA_*: how to reflash (0, auto, when left out)
} gwsync_io_t;

// How gwsync_run reflashes: auto tries the debug port first (when there is one) and the bootloader if that fails;
// cjtag and sbl use only that one.
enum { GWSYNC_VIA_AUTO = 0, GWSYNC_VIA_CJTAG = 1, GWSYNC_VIA_SBL = 2 };

typedef enum {
    GWSYNC_SAME,     // the contact runs the carried image: nothing written
    GWSYNC_UPDATED,  // reflashed, and it now answers with the carried image's ID
    GWSYNC_FAILED,   // a write or the check after it failed: the next start tries again
    GWSYNC_REFUSED,  // not tried: the carried image is not a gateway, or the contact has no usable bootloader
} gwsync_result_t;

typedef struct {
    gwsync_result_t result;
    bool answered;         // the gateway answered HOST_OWS_BUILD_INFO (before any reflash)
    build_info_t running;  // what it answered (the last answer: after a reflash, the new one)
    build_info_t carried;  // the carried image's block
    char mac[9];           // the contact's CCFG MAC, if the bootloader or the debug port was entered
    bool via_cjtag;        // the reflash (or the check) went through the debug port
    uint32_t junk;         // bytes from the contact that were not the answer (a noisy or chattering line)
    char msg[160];         // for the status page
} gwsync_report_t;

// image_len: the carried image's length (a multiple of 4, at most pages 0-29). sbl and page (SBL_PAGE bytes) are the
// caller's: large for a small stack.
gwsync_result_t gwsync_run(const gwsync_io_t *io, uint32_t image_len, sbl_t *sbl, uint8_t *page, gwsync_report_t *r);

// Safe-mode check when startup auto-sync is paused (skip_gwsync): resets the contact and queries its build info
// read-only without ever entering the ROM bootloader or writing flash.
gwsync_result_t gwsync_probe(const gwsync_io_t *io, uint32_t image_len, uint8_t *page, gwsync_report_t *r);

// Just the question: the gateway's build info block (3 tries). False if it does not answer.
bool gwsync_query(const gwsync_io_t *io, build_info_t *out);

// The build info block in the first bytes of an image (its first 1 KB, or less): false if none.
bool gwsync_find_info(const uint8_t *head, size_t len, build_info_t *out);

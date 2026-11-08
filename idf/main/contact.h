// The v1 contact board the relay serves (docs/design/v1-contact.md; wiring: esp32/README.md): its UART (the gateway's host
// link, and the ROM bootloader's; R / DIO1 is also the ROM bootloader backdoor, BL_CONFIG 0xC5FE01C5), its + supply
// pin, its M/C debug port (cJTAG), and its S pad (RESET_N, open-drain).
#pragma once

#include "gwnv.h"
#include "gwsync.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void contact_init(void);

// A reset into its firmware (R high)
void contact_reset(void);

// A reset into the ROM bootloader (R held low across the start, then released: the ROM looks at it at the reset only)
void contact_enter_bootloader(void);

// The UART is shared: the relay uses it unless a bootloader session holds it
bool contact_uart_take(uint32_t wait_ms);
void contact_uart_give(void);

size_t contact_uart_read(uint8_t *buf, size_t cap, uint32_t timeout_ms);
bool contact_uart_write(const uint8_t *data, size_t len);
void contact_uart_flush_input(void);
void contact_uart_counts(uint32_t *tx, uint32_t *rx);

// Reflash the contact (pages 0-29: image, len) through its ROM bootloader (esp32/contact/sbl.c): the UART taken from
// the relay, R held low across a reset into the bootloader, the CCFG checked (expect_mac if not NULL), written and
// verified, page 30 wiped if wipe_nv, then a reset into the new firmware and the UART given back. msg: what happened.
bool contact_flash(const uint8_t *image, size_t len, const char *expect_mac, bool wipe_nv, char *msg, size_t cap);

// Streaming variant of contact_flash() (4 KB page buffer, zero 120 KB heap allocation): begin, feed chunks, then end.
bool contact_flash_begin(const char *expect_mac, bool wipe_nv, char *msg, size_t cap);
bool contact_flash_write(const uint8_t *data, size_t len);
bool contact_flash_end(bool rx_ok, char *msg, size_t cap);

// At the start, before anything is relayed: the contact on the gateway image this firmware carries (#76,
// esp32/contact/gwsync.c): asked for its image ID, reflashed if it runs another image. Takes the UART meanwhile.
void contact_gwsync(void);
bool contact_stock_gw_available(void);

// What it found (NULL before it ran)
const gwsync_report_t *contact_gwsync_report(void);

// The check's last steps ("<ms since start> <step>"), oldest first, a line each: while it runs, too
size_t contact_gwsync_steps(char *out, size_t cap);

// Runtime NV export / zero-ladder migration over the contact's UART (OWS_SUB_NETWORK + OWS_SUB_NV_IMPORT, TASK-14).
bool contact_nv_export(char *json_out, size_t json_cap, char *msg, size_t msg_cap);
bool contact_nv_import(const gwnv_doc_t *doc, char *msg, size_t msg_cap);

// Bring-up: one page 0-30 through the debug port (page 31 never); 1 written, -1 failed (err). mac: the CCFG MAC.
int contact_cjtag_page(uint32_t page, const uint8_t *data, char *mac, char *err, size_t cap);
// Bring-up: a restart of the contact and its UART output from then for ms (send written at send_at_ms)
size_t contact_capture(uint32_t ms, const uint8_t *send, size_t n, uint32_t send_at_ms, uint8_t *out, size_t cap);
// Bring-up: register samples and a memory read through the debug port, the CPU left running (text)
size_t contact_sample(uint32_t wait_ms, int n, uint32_t gap_ms, uint32_t addr, uint32_t len, bool restart_first,
                      char *out, size_t cap);
// Bring-up: page 31 read (op 0) or its code area written, the CCFG kept (op 1); see cjtag_write_page31_code
int contact_page31(int op, const uint8_t *code, uint8_t *page, char *err, size_t cap);

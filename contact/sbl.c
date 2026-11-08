#include "sbl.h"

#include <stdio.h>
#include <string.h>

// TI SWCU117, ROM bootloader: packets {size, checksum, command, data}; ACK 00 CC, NAK 00 33; replies {size, checksum,
// data} that the host ACKs
#define CMD_PING         0x20
#define CMD_DOWNLOAD     0x21
#define CMD_GET_STATUS   0x23
#define CMD_SEND_DATA    0x24
#define CMD_SECTOR_ERASE 0x26
#define CMD_CRC32        0x27
#define CMD_GET_CHIP_ID  0x28
#define CMD_MEMORY_READ  0x2A
#define RET_SUCCESS      0x40
#define MAX_DATA         252  // a packet is at most 255 bytes
#define READ_WORDS       63   // memory read: at most 63 words a time
#define READ_BYTES       ((size_t)READ_WORDS * 4)

static bool nv_writing;  // page 30 may be written (inside sbl_write_nv only)

static bool fail(sbl_t *s, const char *why)
{
    snprintf(s->error, sizeof s->error, "%s", why);
    return false;
}

// Every erase and download is checked here, at the last step before the chip: pages 0-29, page 30 in
// sbl_write_nv(), page 31 never
static bool writable(uint32_t addr, uint32_t len)
{
    if (len == 0 || addr + len < addr) {
        return false;
    }
    if (addr + len <= SBL_APP_END) {
        return true;
    }
    return nv_writing && addr >= SBL_NV_PAGE && addr + len <= SBL_NV_PAGE + SBL_PAGE;
}

void sbl_init(sbl_t *s, const sbl_io_t *io)
{
    memset(s, 0, sizeof *s);
    s->io = *io;
}

static bool get_byte(sbl_t *s, uint8_t *b, uint32_t timeout_ms)
{
    if (!s->pending_len) {
        s->pending_len = s->io.read(s->io.ctx, s->pending, sizeof s->pending, timeout_ms);
        if (!s->pending_len) {
            return false;
        }
    }
    *b = s->pending[0];
    memmove(s->pending, s->pending + 1, --s->pending_len);
    return true;
}

// ACK (true) or NAK / nothing (false), zero bytes before it skipped. A line that keeps sending other bytes ends it
// too (a read only times out on silence)
#define ACK_JUNK_MAX 64

static bool get_ack(sbl_t *s, uint32_t timeout_ms)
{
    uint8_t b;
    for (int junk = 0; junk < ACK_JUNK_MAX && get_byte(s, &b, timeout_ms); junk++) {
        if (b == 0xCC) {
            return true;
        }
        if (b == 0x33) {
            return false;
        }
    }
    return false;
}

static bool command(sbl_t *s, uint8_t cmd, const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    uint8_t p[3 + MAX_DATA], sum = cmd;
    if (len > MAX_DATA) {
        return false;
    }
    if (cmd == CMD_SECTOR_ERASE || cmd == CMD_DOWNLOAD) {
        uint32_t addr = (uint32_t)data[0] << 24 | (uint32_t)data[1] << 16 | (uint32_t)data[2] << 8 | data[3];
        uint32_t n = cmd == CMD_SECTOR_ERASE
                         ? SBL_PAGE
                         : (uint32_t)data[4] << 24 | (uint32_t)data[5] << 16 | (uint32_t)data[6] << 8 | data[7];
        if (!writable(addr, n)) {
            return fail(s, "refused: only pages 0-29, page 30 by sbl_write_nv(), never page 31");
        }
    }
    for (size_t i = 0; i < len; i++) {
        sum = (uint8_t)(sum + data[i]);
    }
    p[0] = (uint8_t)(3 + len), p[1] = sum, p[2] = cmd;
    memcpy(p + 3, data, len);
    return s->io.write(s->io.ctx, p, 3 + len) && get_ack(s, timeout_ms);
}

// The reply packet after a command's ACK: its data (up to cap); ACKed when its checksum holds
static size_t response(sbl_t *s, uint8_t *out, size_t cap)
{
    uint8_t size = 0, sum, b;
    while (get_byte(s, &size, 1000) && size == 0) {
    }
    if (size < 2 || !get_byte(s, &sum, 1000)) {
        return 0;
    }
    size_t n = size - 2u;
    uint8_t got = 0;
    for (size_t i = 0; i < n; i++) {
        if (!get_byte(s, &b, 1000)) {
            return 0;
        }
        if (i < cap) {
            out[i] = b;
        }
        got = (uint8_t)(got + b);
    }
    uint8_t ack[2] = {0x00, got == sum ? 0xCC : 0x33};
    s->io.write(s->io.ctx, ack, 2);
    return got == sum && n <= cap ? n : 0;
}

static void be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v;
}

static bool status_ok(sbl_t *s)
{
    uint8_t st = 0;
    return command(s, CMD_GET_STATUS, NULL, 0, 1000) && response(s, &st, 1) == 1 && st == RET_SUCCESS;
}

bool sbl_chip_crc32(sbl_t *s, uint32_t addr, uint32_t len, uint32_t *crc)
{
    uint8_t a[12] = {0}, r[4];
    be32(a, addr), be32(a + 4, len);
    if (!command(s, CMD_CRC32, a, sizeof a, 5000) || response(s, r, 4) != 4) {
        return false;
    }
    *crc = (uint32_t)r[0] << 24 | (uint32_t)r[1] << 16 | (uint32_t)r[2] << 8 | r[3];
    return true;
}

bool sbl_get_chip_id(sbl_t *s, uint32_t *chip_id)
{
    uint8_t r[4];
    if (!command(s, CMD_GET_CHIP_ID, NULL, 0, 1000) || response(s, r, 4) != 4) {
        return fail(s, "no chip ID");
    }
    s->chip_id = (uint32_t)r[0] << 24 | (uint32_t)r[1] << 16 | (uint32_t)r[2] << 8 | r[3];
    if (chip_id) {
        *chip_id = s->chip_id;
    }
    return true;
}

bool sbl_read_memory(sbl_t *s, uint32_t addr, uint8_t *out, size_t len)
{
    if (!out || len == 0 || (addr & 3u) != 0 || (len & 3u) != 0) {
        return fail(s, "memory read must be non-empty and 32-bit aligned");
    }
    for (size_t off = 0; off < len; off += READ_BYTES) {
        size_t n = len - off < READ_BYTES ? len - off : READ_BYTES;
        uint8_t a[6];
        be32(a, addr + (uint32_t)off);
        a[4] = 1, a[5] = (uint8_t)(n / 4);  // 32-bit accesses
        if (!command(s, CMD_MEMORY_READ, a, sizeof a, 1000) || response(s, out + off, n) != n) {
            return fail(s, "memory read failed");
        }
    }
    return true;
}

bool sbl_verify_page(sbl_t *s, uint32_t page_idx, const uint8_t *expect_4k)
{
    if (page_idx >= 32u || !expect_4k) {
        return fail(s, "page index out of range (0..31)");
    }
    uint8_t verify_buf[READ_BYTES];
    uint32_t addr = page_idx * SBL_PAGE;
    for (size_t off = 0; off < SBL_PAGE; off += READ_BYTES) {
        size_t n = SBL_PAGE - off < READ_BYTES ? SBL_PAGE - off : READ_BYTES;
        if (!sbl_read_memory(s, addr + (uint32_t)off, verify_buf, n)) {
            return false;
        }
        if (memcmp(expect_4k + off, verify_buf, n) != 0) {
            return fail(s, "page read mismatch between pass 1 and pass 2");
        }
    }
    return true;
}

bool sbl_dump_page(sbl_t *s, uint32_t page_idx, uint8_t *out_4k)
{
    if (page_idx >= 32u || !out_4k) {
        return fail(s, "page index out of range (0..31)");
    }
    uint32_t addr = page_idx * SBL_PAGE;
    return sbl_read_memory(s, addr, out_4k, SBL_PAGE) && sbl_verify_page(s, page_idx, out_4k);
}

bool sbl_dump_flash(sbl_t *s, uint8_t *out_128k)
{
    if (!out_128k) {
        return fail(s, "invalid output buffer");
    }
    for (uint32_t p = 0; p < 32u; p++) {
        if (!sbl_dump_page(s, p, out_128k + (size_t)p * SBL_PAGE)) {
            return false;
        }
    }
    return true;
}

uint32_t sbl_crc32_update(uint32_t c, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) {
            c = c >> 1 ^ (0xEDB88320u & (0u - (c & 1)));
        }
    }
    return c;
}

static uint32_t crc32_ieee(const uint8_t *p, size_t n)
{
    return ~sbl_crc32_update(0xFFFFFFFFu, p, n);
}

static bool try_sync(sbl_t *s, uint32_t timeout_ms)
{
    static const uint8_t sync[2] = {0x55, 0x55}, zeros[100] = {0};
    s->pending_len = 0;
    s->already_synced = false;
    s->io.flush_input(s->io.ctx);
    if (!s->io.write(s->io.ctx, sync, 2)) {
        return fail(s, "UART write failed");
    }
    if (!get_ack(s, timeout_ms)) {
        // a bootloader synced already takes the first 0x55 as an 85-byte packet's size: zeros fill it up (its
        // checksum fails: a NAK), are skipped between packets, then a ping tells
        s->io.write(s->io.ctx, zeros, sizeof zeros);
        s->io.read(s->io.ctx, s->pending, sizeof s->pending, 100);
        s->io.flush_input(s->io.ctx);
        s->pending_len = 0;
        if (!command(s, CMD_PING, NULL, 0, timeout_ms)) {
            return false;
        }
        s->already_synced = true;
        return true;
    }
    return command(s, CMD_PING, NULL, 0, timeout_ms) || fail(s, "no answer to a ping");
}

bool sbl_connect(sbl_t *s, const char *expect_mac)
{
    // OWS_SUB_ENTER_SBL (AA 55 53 08 F0 05 "OSBL" 03 7F) for a running gateway + "\r\nsbl\r\n" for a sensor console
    static const uint8_t soft_sbl[19] = {
        0xAA, 0x55, 0x53, 0x08, 0xF0, 0x05, 'O', 'S', 'B', 'L', 0x03, 0x7F, '\r', '\n', 's', 'b', 'l', '\r', '\n',
    };
    s->error[0] = 0;
    if (!try_sync(s, 300)) {
        if (s->error[0]) {
            return false;
        }
        s->io.write(s->io.ctx, soft_sbl, sizeof soft_sbl);
        s->io.read(s->io.ctx, s->pending, sizeof s->pending, 50);
        if (!try_sync(s, 300)) {
            return s->error[0]
                       ? false
                       : fail(s,
                              "no bootloader (no ACK to the sync or a ping): R held low at the reset? bootloader on?");
        }
    }
    if (!sbl_get_chip_id(s, NULL)) {
        return false;
    }
    static uint8_t page31[SBL_PAGE];
    if (!sbl_read_memory(s, SBL_PAGE31, page31, sizeof page31)) {
        return fail(s, "reading page 31 failed");
    }
    const uint8_t *ccfg = page31 + (SBL_CCFG - SBL_PAGE31), *mac = ccfg + 0x24;
    for (int i = 0; i < 8; i++) {
        if (!((mac[i] >= '0' && mac[i] <= '9') || (mac[i] >= 'A' && mac[i] <= 'F'))) {
            return fail(s, "no MAC in the CCFG: not writing");
        }
    }
    if (memcmp(mac, "00000000", 8) == 0) {
        return fail(s, "no MAC in the CCFG: not writing");
    }
    memcpy(s->mac, mac, 8);
    s->mac[8] = 0;
    if (expect_mac && strcmp(expect_mac, s->mac) != 0) {
        snprintf(s->error, sizeof s->error, "the CCFG MAC is %s, not %s: not writing", s->mac, expect_mac);
        return false;
    }
    const uint8_t *bl = ccfg + 0x30;  // BL_CONFIG, little-endian: C5 <pin> FE C5 when enabled
    if (bl[0] != 0xC5 || bl[3] != 0xC5 || bl[1] >= 32 || (bl[2] & 1) != 0) {
        return fail(s, "the bootloader is not enabled in the CCFG");
    }
    s->backdoor_dio = bl[1];
    return true;
}

// pages [first, last] erased, then the image's stretches that are not all FF downloaded, one GET_STATUS each
static bool write_area(sbl_t *s, uint32_t base, const uint8_t *data, size_t len)
{
    for (uint32_t a = base; a < base + len; a += SBL_PAGE) {
        uint8_t p[4];
        be32(p, a);
        if (!command(s, CMD_SECTOR_ERASE, p, 4, 5000) || !status_ok(s)) {
            return s->error[0] ? false : fail(s, "erasing failed");
        }
    }
    for (size_t at = 0; at < len;) {
        while (at < len && !memcmp(data + at, "\xff\xff\xff\xff", 4)) {  // skip erased words
            at += 4;
        }
        size_t end = at;
        for (size_t gap = 0; end < len && gap < 256; end += 4) {  // a stretch: runs of FF under 256 bytes merged
            gap = memcmp(data + end, "\xff\xff\xff\xff", 4) ? 0 : gap + 4;
        }
        while (end > at && !memcmp(data + end - 4, "\xff\xff\xff\xff", 4)) {
            end -= 4;
        }
        if (end == at) {
            break;
        }
        uint8_t d[8];
        be32(d, base + (uint32_t)at), be32(d + 4, (uint32_t)(end - at));
        if (!command(s, CMD_DOWNLOAD, d, 8, 1000) || !status_ok(s)) {
            return s->error[0] ? false : fail(s, "download refused");
        }
        for (size_t off = at; off < end; off += MAX_DATA) {
            size_t n = end - off < MAX_DATA ? end - off : MAX_DATA;
            if (!command(s, CMD_SEND_DATA, data + off, n, 2000)) {
                return fail(s, "sending data failed");
            }
        }
        if (!status_ok(s)) {
            return fail(s, "programming failed");
        }
        at = end;
    }
    uint32_t crc;
    if (!sbl_chip_crc32(s, base, (uint32_t)len, &crc) || crc != crc32_ieee(data, len)) {
        return fail(s, "the chip's CRC32 of what was written differs");
    }
    return true;
}

static bool page31_crc(sbl_t *s, uint32_t *crc)
{
    return sbl_chip_crc32(s, SBL_PAGE31, SBL_PAGE, crc) || fail(s, "no CRC32 of page 31");
}

bool sbl_stream_begin(sbl_t *s)
{
    s->error[0] = 0;
    s->stream_at = 0;
    s->stream_crc = 0xFFFFFFFFu;
    return page31_crc(s, &s->stream_page31);
}

// Page 0 starts with a Cortex-M vector table, ours (an app or the boot selector): the stack pointer in the CC1310's
// 20 KB of SRAM, the reset handler a Thumb address in pages 0-29. Anything else (a file picked by mistake in a
// browser, an ESP image) is refused before a byte is written.
static bool vector_table(const uint8_t *p, size_t len)
{
    if (len < 8) {
        return false;
    }
    uint32_t sp = p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
    uint32_t reset = p[4] | (uint32_t)p[5] << 8 | (uint32_t)p[6] << 16 | (uint32_t)p[7] << 24;
    return sp > 0x20000000u && sp <= 0x20005000u && (reset & 1) && reset < SBL_APP_END;
}

bool sbl_stream_page(sbl_t *s, const uint8_t *data, size_t len)
{
    if (s->stream_at % SBL_PAGE) {
        return fail(s, "a page after a short one");
    }
    if (!len || len > SBL_PAGE || len % 4 || s->stream_at + len > SBL_APP_END) {
        return fail(s, "an image for pages 0-29, a multiple of 4 bytes");
    }
    if (!s->stream_at && !vector_table(data, len)) {
        return fail(s, "not a CC1310 image (no vector table at its start): nothing written");
    }
    if (!write_area(s, s->stream_at, data, len)) {
        return false;
    }
    s->stream_crc = sbl_crc32_update(s->stream_crc, data, len);
    s->stream_at += (uint32_t)len;
    return true;
}

bool sbl_stream_end(sbl_t *s)
{
    uint32_t crc, after;
    if (!s->stream_at) {
        return fail(s, "an empty image");
    }
    if (!sbl_chip_crc32(s, 0, s->stream_at, &crc) || crc != ~s->stream_crc) {
        return fail(s, "the chip's CRC32 of the whole image differs");
    }
    return page31_crc(s, &after) && (after == s->stream_page31 || fail(s, "page 31 changed!"));
}

bool sbl_flash(sbl_t *s, const uint8_t *image, size_t len)
{
    if (!len || len % 4 || len > SBL_APP_END) {
        s->error[0] = 0;
        return fail(s, "an image for pages 0-29, a multiple of 4 bytes");
    }
    if (!sbl_stream_begin(s)) {
        return false;
    }
    for (size_t at = 0; at < len; at += SBL_PAGE) {
        if (!sbl_stream_page(s, image + at, len - at < SBL_PAGE ? len - at : SBL_PAGE)) {
            return false;
        }
    }
    return sbl_stream_end(s);
}

bool sbl_write_nv(sbl_t *s, const uint8_t *page)
{
    uint32_t before, after;
    s->error[0] = 0;
    if (!page31_crc(s, &before)) {
        return false;
    }
    nv_writing = true;
    bool ok = write_area(s, SBL_NV_PAGE, page, SBL_PAGE);
    nv_writing = false;
    return ok && page31_crc(s, &after) && (before == after || fail(s, "page 31 changed!"));
}

bool sbl_slots_image(uint8_t *out, const uint8_t *boot, size_t boot_len, const uint8_t *slot, size_t slot_len)
{
    if (!boot_len || boot_len > SBL_SLOT_A || slot_len < 4 || slot_len > SBL_SLOT_B - SBL_SLOT_A ||
        memcmp(slot, "OWSI", 4) != 0) {
        return false;
    }
    memset(out, 0xFF, SBL_SLOTS_LEN);
    memcpy(out, boot, boot_len);
    memcpy(out + SBL_SLOT_A, slot, slot_len);
    return true;
}

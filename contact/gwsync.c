#include "gwsync.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "host_proto.h"

#define BOOT_MS           300  // after a reset, before the gateway is asked
#define TRIES             3
#define WAIT_MS           200  // a read's wait; an answer has WAITS of them
#define MAC_WAIT_MS       250
#define WAITS             3
#define BYTES_MAX         256   // a try also ends after this many bytes: a chattering line never times out
#define SEARCH            1024  // the block is in the image's first 1 KB (common/build_info.h)
#define BUILD_INFO_P2_LEN 27

bool gwsync_find_info(const uint8_t *head, size_t len, build_info_t *out)
{
    for (size_t off = 0; off + sizeof *out <= len && off < SEARCH; off += 4) {
        build_info_t b;
        memcpy(&b, head + off, sizeof b);
        if (b.magic == BUILD_INFO_MAGIC && b.format == BUILD_INFO_FORMAT) {
            *out = b;
            out->product[sizeof out->product - 1] = out->board[sizeof out->board - 1] = 0;
            out->app[sizeof out->app - 1] = out->version[sizeof out->version - 1] = 0;
            return true;
        }
    }
    return false;
}

static void step(const gwsync_io_t *io, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void step(const gwsync_io_t *io, const char *fmt, ...)
{
    if (io->progress) {
        char s[120];
        va_list a;
        va_start(a, fmt);
        vsnprintf(s, sizeof s, fmt, a);
        va_end(a);
        io->progress(io->ctx, s);
    }
}

static bool unpack_page0(const uint8_t *p, size_t n, build_info_t *out)
{
    if (n != OWS_BUILD_INFO_P0_LEN || p[0] != OWS_SUB_BUILD_INFO) {
        return false;
    }
    build_info_t b;
    memset(&b, 0, sizeof b);
    memcpy(&b, p + 1, 40);  // header(8) + id(8) + product(24)
    memcpy(b.app, p + 41, sizeof b.app);
    if (b.magic != BUILD_INFO_MAGIC || b.format != BUILD_INFO_FORMAT) {
        return false;
    }
    b.product[sizeof b.product - 1] = 0;
    b.app[sizeof b.app - 1] = 0;
    *out = b;
    return true;
}

static bool unpack_page1(const uint8_t *p, size_t n, build_info_t *out)
{
    if (n != OWS_BUILD_INFO_P1_LEN || p[0] != OWS_SUB_BUILD_INFO || memcmp(p + 1, out->id, sizeof out->id) != 0) {
        return false;
    }
    memcpy(out->board, p + 9, sizeof out->board);
    out->board[sizeof out->board - 1] = 0;
    memset(out->version, 0, sizeof out->version);
    memcpy(out->version, p + 33, 22);
    out->version[22] = 0;
    return true;
}

static bool unpack_page2(const uint8_t *p, size_t n, build_info_t *out)
{
    if (n != BUILD_INFO_P2_LEN || p[0] != OWS_SUB_BUILD_INFO || memcmp(p + 1, out->id, sizeof out->id) != 0) {
        return false;
    }
    memcpy(out->version + 22, p + 9, 18);
    out->version[sizeof out->version - 1] = 0;
    return true;
}

static bool query_page(const gwsync_io_t *io, uint8_t part, build_info_t *out, uint32_t *junk, int tries)
{
    uint8_t req[2] = {OWS_SUB_BUILD_INFO, part};
    uint8_t ask[16], ack[16], buf[64];
    size_t n_ask = host_build(ask, HOST_OWS_CMD, req, sizeof req, false);
    size_t n_ack = host_build_ack(ack, HOST_OWS_RSP, false);
    for (int t = 0; t < tries; t++) {
        host_rx_t rx;
        host_rx_init(&rx);
        io->flush_input(io->ctx);
        if (!io->write(io->ctx, ask, n_ask)) {
            return false;
        }
        if (part == 0) {
            step(io, "asking the gateway what it runs (try %d of %d)", t + 1, tries);
        }
        for (size_t w = 0, got = 0; w < WAITS && got < BYTES_MAX;) {
            size_t n = io->read(io->ctx, buf, sizeof buf, WAIT_MS);
            if (!n) {
                w++;
                continue;
            }
            got += n;
            *junk += (uint32_t)n;
            for (size_t i = 0; i < n; i++) {
                host_pkt_t p;
                if (!host_rx_feed(&rx, buf[i], &p) || !p.to_host || p.cmd != HOST_OWS_RSP) {
                    continue;
                }
                io->write(io->ctx, ack, n_ack);  // else it sends it again
                bool ok = false;
                if (part == 0) {
                    ok = unpack_page0(p.payload, p.len, out);
                } else if (part == 1) {
                    ok = unpack_page1(p.payload, p.len, out);
                } else if (part == 2) {
                    ok = unpack_page2(p.payload, p.len, out);
                }
                if (ok) {
                    *junk -= (uint32_t)(n < p.len + 7 ? n : p.len + 7);  // about: the answer's own bytes
                    return true;
                }
                if (part == 2 && p.len >= 1 && p.payload[0] == OWS_SUB_BUILD_INFO) {
                    *junk -= (uint32_t)(n < p.len + 7 ? n : p.len + 7);
                    return false;
                }
            }
        }
    }
    return false;
}

static bool query_mac(const gwsync_io_t *io, char mac[9], uint32_t *junk)
{
    uint8_t ask[16], ack[16], buf[64];
    size_t n_ask = host_build(ask, 0x4304, NULL, 0, false);
    size_t n_ack = host_build_ack(ack, 0x4305, false);
    host_rx_t rx;
    host_rx_init(&rx);
    io->flush_input(io->ctx);
    if (!io->write(io->ctx, ask, n_ask)) {
        return false;
    }
    for (size_t got = 0; got < BYTES_MAX;) {
        size_t n = io->read(io->ctx, buf, sizeof buf, MAC_WAIT_MS);
        if (!n) {
            break;
        }
        got += n;
        *junk += (uint32_t)n;
        for (size_t i = 0; i < n; i++) {
            host_pkt_t p;
            if (!host_rx_feed(&rx, buf[i], &p) || !p.to_host || p.cmd != 0x4305) {
                continue;
            }
            io->write(io->ctx, ack, n_ack);
            if (p.len < 8) {
                return false;
            }
            char tmp[9];
            for (int k = 0; k < 8; k++) {
                uint8_t c = p.payload[k];
                if (c >= 'a' && c <= 'f') {
                    c = (uint8_t)(c - 'a' + 'A');
                }
                if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) {
                    return false;
                }
                tmp[k] = (char)c;
            }
            tmp[8] = '\0';
            if (memcmp(tmp, "00000000", 8) == 0) {
                return false;
            }
            memcpy(mac, tmp, 9);
            *junk -= (uint32_t)(n < p.len + 7 ? n : p.len + 7);
            return true;
        }
    }
    return false;
}

bool gwsync_query(const gwsync_io_t *io, build_info_t *out)
{
    uint32_t junk = 0;
    if (!query_page(io, 0, out, &junk, TRIES)) {
        return false;
    }
    if (query_page(io, 1, out, &junk, TRIES) && !query_page(io, 2, out, &junk, 1)) {
        out->version[22] = '\0';
    }
    return true;
}

// Software SBL entry (OWS_SUB_ENTER_SBL, 0x53F0 0x05 "OSBL"): works on 4-wire ESP builds without W soldered
static bool enter_soft_sbl(const gwsync_io_t *io)
{
    static const uint8_t req[5] = {OWS_SUB_ENTER_SBL, 'O', 'S', 'B', 'L'};
    uint8_t ask[16], ack[16], buf[64];
    size_t n_ask = host_build(ask, HOST_OWS_CMD, req, sizeof req, false);
    size_t n_ack = host_build_ack(ack, HOST_OWS_RSP, false);
    host_rx_t rx;
    host_rx_init(&rx);
    io->flush_input(io->ctx);
    if (!io->write(io->ctx, ask, n_ask)) {
        return false;
    }
    for (size_t got = 0; got < BYTES_MAX;) {
        size_t n = io->read(io->ctx, buf, sizeof buf, WAIT_MS);
        if (!n) {
            break;
        }
        got += n;
        for (size_t i = 0; i < n; i++) {
            host_pkt_t p;
            if (!host_rx_feed(&rx, buf[i], &p) || !p.to_host || p.cmd != HOST_OWS_RSP) {
                continue;
            }
            if (p.len >= 2 && p.payload[0] == OWS_SUB_ENTER_SBL) {
                if (p.payload[1] != 0x00) {
                    io->write(io->ctx, ack, n_ack);
                    return false;
                }
                // Do not send HOST_ACK on 0x00: the gateway is already jumping to ROM SBL, whose auto-baud detector
                // must see 0x55 0x55 as its very first RX edges.
                io->sleep_ms(io->ctx, 30);
                io->flush_input(io->ctx);
                return true;
            }
            io->write(io->ctx, ack, n_ack);
        }
    }
    return false;
}

static void id_hex(char *out, const uint8_t id[8])
{
    for (int i = 0; i < 8; i++) {
        snprintf(out + (size_t)2 * (size_t)i, 3, "%02x", id[i]);
    }
}

static gwsync_result_t done(const gwsync_io_t *io, gwsync_report_t *r, gwsync_result_t res, bool reset)
{
    if (reset) {
        io->reset(io->ctx);  // into its firmware: the bootloader's own reset would leave it running nothing
    }
    r->result = res;
    return res;
}

// the carried image against the chip's flash, by the bootloader's CRC32
static bool same_by_crc(const gwsync_io_t *io, uint32_t len, sbl_t *sbl, uint8_t *page, bool *same)
{
    uint32_t c = 0xFFFFFFFFu, chip;
    for (uint32_t off = 0; off < len; off += SBL_PAGE) {
        size_t n = len - off < SBL_PAGE ? len - off : SBL_PAGE;
        if (!io->image(io->ctx, off, page, n)) {
            return false;
        }
        c = sbl_crc32_update(c, page, n);
    }
    if (!sbl_chip_crc32(sbl, 0, len, &chip)) {
        return false;
    }
    *same = chip == ~c;
    return true;
}

static bool flash(const gwsync_io_t *io, uint32_t len, sbl_t *sbl, uint8_t *page)
{
    if (!sbl_stream_begin(sbl)) {
        return false;
    }
    for (uint32_t off = 0; off < len; off += SBL_PAGE) {
        size_t n = len - off < SBL_PAGE ? len - off : SBL_PAGE;
        step(io, "writing page %u of %u", (unsigned)(off / SBL_PAGE + 1), (unsigned)((len + SBL_PAGE - 1) / SBL_PAGE));
        if (!io->image(io->ctx, off, page, n)) {
            snprintf(sbl->error, sizeof sbl->error, "reading the carried image failed");
            return false;
        }
        if (!sbl_stream_page(sbl, page, n)) {
            return false;
        }
    }
    return sbl_stream_end(sbl);
}

static gwsync_result_t check_new(const gwsync_io_t *io, gwsync_report_t *r, const char *want);

gwsync_result_t gwsync_run(const gwsync_io_t *io, uint32_t image_len, sbl_t *sbl, uint8_t *page, gwsync_report_t *r)
{
    char want[17];
    memset(r, 0, sizeof *r);
    size_t head = image_len < SEARCH ? image_len : SEARCH;
    if (!image_len || image_len % 4 || image_len > SBL_APP_END || !io->image(io->ctx, 0, page, head) ||
        !gwsync_find_info(page, head, &r->carried) || strcmp(r->carried.app, "gateway") != 0) {
        snprintf(r->msg, sizeof r->msg, "the carried image is not a gateway image with a build info block");
        return done(io, r, GWSYNC_REFUSED, false);
    }
    id_hex(want, r->carried.id);
    io->reset(io->ctx);
    io->sleep_ms(io->ctx, BOOT_MS);
    r->answered = query_page(io, 0, &r->running, &r->junk, TRIES);
    if (r->answered && !memcmp(r->running.id, r->carried.id, sizeof r->carried.id)) {
        // the same code (the image ID leaves the version string out): nothing to flash. What it reports is its own
        // build's version (pages 1-2), which can differ from the carried one's (#55): the carried one's if it does not say
        if (query_page(io, 1, &r->running, &r->junk, 1)) {
            if (!query_page(io, 2, &r->running, &r->junk, 1)) {
                r->running.version[22] = '\0';
            }
        } else {
            memcpy(r->running.board, r->carried.board, sizeof r->running.board);
            memcpy(r->running.version, r->carried.version, sizeof r->running.version);
        }
        query_mac(io, r->mac, &r->junk);
        if (strcmp(r->running.version, r->carried.version) != 0) {
            snprintf(r->msg, sizeof r->msg, "runs the carried gateway's code (%s), built as %s (carried: %s)", want,
                     r->running.version, r->carried.version);
        } else {
            snprintf(r->msg, sizeof r->msg, "runs the carried gateway %s (%s)", r->carried.version, want);
        }
        return done(io, r, GWSYNC_SAME, false);
    }

    // the debug port first (#81): it does not depend on the contact's firmware or on a bootloader backdoor
    if (io->transport != GWSYNC_VIA_SBL && io->cjtag_flash) {
        char err[96] = "";
        step(io, "%s: through its debug port (cJTAG)", r->answered ? "another gateway" : "no answer");
        int written = io->cjtag_flash(io->ctx, image_len, r->mac, err, sizeof err);
        if (written == 0) {
            r->via_cjtag = true;
            snprintf(r->msg, sizeof r->msg, "the carried gateway %s (%s) is there (cJTAG), but it did not answer",
                     r->carried.version, want);
            return done(io, r, GWSYNC_SAME, false);
        }
        if (written > 0) {
            r->via_cjtag = true;
            return check_new(io, r, want);
        }
        step(io, "debug port: %s", err);
        if (io->transport == GWSYNC_VIA_CJTAG) {
            snprintf(r->msg, sizeof r->msg, "not flashed through the debug port: %s", err);
            return done(io, r, GWSYNC_FAILED, false);
        }
    } else if (io->transport == GWSYNC_VIA_CJTAG) {
        snprintf(r->msg, sizeof r->msg, "transport cjtag, but no debug port is wired");
        return done(io, r, GWSYNC_REFUSED, false);
    }

    sbl_io_t sio = {io->write, io->read, io->flush_input, io->ctx};
    sbl_init(sbl, &sio);
    bool connected = false;
    if (r->answered) {
        if (query_page(io, 1, &r->running, &r->junk, 1) && !query_page(io, 2, &r->running, &r->junk, 1)) {
            r->running.version[22] = '\0';
        }
        step(io, "it runs %s: into its bootloader", r->running.version[0] ? r->running.version : r->running.product);
        if (enter_soft_sbl(io)) {
            connected = sbl_connect(sbl, NULL);
        }
    } else {
        step(io, "no answer (%lu other bytes): into its bootloader", (unsigned long)r->junk);
    }
    if (!connected) {
        sbl_init(sbl, &sio);
        io->enter_bootloader(io->ctx);
        connected = sbl_connect(sbl, NULL);
    }
    if (!connected) {
        snprintf(r->msg, sizeof r->msg, "%s; cannot update it: %s", r->answered ? "another gateway" : "no answer",
                 sbl->error);
        return done(io, r, GWSYNC_REFUSED, true);
    }
    memcpy(r->mac, sbl->mac, sizeof r->mac);
    step(io, "bootloader: MAC %s, backdoor DIO%u", sbl->mac, sbl->backdoor_dio);
    bool same = false;
    if (!r->answered) {
        step(io, "comparing its flash with the carried image (CRC)");
    }
    if (!r->answered && same_by_crc(io, image_len, sbl, page, &same) && same) {
        snprintf(r->msg, sizeof r->msg, "the carried gateway %s (%s) is there (CRC), but it did not answer",
                 r->carried.version, want);
        return done(io, r, GWSYNC_SAME, true);
    }
    if (!flash(io, image_len, sbl, page)) {
        snprintf(r->msg, sizeof r->msg, "not flashed: %s", sbl->error);
        return done(io, r, GWSYNC_FAILED, true);
    }
    io->reset(io->ctx);
    return check_new(io, r, want);
}

// After a reflash (and a reset into it): the new gateway must answer with the carried image's ID
static gwsync_result_t check_new(const gwsync_io_t *io, gwsync_report_t *r, const char *want)
{
    char have[17];
    io->sleep_ms(io->ctx, BOOT_MS);
    step(io, "written: asking the new gateway");
    uint32_t junk = 0;
    bool answered = query_page(io, 0, &r->running, &junk, TRIES);
    if (!answered || memcmp(r->running.id, r->carried.id, sizeof r->carried.id) != 0) {
        id_hex(have, r->running.id);
        snprintf(r->msg, sizeof r->msg, "flashed, but it answers %s", answered ? have : "nothing");
        return done(io, r, GWSYNC_FAILED, false);
    }
    if (r->mac[0] == '\0') {
        query_mac(io, r->mac, &junk);
    }
    memcpy(r->running.board, r->carried.board, sizeof r->running.board);
    memcpy(r->running.version, r->carried.version, sizeof r->running.version);
    snprintf(r->msg, sizeof r->msg, "updated to the carried gateway %s (%s)%s", r->carried.version, want,
             r->via_cjtag ? " through the debug port" : "");
    return done(io, r, GWSYNC_UPDATED, false);
}

gwsync_result_t gwsync_probe(const gwsync_io_t *io, uint32_t image_len, uint8_t *page, gwsync_report_t *r)
{
    memset(r, 0, sizeof *r);
    if (image_len && image_len % 4 == 0 && image_len <= SBL_APP_END) {
        size_t head = image_len < SEARCH ? image_len : SEARCH;
        if (io->image(io->ctx, 0, page, head)) {
            gwsync_find_info(page, head, &r->carried);
        }
    }
    step(io, "auto-sync paused: probing running gateway");
    io->reset(io->ctx);
    io->sleep_ms(io->ctx, BOOT_MS);
    r->answered = query_page(io, 0, &r->running, &r->junk, TRIES);
    if (r->answered) {
        char have[17];
        if (query_page(io, 1, &r->running, &r->junk, 1) && !query_page(io, 2, &r->running, &r->junk, 1)) {
            r->running.version[22] = '\0';
        }
        query_mac(io, r->mac, &r->junk);
        id_hex(have, r->running.id);
        snprintf(r->msg, sizeof r->msg, "auto-sync paused; runs %s (%s)",
                 r->running.version[0] ? r->running.version : r->running.product, have);
    } else {
        snprintf(r->msg, sizeof r->msg, "auto-sync paused; no answer (%lu other bytes)", (unsigned long)r->junk);
    }
    r->result = GWSYNC_SAME;
    return GWSYNC_SAME;
}

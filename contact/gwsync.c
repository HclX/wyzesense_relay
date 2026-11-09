#include "gwsync.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "host_proto.h"

#define BOOT_MS           300   // after a reset, before the gateway is asked
#define STOCK_BOOT_MS     2000  // the stock gateway (TI-RTOS) answers ~1.5 s after its start (9A78, 2026-11-08)
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

static bool parse_hex_mac(const uint8_t *payload, size_t len, char mac[9])
{
    if (len < 8) {
        return false;
    }
    char tmp[9];
    for (int k = 0; k < 8; k++) {
        uint8_t c = payload[k];
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
    return true;
}

static bool query_mac_cmd(const gwsync_io_t *io, uint16_t ask_cmd, char mac[9], uint32_t *junk)
{
    uint16_t rsp_cmd = (uint16_t)(ask_cmd + 1);
    uint8_t ask[16], ack[16], buf[64];
    size_t n_ask = host_build(ask, ask_cmd, NULL, 0, false);
    size_t n_ack = host_build_ack(ack, rsp_cmd, false);
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
            if (!host_rx_feed(&rx, buf[i], &p) || !p.to_host || p.cmd != rsp_cmd) {
                continue;
            }
            io->write(io->ctx, ack, n_ack);
            if (!parse_hex_mac(p.payload, p.len, mac)) {
                return false;
            }
            *junk -= (uint32_t)(n < p.len + 7 ? n : p.len + 7);
            return true;
        }
    }
    return false;
}

static bool query_mac(const gwsync_io_t *io, char mac[9], uint32_t *junk)
{
    return query_mac_cmd(io, 0x4304, mac, junk);
}

static bool query_stock_mac(const gwsync_io_t *io, char mac[9], uint32_t *junk)
{
    return query_mac_cmd(io, 0x5304, mac, junk);
}

static bool write_stock_mac(const gwsync_io_t *io, const char mac[9])
{
    if (!mac || strlen(mac) != 8) {
        return false;
    }
    uint8_t ask[24], ack[16], buf[64];
    size_t n_ask = host_build(ask, 0x5308, (const uint8_t *)mac, 8, false);
    size_t n_ack = host_build_ack(ack, 0x5309, false);
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
        for (size_t i = 0; i < n; i++) {
            host_pkt_t p;
            if (!host_rx_feed(&rx, buf[i], &p) || !p.to_host || p.cmd != 0x5309) {
                continue;
            }
            io->write(io->ctx, ack, n_ack);
            return true;
        }
    }
    return false;
}

// Stock gateway firmware (no OWS_SUB_BUILD_INFO): answers 0x5316 (HOST_GET_VERSION) with 0x5317 ("0.0.0.47 V1.8 Gateway GW3U")
static bool query_stock(const gwsync_io_t *io, build_info_t *out, uint32_t *junk, int tries)
{
    uint8_t ask[16], ack[16], buf[64];
    size_t n_ask = host_build(ask, HOST_GET_VERSION, NULL, 0, false);
    size_t n_ack = host_build_ack(ack, HOST_GET_VERSION + 1, false);
    for (int t = 0; t < tries; t++) {
        host_rx_t rx;
        host_rx_init(&rx);
        io->flush_input(io->ctx);
        if (!io->write(io->ctx, ask, n_ask)) {
            return false;
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
                if (!host_rx_feed(&rx, buf[i], &p) || !p.to_host || p.cmd != HOST_GET_VERSION + 1 || p.len < 1) {
                    continue;
                }
                io->write(io->ctx, ack, n_ack);
                memset(out, 0, sizeof *out);
                out->magic = BUILD_INFO_MAGIC;
                out->format = BUILD_INFO_FORMAT;
                snprintf(out->product, sizeof out->product, "gateway-stock");
                snprintf(out->board, sizeof out->board, "Wyze Sense v1 contact");
                snprintf(out->app, sizeof out->app, "gateway");
                size_t vlen = p.len < sizeof out->version - 1 ? p.len : sizeof out->version - 1;
                for (size_t k = 0; k < vlen && p.payload[k] >= 0x20 && p.payload[k] < 0x7F; k++) {
                    out->version[k] = (char)p.payload[k];
                }
                *junk -= (uint32_t)(n < p.len + 7 ? n : p.len + 7);
                return out->version[0] != '\0';
            }
        }
    }
    return false;
}

static bool is_stock_image(const gwsync_io_t *io, uint32_t image_len, uint8_t *page, build_info_t *out)
{
    if (image_len <= GWSYNC_PAGE31_CODE_LEN) {
        return false;
    }
    uint32_t app_len = image_len - GWSYNC_PAGE31_CODE_LEN;
    if (app_len % SBL_PAGE != 0 || app_len > SBL_APP_END) {
        return false;
    }
    uint32_t sp = page[0] | (uint32_t)page[1] << 8 | (uint32_t)page[2] << 16 | (uint32_t)page[3] << 24;
    uint32_t reset = page[4] | (uint32_t)page[5] << 8 | (uint32_t)page[6] << 16 | (uint32_t)page[7] << 24;
    if (!(sp > 0x20000000u && sp <= 0x20005000u && (reset & 1) && reset < SBL_APP_END)) {
        return false;
    }
    uint32_t c_app = 0xFFFFFFFFu;
    for (uint32_t off = 0; off < app_len; off += SBL_PAGE) {
        if (!io->image(io->ctx, off, page, SBL_PAGE)) {
            return false;
        }
        c_app = sbl_crc32_update(c_app, page, SBL_PAGE);
    }
    if (!io->image(io->ctx, app_len, page, GWSYNC_PAGE31_CODE_LEN)) {
        return false;
    }
    uint32_t c_p31 = ~sbl_crc32_update(0xFFFFFFFFu, page, GWSYNC_PAGE31_CODE_LEN);
    c_app = ~c_app;
    memset(out, 0, sizeof *out);
    out->magic = BUILD_INFO_MAGIC;
    out->format = BUILD_INFO_FORMAT;
    for (int i = 0; i < 4; i++) {
        out->id[i] = (uint8_t)(c_app >> (24 - 8 * i));
        out->id[4 + i] = (uint8_t)(c_p31 >> (24 - 8 * i));
    }
    snprintf(out->product, sizeof out->product, "gateway-stock");
    snprintf(out->board, sizeof out->board, "Wyze Sense v1 contact");
    snprintf(out->app, sizeof out->app, "gateway");
    snprintf(out->version, sizeof out->version, "0.0.0.47 V1.8 Gateway GW3U");
    return true;
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

static gwsync_result_t check_new(const gwsync_io_t *io, gwsync_report_t *r, const char *want, bool carried_stock);

gwsync_result_t gwsync_run(const gwsync_io_t *io, uint32_t image_len, sbl_t *sbl, uint8_t *page, gwsync_report_t *r)
{
    char want[17];
    memset(r, 0, sizeof *r);
    size_t head = image_len < SEARCH ? image_len : SEARCH;
    bool carried_ows = image_len > 0 && image_len % 4 == 0 && image_len <= SBL_APP_END &&
                       io->image(io->ctx, 0, page, head) && gwsync_find_info(page, head, &r->carried) &&
                       strcmp(r->carried.app, "gateway") == 0;
    bool carried_stock = !carried_ows && image_len >= 8 && io->image(io->ctx, 0, page, 8) &&
                         is_stock_image(io, image_len, page, &r->carried);
    if (!carried_ows && !carried_stock) {
        snprintf(r->msg, sizeof r->msg, "the carried image is not a gateway image with a build info block");
        return done(io, r, GWSYNC_REFUSED, false);
    }
    uint32_t app_len = carried_stock ? image_len - GWSYNC_PAGE31_CODE_LEN : image_len;
    id_hex(want, r->carried.id);
    io->reset(io->ctx);
    io->sleep_ms(io->ctx, BOOT_MS);
    r->answered = query_page(io, 0, &r->running, &r->junk, carried_stock ? 1 : TRIES);
    if (!r->answered && carried_stock && query_stock(io, &r->running, &r->junk, TRIES)) {
        r->answered = true;
        r->stock = true;
        memcpy(r->running.id, r->carried.id, sizeof r->running.id);
    }
    // A stock image is never taken as there from the UART: our gateway answers 0x5316 with the stock version string
    // too, and the stock one has no build info to ask. Its flash is compared through the debug port below instead.
    if (!carried_stock && r->answered && !memcmp(r->running.id, r->carried.id, sizeof r->carried.id)) {
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
        if (written == 0 && carried_stock) {
            // the flash already holds the stock image (pages 0-23 and page 31's code, compared through the debug
            // port): only its identity is checked (the contact's MAC in the stock NV)
            r->via_cjtag = true;
            gwsync_result_t res = check_new(io, r, want, true);
            if (res == GWSYNC_UPDATED) {
                snprintf(r->msg, sizeof r->msg,
                         "runs the carried stock gateway %s (%s), checked through the debug port", r->running.version,
                         want);
                return done(io, r, GWSYNC_SAME, false);
            }
            return res;
        }
        if (written == 0) {
            r->via_cjtag = true;
            r->stock = carried_stock;
            snprintf(r->msg, sizeof r->msg, "the carried gateway %s (%s) is there (cJTAG), but it did not answer",
                     r->carried.version, want);
            return done(io, r, GWSYNC_SAME, false);
        }
        if (written > 0) {
            r->via_cjtag = true;
            if (err[0]) {
                step(io, "debug port wrote %s", err);
            }
            return check_new(io, r, want, carried_stock);
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

    if (carried_stock) {
        // the bootloader writes pages 0-29 only: the stock image's page-31 code would be left out, and the stock
        // gateway does not start without it. Only the debug port (cJTAG, M/C wired) writes it, under the CCFG rule
        snprintf(r->msg, sizeof r->msg, "not flashed: the stock gateway needs the debug port (page 31)");
        return done(io, r, GWSYNC_FAILED, false);
    }
    sbl_io_t sio = {io->write, io->read, io->flush_input, io->ctx};
    sbl_init(sbl, &sio);
    bool connected = false;
    if (r->answered) {
        if (!r->stock && query_page(io, 1, &r->running, &r->junk, 1) && !query_page(io, 2, &r->running, &r->junk, 1)) {
            r->running.version[22] = '\0';
        }
        step(io, "it runs %s: into its bootloader", r->running.version[0] ? r->running.version : r->running.product);
        if (!r->stock && enter_soft_sbl(io)) {
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
    if (!r->answered && same_by_crc(io, app_len, sbl, page, &same) && same) {
        r->stock = carried_stock;
        snprintf(r->msg, sizeof r->msg, "the carried gateway %s (%s) is there (CRC), but it did not answer",
                 r->carried.version, want);
        return done(io, r, GWSYNC_SAME, true);
    }
    if (!flash(io, app_len, sbl, page)) {
        snprintf(r->msg, sizeof r->msg, "not flashed: %s", sbl->error);
        return done(io, r, GWSYNC_FAILED, true);
    }
    io->reset(io->ctx);
    return check_new(io, r, want, carried_stock);
}

// After a reflash (and a reset into it): the new gateway must answer with the carried image's ID
static gwsync_result_t check_new(const gwsync_io_t *io, gwsync_report_t *r, const char *want, bool carried_stock)
{
    char have[17];
    io->sleep_ms(io->ctx, BOOT_MS);
    step(io, "written: asking the new gateway");
    uint32_t junk = 0;
    if (carried_stock) {
        io->reset(io->ctx);  // a start of its own, as a plain restart gives it (the one after the debug port may not)
        io->sleep_ms(io->ctx, STOCK_BOOT_MS);
        if (!query_stock(io, &r->running, &junk, TRIES)) {
            snprintf(r->msg, sizeof r->msg, "flashed, but the stock gateway did not answer");
            return done(io, r, GWSYNC_FAILED, false);
        }
        r->stock = true;
        memcpy(r->running.id, r->carried.id, sizeof r->running.id);
        char nv_mac[9] = "";
        query_stock_mac(io, nv_mac, &junk);
        if (r->mac[0] != '\0' && strcmp(nv_mac, r->mac) != 0) {
            write_stock_mac(io, r->mac);
        } else if (r->mac[0] == '\0' && nv_mac[0] != '\0') {
            memcpy(r->mac, nv_mac, sizeof r->mac);
        }
        snprintf(r->msg, sizeof r->msg, "updated to the carried stock gateway %s (%s)%s", r->running.version, want,
                 r->via_cjtag ? " through the debug port" : "");
        return done(io, r, GWSYNC_UPDATED, false);
    }
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
    } else if (image_len > GWSYNC_PAGE31_CODE_LEN && io->image(io->ctx, 0, page, 8)) {
        is_stock_image(io, image_len, page, &r->carried);
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
    } else if (query_stock(io, &r->running, &r->junk, TRIES)) {
        r->answered = true;
        r->stock = true;
        query_stock_mac(io, r->mac, &r->junk);
        snprintf(r->msg, sizeof r->msg, "auto-sync paused; runs stock %s", r->running.version);
    } else {
        snprintf(r->msg, sizeof r->msg, "auto-sync paused; no answer (%lu other bytes)", (unsigned long)r->junk);
    }
    r->result = GWSYNC_SAME;
    return GWSYNC_SAME;
}

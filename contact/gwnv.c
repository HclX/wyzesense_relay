#include "gwnv.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "host_proto.h"

#define TRIES     3
#define WAIT_MS   200
#define WAITS     3
#define BYTES_MAX 256

static bool fail(char *err, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

static bool fail(char *err, size_t cap, const char *fmt, ...)
{
    if (err && cap) {
        va_list a;
        va_start(a, fmt);
        vsnprintf(err, cap, fmt, a);
        va_end(a);
    }
    return false;
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

static bool parse_hex(const char *s, size_t min_len, size_t max_len, uint64_t *out)
{
    size_t n = strlen(s);
    if (n < min_len || n > max_len) {
        return false;
    }
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++) {
        int d = hex_nibble(s[i]);
        if (d < 0) {
            return false;
        }
        v = v << 4 | (uint64_t)d;
    }
    *out = v;
    return true;
}

static bool parse_mac(const char *s, char out[9])
{
    if (strlen(s) != 8) {
        return false;
    }
    for (int i = 0; i < 8; i++) {
        char c = s[i];
        if (c >= 'a' && c <= 'f') {
            c = (char)(c - 'a' + 'A');
        }
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) {
            return false;
        }
        out[i] = c;
    }
    out[8] = 0;
    return memcmp(out, "00000000", 8) != 0;
}

static bool validate_doc(gwnv_doc_t *doc, char *err, size_t err_cap)
{
    if (doc->pan == 0x0000 || doc->pan == 0xFFFF) {
        return fail(err, err_cap, "PAN %04X is reserved", doc->pan);
    }
    if (doc->short_addr >= 0xFFFE) {
        return fail(err, err_cap, "coordinator short address %04X is reserved", doc->short_addr);
    }
    if (doc->n_sensors > GWNV_MAX_SENSORS) {
        return fail(err, err_cap, "too many sensors (%u > %u)", (unsigned)doc->n_sensors, GWNV_MAX_SENSORS);
    }
    for (size_t i = 0; i < doc->n_sensors; i++) {
        char norm[9];
        gwnv_sensor_t *s = &doc->sensors[i];
        if (!parse_mac(s->mac, norm)) {
            return fail(err, err_cap, "sensor %u: invalid MAC", (unsigned)i);
        }
        if (memcmp(s->mac, norm, 8) != 0) {
            memcpy(s->mac, norm, 8);
        }
        if (s->mac[8] != '\0') {
            s->mac[8] = '\0';
        }
        if (s->short_addr == 0x0000 || s->short_addr >= 0xFFFE || s->short_addr == doc->short_addr) {
            return fail(err, err_cap, "sensor %.8s: unusable short address %04X", norm, s->short_addr);
        }
        for (size_t j = 0; j < i; j++) {
            const gwnv_sensor_t *o = &doc->sensors[j];
            if (!memcmp(o->mac, norm, 8)) {
                return fail(err, err_cap, "duplicate sensor MAC %.8s", norm);
            }
            if (o->short_addr == s->short_addr) {
                return fail(err, err_cap, "duplicate sensor short address %04X", s->short_addr);
            }
            if (o->sub == s->sub) {
                return fail(err, err_cap, "duplicate sensor sub-ID %u", s->sub);
            }
        }
    }
    return true;
}

// --- compact recursive-descent JSON reader for openwyzesense-nv-json v1 ---

typedef struct {
    const char *p;
    const char *end;
} jcur_t;

static void j_ws(jcur_t *c)
{
    while (c->p < c->end && (*c->p == ' ' || *c->p == '\t' || *c->p == '\r' || *c->p == '\n')) {
        c->p++;
    }
}

static bool j_take(jcur_t *c, char ch)
{
    j_ws(c);
    if (c->p < c->end && *c->p == ch) {
        c->p++;
        return true;
    }
    return false;
}

static bool j_str(jcur_t *c, char *out, size_t cap)
{
    j_ws(c);
    if (c->p >= c->end || *c->p != '"') {
        return false;
    }
    c->p++;
    size_t n = 0;
    while (c->p < c->end) {
        char ch = *c->p++;
        if (ch == '"') {
            if (out && cap) {
                out[n < cap ? n : cap - 1] = 0;
            }
            return !out || n < cap;
        }
        if ((unsigned char)ch < 0x20) {
            return false;
        }
        if (ch == '\\') {
            if (c->p >= c->end) {
                return false;
            }
            char esc = *c->p++;
            if (esc == 'u') {
                if (c->end - c->p < 4) {
                    return false;
                }
                c->p += 4;
                ch = '?';
            } else {
                ch = esc;
            }
        }
        if (out && n + 1 < cap) {
            out[n] = ch;
        }
        n++;
    }
    return false;
}

static bool j_uint(jcur_t *c, uint32_t *out)
{
    j_ws(c);
    if (c->p >= c->end || *c->p < '0' || *c->p > '9') {
        return false;
    }
    uint64_t v = 0;
    while (c->p < c->end && *c->p >= '0' && *c->p <= '9') {
        v = v * 10u + (uint64_t)(*c->p++ - '0');
        if (v > 0xFFFFFFFFull) {
            return false;
        }
    }
    *out = (uint32_t)v;
    return true;
}

static bool j_skip(jcur_t *c, int depth)
{
    if (depth > 8) {
        return false;
    }
    j_ws(c);
    if (c->p >= c->end) {
        return false;
    }
    if (*c->p == '"') {
        return j_str(c, NULL, 0);
    }
    if (*c->p == '{') {
        c->p++;
        if (j_take(c, '}')) {
            return true;
        }
        for (;;) {
            if (!j_str(c, NULL, 0) || !j_take(c, ':') || !j_skip(c, depth + 1)) {
                return false;
            }
            if (j_take(c, '}')) {
                return true;
            }
            if (!j_take(c, ',')) {
                return false;
            }
        }
    }
    if (*c->p == '[') {
        c->p++;
        if (j_take(c, ']')) {
            return true;
        }
        for (;;) {
            if (!j_skip(c, depth + 1)) {
                return false;
            }
            if (j_take(c, ']')) {
                return true;
            }
            if (!j_take(c, ',')) {
                return false;
            }
        }
    }
    const char *start = c->p;
    while (c->p < c->end && *c->p != ',' && *c->p != '}' && *c->p != ']' && *c->p != ' ' && *c->p != '\t' &&
           *c->p != '\r' && *c->p != '\n') {
        c->p++;
    }
    return c->p > start;
}

static bool parse_net_obj(jcur_t *c, gwnv_doc_t *out)
{
    if (!j_take(c, '{')) {
        return false;
    }
    bool got_pan = false, got_short = false, got_ext = false;
    if (j_take(c, '}')) {
        return false;
    }
    for (;;) {
        char key[24], val[32];
        uint64_t v = 0;
        if (!j_str(c, key, sizeof key) || !j_take(c, ':')) {
            return false;
        }
        if (!strcmp(key, "pan")) {
            if (!j_str(c, val, sizeof val) || !parse_hex(val, 1, 4, &v)) {
                return false;
            }
            out->pan = (uint16_t)v;
            got_pan = true;
        } else if (!strcmp(key, "short")) {
            if (!j_str(c, val, sizeof val) || !parse_hex(val, 1, 4, &v)) {
                return false;
            }
            out->short_addr = (uint16_t)v;
            got_short = true;
        } else if (!strcmp(key, "ext")) {
            if (!j_str(c, val, sizeof val) || !parse_hex(val, 1, 16, &v)) {
                return false;
            }
            out->ext = v;
            got_ext = true;
        } else if (!j_skip(c, 1)) {
            return false;
        }
        if (j_take(c, '}')) {
            return got_pan && got_short && got_ext;
        }
        if (!j_take(c, ',')) {
            return false;
        }
    }
}

static bool parse_sensor_obj(jcur_t *c, size_t idx, gwnv_sensor_t *s)
{
    memset(s, 0, sizeof *s);
    s->sub = (uint16_t)idx;
    if (!j_take(c, '{') || j_take(c, '}')) {
        return false;
    }
    bool got_mac = false, got_short = false;
    for (;;) {
        char key[24], val[24];
        uint64_t v = 0;
        uint32_t u = 0;
        if (!j_str(c, key, sizeof key) || !j_take(c, ':')) {
            return false;
        }
        if (!strcmp(key, "mac")) {
            if (!j_str(c, val, sizeof val) || !parse_mac(val, s->mac)) {
                return false;
            }
            got_mac = true;
        } else if (!strcmp(key, "short")) {
            if (!j_str(c, val, sizeof val) || !parse_hex(val, 1, 4, &v)) {
                return false;
            }
            s->short_addr = (uint16_t)v;
            got_short = true;
        } else if (!strcmp(key, "sub")) {
            if (!j_uint(c, &u) || u > 0xFFFFu) {
                return false;
            }
            s->sub = (uint16_t)u;
        } else if (!strcmp(key, "type")) {
            j_ws(c);
            if (c->p < c->end && *c->p == '"') {
                if (!j_str(c, val, sizeof val) || !parse_hex(val, 1, 2, &v)) {
                    return false;
                }
                s->type = (uint8_t)v;
            } else {
                if (!j_uint(c, &u) || u > 0xFFu) {
                    return false;
                }
                s->type = (uint8_t)u;
            }
        } else if (!j_skip(c, 2)) {
            return false;
        }
        if (j_take(c, '}')) {
            return got_mac && got_short;
        }
        if (!j_take(c, ',')) {
            return false;
        }
    }
}

bool gwnv_parse_json(const char *json, size_t len, gwnv_doc_t *out, char *err, size_t err_cap)
{
    memset(out, 0, sizeof *out);
    if (!json || !len) {
        return fail(err, err_cap, "empty JSON body");
    }
    jcur_t c = {json, json + len};
    if (!j_take(&c, '{')) {
        return fail(err, err_cap, "expected JSON object");
    }
    bool got_fmt = false, got_ver = false, got_net = false;
    if (!j_take(&c, '}')) {
        for (;;) {
            char key[32], val[40];
            uint32_t ver = 0;
            if (!j_str(&c, key, sizeof key) || !j_take(&c, ':')) {
                return fail(err, err_cap, "malformed JSON key");
            }
            if (!strcmp(key, "format")) {
                if (!j_str(&c, val, sizeof val) || strcmp(val, "openwyzesense-nv-json") != 0) {
                    return fail(err, err_cap, "expected format openwyzesense-nv-json");
                }
                got_fmt = true;
            } else if (!strcmp(key, "version")) {
                if (!j_uint(&c, &ver) || ver != 1) {
                    return fail(err, err_cap, "expected openwyzesense-nv-json version 1");
                }
                got_ver = true;
            } else if (!strcmp(key, "network")) {
                if (!parse_net_obj(&c, out)) {
                    return fail(err, err_cap, "missing or invalid network object");
                }
                got_net = true;
            } else if (!strcmp(key, "sensors")) {
                if (!j_take(&c, '[')) {
                    return fail(err, err_cap, "sensors must be a JSON array");
                }
                if (!j_take(&c, ']')) {
                    for (;;) {
                        if (out->n_sensors == GWNV_MAX_SENSORS) {
                            return fail(err, err_cap, "more than %u sensors", GWNV_MAX_SENSORS);
                        }
                        if (!parse_sensor_obj(&c, out->n_sensors, &out->sensors[out->n_sensors])) {
                            return fail(err, err_cap, "invalid sensor entry at index %u", (unsigned)out->n_sensors);
                        }
                        out->n_sensors++;
                        if (j_take(&c, ']')) {
                            break;
                        }
                        if (!j_take(&c, ',')) {
                            return fail(err, err_cap, "malformed sensors array");
                        }
                    }
                }
            } else if (!j_skip(&c, 1)) {
                return fail(err, err_cap, "malformed JSON value for %s", key);
            }
            if (j_take(&c, '}')) {
                break;
            }
            if (!j_take(&c, ',')) {
                return fail(err, err_cap, "expected ',' or '}' in root object");
            }
        }
    }
    j_ws(&c);
    if (c.p != c.end) {
        return fail(err, err_cap, "trailing characters after JSON object");
    }
    if (!got_fmt || !got_ver || !got_net) {
        return fail(err, err_cap, "missing format, version, or network in nv.json");
    }
    return validate_doc(out, err, err_cap);
}

size_t gwnv_format_json(const gwnv_doc_t *doc, char *out, size_t cap)
{
    if (!out || cap < 2) {
        return 0;
    }
    int head =
        snprintf(out, cap,
                 "{\"format\":\"openwyzesense-nv-json\",\"version\":1,\"source\":{\"layout\":\"ows\"},"
                 "\"network\":{\"pan\":\"%04X\",\"short\":\"%04X\",\"ext\":\"%08lX%08lX\"},\"sensors\":[",
                 doc->pan, doc->short_addr, (unsigned long)(doc->ext >> 32), (unsigned long)(doc->ext & 0xFFFFFFFFu));
    if (head <= 0 || (size_t)head >= cap) {
        return 0;
    }
    size_t n = (size_t)head;
    for (size_t i = 0; i < doc->n_sensors; i++) {
        const gwnv_sensor_t *s = &doc->sensors[i];
        int k;
        if (s->type) {
            k = snprintf(out + n, cap - n, "%s{\"sub\":%u,\"mac\":\"%.8s\",\"short\":\"%04X\",\"type\":\"%02X\"}",
                         i ? "," : "", s->sub, s->mac, s->short_addr, s->type);
        } else {
            k = snprintf(out + n, cap - n, "%s{\"sub\":%u,\"mac\":\"%.8s\",\"short\":\"%04X\"}", i ? "," : "", s->sub,
                         s->mac, s->short_addr);
        }
        if (k <= 0 || n + (size_t)k >= cap) {
            return 0;
        }
        n += (size_t)k;
    }
    int tail = snprintf(out + n, cap - n, "],\"join\":null,\"dropped\":[],\"unrecognised\":[]}\n");
    if (tail <= 0 || n + (size_t)tail >= cap) {
        return 0;
    }
    return n + (size_t)tail;
}

// --- UART exchange over 0x53F0 / 0x53F1 ---

static bool ows_exchange(const gwnv_io_t *io, uint8_t sub_id, const uint8_t *req_body, size_t req_len, uint8_t *rsp_out,
                         size_t rsp_cap, size_t *rsp_len)
{
    uint8_t req[OWS_MAX_PAYLOAD], pkt[HOST_MAX_PACKET], ack[16], buf[64];
    if (1 + req_len > sizeof req) {
        return false;
    }
    req[0] = sub_id;
    if (req_len) {
        memcpy(req + 1, req_body, req_len);
    }
    size_t n_pkt = host_build(pkt, HOST_OWS_CMD, req, 1 + req_len, false);
    size_t n_ack = host_build_ack(ack, HOST_OWS_RSP, false);
    for (int t = 0; t < TRIES; t++) {
        host_rx_t rx;
        host_rx_init(&rx);
        io->flush_input(io->ctx);
        if (!io->write(io->ctx, pkt, n_pkt)) {
            return false;
        }
        for (size_t w = 0, got = 0; w < WAITS && got < BYTES_MAX;) {
            size_t n = io->read(io->ctx, buf, sizeof buf, WAIT_MS);
            if (!n) {
                w++;
                continue;
            }
            got += n;
            for (size_t i = 0; i < n; i++) {
                host_pkt_t p;
                if (!host_rx_feed(&rx, buf[i], &p) || !p.to_host || p.cmd != HOST_OWS_RSP || p.len < 1) {
                    continue;
                }
                io->write(io->ctx, ack, n_ack);
                if (p.payload[0] != sub_id) {
                    continue;
                }
                if (req_len >= 1 && sub_id == OWS_SUB_NV_IMPORT && (p.len < 2 || p.payload[1] != req_body[0])) {
                    continue;
                }
                size_t body_len = p.len - 1;
                if (body_len > rsp_cap) {
                    return false;
                }
                if (body_len) {
                    memcpy(rsp_out, p.payload + 1, body_len);
                }
                *rsp_len = body_len;
                return true;
            }
        }
    }
    return false;
}

bool gwnv_export(const gwnv_io_t *io, gwnv_doc_t *out, char *err, size_t err_cap)
{
    uint8_t rsp[24];
    size_t n = 0;
    memset(out, 0, sizeof *out);
    if (!ows_exchange(io, OWS_SUB_NETWORK, NULL, 0, rsp, sizeof rsp, &n) || n != 13 || rsp[0] != 0x00) {
        return fail(err, err_cap, "gateway did not answer OWS_SUB_NETWORK");
    }
    out->pan = (uint16_t)(rsp[1] | rsp[2] << 8);
    out->short_addr = (uint16_t)(rsp[3] | rsp[4] << 8);
    out->ext = 0;
    for (int i = 7; i >= 0; i--) {
        out->ext = out->ext << 8 | rsp[5 + i];
    }
    uint8_t ask0[2] = {OWS_NV_GET_SENSOR, 0};
    if (!ows_exchange(io, OWS_SUB_NV_IMPORT, ask0, sizeof ask0, rsp, sizeof rsp, &n) || n < 4) {
        return fail(err, err_cap, "gateway did not answer OWS_NV_GET_SENSOR");
    }
    uint8_t count = rsp[2];
    if (count > GWNV_MAX_SENSORS) {
        count = GWNV_MAX_SENSORS;
    }
    for (uint8_t idx = 0; idx < count; idx++) {
        if (idx > 0) {
            uint8_t ask[2] = {OWS_NV_GET_SENSOR, idx};
            if (!ows_exchange(io, OWS_SUB_NV_IMPORT, ask, sizeof ask, rsp, sizeof rsp, &n)) {
                return fail(err, err_cap, "failed reading sensor %u", idx);
            }
        }
        if (n != 17 || rsp[1] != 0x00 || rsp[3] != idx) {
            return fail(err, err_cap, "malformed sensor reply at index %u", idx);
        }
        gwnv_sensor_t *s = &out->sensors[out->n_sensors++];
        s->sub = (uint16_t)(rsp[4] | rsp[5] << 8);
        memcpy(s->mac, rsp + 6, 8);
        s->mac[8] = 0;
        s->short_addr = (uint16_t)(rsp[14] | rsp[15] << 8);
        s->type = rsp[16];
    }
    return true;
}

bool gwnv_import(const gwnv_io_t *io, const gwnv_doc_t *doc, char *err, size_t err_cap)
{
    if (!validate_doc((gwnv_doc_t *)(uintptr_t)doc, err, err_cap)) {
        return false;
    }
    uint8_t rsp[8];
    size_t n = 0;
    uint8_t clr[1] = {OWS_NV_CLEAR};
    if (!ows_exchange(io, OWS_SUB_NV_IMPORT, clr, sizeof clr, rsp, sizeof rsp, &n) || n < 2 || rsp[1] != 0x00) {
        return fail(err, err_cap, "gateway refused OWS_NV_CLEAR");
    }
    uint8_t net[13];
    net[0] = OWS_NV_SET_NET;
    net[1] = (uint8_t)doc->pan;
    net[2] = (uint8_t)(doc->pan >> 8);
    net[3] = (uint8_t)doc->short_addr;
    net[4] = (uint8_t)(doc->short_addr >> 8);
    for (int i = 0; i < 8; i++) {
        net[5 + i] = (uint8_t)(doc->ext >> (8 * i));
    }
    if (!ows_exchange(io, OWS_SUB_NV_IMPORT, net, sizeof net, rsp, sizeof rsp, &n) || n < 2 || rsp[1] != 0x00) {
        return fail(err, err_cap, "gateway refused OWS_NV_SET_NET (status 0x%02X)", n >= 2 ? rsp[1] : 0xFFu);
    }
    for (size_t i = 0; i < doc->n_sensors; i++) {
        const gwnv_sensor_t *s = &doc->sensors[i];
        uint8_t add[14];
        add[0] = OWS_NV_ADD_SENSOR;
        memcpy(add + 1, s->mac, 8);
        add[9] = (uint8_t)s->short_addr;
        add[10] = (uint8_t)(s->short_addr >> 8);
        add[11] = s->type;
        add[12] = (uint8_t)s->sub;
        add[13] = (uint8_t)(s->sub >> 8);
        if (!ows_exchange(io, OWS_SUB_NV_IMPORT, add, sizeof add, rsp, sizeof rsp, &n) || n < 2 || rsp[1] != 0x00) {
            return fail(err, err_cap, "gateway refused sensor %.8s (status 0x%02X)", s->mac, n >= 2 ? rsp[1] : 0xFFu);
        }
    }
    return true;
}

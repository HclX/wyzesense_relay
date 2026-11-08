#include "http_auth.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

const char *http_auth_secret(const char *admin_pass, const char *token)
{
    if (admin_pass && admin_pass[0]) {
        return admin_pass;
    }
    if (token && token[0]) {
        return token;
    }
    return NULL;
}

static bool is_ws(char c)
{
    return c == ' ' || c == '\t';
}

static bool ct_eq(const char *got, size_t got_len, const char *want)
{
    size_t want_len = strlen(want);
    unsigned diff = (unsigned)(got_len ^ want_len);
    for (size_t i = 0; i < want_len; i++) {
        unsigned char a = i < got_len ? (unsigned char)got[i] : 0;
        unsigned char b = (unsigned char)want[i];
        diff |= (unsigned)(a ^ b);
    }
    return diff == 0;
}

static bool match_scheme(const char *hdr, const char *scheme, const char **cred, size_t *cred_len)
{
    while (is_ws(*hdr)) {
        hdr++;
    }
    for (size_t i = 0; scheme[i]; i++) {
        unsigned char a = (unsigned char)hdr[i];
        unsigned char b = (unsigned char)scheme[i];
        if (!a || (unsigned char)(a | 0x20u) != (unsigned char)(b | 0x20u)) {
            return false;
        }
    }
    hdr += strlen(scheme);
    if (!is_ws(*hdr)) {
        return false;
    }
    while (is_ws(*hdr)) {
        hdr++;
    }
    size_t n = strlen(hdr);
    while (n && (is_ws(hdr[n - 1]) || hdr[n - 1] == '\r' || hdr[n - 1] == '\n')) {
        n--;
    }
    if (!n) {
        return false;
    }
    *cred = hdr;
    *cred_len = n;
    return true;
}

static int b64_val(unsigned char c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

static bool b64_decode(const char *in, size_t in_len, char *out, size_t cap, size_t *out_len)
{
    if (!in_len || (in_len % 4) != 0) {
        return false;
    }
    size_t n = 0;
    for (size_t i = 0; i < in_len; i += 4) {
        unsigned char c0 = (unsigned char)in[i];
        unsigned char c1 = (unsigned char)in[i + 1];
        unsigned char c2 = (unsigned char)in[i + 2];
        unsigned char c3 = (unsigned char)in[i + 3];
        int v0 = b64_val(c0), v1 = b64_val(c1);
        if (v0 < 0 || v1 < 0) {
            return false;
        }
        bool p2 = c2 == '=', p3 = c3 == '=';
        if ((p2 && !p3) || ((p2 || p3) && i + 4 != in_len)) {
            return false;
        }
        int v2 = p2 ? 0 : b64_val(c2);
        int v3 = p3 ? 0 : b64_val(c3);
        if (v2 < 0 || v3 < 0) {
            return false;
        }
        uint32_t acc = ((uint32_t)v0 << 18) | ((uint32_t)v1 << 12) | ((uint32_t)v2 << 6) | (uint32_t)v3;
        size_t emit = p2 ? 1 : (p3 ? 2 : 3);
        if (n + emit > cap) {
            return false;
        }
        out[n++] = (char)(acc >> 16);
        if (emit >= 2) {
            out[n++] = (char)(acc >> 8);
        }
        if (emit == 3) {
            out[n++] = (char)acc;
        }
    }
    *out_len = n;
    return true;
}

bool http_auth_ok(const char *auth_hdr, const char *admin_pass, const char *token)
{
    const char *secret = http_auth_secret(admin_pass, token);
    if (!secret) {
        return true;
    }
    if (!auth_hdr || !auth_hdr[0]) {
        return false;
    }
    const char *cred = NULL;
    size_t cred_len = 0;
    if (match_scheme(auth_hdr, "bearer", &cred, &cred_len)) {
        return ct_eq(cred, cred_len, secret);
    }
    if (match_scheme(auth_hdr, "basic", &cred, &cred_len)) {
        char dec[192];
        size_t dec_len = 0;
        if (!b64_decode(cred, cred_len, dec, sizeof dec, &dec_len)) {
            return false;
        }
        const char *colon = memchr(dec, ':', dec_len);
        if (!colon) {
            return false;
        }
        size_t pass_off = (size_t)(colon - dec) + 1;
        return ct_eq(dec + pass_off, dec_len - pass_off, secret);
    }
    return false;
}

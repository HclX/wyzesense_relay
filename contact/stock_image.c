#include "stock_image.h"

#include <stdio.h>
#include <string.h>

// SHA-256 of the packed 0x1EFA8-byte Wyze Sense v1.8 dongle gateway image (pages 0-29 + page 31 code
// 0x1F000..0x1FFA7): ab9c235bbce832a7dc74d67092199ba4108d8d24a33209c781bf483b85684ea0
const uint8_t k_stock_image_sha256[32] = {
    0xab, 0x9c, 0x23, 0x5b, 0xbc, 0xe8, 0x32, 0xa7, 0xdc, 0x74, 0xd6, 0x70, 0x92, 0x19, 0x9b, 0xa4,
    0x10, 0x8d, 0x8d, 0x24, 0xa3, 0x32, 0x09, 0xc7, 0x81, 0xbf, 0x48, 0x3b, 0x85, 0x68, 0x4e, 0xa0,
};

static const uint32_t k_sha256_k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static uint32_t rotr32(uint32_t x, unsigned n)
{
    return (x >> n) | (x << (32u - n));
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

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

static void sha256_block(uint32_t state[8], const uint8_t block[64])
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = be32(block + (size_t)i * 4u);
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    for (int i = 0; i < 64; i++) {
        uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + s1 + ch + k_sha256_k[i] + w[i];
        uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

void stock_sha256_init(stock_sha256_t *s)
{
    s->state[0] = 0x6a09e667u;
    s->state[1] = 0xbb67ae85u;
    s->state[2] = 0x3c6ef372u;
    s->state[3] = 0xa54ff53au;
    s->state[4] = 0x510e527fu;
    s->state[5] = 0x9b05688cu;
    s->state[6] = 0x1f83d9abu;
    s->state[7] = 0x5be0cd19u;
    s->bit_len = 0;
    s->buf_len = 0;
}

void stock_sha256_update(stock_sha256_t *s, const uint8_t *data, size_t len)
{
    s->bit_len += (uint64_t)len * 8u;
    while (len > 0) {
        if (s->buf_len == 0 && len >= 64) {
            sha256_block(s->state, data);
            data += 64;
            len -= 64;
            continue;
        }
        size_t n = 64u - s->buf_len < len ? 64u - s->buf_len : len;
        memcpy(s->buf + s->buf_len, data, n);
        s->buf_len += n;
        data += n;
        len -= n;
        if (s->buf_len == 64) {
            sha256_block(s->state, s->buf);
            s->buf_len = 0;
        }
    }
}

void stock_sha256_final(stock_sha256_t *s, uint8_t out[32])
{
    if (s->buf_len >= sizeof s->buf) {  // never (update leaves it below 64), but keeps the index provably in range
        sha256_block(s->state, s->buf);
        s->buf_len = 0;
    }
    s->buf[s->buf_len++] = 0x80u;
    if (s->buf_len > 56) {
        memset(s->buf + s->buf_len, 0, 64u - s->buf_len);
        sha256_block(s->state, s->buf);
        s->buf_len = 0;
    }
    memset(s->buf + s->buf_len, 0, 56u - s->buf_len);
    for (int i = 0; i < 8; i++) {
        s->buf[56 + i] = (uint8_t)(s->bit_len >> (56 - 8 * i));
    }
    sha256_block(s->state, s->buf);
    for (int i = 0; i < 8; i++) {
        out[4 * i + 0] = (uint8_t)(s->state[i] >> 24);
        out[4 * i + 1] = (uint8_t)(s->state[i] >> 16);
        out[4 * i + 2] = (uint8_t)(s->state[i] >> 8);
        out[4 * i + 3] = (uint8_t)s->state[i];
    }
}

void stock_sha256_hex(const uint8_t sha256[32], char hex_out[65])
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; i++) {
        hex_out[2 * i] = hex[sha256[i] >> 4];
        hex_out[2 * i + 1] = hex[sha256[i] & 0x0Fu];
    }
    hex_out[64] = '\0';
}

void stock_hdr_encode(uint8_t out[STOCK_HDR_LEN], const uint8_t sha256[32])
{
    static const uint8_t magic[8] = {'O', 'W', 'S', 'S', 'T', 'O', 'C', 'K'};  // STOCK_HDR_MAGIC, no NUL
    memcpy(out, magic, sizeof magic);
    put_le32(out + 8, STOCK_HDR_FORMAT);
    put_le32(out + 12, STOCK_IMAGE_LEN);
    memcpy(out + 16, sha256 ? sha256 : k_stock_image_sha256, 32);
}

bool stock_hdr_check(const uint8_t raw[STOCK_HDR_LEN], const uint8_t *expect_sha256)
{
    const uint8_t *want = expect_sha256 ? expect_sha256 : k_stock_image_sha256;
    return memcmp(raw, STOCK_HDR_MAGIC, 8) == 0 && le32(raw + 8) == STOCK_HDR_FORMAT &&
           le32(raw + 12) == STOCK_IMAGE_LEN && memcmp(raw + 16, want, 32) == 0;
}

void stock_pack_init(stock_pack_t *p, uint8_t *p31_buf, stock_write_fn write, void *ctx)
{
    memset(p, 0, sizeof *p);
    stock_sha256_init(&p->sha);
    p->p31_buf = p31_buf;
    p->write = write;
    p->ctx = ctx;
}

bool stock_pack_feed(stock_pack_t *p, const uint8_t *data, size_t len)
{
    if (p->overflow || p->write_failed) {
        return false;
    }
    if (len > STOCK_DUMP_LEN - p->in_len) {
        p->overflow = true;
        return false;
    }
    uint32_t cur = p->in_len;
    uint32_t end = cur + (uint32_t)len;

    // 1. Pages 0-29 (0x00000..0x1DFFF): identical in both a 128 KB dump and a pre-packed 0x1EFA8 image.
    if (cur < STOCK_APP_LEN) {
        uint32_t sub_end = end < STOCK_APP_LEN ? end : STOCK_APP_LEN;
        size_t n = sub_end - cur;
        stock_sha256_update(&p->sha, data, n);
        if (p->write && !p->write(p->ctx, cur, data, n)) {
            p->write_failed = true;
            return false;
        }
    }

    // 2. 0x1E000..0x1EFA7: page 31 code in a pre-packed 0x1EFA8 image (or page 30 in a 128 KB dump, overwritten below).
    if (end > STOCK_APP_LEN && cur < STOCK_IMAGE_LEN) {
        uint32_t s = cur > STOCK_APP_LEN ? cur : STOCK_APP_LEN;
        uint32_t e = end < STOCK_IMAGE_LEN ? end : STOCK_IMAGE_LEN;
        memcpy(p->p31_buf + (s - STOCK_APP_LEN), data + (s - cur), e - s);
    }

    // 3. 0x1F000..0x1FFA7: page 31 code in a 128 KB dump (page 30 at 0x1E000..0x1EFFF and CCFG at 0x1FFA8..0x1FFFF
    //    are skipped).
    const uint32_t p31_end = STOCK_PAGE31_OFFSET + STOCK_PAGE31_CODE_LEN;
    if (end > STOCK_PAGE31_OFFSET && cur < p31_end) {
        uint32_t s = cur > STOCK_PAGE31_OFFSET ? cur : STOCK_PAGE31_OFFSET;
        uint32_t e = end < p31_end ? end : p31_end;
        memcpy(p->p31_buf + (s - STOCK_PAGE31_OFFSET), data + (s - cur), e - s);
    }

    p->in_len = end;
    return true;
}

bool stock_pack_finish(stock_pack_t *p, const uint8_t *expect_sha256, uint8_t hdr_out[STOCK_HDR_LEN],
                       uint8_t digest_out[32], char *err, size_t err_cap)
{
    if (p->write_failed) {
        snprintf(err, err_cap, "writing to the stock flash partition failed");
        return false;
    }
    if (p->overflow || (p->in_len != STOCK_IMAGE_LEN && p->in_len != STOCK_DUMP_LEN)) {
        snprintf(err, err_cap, "expected a 131072-byte flash dump or 126888-byte packed stock image (got %lu bytes)",
                 (unsigned long)p->in_len);
        return false;
    }
    stock_sha256_update(&p->sha, p->p31_buf, STOCK_PAGE31_CODE_LEN);
    uint8_t digest[32];
    stock_sha256_final(&p->sha, digest);
    if (digest_out) {
        memcpy(digest_out, digest, 32);
    }
    const uint8_t *want = expect_sha256 ? expect_sha256 : k_stock_image_sha256;
    if (memcmp(digest, want, 32) != 0) {
        char got_hex[65];
        stock_sha256_hex(digest, got_hex);
        snprintf(err, err_cap, "not the Wyze v1.8 stock gateway image (SHA-256 %.16s..., expected %.16s...)", got_hex,
                 STOCK_IMAGE_SHA256_HEX);
        return false;
    }
    if (p->write && !p->write(p->ctx, STOCK_APP_LEN, p->p31_buf, STOCK_PAGE31_CODE_LEN)) {
        p->write_failed = true;
        snprintf(err, err_cap, "writing page 31 code to the stock flash partition failed");
        return false;
    }
    if (hdr_out) {
        stock_hdr_encode(hdr_out, digest);
    }
    return true;
}

bool stock_verify_partition(stock_read_fn read, void *ctx, const uint8_t *expect_sha256, uint8_t *chunk_buf,
                            size_t chunk_cap, char *err, size_t err_cap)
{
    if (chunk_cap < STOCK_HDR_LEN) {
        snprintf(err, err_cap, "verify buffer too small");
        return false;
    }
    if (!read(ctx, 0, chunk_buf, STOCK_HDR_LEN)) {
        snprintf(err, err_cap, "reading stock partition header failed");
        return false;
    }
    if (!stock_hdr_check(chunk_buf, expect_sha256)) {
        snprintf(err, err_cap, "no valid stock gateway header in partition");
        return false;
    }
    stock_sha256_t sha;
    stock_sha256_init(&sha);
    size_t step = chunk_cap & ~3u;
    for (uint32_t off = 0; off < STOCK_IMAGE_LEN;) {
        size_t n = STOCK_IMAGE_LEN - off < step ? STOCK_IMAGE_LEN - off : step;
        if (!read(ctx, STOCK_HDR_LEN + off, chunk_buf, n)) {
            snprintf(err, err_cap, "reading stock partition at 0x%lx failed", (unsigned long)off);
            return false;
        }
        stock_sha256_update(&sha, chunk_buf, n);
        off += (uint32_t)n;
    }
    uint8_t digest[32];
    stock_sha256_final(&sha, digest);
    const uint8_t *want = expect_sha256 ? expect_sha256 : k_stock_image_sha256;
    if (memcmp(digest, want, 32) != 0) {
        snprintf(err, err_cap, "stored stock image SHA-256 readback mismatch");
        return false;
    }
    return true;
}

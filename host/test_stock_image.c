// Native unit tests for contact/stock_image.c (SHA-256, OWSSTOCK header, streaming packer, and partition verifier),
// plus a `--pack DUMP OUT [SHA256_HEX]` mode used by tests/test_stock_partition.py to verify byte-for-byte identity
// between the C packer and tools/stock_partition.py.
#include "stock_image.h"

#include <stdio.h>
#include <stdlib.h>
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

typedef struct {
    uint8_t part[STOCK_PART_LEN];
    bool p31_written;
    bool fail_at_p31;
} mem_part_t;

static bool mem_write(void *ctx, uint32_t off, const uint8_t *data, size_t len)
{
    mem_part_t *m = ctx;
    if (off + len > STOCK_IMAGE_LEN) {
        return false;
    }
    if (off == STOCK_APP_LEN) {
        if (m->fail_at_p31) {
            return false;
        }
        m->p31_written = true;
    }
    memcpy(m->part + STOCK_HDR_LEN + off, data, len);
    return true;
}

static bool mem_read(void *ctx, uint32_t off, uint8_t *buf, size_t len)
{
    const mem_part_t *m = ctx;
    if (off + len > STOCK_PART_LEN) {
        return false;
    }
    memcpy(buf, m->part + off, len);
    return true;
}

static bool parse_hex32(const char *hex, uint8_t out[32])
{
    if (strlen(hex) != 64) {
        return false;
    }
    for (int i = 0; i < 32; i++) {
        unsigned v = 0;
        for (int k = 0; k < 2; k++) {
            char c = hex[2 * i + k];
            unsigned nib;
            if (c >= '0' && c <= '9') {
                nib = (unsigned)(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                nib = (unsigned)(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                nib = (unsigned)(c - 'A' + 10);
            } else {
                return false;
            }
            v = (v << 4) | nib;
        }
        out[i] = (uint8_t)v;
    }
    return true;
}

static int run_pack_cli(const char *dump_path, const char *out_path, const char *sha_hex)
{
    uint8_t expect[32];
    const uint8_t *expect_ptr = k_stock_image_sha256;
    if (sha_hex) {
        if (!parse_hex32(sha_hex, expect)) {
            fprintf(stderr, "invalid sha256 hex: %s\n", sha_hex);
            return 2;
        }
        expect_ptr = expect;
    }
    FILE *in = fopen(dump_path, "rb");
    if (!in) {
        perror(dump_path);
        return 1;
    }
    static mem_part_t m;
    static uint8_t p31[STOCK_PAGE31_CODE_LEN];
    memset(&m, 0, sizeof m);
    stock_pack_t pack;
    stock_pack_init(&pack, p31, mem_write, &m);
    uint8_t buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
        if (!stock_pack_feed(&pack, buf, n)) {
            break;
        }
    }
    fclose(in);
    char err[160] = "";
    if (!stock_pack_finish(&pack, expect_ptr, m.part, NULL, err, sizeof err)) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    if (!stock_verify_partition(mem_read, &m, expect_ptr, buf, sizeof buf, err, sizeof err)) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    FILE *out = fopen(out_path, "wb");
    if (!out) {
        perror(out_path);
        return 1;
    }
    if (fwrite(m.part, 1, STOCK_PART_LEN, out) != STOCK_PART_LEN) {
        fclose(out);
        return 1;
    }
    fclose(out);
    return 0;
}

static void sha256_oneshot(const void *data, size_t len, char hex_out[65])
{
    stock_sha256_t s;
    uint8_t d[32];
    stock_sha256_init(&s);
    stock_sha256_update(&s, data, len);
    stock_sha256_final(&s, d);
    stock_sha256_hex(d, hex_out);
}

int main(int argc, char **argv)
{
    if (argc >= 4 && strcmp(argv[1], "--pack") == 0) {
        return run_pack_cli(argv[2], argv[3], argc >= 5 ? argv[4] : NULL);
    }

    // 1. Known constant & standard SHA-256 test vectors (including 55/56/64-byte padding boundaries)
    char hex[65];
    stock_sha256_hex(k_stock_image_sha256, hex);
    CHECK(strcmp(hex, STOCK_IMAGE_SHA256_HEX) == 0);

    sha256_oneshot("", 0, hex);
    CHECK(strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0);

    sha256_oneshot("abc", 3, hex);
    CHECK(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);

    const char *vec56 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha256_oneshot(vec56, strlen(vec56), hex);
    CHECK(strcmp(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1") == 0);

    // 2. Header encoding and checking
    uint8_t hdr[STOCK_HDR_LEN];
    stock_hdr_encode(hdr, NULL);
    CHECK(stock_hdr_check(hdr, NULL));
    CHECK(stock_hdr_check(hdr, k_stock_image_sha256));
    hdr[0] ^= 1;
    CHECK(!stock_hdr_check(hdr, NULL));
    hdr[0] ^= 1;
    hdr[8] = 2;  // wrong format
    CHECK(!stock_hdr_check(hdr, NULL));
    hdr[8] = 1;
    hdr[12] ^= 4;  // wrong image length
    CHECK(!stock_hdr_check(hdr, NULL));
    hdr[12] ^= 4;
    hdr[16] ^= 0x80;  // wrong SHA-256
    CHECK(!stock_hdr_check(hdr, NULL));

    // 3. Build a synthetic 128 KB dump: pages 0-23 pattern, pages 24-29 0x00, page 30 NV, page 31 code + CCFG
    static uint8_t dump[STOCK_DUMP_LEN];
    static uint8_t expected_packed[STOCK_IMAGE_LEN];
    for (size_t i = 0; i < 24 * 4096; i++) {
        dump[i] = (uint8_t)((i * 73u + 19u) & 0xFFu);
    }
    memset(dump + 24 * 4096, 0x00, 6 * 4096);    // pages 24-29: 24 KB of 0x00
    memset(dump + STOCK_NV_OFFSET, 0xA5, 4096);  // page 30: per-dongle NV (must be skipped)
    for (size_t i = 0; i < STOCK_PAGE31_CODE_LEN; i++) {
        dump[STOCK_PAGE31_OFFSET + i] = (uint8_t)((i * 29u + 7u) & 0xFFu);
    }
    memset(dump + STOCK_PAGE31_OFFSET + STOCK_PAGE31_CODE_LEN, 0xC3, 88);  // 88-byte CCFG (must be skipped)

    memcpy(expected_packed, dump, STOCK_APP_LEN);
    memcpy(expected_packed + STOCK_APP_LEN, dump + STOCK_PAGE31_OFFSET, STOCK_PAGE31_CODE_LEN);

    stock_sha256_t sha;
    uint8_t synth_sha[32];
    stock_sha256_init(&sha);
    stock_sha256_update(&sha, expected_packed, sizeof expected_packed);
    stock_sha256_final(&sha, synth_sha);

    // 4. Stream the 128 KB dump and the pre-packed 0x1EFA8 image with various chunk sizes
    const size_t chunk_sizes[] = {1, 7, 64, 257, 1460, 4096, STOCK_DUMP_LEN};
    static mem_part_t ref_part, cur_part;
    static uint8_t p31_buf[STOCK_PAGE31_CODE_LEN];
    static uint8_t verify_buf[256];
    char err[160];

    for (size_t ci = 0; ci < sizeof chunk_sizes / sizeof chunk_sizes[0]; ci++) {
        size_t cs = chunk_sizes[ci];
        memset(&cur_part, 0, sizeof cur_part);
        stock_pack_t pack;
        stock_pack_init(&pack, p31_buf, mem_write, &cur_part);
        for (size_t off = 0; off < sizeof dump; off += cs) {
            size_t n = sizeof dump - off < cs ? sizeof dump - off : cs;
            CHECK(stock_pack_feed(&pack, dump + off, n));
        }
        CHECK(!cur_part.p31_written);  // page 31 code is not written before stock_pack_finish verifies the hash
        CHECK(stock_pack_finish(&pack, synth_sha, cur_part.part, NULL, err, sizeof err));
        CHECK(cur_part.p31_written);
        CHECK(memcmp(cur_part.part + STOCK_HDR_LEN, expected_packed, STOCK_IMAGE_LEN) == 0);
        CHECK(stock_verify_partition(mem_read, &cur_part, synth_sha, verify_buf, sizeof verify_buf, err, sizeof err));
        if (ci == 0) {
            ref_part = cur_part;
        } else {
            CHECK(memcmp(&cur_part, &ref_part, sizeof ref_part) == 0);
        }

        // Pre-packed 0x1EFA8 input with the same chunk size must produce byte-identical partition content
        memset(&cur_part, 0, sizeof cur_part);
        stock_pack_init(&pack, p31_buf, mem_write, &cur_part);
        for (size_t off = 0; off < sizeof expected_packed; off += cs) {
            size_t n = sizeof expected_packed - off < cs ? sizeof expected_packed - off : cs;
            CHECK(stock_pack_feed(&pack, expected_packed + off, n));
        }
        CHECK(stock_pack_finish(&pack, synth_sha, cur_part.part, NULL, err, sizeof err));
        CHECK(memcmp(cur_part.part, ref_part.part, STOCK_PART_LEN) == 0);
    }

    // 5. Different page 30 (NV) or CCFG bytes in a 128 KB dump must NOT affect the packed partition
    memset(dump + STOCK_NV_OFFSET, 0x3E, 4096);
    memset(dump + STOCK_PAGE31_OFFSET + STOCK_PAGE31_CODE_LEN, 0x99, 88);
    memset(&cur_part, 0, sizeof cur_part);
    stock_pack_t pack;
    stock_pack_init(&pack, p31_buf, mem_write, &cur_part);
    CHECK(stock_pack_feed(&pack, dump, sizeof dump));
    CHECK(stock_pack_finish(&pack, synth_sha, cur_part.part, NULL, err, sizeof err));
    CHECK(memcmp(cur_part.part, ref_part.part, STOCK_PART_LEN) == 0);

    // 6. Changing a byte in pages 24-29 (0x18000..0x1DFFF) MUST fail the SHA-256 check before writing page 31 code
    dump[25 * 4096 + 123] = 0x01;
    memset(&cur_part, 0, sizeof cur_part);
    stock_pack_init(&pack, p31_buf, mem_write, &cur_part);
    CHECK(stock_pack_feed(&pack, dump, sizeof dump));
    CHECK(!stock_pack_finish(&pack, synth_sha, cur_part.part, NULL, err, sizeof err));
    CHECK(!cur_part.p31_written);
    dump[25 * 4096 + 123] = 0x00;

    // 7. Reject wrong input lengths (old 24-page 0x18FA8 length, short, and >128 KB overflow)
    const size_t bad_lens[] = {0, 0x18FA8, STOCK_IMAGE_LEN - 1, STOCK_IMAGE_LEN + 1, STOCK_DUMP_LEN - 1};
    for (size_t i = 0; i < sizeof bad_lens / sizeof bad_lens[0]; i++) {
        stock_pack_init(&pack, p31_buf, mem_write, &cur_part);
        CHECK(stock_pack_feed(&pack, dump, bad_lens[i]));
        CHECK(!stock_pack_finish(&pack, synth_sha, cur_part.part, NULL, err, sizeof err));
    }
    stock_pack_init(&pack, p31_buf, mem_write, &cur_part);
    CHECK(stock_pack_feed(&pack, dump, STOCK_DUMP_LEN));
    CHECK(!stock_pack_feed(&pack, dump, 1));
    CHECK(!stock_pack_finish(&pack, synth_sha, cur_part.part, NULL, err, sizeof err));

    // 8. Partition verifier detects bit flips anywhere in the header or image
    cur_part = ref_part;
    cur_part.part[STOCK_HDR_LEN + 27 * 4096] ^= 0x40;  // flip a bit in page 27
    CHECK(!stock_verify_partition(mem_read, &cur_part, synth_sha, verify_buf, sizeof verify_buf, err, sizeof err));
    cur_part = ref_part;
    cur_part.part[STOCK_PART_LEN - 1] ^= 0x01;  // flip last byte of page 31 code
    CHECK(!stock_verify_partition(mem_read, &cur_part, synth_sha, verify_buf, sizeof verify_buf, err, sizeof err));

    if (failures) {
        fprintf(stderr, "%d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("test_stock_image: %d checks passed\n", checks);
    return 0;
}

// Dedicated flash storage format and streaming packer/verifier for the stock Wyze v1.8 dongle gateway image.
//
// Stock firmware is proprietary to Wyze and is never embedded in the repository or in any relay app binary. Instead,
// a relay stores it in a dedicated flash area (`gwstock` data partition on ESP32/ESP32-S3; unused `_FS_start`
// filesystem region on ESP8266), populated in the field via `POST /contact/stock` or pre-populated locally with
// `tools/stock_partition.py`.
//
// Partition layout (126,936 bytes = 0x1EFD8):
//   0x00000..0x0002F (48 B)      stock_hdr_t: magic "OWSSTOCK" (8 B), format 1 (u32 LE), image length 0x1EFA8
//                                (u32 LE), and the 32-byte SHA-256 of the packed image.
//   0x00030..0x1E02F (0x1E000 B) CC1310 flash pages 0-29 (120 KB; pages 24-29 are 24 KB of 0x00 in stock v1.8).
//   0x1E030..0x1EFD7 (0x00FA8 B) CC1310 page 31 code area (0x1F000..0x1FFA7, 4,008 B before the 88-byte CCFG).
// Page 30 (per-dongle NV at 0x1E000) and the 88-byte CCFG (per-unit factory MAC/trim at 0x1FFA8..0x1FFFF) are excluded.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define STOCK_DUMP_LEN        0x20000u  // 128 KB full CC1310 flash dump (32 pages of 4 KB)
#define STOCK_APP_LEN         0x1E000u  // pages 0-29 (30 * 4096 B; pages 24-29 are 0x00 in v1.8)
#define STOCK_NV_OFFSET       0x1E000u  // page 30 (per-dongle NV: skipped when packing a 128 KB dump)
#define STOCK_PAGE31_OFFSET   0x1F000u  // page 31 start in a 128 KB dump
#define STOCK_PAGE31_CODE_LEN 0x00FA8u  // 0x1F000..0x1FFA7 (4,008 B before the 88-byte CCFG)
#define STOCK_IMAGE_LEN       (STOCK_APP_LEN + STOCK_PAGE31_CODE_LEN)  // 0x1EFA8 (126,888 B)

#define STOCK_HDR_MAGIC  "OWSSTOCK"
#define STOCK_HDR_FORMAT 1u
#define STOCK_HDR_LEN    48u
#define STOCK_PART_LEN   (STOCK_HDR_LEN + STOCK_IMAGE_LEN)  // 0x1EFD8 (126,936 B)
#define STOCK_PART_LABEL "gwstock"

// Known SHA-256 of the packed 0x1EFA8-byte Wyze Sense v1.8 dongle gateway image (pages 0-29 + page 31 code
// 0x1F000..0x1FFA7; computed from two dongles' dumps, identical on both).
#define STOCK_IMAGE_SHA256_HEX "ab9c235bbce832a7dc74d67092199ba4108d8d24a33209c781bf483b85684ea0"

extern const uint8_t k_stock_image_sha256[32];

typedef struct {
    char magic[8];       // "OWSSTOCK" (8 bytes, not NUL-terminated)
    uint32_t format;     // STOCK_HDR_FORMAT (1, little-endian)
    uint32_t len;        // STOCK_IMAGE_LEN (0x1EFA8, little-endian)
    uint8_t sha256[32];  // SHA-256 of the packed 0x1EFA8-byte image
} stock_hdr_t;

#ifdef __cplusplus
static_assert(sizeof(stock_hdr_t) == STOCK_HDR_LEN, "stock_hdr_t must be 48 bytes");
#else
_Static_assert(sizeof(stock_hdr_t) == STOCK_HDR_LEN, "stock_hdr_t must be 48 bytes");
#endif

typedef struct {
    uint32_t state[8];
    uint64_t bit_len;
    uint8_t buf[64];
    size_t buf_len;
} stock_sha256_t;

void stock_sha256_init(stock_sha256_t *s);
void stock_sha256_update(stock_sha256_t *s, const uint8_t *data, size_t len);
void stock_sha256_final(stock_sha256_t *s, uint8_t out[32]);
void stock_sha256_hex(const uint8_t sha256[32], char hex_out[65]);

// Encode / validate the 48-byte partition header. When expect_sha256 is NULL, k_stock_image_sha256 is required.
void stock_hdr_encode(uint8_t out[STOCK_HDR_LEN], const uint8_t sha256[32]);
bool stock_hdr_check(const uint8_t raw[STOCK_HDR_LEN], const uint8_t *expect_sha256);

// Callback receiving packed image bytes at image offset `off` (0 .. STOCK_IMAGE_LEN - 1).
typedef bool (*stock_write_fn)(void *ctx, uint32_t off, const uint8_t *data, size_t len);
// Callback reading stored partition bytes at partition offset `off` (0 .. STOCK_PART_LEN - 1).
typedef bool (*stock_read_fn)(void *ctx, uint32_t off, uint8_t *buf, size_t len);

// Streaming packer: accepts either a full 128 KB flash dump (0x20000 B: skips page 30 and the 88-byte CCFG on the
// fly) or an already-packed 0x1EFA8-byte image in arbitrary chunks. Pages 0-29 are hashed and passed to `write` (if
// non-NULL) as they arrive; the 0xFA8-byte page-31 code is held in `p31_buf` and only passed to `write` in
// stock_pack_finish() after the full image's SHA-256 is verified.
typedef struct {
    stock_sha256_t sha;
    uint32_t in_len;
    uint8_t *p31_buf;  // caller-provided buffer of at least STOCK_PAGE31_CODE_LEN (0xFA8) bytes
    stock_write_fn write;
    void *ctx;
    bool overflow;
    bool write_failed;
} stock_pack_t;

void stock_pack_init(stock_pack_t *p, uint8_t *p31_buf, stock_write_fn write, void *ctx);
bool stock_pack_feed(stock_pack_t *p, const uint8_t *data, size_t len);
bool stock_pack_finish(stock_pack_t *p, const uint8_t *expect_sha256, uint8_t hdr_out[STOCK_HDR_LEN],
                       uint8_t digest_out[32], char *err, size_t err_cap);

// Read back the 48-byte header + 0x1EFA8-byte image via `read` using `chunk_buf` (capacity >= STOCK_HDR_LEN) and
// verify the header fields and image SHA-256 against `expect_sha256` (or k_stock_image_sha256 when NULL).
bool stock_verify_partition(stock_read_fn read, void *ctx, const uint8_t *expect_sha256, uint8_t *chunk_buf,
                            size_t chunk_cap, char *err, size_t err_cap);

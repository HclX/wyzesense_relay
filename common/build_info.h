// What an image is (#76): a block in every firmware image, right after its vector table, that a tool, a relay or a
// web page finds by its magic in the image's first 1 KB. The image ID identifies the code: tools/image_id.py writes
// it after the link, the first 8 bytes of a SHA-256 over the image (pages 0-29, as in <product>-app.bin) with id[] and
// version[] zeroed, so the same code gives the same ID whatever its version string or build directory. Little-endian,
// strings NUL-padded.
#pragma once

#include <stdint.h>

#define BUILD_INFO_MAGIC  0x4253574Fu  // "OWSB"
#define BUILD_INFO_FORMAT 1
#define BUILD_INFO_TX     0x01  // flags: the transmit path is built in
#define BUILD_INFO_DEBUG  0x02  //        the debug host commands are

typedef struct {
    uint32_t magic;
    uint8_t format;
    uint8_t flags;
    uint16_t reserved;  // 0. Not the PAN: a network is not part of an image (the gateway keeps it in NV)
    uint8_t id[8];      // the image ID (zero as linked)
    char product[24];   // e.g. "gateway-contact-v1"
    char board[24];     // BOARD_NAME
    char app[12];       // "gateway", "sensor", "sniffer"
    char version[40];   // git describe: for people, not in the ID
} build_info_t;         // 116 bytes

_Static_assert(sizeof(build_info_t) == 116, "the layout tools/image_id.py and the relays read");

extern const build_info_t build_info;

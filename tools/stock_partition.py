#!/usr/bin/env python3
"""Build a pre-populated stock gateway flash partition image from a local Wyze v1.8 dongle dump.

Usage:
    tools/stock_partition.py DUMP OUT

Accepts either a full 128 KB CC1310 flash dump (0x20000 bytes, e.g. FLASH.bin or flash.bin from `ows-flash backup`)
or an already-packed 0x1EFA8-byte image (pages 0-29 + page 31 code 0x1F000..0x1FFA7), verifies its SHA-256 against
the known Wyze Sense v1.8 gateway digest, and writes the 48-byte OWSSTOCK header + 0x1EFA8-byte image to OUT:

    # ESP32 / ESP32-S3 (gwstock partition at 0x320000 in idf/partitions.csv):
    esptool.py write_flash 0x320000 OUT

    # ESP8266 4 MB Wemos D1 mini (_FS_start at 0x40500000 - 0x40200000 = 0x300000 in eagle.flash.4m1m.ld):
    esptool.py write_flash 0x300000 OUT
"""

from __future__ import annotations

import argparse
import hashlib
import pathlib
import struct
import sys

STOCK_DUMP_LEN = 0x20000  # 128 KB full CC1310 flash dump (32 pages of 4 KB)
STOCK_APP_LEN = 0x1E000  # pages 0-29 (120 KB; pages 24-29 are 24 KB of 0x00 in stock v1.8)
STOCK_PAGE31_OFFSET = 0x1F000
STOCK_PAGE31_CODE_LEN = 0xFA8  # 0x1F000..0x1FFA7 (4,008 B before the 88-byte CCFG)
STOCK_IMAGE_LEN = STOCK_APP_LEN + STOCK_PAGE31_CODE_LEN  # 0x1EFA8 (126,888 B)

STOCK_HDR_MAGIC = b'OWSSTOCK'
STOCK_HDR_FORMAT = 1
STOCK_HDR_STRUCT = struct.Struct('<8sII32s')
STOCK_HDR_LEN = STOCK_HDR_STRUCT.size  # 48 B
STOCK_PART_LEN = STOCK_HDR_LEN + STOCK_IMAGE_LEN  # 0x1EFD8 (126,936 B)

ESP32_GWSTOCK_OFFSET = 0x320000  # idf/partitions.csv: gwstock, data, 0x40, 0x320000, 0x20000
ESP8266_FS_START_OFFSET = 0x300000  # eagle.flash.4m1m.ld: _FS_start (0x40500000) - 0x40200000

STOCK_IMAGE_SHA256_HEX = 'ab9c235bbce832a7dc74d67092199ba4108d8d24a33209c781bf483b85684ea0'


def pack_stock_image(raw: bytes) -> bytes:
    """Pack a 128 KB CC1310 flash dump (skipping page 30 NV and the 88-byte CCFG) or return a 0x1EFA8 packed image."""
    if len(raw) == STOCK_DUMP_LEN:
        p31_end = STOCK_PAGE31_OFFSET + STOCK_PAGE31_CODE_LEN
        return raw[:STOCK_APP_LEN] + raw[STOCK_PAGE31_OFFSET:p31_end]
    if len(raw) == STOCK_IMAGE_LEN:
        return raw
    raise ValueError(
        f'expected a {STOCK_DUMP_LEN}-byte (128 KB) flash dump or {STOCK_IMAGE_LEN}-byte (0x1EFA8) packed image '
        f'(got {len(raw)} bytes)'
    )


def build_stock_partition(raw: bytes, *, expected_sha256: str = STOCK_IMAGE_SHA256_HEX) -> bytes:
    """Pack `raw`, verify its SHA-256 against `expected_sha256`, and return the 48-byte header + 0x1EFA8-byte image."""
    want_hex = expected_sha256.strip().lower()
    if len(want_hex) != 64 or any(c not in '0123456789abcdef' for c in want_hex):
        raise ValueError(f'invalid expected SHA-256 hex digest: {expected_sha256!r}')
    image = pack_stock_image(raw)
    digest = hashlib.sha256(image).digest()
    got_hex = digest.hex()
    if got_hex != want_hex:
        raise ValueError(
            f'not the Wyze v1.8 stock gateway image (SHA-256 {got_hex[:16]}..., expected {want_hex[:16]}...)'
        )
    hdr = STOCK_HDR_STRUCT.pack(STOCK_HDR_MAGIC, STOCK_HDR_FORMAT, STOCK_IMAGE_LEN, digest)
    return hdr + image


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('dump', type=pathlib.Path, help='128 KB CC1310 flash dump (FLASH.bin) or 0x1EFA8 packed image')
    ap.add_argument('out', type=pathlib.Path, help='output partition binary (48 B header + 0x1EFA8 B image)')
    ap.add_argument(
        '--sha256',
        default=STOCK_IMAGE_SHA256_HEX,
        help='expected SHA-256 of the packed image (default: Wyze v1.8 stock gateway)',
    )
    args = ap.parse_args(argv)
    if not args.dump.is_file():
        print(f'error: {args.dump} does not exist', file=sys.stderr)
        return 1
    try:
        part = build_stock_partition(args.dump.read_bytes(), expected_sha256=args.sha256)
    except ValueError as exc:
        print(f'error: {exc}', file=sys.stderr)
        return 1
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(part)
    return 0


if __name__ == '__main__':
    sys.exit(main())

#!/usr/bin/env python3
"""The gateway images a relay carries for its contact board (#76), written to OUT padded to whole words.

    gateway_image.py OUT              OpenWyzeSense gateway (firmware/gateway-contact-v1-app.bin or OWS_GATEWAY_IMAGE)
    gateway_image.py --stock OUT      Stock v1.8 gateway (firmware/gateway-stock-v1.8-app.bin, 128 KB FLASH.bin, or
                                      OWS_STOCK_GATEWAY_IMAGE; 4-byte 0xFF placeholder when absent)

Both relay builds use it: esp8266/embed.py, idf/main/CMakeLists.txt.
"""

import argparse
import os
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent
STOCK_APP_END = 0x18000  # pages 0-23 (98,304 B)
PAGE31_OFFSET = 0x1F000
PAGE31_CODE_LEN = 0xFA8  # 0x1F000..0x1FFA7 (4,008 B before the 88-byte CCFG)


def ows_image() -> bytes:
    given = os.environ.get('OWS_GATEWAY_IMAGE')
    src = pathlib.Path(given) if given else ROOT / 'firmware' / 'gateway-contact-v1-app.bin'
    if not src.is_file():
        sys.exit(f'{src} missing: place gateway-contact-v1-app.bin in firmware/ or set OWS_GATEWAY_IMAGE')
    return src.read_bytes()


def pack_stock(raw: bytes) -> bytes:
    if len(raw) == 0x20000:
        return raw[:STOCK_APP_END] + raw[PAGE31_OFFSET : PAGE31_OFFSET + PAGE31_CODE_LEN]
    return raw


def stock_image() -> bytes:
    given = os.environ.get('OWS_STOCK_GATEWAY_IMAGE')
    if given:
        p = pathlib.Path(given)
        if p.is_file():
            return pack_stock(p.read_bytes())
    for name in ('gateway-stock-v1.8-app.bin', 'gateway-stock-v1.8.bin', 'FLASH.bin'):
        p = ROOT / 'firmware' / name
        if p.is_file():
            return pack_stock(p.read_bytes())
    return b'\xff\xff\xff\xff'


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--stock', action='store_true', help='emit stock v1.8 gateway image (or 4B placeholder if absent)')
    ap.add_argument('out', type=pathlib.Path)
    a = ap.parse_args()
    data = stock_image() if a.stock else ows_image()
    data += b'\xff' * (-len(data) % 4)  # the bootloader writes words
    a.out.parent.mkdir(parents=True, exist_ok=True)
    if not a.out.exists() or a.out.read_bytes() != data:
        a.out.write_bytes(data)
    return 0


if __name__ == '__main__':
    sys.exit(main())

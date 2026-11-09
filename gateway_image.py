#!/usr/bin/env python3
"""The OpenWyzeSense gateway image a relay carries for its contact board (#76), written to OUT padded to whole words.

    gateway_image.py OUT    OpenWyzeSense gateway (firmware/gateway-contact-v1-app.bin or OWS_GATEWAY_IMAGE)

Both relay builds use it: esp8266/embed.py, idf/main/CMakeLists.txt.
"""

import argparse
import os
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent


def ows_image() -> bytes:
    given = os.environ.get('OWS_GATEWAY_IMAGE')
    src = pathlib.Path(given) if given else ROOT / 'firmware' / 'gateway-contact-v1-app.bin'
    if not src.is_file():
        sys.exit(f'{src} missing: place gateway-contact-v1-app.bin in firmware/ or set OWS_GATEWAY_IMAGE')
    return src.read_bytes()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('out', type=pathlib.Path)
    a = ap.parse_args()
    data = ows_image()
    data += b'\xff' * (-len(data) % 4)  # the bootloader writes words
    a.out.parent.mkdir(parents=True, exist_ok=True)
    if not a.out.exists() or a.out.read_bytes() != data:
        a.out.write_bytes(data)
    return 0


if __name__ == '__main__':
    sys.exit(main())

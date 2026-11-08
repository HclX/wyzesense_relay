#!/usr/bin/env python3
"""Pads the gateway image the relay carries (firmware/gateway-contact-v1-app.bin, or OWS_GATEWAY_IMAGE) to a 4-byte
multiple of 0xFF (what sbl_check_image expects, and what an ESP8266's flash reads as 32-bit words).

    gateway_image.py --bin OUT.bin       for ESP-IDF's EMBED_FILES (idf/main/CMakeLists.txt)
    gateway_image.py --header OUT.h      PROGMEM array for PlatformIO (esp8266/embed.py)
"""

import argparse
import os
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent
DEFAULT = ROOT / 'firmware' / 'gateway-contact-v1-app.bin'


def load() -> bytes:
    src = pathlib.Path(os.environ.get('OWS_GATEWAY_IMAGE', DEFAULT))
    if not src.is_file():
        sys.exit(
            f'{src} missing: place gateway-contact-v1-app.bin in firmware/ or set OWS_GATEWAY_IMAGE '
            '(what the relay checks its contact board against at boot)'
        )
    b = src.read_bytes()
    return b + b'\xff' * (-len(b) % 4)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--bin', type=pathlib.Path)
    ap.add_argument('--header', type=pathlib.Path)
    a = ap.parse_args()
    b = load()
    if a.bin:
        a.bin.parent.mkdir(parents=True, exist_ok=True)
        a.bin.write_bytes(b)
    if a.header:
        a.header.parent.mkdir(parents=True, exist_ok=True)
        rows = [', '.join(f'0x{x:02x}' for x in b[i : i + 16]) for i in range(0, len(b), 16)]
        body = ',\n    '.join(rows)
        a.header.write_text(
            '#pragma once\n#include <Arduino.h>\n#include <pgmspace.h>\n'
            f'static const uint8_t GATEWAY_BIN[{len(b)}] PROGMEM = {{\n    {body}\n}};\n'
        )


if __name__ == '__main__':
    main()

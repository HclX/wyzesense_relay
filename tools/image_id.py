#!/usr/bin/env python3
"""The build info block of a firmware image (firmware/common/build_info.h, #76) and its image ID.

    image_id.py stamp ELF --objcopy OBJCOPY   after the link: computes the ID, writes it into the ELF's .ows_info
    image_id.py show IMAGE.bin                prints the block of a built image

The ID: the first 8 bytes of a SHA-256 over the image as flashed (the ELF as `objcopy -O binary -R .ccfg` gives it,
i.e. <product>-app.bin) with the block's id and version fields zeroed. Same code, same ID: the version string
(git describe) and the build directory do not count (test_image_id.py).
"""

import argparse
import hashlib
import pathlib
import struct
import subprocess
import sys
import tempfile

MAGIC = b'OWSB'
SIZE = 116
ID_AT, ID_LEN = 8, 8
VERSION_AT, VERSION_LEN = 76, 40
SEARCH = 1024  # the block is right after the vector table, in the first 1 KB


def find(image: bytes) -> int:
    """The block's offset in an image, or -1."""
    for off in range(0, min(len(image), SEARCH) - SIZE + 1, 4):
        if image[off : off + 4] == MAGIC and image[off + 4] == 1:
            return off
    return -1


def image_id(image: bytes) -> bytes:
    off = find(image)
    if off < 0:
        raise ValueError('no build info block (OWSB) in the first 1 KB')
    b = bytearray(image)
    b[off + ID_AT : off + ID_AT + ID_LEN] = bytes(ID_LEN)
    b[off + VERSION_AT : off + VERSION_AT + VERSION_LEN] = bytes(VERSION_LEN)
    return hashlib.sha256(bytes(b)).digest()[:ID_LEN]


def parse(image: bytes) -> dict:
    off = find(image)
    if off < 0:
        raise ValueError('no build info block (OWSB) in the first 1 KB')
    _, fmt, flags, _, ident, product, board, app, version = struct.unpack_from('<4sBBH8s24s24s12s40s', image, off)

    def text(b):
        return b.split(b'\0', 1)[0].decode('ascii', 'replace')

    return {
        'offset': off,
        'format': fmt,
        'tx': bool(flags & 1),
        'debug': bool(flags & 2),
        'id': ident.hex(),
        'product': text(product),
        'board': text(board),
        'app': text(app),
        'version': text(version),
    }


def stamp(elf: pathlib.Path, objcopy: str) -> str:
    with tempfile.TemporaryDirectory() as t:
        raw = pathlib.Path(t) / 'image.bin'
        subprocess.run([objcopy, '-O', 'binary', '-R', '.ccfg', str(elf), str(raw)], check=True)
        image = raw.read_bytes()
        off = find(image)
        if off < 0:
            raise SystemExit(f'{elf}: no build info block (OWSB) in the first 1 KB')
        ident = image_id(image)
        block = bytearray(image[off : off + SIZE])
        block[ID_AT : ID_AT + ID_LEN] = ident
        sec = pathlib.Path(t) / 'ows_info.bin'
        sec.write_bytes(block)
        subprocess.run([objcopy, '--update-section', f'.ows_info={sec}', str(elf)], check=True)
    return ident.hex()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    s = sub.add_parser('stamp')
    s.add_argument('elf', type=pathlib.Path)
    s.add_argument('--objcopy', required=True)
    s = sub.add_parser('show')
    s.add_argument('image', type=pathlib.Path)
    a = ap.parse_args()
    if a.cmd == 'stamp':
        print(f'{a.elf.name}: image ID {stamp(a.elf, a.objcopy)}')
        return 0
    info = parse(a.image.read_bytes())
    for k, v in info.items():
        print(f'{k:8} {v}')
    return 0


if __name__ == '__main__':
    sys.exit(main())

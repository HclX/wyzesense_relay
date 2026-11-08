#!/usr/bin/env python3
"""TI's CC13x0 ROM serial bootloader on a plain UART: a v1 contact's RX/TX pads (DIO1/DIO0 on the 5x5 package), once
its CCFG enables the bootloader.

    uv run python tools/uart_sbl.py --port PORT check
    uv run python tools/uart_sbl.py --port PORT flash BOOT.bin SLOT_A.img --yes [--mac 777B42DF]
    uv run python tools/uart_sbl.py --port PORT flash APP.bin --yes [--mac ...]
"""

import argparse
import binascii
import os
import pathlib
import sys
import termios
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import sbl_common as sbl  # noqa: E402

GAP = float(os.environ.get('OWS_UART_GAP', 0.02))


class UartAsHid:
    """The file object sbl_common expects (read(size) -> one HID report: a length byte, then what the chip sent;
    write(packet)), over a UART. A read waits up to 5 s for the first byte (sbl_common's own timeout), then takes what
    follows until the line is quiet."""

    def __init__(self, port):
        self.fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        a = termios.tcgetattr(self.fd)
        a[0] = a[1] = 0
        a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
        a[3] = 0
        a[4] = a[5] = termios.B115200
        termios.tcsetattr(self.fd, termios.TCSANOW, a)
        termios.tcflush(self.fd, termios.TCIOFLUSH)
        self.pending = b''
        self.timeout = 5

    def _take(self):
        try:
            self.pending += os.read(self.fd, 4096)
            return True
        except BlockingIOError:
            return False

    def read(self, size):
        end = time.time() + self.timeout
        while not self.pending:
            if time.time() > end:
                raise TimeoutError('read timeout')
            if not self._take():
                time.sleep(0.002)
        quiet = time.time()
        while time.time() - quiet < GAP:
            if self._take():
                quiet = time.time()
            else:
                time.sleep(0.002)
        n = min(len(self.pending), size - 1)
        out, self.pending = self.pending[:n], self.pending[n:]
        return bytes([n]) + out

    def write(self, data):
        os.write(self.fd, data)
        termios.tcdrain(self.fd)

    def close(self):
        os.close(self.fd)


SOFT_SBL = bytes.fromhex('aa555308f0054f53424c037f') + b'\r\nsbl\r\n'


def _try_sync(u, timeout):
    u.timeout = timeout
    os.write(u.fd, b'\x55\x55')
    try:
        r = u.read(0x3F)[1:].lstrip(b'\0')
    except TimeoutError:
        r = b''
    if r[:1] == b'\xcc':
        assert sbl.sbl_cmd(u, sbl.CMD_PING), 'no answer to ping'
        return 'fresh'
    os.write(u.fd, b'\0' * 100)
    time.sleep(0.1)
    u.pending = b''
    termios.tcflush(u.fd, termios.TCIFLUSH)
    try:
        if sbl.sbl_cmd(u, sbl.CMD_PING):
            return 'synced'
    except (TimeoutError, AssertionError):
        pass
    return None


def enter(u):
    """Auto-baud, ping, chip ID; falls back to software SBL entry (OWS_SUB_ENTER_SBL + sensor 'sbl') when running
    OWS firmware is on the UART."""
    state = _try_sync(u, 0.3)
    if state is None:
        u.write(SOFT_SBL)
        time.sleep(0.05)
        u.pending = b''
        termios.tcflush(u.fd, termios.TCIFLUSH)
        state = _try_sync(u, 0.3)
    if state is None:
        raise SystemExit(
            'no bootloader: no ACK to the auto-baud sync or a ping (backdoor held at reset? bootloader enabled?)'
        )
    if state == 'synced':
        print('bootloader already synced')
    chip_id = sbl.sbl_cmd(u, sbl.CMD_GET_CHIP_ID)
    u.timeout = 5
    return chip_id


def check_page31(page31, mac):
    ccfg = page31[sbl.CCFG_ADDR - sbl.PAGE31 :]
    have = ccfg[0x24:0x2C]
    if len(have) != 8 or not set(have) <= set(b'0123456789ABCDEF') or have == b'00000000':
        raise SystemExit(f'no MAC in the CCFG ({have.hex()}): not writing')
    if mac and have != mac.encode():
        raise SystemExit(f'the CCFG MAC is {have.decode()}, not {mac}: not writing')
    bl = ccfg[sbl.BL_CONFIG : sbl.BL_CONFIG + 4]
    bl_word = int.from_bytes(bl, 'little')
    assert len(bl) == 4 and bl[0] == 0xC5 and bl[3] == 0xC5 and bl[1] < 32 and (bl[2] & 1) == 0, (
        f'BL_CONFIG {bl_word:#010x}: bootloader not enabled?'
    )
    return have.decode(), bl[1]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--port', required=True)
    sub = ap.add_subparsers(dest='cmd', required=True)
    sub.add_parser('check')
    f = sub.add_parser('flash')
    f.add_argument('files', nargs='+', type=pathlib.Path, help='BOOT.bin SLOT_A.img, or APP.bin')
    f.add_argument('--yes', action='store_true')
    f.add_argument('--mac', help='write only if the CCFG holds this MAC')
    f.add_argument(
        '--slow', action='store_true', help='status after every packet and a read-back (as ows-flash does by default)'
    )
    f.add_argument('--nv', default='keep', help='page 30: keep (default), wipe, or a 4 KB NV page binary')
    a = ap.parse_args()
    u = UartAsHid(a.port)
    chip = enter(u)
    print('bootloader: ACK; chip ID', chip.hex() if chip else '?')
    page31 = sbl.read_mem(u, sbl.PAGE31, sbl.PAGE)
    mac, pin = check_page31(page31, getattr(a, 'mac', None))
    print(f'CCFG: MAC {mac}, backdoor DIO{pin}')
    if a.cmd == 'check':
        return
    if len(a.files) == 2:
        image = sbl.slots_image(*a.files)
    elif len(a.files) == 1:
        image = a.files[0].read_bytes()
    else:
        ap.error('BOOT.bin SLOT_A.img, or APP.bin')
    image = sbl.check_image(image)
    print(f'flash: {len(image)} bytes to pages 0-{(len(image) - 1) // sbl.PAGE}, CRC {binascii.crc32(image):#010x}')
    if not a.yes:
        print('not written: add --yes')
        return
    sbl.write_pages(
        u,
        image,
        {},
        b'\xff' * sbl.PAGE31 + page31,
        fast=not a.slow,
        entered=True,
        reset=False,
        nv=lambda _current: (
            None if a.nv == 'keep' else (b'\xff' * sbl.PAGE if a.nv == 'wipe' else pathlib.Path(a.nv).read_bytes())
        ),
    )
    print("written; now reset it with S or a power cycle (not the bootloader's own reset)")


if __name__ == '__main__':
    main()

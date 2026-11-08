# tools/uart_sbl.py against a simulated CC13x0 ROM bootloader on a pseudo-terminal: no hardware. The simulation keeps
# the packet rules the tool depends on (TI SWCU117 "ROM bootloader"): 0x55 0x55 syncs once; then packets (size,
# checksum, command, data), zero bytes between them skipped; ACK 00 CC / NAK 00 33; a reply packet the host ACKs.
# It checks what reaches the flash: the image in pages 0..n, pages 30-31 untouched, nothing written when the CCFG is
# wrong; and the second connection to a bootloader that is already synced (seen on 777B42DF, 2026-10-24).
import binascii
import os
import pathlib
import random
import struct
import subprocess
import sys
import threading
import time

import pytest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import sbl_common as uniflash  # noqa: E402

STOCK_CCFG = ROOT / 'tests' / 'vectors' / 'ccfg' / 'contact_v1_777B42DF_2026-10-24.ccfg.bin'  # as it came
CHIP_ID = bytes.fromhex('20018000')


class FakeRom(threading.Thread):
    def __init__(self, fd, flash, silent=False, app_first=False):
        super().__init__(daemon=True)
        self.fd, self.flash, self.silent, self.in_app = fd, flash, silent, app_first
        self.synced, self.buf, self.stop = False, b'', False
        self.writes = []  # (address, length) of every flash write
        self.dl = None  # [address, bytes left] of the open download
        self.status = 0x40

    def reply(self, data=b''):
        os.write(self.fd, b'\0\xcc')
        if data:
            os.write(self.fd, bytes([len(data) + 2, sum(data) & 0xFF]) + data)
            self.await_ack = True

    def take(self, n):
        while len(self.buf) < n:
            try:
                self.buf += os.read(self.fd, 4096)
            except OSError:
                if self.stop:
                    raise SystemExit from None
                time.sleep(0.001)
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def run(self):
        os.set_blocking(self.fd, False)
        self.await_ack = False
        try:
            app_buf = b''
            while not self.stop:
                if self.silent:
                    self.take(1)
                    continue
                if self.in_app:
                    app_buf += self.take(1)
                    if b'sbl\r\n' in app_buf or bytes.fromhex('aa555308f0054f53424c037f') in app_buf:
                        os.write(self.fd, b'ok\r\n')
                        self.in_app, self.buf = False, b''
                    continue
                if not self.synced:
                    if self.take(1) == b'\x55' and self.take(1) == b'\x55':
                        self.synced = True
                        os.write(self.fd, b'\0\xcc')
                    continue
                b = self.take(1)[0]
                if self.await_ack:
                    if b in (0xCC, 0x33):
                        self.await_ack = False
                    continue
                if b == 0:
                    continue
                size, chk = b, self.take(1)[0]
                body = self.take(size - 2)
                if sum(body) & 0xFF != chk:
                    os.write(self.fd, b'\0\x33')
                    continue
                self.command(body[0], body[1:])
        except SystemExit:
            pass

    def command(self, cmd, d):
        def be(b):
            return int.from_bytes(b, 'big')

        if cmd in (0x20, 0x25):
            self.reply()  # ping, reset
        elif cmd == 0x28:
            self.reply(CHIP_ID)  # get chip ID
        elif cmd == 0x23:
            self.reply(bytes([self.status]))  # get status
        elif cmd == 0x2A:  # memory read
            a, n = be(d[:4]), d[5] * 4
            self.reply(bytes(self.flash[a : a + n]))
        elif cmd == 0x27:  # CRC32
            a, n = be(d[:4]), be(d[4:8])
            self.reply(struct.pack('>I', binascii.crc32(bytes(self.flash[a : a + n]))))
        elif cmd == 0x26:  # sector erase
            a = be(d[:4])
            self.flash[a : a + 0x1000] = b'\xff' * 0x1000
            self.writes.append((a, 0x1000))
            self.reply()
        elif cmd == 0x21:  # download
            self.dl = [be(d[:4]), be(d[4:8])]
            self.reply()
        elif cmd == 0x24:  # send data
            a = self.dl[0]
            for i, v in enumerate(d):
                self.flash[a + i] &= v
            self.writes.append((a, len(d)))
            self.dl[0] += len(d)
            self.dl[1] -= len(d)
            self.reply()
        else:
            os.write(self.fd, b'\0\x33')


def chip(ccfg=None):
    f = bytearray(b'\xff' * 0x20000)
    f[0x1E000:0x1E010] = b'NV page, keep me'
    f[0x1F000:0x1F010] = b'stock code here.'
    c = ccfg if ccfg is not None else uniflash.bootloader_ccfg(STOCK_CCFG.read_bytes(), 10)
    f[uniflash.CCFG_ADDR :] = c
    return f


def images(tmp):
    rnd = random.Random(1)
    boot = struct.pack('<II', 0x20004000, 0x101) + bytes(rnd.randrange(256) for _ in range(1500))
    slot = b'OWSI' + bytes(rnd.randrange(256) for _ in range(5000))
    (tmp / 'boot.bin').write_bytes(boot)
    (tmp / 'slot.img').write_bytes(slot)
    return tmp / 'boot.bin', tmp / 'slot.img'


def run(port, *args):
    return subprocess.run(
        [sys.executable, str(ROOT / 'tools' / 'uart_sbl.py'), '--port', port, *map(str, args)],
        capture_output=True,
        text=True,
        timeout=120,
        env={**os.environ, 'OWS_UART_GAP': '0.003'},
    )


@pytest.fixture
def rom():
    started = []

    def start(flash, silent=False, app_first=False):
        master, slave = os.openpty()
        r = FakeRom(master, flash, silent, app_first=app_first)
        r.start()
        started.append((r, master, slave))
        return r, os.ttyname(slave)

    yield start
    for r, master, slave in started:
        r.stop = True
        r.join(2)
        os.close(slave)
        os.close(master)


def test_flash_writes_pages_0_to_n_only(rom, tmp_path):
    flash = chip()
    before = bytes(flash)
    r, port = rom(flash)
    boot, slot = images(tmp_path)
    p = run(port, 'flash', boot, slot, '--mac', '777B42DF', '--yes')
    assert p.returncode == 0, p.stdout + p.stderr
    image = uniflash.slots_image(boot, slot)
    assert bytes(flash[: len(image)]) == image
    assert bytes(flash[0x1E000:]) == before[0x1E000:]  # pages 30-31 as they were
    assert all(a + n <= 0x1E000 for a, n in r.writes)
    assert 'now reset it' in p.stdout


def test_flash_can_wipe_page_30(rom, tmp_path):  # #61: --nv wipe; page 31 still untouched
    flash = chip()
    before = bytes(flash)
    r, port = rom(flash)
    boot, slot = images(tmp_path)
    p = run(port, 'flash', boot, slot, '--mac', '777B42DF', '--yes', '--nv', 'wipe')
    assert p.returncode == 0, p.stdout + p.stderr
    assert bytes(flash[0x1E000:0x1F000]) == b'\xff' * 0x1000 and bytes(flash[0x1F000:]) == before[0x1F000:]
    assert all(a + n <= 0x1F000 for a, n in r.writes)


def test_a_second_connection_to_a_synced_bootloader(rom):
    r, port = rom(chip())
    first = run(port, 'check')
    assert first.returncode == 0 and 'MAC 777B42DF, backdoor DIO10' in first.stdout, first.stdout + first.stderr
    second = run(port, 'check')  # synced already: 55 55 is a packet now
    assert second.returncode == 0 and 'already synced' in second.stdout, second.stdout + second.stderr


@pytest.mark.parametrize(
    'why, ccfg, extra',
    [
        ('no MAC', bytes(0x24) + bytes(8) + uniflash.bootloader_ccfg(STOCK_CCFG.read_bytes(), 10)[0x2C:], []),
        ('another unit', None, ['--mac', '777A0000']),
    ],
    ids=['no MAC', 'another unit'],
)
def test_nothing_written_when_the_ccfg_is_wrong(rom, tmp_path, why, ccfg, extra):
    r, port = rom(chip(ccfg))
    boot, slot = images(tmp_path)
    p = run(port, 'flash', boot, slot, '--yes', *extra)
    assert p.returncode != 0, why
    assert r.writes == [], why


def test_no_bootloader(rom, tmp_path):
    r, port = rom(chip(), silent=True)
    boot, slot = images(tmp_path)
    p = run(port, 'flash', boot, slot, '--yes')
    assert p.returncode != 0 and 'no bootloader' in p.stdout + p.stderr


def test_software_sbl_entry_from_running_firmware(rom, tmp_path):
    flash = chip()
    r, port = rom(flash, app_first=True)
    p = run(port, 'check')
    assert p.returncode == 0 and 'MAC 777B42DF' in p.stdout, p.stdout + p.stderr

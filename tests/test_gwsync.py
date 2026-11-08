# ows-gwsync (esp32/contact/gwsync.c: what a relay does at its start to keep its contact on the gateway image it
# carries, #76) against a simulated contact board on a pseudo-terminal: a gateway that answers HOST_OWS_BUILD_INFO
# from the build info block of what is in its flash (or not at all: an older image without one, or a hung one), and,
# after a reset with W low, the same simulated ROM bootloader as test_uart_sbl.py. Checked: the same image is not
# written again (by its ID, or by the bootloader's CRC when the gateway is silent); another one is replaced, pages
# 30-31 untouched, and checked by its new answer; a contact without a bootloader, or a carried image that is not a
# gateway, is left alone. No hardware.
import contextlib
import os
import pathlib
import pty
import random
import select
import struct
import subprocess
import sys
import time

import pytest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import host_proto as host  # noqa: E402
import image_id  # noqa: E402
from test_uart_sbl import STOCK_CCFG, FakeRom, chip  # noqa: E402

HOST_BUILD = pathlib.Path(os.environ.get('OWS_HOST_BUILD', ROOT / 'build' / 'host'))
GWSYNC = HOST_BUILD / 'ows-gwsync'

pytestmark = pytest.mark.skipif(not GWSYNC.exists(), reason='build the host targets first: cmake --build --preset host')


def image(seed, app='gateway', with_block=True, size=9000, version=None):
    rnd = random.Random(seed)
    ver = (version if version is not None else f'v0.{seed}-test').encode()
    b = bytearray(struct.pack('<II', 0x20005000, 0x335)) + bytes(56)
    if with_block:
        b += struct.pack(
            '<4sBBH8s24s24s12s40s',
            b'OWSB',
            1,
            1,
            0,
            bytes(8),
            b'gateway-contact-v1',
            b'Wyze Sense v1 contact',
            app.encode(),
            ver,
        )
    b += bytes(rnd.randrange(256) for _ in range(size - len(b)))
    if with_block:
        b[64 + 8 : 64 + 16] = image_id.image_id(bytes(b))
    return bytes(b)


class FakeContact(FakeRom):
    """The contact: its gateway firmware, or (after "B" on the control FIFO or OWS_SUB_ENTER_SBL) its ROM bootloader."""

    def __init__(self, fd, ctl, flash, hung=False, chatter=False, soft_sbl=False, stock_mac=None):
        super().__init__(fd, flash)
        self.ctl, self.hung, self.chatter, self.soft_sbl = ctl, hung, chatter, soft_sbl
        self.stock_mac = stock_mac
        self.mode, self.pads, self.gwbuf = 'gw', [], b''
        self.jtag_image = None  # what "J" (written through the debug port, #81) puts in pages 0-29

    def run(self):
        os.set_blocking(self.fd, False)
        self.await_ack = False
        while not self.stop:
            r, _, _ = select.select([self.fd, self.ctl], [], [], 0.002 if self.chatter else 0.05)
            if self.chatter:  # a line that never goes quiet (noise, a board in a loop): no read ever times out
                with contextlib.suppress(OSError):
                    os.write(self.fd, b'\x5a' * 32)
            if self.ctl in r:
                for line in os.read(self.ctl, 64).decode().split():
                    self.pads.append(line)
                    if line == 'J' and self.jtag_image is not None:  # the debug port writes, then resets into it
                        if len(self.jtag_image) > 0xFA8 and (len(self.jtag_image) - 0xFA8) % 4096 == 0:
                            app_len = len(self.jtag_image) - 0xFA8
                            self.flash[:app_len] = self.jtag_image[:app_len]
                            self.flash[0x1F000 : 0x1F000 + 0xFA8] = self.jtag_image[app_len:]
                        else:
                            self.flash[: len(self.jtag_image)] = self.jtag_image
                    self.mode = 'rom' if line == 'B' else 'gw'
                    self.synced, self.buf, self.gwbuf, self.await_ack = False, b'', b'', False
                    with contextlib.suppress(OSError):  # what was on the line before the reset is lost
                        os.read(self.fd, 4096)
            if self.fd in r:
                try:
                    data = os.read(self.fd, 4096)
                except OSError:
                    continue
                if self.mode == 'gw':
                    self.gateway(data)
                else:
                    self.buf += data
                    self.rom()

    def gateway(self, data):
        self.gwbuf += data
        for p in host.scan(self.gwbuf):
            m = host.parse(p)
            if m['to_host'] or self.hung:
                continue
            is_stock = image_id.find(bytes(self.flash[:1024])) < 0 and self.flash[0x1F000] != 0xFF
            if m['cmd'] == host.OWS_CMD and m['payload'][:1] == bytes([host.OWS_SUB_BUILD_INFO]):
                if is_stock:
                    continue
                os.write(self.fd, host.build_ack(host.OWS_CMD, to_host=True))
                part = m['payload'][1] if len(m['payload']) > 1 else 0
                at = image_id.find(bytes(self.flash[:1024]))
                if at >= 0:  # an image without the block is older firmware: no answer
                    blk = bytes(self.flash[at : at + 116])
                    if part == 0:
                        rsp = bytes([host.OWS_SUB_BUILD_INFO]) + blk[:40] + blk[64:76]
                    elif part == 1:
                        rsp = bytes([host.OWS_SUB_BUILD_INFO]) + blk[8:16] + blk[40:64] + blk[76:98]
                    elif part == 2:
                        rsp = bytes([host.OWS_SUB_BUILD_INFO]) + blk[8:16] + blk[98:116]
                    else:
                        rsp = bytes([host.OWS_SUB_BUILD_INFO])
                    os.write(self.fd, host.build(host.OWS_RSP, rsp, to_host=True))
            elif m['cmd'] == 0x5316 and is_stock:
                os.write(self.fd, host.build_ack(0x5316, to_host=True))
                os.write(self.fd, host.build(0x5317, STOCK_VERSION.encode(), to_host=True))
            elif m['cmd'] == 0x5304 and is_stock:
                os.write(self.fd, host.build_ack(0x5304, to_host=True))
                mac = self.stock_mac if self.stock_mac is not None else b'\xff' * 8
                os.write(self.fd, host.build(0x5305, mac, to_host=True))
            elif m['cmd'] == 0x5308 and is_stock and len(m['payload']) == 8:
                os.write(self.fd, host.build_ack(0x5308, to_host=True))
                self.stock_mac = m['payload']
                os.write(self.fd, host.build(0x5309, bytes([0x01]), to_host=True))
            elif m['cmd'] == 0x4304 and not is_stock:
                os.write(self.fd, host.build_ack(0x4304, to_host=True))
                mac = bytes(self.flash[0x1FFCC : 0x1FFCC + 8])
                os.write(self.fd, host.build(0x4305, mac, to_host=True))
            elif (
                self.soft_sbl and m['cmd'] == host.OWS_CMD and m['payload'] == bytes([host.OWS_SUB_ENTER_SBL]) + b'OSBL'
            ):
                os.write(self.fd, host.build_ack(host.OWS_CMD, to_host=True))
                bl_ok = self.flash[0x1FFA8 + 0x33] == 0xC5
                os.write(
                    self.fd,
                    host.build(
                        host.OWS_RSP,
                        bytes([host.OWS_SUB_ENTER_SBL, 0x00 if bl_ok else 0xE0]),
                        to_host=True,
                    ),
                )
                if bl_ok:
                    self.mode, self.synced, self.buf, self.await_ack = 'rom', False, b'', False
        self.gwbuf = b''

    def rom(self):  # FakeRom.run's packet rules, on what has come
        while self.buf:
            if not self.synced:
                if len(self.buf) < 2:
                    return
                if self.buf[:2] == b'\x55\x55':
                    self.synced = True
                    os.write(self.fd, b'\0\xcc')
                    self.buf = self.buf[2:]
                else:
                    self.buf = self.buf[1:]
                continue
            b = self.buf[0]
            if self.await_ack or b == 0:
                self.await_ack = self.await_ack and b not in (0xCC, 0x33)
                self.buf = self.buf[1:]
                continue
            if len(self.buf) < 2 or len(self.buf) < b:
                return
            chk, body = self.buf[1], self.buf[2:b]
            self.buf = self.buf[b:]
            if sum(body) & 0xFF != chk:
                os.write(self.fd, b'\0\x33')
                continue
            self.command(body[0], body[1:])


@pytest.fixture
def contact(tmp_path):
    started = []

    def start(flash, **kw):
        master, slave = pty.openpty()
        fifo = tmp_path / 'ctl'
        os.mkfifo(fifo)
        ctl = os.open(fifo, os.O_RDONLY | os.O_NONBLOCK)
        c = FakeContact(master, ctl, flash, **kw)
        c.start()
        started.append(c)
        return c, os.ttyname(slave), fifo

    yield start
    for c in started:
        c.stop = True


def run(port, fifo, img, tmp_path, extra=()):
    f = tmp_path / 'carried.bin'
    f.write_bytes(img)
    p = subprocess.run(
        [str(GWSYNC), *extra, '--port', port, '--ctl', str(fifo), str(f)],
        capture_output=True,
        text=True,
        timeout=120,
        env={**os.environ, 'OWS_UART_GAP_MS': '3'},
    )
    out = dict(line.split(': ', 1) for line in p.stdout.splitlines() if ': ' in line)
    return p.returncode, out


def query(port):
    p = subprocess.run(
        [str(GWSYNC), '--query', '--port', port],
        capture_output=True,
        text=True,
        timeout=30,
        env={**os.environ, 'OWS_UART_GAP_MS': '3'},
    )
    out = dict(line.split(': ', 1) for line in p.stdout.splitlines() if ': ' in line)
    return p.returncode, out


def put(flash, img):
    flash[: len(img)] = img


def test_same_image_not_written(contact, tmp_path):
    a = image(1)
    flash = chip()
    put(flash, a)
    c, port, fifo = contact(flash)
    code, out = run(port, fifo, a, tmp_path)
    assert code == 0 and out['result'] == 'same' and out['answered'] == 'yes' and out['mac'] == '777B42DF'
    assert c.pads == ['R'] and not c.writes  # never in the bootloader
    assert out['step'].startswith('asking the gateway')  # the steps a status page shows


def test_another_image_is_replaced(contact, tmp_path):
    a, b = image(1), image(2)
    flash = chip()
    put(flash, b)
    before = bytes(flash)
    c, port, fifo = contact(flash)
    code, out = run(port, fifo, a, tmp_path)
    assert code == 0 and out['result'] == 'updated', out
    assert bytes(flash[: len(a)]) == a and bytes(flash[0x1E000:]) == before[0x1E000:]
    assert all(addr + n <= 0x1E000 for addr, n in c.writes)
    assert out['mac'] == '777B42DF' and c.pads == ['R', 'B', 'R']  # start, bootloader, into the new image


def test_4wire_software_sbl_without_w_pad(contact, tmp_path):
    a, b = image(1), image(2)
    flash = chip()
    put(flash, b)
    before = bytes(flash)
    c, port, fifo = contact(flash, soft_sbl=True)
    code, out = run(port, fifo, a, tmp_path)
    assert code == 0 and out['result'] == 'updated', out
    assert bytes(flash[: len(a)]) == a and bytes(flash[0x1E000:]) == before[0x1E000:]
    assert out['mac'] == '777B42DF' and c.pads == ['R', 'R']  # never needed W ('B')!


def test_older_firmware_without_an_answer_is_replaced(contact, tmp_path):
    a = image(1)
    flash = chip()
    put(flash, image(3, with_block=False))  # an image from before #76: it does not answer
    c, port, fifo = contact(flash)
    code, out = run(port, fifo, a, tmp_path)
    assert code == 0 and out['result'] == 'updated' and out['answered'] == 'no'
    assert bytes(flash[: len(a)]) == a


def test_a_silent_gateway_with_the_image_is_not_written(contact, tmp_path):
    a = image(1)
    flash = chip()
    put(flash, a)
    c, port, fifo = contact(flash, hung=True)
    code, out = run(port, fifo, a, tmp_path)
    assert code == 0 and out['result'] == 'same' and 'CRC' in out['message']
    assert not c.writes


# The debug port (cJTAG on M/C, #81): tried first; auto falls back to the bootloader; cjtag-only never does


def test_another_image_is_replaced_through_the_debug_port(contact, tmp_path):
    a, b = image(1), image(2)
    flash = chip()
    put(flash, b)
    before = bytes(flash)
    c, port, fifo = contact(flash)
    c.jtag_image = a
    code, out = run(port, fifo, a, tmp_path, extra=('--cjtag', 'ok'))
    assert code == 0 and out['result'] == 'updated' and 'debug port' in out['message'], out
    assert bytes(flash[: len(a)]) == a and bytes(flash[0x1E000:]) == before[0x1E000:]
    assert c.pads == ['R', 'J'] and not c.writes  # never in the bootloader


def test_a_silent_gateway_with_the_image_through_the_debug_port(contact, tmp_path):
    a = image(1)
    flash = chip()
    put(flash, a)
    c, port, fifo = contact(flash, hung=True)
    code, out = run(port, fifo, a, tmp_path, extra=('--cjtag', 'same'))
    assert code == 0 and out['result'] == 'same' and 'cJTAG' in out['message'], out
    assert c.pads == ['R'] and not c.writes


def test_auto_falls_back_to_the_bootloader(contact, tmp_path):
    a, b = image(1), image(2)
    flash = chip()
    put(flash, b)
    c, port, fifo = contact(flash)
    code, out = run(port, fifo, a, tmp_path, extra=('--cjtag', 'fail'))
    assert code == 0 and out['result'] == 'updated' and 'debug port' not in out['message'], out
    assert bytes(flash[: len(a)]) == a and c.pads == ['R', 'B', 'R']


def test_cjtag_only_never_uses_the_bootloader(contact, tmp_path):
    a, b = image(1), image(2)
    flash = chip()
    put(flash, b)
    before = bytes(flash)
    c, port, fifo = contact(flash)
    code, out = run(port, fifo, a, tmp_path, extra=('--cjtag', 'fail', '--transport', 'cjtag'))
    assert code == 1 and out['result'] == 'failed' and 'debug port' in out['message'], out
    assert bytes(flash) == before and c.pads == ['R'] and not c.writes


def test_sbl_only_ignores_the_debug_port(contact, tmp_path):
    a, b = image(1), image(2)
    flash = chip()
    put(flash, b)
    c, port, fifo = contact(flash)
    c.jtag_image = image(3)  # would be wrong: must not be used
    code, out = run(port, fifo, a, tmp_path, extra=('--cjtag', 'ok', '--transport', 'sbl'))
    assert code == 0 and out['result'] == 'updated', out
    assert bytes(flash[: len(a)]) == a and 'J' not in c.pads


def test_a_chattering_line_does_not_hang_it(contact, tmp_path):
    # the LOLIN32's start-up check never ended once (2026-10-25); a line that keeps sending is one way a wait without a
    # byte budget never ends. (This simulation does not make the old code hang: it only shows the check ends.)
    flash = chip()
    put(flash, image(2))
    before = bytes(flash)
    c, port, fifo = contact(flash, chatter=True)
    t = time.monotonic()
    code, out = run(port, fifo, image(1), tmp_path)
    assert time.monotonic() - t < 60 and code != 0 and out['result'] in ('refused', 'failed'), out
    assert int(out['junk']) > 0 and bytes(flash) == before


def test_no_bootloader_left_alone(contact, tmp_path):
    flash = chip(STOCK_CCFG.read_bytes())  # the stock CCFG: the bootloader disabled
    put(flash, image(2))
    before = bytes(flash)
    c, port, fifo = contact(flash)
    code, out = run(port, fifo, image(1), tmp_path)
    assert code != 0 and out['result'] == 'refused' and bytes(flash) == before


@pytest.mark.parametrize('carried', [image(1, app='sensor'), image(1, with_block=False)])
def test_not_a_gateway_image_left_alone(contact, tmp_path, carried):
    flash = chip()
    put(flash, image(2))
    before = bytes(flash)
    c, port, fifo = contact(flash)
    code, out = run(port, fifo, carried, tmp_path)
    time.sleep(0.1)
    assert code != 0 and out['result'] == 'refused' and bytes(flash) == before and not c.pads


def test_query_live_build_info(contact):
    long_ver = 'v0.99-rc1-20261029-gdeadbeef-dirty'  # > 22 chars: exercises part 2 reassembly
    flash = chip()
    put(flash, image(1, version=long_ver))
    c, port, _ = contact(flash)
    code, out = query(port)
    assert code == 0 and out['answered'] == 'yes' and not c.pads
    assert out['product'] == 'gateway-contact-v1' and out['board'] == 'Wyze Sense v1 contact'
    assert out['app'] == 'gateway' and out['version'] == long_ver and out['tx'] == '1'


def test_query_silent_gateway(contact):
    flash = chip()
    put(flash, image(3, with_block=False))
    c, port, _ = contact(flash)
    code, out = query(port)
    assert code != 0 and out['answered'] == 'no' and not c.pads


def test_skip_probe_checks_without_flashing(contact, tmp_path):
    a, b = image(1), image(2)
    flash = chip()
    put(flash, b)
    before = bytes(flash)
    c, port, fifo = contact(flash)
    code, out = run(port, fifo, a, tmp_path, extra=('--skip',))
    assert code == 0 and out['result'] == 'same' and 'auto-sync paused' in out['message']
    assert out['answered'] == 'yes' and out['mac'] == '777B42DF' and not c.writes and c.pads == ['R']
    assert bytes(flash) == before


def test_the_real_gateway_image_has_a_block():  # what the relays will carry
    real = ROOT / 'firmware' / 'gateway-contact-v1-app.bin'
    if not real.exists():
        pytest.skip('build the fw preset first')
    info = image_id.parse(real.read_bytes())
    assert info['app'] == 'gateway' and info['product'] == 'gateway-contact-v1' and info['tx']


STOCK_VERSION = '0.0.0.47 V1.8 Gateway GW3U'  # the stock v1.8 gateway's answer to 0x5316 (FACTS.md)


def stock_image(pages=2):
    rnd = random.Random(99)
    app = bytearray(struct.pack('<II', 0x20005000, 0x00010539))
    app += bytes(rnd.randrange(256) for _ in range(pages * 4096 - len(app)))
    p31_code = bytes(0x4D ^ (i & 0xFF) for i in range(0xFA8))
    return bytes(app) + p31_code


def test_stock_gateway_same_image_not_written(contact, tmp_path):
    stk = stock_image(2)
    flash = chip()
    flash[: 2 * 4096] = stk[: 2 * 4096]
    flash[0x1F000 : 0x1F000 + 0xFA8] = stk[2 * 4096 :]
    c, port, fifo = contact(flash, stock_mac=b'777B42DF')
    code, out = run(port, fifo, stk, tmp_path)
    assert code == 0 and out['result'] == 'same' and out['answered'] == 'yes' and out['stock'] == 'yes', out
    assert out['carried'] == STOCK_VERSION and out['running'] == STOCK_VERSION and out['mac'] == '777B42DF'
    assert c.pads == ['R'] and not c.writes


def test_stock_gateway_replaces_ows_and_provisions_mac(contact, tmp_path):
    stk = stock_image(2)
    flash = chip()
    put(flash, image(1))
    before_ccfg = bytes(flash[0x1FFA8:0x20000])
    c, port, fifo = contact(flash, stock_mac=None)
    c.jtag_image = stk
    code, out = run(port, fifo, stk, tmp_path, extra=('--cjtag', 'ok'))
    assert code == 0 and out['result'] == 'updated' and out['stock'] == 'yes', out
    assert out['running'] == STOCK_VERSION and out['mac'] == '777B42DF'
    assert c.stock_mac == b'777B42DF'
    assert bytes(flash[: 2 * 4096]) == stk[: 2 * 4096]
    assert bytes(flash[0x1F000 : 0x1F000 + 0xFA8]) == stk[2 * 4096 :]
    assert bytes(flash[0x1FFA8:0x20000]) == before_ccfg


def test_stock_gateway_never_over_the_bootloader(contact, tmp_path):
    # without the debug port the bootloader could write pages 0-29 only: the stock page-31 code would be left out
    stk = stock_image(2)
    flash = chip()
    put(flash, image(1))
    before = bytes(flash)
    c, port, fifo = contact(flash, stock_mac=None)
    code, out = run(port, fifo, stk, tmp_path)
    assert code != 0 and out['result'] == 'failed' and any('debug port' in v for v in out.values()), out
    assert bytes(flash) == before

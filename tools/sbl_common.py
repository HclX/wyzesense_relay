#!/usr/bin/env python3
"""TI's CC13x0 ROM serial bootloader (SBL): the packet framing WyzeSenseUpdater uses through the Wyze dongle's USB HID
(https://github.com/HclX/WyzeSenseUpdater, ws_updater.py), and the guarded write path tools/uart_sbl.py uses on a v1
contact's UART. The dongle itself is flashed with ows-flash (crates/ows-flash, crates/ows-host; #44 retired the Python
dongle tool this came from); the same checks are in crates/ows-host's sbl.rs and dongle.rs.

Pages 0-29 only, and page 30 inside write_nv(): every erase and download is checked against APP_END at the last step
before the chip; bank erase, set-CCFG and memory-write are not in this file. write_pages() checks the image (size,
vector table), that page 31 is the expected one, verifies what it wrote (the chip's CRC32 and a read-back unless
fast) and that pages 30-31 did not change, and only then resets the chip.
"""

import binascii
import logging
import signal
import time

# --- WyzeSenseUpdater's framing (ws_updater.py, unchanged but for names) ---

SBL_MAX_MEMREAD_BYTES = 63 * 4
RSP_CODE_ACK = 0x00CC
CMD_PING, CMD_DOWNLOAD, CMD_GET_STATUS, CMD_SEND_DATA, CMD_RESET = 0x20, 0x21, 0x23, 0x24, 0x25
CMD_SECTOR_ERASE, CMD_CRC32 = 0x26, 0x27
CMD_GET_CHIP_ID, CMD_MEMORY_READ, CMD_AUTO_BAUD = 0x28, 0x2A, 0x55
UPDATE_MODE_REQUEST = b'\x07\xaa\x55\x43\x03\x12\x01\x57'  # host command 0x4312, HID length byte first
MAX_PACK_SIZE = 0x3A
PAGE = 0x1000
APP_END = 0x1E000  # flash pages 0-29: the only ones `flash` writes
READ_ONLY = (CMD_PING, CMD_GET_STATUS, CMD_RESET, CMD_CRC32, CMD_GET_CHIP_ID, CMD_MEMORY_READ, CMD_AUTO_BAUD)


def dongle_read(fd, size):
    def handler(signum, frame):
        raise TimeoutError('read timeout')

    old = signal.signal(signal.SIGALRM, handler)
    signal.alarm(5)
    try:
        return fd.read(size)
    finally:
        signal.alarm(0)
        signal.signal(signal.SIGALRM, old)


def rawhid_read(fd):
    pkt = dongle_read(fd, 0x3F)
    logging.debug(f'[R] {pkt.hex()}')
    return pkt[1 : 1 + pkt[0]]


def rawhid_write(fd, payload):
    logging.debug(f'[W] {payload.hex()}')
    fd.write(payload)


def checksum(cmd, data=b''):
    return (cmd + sum(data)) & 0xFF


def ack(fd, success):
    rawhid_write(fd, bytes([0x00, 0xCC if success else 0x33]))


NV_PAGE = 0x1E000  # page 30: written only by write_nv() (#61)
_nv_writing = False


def below_app_end(addr, size):
    """The only flash this tool erases or programs: pages 0-29, and page 30 inside write_nv(); never page 31."""
    if addr >= 0 and size > 0 and addr + size <= APP_END:
        return
    if _nv_writing and addr >= NV_PAGE and size > 0 and addr + size <= NV_PAGE + PAGE:
        return
    raise ValueError(
        f'refusing {addr:#x}+{size:#x}: only flash below {APP_END:#x} (pages 0-29) is ever written, '
        'and page 30 by write_nv()'
    )


def sbl_cmd(fd, cmd, payload=b''):
    # the only writing commands, each with its target checked again here, at the last step before the chip
    if cmd == CMD_SECTOR_ERASE:
        below_app_end(int.from_bytes(payload[:4], 'big'), PAGE)
    elif cmd == CMD_DOWNLOAD:
        below_app_end(int.from_bytes(payload[:4], 'big'), int.from_bytes(payload[4:8], 'big'))
    elif cmd != CMD_SEND_DATA:
        assert cmd in READ_ONLY, f'command {cmd:#x} is not used by this tool'
        # (bank erase 0x2C, memory write 0x2B, set CCFG 0x2D: never)
    rawhid_write(fd, bytes([len(payload) + 3, checksum(cmd, payload), cmd]) + payload)
    rsp = rawhid_read(fd)
    assert len(rsp) >= 2, 'invalid response'
    if (rsp[0] << 8 | rsp[1]) != RSP_CODE_ACK:
        return False
    if len(rsp) == 2:
        return True
    assert len(rsp) >= 4, 'invalid payload'
    n = rsp[2] - 2
    want = rsp[3]
    data = rsp[4:]
    assert n >= len(data), 'invalid payload length'
    n -= len(data)
    while n > 0:
        more = rawhid_read(fd)[:n]
        data += more
        n -= len(more)
    if checksum(0, data) != want:
        ack(fd, False)
        return None
    ack(fd, True)
    return data


def read_mem(fd, addr, size):
    assert addr % 4 == 0 and size % 4 == 0 and size > 0
    data = b''
    while size > 0:
        n = min(size, SBL_MAX_MEMREAD_BYTES)
        rsp = sbl_cmd(fd, CMD_MEMORY_READ, addr.to_bytes(4, 'big') + bytes([1, n // 4]))
        if not rsp or len(rsp) != n:
            raise RuntimeError(f'read failed at {addr:#010x}')
        data += rsp
        addr += n
        size -= n
        print(f'\r  {addr:#010x}', end='', flush=True)
    print()
    return data


def chip_crc32(fd, addr, size):
    rsp = sbl_cmd(fd, CMD_CRC32, addr.to_bytes(4, 'big') + size.to_bytes(4, 'big') + b'\0\0\0\0')
    return int.from_bytes(rsp, 'big') if rsp and len(rsp) == 4 else None


def status_ok(fd):
    rsp = sbl_cmd(fd, CMD_GET_STATUS)
    return bool(rsp) and len(rsp) == 1 and rsp[0] == 0x40  # COMMAND_RET_SUCCESS


def enter(fd):
    """Update mode, then the bootloader: returns (reply to 0x4312, chip ID)."""
    rawhid_write(fd, UPDATE_MODE_REQUEST)
    rsp = rawhid_read(fd)
    assert sbl_cmd(fd, CMD_AUTO_BAUD), 'no bootloader (auto baud)'
    assert sbl_cmd(fd, CMD_PING), 'no bootloader (ping)'
    return rsp, sbl_cmd(fd, CMD_GET_CHIP_ID)


# --- flash ---


def check_image(image):
    """Our image: fits below APP_END, and starts with a vector table (initial SP in RAM, reset handler in it)."""
    if not image or len(image) > APP_END:
        raise ValueError(
            f"image is {len(image)} bytes: must fit below {APP_END:#x} (the build's <product>-app.bin, "
            'without the CCFG; not <product>.bin)'
        )
    sp, reset = int.from_bytes(image[0:4], 'little'), int.from_bytes(image[4:8], 'little')
    if not (0x20000000 < sp <= 0x20005000) or not (reset & 1) or (reset & ~1) >= len(image):
        raise ValueError(f'no vector table: SP {sp:#x}, reset {reset:#x}')
    return image + b'\xff' * (-len(image) % 4)


def runs(image, skip_ff):
    """(offset, bytes) stretches to send: the whole image, or (fast) only its stretches that are not all FF (erased
    flash reads FF already), in 4-byte words, with gaps under 256 bytes merged (a new download costs a round trip)."""
    if not skip_ff:
        return [(0, image)]
    words = [k for k in range(0, len(image), 4) if image[k : k + 4] != b'\xff\xff\xff\xff']
    out = []
    for k in words:
        if out and k - (out[-1][0] + out[-1][1]) < 256:
            out[-1][1] = k + 4 - out[-1][0]
        else:
            out.append([k, 4])
    return [(a, image[a : a + n]) for a, n in out]


def write_nv(fd, page):
    """Page 30 <- page (4096 bytes, our NV format or all FF), verified by the chip's CRC32."""
    global _nv_writing
    assert len(page) == PAGE
    _nv_writing = True
    try:
        sbl_cmd(fd, CMD_SECTOR_ERASE, NV_PAGE.to_bytes(4, 'big'))
        assert status_ok(fd), 'erasing page 30 failed'
        for start, data in runs(page, True):
            a = NV_PAGE + start
            assert sbl_cmd(fd, CMD_DOWNLOAD, a.to_bytes(4, 'big') + len(data).to_bytes(4, 'big')) and status_ok(fd)
            for off in range(0, len(data), MAX_PACK_SIZE):
                assert sbl_cmd(fd, CMD_SEND_DATA, data[off : off + MAX_PACK_SIZE]), (
                    f'sending page 30 failed at {a + off:#x}'
                )
            assert status_ok(fd), f'programming page 30 at {a:#x} failed'
    finally:
        _nv_writing = False
    assert chip_crc32(fd, NV_PAGE, PAGE) == binascii.crc32(page), 'page 30 CRC differs from what was written'


def write_pages(fd, image, info, flash_backup, fast=False, entered=False, reset=True, nv=None):
    """Pages 0..n-1 <- image, with every check; raises before reset if anything is off. fast: GET_STATUS once per
    stretch instead of after every packet, all-FF stretches skipped, verified by the chip's CRC32 alone (no read-back;
    pages 30-31 still checked). Returns the seconds each phase took."""
    t = {'start': time.time()}
    if not entered:
        _, chip_id = enter(fd)
        assert chip_id.hex() == info['chip_id'], f"chip ID {chip_id.hex()} is not the backup's {info['chip_id']}"
    assert chip_crc32(fd, 0x1F000, PAGE) == binascii.crc32(flash_backup[0x1F000:]), "page 31 is not the backup's"
    keep = chip_crc32(fd, APP_END, 0x20000 - APP_END)  # pages 30-31 now (30 may have changed since)
    print(f'pages 30-31 CRC {keep:#010x} (page 31 as in the backup)')
    t['checked'] = time.time()
    pages = (len(image) + PAGE - 1) // PAGE
    for p in range(pages):
        sbl_cmd(fd, CMD_SECTOR_ERASE, (p * PAGE).to_bytes(4, 'big'))
        assert status_ok(fd), f'erasing page {p} failed'
        print(f'\r  erased page {p}', end='', flush=True)
    print()
    t['erased'] = time.time()
    sent = 0
    for start, data in runs(image, fast):
        assert sbl_cmd(fd, CMD_DOWNLOAD, start.to_bytes(4, 'big') + len(data).to_bytes(4, 'big')) and status_ok(fd), (
            f'download at {start:#x} refused'
        )
        off, tries = 0, 0
        while off < len(data):
            chunk = data[off : off + MAX_PACK_SIZE]
            if sbl_cmd(fd, CMD_SEND_DATA, chunk) and (fast or status_ok(fd)):
                off, tries, sent = off + len(chunk), 0, sent + len(chunk)
                print(f'\r  written {start + off:#07x}/{len(image):#07x}', end='', flush=True)
            else:
                tries += 1
                assert tries <= 3, f'sending data failed at {start + off:#x}'
        if fast:
            assert status_ok(fd), f'programming the stretch at {start:#x} failed'
    print()
    t['written'] = time.time()
    crc = chip_crc32(fd, 0, len(image))
    assert crc == binascii.crc32(image), f'chip CRC {crc:#x} != image CRC {binascii.crc32(image):#x}'
    if not fast:
        assert read_mem(fd, 0, len(image)) == image, 'read-back differs'
    assert chip_crc32(fd, APP_END, 0x20000 - APP_END) == keep, 'pages 30-31 changed!'
    if nv is not None:  # nv(current page 30) -> what to write there (#61), after the image is verified; None: nothing
        page = nv(read_mem(fd, NV_PAGE, PAGE))
        if page is not None:
            write_nv(fd, page)
        assert chip_crc32(fd, 0x1F000, PAGE) == binascii.crc32(flash_backup[0x1F000:]), 'page 31 changed!'
    t['verified'] = time.time()
    print(f'verified: chip CRC{"" if fast else " and read-back"} match, page 31 unchanged')
    if reset:
        sbl_cmd(fd, CMD_RESET)
    phases = {
        'checks': t['checked'] - t['start'],
        'erase': t['erased'] - t['checked'],
        'write': t['written'] - t['erased'],
        'verify': t['verified'] - t['written'],
        'total': t['verified'] - t['start'],
        'bytes_sent': sent,
    }
    print('  ' + ', '.join(f'{k} {v:.2f} s' if k != 'bytes_sent' else f'{v} bytes sent' for k, v in phases.items()))
    return phases


# --- CC1310 layout & CCFG helpers (shared by uart_sbl.py, contact_image.py, and tests) ---

SLOT_A, SLOT_B = 0x01000, 0x0F000
CCFG_ADDR, PAGE31 = 0x1FFA8, 0x1F000
BL_CONFIG = 0x30  # offset in the CCFG
MAX_BL_PIN = 14  # 5x5 package: DIO0-14 (TI errata SWRZ062F advisory 13)


def slots_image(boot, slot_a):
    """The two-slot layout as one image at 0x0: the boot selector (page 0), slot A, and slot B's first page blank."""
    import pathlib as _pathlib

    b, a = _pathlib.Path(boot).read_bytes(), _pathlib.Path(slot_a).read_bytes()
    if len(b) > SLOT_A or len(a) > SLOT_B - SLOT_A or a[:4] != b'OWSI':
        raise ValueError('a boot selector of at most one page and a slot image (OWSI) expected')
    img = bytearray(b'\xff' * (SLOT_B + 0x1000))
    img[: len(b)] = b
    img[SLOT_A : SLOT_A + len(a)] = a
    return bytes(img)


def bootloader_ccfg(ccfg, pin):
    """The unit's CCFG with only BL_CONFIG changed: ROM bootloader on, backdoor on DIO<pin>, active low."""
    if len(ccfg) != 88 or not 0 <= pin <= MAX_BL_PIN:
        raise ValueError('an 88-byte CCFG and a backdoor pin 0-14 expected')
    new = bytearray(ccfg)
    new[BL_CONFIG : BL_CONFIG + 4] = (0xC5FE00C5 | pin << 8).to_bytes(4, 'little')

    def w(c, o):
        return int.from_bytes(c[o : o + 4], 'little')

    assert new[:BL_CONFIG] + new[BL_CONFIG + 4 :] == ccfg[:BL_CONFIG] + ccfg[BL_CONFIG + 4 :]
    assert w(new, 0x3C) == 0xFFC500C5, 'the CPU debug port must stay enabled (TAP_DAP_0)'
    assert w(new, 0x44) == 0, 'IMAGE_VALID_CONF must stay 0 (the flash image runs)'
    return bytes(new)

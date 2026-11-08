# ows-sbl (esp32/contact/sbl.c: the bootloader code the ESP32 runs to reflash its contact board, #22) against the same
# simulated ROM as tools/uart_sbl.py (test_uart_sbl.py): the image lands in pages 0..n, pages 30-31 are untouched
# (page 30 only with --nv), nothing is written for a CCFG with no MAC or another unit's, a synced bootloader is found
# again, a silent port is reported. No hardware.
import os
import pathlib
import subprocess
import sys

import pytest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / 'tools'))
import sbl_common as uniflash  # noqa: E402
import uart_sbl  # noqa: E402
from test_uart_sbl import STOCK_CCFG, chip, images, rom  # noqa: E402, F401 (rom: the fixture)

ROOT = pathlib.Path(__file__).resolve().parents[1]
HOST_BUILD = pathlib.Path(os.environ.get('OWS_HOST_BUILD', ROOT / 'build' / 'host'))
SBL = HOST_BUILD / 'ows-sbl'

pytestmark = pytest.mark.skipif(not SBL.exists(), reason='build the host targets first: cmake --build --preset host')


def run(port, *args):
    return subprocess.run(
        [str(SBL), '--port', port, *map(str, args)],
        capture_output=True,
        text=True,
        timeout=120,
        env={**os.environ, 'OWS_UART_GAP_MS': '3'},
    )


def test_flash_writes_pages_0_to_n_only(rom, tmp_path):  # noqa: F811
    flash = chip()
    before = bytes(flash)
    r, port = rom(flash)
    boot, slot = images(tmp_path)
    p = run(port, 'flash', boot, slot, '--mac', '777B42DF', '--yes')
    assert p.returncode == 0, p.stdout + p.stderr
    image = uniflash.slots_image(boot, slot)
    assert bytes(flash[: len(image)]) == image
    assert bytes(flash[0x1E000:]) == before[0x1E000:]
    assert all(a + n <= 0x1E000 for a, n in r.writes)
    assert 'now reset it' in p.stdout and 'backdoor DIO10' in p.stdout


def test_nv_wipe_and_an_nv_page(rom, tmp_path):  # noqa: F811
    for nv, want in (('wipe', b'\xff' * 0x1000), ('page', bytes(range(256)) * 16)):
        flash = chip()
        before = bytes(flash)
        r, port = rom(flash)
        boot, slot = images(tmp_path)
        arg = nv
        if nv == 'page':
            (tmp_path / 'nv.bin').write_bytes(want)
            arg = tmp_path / 'nv.bin'
        p = run(port, 'flash', boot, slot, '--yes', '--nv', arg)
        assert p.returncode == 0, p.stdout + p.stderr
        assert bytes(flash[0x1E000:0x1F000]) == want and bytes(flash[0x1F000:]) == before[0x1F000:]
        assert all(a + n <= 0x1F000 for a, n in r.writes)


def test_a_second_connection_to_a_synced_bootloader(rom):  # noqa: F811
    r, port = rom(chip())
    assert run(port, 'check').returncode == 0
    second = run(port, 'check')
    assert second.returncode == 0 and 'already synced' in second.stdout, second.stdout + second.stderr


@pytest.mark.parametrize(
    'ccfg, extra',
    [
        (bytes(0x24) + bytes(8) + uniflash.bootloader_ccfg(STOCK_CCFG.read_bytes(), 10)[0x2C:], []),
        (bytes(0x24) + b'00000000' + uniflash.bootloader_ccfg(STOCK_CCFG.read_bytes(), 10)[0x2C:], []),
        (None, ['--mac', '777A0000']),
        (STOCK_CCFG.read_bytes(), []),  # the bootloader disabled (stock): not written either
        (
            uniflash.bootloader_ccfg(STOCK_CCFG.read_bytes(), 10)[:0x30]
            + bytes([0xC5, 32, 0xFE, 0xC5])
            + uniflash.bootloader_ccfg(STOCK_CCFG.read_bytes(), 10)[0x34:],
            [],
        ),
        (
            uniflash.bootloader_ccfg(STOCK_CCFG.read_bytes(), 10)[:0x30]
            + bytes([0xC5, 10, 0xFF, 0xC5])
            + uniflash.bootloader_ccfg(STOCK_CCFG.read_bytes(), 10)[0x34:],
            [],
        ),
    ],
    ids=['no MAC', 'all-zero MAC', 'another unit', 'bootloader off', 'bad BL pin', 'active-high BL'],
)
def test_nothing_written_when_the_ccfg_is_wrong(rom, tmp_path, ccfg, extra):  # noqa: F811
    r, port = rom(chip(ccfg))
    boot, slot = images(tmp_path)
    p = run(port, 'flash', boot, slot, '--yes', *extra)
    assert p.returncode != 0 and r.writes == [], p.stdout


def test_uart_sbl_check_page31_rejects_zero_mac_and_bad_bl_config():
    valid_ccfg = bytearray(uniflash.bootloader_ccfg(STOCK_CCFG.read_bytes(), 10))
    valid_ccfg[0x24:0x2C] = b'777B42DF'
    pad = b'\xff' * (uniflash.CCFG_ADDR - uniflash.PAGE31)
    assert uart_sbl.check_page31(pad + bytes(valid_ccfg), '777B42DF') == ('777B42DF', 10)

    zero_mac = bytearray(valid_ccfg)
    zero_mac[0x24:0x2C] = b'00000000'
    with pytest.raises(SystemExit):
        uart_sbl.check_page31(pad + bytes(zero_mac), None)

    for bad_bl in (bytes([0xC5, 32, 0xFE, 0xC5]), bytes([0xC5, 10, 0xFF, 0xC5])):
        bad = bytearray(valid_ccfg)
        bad[uniflash.BL_CONFIG : uniflash.BL_CONFIG + 4] = bad_bl
        with pytest.raises(AssertionError):
            uart_sbl.check_page31(pad + bytes(bad), None)


def test_not_a_cc1310_image(rom, tmp_path):  # noqa: F811
    # a file picked by mistake (here the start of an ESP8266 image): refused, nothing erased or written
    flash = chip()
    before = bytes(flash)
    r, port = rom(flash)
    (tmp_path / 'esp.bin').write_bytes(b'\xe9\x01\x02\x20' + bytes(range(256)) * 8)
    p = run(port, 'flash', tmp_path / 'esp.bin', '--mac', '777B42DF', '--yes')
    assert p.returncode != 0 and 'not a CC1310 image' in p.stdout
    assert bytes(flash) == before and not r.writes


def test_no_bootloader(rom, tmp_path):  # noqa: F811
    r, port = rom(chip(), silent=True)
    boot, slot = images(tmp_path)
    p = run(port, 'flash', boot, slot, '--yes')
    assert p.returncode != 0 and 'no bootloader' in p.stdout


def test_contact_image_tool(tmp_path):  # the file POST /contact/flash takes: selector + slot A, as uniflash.slots
    boot, slot = images(tmp_path)
    out = tmp_path / 'contact.bin'
    subprocess.run(
        ['uv', 'run', 'python', str(ROOT / 'tools' / 'contact_image.py'), boot, slot, out],
        check=True,
        capture_output=True,
    )
    assert out.read_bytes() == uniflash.slots_image(boot, slot)

# tools/release_bundle.py: the relay's release bundle from CI's build outputs, with fake inputs.
import hashlib
import json
import pathlib
import sys
import zipfile

import pytest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import release_bundle  # noqa: E402


def fake_build(tmp, version):
    esp = tmp / 'idf'
    for d, *_ in release_bundle.ESP32:
        (esp / d).mkdir(parents=True)
        (esp / d / 'ows_relay.bin').write_bytes(b'relay OWS-RELAY-VERSION:' + version.encode())
        (esp / d / 'ows_relay_full.bin').write_bytes(b'boot+table+relay OWS-RELAY-VERSION:' + version.encode())
    e8266 = tmp / 'firmware.bin'
    e8266.write_bytes(b'esp8266 OWS-RELAY-VERSION:' + version.encode())
    return esp, e8266


def test_bundle_has_the_products_w2m_updates_relays_from(tmp_path):
    esp, e8266 = fake_build(tmp_path, 'v0.3.0')
    out = tmp_path / 'dist'
    names = release_bundle.bundle('v0.3.0', esp, e8266, out)
    assert len(names) == 2 * len(release_bundle.ESP32) + 1 + len(release_bundle.UDEV) + len(release_bundle.SCRIPTS)
    with zipfile.ZipFile(out / 'wyzesenserelay-v0.3.0.zip') as zf:
        assert set(names + ['SHA256SUMS', 'manifest.json', 'README.md']) == set(zf.namelist())
    manifest = json.loads((out / 'manifest.json').read_text())
    assert manifest['format'] == 'openwyzesense-release' and manifest['version'] == 'v0.3.0'
    products = {f['product'] for f in manifest['files']}
    # wyzesense2mqtt-rs (ows branch, src/firmware/relay.rs) looks these up by the relay's chip
    assert {'relay-esp32-ota', 'relay-esp32s3-ota', 'relay-esp8266'} <= products
    sums = dict(reversed(line.split('  ')) for line in (out / 'SHA256SUMS').read_text().splitlines())
    assert set(sums) == set(names)
    assert {f['file']: f['sha256'] for f in manifest['files']} == sums
    for n, h in sums.items():
        assert hashlib.sha256((out / n).read_bytes()).hexdigest() == h


def test_a_build_of_another_version_is_refused(tmp_path):
    esp, e8266 = fake_build(tmp_path, 'v0.3.0')
    (esp / 'build' / 'ows_relay.bin').write_bytes(b'relay OWS-RELAY-VERSION:unknown')
    with pytest.raises(SystemExit) as e:
        release_bundle.bundle('v0.3.0', esp, e8266, tmp_path / 'dist')
    assert 'does not carry OWS-RELAY-VERSION:v0.3.0' in str(e.value)


def test_an_image_with_stock_firmware_is_never_published(tmp_path):
    # a local build embeds the user's own stock dump (firmware/gateway-stock-v1.8-app.bin): proprietary
    esp, e8266 = fake_build(tmp_path, 'v0.3.0')
    e8266.write_bytes(b'esp8266 OWS-RELAY-VERSION:v0.3.0 ... START111\r\n ...')
    with pytest.raises(SystemExit) as e:
        release_bundle.bundle('v0.3.0', esp, e8266, tmp_path / 'dist')
    assert 'embeds stock Wyze firmware' in str(e.value)


def test_the_local_builds_are_refused_when_they_carry_a_stock_dump(tmp_path):
    # the real images built on this machine, if any: a bundle from them must fail while the dump is embedded
    e8266 = ROOT / 'esp8266' / '.pio' / 'build' / 'd1_mini' / 'firmware.bin'
    if not e8266.exists() or release_bundle.STOCK_MARKER not in e8266.read_bytes():
        pytest.skip('no local build with a stock dump')
    esp, _ = fake_build(tmp_path, 'v0.3.0')
    with pytest.raises(SystemExit) as e:
        release_bundle.bundle('v0.3.0', esp, e8266, tmp_path / 'dist')
    assert 'embeds stock Wyze firmware' in str(e.value)

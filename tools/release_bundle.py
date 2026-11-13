#!/usr/bin/env python3
"""The relay's release bundle: CI's ESP32 / ESP32-S3 / ESP8266 images, renamed per board, with SHA256SUMS, a README
saying which file goes where, and manifest.json in openwyzesense's format (each file's product, size and SHA-256), which
wyzesense2mqtt-rs reads to update relays (products relay-esp32-ota, relay-esp32s3-ota, relay-esp8266). Every CI run
assembles it as an artifact; a v* tag publishes it as a GitHub Release.

    uv run tools/release_bundle.py --version v0.3.0 --esp32 idf \
        --esp8266 esp8266/.pio/build/d1_mini/firmware.bin --out dist

--esp32 is the directory holding build/ (ESP32-S3) and build-esp32/ (classic ESP32), each with ows_relay.bin and the
merged first-flash image ows_relay_full.bin. Every image must carry OWS-RELAY-VERSION:<version>: a bundle never mixes
builds. An image that embeds stock Wyze firmware (a local build from a dump) is refused: those are never published.
"""

import argparse
import hashlib
import json
import pathlib
import shutil
import sys
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
GUIDE = 'https://github.com/HclX/wyzesenserelay'
MANIFEST_FORMAT = 'openwyzesense-release'  # the same format wyzesense2mqtt-rs reads from openwyzesense's releases
RELAY_TAG = b'OWS-RELAY-VERSION:'  # the relay's own version in its image (idf/main/web.c, esp8266 main.cpp)
STOCK_MARKER = b'START111'  # the stock Wyze gateway firmware's start-up line: only in a build that embeds a dump
ESP32 = [  # (build directory under --esp32, chip name, label)
    ('build', 'esp32s3', 'ESP32-S3'),
    ('build-esp32', 'esp32', 'classic ESP32 (LOLIN32)'),
]
UDEV = [('72-esp-usb-serial.rules', 'ESP32 / ESP8266 USB serial')]
SCRIPTS = [
    ('flash.sh', 'Linux / macOS', 'first-flash helper (auto-detects chip & image)', '`./flash.sh [PORT]`'),
    ('flash.bat', 'Windows', 'first-flash helper (auto-detects chip & image)', '`flash.bat [COM_PORT]`'),
]


def bundle(version, esp32, esp8266, out):
    out = pathlib.Path(out)
    out.mkdir(parents=True, exist_ok=True)
    rows, errors, files = [], [], []

    def add(src, name, device, what, how, product, versioned=True):
        src = pathlib.Path(src)
        if not src.is_file():
            errors.append(f'missing: {src}')
            return
        data = src.read_bytes()
        want = RELAY_TAG + version.encode()
        if versioned and want not in data:
            errors.append(f'{src}: does not carry {want.decode()} (a different build?)')
        if STOCK_MARKER in data:
            errors.append(f'{src}: embeds stock Wyze firmware (a local build): never published')
        shutil.copyfile(src, out / name)
        rows.append((name, device, what, how))
        files.append(
            {
                'file': name,
                'device': device,
                'what': what,
                'board': None,
                'product': product,
                'size': len(data),
                'sha256': hashlib.sha256(data).hexdigest(),
            }
        )

    for d, chip, label in ESP32:
        b = pathlib.Path(esp32) / d
        add(
            b / 'ows_relay_full.bin',
            f'ows-relay-{chip}-full-{version}.bin',
            label,
            'relay: first flash',
            f'`esptool.py --chip {chip} write_flash 0x0 FILE` (also clears its settings: Wi-Fi set up again via the '
            'OWS-relay-XXXX access point)',
            f'relay-{chip}-full',
        )
        add(
            b / 'ows_relay.bin',
            f'ows-relay-{chip}-ota-{version}.bin',
            label,
            'relay: update',
            'the relay page: Update, or `curl --data-binary @FILE http://<relay>/ota`',
            f'relay-{chip}-ota',
        )
    add(
        esp8266,
        f'ows-relay-esp8266-{version}.bin',
        'ESP8266 (D1 mini, NodeMCU)',
        'relay: first flash and update',
        'first: `esptool.py --chip esp8266 write_flash 0x0 FILE`; then the relay page: Update',
        'relay-esp8266',
    )
    for rule, device in UDEV:
        add(
            ROOT / 'tools' / 'udev' / rule,
            rule,
            device,
            'Linux udev rule: access without root',
            f'`sudo cp {rule} /etc/udev/rules.d/ && sudo udevadm control --reload && sudo udevadm trigger`',
            'udev',
            versioned=False,
        )
    for script, device, what, how in SCRIPTS:
        add(
            ROOT / 'tools' / script,
            script,
            device,
            what,
            how,
            'flash-script',
            versioned=False,
        )
        if script.endswith('.sh') and (out / script).is_file():
            (out / script).chmod(0o755)
    if errors:
        raise SystemExit('release bundle not made:\n  ' + '\n  '.join(errors))

    sums = ''.join(f'{hashlib.sha256((out / n).read_bytes()).hexdigest()}  {n}\n' for n, *_ in rows)
    (out / 'SHA256SUMS').write_text(sums)
    manifest = {'format': MANIFEST_FORMAT, 'version': version, 'files': files}
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    lines = [
        f'# WyzeSense relay {version}',
        '',
        'The ESP32 / ESP32-S3 / ESP8266 IP relay for a Wyze v1 contact board as a Wi-Fi gateway. Step by step:',
        f'the user guide, {GUIDE}/tree/{version}#user-guide. Check the files with `sha256sum -c SHA256SUMS`;',
        "manifest.json lists the same with each file's product, for programs (wyzesense2mqtt-rs).",
        '',
        'The relay images carry the OpenWyzeSense gateway image for the contact board. To run Stock Wyze v1.8',
        'instead, upload your dongle dump in the Web UI under Update -> Stock gateway firmware.',
        '',
        '| File | Device | What | How to install |',
        '|---|---|---|---|',
        *(f'| `{n}` | {dev} | {what} | {how} |' for n, dev, what, how in rows),
        '',
    ]
    (out / 'README.md').write_text('\n'.join(lines))
    zip_name = f'wyzesenserelay-{version}.zip'
    bundle_files = [n for n, *_ in rows] + ['SHA256SUMS', 'manifest.json', 'README.md']
    with zipfile.ZipFile(out / zip_name, 'w', compression=zipfile.ZIP_DEFLATED) as zf:
        for name in bundle_files:
            zf.write(out / name, arcname=name)
    return [r[0] for r in rows]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--version', required=True)
    ap.add_argument(
        '--esp32', required=True, help='idf, holding build/ and build-esp32/ (CI: the esp32-relay artifact)'
    )
    ap.add_argument('--esp8266', required=True, help='the ESP8266 firmware.bin (CI: the esp8266-relay artifact)')
    ap.add_argument('--out', required=True)
    a = ap.parse_args(argv)
    names = bundle(a.version, a.esp32, a.esp8266, a.out)
    print(f'{len(names)} files in {a.out}', file=sys.stderr)


if __name__ == '__main__':
    main()

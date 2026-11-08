# The build info block and the image ID (common/build_info.h, tools/image_id.py, #76).
# The same code gives the same ID regardless of version string; changing code or flags changes the ID.
import pathlib
import random
import struct
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import image_id  # noqa: E402


def make_image(version: str, flags: int = 1, seed: int = 42) -> bytes:
    rnd = random.Random(seed)
    b = bytearray(struct.pack('<II', 0x20005000, 0x335)) + bytes(56)
    b += struct.pack(
        '<4sBBH8s24s24s12s40s',
        b'OWSB',
        1,
        flags,
        0,
        bytes(8),
        b'gateway-contact-v1',
        b'Wyze Sense v1 contact',
        b'gateway',
        version.encode(),
    )
    b += bytes(rnd.randrange(256) for _ in range(4096 - len(b)))
    b[64 + 8 : 64 + 16] = image_id.image_id(bytes(b))
    return bytes(b)


def test_same_code_same_id():
    img_a = make_image('v1.0-test-a')
    img_b = make_image('v2.7-123-gdeadbeef-dirty')
    a, b = image_id.parse(img_a), image_id.parse(img_b)
    assert (a['version'], b['version']) == ('v1.0-test-a', 'v2.7-123-gdeadbeef-dirty')
    assert a['id'] == b['id'] == image_id.image_id(img_a).hex()
    assert len(img_a) == len(img_b)
    at = a['offset'] + image_id.VERSION_AT
    differ = [i for i, (x, y) in enumerate(zip(img_a, img_b, strict=True)) if x != y]
    assert differ and all(at <= i < at + image_id.VERSION_LEN for i in differ)


def test_the_block():
    real = ROOT / 'firmware' / 'gateway-contact-v1-app.bin'
    a = image_id.parse(real.read_bytes())
    assert a['offset'] < image_id.SEARCH and a['format'] == 1
    assert (a['product'], a['app'], a['board']) == ('gateway-contact-v1', 'gateway', 'Wyze Sense v1 contact')
    assert a['tx'] and not a['debug']
    assert a['id'] == image_id.image_id(real.read_bytes()).hex()


def test_another_image_another_id():
    img_a = make_image('v1.0-test-a', flags=1)
    img_dbg = make_image('v1.0-test-a', flags=3)
    d = image_id.parse(img_dbg)
    assert d['debug'] and d['id'] != image_id.parse(img_a)['id']

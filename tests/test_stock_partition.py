"""Tests for tools/stock_partition.py and byte-for-byte equivalence with the C packer (contact/stock_image.c)."""

from __future__ import annotations

import hashlib
import os
import pathlib
import subprocess
import sys

import pytest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import stock_partition  # noqa: E402

HOST_BUILD = pathlib.Path(os.environ.get('OWS_HOST_BUILD', ROOT / 'build' / 'host'))
TEST_STOCK_BIN = HOST_BUILD / 'test_stock_image'


def _synthetic_dump(nv_fill: int = 0xA5, ccfg_fill: int = 0x5A) -> tuple[bytes, bytes, str]:
    """Return (dump_128k, packed_image_0x1efa8, sha256_hex) with pages 24-29 = 0x00."""
    pages_0_23 = bytes(((i * 31 + 17) & 0xFF) for i in range(24 * 4096))
    pages_24_29 = bytes(6 * 4096)  # 24 KB of 0x00 on every stock v1.8 dongle
    page_30_nv = bytes([nv_fill]) * 4096
    page_31_code = bytes(((i * 13 + 9) & 0xFF) for i in range(stock_partition.STOCK_PAGE31_CODE_LEN))
    ccfg = bytes([ccfg_fill]) * 88
    dump = pages_0_23 + pages_24_29 + page_30_nv + page_31_code + ccfg
    assert len(dump) == stock_partition.STOCK_DUMP_LEN == 0x20000
    packed = pages_0_23 + pages_24_29 + page_31_code
    assert len(packed) == stock_partition.STOCK_IMAGE_LEN == 0x1EFA8
    return dump, packed, hashlib.sha256(packed).hexdigest()


def test_pack_stock_image_includes_pages_24_to_29_and_skips_nv_and_ccfg() -> None:
    dump_a, packed_a, sha_a = _synthetic_dump(nv_fill=0x11, ccfg_fill=0x22)
    dump_b, packed_b, sha_b = _synthetic_dump(nv_fill=0xEE, ccfg_fill=0xFF)
    assert packed_a == packed_b and sha_a == sha_b
    assert stock_partition.pack_stock_image(dump_a) == packed_a
    assert stock_partition.pack_stock_image(packed_a) == packed_a
    assert packed_a[0x18000:0x1E000] == bytes(0x6000)

    part_from_dump = stock_partition.build_stock_partition(dump_a, expected_sha256=sha_a)
    part_from_packed = stock_partition.build_stock_partition(packed_a, expected_sha256=sha_a)
    assert part_from_dump == part_from_packed
    assert len(part_from_dump) == stock_partition.STOCK_PART_LEN == 0x1EFD8
    magic, fmt, img_len, digest = stock_partition.STOCK_HDR_STRUCT.unpack(
        part_from_dump[: stock_partition.STOCK_HDR_LEN]
    )
    assert magic == b'OWSSTOCK'
    assert fmt == 1
    assert img_len == 0x1EFA8
    assert digest.hex() == sha_a
    assert part_from_dump[stock_partition.STOCK_HDR_LEN :] == packed_a


def test_stock_partition_rejects_bad_size_or_wrong_hash(tmp_path: pathlib.Path) -> None:
    dump, packed, _ = _synthetic_dump()
    # Wrong size (old 24-page 0x18FA8 layout) must be rejected
    with pytest.raises(ValueError, match='expected a 131072-byte'):
        stock_partition.pack_stock_image(packed[:0x18000] + packed[0x1E000:])
    # Synthetic dump does not match the real Wyze v1.8 SHA-256 constant
    with pytest.raises(ValueError, match='not the Wyze v1.8 stock gateway image'):
        stock_partition.build_stock_partition(dump)

    bad_in = tmp_path / 'bad.bin'
    out = tmp_path / 'out.bin'
    bad_in.write_bytes(dump)
    assert stock_partition.main([str(bad_in), str(out)]) == 1
    assert not out.exists()


@pytest.mark.skipif(not TEST_STOCK_BIN.exists(), reason='build the host targets first: cmake --build --preset host')
def test_python_tool_and_c_packer_produce_byte_identical_partition(tmp_path: pathlib.Path) -> None:
    dump, packed, sha = _synthetic_dump()
    dump_file = tmp_path / 'dump.bin'
    packed_file = tmp_path / 'packed.bin'
    py_out = tmp_path / 'py_part.bin'
    c_out_dump = tmp_path / 'c_part_dump.bin'
    c_out_packed = tmp_path / 'c_part_packed.bin'
    dump_file.write_bytes(dump)
    packed_file.write_bytes(packed)

    assert stock_partition.main(['--sha256', sha, str(dump_file), str(py_out)]) == 0
    res_dump = subprocess.run(
        [str(TEST_STOCK_BIN), '--pack', str(dump_file), str(c_out_dump), sha],
        capture_output=True,
        text=True,
        check=False,
    )
    assert res_dump.returncode == 0, res_dump.stderr
    res_packed = subprocess.run(
        [str(TEST_STOCK_BIN), '--pack', str(packed_file), str(c_out_packed), sha],
        capture_output=True,
        text=True,
        check=False,
    )
    assert res_packed.returncode == 0, res_packed.stderr
    assert py_out.read_bytes() == c_out_dump.read_bytes() == c_out_packed.read_bytes()

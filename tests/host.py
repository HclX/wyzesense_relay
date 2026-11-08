# Host <-> dongle protocol (FACTS.md §9). The dongle's CC1310 speaks it on UART0 (115200 8N1); in the stock
# dongle a CH554 bridges that UART to USB HID (each HID read = 1 length byte + up to 63 protocol bytes).
#
#   packet: magic(2) type(1) b2(1) cmd(1) payload(b2 - 3) checksum(2, BE, 16-bit sum of all preceding bytes)
#   magic : AA 55 host -> dongle, 55 AA dongle -> host
#   type  : 0x43 sync (host-initiated request/reply), 0x53 async
#   ACK   : cmd 0xFF, b2 = low byte of the acknowledged command, no payload (7 bytes)
# Replies use cmd + 1. Every async packet except an ACK is ACKed by the receiver.

import struct

MAGIC_TO_DONGLE, MAGIC_TO_HOST = b'\xaa\x55', b'\x55\xaa'
SYNC, ASYNC = 0x43, 0x53

INQUIRY, GET_ENR, GET_MAC, GET_KEY = 0x4327, 0x4302, 0x4304, 0x4306
FINISH_AUTH, GET_VERSION, SET_SCAN = 0x5314, 0x5316, 0x531C
ALARM, SCAN_RESULT, GET_R1, VERIFY_SENSOR, DELETE_SENSOR = 0x5319, 0x5320, 0x5321, 0x5323, 0x5325
GET_SENSOR_COUNT, GET_SENSOR_LIST, SENSOR_LIST_ITEM = 0x532E, 0x5330, 0x5331
TIME_SYNC_REQ, TIME_SYNC, EVENT_LOG, DELETE_ALL, ALARM2, PLAY_CHIME = 0x5332, 0x5333, 0x5335, 0x533F, 0x5355, 0x5370
ACK = 0x53FF
# OpenWyzeSense host extensions: multiplexed under 0x53F0 (host -> gw) / 0x53F1 (gw -> host) + OWS_SUB_* (<= 56 B)
OWS_CMD, OWS_RSP = 0x53F0, 0x53F1
OWS_SUB_FLAGS, OWS_SUB_BUILD_INFO, OWS_SUB_CCFG = 0x01, 0x02, 0x03
OWS_SUB_NETWORK, OWS_SUB_ENTER_SBL, OWS_SUB_NV_IMPORT = 0x04, 0x05, 0x06
OWS_NV_CLEAR, OWS_NV_SET_NET, OWS_NV_ADD_SENSOR, OWS_NV_GET_SENSOR = 0x01, 0x02, 0x03, 0x04
OWS_SUB_OTA_OFFER, OWS_SUB_OTA_NEED, OWS_SUB_OTA_DATA, OWS_SUB_OTA_DONE = 0x10, 0x11, 0x12, 0x13
OWS_SUB_OVERHEARD, OWS_SUB_ADDR_RANGE = 0x20, 0x23  # roaming (GW_FLAG_ROAM, docs/design/roaming.md)
OWS_BUILD_INFO_P0_LEN, OWS_BUILD_INFO_P1_LEN, OWS_BUILD_INFO_P2_LEN = 53, 55, 27
FLAG_DEDUP, FLAG_OTA, FLAG_ANNOUNCE, FLAG_ROAM = 1 << 0, 1 << 2, 1 << 3, 1 << 4  # 1 << 1: reserved (#31)
OTA_DONE_STATUS = {0: 'ok', 1: 'bad CRC', 2: 'refused', 3: 'gave up (no blocks)', 4: 'flash failed / battery low'}
CCFG_MAGIC = b'OWS-CCFG-RESTORE'
CCFG_STATUS = {0x00: 'ok', 0x01: 'already right', 0xE0: 'refused', 0xE1: 'built-in CCFG not safe', 0xE2: 'failed'}
# Legacy pre-consolidation opcodes retained for ground_truth/host/2026-10-24_ota_launchpad_over_spare_dongle replay
OWS_FLAGS = 0x53F0
OWS_CCFG = 0x53F2
OWS_OTA_OFFER, OWS_OTA_NEED, OWS_OTA_DATA, OWS_OTA_DONE = 0x53F4, 0x53F6, 0x53F7, 0x53F8


def build(cmd, payload=b'', to_host=False):
    typ, c = cmd >> 8, cmd & 0xFF
    b = (MAGIC_TO_HOST if to_host else MAGIC_TO_DONGLE) + bytes([typ, len(payload) + 3, c]) + payload
    return b + (sum(b) & 0xFFFF).to_bytes(2, 'big')


def build_ack(cmd, to_host=False):
    b = (MAGIC_TO_HOST if to_host else MAGIC_TO_DONGLE) + bytes([cmd >> 8, cmd & 0xFF, 0xFF])
    return b + (sum(b) & 0xFFFF).to_bytes(2, 'big')


def parse(p):
    """One complete packet -> dict(cmd, payload, to_host[, acked]); None if malformed."""
    if (
        len(p) < 7
        or p[:2] not in (MAGIC_TO_DONGLE, MAGIC_TO_HOST)
        or p[2] not in (SYNC, ASYNC)
        or sum(p[:-2]) & 0xFFFF != int.from_bytes(p[-2:], 'big')
    ):
        return None
    m = {'cmd': p[2] << 8 | p[4], 'to_host': p[:2] == MAGIC_TO_HOST}
    if p[4] == 0xFF:
        m['acked'] = p[2] << 8 | p[3]
        m['payload'] = b''
    else:
        if len(p) != p[3] + 4:
            return None
        m['payload'] = p[5:-2]
    return m


def scan(buf):
    """Byte stream (possibly with garbage between packets) -> list of complete, checksum-valid packets."""
    out, i = [], 0
    while i + 7 <= len(buf):
        if buf[i : i + 2] in (MAGIC_TO_DONGLE, MAGIC_TO_HOST):
            n = 7 if buf[i + 4] == 0xFF else buf[i + 3] + 4
            if parse(buf[i : i + n]):
                out.append(buf[i : i + n])
                i += n
                continue
        i += 1
    return out


def hid_unwrap(report):
    """One HID read from the stock dongle -> its protocol bytes (byte 0 = valid length, rest is stale)."""
    return report[1 : 1 + min(report[0], 0x3F)]


# --- RF plaintext -> host notification (what the stock dongle forwards) ---


def alarm_payload(pt, time_ms, rssi):
    """0x5319 payload for a decrypted sensor message: dongle clock (8 bytes, Unix ms BE), then the RF plaintext
    up to its counter (A1/A2/E8: 16 + N bytes; AB: 23 bytes), then the received signal strength (-dBm)."""
    n = 23 if pt[0] == 0xAB else 16 + pt[13]  # 13: the data length (12: the low-battery flag, FACTS.md)
    return time_ms.to_bytes(8, 'big') + pt[:n] + bytes([rssi])


def parse_alarm(payload):
    """0x5319 payload -> dict(time_ms, plaintext, rssi)."""
    return {'time_ms': int.from_bytes(payload[:8], 'big'), 'plaintext': payload[8:-1], 'rssi': payload[-1]}


def parse_build_info(p0, p1, p2=None):
    """OWS_SUB_BUILD_INFO pages 0, 1[, 2] (with or without leading 0x02 sub-ID) -> dict, or None."""
    if p0 and len(p0) == OWS_BUILD_INFO_P0_LEN and p0[0] == OWS_SUB_BUILD_INFO:
        p0 = p0[1:]
    if p1 and len(p1) == OWS_BUILD_INFO_P1_LEN and p1[0] == OWS_SUB_BUILD_INFO:
        p1 = p1[1:]
    if p2 and len(p2) == OWS_BUILD_INFO_P2_LEN and p2[0] == OWS_SUB_BUILD_INFO:
        p2 = p2[1:]
    if (
        not p0
        or len(p0) != OWS_BUILD_INFO_P0_LEN - 1
        or not p1
        or len(p1) != OWS_BUILD_INFO_P1_LEN - 1
        or p0[8:16] != p1[:8]
    ):
        return None
    magic, fmt, flags, _ = struct.unpack_from('<IBBH', p0, 0)
    if magic != 0x4253574F or fmt != 1:
        return None
    ver = p1[32:54]
    if p2 is not None and len(p2) == OWS_BUILD_INFO_P2_LEN - 1:
        if p2[:8] != p0[8:16]:
            return None
        ver += p2[8:26]
    return {
        'flags': flags,
        'id': p0[8:16].hex(),
        'product': p0[16:40].split(b'\x00', 1)[0].decode(errors='replace'),
        'app': p0[40:52].split(b'\x00', 1)[0].decode(errors='replace'),
        'board': p1[8:32].split(b'\x00', 1)[0].decode(errors='replace'),
        'version': ver.split(b'\x00', 1)[0].decode(errors='replace'),
    }

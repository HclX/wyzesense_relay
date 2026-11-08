# Host <-> dongle protocol packet framing for relay / gwsync tests.
MAGIC_TO_DONGLE, MAGIC_TO_HOST = b'\xaa\x55', b'\x55\xaa'
SYNC, ASYNC = 0x43, 0x53
OWS_CMD, OWS_RSP = 0x53F0, 0x53F1
OWS_SUB_FLAGS, OWS_SUB_BUILD_INFO, OWS_SUB_CCFG = 0x01, 0x02, 0x03
OWS_SUB_NETWORK, OWS_SUB_ENTER_SBL, OWS_SUB_NV_IMPORT = 0x04, 0x05, 0x06


def build(cmd, payload=b'', to_host=False):
    typ, c = cmd >> 8, cmd & 0xFF
    b = (MAGIC_TO_HOST if to_host else MAGIC_TO_DONGLE) + bytes([typ, len(payload) + 3, c]) + payload
    return b + (sum(b) & 0xFFFF).to_bytes(2, 'big')


def build_ack(cmd, to_host=False):
    b = (MAGIC_TO_HOST if to_host else MAGIC_TO_DONGLE) + bytes([cmd >> 8, cmd & 0xFF, 0xFF])
    return b + (sum(b) & 0xFFFF).to_bytes(2, 'big')


def parse(p):
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

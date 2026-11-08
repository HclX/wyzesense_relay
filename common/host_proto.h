// Host <-> dongle protocol (docs/FACTS.md §9). Port of reference/wyzesense_ref/host.py.
//   packet: magic(2) type(1) b2(1) cmd(1) payload(b2 - 3) checksum(2, BE 16-bit sum of all preceding bytes)
//   magic aa 55 host -> dongle, 55 aa dongle -> host; ACK: cmd 0xFF, b2 = low byte of the acked command
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HOST_MAX_PACKET 260  // b2 is one byte: payload <= 252

enum {
    HOST_SYNC = 0x43,
    HOST_ASYNC = 0x53,
    HOST_UPDATE_MODE =
        0x4312,  // the dongle's field update (WyzeSenseUpdater): its CH554 resets the CC1310 into the ROM
                 // bootloader; the stock CC1310 answers 0x4313, empty (FACTS.md §9)
    HOST_INQUIRY = 0x4327,
    HOST_GET_ENR = 0x4302,
    HOST_GET_MAC = 0x4304,
    HOST_GET_KEY = 0x4306,
    HOST_FINISH_AUTH = 0x5314,
    HOST_GET_VERSION = 0x5316,
    HOST_ALARM = 0x5319,
    HOST_ALARM2 = 0x5355,       // secondary leak/keypad report (RF opcode 0xEA), forwarded verbatim + RSSI
    HOST_SEND_KEYPAD = 0x5353,  // MAC(8) + 0xE9 + n + data[n]: queue an RF keypad reply
    HOST_SET_SCAN = 0x531C,
    HOST_SCAN_RESULT = 0x5320,
    HOST_GET_R1 = 0x5321,
    HOST_VERIFY_SENSOR = 0x5323,
    HOST_DELETE_SENSOR = 0x5325,
    HOST_GET_SENSOR_COUNT = 0x532E,
    HOST_GET_SENSOR_LIST = 0x5330,
    HOST_SENSOR_LIST_ITEM = 0x5331,
    HOST_TIME_SYNC_REQ = 0x5332,
    HOST_TIME_SYNC = 0x5333,
    HOST_DELETE_ALL = 0x533F,
    HOST_PLAY_CHIME = 0x5370,
    HOST_ACK = 0x53FF,
    // OpenWyzeSense extensions (docs/design/DESIGN.md §5.2.2): multiplexed under 0x53F0 (host -> gw) / 0x53F1 (gw -> host)
    // with payload[0] = OWS_SUB_*, every packet <= 56 B payload (<= 63 B frame) for the dongle's CH554 USB-HID.
    HOST_OWS_CMD = 0x53F0,
    HOST_OWS_RSP = 0x53F1,
    HOST_OWS_FLAGS = 0x53F0,              // legacy 0- or 4-byte flags packet (payload[0] == 0)
    HOST_OWS_BUILD_INFO_LEGACY = 0x53FA,  // temporary read-only alias (-> 0x53FB) for rolling ESP updates
};

enum {
    OWS_SUB_FLAGS = 0x01,       // [] or [flags_be32(4)] -> [0x01, active_be32(4), supported_be32(4)]
    OWS_SUB_BUILD_INFO = 0x02,  // [part] -> [0x02, ...] (part 0: 53 B; part 1: 55 B; part 2: 27 B; <= 56 B CH554-safe)
    OWS_SUB_CCFG = 0x03,        // [GW_CCFG_MAGIC(16), step(1), ieee_be64(8)[, nonce(4)]] -> [0x03, step, st[, nonce]]
    OWS_SUB_NETWORK = 0x04,     // [] or [pan_le16, short_le16, ext_le64] -> [0x04, status, pan, short, ext]
    OWS_SUB_ENTER_SBL = 0x05,   // ["OSBL"] -> [0x05, status] (direct-UART gateways only; 0xE0 on dongle)
    OWS_SUB_NV_IMPORT = 0x06,   // [OWS_NV_*, ...] -> [0x06, OWS_NV_*, status, ...]
    OWS_SUB_OTA_OFFER = 0x10,   // [mac(8), offer(24)] -> [0x10, status]
    OWS_SUB_OTA_NEED = 0x11,    // gw -> host: [0x11, mac(8), slot(1), off_be32(4), len(1)] (15 B)
    OWS_SUB_OTA_DATA = 0x12,    // host -> gw: [0x12, off_be32(4), data(<=32)] (<= 37 B, ACK only)
    OWS_SUB_OTA_DONE = 0x13,    // gw -> host: [0x13, mac(8), status(1), version_be32(4)] (14 B)
    OWS_SUB_OVERHEARD = 0x20,   // gw -> host (GW_FLAG_ROAM): [0x20, short_le16, <0x5319 payload>], not resent
    OWS_SUB_ADDR_RANGE = 0x23,  // [] or [first_le16, last_le16] -> [0x23, status, first_le16, last_le16]
};

enum {
    OWS_NV_CLEAR = 0x01,       // [0x01] -> [0x06, 0x01, status]
    OWS_NV_SET_NET = 0x02,     // [0x02, pan_le16(2), short_le16(2), ext_le64(8)] -> [0x06, 0x02, status]
    OWS_NV_ADD_SENSOR = 0x03,  // [0x03, mac(8), short_le16(2), type(1)[, sub_le16(2)]] -> [0x06, 0x03, status, count]
    OWS_NV_GET_SENSOR = 0x04,  // [0x04, index(1)] -> [0x06, 0x04, status, count, index, sub_le16, mac(8), short, type]
};

#define OWS_MAX_PAYLOAD       56  // CH554 single-report USB-HID limit: 63 protocol bytes - 7 framing bytes
#define OWS_BUILD_INFO_P0_LEN 53  // [0x02, header(8), id(8), product(24), app(12)]
#define OWS_BUILD_INFO_P1_LEN 55  // [0x02, id(8), board(24), version[0..21](22)]
#define OWS_BUILD_INFO_P2_LEN 27  // [0x02, id(8), version[22..39](18)]

typedef struct {
    uint16_t cmd;
    bool to_host;
    uint16_t acked;  // for HOST_ACK: the acknowledged command
    const uint8_t *payload;
    size_t len;
} host_pkt_t;

size_t host_build(uint8_t *out, uint16_t cmd, const uint8_t *payload, size_t n, bool to_host);
size_t host_build_ack(uint8_t *out, uint16_t cmd, bool to_host);

// One complete packet -> fields; false if malformed (magic, length or checksum).
bool host_parse(const uint8_t *p, size_t n, host_pkt_t *out);

// Byte-stream reassembly: feed bytes; returns true when *out holds a complete packet (valid until the next call).
typedef struct {
    uint8_t buf[HOST_MAX_PACKET];
    size_t n;
} host_rx_t;

void host_rx_init(host_rx_t *rx);
bool host_rx_feed(host_rx_t *rx, uint8_t byte, host_pkt_t *out);

// 0x5319 alarm payload for a decrypted sensor message: dongle clock (Unix ms, 8 bytes BE), the plaintext up to
// its counter (A1/A2/E8: 16 + N bytes; AB: 23 bytes), signal strength (-dBm). Returns its length, 0 if pt is short.
size_t host_alarm_payload(uint8_t *out, const uint8_t *pt, size_t pt_len, uint64_t time_ms, uint8_t rssi);

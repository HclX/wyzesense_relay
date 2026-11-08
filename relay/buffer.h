// Keeping the gateway's reports through an outage (issue #22): a layer around the relay that understands the host
// protocol (firmware/common/host_proto.c, the gateway's own code). While the connection to wyzesense2mqtt-rs is down,
// each alarm (0x5319) from the gateway is ACKed here, in the host's place (so the gateway does not give up on it
// after its 4 sends), and queued; once the connection is up again and the server has had time for its handshake
// with the gateway, the queue is sent on, one alarm at a time, and the server's ACKs for them are kept from the
// gateway (it had its ACK already). The alarms carry the gateway's clock, so their times stay right. Everything
// else passes as it is; while down, other packets are dropped as before.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "host_proto.h"
#include "relay.h"

#define BUFFER_ALARMS             32                // queued at most (the oldest go first when full)
#define BUFFER_ALARM_MAX          80                // bytes of one queued packet (an alarm is ~40)
#define BUFFER_SENSORS            32                // passive last-heard sensor telemetry (LRU when full)
#define BUFFER_REPLAY_AFTER       3000              // ms after connecting before the queue is sent (handshake first)
#define BUFFER_REPLAY_EVERY       200               // ms between queued alarms
#define BUFFER_ACK_TIMEOUT        2000              // ms after the last replay before unreceived server ACKs expire
#define BUFFER_MIN_VALID_EPOCH_MS 1577836800000ULL  // 2020-01-01T00:00:00Z: earlier alarms cold-booted without time

typedef struct {
    uint32_t last_seen_ms;  // monotonic now_ms of most recent 0x5319
    uint16_t last_bat_mv;   // mV from 1/32 V BATMON (0 if only 0xAB/0xAD seen so far)
    int8_t last_rssi;       // dBm (e.g. -65)
    uint8_t type;           // wyze_app.h SENSOR_*
    char mac[9];            // 8-char ASCII hex MAC + NUL
} buffer_sensor_t;

typedef struct {
    relay_t *relay;
    void (*uart_write)(void *ctx, const uint8_t *data, size_t len);  // the real UART
    void *ctx;
    host_rx_t up, down;  // packets from the gateway, and from the server
    uint8_t queue[BUFFER_ALARMS][BUFFER_ALARM_MAX];
    uint8_t queue_len[BUFFER_ALARMS];
    uint32_t queue_at_ms[BUFFER_ALARMS];
    unsigned head, count;
    uint32_t connects_seen, open_since_ms, next_replay_ms, last_replay_ms, now_ms;
    unsigned acks_to_keep;                    // server ACKs of replayed alarms still to be kept from the gateway
    uint8_t up_pending[HOST_MAX_PACKET * 2];  // gateway bytes held until we know whether the packet is an alarm
    size_t up_len;
    uint8_t down_pending[HOST_MAX_PACKET * 2];  // server bytes not yet passed on (a packet in progress)
    size_t down_len;
    uint32_t queued_total, dropped_total, replayed_total;  // for the status page
    buffer_sensor_t sensors[BUFFER_SENSORS];
    unsigned sensor_count;
    char gateway_mac[9];
    bool translate_sync;  // CH554 0x43 <-> 0x53 sync command translation for stock CC1310 gateway firmware
    bool sync_pending;
    uint8_t sync_cmd;
} buffer_t;

// uart_write(ctx, ...): the real UART. The platform's relay_io_t uart_write callback calls buffer_from_server(), and
// the platform hands the gateway's bytes to buffer_from_gateway() instead of relay_on_uart().
void buffer_init(buffer_t *b, relay_t *r, void (*uart_write)(void *, const uint8_t *, size_t), void *ctx);

// Bytes the gateway sent (instead of relay_on_uart)
void buffer_from_gateway(buffer_t *b, const uint8_t *data, size_t len, uint32_t now_ms);

// Bytes the relay has for the gateway (the platform's relay uart_write callback calls this)
void buffer_from_server(buffer_t *b, const uint8_t *data, size_t len);

// Time passes (after relay_tick): sends queued alarms when due
void buffer_tick(buffer_t *b, uint32_t now_ms);

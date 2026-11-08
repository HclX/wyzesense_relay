// The relay between the contact's UART and wyzesense2mqtt-rs (esp32/relay), run as a task, with the outage buffer
// (esp32/relay/buffer.c: the gateway's alarms kept while Wi-Fi or the server is down); paused while a bootloader
// session holds the UART.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "buffer.h"
#include "relay.h"

void relay_task_start(void);
void relay_task_reconnect(void);

// For the status page
relay_state_t relay_task_state(void);
uint32_t relay_task_connects(void);
void relay_task_last_log(char *out, size_t cap);
void relay_task_gateway_mac(char out[9]);

// The outage buffer's counts: alarms queued, sent on, dropped (full) since the start; waiting now
void relay_task_buffer(uint32_t *queued, uint32_t *replayed, uint32_t *dropped, unsigned *waiting);

// Snapshot of passively observed sensor telemetry (up to cap entries); sets *now_out to monotonic ms
unsigned relay_task_sensors(buffer_sensor_t *out, unsigned cap, uint32_t *now_out);

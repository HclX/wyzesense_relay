// Runtime gateway network & sensor-table migration over the host UART link (OWS_SUB_NETWORK + OWS_SUB_NV_IMPORT,
// docs/design/DESIGN.md §5.2.2, TASK-14). Portable C99 (zero OS/HW headers), shared across ESP-IDF, ESP8266, and host tests.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GWNV_MAX_SENSORS 64

typedef struct {
    uint16_t sub;
    uint16_t short_addr;
    uint8_t type;
    char mac[9];  // 8 uppercase ASCII hex digits + NUL
} gwnv_sensor_t;

typedef struct {
    uint16_t pan;
    uint16_t short_addr;
    uint64_t ext;
    size_t n_sensors;
    gwnv_sensor_t sensors[GWNV_MAX_SENSORS];
} gwnv_doc_t;

typedef struct {
    bool (*write)(void *ctx, const uint8_t *data, size_t len);
    size_t (*read)(void *ctx, uint8_t *buf, size_t cap, uint32_t timeout_ms);
    void (*flush_input)(void *ctx);
    void *ctx;
} gwnv_io_t;

// Parses and validates an openwyzesense-nv-json v1 document (as produced by tools/nv_json.py or gwnv_format_json)
// into *out before any UART command is sent. Writes a human-readable reason to err on failure.
bool gwnv_parse_json(const char *json, size_t len, gwnv_doc_t *out, char *err, size_t err_cap);

// Formats *doc as an openwyzesense-nv-json v1 JSON document into out[0..cap-1]. Returns bytes written (0 if truncated).
size_t gwnv_format_json(const gwnv_doc_t *doc, char *out, size_t cap);

// Queries the attached OWS gateway via OWS_SUB_NETWORK + OWS_NV_GET_SENSOR and populates *out.
bool gwnv_export(const gwnv_io_t *io, gwnv_doc_t *out, char *err, size_t err_cap);

// Validates *doc and writes it into the attached OWS gateway's Page 30 OWNV via CLEAR -> SET_NET -> ADD_SENSOR*.
bool gwnv_import(const gwnv_io_t *io, const gwnv_doc_t *doc, char *err, size_t err_cap);

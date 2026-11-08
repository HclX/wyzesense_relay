// A multipart/form-data body taken as it streams in (issue #22): the relays' firmware uploads come from one web page
// (esp32/web/index.html) as a form field, on the ESP8266 (whose web server only knows multipart) and the S3 alike, and
// neither can hold a whole upload. The one field asked for is handed over in pieces as it arrives; other fields are
// skipped. Portable C, no I/O.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MP_MAX_BOUNDARY 70  // RFC 2046
#define MP_MAX_HEADERS  512

// a piece of the field's content; false stops the parse (mp_feed returns false)
typedef bool (*mp_data_fn)(void *ctx, const uint8_t *data, size_t len);

typedef struct {
    char delim[MP_MAX_BOUNDARY + 5];  // CR LF "--" boundary
    size_t dlen;
    uint8_t win[MP_MAX_BOUNDARY + 4];  // bytes that may start a delimiter, held back
    size_t wlen;
    char headers[MP_MAX_HEADERS + 1];
    size_t hlen;
    uint8_t out[256];  // the field's bytes, passed on in batches
    size_t olen;
    char field[40];
    int state;
    size_t after;  // bytes seen after a delimiter (its "--" or CR LF)
    bool in_field, found, failed;
    mp_data_fn on_data;
    void *ctx;
} mp_t;

// content_type: the request's header (multipart/form-data; boundary=...); field: the form field's name. False if
// there is no usable boundary.
bool mp_init(mp_t *m, const char *content_type, const char *field, mp_data_fn on_data, void *ctx);

// The next bytes of the body. False when it cannot go on: the callback said stop, or the body is malformed.
bool mp_feed(mp_t *m, const uint8_t *data, size_t len);

// The body ended properly (the closing delimiter) and the field was in it.
bool mp_complete(const mp_t *m);

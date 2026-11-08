#include "multipart.h"

#include <stdio.h>
#include <string.h>

// RFC 7578 / 2046: --boundary CR LF headers CR LF CR LF content CR LF --boundary ... CR LF --boundary-- epilogue. The
// content is scanned for CR LF "--" boundary with the bytes that could start one held back in win.
enum { PREAMBLE, HEADERS, BODY, AFTER, END };

bool mp_init(mp_t *m, const char *content_type, const char *field, mp_data_fn on_data, void *ctx)
{
    memset(m, 0, sizeof *m);
    const char *b = content_type ? strstr(content_type, "boundary=") : NULL;
    if (!b || strlen(field) >= sizeof m->field) {
        return false;
    }
    b += 9;
    bool quoted = *b == '"';
    b += quoted;
    size_t n = 0;
    while (b[n] && b[n] != (quoted ? '"' : ';') && !(!quoted && (b[n] == ' ' || b[n] == '\t'))) {
        n++;
    }
    if (n == 0 || n > MP_MAX_BOUNDARY) {
        return false;
    }
    m->dlen = (size_t)snprintf(m->delim, sizeof m->delim, "\r\n--%.*s", (int)n, b);
    memcpy(m->win, "\r\n", 2);  // the first delimiter comes without the CR LF before it
    m->wlen = 2;
    snprintf(m->field, sizeof m->field, "%s", field);
    m->on_data = on_data;
    m->ctx = ctx;
    m->state = PREAMBLE;
    return true;
}

static bool flush(mp_t *m)
{
    if (m->olen && !m->on_data(m->ctx, m->out, m->olen)) {
        m->failed = true;
    }
    m->olen = 0;
    return !m->failed;
}

static bool content(mp_t *m, uint8_t c)
{
    if (m->state != BODY || !m->in_field) {
        return true;
    }
    m->out[m->olen++] = c;
    return m->olen < sizeof m->out || flush(m);
}

// a byte of the preamble or a part's content
static bool scan(mp_t *m, uint8_t c)
{
    m->win[m->wlen++] = c;
    while (m->wlen) {
        if (!memcmp(m->win, m->delim, m->wlen)) {  // still a delimiter's start
            if (m->wlen < m->dlen) {
                return true;
            }
            m->wlen = 0;  // a delimiter: the part before it ends
            if (m->state == BODY && m->in_field) {
                m->found = true;
                if (!flush(m)) {
                    return false;
                }
            }
            m->in_field = false;
            m->state = AFTER;
            m->after = 0;
            return true;
        }
        if (!content(m, m->win[0])) {
            return false;
        }
        memmove(m->win, m->win + 1, --m->wlen);
    }
    return true;
}

static bool header_byte(mp_t *m, uint8_t c)
{
    if (m->hlen >= MP_MAX_HEADERS) {
        return false;
    }
    m->headers[m->hlen++] = (char)c;
    if (m->hlen < 4 || memcmp(m->headers + m->hlen - 4, "\r\n\r\n", 4) != 0) {
        return true;
    }
    m->headers[m->hlen] = 0;
    char want[sizeof m->field + 8];
    snprintf(want, sizeof want, "name=\"%s\"", m->field);
    m->in_field = strstr(m->headers, want) != NULL;
    m->state = BODY;
    m->wlen = 0;
    return true;
}

bool mp_feed(mp_t *m, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len && !m->failed; i++) {
        uint8_t c = data[i];
        bool ok = true;
        switch (m->state) {
            case PREAMBLE:
            case BODY:
                ok = scan(m, c);
                break;
            case HEADERS:
                ok = header_byte(m, c);
                break;
            case AFTER:  // "--": the last part; CR LF: another part's headers
                if (m->after++ == 0) {
                    m->headers[0] = (char)c;
                } else if (m->headers[0] == '-' && c == '-') {
                    m->state = END;
                } else if (m->headers[0] == '\r' && c == '\n') {
                    m->state = HEADERS;
                    m->hlen = 0;
                } else {
                    ok = false;
                }
                break;
            default:  // END: the epilogue, ignored
                break;
        }
        if (!ok) {
            m->failed = true;
        }
    }
    return !m->failed;
}

bool mp_complete(const mp_t *m)
{
    return m->state == END && m->found && !m->failed;
}

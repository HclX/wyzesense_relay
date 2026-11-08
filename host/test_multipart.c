// Native tests of the streaming multipart parser (esp32/web/multipart.c): the field's bytes come out exactly, however
// the body is cut into pieces, also when the content holds near-delimiters (CR LF "--" and part of the boundary);
// other fields are skipped; a body without its closing delimiter, or without the field, is not complete; a callback
// can stop it; a quoted boundary; headers that never end.
#include <stdio.h>
#include <string.h>

#include "multipart.h"

static int failures, checks;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        checks++;                                                                    \
        if (!(cond)) {                                                               \
            failures++;                                                              \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                            \
    } while (0)

static struct {
    uint8_t got[8192];
    size_t len;
    size_t stop_after;  // the callback says stop once this much came (0: never)
} r;

static bool on_data(void *ctx, const uint8_t *d, size_t n)
{
    (void)ctx;
    if (r.len + n > sizeof r.got) {
        return false;
    }
    memcpy(r.got + r.len, d, n);
    r.len += n;
    return !r.stop_after || r.len < r.stop_after;
}

#define BOUNDARY "----owsFormBoundary7MA4YWxkTrZu0gW"

static uint8_t content[3000];

// a browser's body: a text field, then the image field, then the end (or not)
static size_t body(uint8_t *b, bool closed, bool with_image)
{
    size_t n = (size_t)sprintf((char *)b,
                               "--" BOUNDARY "\r\nContent-Disposition: form-data; name=\"mac\"\r\n\r\n777B42DF\r\n");
    if (with_image) {
        n += (size_t)sprintf((char *)b + n,
                             "--" BOUNDARY "\r\nContent-Disposition: form-data; name=\"image\"; filename=\"a.bin\"\r\n"
                             "Content-Type: application/octet-stream\r\n\r\n");
        memcpy(b + n, content, sizeof content);
        n += sizeof content;
    }
    n += (size_t)sprintf((char *)b + n, closed ? "\r\n--" BOUNDARY "--\r\n" : "\r\n--" BOUNDARY);
    return n;
}

static bool run(const uint8_t *b, size_t n, size_t piece, const char *ctype, mp_t *m)
{
    r.len = 0;
    if (!mp_init(m, ctype, "image", on_data, NULL)) {
        return false;
    }
    for (size_t at = 0; at < n; at += piece) {
        if (!mp_feed(m, b + at, n - at < piece ? n - at : piece)) {
            return false;
        }
    }
    return true;
}

int main(void)
{
    static uint8_t b[sizeof content + 1024];
    static mp_t m;
    const char *ctype = "multipart/form-data; boundary=" BOUNDARY;
    uint32_t x = 1;  // a fixed pseudo-random content (an LCG: the same bytes every run)
    for (size_t i = 0; i < sizeof content; i++) {
        x = x * 1103515245u + 12345u;
        content[i] = (uint8_t)(x >> 16);
    }
    // near-delimiters inside the content: CR LF "--" and most of the boundary, at a few places
    const char *near = "\r\n--" BOUNDARY;
    memcpy(content + 100, near, strlen(near) - 1);
    memcpy(content + 1000, "\r\n--", 4);
    memcpy(content + 2000, "\r\r\n--\r\n-", 8);
    content[sizeof content - 1] = '\r';  // a CR just before the real delimiter
    size_t n = body(b, true, true);

    for (size_t piece = 1; piece <= 600; piece = piece < 20 ? piece + 1 : piece * 3 / 2) {
        CHECK(run(b, n, piece, ctype, &m));
        CHECK(mp_complete(&m));
        CHECK(r.len == sizeof content && !memcmp(r.got, content, sizeof content));
    }
    CHECK(run(b, n, n, "multipart/form-data; boundary=\"" BOUNDARY "\"", &m) && mp_complete(&m));  // quoted
    CHECK(r.len == sizeof content);

    n = body(b, false, true);  // cut off before the closing "--"
    CHECK(run(b, n, 64, ctype, &m) && !mp_complete(&m));
    n = body(b, true, false);  // no image field
    CHECK(run(b, n, 64, ctype, &m) && !mp_complete(&m) && r.len == 0);

    n = body(b, true, true);
    r.stop_after = 1000;  // the callback stops it (a flash that failed)
    CHECK(!run(b, n, 64, ctype, &m) && !mp_complete(&m));
    r.stop_after = 0;

    CHECK(!mp_init(&m, "multipart/form-data", "image", on_data, NULL));  // no boundary
    CHECK(!mp_init(&m, NULL, "image", on_data, NULL));
    memset(b, 'x', sizeof b);  // headers that never end
    memcpy(b, "--" BOUNDARY "\r\n", strlen("--" BOUNDARY "\r\n"));
    CHECK(!run(b, 2000, 64, ctype, &m));

    printf("%d checks, %d failed\n", checks, failures);
    return failures != 0;
}

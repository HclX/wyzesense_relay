// ows-sbl: the bootloader code the ESP32 runs to reflash its contact board (esp32/contact/sbl.c), on a PC, as
// tools/uart_sbl.py does: reference/tests/test_ows_sbl.py runs it against the same simulated ROM as uart_sbl.py.
//   ows-sbl --port PORT check
//   ows-sbl --port PORT flash (BOOT.bin SLOT_A.img | APP.bin) --yes [--mac 777B42DF] [--nv keep|wipe|PAGE.bin]
// The chip is left in the bootloader; reset it with S (RESET_N) or the power.
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "sbl.h"

static int fd = -1, quiet_ms = 20;  // a read ends when the line is quiet this long (OWS_UART_GAP_MS: tests)

static bool u_write(void *ctx, const uint8_t *d, size_t n)
{
    (void)ctx;
    while (n) {
        ssize_t k = write(fd, d, n);
        if (k < 0 && (errno == EAGAIN || errno == EINTR)) {
            struct pollfd p = {fd, POLLOUT, 0};
            poll(&p, 1, 100);
            continue;
        }
        if (k <= 0) {
            return false;
        }
        d += k, n -= (size_t)k;
    }
    tcdrain(fd);
    return true;
}

static size_t u_read(void *ctx, uint8_t *buf, size_t cap, uint32_t timeout_ms)
{
    (void)ctx;
    struct pollfd p = {fd, POLLIN, 0};
    if (poll(&p, 1, (int)timeout_ms) <= 0) {
        return 0;
    }
    size_t n = 0;
    for (;;) {  // what has come, until the line is quiet for a moment
        ssize_t k = read(fd, buf + n, cap - n);
        if (k > 0) {
            n += (size_t)k;
        }
        if (n == cap || poll(&p, 1, quiet_ms) <= 0) {
            return n;
        }
    }
}

static void u_flush(void *ctx)
{
    (void)ctx;
    tcflush(fd, TCIFLUSH);
}

static uint8_t *load(const char *path, size_t *len)
{
    if (!path) {
        fprintf(stderr, "a file name is missing\n");
        exit(2);
    }
    FILE *f = fopen(path, "rb");
    uint8_t *b = NULL;
    if (f && !fseek(f, 0, SEEK_END)) {
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        b = n > 0 ? malloc((size_t)n) : NULL;
        if (b && fread(b, 1, (size_t)n, f) == (size_t)n) {
            *len = (size_t)n;
        } else {
            free(b);
            b = NULL;
        }
    }
    if (f) {
        fclose(f);
    }
    if (!b) {
        fprintf(stderr, "%s: cannot read\n", path);
        exit(2);
    }
    return b;
}

int main(int argc, char **argv)
{
    const char *port = NULL, *mac = NULL, *nv = "keep", *files[2] = {NULL, NULL}, *cmd = NULL;
    bool yes = false;
    int nfiles = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) {
            port = argv[++i];
        } else if (!strcmp(argv[i], "--mac") && i + 1 < argc) {
            mac = argv[++i];
        } else if (!strcmp(argv[i], "--nv") && i + 1 < argc) {
            nv = argv[++i];
        } else if (!strcmp(argv[i], "--yes")) {
            yes = true;
        } else if (!cmd) {
            cmd = argv[i];
        } else if (nfiles < 2) {
            files[nfiles++] = argv[i];
        }
    }
    if (!port || !cmd || (strcmp(cmd, "check") != 0 && strcmp(cmd, "flash") != 0) ||
        (!strcmp(cmd, "flash") && (!nfiles || !files[0]))) {
        fprintf(stderr, "usage: ows-sbl --port PORT check | flash (BOOT.bin SLOT_A.img | APP.bin) --yes [--mac M] "
                        "[--nv keep|wipe|PAGE.bin]\n");
        return 2;
    }
    const char *gap = getenv("OWS_UART_GAP_MS");
    if (gap) {
        quiet_ms = (int)strtol(gap, NULL, 10);
    }
    fd = open(port, O_RDWR | O_NOCTTY | O_NONBLOCK);
    struct termios t;
    if (fd < 0 || tcgetattr(fd, &t)) {
        perror(port);
        return 2;
    }
    cfmakeraw(&t);
    t.c_cflag |= CLOCAL | CREAD;
    cfsetispeed(&t, B115200);
    cfsetospeed(&t, B115200);
    tcsetattr(fd, TCSANOW, &t);

    sbl_t s;
    sbl_io_t io = {u_write, u_read, u_flush, NULL};
    sbl_init(&s, &io);
    if (!sbl_connect(&s, mac)) {
        printf("%s\n", s.error);
        return 1;
    }
    printf("%sbootloader: ACK; chip ID %08x\nCCFG: MAC %s, backdoor DIO%u\n",
           s.already_synced ? "bootloader already synced\n" : "", s.chip_id, s.mac, s.backdoor_dio);
    if (!strcmp(cmd, "check")) {
        return 0;
    }
    static uint8_t image[SBL_APP_END], page[SBL_PAGE];
    size_t len = 0;
    bool with_page = strcmp(nv, "keep") != 0;
    if (nfiles == 2) {
        size_t blen = 0, slen = 0;
        uint8_t *boot = load(files[0], &blen), *slot = load(files[1], &slen);
        bool ok = sbl_slots_image(image, boot, blen, slot, slen);
        free(boot);
        free(slot);
        if (!ok) {
            printf("a boot selector of at most a page and a slot image (OWSI) expected\n");
            return 1;
        }
        len = SBL_SLOTS_LEN;
    } else {
        uint8_t *app = load(files[0], &len);
        bool fits = len <= sizeof image;
        if (fits) {
            memset(image, 0xFF, sizeof image);
            memcpy(image, app, len);
            len = (len + 3) & ~(size_t)3;
        }
        free(app);
        if (!fits) {
            printf("%s: more than pages 0-29\n", files[0]);
            return 1;
        }
    }
    if (!strcmp(nv, "wipe")) {
        memset(page, 0xFF, sizeof page);
    } else if (with_page) {
        size_t n = 0;
        uint8_t *p = load(nv, &n);
        bool ok = n == SBL_PAGE;
        if (ok) {
            memcpy(page, p, n);
        }
        free(p);
        if (!ok) {
            printf("%s: a 4096-byte NV page expected (tools/nv_json.py import)\n", nv);
            return 1;
        }
    }
    printf("flash: %zu bytes to pages 0-%zu; NV page: %s\n", len, (len - 1) / SBL_PAGE, nv);
    if (!yes) {
        printf("not written: add --yes\n");
        return 0;
    }
    if (!sbl_flash(&s, image, len) || (with_page && !sbl_write_nv(&s, page))) {
        printf("%s\n", s.error);
        return 1;
    }
    printf("verified: chip CRC32 match, page 31 unchanged\nwritten; now reset it with S or a power cycle\n");
    return 0;
}

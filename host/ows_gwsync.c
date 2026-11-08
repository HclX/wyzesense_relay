// ows-gwsync: the relays' start-up gateway check and update (esp32/contact/gwsync.c, #76) on a PC, against a
// simulated contact board (reference/tests/test_gwsync.py: a gateway that answers or not, and the ROM bootloader).
//   ows-gwsync --port PORT --ctl FIFO IMAGE.bin
// Control lines are written to FIFO: "R" a reset into the firmware, "B" a reset into the bootloader (R held low),
// "J" the carried image written through the debug port (--cjtag ok). Prints the result and the report; exit 0 for same
// or updated. --cjtag ok|same|fail simulates a debug port (#81): written, already equal, or not reachable;
// --transport auto|cjtag|sbl picks how gwsync reflashes.
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "gwsync.h"

static int fd = -1, ctl = -1, quiet_ms = 20;  // a read ends when the line is quiet this long (OWS_UART_GAP_MS)
static uint8_t *image;
static size_t image_len;

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
    for (;;) {
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

static void sleep_ms(void *ctx, uint32_t ms)
{
    (void)ctx;
    struct timespec t = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
    nanosleep(&t, NULL);
}

static void pad(const char *what)  // the simulation switches, as the board would at the reset
{
    if (write(ctl, what, strlen(what)) < 0) {
        perror("ctl");
    }
    sleep_ms(NULL, 50);
}

static void reset(void *ctx)
{
    (void)ctx;
    pad("R\n");
}

static void enter_bootloader(void *ctx)
{
    (void)ctx;
    pad("B\n");
}

static const char *cjtag_mode;  // NULL: no debug port

static int cjtag_flash(void *ctx, uint32_t len, char *mac, char *err, size_t cap)
{
    (void)ctx;
    if (!strcmp(cjtag_mode, "fail")) {
        snprintf(err, cap, "no CC1310 ICEPick-C IDCODE on probe pins");
        return -1;
    }
    snprintf(mac, 9, "777B42DF");
    if (!strcmp(cjtag_mode, "same")) {
        return 0;
    }
    pad("J\n");  // the simulation writes the carried image, as the debug port would, and resets into it
    return (int)((len + SBL_PAGE - 1) / SBL_PAGE);
}

static void progress(void *ctx, const char *s)
{
    (void)ctx;
    printf("step: %s\n", s);
    fflush(stdout);
}

static bool read_image(void *ctx, uint32_t off, uint8_t *buf, size_t len)
{
    (void)ctx;
    if (off + len > image_len) {
        return false;
    }
    memcpy(buf, image + off, len);
    return true;
}

int main(int argc, char **argv)
{
    const char *port = NULL, *ctl_path = NULL, *path = NULL;
    bool skip = false, query = false;
    uint8_t transport = GWSYNC_VIA_AUTO;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) {
            port = argv[++i];
        } else if (!strcmp(argv[i], "--ctl") && i + 1 < argc) {
            ctl_path = argv[++i];
        } else if (!strcmp(argv[i], "--skip")) {
            skip = true;
        } else if (!strcmp(argv[i], "--query")) {
            query = true;
        } else if (!strcmp(argv[i], "--cjtag") && i + 1 < argc) {
            cjtag_mode = argv[++i];
        } else if (!strcmp(argv[i], "--transport") && i + 1 < argc) {
            const char *t = argv[++i];
            transport = !strcmp(t, "cjtag") ? GWSYNC_VIA_CJTAG : !strcmp(t, "sbl") ? GWSYNC_VIA_SBL : GWSYNC_VIA_AUTO;
        } else {
            path = argv[i];
        }
    }
    if (!port || (!query && (!ctl_path || !path))) {
        fprintf(stderr, "usage: ows-gwsync [--skip] --port PORT --ctl FIFO IMAGE.bin\n"
                        "       ows-gwsync --query --port PORT\n");
        return 2;
    }
    const char *gap = getenv("OWS_UART_GAP_MS");
    if (gap) {
        quiet_ms = (int)strtol(gap, NULL, 10);
    }
    static uint8_t buf[SBL_APP_END];
    if (!query) {
        FILE *f = fopen(path, "rb");
        image_len = f ? fread(buf, 1, sizeof buf, f) : 0;
        if (f) {
            fclose(f);
        }
        image = buf;
    }
    fd = open(port, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (!query) {
        ctl = open(ctl_path, O_WRONLY);
    }
    struct termios t;
    if ((!query && (!image_len || ctl < 0)) || fd < 0 || tcgetattr(fd, &t)) {
        perror("ows-gwsync");
        return 2;
    }
    cfmakeraw(&t);
    t.c_cflag |= CLOCAL | CREAD;
    cfsetispeed(&t, B115200);
    cfsetospeed(&t, B115200);
    tcsetattr(fd, TCSANOW, &t);

    gwsync_io_t io = {u_write,  u_read,     u_flush,  reset, enter_bootloader,
                      sleep_ms, read_image, progress, NULL,  cjtag_mode ? cjtag_flash : NULL,
                      transport};
    if (query) {
        build_info_t info;
        if (!gwsync_query(&io, &info)) {
            printf("answered: no\n");
            return 1;
        }
        printf("answered: yes\nproduct: %.24s\nboard: %.24s\napp: %.12s\nversion: %.40s\ntx: %u\n", info.product,
               info.board, info.app, info.version, (unsigned)((info.flags & BUILD_INFO_TX) != 0));
        return 0;
    }

    static sbl_t s;
    static uint8_t page[SBL_PAGE];
    static gwsync_report_t r;
    static const char *names[] = {"same", "updated", "failed", "refused"};
    gwsync_result_t res =
        skip ? gwsync_probe(&io, (uint32_t)image_len, page, &r) : gwsync_run(&io, (uint32_t)image_len, &s, page, &r);
    printf("result: %s\nanswered: %s\nmac: %s\njunk: %lu\nmessage: %s\n", names[res], r.answered ? "yes" : "no",
           r.mac[0] ? r.mac : "-", (unsigned long)r.junk, r.msg);
    return res == GWSYNC_SAME || res == GWSYNC_UPDATED ? 0 : 1;
}

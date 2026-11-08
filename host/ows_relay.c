// ows-relay: the relay core (esp32/relay) on a PC: a gateway's UART (a serial port or a host gateway's pty) to
// wyzesense2mqtt-rs's /ws/bridge, as tools/ws_bridge.py does. It runs the same code the ESP32 firmware will, so the
// end-to-end test (reference/tests/test_host_e2e.py) checks it with no hardware.
//   ows-relay [--device /dev/ttyACM0] [--gateway ws://127.0.0.1:8080/ws/bridge] [--token SECRET] [--no-buffer]
// The gateway's alarms are kept through an outage of the server (esp32/relay/buffer.c) unless --no-buffer.
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "buffer.h"
#include "relay.h"
#include "sock_posix.h"

static int uart = -1;
static sock_link_t tcp_link;
static buffer_t buffer;
static volatile sig_atomic_t quit;

static uint32_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)((uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000);
}

static void to_gateway(void *ctx, const uint8_t *data, size_t len)  // the relay's bytes for the gateway: the buffer's
{
    (void)ctx;
    buffer_from_server(&buffer, data, len);
}

static void uart_write(void *ctx, const uint8_t *data, size_t len)
{
    (void)ctx;
    while (len && uart >= 0) {
        ssize_t n = write(uart, data, len);
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
            struct pollfd p = {uart, POLLOUT, 0};
            poll(&p, 1, 100);
            continue;
        }
        if (n <= 0) {
            return;
        }
        data += n, len -= (size_t)n;
    }
}

static uint32_t random32(void *ctx)
{
    (void)ctx;
    return (uint32_t)rand() << 16 ^ (uint32_t)rand();
}

static void log_line(void *ctx, const char *msg)
{
    (void)ctx;
    fprintf(stderr, "%s\n", msg);
}

static int open_uart(const char *path)
{
    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    struct termios t;
    if (fd < 0 || tcgetattr(fd, &t) != 0) {
        if (fd >= 0) {
            close(fd);
        }
        return -1;
    }
    cfmakeraw(&t);
    t.c_cflag |= CLOCAL | CREAD;
    cfsetispeed(&t, B115200);
    cfsetospeed(&t, B115200);
    tcsetattr(fd, TCSANOW, &t);
    tcflush(fd, TCIOFLUSH);
    return fd;
}

static void on_signal(int s)
{
    (void)s;
    quit = 1;
}

int main(int argc, char **argv)
{
    const char *device = "/dev/ttyACM0", *url = "ws://127.0.0.1:8080/ws/bridge", *token = NULL;
    bool buffered = true;
    for (int i = 1; i < argc; i++) {
        bool value = i + 1 < argc;
        if (value && (!strcmp(argv[i], "--device") || !strcmp(argv[i], "-d"))) {
            device = argv[++i];
        } else if (value && (!strcmp(argv[i], "--gateway") || !strcmp(argv[i], "-g"))) {
            url = argv[++i];
        } else if (value && (!strcmp(argv[i], "--token") || !strcmp(argv[i], "-t"))) {
            token = argv[++i];
        } else if (!strcmp(argv[i], "--no-buffer")) {
            buffered = false;
        } else {
            fprintf(stderr,
                    "usage: %s [--device PATH] [--gateway ws://host:port/path] [--token SECRET] [--no-buffer]\n",
                    argv[0]);
            return 2;
        }
    }
    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    relay_t r;
    relay_io_t io = {sock_link_relay_connect,
                     sock_link_relay_send,
                     sock_link_relay_close,
                     buffered ? to_gateway : uart_write,
                     random32,
                     log_line,
                     &tcp_link};
    sock_link_init(&tcp_link, 5000);
    sock_link_set_nonblocking(&tcp_link, true);
    buffer_init(&buffer, &r, uart_write, NULL);
    if (!relay_init(&r, url, device, token, &io)) {
        fprintf(stderr, "%s: not a ws:// URL this relay can use\n", url);
        return 2;
    }
    uint32_t uart_retry = 0;
    while (!quit) {
        uint32_t now = now_ms();
        if (uart < 0 && (int32_t)(now - uart_retry) >= 0) {
            uart = open_uart(device);
            uart_retry = now + 1000;
            if (uart >= 0) {
                fprintf(stderr, "relay: %s open\n", device);
            }
        }
        if (uart >= 0) {
            relay_tick(&r, now);  // the WebSocket only while there is a gateway on the line
            buffer_tick(&buffer, now);
        }
        struct pollfd p[2] = {{uart, POLLIN, 0}, {tcp_link.fd, tcp_link.connecting ? POLLOUT : POLLIN, 0}};
        if (poll(p, 2, 200) < 0 && errno != EINTR) {
            break;
        }
        now = now_ms();
        uint8_t buf[1024];
        if (uart >= 0 && p[0].revents) {
            ssize_t n = read(uart, buf, sizeof buf);
            if (n > 0) {
                if (buffered) {
                    buffer_from_gateway(&buffer, buf, (size_t)n, now);
                } else {
                    relay_on_uart(&r, buf, (size_t)n, now);
                }
            } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
                fprintf(stderr, "relay: %s hung up\n", device);  // unplugged, or a host gateway's pty closed
                close(uart);
                uart = -1;
                relay_on_tcp_closed(&r, now);
            }
        }
        if (tcp_link.fd >= 0 && p[1].revents) {
            if (tcp_link.connecting) {
                if (sock_link_finish_connect(&tcp_link) < 0) {
                    relay_on_tcp_closed(&r, now);
                }
            } else {
                int n = sock_link_recv(&tcp_link, buf, sizeof buf);
                if (n > 0) {
                    relay_on_tcp(&r, buf, (size_t)n, now);
                } else if (n < 0) {
                    relay_on_tcp_closed(&r, now);
                }
            }
        }
    }
    sock_link_close(&tcp_link);
    return 0;
}

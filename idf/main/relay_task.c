#include "relay_task.h"

#include "buffer.h"

#include <stdio.h>
#include <string.h>
#include <sys/select.h>

#include "contact.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net.h"
#include "settings.h"
#include "sock_posix.h"

static const char *TAG = "relay";
static relay_t relay;
static buffer_t buffer;
static sock_link_t link_;
static bool ready;
static volatile bool reconnect_requested;
static portMUX_TYPE s_log_mux = portMUX_INITIALIZER_UNLOCKED;
static char s_last_log[128];

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void uart_out(void *ctx, const uint8_t *d, size_t n)  // the real UART (the buffer's)
{
    (void)ctx;
    contact_uart_write(d, n);
}

static void to_gateway(void *ctx, const uint8_t *d, size_t n)  // the relay's bytes for the gateway: the buffer's
{
    (void)ctx;
    buffer_from_server(&buffer, d, n);
}

static uint32_t rnd(void *ctx)
{
    (void)ctx;
    return esp_random();
}

static void set_last_log(const char *msg)
{
    char tmp[sizeof s_last_log];
    snprintf(tmp, sizeof tmp, "%s", msg ? msg : "");
    portENTER_CRITICAL(&s_log_mux);
    memcpy(s_last_log, tmp, sizeof s_last_log);
    portEXIT_CRITICAL(&s_log_mux);
}

static void r_log(void *ctx, const char *msg)
{
    (void)ctx;
    set_last_log(msg);
    ESP_LOGI(TAG, "%s", msg);
}

static void task(void *arg)
{
    (void)arg;
    uint8_t buf[512];
    contact_gwsync();  // before anything is relayed: the contact on the carried gateway image (#76)
    const gwsync_report_t *rep = contact_gwsync_report();
    if (rep && rep->msg[0]) {
        char gmsg[sizeof s_last_log];
        snprintf(gmsg, sizeof gmsg, "gateway: %.110s", rep->msg);
        set_last_log(gmsg);
    }
    for (;;) {
        uint32_t now = now_ms();
        if (reconnect_requested) {
            reconnect_requested = false;
            relay_on_tcp_closed(&relay, now);
        }
        if (ready && net_connected()) {
            relay_tick(&relay, now);
        }
        if (!contact_uart_take(100)) {
            continue;  // a reflash holds it
        }
        buffer_tick(&buffer, now_ms());
        if (link_.fd >= 0) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(link_.fd, &fds);
            struct timeval tv = {0, 10000};
            if (link_.connecting) {
                if (select(link_.fd + 1, NULL, &fds, NULL, &tv) == 1 && sock_link_finish_connect(&link_) < 0) {
                    relay_on_tcp_closed(&relay, now_ms());
                }
            } else if (select(link_.fd + 1, &fds, NULL, NULL, &tv) == 1) {
                int n = sock_link_recv(&link_, buf, sizeof buf);
                if (n > 0) {
                    relay_on_tcp(&relay, buf, (size_t)n, now_ms());
                } else if (n < 0) {
                    relay_on_tcp_closed(&relay, now_ms());
                }
            }
        }
        // the gateway is read also while Wi-Fi or the server is down: its alarms are ACKed and kept (buffer.c)
        size_t n = contact_uart_read(buf, sizeof buf, link_.fd >= 0 ? 0 : 20);
        if (n) {
            buffer_from_gateway(&buffer, buf, n, now_ms());
        }
        contact_uart_give();
        // let a waiting web request (console, NV) have the UART: given and taken back at once at the same
        // priority, the lock never reached it ("the contact is busy", 2026-11-01)
        vTaskDelay(1);
    }
}

void relay_task_start(void)
{
    uint8_t mac[6];
    char device[32];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(device, sizeof device, "esp32-%02x%02x%02x", mac[3], mac[4], mac[5]);  // the server's log label
    relay_io_t io = {
        sock_link_relay_connect, sock_link_relay_send, sock_link_relay_close, to_gateway, rnd, r_log, &link_};
    sock_link_init(&link_, 5000);
    sock_link_set_nonblocking(&link_, true);
    buffer_init(&buffer, &relay, uart_out, NULL);
    ready = settings.url[0] && relay_init(&relay, settings.url, device, settings.token[0] ? settings.token : NULL, &io);
    if (!ready) {
        set_last_log("no usable wyzesense2mqtt-rs URL set");
        ESP_LOGW(TAG, "no usable wyzesense2mqtt-rs URL set (ws://host:port/ws/bridge): open the setup page");
    }
    xTaskCreate(task, "relay", 6144, NULL, 5, NULL);
}

void relay_task_reconnect(void)
{
    reconnect_requested = true;
}

relay_state_t relay_task_state(void)
{
    return relay.state;
}

uint32_t relay_task_connects(void)
{
    return relay.connects;
}

void relay_task_last_log(char *out, size_t cap)
{
    if (!out || !cap) {
        return;
    }
    char tmp[sizeof s_last_log];
    portENTER_CRITICAL(&s_log_mux);
    memcpy(tmp, s_last_log, sizeof tmp);
    portEXIT_CRITICAL(&s_log_mux);
    snprintf(out, cap, "%s", tmp);
}

void relay_task_gateway_mac(char out[9])
{
    bool locked = contact_uart_take(50);
    memcpy(out, buffer.gateway_mac, 9);
    out[8] = '\0';
    if (locked) {
        contact_uart_give();
    }
}

void relay_task_buffer(uint32_t *queued, uint32_t *replayed, uint32_t *dropped, unsigned *waiting)
{
    *queued = buffer.queued_total, *replayed = buffer.replayed_total, *dropped = buffer.dropped_total;
    *waiting = buffer.count;
}

unsigned relay_task_sensors(buffer_sensor_t *out, unsigned cap, uint32_t *now_out)
{
    bool locked = contact_uart_take(50);
    unsigned n = buffer.sensor_count < cap ? buffer.sensor_count : cap;
    if (n) {
        memcpy(out, buffer.sensors, n * sizeof *out);
    }
    if (now_out) {
        *now_out = now_ms();
    }
    if (locked) {
        contact_uart_give();
    }
    return n;
}

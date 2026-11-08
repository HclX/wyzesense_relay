// The battery rig's core (esp32/rig/rig.c) against a simulated contact board: its decoupling capacitance as the only
// store, fed by an ESP32 GPIO (3.3 V through its on-resistance) when the rig drives it, drained by a sensor (sleep
// current, short CPU wakes, radio bursts), read by a noisy ADC that takes time. Checks that the sleep current comes
// out right with or without wakes, bursts and noise, that wakes and bursts are counted, and that the supply never
// nears the brownout.
#include "rig.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int failures;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
            failures++;                                                  \
        }                                                                \
    } while (0)

typedef struct {
    double c_f, r_on, v_src;              // the board's capacitance, the GPIO's on-resistance, its supply
    double sleep_a;                       // the sensor asleep
    double wake_a, wake_s, wake_every_s;  // CPU wakes
    double tx_a, tx_s, tx_every_s;        // radio bursts (0: none)
    double noise_mv;                      // ADC noise (uniform +-)
    uint32_t spike_every;                 // one reading in this many is an ADC spike (0: none)
    double spike_mv;
    double read_s;  // one ADC reading (the rig's loop period while waiting)
    // state
    double t, v, v_min;
    uint32_t rng;
} board_t;

static double load_a(const board_t *b)
{
    double i = b->sleep_a;
    if (b->wake_every_s > 0 && fmod(b->t, b->wake_every_s) < b->wake_s) {
        i += b->wake_a;
    }
    if (b->tx_every_s > 0 && fmod(b->t + 0.5, b->tx_every_s) < b->tx_s) {
        i += b->tx_a;
    }
    return i;
}

// dt seconds, the supply driven or not
static void run(board_t *b, double dt, bool driven)
{
    const double step = 1e-6;
    long steps = (long)ceil(dt / step - 1e-9);
    for (long k = 0; k < steps; k++) {
        double done = (double)k * step;
        double i = load_a(b), h = dt - done < step ? dt - done : step;
        double in = driven ? (b->v_src - b->v) / b->r_on : 0;
        b->v += (in - i) * h / b->c_f;
        b->t += h;
        if (b->v < b->v_min) {
            b->v_min = b->v;
        }
    }
}

static uint16_t adc(board_t *b)
{
    b->rng = b->rng * 1103515245u + 12345u;
    double n = ((double)(b->rng >> 8 & 0xFFFF) / 65535.0 * 2 - 1) * b->noise_mv;
    if (b->spike_every && (b->rng >> 4) % b->spike_every == 0) {
        n -= b->spike_mv;
    }
    return (uint16_t)lround(b->v * 1000 + n);
}

static void simulate(board_t *b, rig_t *r, double seconds)
{
    rig_cfg_t cfg;
    rig_default_cfg(&cfg);
    cfg.cap_nf = (uint32_t)lround(b->c_f * 1e9);  // calibrated
    rig_start(r, &cfg);
    b->v = b->v_min = 3.0;
    b->rng = 1;
    rig_kind_t last = RIG_WAIT;
    while (b->t < seconds) {
        bool driven = last == RIG_HOLD;  // a hold reads with the supply still on
        run(b, b->read_s, driven);       // the reading itself
        rig_action_t a = rig_step(r, (uint64_t)llround(b->t * 1e6), adc(b), driven);
        if (a.kind != RIG_WAIT) {
            run(b, a.us * 1e-6, true);
        }
        last = a.kind;
    }
}

static board_t base(void)
{
    board_t b = {0};
    b.c_f = 8.2e-6;
    b.r_on = 40;
    b.v_src = 3.3;
    b.sleep_a = 1.5e-6;
    b.read_s = 25e-6;
    return b;
}

static void test_sleep_only(void)
{
    board_t b = base();
    rig_t r;
    simulate(&b, &r, 120);
    double ua = rig_sleep_na(&r) / 1000.0;
    printf("sleep only: %.3f uA (true 1.500), %u decays, %u pulses, v %u..%u\n", ua, r.quiet_segments, r.pulses,
           r.v_min_mv, r.v_max_mv);
    CHECK(fabs(ua - 1.5) < 0.03);
    CHECK(r.holds == 0 && r.wakes == 0 && r.quiet_segments > 50);
}

static void test_noisy_adc_and_cpu_wakes(void)
{
    board_t b = base();
    b.noise_mv = 8;
    b.wake_a = 3e-3;  // 3 mA for 3 ms every 10 s
    b.wake_s = 3e-3;
    b.wake_every_s = 10;
    rig_t r;
    simulate(&b, &r, 300);
    double ua = rig_sleep_na(&r) / 1000.0;
    printf("noisy ADC + CPU wakes: sleep %.3f uA (true 1.500), wakes %u (%.1f ms), holds %u, min %.3f V\n", ua, r.wakes,
           r.wake_time_us / 1000.0, r.holds, b.v_min);
    CHECK(fabs(ua - 1.5) < 0.05);
    CHECK(r.holds == 0);  // a 3 mA wake is followed by pulses, not held
    CHECK(r.wakes >= 29 && r.wakes <= 31);
    CHECK(b.v_min > 2.8);
    CHECK(r.wake_gaps > 0 && r.wake_gap_min_us > 9900000 && r.wake_gap_max_us < 10100000);  // every 10 s
}

static void test_radio_bursts_are_held(void)
{
    board_t b = base();
    b.noise_mv = 8;
    b.tx_a = 25e-3;  // a radio burst: 25 mA for 5 ms every 30 s
    b.tx_s = 5e-3;
    b.tx_every_s = 30;
    rig_t r;
    simulate(&b, &r, 300);
    double ua = rig_sleep_na(&r) / 1000.0;
    printf("radio bursts: sleep %.3f uA (true 1.500), wakes %u, holds %u (%.1f ms), min %.3f V\n", ua, r.wakes, r.holds,
           r.hold_time_us / 1000.0, b.v_min);
    CHECK(fabs(ua - 1.5) < 0.05);
    CHECK(r.holds >= 9 && r.holds <= 11 && r.wakes >= 9 && r.wakes <= 11);  // 10 bursts: one hold each
    CHECK(b.v_min > 2.0);                                                   // never near the CC1310's ~1.8 V brownout
}

static void test_capacitance_scales(void)
{
    // the capacitance is a guess until calibrated: the current scales with it, nothing else changes
    board_t b = base();
    rig_t r;
    simulate(&b, &r, 60);
    int64_t na = rig_sleep_na(&r);
    r.cfg.cap_nf *= 2;
    CHECK(llabs(rig_sleep_na(&r) - 2 * na) <= 2);
}

static void test_small_capacitance_stays_safe(void)
{
    // the board's capacitance is not known: with only 1 uF a radio burst falls 8x faster, and it must still be held
    // well above the brownout, and the sleep current still come out right
    board_t b = base();
    b.c_f = 1e-6;
    b.noise_mv = 8;
    b.tx_a = 25e-3;
    b.tx_s = 5e-3;
    b.tx_every_s = 30;
    rig_t r;
    simulate(&b, &r, 120);
    double ua = rig_sleep_na(&r) / 1000.0;
    printf("1 uF board: sleep %.3f uA (true 1.500), holds %u, min %.3f V\n", ua, r.holds, b.v_min);
    CHECK(b.v_min > 1.95);
    CHECK(fabs(ua - 1.5) < 0.08);
}

static void test_adc_spikes_are_not_wakes(void)
{
    // the classic ESP32's ADC gives a stray low reading now and then (Wi-Fi on): not a wake, not in the averages
    board_t b = base();
    b.noise_mv = 8;
    b.spike_every = 2000;
    b.spike_mv = 200;
    rig_t r;
    simulate(&b, &r, 120);
    double ua = rig_sleep_na(&r) / 1000.0;
    printf("ADC spikes: sleep %.3f uA (true 1.500), wakes %u\n", ua, r.wakes);
    CHECK(r.wakes == 0);  // a spike below v_low only starts a refill early: one pulse, no wake
    CHECK(fabs(ua - 1.5) < 0.05);
}

static void test_heavy_adc_noise(void)
{
    // the LOLIN32's ADC on hardware: much noisier than the datasheet suggests (2026-11-01, ~1000 readings/s more than
    // 40 mV from their neighbour). The medians still find the slope
    board_t b = base();
    b.noise_mv = 30;
    rig_t r;
    simulate(&b, &r, 300);
    double ua = rig_sleep_na(&r) / 1000.0;
    printf("+-30 mV ADC noise: sleep %.3f uA (true 1.500), %u decays, wakes %u\n", ua, r.quiet_segments, r.wakes);
    CHECK(fabs(ua - 1.5) < 0.1);
}

int main(void)
{
    test_sleep_only();
    test_noisy_adc_and_cpu_wakes();
    test_radio_bursts_are_held();
    test_capacitance_scales();
    test_small_capacitance_stays_safe();
    test_adc_spikes_are_not_wakes();
    test_heavy_adc_noise();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("rig: all passed\n");
    return 0;
}

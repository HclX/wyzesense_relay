#include "rig.h"

#include <string.h>

void rig_default_cfg(rig_cfg_t *cfg)
{
    cfg->v_low_mv = 2950;
    cfg->v_high_mv = 3000;
    // a wake shows as the charge it draws (many pulses to refill); low readings alone mislead on the LOLIN32, whose
    // ADC its own Wi-Fi disturbs every ~1.7 s (2026-11-01: "wakes" every 1.72 s that needed no extra pulses)
    cfg->v_busy_mv = 2650;
    cfg->busy_pulses = 6;   // a routine refill takes ~2 (B15C + 777B42DF)
    cfg->v_hold_mv = 2600;  // well above the CC1310's ~1.8 V brownout and the 2.5 V NV write guard
    cfg->pulse_us = 10;
    cfg->hold_us = 100;
    cfg->v_release_mv = 3150;
    cfg->cap_nf = 10000;  // a guess (docs/design/low-battery.md: ~5-10 µF on the board) until calibrated
}

void rig_start(rig_t *r, const rig_cfg_t *cfg)
{
    memset(r, 0, sizeof *r);
    r->cfg = *cfg;
}

static void wake_begins(rig_t *r, uint64_t start_us)
{
    if (r->wakes) {
        uint64_t gap = start_us - r->last_wake_us;
        if (!r->wake_gaps || gap < r->wake_gap_min_us) {
            r->wake_gap_min_us = gap;
        }
        if (gap > r->wake_gap_max_us) {
            r->wake_gap_max_us = gap;
        }
        r->wake_gap_sum_us += gap;
        r->wake_gaps++;
    }
    r->last_wake_us = start_us;
}

static void refill_begin(rig_t *r, uint64_t now_us)
{
    r->refilling = true;
    r->refill_t0_us = now_us;
    r->refill_pulses = 0;
    r->refill_busy = false;
}

static void decay_reading(rig_t *r, uint64_t now_us, uint16_t v_mv)
{
    if (r->top_n < RIG_AVG) {
        r->top_mv[r->top_n++] = v_mv;
        r->top_t_sum += (int64_t)(now_us - r->t0_us);
        return;
    }
    r->ring_mv[r->ring_at] = v_mv;
    r->ring_t[r->ring_at] = now_us - r->t0_us;
    r->ring_at = (r->ring_at + 1) % RIG_AVG;
    if (r->ring_n < RIG_AVG) {
        r->ring_n++;
    }
}

static int32_t median_uv(const uint16_t *mv)  // of RIG_AVG readings, in µV (the mean of the middle two)
{
    uint16_t v[RIG_AVG];
    for (int k = 0; k < RIG_AVG; k++) {
        v[k] = mv[k];
    }
    for (int a = 1; a < RIG_AVG; a++) {  // insertion sort: 16 values
        uint16_t x = v[a];
        int b = a - 1;
        for (; b >= 0 && v[b] > x; b--) {
            v[b + 1] = v[b];
        }
        v[b + 1] = x;
    }
    return ((int32_t)v[RIG_AVG / 2 - 1] + v[RIG_AVG / 2]) * 500;
}

static void decay_close(rig_t *r)
{
    if (r->decay_open && !r->decay_dirty && r->top_n == RIG_AVG && r->ring_n == RIG_AVG) {
        int64_t bot_t = 0;
        for (int k = 0; k < RIG_AVG; k++) {
            bot_t += (int64_t)r->ring_t[k];
        }
        int64_t dt = (bot_t - r->top_t_sum) / RIG_AVG;
        if (dt > 0) {
            r->quiet_fall_uv += median_uv(r->top_mv) - median_uv(r->ring_mv);
            r->quiet_time_us += (uint64_t)dt;
            r->quiet_segments++;
        }
    }
    r->decay_open = false;
}

rig_action_t rig_step(rig_t *r, uint64_t now_us, uint16_t v_mv, bool driven)
{
    if (!r->started) {
        r->started = true;
        r->t0_us = now_us;
        r->v_first_mv = r->v_min_mv = r->v_max_mv = v_mv;
    }
    r->now_us = now_us;
    r->v_mv = v_mv;
    if (r->holding) {  // a driven reading: the load, not the capacitor
        r->hold_time_us += r->cfg.hold_us;
        if (driven && v_mv < r->cfg.v_release_mv) {
            return (rig_action_t){RIG_HOLD, r->cfg.hold_us};
        }
        r->holding = false;  // the burst is over: let it fall to v_low; the next refill starts a clean decay
        r->refilling = false;
        wake_begins(r, r->refill_t0_us);
        r->wakes++;
        r->wake_time_us += now_us - r->refill_t0_us;
        return (rig_action_t){RIG_WAIT, 0};
    }
    if (v_mv < r->v_min_mv) {
        r->v_min_mv = v_mv;
    }
    if (v_mv > r->v_max_mv) {
        r->v_max_mv = v_mv;
    }
    if (v_mv < r->cfg.v_hold_mv) {  // falling fast: keep the board alive
        r->decay_dirty = true;
        decay_close(r);
        if (!r->refilling) {
            refill_begin(r, now_us);
        }
        r->holding = true;
        r->holds++;
        return (rig_action_t){RIG_HOLD, r->cfg.hold_us};
    }
    r->low_run = v_mv < r->cfg.v_busy_mv ? r->low_run + 1 : 0;
    if (r->refilling) {
        if (r->low_run >= 2 || r->refill_pulses > r->cfg.busy_pulses) {
            r->refill_busy = true;
        }
        if (v_mv >= r->cfg.v_high_mv) {  // a quiet decay starts
            r->refilling = false;
            r->refills++;
            if (r->refill_busy) {
                wake_begins(r, r->refill_t0_us);
                r->wakes++;
                r->wake_time_us += now_us - r->refill_t0_us;
            }
            r->decay_open = true;
            r->decay_dirty = false;
            r->top_n = r->ring_n = r->ring_at = 0;
            r->top_t_sum = 0;
            return (rig_action_t){RIG_WAIT, 0};
        }
        r->pulses++;
        r->refill_pulses++;
        return (rig_action_t){RIG_PULSE, r->cfg.pulse_us};
    }
    if (v_mv <= r->cfg.v_low_mv) {  // a refill starts: the decay before it closes (without this reading)
        decay_close(r);
        refill_begin(r, now_us);
        r->pulses++;
        r->refill_pulses++;
        return (rig_action_t){RIG_PULSE, r->cfg.pulse_us};
    }
    if (r->decay_open) {
        decay_reading(r, now_us, v_mv);
    }
    return (rig_action_t){RIG_WAIT, 0};
}

static int64_t nc(const rig_t *r, int64_t uv)
{
    return uv * (int64_t)r->cfg.cap_nf / 1000000;  // nF × µV = fC
}

int64_t rig_sleep_na(const rig_t *r)
{
    if (!r->quiet_time_us) {
        return -1;
    }
    return nc(r, r->quiet_fall_uv) * 1000000 / (int64_t)r->quiet_time_us;
}

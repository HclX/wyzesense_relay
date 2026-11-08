// The battery rig (#95, docs/design/low-battery.md §3, Option B): an ESP32 GPIO is the contact board's only supply, with the
// board's own decoupling capacitance (C) as the battery. The rig reads the supply voltage; when it falls to v_low it
// gives short pulses until v_high, and lets it fall again. While the sensor sleeps, the fall between two refills is
// slow and clean: sleep current = C × fall / time, from voltage readings alone (each end the mean of RIG_AVG readings,
// so ADC noise and spikes drop out). That is the figure no meter-less method gave us (#95).
// What it cannot measure: the charge of a wake. C (a few µF) holds far less than a CPU wake (mA for ms) or a radio
// burst takes, so those are fed by the GPIO directly. The rig counts and times them instead: a wake is a refill that
// had to fight a fall (a reading well below v_low, or many pulses); a fall below v_hold (a radio burst) makes it hold
// the supply on, reading it all the while with the supply driven (3.3 V less the load's current through the GPIO),
// and let go only when that shows the burst over, so it never leaves a small C alone mid-burst; it counts that. Their charge comes from the model and the firmware's counters.
// Portable: the ESP-IDF driver (esp32/idf/main/rig_task.c) feeds it readings and does what it says; host tests
// (esp32/host/test_rig.c) feed it a simulated board.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define RIG_AVG 16  // readings at each end of a quiet decay: their median (robust to the classic ESP32's ADC spikes)

typedef struct {
    uint16_t v_low_mv, v_high_mv;  // refill window
    uint16_t v_busy_mv;            // a refill with a reading below this, or more than busy_pulses pulses, was a wake
    uint32_t busy_pulses;
    uint16_t v_hold_mv;     // below this: hold the supply on (brownout guard)
    uint32_t pulse_us;      // one refill pulse
    uint32_t hold_us;       // between readings while holding (taken with the supply still on)
    uint16_t v_release_mv;  // a hold ends when the driven supply reads above this (the burst is over)
    uint32_t cap_nf;        // the board's capacitance, for charges in nC (default a guess: calibrate)
} rig_cfg_t;

typedef enum { RIG_WAIT, RIG_PULSE, RIG_HOLD } rig_kind_t;

typedef struct {
    rig_kind_t kind;
    uint32_t us;  // for RIG_PULSE / RIG_HOLD: how long to drive the supply
} rig_action_t;

typedef struct {
    rig_cfg_t cfg;
    bool started;
    uint64_t t0_us, now_us;
    uint16_t v_first_mv, v_mv, v_min_mv, v_max_mv;
    bool refilling, holding;
    // totals since rig_start
    uint32_t pulses, refills, holds;
    uint64_t hold_time_us;  // time spent holding
    uint32_t wakes;         // refills that fought a fall (or holds that began one)
    uint64_t wake_time_us;  // their length
    uint64_t last_wake_us;  // start of the last wake, and the gaps between wakes' starts
    uint64_t wake_gap_min_us, wake_gap_max_us, wake_gap_sum_us;
    uint32_t wake_gaps;
    uint32_t low_run;  // readings in a row below v_busy (one alone is an ADC spike, not a wake)

    uint64_t refill_t0_us;
    uint32_t refill_pulses;
    bool refill_busy;
    // quiet decays: from a refill's end to the next refill's start, no hold in between. Their fall, from the median of
    // the RIG_AVG readings just after the end to the median of the RIG_AVG just before the start (the readings that
    // crossed a threshold left out: chosen by their noise, they would bias the result), over the time between the
    // mean times of the two groups.
    bool decay_open, decay_dirty;
    uint32_t top_n;
    uint16_t top_mv[RIG_AVG];
    int64_t top_t_sum;
    uint32_t ring_n, ring_at;
    uint16_t ring_mv[RIG_AVG];
    uint64_t ring_t[RIG_AVG];
    int64_t quiet_fall_uv;
    uint64_t quiet_time_us;
    uint32_t quiet_segments;
} rig_t;

void rig_default_cfg(rig_cfg_t *cfg);
void rig_start(rig_t *r, const rig_cfg_t *cfg);
// One reading of the supply (mV) at now_us; returns what to do now. After a PULSE the supply is released and read;
// after a HOLD it stays driven and the reading is taken that way (driven = true), until the rig returns WAIT.
rig_action_t rig_step(rig_t *r, uint64_t now_us, uint16_t v_mv, bool driven);

// The sleep current in nA (C × falls / time; -1 until a quiet decay closed). It scales with cfg.cap_nf.
int64_t rig_sleep_na(const rig_t *r);

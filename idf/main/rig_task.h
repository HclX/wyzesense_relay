// The battery rig on a classic ESP32 (#95, esp32/rig/rig.h): one GPIO (OWS_PIN_RIG_SUPPLY, ADC1-capable)
// is the contact board's supply. Off unless enabled in the settings (settings.rig): then the GPIO supplies the board
// from boot ("supply"), and on request the rig measures ("measure").
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "rig.h"
#include "pins.h"

void rig_task_start(void);                             // supplies the board at once when the rig is enabled
bool rig_task_available(void);                         // enabled and built with a supply GPIO
bool rig_task_measure(bool on, const rig_cfg_t *cfg);  // start (fresh counters, cfg or the defaults) / stop measuring
void rig_task_cfg(rig_cfg_t *cfg);                     // the configuration in use (or the defaults)
size_t rig_task_json(char *out, size_t cap);           // the state and results, for /api/rig

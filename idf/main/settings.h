// The relay's settings, kept in NVS (namespace "ows"): set from the setup page (web.c).
#pragma once
#include "pins.h"

#include <stdbool.h>
#include <stdint.h>

#define ROLE_RELAY   0  // the relay, its contact the gateway (the default)
#define ROLE_STATION 2  // the provisioning station, on the same harness (#81; 1 was "relay only", read as relay)

typedef struct {
    char ssid[33], pass[65];  // Wi-Fi; an empty SSID: not set up yet (the setup access point)
    char url[128];            // wyzesense2mqtt-rs's bridge: ws://host:port/ws/bridge
    char token[64];           // its bridge token, or empty
    char admin_pass[64];
    char wire_r[16],
        wire_t
            [16];  // the colours of the harness's R and T wires (the station's hand probes), as named      // HTTP admin password for mutating endpoints (falls back to token when empty)
    bool skip_gwsync;       // pause startup CC1310 auto-reflash (safe mode / custom radio builds)
    bool rig;               // the battery rig (#95): OWS_PIN_RIG_SUPPLY supplies the contact (rig_task.c)
    uint8_t contact_flash;  // how gwsync reflashes the contact (#81): GWSYNC_VIA_AUTO / _CJTAG / _SBL
    uint8_t role;           // ROLE_RELAY or ROLE_STATION: mutually exclusive
} settings_t;

extern settings_t settings;

void settings_load(void);
bool settings_save(void);

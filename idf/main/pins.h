#pragma once
// The relay's GPIOs, fixed per chip (#81: the user, 2026-11-06: fixed assignments, for simplicity; the wiring tables in
// esp32/README.md and docs/guide follow them). The contact's pads by their silkscreen: R is its input and ROM
// bootloader backdoor (DIO1), T its output (DIO0), M/C its 2-pin debug port (cJTAG TMSC/TCKC), S its RESET_N, + its
// supply.
// Left alone on purpose: the flash pins (ESP32 6-11, S3 26-32), the S3's USB (19/20), the consoles (ESP32 1/3, S3
// 43/44), and the boot straps that matter (ESP32 0, 5, 12, 15; S3 0, 3, 45, 46). The supply pin is low or floating
// through the ESP's own start, so nothing is powered before the firmware decides to.

#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32       // LOLIN32 / classic ESP32
#define OWS_PIN_CONTACT_POWER 27  // + (a reset is a power cycle); unwired (+ on 3V3) it does nothing
#define OWS_PIN_CONTACT_TX    17  // R, UART1 TX (and ROM bootloader backdoor, DIO1 active low)
#define OWS_PIN_CONTACT_RX    18  // T, UART1 RX
#define OWS_PIN_CONTACT_TMS   25  // M
#define OWS_PIN_CONTACT_TCK   26  // C
#define OWS_PIN_CONTACT_RESET 4   // S, open-drain (GPIO4 pulls down at the ESP's reset: S low until we start)
#define OWS_PIN_CONTACT_BOOT  (-1)
#define OWS_PIN_RIG_SUPPLY    32  // the battery rig's supply (#95): ADC1 channel 4 (ADC2 does not work with Wi-Fi)
#elif CONFIG_IDF_TARGET_ESP32S3
#define OWS_PIN_CONTACT_POWER 7
#define OWS_PIN_CONTACT_TX    17
#define OWS_PIN_CONTACT_RX    18
#define OWS_PIN_CONTACT_TMS   15
#define OWS_PIN_CONTACT_TCK   16
#define OWS_PIN_CONTACT_RESET 4
#define OWS_PIN_CONTACT_BOOT  (-1)
#define OWS_PIN_RIG_SUPPLY    (-1)  // the battery rig is on the classic ESP32 only
#else
#error "pins.h: no pin map for this chip"
#endif

#define OWS_PIN_STATUS_LED (-1)  // no status LED

// The provisioning station (role station, mutually exclusive with the relay: the user, 2026-11-06/07): four wires,
// the sensor's + and - on clips (the contact's supply pin and GND) and two hand-held probes, the harness's R and T
// wires, used for every mode: cJTAG (M/C), the bootloader's T/R, and the experimental W/S check.
#define OWS_PIN_STATION_PROBE1 OWS_PIN_CONTACT_TX
#define OWS_PIN_STATION_PROBE2 OWS_PIN_CONTACT_RX
#define OWS_PIN_STATION_A      OWS_PIN_STATION_PROBE1  // cJTAG (M or C, polarity found)
#define OWS_PIN_STATION_B      OWS_PIN_STATION_PROBE2
#define OWS_PIN_STATION_SBL_0  OWS_PIN_STATION_PROBE1  // the bootloader's R/T (sbl2), or W/S
#define OWS_PIN_STATION_SBL_1  OWS_PIN_STATION_PROBE2
#define OWS_PIN_STATION_SBL_2  (-1)
#define OWS_PIN_STATION_SBL_3  (-1)
#define OWS_PIN_STATION_POWER  OWS_PIN_CONTACT_POWER

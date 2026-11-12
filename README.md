# ESP32 relay (issue #22)

An ESP32-S3 next to a v1 contact board running our gateway (`gateway-contact-v1`) relays the gateway's host link
(its RX/TX pads, 115200) to wyzesense2mqtt-rs's `/ws/bridge` over Wi-Fi: an IP gateway. The bytes go as they are,
and wyzesense2mqtt-rs does pairing, Home Assistant and the rest; several relays (one per contact board, each with its
own MAC) connect to one wyzesense2mqtt-rs. While Wi-Fi or wyzesense2mqtt-rs is down, the gateway's alarms are kept
(up to 32) and sent on when it is back. The ESP32 can also reset and reflash its contact board.

| Path | What |
|---|---|
| `relay/` | the relay core, portable C with no I/O of its own: the WebSocket client (handshake, framing, client/server ping/pong, close), reconnects, the relay; `sock_posix.c`: its TCP link on BSD sockets (the PC and lwIP); `buffer.c`: the alarms kept through an outage (strict FIFO ordering, `1970` timestamp rebasing, remote SBL filtering; host protocol read with `firmware/common/host_proto.c`); `gwnv.c`: zero-ladder NV JSON import/export over `0x53F0` sub-IDs `0x04`/`0x06` |
| `contact/` | the contact's ROM bootloader (`sbl.c`, including `sbl_stream_*` page-by-page streaming), 2-pin cJTAG debug port (`cjtag.c`), dedicated stock gateway partition packer/verifier (`stock_image.c`), and startup sync (`gwsync.c`): tries 4-wire software SBL (`0x53F0` sub-ID `0x05` `OWS_SUB_ENTER_SBL`) first and falls back to hardware `R` + `S`; enforces the rules of `tools/uart_sbl.py` (pages 0-29, page 30 only on request, page 31 never except page 31 code via `cjtag_write_page31_code`, CCFG checked first) |
| `web/` | the shared Web UI (`index.html`), streaming `multipart/form-data` parser (`multipart.c`), HTTP Basic/Bearer auth (`http_auth.c`), and mDNS packet responder (`mdns.c`) |
| `host/` | the cores on a PC: `ows-relay` (the counterpart of `tools/ws_bridge.py`), `ows-sbl` (of `tools/uart_sbl.py`), `ows-gwsync`, and the host test binaries (`test_relay`, `test_buffer`, `test_cjtag`, `test_sblprobe`, `test_gwnv`, `test_stock_image`, `test_multipart`, `test_http_auth`, `test_mdns`, `test_rig`) |
| `idf/` | the ESP-IDF firmware for the ESP32-S3 and classic ESP32 (`tools/idf.sh build`) |
| `esp8266/` | the ESP8266 version (Wemos D1 mini / NodeMCU): an Arduino shell around the same relay, buffer, `gwnv`, `http_auth`, and streaming `sbl_stream_*` (`tools/pio.sh run`) |

## Tested without hardware

- `ctest --preset host`: all 6 `esp32/host` test binaries — `test_relay` (`relay`), `test_buffer` (`relay_buffer`),
  `test_gwnv` (`gwnv`), `test_multipart` (`multipart`), `test_http_auth` (`http_auth`), and `test_mdns` (`mdns`).
- `reference/tests/test_host_e2e.py`: wyzesense2mqtt-rs, a host gateway and two host sensors with `ows-relay`
  (the relay code the ESP32 runs): pairing, reports, restarts, and an event made while wyzesense2mqtt-rs is
  stopped reaching MQTT once it is back.
- `reference/tests/test_ows_sbl.py` and `reference/tests/test_gwsync.py`: `ows-sbl` and `ows-gwsync` on the simulated
  ROM and gateway.
- CI's `esp32` and `esp8266` jobs build the firmware in `espressif/idf:v5.5.6` and PlatformIO (versions pinned in
  `tools/toolchain.lock`).

Hardware bench test: `reference/tests/test_hw_esp32.py`, with the board wired as below.

## Wiring

The GPIOs are fixed per chip (`esp32/idf/main/pins.h`; the user, 2026-11-06).

| Contact pad | ESP32-S3 |
|---|---|
| `+` | GPIO7 (#81: a reset is a power cycle); or 3V3, as before |
| `-` | GND |
| `RX` (DIO1) | GPIO17 (UART1 TX) |
| `TX` (DIO0) | GPIO18 (UART1 RX) |
| `M` (TMSC) | GPIO15 (#81: updates through the debug port) |
| `C` (TCKC) | GPIO16 |
| `S` (RESET_N) | GPIO4, open-drain; optional when `+` is on GPIO7 |

Since #81 the relay powers the contact from a GPIO and updates it through its 2-pin debug port (cJTAG on `M`/`C`),
like the LaunchPad does: no bootloader backdoor needed. Setup page "Contact updates through" (`auto`, the default:
the debug port, else the ROM bootloader; `cjtag`; `sbl`). Into the bootloader, `R` (DIO1, the stock backdoor
`0xC5FE01C5`) is held low across the start. While the power is off, the relay drives no line high (`R` low, `T`'s
pull-up off, `S` released), so nothing feeds the chip through its pins. A relay wired with `+` on 3V3 and `S` keeps
working: the power GPIO then does nothing and a debug port that does not answer falls back to the bootloader. Tried on
the LOLIN32 (docs/TODO.md, bench checklist).

### Relay or provisioning station

One image, two roles, chosen on the setup page ("Device role"; one at a time, the ESP restarts to switch): the
**relay** (the default) or the **provisioning station**, which dumps and flashes other sensors (no relay meanwhile).
The station has four wires (the user, 2026-11-07): the sensor's `+` (the supply pin, switched) and `−` on clips at its
battery holder (battery out), and two hand-held probes, the harness's `R` and `T` wires (probe 1, probe 2; colours
set on the Probe tab), used for every mode:
- **cJTAG**: the probes on `M` and `C` (polarity found).
- **Bootloader T/R** (`sbl2`): the probes on its TX and RX pads, either way round. The station tries only once both
  probes have been on the sensor for 200 ms (a 20 us pull-up pulse on each, the sensor unpowered with its supply
  driven low: low on a pad, high in the air), holds the candidate RX (the stock backdoor, DIO1) or the other pad low
  across a power-up, and listens for the bootloader; after a try that found nothing, it waits for the probes to be
  lifted.
- **W/S** (after T/R): probe 1 on `T` (or `R`), probe 2 on one of the two other pads. The sensor is powered up with
  that pad released, then held low, twice each: if holding it kept probe 1's pad dead (the chip held in reset), it is
  `S`, otherwise `W`. Needs a firmware that drives `T` (or pulls `R` up) while it runs, else "can't tell". It keeps the
  T/R identification.

Probe tab, two steps: **Identify** with one of those modes (the pins found and confirmed, the sensor let go; nothing
read but what identifies it, nothing written), then **Dump** or **Flash**, on the identified pins only (refused
before an identification). A dump of the unit, saved, before any flash of it; it stays on the station until the next
Dump, staged image or Disarm. Pages 0-29 (30 only for an NV wipe), every page verified, page 31 never.

Provision the contact board once over JTAG (`docs/design/v1-contact.md`), backing up its stock flash first:
`tools/uniflash.py provision --gateway --backup backups/contact_<MAC>` (or step by step:
`tools/uniflash.py backup backups/contact_<MAC>`,
`tools/uniflash.py flash --backup backups/contact_<MAC> build/fw/firmware/gateway-contact-v1-app.bin`, and
`tools/uniflash.py enable-bootloader r --backup backups/contact_<MAC> --yes`). Later updates come from the ESP itself
(trying 4-wire software SBL `OWS_SUB_ENTER_SBL` over `RX`/`TX` first, and falling back to `R` + `S` hardware entry).

## First flash and setup

```
tools/toolchain.sh esp-idf          # once: ESP-IDF v5.5.6 and the S3 tools into .toolchain (~6 GB)
tools/idf.sh build
tools/idf.sh -p /dev/ttyACM0 flash monitor
```

When unprovisioned (`ssid` empty), it starts the setup access point `OWS-relay-XXXX`: join it, open
http://192.168.4.1/setup, enter the Wi-Fi network, its password, the bridge
(`ws://<wyzesense2mqtt-rs host>:8080/ws/bridge`), its WebSocket token (passed via `?device=<name>&token=<token>`), and an
optional HTTP `admin_pass`; it restarts onto that network, tears down `OWS-relay-XXXX` as soon as STA obtains an IP
(`GOT_IP`), and advertises `http://ows-relay-xxxx.local/` via mDNS. wyzesense2mqtt-rs needs `bridge.enabled: true`.

## Use

**One update for both (#76).** A relay's firmware carries the gateway firmware for its contact
(`gateway-contact-v1-app.bin` from the `fw` build, embedded at build time by `esp32/gateway_image.py`). At every
start (unless paused with `skip=1`), before it relays anything, the relay asks the contact for its build info block
(host command `0x53F0` sub-ID `0x02` `OWS_SUB_BUILD_INFO`, `part = 0`) and MAC (`0x4304`) and compares the image IDs
(`esp32/contact/gwsync.c`). If they differ, or the contact does not answer, it enters the contact's ROM bootloader
(software `OWS_SUB_ENTER_SBL` first, then `R` + `S` hardware fallback) and streams pages 0-29 via `sbl_stream_*`, so the
paired sensors (page 30) and MAC (page 31) stay. When the contact is silent, the bootloader's CRC32 first tells whether
the image is already there. The ID is the hash of the code (`tools/image_id.py`), so a relay update that does not change
the gateway code does not reflash it. An OTA rollback puts the matching gateway back only if its code differs. So
updating the relay (`/ota`, or the page's upload) is the whole update. Measured on the NodeMCU: 32 s from the upload to
relaying with a reflash, 8 s without.

```
curl http://<ip>/api/status                                 # status (JSON): relay, gwsync, bridge, sensors
curl -u admin:<pass> -X POST http://<ip>/contact/reset
curl -u admin:<pass> -X POST 'http://<ip>/contact/gwsync?skip=1'          # pause startup auto-sync (skip=0 resumes)
curl -u admin:<pass> http://<ip>/contact/nv > nv.json                     # export network + paired sensors
curl -u admin:<pass> --data-binary @nv.json http://<ip>/contact/nv        # zero-ladder NV import
uv run python tools/contact_image.py BOOT.bin SLOT_A.img contact.bin      # a sensor image (selector + slot A)
curl -u admin:<pass> --data-binary @build/fw/firmware/gateway-contact-v1-app.bin \
  'http://<ip>/contact/flash?mac=777B42DF&nv=wipe&keep=1'
curl -u admin:<pass> -F image=@FLASH.bin http://<ip>/contact/stock        # store stock v1.8 gateway in flash partition
curl -u admin:<pass> --data-binary @esp32/idf/build/ows_relay.bin http://<ip>/ota   # this firmware; it restarts
```

`/contact/flash` streams pages 0-29 directly to the contact via `sbl_stream_*` on both ESP-IDF and ESP8266 (zero 120 KB
RAM buffer): it takes the UART from the relay, enters the bootloader (`OWS_SUB_ENTER_SBL` or `R` + `S`), checks its CCFG
(a valid MAC, matching `?mac=` if given, bootloader enabled), streams pages 0-29 and checks the chip's CRC32, wipes page
30 when `?nv=wipe` is set, optionally sets `skip_gwsync` when `?keep=1` is passed (so the next reboot does not overwrite
a custom bench image with the embedded gateway), resets into the new firmware, and gives the UART back. `/ota` writes the
other app slot; on ESP-IDF the new firmware stays only once it reaches the network again, otherwise the next reset rolls
back.

### Stock Wyze v1.8 gateway firmware (dedicated flash partition)

Stock Wyze firmware is never embedded in any relay app binary. Instead, the relay stores it in a dedicated flash area
separate from the app (`gwstock` data partition at `0x320000`, size `0x20000` in `idf/partitions.csv` on ESP32 /
ESP32-S3; the unused `_FS_start` filesystem area at `0x300000` on 4 MB ESP8266 D1 mini builds), surviving `/ota` app
updates until a full USB flash erase.

- **Stored format (`contact/stock_image.h`)**: 48-byte header (`magic = "OWSSTOCK"`, `format = 1`, `len = 0x1EFA8`,
  and the 32-byte SHA-256) followed by the `0x1EFA8`-byte (126,888 B) packed image: CC1310 pages 0–29 (`0x1E000` B,
  including pages 24–29 which are 24 KB of `0x00` in stock v1.8) plus page 31's code (`0x1F000..0x1FFA7`, `0xFA8` B).
  Page 30 (per-dongle NV) and the 88-byte CCFG (`0x1FFA8..0x1FFFF`) are excluded.
- **Upload in the field (`POST /contact/stock` or the Update tab's "Stock gateway firmware" card)**: accepts either a
  full 128 KB (`0x20000`) dongle flash dump (`FLASH.bin` or `flash.bin` from `ows-flash backup`, packed on the fly) or
  the packed `0x1EFA8`-byte image. The relay verifies that the packed image's SHA-256 equals the known v1.8 digest
  `ab9c235bbce832a7dc74d67092199ba4108d8d24a33209c781bf483b85684ea0` before writing the partition, then reads it back
  and verifies it. `GET /api/status` reports `"stock_stored": true|false` (the relay never serves the stored image
  back). Once stored, select **Stock Wyze v1.8 (dongle gateway)** in Setup (`gw_fw=stock`); at startup `gwsync` writes
  pages 0–29 and page 31's code through the debug port (`cJTAG` on `M`/`C`). Existing ESP32 / ESP32-S3 relays built
  before the `gwstock` partition was added need a one-time full USB flash so their partition table includes `gwstock`.
- **Local pre-population (`tools/stock_partition.py`)**: build the partition image on a PC and flash it directly with
  `esptool.py`:

```bash
uv run python tools/stock_partition.py FLASH.bin build/gwstock.bin
# ESP32 / ESP32-S3 (gwstock partition at 0x320000 in idf/partitions.csv):
esptool.py write_flash 0x320000 build/gwstock.bin
# ESP8266 4 MB Wemos D1 mini (_FS_start = 0x40500000 - 0x40200000 = 0x300000 in eagle.flash.4m1m.ld):
esptool.py write_flash 0x300000 build/gwstock.bin
```

## Security and limits

- **Authentication (`esp32/web/http_auth.c`)**: Once `admin_pass` (or WebSocket `token`) is configured, all mutating HTTP
  endpoints (`POST /setup`, `POST /wires`, `POST /ota`, `POST /contact/reset`, `POST /contact/flash`,
  `POST /contact/stock`, `POST /contact/gwsync`, `POST /contact/nv`, and `#ifdef OWS_DEBUG_UART` `/debug/uart`) require
  HTTP Basic (`-u admin:<pass>`) or `Authorization: Bearer <pass>`. `GET /setup` never echoes saved Wi-Fi/token/admin
  secrets.
- **Fallback SoftAP lifecycle**: When `ssid` is configured and Wi-Fi drops (e.g., router reboot), the relay retries STA
  with backoff (`~90–120 s`) before opening `OWS-relay-XXXX`, tears down the SoftAP immediately (`WIFI_MODE_STA` /
  `WiFi.softAPdisconnect(true)`) as soon as STA obtains an IP (`GOT_IP`), and closes an idle fallback SoftAP after 15
  minutes while continuing STA reconnects.
- **Outage buffer**: Through an outage only `0x5319` alarms are kept, at most 32 in RAM (an ESP restart loses them),
  replayed in strict chronological FIFO order (with cold-boot `1970` timestamps rebased on `0x5333` time sync); a
  pairing or host command needs the live connection. An alarm the server got just before the connection dropped may come
  twice.
- Plain `ws://` / `http://` on the home LAN (no TLS).

## Classic ESP32 (LOLIN32)

The same firmware builds for the classic ESP32 (ESP32-WROOM-32, 4 MB), in its own build directory:

```
tools/idf.sh -B build-esp32 -D SDKCONFIG=build-esp32/sdkconfig -D IDF_TARGET=esp32 build
tools/idf.sh -B build-esp32 -D SDKCONFIG=build-esp32/sdkconfig -p /dev/ttyUSB0 flash monitor
```

| Contact pad | LOLIN32 |
|---|---|
| `+` | GPIO27; or 3V3 |
| `-` | GND |
| `R` (the contact's input) | GPIO17 (UART1 TX) |
| `T` (the contact's output) | GPIO18 (UART1 RX) |
| `M` (TMSC) | GPIO25 |
| `C` (TCKC) | GPIO26 |
| `S` (RESET_N) | GPIO4 (optional with `+` on GPIO27) |

Both relays pull their UART RX pin up: a contact that does not drive its T pad (a half-written image after an
interrupted flash) leaves the line floating, and the noise came in as bytes (`00 ff 00 fe …`, hundreds a second), which
kept the start-up check's waits from ever timing out (2026-10-25, the LOLIN32; reproduced by interrupting a flash, and
gone with the pull-up). On the classic ESP32, GPIO5 is a boot strap, and on a LOLIN32 it drives the blue
LED (the Lite's LED is on GPIO22). The other straps (GPIO0, 2, 12, 15) stay free too. GPIO12 above all: high at the
start, it sets the flash to 1.8 V. The console is UART0, the board's USB serial chip.

### Battery rig (classic ESP32, #95)

The LOLIN32 can be the contact's battery instead of its 3V3 pin, to measure the sensor's sleep current with no meter
(`docs/design/low-battery.md` §3, Option B; core `esp32/rig/`, tests `test_rig`). Move one wire: the contact's `+` from
**3V3** to **GPIO32** (ADC1 channel 4: ADC2 pins do not work with Wi-Fi on). Then:

```
curl -d enable=1 http://<ip>/rig            # kept in the settings; restarts; GPIO32 supplies the contact from boot
curl -d measure=1 http://<ip>/rig           # fresh counters (optional: cap_nf=, v_low_mv=, v_high_mv=, pulse_us=, ...)
curl http://<ip>/api/rig                    # sleep_na, wakes, holds, voltages
curl -d measure=0 http://<ip>/rig           # back to a steady supply
curl -d enable=0 http://<ip>/rig            # off (move + back to 3V3 first)
```

How it measures: the GPIO lets the supply fall from 3.00 V to 2.95 V on the board's own decoupling capacitance (C),
then tops it up in 10 µs pulses. Sleep current = C × fall / time, each end the mean of 16 readings. A CPU wake or a
radio burst draws far more than C holds, so the GPIO feeds those directly: they are counted and timed (`wakes`,
`holds`), not measured. While enabled, the contact's UART lines don't feed it (no pull-up on RX, TX open-drain).
The sensor's console, which the relay does not forward (only host-protocol packets reach the bridge):
`curl -d info 'http://<ip>/contact/console?wait_ms=1500'`. Every console byte keeps a battery sensor awake for 5
minutes (about 0.5 mA instead of under a µA): end that with `-d sleep` before measuring. A wake is judged by the charge it takes (`busy_pulses`: more than 6 pulses for one refill), not by low readings:
the LOLIN32's own Wi-Fi disturbs its ADC about every 1.7 s. Measured on B15C (our sensor on 777B42DF, unpaired, its
console window closed): ~300 nA asleep at the 10 µF guess, ~2.2 pulses per refill, and no wakes. `sleep_na` scales with `cap_nf`, a guess (10 µF) until calibrated, and the classic ESP32's ADC reads above ~2.45 V
with a gain error of its own: compare figures with each other (firmware A vs B) until a known load calibrates both.

## ESP8266 (D1 mini)

The same relay, outage buffer, `gwnv`, `http_auth`, and streaming `sbl_stream_*` in an Arduino shell (`esp32/esp8266`;
PlatformIO 6.2.0 through `tools/pio.sh`, the platform espressif8266 4.2.1 pinned in `platformio.ini`; CI's `esp8266` job
builds it). Verified on NodeMCU and D1 mini hardware (2026-10-26).

| Contact pad | D1 mini |
|---|---|
| `+` / `-` | 3V3 / G |
| `R` (silkscreen; the contact's input) | D4 (GPIO2: UART1 TX) |
| `T` (silkscreen; the contact's output) | D7 (GPIO13: UART0 RX after `Serial.swap()`) |
| `S` (RESET_N) | D2 (GPIO4) |
| `M` (TMSC) | D5 (GPIO14): the debug port, optional |
| `C` (TCKC) | D6 (GPIO12) |

Into the bootloader the relay holds `R` (D4, taken from UART1 meanwhile) low through an `S` reset, so a
unit with the stock backdoor (`R`/DIO1, `BL_CONFIG 0xC5FE01C5`) needs only five wires (#81, 2026-11-07). With `M` and
`C` wired, the start-up check updates the gateway through the debug port first (`esp32/contact/cjtag.c`, as the ESP32
does; 65 s for 10 pages through Arduino's `digitalWrite`, the bootloader fallback after it), and the bootloader's
backdoor can be read and set (`cjtag_set_backdoor`, under `docs/design/ccfg-rule.md`: page 31 read twice, only `BL_CONFIG`
changed, the page verified whole, put back on a failure; the unit's MAC must be given):

```
curl http://<ip>/contact/bootloader                                       # {"mac", "bl_config", "backdoor": r|w|button|off|other}
curl -u admin:<pass> -d 'backdoor=r&mac=<MAC>' http://<ip>/contact/bootloader  # r (DIO1, stock): the pin the relay drives
```

Newer v1 contacts keep their MAC only in page 29 (`0x1D000`), the CCFG MAC slot blank (77A9B885, 2026-11-07); the
relay and the station refuse to write such a unit until `curl -u admin:<pass> -d mac=<MAC> http://<ip>/contact/ccfg_mac`
fills the slot with that same MAC (`cjtag_fill_ccfg_mac`: a blank slot only, program only, no erase; the user's
decision, 2026-11-07).

Verified on a NodeMCU with 777B42DF, 2026-11-07: `BL_CONFIG` `c5fe0ac5` -> `c5fe01c5`, then a reflash through `R`
alone (our sensor firmware running, so no software entry), a gateway update through the debug port, and
`test_hw_esp32.py` (the relay, reset, `nv.json`, a reflash, OTA).

```
tools/pio.sh run -t upload --upload-port /dev/ttyUSB0      # its start-up report: 115200 on the USB port
curl -u admin:<pass> -F image=@esp32/esp8266/.pio/build/d1_mini/firmware.bin http://<ip>/ota
```

Its web page (`http://<ip>/`, the one both relays serve: `esp32/web/index.html`): status (refreshed every 2 s from
`GET /api/status`, including per-sensor `mac`, `type`, `seen_s`, `rssi`, `bat_mv`), radio maintenance (reset, `gwsync`
pause/resume, `nv.json` import/export, and custom radio flash), ESP OTA update, and setup. Both ESP-IDF and ESP8266
stream contact reflashes page by page via `sbl_stream_*` (`curl -u admin:<pass> -F image=@gateway-contact-v1-app.bin
"http://<ip>/contact/flash?mac=<MAC>&nv=wipe&keep=1"`), and both try 4-wire software SBL (`OWS_SUB_ENTER_SBL`) before
falling back to `R` + `S` hardware entry; a broken upload leaves pages 0-29 part written and the bootloader, CCFG, and
page 30 untouched, so flash again (wiring `S` is recommended so a half-written image can enter the
bootloader by hardware reset). OTA as a multipart upload (`curl -F`), settings in EEPROM (`GET /debug/uart` is compiled
only under `#ifdef OWS_DEBUG_UART`). Nothing goes on D8 (GPIO15): it must be low when the ESP8266
starts, and both of the contact's UART pads idle high (on a NodeMCU, 2026-10-25: either pad on D8 gave boot mode
(7,x), the SDIO mode, and no start), so the contact's input is driven by UART1's TX on D4 (GPIO2, high at start
as it must be; the ESP-12's blue LED flickers with the traffic). A software serial TX on D6 lost about one Inquiry
in five with Wi-Fi on; UART1 answered 100 of 100. So there is no log UART: the status page shows the last log line.
The pads' silkscreen is the contact's own side: T is its output, R its input (the handshake confirmed it).

## Bring-up on the bench (the first time on hardware)

1. The contact board: back up and provision our gateway + backdoor `r` (`tools/uniflash.py provision --gateway --backup
   backups/contact_<MAC>`, or `backup` -> `flash --backup ... build/fw/firmware/gateway-contact-v1-app.bin` ->
   `enable-bootloader r --backup ... --yes`); then unwire it from the LaunchPad.
2. Wire it to the S3 (the table above); the GPIOs are fixed (`pins.h`).
3. `tools/idf.sh -p /dev/ttyACM0 flash monitor`: the log shows `OpenWyzeSense relay OWS-relay-XXXX up` and the
   setup access point.
4. Join `OWS-relay-XXXX`, http://192.168.4.1/setup: Wi-Fi, the bridge URL, optional token / `admin_pass`. After its
   restart the SoftAP shuts down on `GOT_IP`, the log shows its IP (and `ows-relay-xxxx.local`) and `relay: connected`;
   wyzesense2mqtt-rs lists the contact's MAC as a dongle (transport `bridge`).
5. `OWS_HW=1 OWS_HW_ESP32=<ip> OWS_W2M=<wyzesense2mqtt-rs> [OWS_HW_ESP32_AUTH=admin:<pass>] [OWS_HW_TX=1] uv run pytest
   -s reference/tests/test_hw_esp32.py`: the relay, `/contact/nv`, the contact's reset and reflash, OTA.
6. By hand: pair a sensor through wyzesense2mqtt-rs; stop wyzesense2mqtt-rs, move the sensor, start it again: the
   event arrives (the status page counts it as kept and sent on).

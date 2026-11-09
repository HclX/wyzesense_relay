# The relay's task list

Moved from openwyzesense's `docs/TODO.md` (bench checklist) when the relay became its own repository (2026-11-08).
Tick with the date and the evidence; remove what gets done.

## Next

- **The first release**: `tools/release_bundle.py` and CI's `release` job (manifest.json in openwyzesense's format,
  the products `relay-esp32-ota`, `relay-esp32s3-ota`, `relay-esp8266` that wyzesense2mqtt-rs updates relays from,
  plus the first-flash images and `72-esp-usb-serial.rules`; any image with stock firmware refused) are in. Left: the
  user's tag (v*), and wyzesense2mqtt-rs (`ows`) taking relay images from this repository's releases instead of
  openwyzesense's before openwyzesense releases without them.
- **Stock gateway firmware on a contact board** (Setup: `gw_fw` `stock`): works on both relays (2026-11-08, with
  the stock-only wyzesense2mqtt-rs release v0.1.3): flashed through the debug port (ESP32 9A78 / 7779749A, ESP8266 9130
  / 77A9B885, ~2.5 min on the ESP8266), the handshake through the translation (Get MAC answered by the relay with the
  contact's CCFG MAC: the stock CC1310 answers zeros, also after Write MAC), scan on/off, and stock sensors paired and
  reporting: a contact v2 (77BD514D) via the ESP8266, the climate 77C68193 via the ESP32. Its networks: PAN 0x9C16 /
  coordinator 0x7F38 (77A9B885), 0x23C3 / 0x561B (7779749A). Open:
  - openwyzesense's LaunchPad sensor does not join it ("join failed": the permit beacon heard at -39 dBm, no answer
    to its association), though it joins a stock dongle; the stock sensors do.
  - After its first write the stock gateway answers the relay's check but not the host's handshake until a restart
    (all three relays: 9A78, 9130, B15C / 777B42DF). The relay now restarts it once more after a write; check that on
    hardware. Cause unknown (the first start reformatting our NV page 30?).
  - The step "written: asking the new gateway" also shows when nothing was written (stock, the debug port equal).
- **The host protocol** (`common/host_proto.*`) is a copy of openwyzesense's `firmware/common/host_proto.*`: keep the
  two in step (openwyzesense's #28 adds secured-link constants).

- **Reception and wiring**: a short-wired relay heard two side-by-side sensors 14-18 dB worse than a long-wired
  one (swapped to rule the sensors out; 9130 short vs B15C long, 2026-11-08). Not separated yet: the wire length vs
  ESP8266 vs ESP32 vs the board; the same ESP and board with short then long wires would settle it. The guide now
  says 20-30 cm. The VDDR boost (`POST /contact/vddr`, ESP32 only) is on for B15C / 777B42DF: its effect (transmit
  only) still to measure with the LaunchPad's beacon RSSI.

## Bench checklist

- `TASK-08` (relay half): a sensor OTA through an ESP32 and an ESP8266 relay with no poll timeouts; a relay whose
  server vanishes without a FIN goes down within ~40 s and buffers; `gwsync` and `/contact/flash` on all three ESP
  boards.
- `TASK-11` / `TASK-21` (relay half): a 4-wire relay (no `W`, no `S`) enters the bootloader in software, flashes and
  comes back through the gateway's ~15 s watchdog.
- **#81 relay wiring**: done on the LOLIN32 B15C with contact 777B42DF, 2026-11-06 (the user wired it; console logs
  of each run). `+` on GPIO27: starts and runs. Updates through `M`/`C`: ~20 s (four runs; the first build overflowed
  the relay task's stack, fixed). Unchanged code is left alone. Fallback with `M`/`C` off: bootloader by a power cycle
  with `R`/`W` low, ~10 s, with `S` and with `S` floating; the debug-port update with `S` floating too, so `S` is
  optional; `W` only for the fallback on units with that backdoor. Trap: an ESP32's GPIO4 pulls down at reset, so `S` wired to it but unconfigured holds the
  contact in reset. Station role on the same harness (fixed pins, 2026-11-06): a cJTAG dump of 777B42DF (powered from
  GPIO27, M/C polarity found, 32 pages, ~27 s, equal to the carried gateway image) and an SBL dump with the pad search
  (R/T/backdoor W found on GPIO17/18/23, chip ID 0x20018000), the two dumps byte-identical. 2026-11-07, Identify then
  Dump/Flash as separate steps: dump refused before Identify; per transport Identify, Dump (equal) and Flash of the
  running gateway image (10 pages verified, page 31 intact). `sbl2` (two hand probes on TX/RX, + and − clipped,
  2026-11-07): touch seen on the soldered 777B42DF, one round, then waiting for a lift (its backdoor is W, not on the
  probes). The leak sensor 77C685AE was dumped this way by the user (2026-11-07, `backups/`), so touch, lift and the
  DIO1 backdoor work on a real sensor; dumped again at 10:55 after a run with + and - swapped by mistake, so it
  survived that. Since: the station on four wires (two probes on GPIO17/18 for every mode) and the
  W/S check (probe 1 on T or R as the witness, probe 2's pad held low across a power-up: S keeps the chip in reset):
  on the leak sensor it called both remaining pads W (2026-11-07). Hypothesis to check: the board pulls its T line up,
  so the witness reads high even with the chip held in reset; a fix could compare the witness against the chip
  unpowered, or watch T for real UART activity instead of a level. Check on sensors whose pads are known; cJTAG
  on the two probes; a slip during an SBL dump.
  Left: a sensor that is not a v1 contact (the leak sensor), the user guide's level 4 and its wiring SVGs (`tools/wiring_svg.py`) on the new wiring; the
  ESP32-S3 (GPIO7, 15/16); a stock-backdoor unit (`R`) on the bootloader path; transmit bursts on GPIO power.

- `TASK-01` relays: the fallback access point closes as soon as Wi-Fi is back (watch it from a phone); with a
  password set, every changing endpoint answers 401 without it, on ESP-IDF and ESP8266, and `GET /setup` shows no
  secrets.
- `TASK-02` outage buffer: 5 magnet events with wyzesense2mqtt-rs stopped, 2 more while it replays: all 7 arrive in
  order. A cold-booted relay's buffered alarm (1970 clock) gets the current time after `0x5333`.

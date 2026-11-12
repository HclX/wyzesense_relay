# CLAUDE.md — WyzeSense Relay (`wyzesense_relay`)

ESP32 / ESP32-S3 (ESP-IDF) and ESP8266 (PlatformIO / Arduino) IP relay, cJTAG / UART ROM SBL programmer, and provisioning station for Wyze Sense v1 contact / motion boards and gateways, bridging the CC1310 UART host protocol (`0xAA55` / `0x55AA`, 115200 8N1) over WebSocket to `wyzesense2mqtt-rs` (`/ws/bridge`).

Broken out from [`HclX/openwyzesense`](https://github.com/HclX/openwyzesense) (`esp32/`).

## Repository Layout

- `common/`: Shared host protocol framing (`host_proto.c`, `host_proto.h`) and OWS build info block header (`build_info.h`).
- `relay/`: Portable C11 UART <-> WebSocket frame relay, ring buffer, SHA-1, Base64, and RFC 6455 client handshake.
- `contact/`: Portable C11 CC1310 2-pin cJTAG IEEE 1149.7 OSCAN1 flash programmer (`cjtag.c`), ROM serial bootloader client (`sbl.c`), boot-time gateway image sync (`gwsync.c`), dedicated stock gateway partition packer/verifier (`stock_image.c`), and `nv.json` live export/import (`gwnv.c`).
- `web/`: Portable C11 HTTP Basic/Bearer auth, streaming multipart parser, mDNS responder, and single-file Setup UI (`web/index.html`).
- `rig/`: Portable C11 contact test-rig signal controller (reed, button, tamper, LED sense).
- `host/`: Native Linux unit tests (`test_relay`, `test_cjtag`, `test_sblprobe`, `test_gwnv`, `test_stock_image`, `test_buffer`, `test_multipart`, `test_http_auth`, `test_mdns`, `test_rig`) and host CLIs (`ows-relay`, `ows-sbl`, `ows-gwsync`).
- `idf/`: ESP-IDF firmware (`esp32s3` and classic `esp32`).
- `esp8266/`: PlatformIO Arduino firmware (`d1_mini` / NodeMCU ESP8266).
- `firmware/`: Embedded CC1310 gateway firmware image (`gateway-contact-v1-app.bin`).
- `tools/`: Build, lint, coverage, and SBL helper scripts (`idf.sh`, `pio.sh`, `lint.sh`, `coverage.sh`, `toolchain.sh`, `uart_sbl.py`, `image_id.py`, `stock_partition.py`).
- `tests/`: Pytest suite for `ows-sbl`, `ows-gwsync`, `uart_sbl.py`, `image_id.py`, and `stock_partition.py`.

## Common Commands

```bash
# Configure, build, and run native C unit tests
cmake --preset host
cmake --build --preset host
ctest --preset host

# Run Python SBL and gateway sync tests
uv run pytest

# Run all formatters and static checks (clang-format, clang-tidy, ruff, shfmt, shellcheck)
tools/lint.sh

# Build ESP32-S3 / ESP32 firmware (after `tools/toolchain.sh esp-idf`)
tools/idf.sh build

# Build ESP8266 firmware
tools/pio.sh run
```

## Rules (carried over from openwyzesense; they still hold here)

- **Radio:** the relay itself never transmits on 906.8 MHz, but the gateway image it carries does. Flashing a gateway
  that can transmit, or running anything that makes it transmit, needs the user's OK. Test PANs only (0x65FE, 0x65FD,
  0x65FC); never the house PAN 0x845B without the user's explicit OK each time.
- **CCFG / page 31** (the CCFG rule, `docs/design/ccfg-rule.md` in openwyzesense, the user 2026-11-07): never a debug
  port off, protection bits, `IMAGE_VALID_CONF`, an unreachable bootloader, or a dongle's page 31. Program-only fills of
  blank bytes are fine. Erase-and-rewrite of page 31 only with a backup, M/C wired, stable power, the guards
  (`ccfg_safe`, `cjtag_write_page31_code` keeps the unit's CCFG byte for byte and puts page 31 back on a failure),
  and logged. Every page-31 writer goes through `cjtag_write_page31_code`.
- **Stock devices:** back up a unit's whole flash read-only before its first write.
- **Proprietary firmware never goes into this public repository or any app image:** no stock firmware in any app image,
  repository file, CI artifact, or release bundle. Only the known SHA-256 constant lives in `contact/stock_image.h`.
- **GitHub:** `gh` only on HclX repositories the user names. Branch per issue with a PR; push only when asked.

## Stock gateway firmware on a contact board

The relay can run the stock Wyze v1.8 dongle gateway firmware on its v1 contact board (Setup: gateway firmware `ows` / `stock`).
- **Dedicated flash partition (never embedded in the app):**
  - ESP32 / ESP32-S3 (`idf/partitions.csv`): `gwstock` (`data`, subtype `0x40`, offset `0x320000`, size `0x20000`),
    memory-mapped with `esp_partition_mmap` (registered at runtime via `esp_partition_register_external` on relays
    OTA-updated from pre-`gwstock` partition tables).
  - ESP8266 (`esp8266/src/main.cpp`): start of the unused filesystem flash area (`_FS_start`, offset `0x300000` on
    4 MB D1 mini per `eagle.flash.4m1m.ld`), accessed via `ESP.flashEraseSector` / `ESP.flashWrite` / `ESP.flashRead`.
- **Stored format (`contact/stock_image.h`):** 48-byte `OWSSTOCK` header (`magic[8] = "OWSSTOCK"`, `format = 1`,
  `len = 0x1EFA8`, `sha256[32]`) followed by the `0x1EFA8`-byte packed image: pages 0–29 (`0x1E000` bytes; pages 24–29
  are 24 KB of `0x00` in stock v1.8 and must be written) plus page 31's code (`0x1F000..0x1FFA7`, `0xFA8` bytes),
  excluding page 30 (NV) and the 88-byte CCFG (`0x1FFA8..0x1FFFF`). Accepted only if the packed image's SHA-256 equals
  `ab9c235bbce832a7dc74d67092199ba4108d8d24a33209c781bf483b85684ea0`.
- **Field upload & local pre-population:** `POST /contact/stock` (multipart field `image`) packs a 128 KB dump or
  `0x1EFA8` image on the fly, checks its SHA-256 before writing the partition, and verifies read-back; `GET /api/status`
  reports `"stock_stored": true|false` (no download or delete endpoint). Locally, `tools/stock_partition.py DUMP OUT`
  writes the byte-identical partition binary for `esptool.py write_flash`.
- **Flashing:** only through the debug port (cJTAG): the bootloader cannot write page 31, so `gwsync` refuses a stock
  image without it. Page 30 is never touched; page 31 is written only via `cjtag_write_page31_code`, keeping the unit's
  CCFG (and factory MAC) byte for byte.
- **Identity:** the stock CC1310 has no CH554 in front of it, so the relay does the CH554's part: `buffer.translate_sync`
  (`ows-relay --stock`) rewrites the host's sync requests `0x43xx` to `0x53xx`, ACKs the CC1310's reply on the UART and
  hands it to the host as `0x43xx`; only the request it translated, never the host's own async ones. After flashing,
  `gwsync` writes the contact's CCFG MAC into the stock NV with `0x5308` (Write MAC, FACTS.md in openwyzesense).

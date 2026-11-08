# CLAUDE.md — WyzeSense Relay (`wyzesense_relay`)

ESP32 / ESP32-S3 (ESP-IDF) and ESP8266 (PlatformIO / Arduino) IP relay, cJTAG / UART ROM SBL programmer, and provisioning station for Wyze Sense v1 contact / motion boards and gateways, bridging the CC1310 UART host protocol (`0xAA55` / `0x55AA`, 115200 8N1) over WebSocket to `wyzesense2mqtt-rs` (`/ws/bridge`).

Broken out from [`HclX/openwyzesense`](https://github.com/HclX/openwyzesense) (`esp32/`).

## Repository Layout

- `common/`: Shared host protocol framing (`host_proto.c`, `host_proto.h`) and OWS build info block header (`build_info.h`).
- `relay/`: Portable C11 UART <-> WebSocket frame relay, ring buffer, SHA-1, Base64, and RFC 6455 client handshake.
- `contact/`: Portable C11 CC1310 2-pin cJTAG IEEE 1149.7 OSCAN1 flash programmer (`cjtag.c`), ROM serial bootloader client (`sbl.c`), boot-time gateway image sync (`gwsync.c`), and `nv.json` live export/import (`gwnv.c`).
- `web/`: Portable C11 HTTP Basic/Bearer auth, streaming multipart parser, mDNS responder, and single-file Setup UI (`web/index.html`).
- `rig/`: Portable C11 contact test-rig signal controller (reed, button, tamper, LED sense).
- `host/`: Native Linux unit tests (`test_relay`, `test_gwnv`, `test_buffer`, `test_multipart`, `test_http_auth`, `test_mdns`) and host CLIs (`ows-relay`, `ows-sbl`, `ows-gwsync`).
- `idf/`: ESP-IDF firmware (`esp32s3` and classic `esp32`).
- `esp8266/`: PlatformIO Arduino firmware (`d1_mini` / NodeMCU ESP8266).
- `firmware/`: Embedded CC1310 gateway firmware image (`gateway-contact-v1-app.bin`).
- `tools/`: Build, lint, coverage, and SBL helper scripts (`idf.sh`, `pio.sh`, `lint.sh`, `coverage.sh`, `toolchain.sh`, `uart_sbl.py`, `image_id.py`).
- `tests/`: Pytest suite for `ows-sbl`, `ows-gwsync`, `uart_sbl.py`, and `image_id.py`.

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
- **Proprietary firmware never goes into this public repository:** stock images, dumps, and ESP builds that embed a
  stock image. `firmware/gateway-stock*.bin` is gitignored; CI builds with a placeholder.
- **GitHub:** `gh` only on HclX repositories the user names. Branch per issue with a PR; push only when asked.

## Stock gateway firmware on a contact board

The relay can carry the stock Wyze v1.8 dongle gateway firmware next to ours (Setup: gateway firmware `ows` / `stock`).
- **Image:** `gateway_image.py --stock OUT` packs it from a local stock dump as pages 0-23 (0x18000 bytes) plus
  page 31's code (0xFA8 bytes, before the CCFG), 0x18FA8 bytes in all; a 4-byte placeholder when there is none (CI).
  The ESP builds embed `firmware/gateway-stock-v1.8-app.bin` if present (`kStockGatewayImage`).
- **Flashing:** only through the debug port (cJTAG): the bootloader cannot write page 31, so `gwsync` refuses a stock
  image without it. The unit's CCFG (its MAC) stays byte for byte.
- **Identity:** the stock CC1310 has no CH554 in front of it, so the relay does the CH554's part: `buffer.translate_sync`
  (`ows-relay --stock`) rewrites the host's sync requests `0x43xx` to `0x53xx`, ACKs the CC1310's reply on the UART and
  hands it to the host as `0x43xx`; only the request it translated, never the host's own async ones. After flashing,
  `gwsync` writes the contact's CCFG MAC into the stock NV with `0x5308` (Write MAC, FACTS.md in openwyzesense).

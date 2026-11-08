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

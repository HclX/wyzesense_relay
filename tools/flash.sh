#!/usr/bin/env bash
# Flash a WyzeSense relay release binary to an ESP32-S3, ESP32, or ESP8266.
# Usage: ./flash.sh [SERIAL_PORT]
set -euo pipefail

DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
PORT=${1:-}

find_esptool() {
    if command -v esptool >/dev/null 2>&1; then
        ESPTOOL=(esptool)
    elif command -v esptool.py >/dev/null 2>&1; then
        ESPTOOL=(esptool.py)
    elif command -v python3 >/dev/null 2>&1 && python3 -m esptool version >/dev/null 2>&1; then
        ESPTOOL=(python3 -m esptool)
    elif command -v uv >/dev/null 2>&1; then
        ESPTOOL=(uvx esptool)
    else
        echo "error: esptool not found. Install it with: pip install esptool (or pipx install esptool)" >&2
        exit 1
    fi
}

find_esptool

PORT_ARGS=()
if [[ -n "$PORT" ]]; then
    PORT_ARGS=(--port "$PORT")
fi

echo "Detecting ESP chip..."
INFO=$("${ESPTOOL[@]}" "${PORT_ARGS[@]}" chip_id 2>&1) || {
    echo "$INFO" >&2
    exit 1
}

if printf '%s\n' "$INFO" | grep -qi 'ESP32-S3'; then
    CHIP=esp32s3
    PATTERN="ows-relay-esp32s3-full-*.bin"
elif printf '%s\n' "$INFO" | grep -qi 'ESP32'; then
    CHIP=esp32
    PATTERN="ows-relay-esp32-full-*.bin"
elif printf '%s\n' "$INFO" | grep -qi 'ESP8266'; then
    CHIP=esp8266
    PATTERN="ows-relay-esp8266-*.bin"
else
    echo "$INFO" >&2
    echo "error: unsupported chip (expected ESP32-S3, ESP32, or ESP8266)" >&2
    exit 1
fi

shopt -s nullglob
MATCHES=("$DIR"/$PATTERN)
shopt -u nullglob
if [[ ${#MATCHES[@]} -ne 1 ]]; then
    echo "error: expected 1 file matching $DIR/$PATTERN, found ${#MATCHES[@]}" >&2
    exit 1
fi

IMAGE=${MATCHES[0]}
echo "Flashing $CHIP with $(basename "$IMAGE")..."
exec "${ESPTOOL[@]}" --chip "$CHIP" "${PORT_ARGS[@]}" write_flash 0x0 "$IMAGE"

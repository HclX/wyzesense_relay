#!/usr/bin/env bash
# PlatformIO, pinned, for the ESP8266 relay (esp8266/): run in that directory with any pio arguments:
#   tools/pio.sh run                    (build)    tools/pio.sh run -t upload --upload-port /dev/ttyUSB0
# PlatformIO itself runs through uv at the version below; the platform's version is in platformio.ini.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
export PLATFORMIO_CORE_DIR=$ROOT/.toolchain/platformio # its packages beside the other tools, not in ~/.platformio
cd "$ROOT/esp8266"
exec uv tool run --from platformio==6.2.0 pio "$@"

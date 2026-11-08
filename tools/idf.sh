#!/usr/bin/env bash
# idf.py with the pinned ESP-IDF (tools/toolchain.sh esp-idf) set up, for the ESP32 relay firmware (idf/):
#   tools/idf.sh build            (in idf/; any idf.py arguments: build, flash -p PORT, monitor, menuconfig)
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
# shellcheck source-path=SCRIPTDIR source=toolchain.lock
source "$ROOT/tools/toolchain.lock"
IDF=$ROOT/.toolchain/esp-idf-$ESP_IDF_VERSION
[[ -x $IDF/tools/idf.py ]] || {
    echo "ESP-IDF missing: tools/toolchain.sh esp-idf" >&2
    exit 1
}
export IDF_TOOLS_PATH=$ROOT/.toolchain/espressif
# shellcheck source=/dev/null
source "$IDF/export.sh" > /dev/null
cd "$ROOT/idf"
exec idf.py "$@"

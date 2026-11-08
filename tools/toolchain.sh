#!/usr/bin/env bash
# Fetches the pinned ESP-IDF build tools (tools/toolchain.lock) into .toolchain/, checking the archive's SHA-256.
# Idempotent: an install already unpacked is left alone. Linux x86-64 only.
#   tools/toolchain.sh [esp-idf]...
# TOOLCHAIN_DEST overrides .toolchain (to try an install somewhere else).
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEST=${TOOLCHAIN_DEST:-$ROOT/.toolchain}
# shellcheck source-path=SCRIPTDIR source=toolchain.lock
source "$ROOT/tools/toolchain.lock"

esp_idf() { # the ESP-IDF release into $DEST/esp-idf-<version>, its esp32 and esp32s3 tools into $DEST/espressif
    local dir=$DEST/esp-idf-$ESP_IDF_VERSION tools=$DEST/espressif
    if [[ -x $dir/tools/idf.py && -f $dir/.ows-installed ]]; then
        echo "esp-idf: $dir"
        return
    fi
    if [[ ! -x $dir/tools/idf.py ]]; then
        mkdir -p "$DEST/.dl"
        local zip=$DEST/.dl/${ESP_IDF_URL##*/}
        if [[ ! -f $zip ]] || ! echo "$ESP_IDF_SHA256  $zip" | sha256sum -c --quiet - 2> /dev/null; then
            echo "esp-idf: downloading ${ESP_IDF_URL##*/}"
            curl -fsSL --retry 3 -o "$zip.part" "$ESP_IDF_URL"
            mv "$zip.part" "$zip"
        fi
        echo "$ESP_IDF_SHA256  $zip" | sha256sum -c --quiet -
        rm -rf "$dir" && (cd "$DEST" && unzip -q "$zip")
        rm -f "$zip"
    fi
    # its install.sh makes its Python env with `python3 -m venv`, which needs the distribution's python3-venv;
    # uv makes it instead, at the path install.sh looks for
    local py env major minor _
    py=$(python3 -c 'import sys; print(f"{sys.version_info[0]}.{sys.version_info[1]}")')
    IFS=. read -r major minor _ <<< "${ESP_IDF_VERSION#v}"
    env=$tools/python_env/idf${major}.${minor}_py${py}_env # the name idf_tools.py gives it
    [[ -x $env/bin/python ]] || uv venv -q --seed --python "$py" "$env"
    IDF_TOOLS_PATH=$tools "$dir/install.sh" esp32,esp32s3 > /dev/null
    touch "$dir/.ows-installed"
    echo "esp-idf: $dir"
}

tools=("$@")
[[ ${#tools[@]} -gt 0 ]] || tools=(esp-idf)
for t in "${tools[@]}"; do
    case $t in
        esp-idf) esp_idf ;;
        *)
            echo "unknown tool: $t" >&2
            exit 2
            ;;
    esac
done

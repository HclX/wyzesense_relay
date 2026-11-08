#!/usr/bin/env bash
# Build the CMake "coverage" preset (Clang/LLVM source-based coverage in build/coverage), run all automated host
# tests (ctest + pytest), and emit C (llvm-cov report + HTML) coverage summaries.
#   tools/coverage.sh [--html-dir DIR]
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

HTML_DIR=$ROOT/build/coverage/html
while (($#)); do
    case $1 in
        --html-dir)
            HTML_DIR=$2
            shift 2
            ;;
        *)
            echo "usage: tools/coverage.sh [--html-dir DIR]" >&2
            exit 2
            ;;
    esac
done

for bin in clang llvm-profdata llvm-cov; do
    command -v "$bin" > /dev/null || {
        echo "coverage: missing $bin" >&2
        exit 1
    }
done

BUILD=$ROOT/build/coverage
rm -rf "$BUILD/profraw" "$HTML_DIR"
mkdir -p "$BUILD/profraw"

echo "== cmake --preset coverage"
cmake --preset coverage > /dev/null
cmake --build --preset coverage --parallel

export LLVM_PROFILE_FILE="$BUILD/profraw/%p-%m%c.profraw"
export OWS_HOST_BUILD="$BUILD"
export no_proxy="127.0.0.1,localhost" NO_PROXY="127.0.0.1,localhost"

echo "== ctest --preset coverage"
ctest --preset coverage

echo "== pytest"
uv run pytest -q

PROFDATA=$BUILD/coverage.profdata
shopt -s nullglob
profraws=("$BUILD/profraw/"*.profraw)
shopt -u nullglob
llvm-profdata merge -sparse "${profraws[@]}" -o "$PROFDATA"

bins=(
    "$BUILD/test_relay"
    "$BUILD/test_gwnv"
    "$BUILD/test_buffer"
    "$BUILD/test_multipart"
    "$BUILD/test_http_auth"
    "$BUILD/test_mdns"
    "$BUILD/ows-relay"
    "$BUILD/ows-sbl"
    "$BUILD/ows-gwsync"
)
obj_args=("${bins[0]}")
for b in "${bins[@]:1}"; do
    obj_args+=(-object "$b")
done
ignore_re='.*(/tests/|/host/test_).*'

echo ""
echo "=== C Coverage (llvm-cov, excluding test_*.c) ==="
llvm-cov report "${obj_args[@]}" "-instr-profile=$PROFDATA" "-ignore-filename-regex=$ignore_re"
llvm-cov export --summary-only "${obj_args[@]}" "-instr-profile=$PROFDATA" "-ignore-filename-regex=$ignore_re" \
    > "$BUILD/c_summary.json"
llvm-cov show "${obj_args[@]}" "-instr-profile=$PROFDATA" "-ignore-filename-regex=$ignore_re" \
    -format=html "-output-dir=$HTML_DIR" -show-branches=count -show-line-counts-or-regions

echo ""
echo "coverage: HTML report at $HTML_DIR/index.html"

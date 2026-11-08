#!/usr/bin/env bash
# The project's lint and format checks, the same locally and in CI; all tools pinned in uv.lock.
#   tools/lint.sh          check: fails on any finding
#   tools/lint.sh --fix    rewrite what the formatters can fix (clang-format, ruff format / --fix, shfmt), then check
# C: clang-format (.clang-format) on all C/C++ sources; clang-tidy (.clang-tidy) on the files built for the host (it needs
# build/host/compile_commands.json, configured here if missing). Python: ruff format + ruff check (pyproject.toml).
# Shell: shfmt + shellcheck. .toolchain/ is never touched.
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1

fix=0
[[ ${1:-} == --fix ]] && fix=1
failed=()

step() { # name command...
    local name=$1
    shift
    echo "== $name"
    "$@" || failed+=("$name")
}

mapfile -t c_files < <(git ls-files '*.c' '*.h' '*.cpp')
mapfile -t sh_files < <(git ls-files '*.sh')
# clang-tidy: the files in the host build (idf/ builds under ESP-IDF's own warnings)
mapfile -t tidy_files < <(git ls-files '*.c' | grep -v -e '^idf/')

if ((fix)); then
    uv run clang-format -i "${c_files[@]}"
    uv run ruff format --quiet .
    uv run ruff check --fix --quiet . || true
    uv run shfmt -i 4 -ci -sr -w "${sh_files[@]}"
fi

step clang-format uv run clang-format --dry-run --Werror "${c_files[@]}"
if [[ ! -f build/host/compile_commands.json ]]; then
    cmake --preset host -DCMAKE_EXPORT_COMPILE_COMMANDS=ON > /dev/null
fi
step clang-tidy uv run clang-tidy -p build/host --quiet "${tidy_files[@]}"
step ruff-format uv run ruff format --check --quiet .
step ruff-check uv run ruff check --quiet .
step shfmt uv run shfmt -i 4 -ci -sr -d "${sh_files[@]}"
step shellcheck uv run shellcheck -x "${sh_files[@]}"

if ((${#failed[@]})); then
    echo "lint: FAILED: ${failed[*]} (tools/lint.sh --fix rewrites what can be fixed automatically)"
    exit 1
fi
echo "lint: all clean"

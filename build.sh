#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds WarcraftXL.dll and wxl-patcher.exe from Linux (32-bit Windows binaries); ./build.sh --help.
set -euo pipefail

usage() {
    cat <<'TEXT'
./build.sh [--client <dir>] [--sdk <dir>] [--config Release|Debug] [--clean] [--test] [--autopatch]

  --client     client folder the DLL and the patcher are copied into after each build (kept in the cache)
  --sdk        wxl-sdk working copy; default ../wxl-sdk when present, else the installed package
  --test       runs ctest; the tests are Windows programs, run through CMAKE_CROSSCOMPILING_EMULATOR (wine)
  --autopatch  runs wxl-patcher.exe on the client's Wow.exe through wine

Toolchain: $CMAKE_TOOLCHAIN_FILE when set (e.g. clang-cl with the MSVC CRT and SDK), otherwise
cmake/toolchain/mingw-i686.cmake (clang mingw-w64).
TEXT
}

root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
build_dir="$root/build/x86"
config="Release"
client=""
sdk=""
clean=0
test=0
autopatch=0
reconfigure=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --client)    client="$(cd "$2" && pwd)"; reconfigure=1; shift 2 ;;
        --sdk)       sdk="$(cd "$2" && pwd)"; reconfigure=1; shift 2 ;;
        --config)    config="$2"; shift 2 ;;
        --clean)     clean=1; shift ;;
        --test)      test=1; shift ;;
        --autopatch) autopatch=1; shift ;;
        -h|--help)   usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

if [[ -z "$sdk" && -f "$root/../wxl-sdk/CMakeLists.txt" ]]; then
    sdk="$(cd "$root/../wxl-sdk" && pwd)"
fi
toolchain="${CMAKE_TOOLCHAIN_FILE:-$root/cmake/toolchain/mingw-i686.cmake}"

if [[ $clean -eq 1 ]]; then
    rm -rf "${build_dir:?}"
fi
if [[ $clean -eq 1 || $reconfigure -eq 1 || ! -f "$build_dir/CMakeCache.txt" ]]; then
    cmake -S "$root" -B "$build_dir" -G Ninja \
        -DCMAKE_BUILD_TYPE="$config" \
        -DCMAKE_TOOLCHAIN_FILE="$toolchain" \
        ${client:+-DCLIENT_PATH="$client"} \
        ${sdk:+-DWXL_SDK_DIR="$sdk"}
fi
cmake --build "$build_dir" --parallel

if [[ $test -eq 1 ]]; then
    ctest --test-dir "$build_dir" --output-on-failure
fi

if [[ $autopatch -eq 1 ]]; then
    client="$(sed -n 's/^CLIENT_PATH:PATH=//p' "$build_dir/CMakeCache.txt" | head -1)"
    [[ -f "$client/Wow.exe" ]] || { echo "No Wow.exe in the client folder '$client' (--client)." >&2; exit 1; }
    command -v wine >/dev/null || { echo "wine runs wxl-patcher.exe from Linux; it is not installed." >&2; exit 1; }
    wine "$build_dir/wxl-patcher.exe" "$client/Wow.exe"
fi

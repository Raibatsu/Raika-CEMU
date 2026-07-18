#!/usr/bin/env bash
set -euo pipefail

export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_JOBS="${BUILD_JOBS:-$(nproc)}"
case "${BUILD_JOBS}" in
	''|*[!0-9]*|0) echo "BUILD_JOBS must be a positive integer" >&2; exit 2 ;;
esac
cd "${ROOT}"

bash "${ROOT}/dist/switch/deps/prepare_submodules.sh"

echo ">> Localizing NVK driver symbols ..."
bash "${ROOT}/dist/switch/deps/prepare_nvk.sh"

cmake -S . -B build_switch -G Ninja \
	-DCMAKE_TOOLCHAIN_FILE="${ROOT}/cmake/Toolchain-Switch.cmake" \
	-DCMAKE_BUILD_TYPE=Release \
	-DSWITCH_LTO_JOBS="${BUILD_JOBS}"

cmake --build build_switch --parallel "${BUILD_JOBS}" --target CemuNro

echo ">> Output: ${ROOT}/bin/Cemu.nro"

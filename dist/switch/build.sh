#!/usr/bin/env bash
set -euo pipefail

export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_JOBS="${BUILD_JOBS:-$(nproc)}"
RELEASE_VERSION="${RELEASE_VERSION:-1.1.0}"
case "${BUILD_JOBS}" in
	''|*[!0-9]*|0) echo "BUILD_JOBS must be a positive integer" >&2; exit 2 ;;
esac
if [[ ! "$RELEASE_VERSION" =~ ^([0-9]+)\.([0-9]+)\.([0-9]+)$ ]]; then
	echo "RELEASE_VERSION must use major.minor.patch format" >&2
	exit 2
fi
VERSION_MAJOR="${BASH_REMATCH[1]}"
VERSION_MINOR="${BASH_REMATCH[2]}"
VERSION_PATCH="${BASH_REMATCH[3]}"
cd "${ROOT}"

bash "${ROOT}/dist/switch/deps/prepare_submodules.sh"

echo ">> Localizing NVK driver symbols ..."
bash "${ROOT}/dist/switch/deps/prepare_nvk.sh"

cmake -S . -B build_switch -G Ninja \
	-DCMAKE_TOOLCHAIN_FILE="${ROOT}/cmake/Toolchain-Switch.cmake" \
	-DCMAKE_BUILD_TYPE=Release \
	-DEMULATOR_VERSION_MAJOR="${VERSION_MAJOR}" \
	-DEMULATOR_VERSION_MINOR="${VERSION_MINOR}" \
	-DEMULATOR_VERSION_PATCH="${VERSION_PATCH}" \
	-DSWITCH_LTO_JOBS="${BUILD_JOBS}"

cmake --build build_switch --parallel "${BUILD_JOBS}" --target CemuNro

echo ">> Output: ${ROOT}/bin/cemu_core.nro"

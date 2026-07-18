#!/bin/bash
set -euo pipefail

BUILD_JOBS="${BUILD_JOBS:-18}"
case "${BUILD_JOBS}" in
	''|*[!0-9]*|0) echo "BUILD_JOBS must be a positive integer" >&2; exit 2 ;;
esac
export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
export PATH="/usr/bin:${DEVKITPRO}/tools/bin:${DEVKITPRO}/devkitA64/bin:$PATH"
mkdir -p /tmp
export TMPDIR=/tmp TMP=/tmp TEMP=/tmp

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

echo "[1/4] Building the emulator"
BUILD_JOBS="$BUILD_JOBS" bash "$ROOT/dist/switch/build.sh"

echo "[2/4] Staging the emulator"
mkdir -p "$ROOT/switch_launcher/romfs/emu"
rm -f "$ROOT/switch_launcher/romfs/emu/Cemu.nro" "$ROOT/switch_launcher/romfs/emu/cemu_vk.nro"
cp -f "$ROOT/bin/Cemu.nro" "$ROOT/switch_launcher/romfs/emu/cemu_vk.nro"
sha256sum "$ROOT/switch_launcher/romfs/emu/cemu_vk.nro" | awk '{print $1}' > "$ROOT/switch_launcher/romfs/emu/cemu_vk.sha256"

echo "[3/4] Building the HOME Menu forwarder"
make -C "$ROOT/switch_launcher/fwd" clean
make -j "$BUILD_JOBS" -C "$ROOT/switch_launcher/fwd"

echo "[4/4] Building the launcher"
cd "$ROOT/switch_launcher"
make clean
make -j "$BUILD_JOBS"

echo
echo "Copy switch_launcher/Cemu.nro to sdmc:/switch/Cemu/Cemu.nro"
ls -la "$ROOT/switch_launcher/Cemu.nro"

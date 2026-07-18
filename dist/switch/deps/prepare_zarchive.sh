#!/usr/bin/env bash
set -euo pipefail
source "$(cd "$(dirname "$0")" && pwd)/common.sh"
parse_check_only "$@"

SUBMODULE="dependencies/ZArchive"
PATCH="${ROOT}/dist/switch/patches/zarchive-switch.patch"

if (( CHECK_ONLY )); then
	submodule_is_ready "${SUBMODULE}" "CMakeLists.txt" || die "missing submodule: ${SUBMODULE}"
	patch_is_applied "${ROOT}/${SUBMODULE}" "${PATCH}" || die "patch is not applied: ${PATCH#${ROOT}/}"
	echo "Ready: ${SUBMODULE}"
	exit 0
fi

prepare_submodule "${SUBMODULE}" "CMakeLists.txt"
apply_patch_once "${ROOT}/${SUBMODULE}" "${PATCH}"

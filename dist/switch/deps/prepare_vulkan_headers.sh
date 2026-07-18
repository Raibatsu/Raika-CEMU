#!/usr/bin/env bash
set -euo pipefail
source "$(cd "$(dirname "$0")" && pwd)/common.sh"
parse_check_only "$@"

SUBMODULE="dependencies/Vulkan-Headers"
MARKER="include/vulkan/vulkan.h"

if (( CHECK_ONLY )); then
	submodule_is_ready "${SUBMODULE}" "${MARKER}" || die "missing submodule: ${SUBMODULE}"
	echo "Ready: ${SUBMODULE}"
	exit 0
fi

prepare_submodule "${SUBMODULE}" "${MARKER}"
echo "Ready: ${SUBMODULE}"

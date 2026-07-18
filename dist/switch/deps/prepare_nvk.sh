#!/usr/bin/env bash
set -euo pipefail
source "$(cd "$(dirname "$0")" && pwd)/common.sh"
parse_check_only "$@"

NVK_DIR="${ROOT}/dependencies/switch_mesa_vulkan"
CHECKSUMS="${NVK_DIR}/nvk-archives.sha256"
LOCAL_OBJECT="${NVK_DIR}/libnvk_local.o"

[[ -d "${NVK_DIR}/lib" ]] || die "NVK directory is missing: dependencies/switch_mesa_vulkan/lib"
[[ -f "${CHECKSUMS}" ]] || die "NVK checksum manifest is missing: dependencies/switch_mesa_vulkan/nvk-archives.sha256"

if (( CHECK_ONLY )); then
	(
		cd "${NVK_DIR}/lib"
		sha256sum --check --strict "${CHECKSUMS}"
	)
	[[ -f "${LOCAL_OBJECT}" ]] || die "localized NVK object is missing; run dist/switch/deps/prepare_nvk.sh"
	echo "Ready: NVK driver"
	exit 0
fi

bash "${ROOT}/dist/switch/localize_nvk.sh"

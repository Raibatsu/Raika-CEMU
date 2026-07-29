#!/usr/bin/env bash
set -euo pipefail
source "$(cd "$(dirname "$0")" && pwd)/common.sh"
parse_check_only "$@"

NVK_DIR="${ROOT}/dependencies/switch_mesa_vulkan"
CHECKSUMS="${NVK_DIR}/share/nvk-switch/SHA256SUMS"
LOCAL_OBJECT="${NVK_DIR}/libnvk_local.o"

[[ -d "${NVK_DIR}/lib" ]] || die "NVK directory is missing: dependencies/switch_mesa_vulkan/lib"
[[ -f "${CHECKSUMS}" ]] || die "NVK SDK checksum manifest is missing: dependencies/switch_mesa_vulkan/share/nvk-switch/SHA256SUMS"

if (( CHECK_ONLY )); then
	(
		cd "${NVK_DIR}"
		tr -d '\r' < "${CHECKSUMS}" | sha256sum --check --strict -
	)
	[[ -f "${LOCAL_OBJECT}" ]] || die "localized NVK object is missing; run dist/switch/deps/prepare_nvk.sh"
	echo "Ready: NVK driver"
	exit 0
fi

bash "${ROOT}/dist/switch/localize_nvk.sh"

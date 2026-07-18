#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"

case "$#" in
	0) ARGS=() ;;
	1)
		[[ "$1" == "--check" ]] || { echo "error: unknown option: $1" >&2; exit 1; }
		ARGS=(--check)
		;;
	*) echo "error: expected no arguments or --check" >&2; exit 1 ;;
esac

for script in \
	prepare_zarchive.sh \
	prepare_xbyak_aarch64.sh \
	prepare_vulkan_headers.sh \
	prepare_imgui.sh; do
	bash "${HERE}/${script}" "${ARGS[@]}"
done

#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"

CHECK_ONLY=0
SKIP_SYSTEM=0
SKIP_NVK=0
usage() {
	echo "Usage: $0 [--check] [--skip-system-packages] [--skip-nvk]"
}

for argument in "$@"; do
	case "${argument}" in
		--check) CHECK_ONLY=1 ;;
		--skip-system-packages) SKIP_SYSTEM=1 ;;
		--skip-nvk) SKIP_NVK=1 ;;
		-h|--help) usage; exit 0 ;;
		*) usage >&2; echo "error: unknown option: ${argument}" >&2; exit 1 ;;
	esac
done

if (( CHECK_ONLY )); then
	RESULT=0
	if (( SKIP_SYSTEM == 0 )); then
		bash "${HERE}/deps/install_devkitpro_packages.sh" --check || RESULT=1
	fi
	bash "${HERE}/deps/prepare_submodules.sh" --check || RESULT=1
	bash "${HERE}/deps/build_boost.sh" --check || RESULT=1
	if (( SKIP_NVK == 0 )); then
		bash "${HERE}/deps/prepare_nvk.sh" --check || RESULT=1
	fi
	exit "${RESULT}"
fi

if (( SKIP_SYSTEM == 0 )); then
	bash "${HERE}/deps/install_devkitpro_packages.sh"
fi
bash "${HERE}/deps/prepare_submodules.sh"
bash "${HERE}/deps/build_boost.sh"
if (( SKIP_NVK == 0 )); then
	bash "${HERE}/deps/prepare_nvk.sh"
fi

echo "Switch dependencies are ready."

#!/usr/bin/env bash
set -euo pipefail

SWITCH_DEPS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SWITCH_DEPS_DIR}/../../.." && pwd)"
export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
SWITCH_DEPS_ROOT="${ROOT}/dependencies/switch_deps"
SWITCH_BOOST_PREFIX="${SWITCH_DEPS_ROOT}/boost"

die() {
	echo "error: $*" >&2
	exit 1
}

parse_check_only() {
	CHECK_ONLY=0
	case "$#" in
		0) ;;
		1)
			[[ "$1" == "--check" ]] || die "unknown option: $1"
			CHECK_ONLY=1
			;;
		*) die "expected no arguments or --check" ;;
	esac
}

require_command() {
	command -v "$1" >/dev/null 2>&1 || die "required command not found: $1"
}

submodule_is_ready() {
	local path="$1"
	local marker="$2"
	local status
	status="$(git -C "${ROOT}" submodule status -- "${path}" 2>/dev/null)" || return 1
	[[ -n "${status}" && "${status:0:1}" != "-" && -e "${ROOT}/${path}/${marker}" ]]
}

prepare_submodule() {
	local path="$1"
	local marker="$2"
	if submodule_is_ready "${path}" "${marker}"; then
		return
	fi
	git -C "${ROOT}" submodule update --init --depth 1 -- "${path}"
	submodule_is_ready "${path}" "${marker}" || die "submodule is incomplete: ${path}"
}

patch_is_applied() {
	local repository="$1"
	local patch="$2"
	git -C "${repository}" apply --reverse --check --ignore-space-change < "${patch}" >/dev/null 2>&1
}

apply_patch_once() {
	local repository="$1"
	local patch="$2"
	if patch_is_applied "${repository}" "${patch}"; then
		echo "Already patched: ${repository#${ROOT}/}"
		return
	fi
	git -C "${repository}" apply --check --ignore-space-change < "${patch}" ||
		die "patch does not apply cleanly: ${patch}"
	git -C "${repository}" apply --ignore-space-change < "${patch}"
	echo "Applied: ${patch#${ROOT}/}"
}

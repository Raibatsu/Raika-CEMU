#!/usr/bin/env bash
set -euo pipefail
source "$(cd "$(dirname "$0")" && pwd)/common.sh"

CHECK_ONLY=0
FORCE=0
BUILD_JOBS="${BUILD_JOBS:-$(nproc)}"
case "${BUILD_JOBS}" in
	''|*[!0-9]*|0) die "BUILD_JOBS must be a positive integer" ;;
esac
for argument in "$@"; do
	case "${argument}" in
		--check) CHECK_ONLY=1 ;;
		--force) FORCE=1 ;;
		*) die "unknown option: ${argument}" ;;
	esac
done
(( CHECK_ONLY == 0 || FORCE == 0 )) || die "--check and --force cannot be combined"

BOOST_VERSION="1.86.0"
BOOST_TAG="1_86_0"
BOOST_ARCHIVE="boost_${BOOST_TAG}.tar.bz2"
BOOST_URL="https://archives.boost.io/release/${BOOST_VERSION}/source/${BOOST_ARCHIVE}"
BOOST_SHA256="1bed88e40401b2cb7a1f76d4bab499e352fa4d0c5f31c0dbae64e24d34d7513b"
CACHE_DIR="${SWITCH_DEPS_ROOT}/cache"
SOURCE_DIR="${SWITCH_DEPS_ROOT}/src/boost_${BOOST_TAG}"
BUILD_DIR="${SWITCH_DEPS_ROOT}/build/boost_${BOOST_TAG}"
STAGE_DIR="${SWITCH_DEPS_ROOT}/stage/boost_${BOOST_TAG}"
ARCHIVE_PATH="${CACHE_DIR}/${BOOST_ARCHIVE}"
PATCH="${ROOT}/dist/switch/patches/boost-1.86.0-switch.patch"
USER_CONFIG="${SWITCH_DEPS_ROOT}/build/boost-user-config.jam"

boost_is_ready() {
	[[ -f "${SOURCE_DIR}/boost/version.hpp" &&
		-f "${SWITCH_BOOST_PREFIX}/lib/libboost_filesystem.a" &&
		-f "${SWITCH_BOOST_PREFIX}/lib/libboost_program_options.a" ]] &&
		grep -Eq 'BOOST_VERSION[[:space:]]+108600' "${SOURCE_DIR}/boost/version.hpp" &&
		patch_is_applied "${SOURCE_DIR}" "${PATCH}"
}

if (( CHECK_ONLY )); then
	boost_is_ready || die "Boost ${BOOST_VERSION} is not installed; run dist/switch/deps/build_boost.sh"
	echo "Ready: Boost ${BOOST_VERSION} at ${SWITCH_BOOST_PREFIX#${ROOT}/}"
	exit 0
fi

if (( FORCE == 0 )) && boost_is_ready; then
	echo "Ready: Boost ${BOOST_VERSION} at ${SWITCH_BOOST_PREFIX#${ROOT}/}"
	exit 0
fi

for command_name in curl sha256sum tar git g++; do
	require_command "${command_name}"
done

CXX="${DEVKITPRO}/devkitA64/bin/aarch64-none-elf-g++"
[[ -x "${CXX}" ]] || die "devkitA64 compiler not found: ${CXX}"

mkdir -p "${CACHE_DIR}" "${SWITCH_DEPS_ROOT}/src" "${SWITCH_DEPS_ROOT}/build" "${SWITCH_DEPS_ROOT}/stage"
if [[ -f "${ARCHIVE_PATH}" ]] && ! printf '%s  %s\n' "${BOOST_SHA256}" "${ARCHIVE_PATH}" | sha256sum --check --status; then
	rm -f "${ARCHIVE_PATH}"
fi
if [[ ! -f "${ARCHIVE_PATH}" ]]; then
	echo "Downloading Boost ${BOOST_VERSION} ..."
	curl --fail --location --retry 3 --output "${ARCHIVE_PATH}.tmp" "${BOOST_URL}"
	printf '%s  %s\n' "${BOOST_SHA256}" "${ARCHIVE_PATH}.tmp" | sha256sum --check --status || {
		rm -f "${ARCHIVE_PATH}.tmp"
		die "Boost archive checksum mismatch"
	}
	mv -f "${ARCHIVE_PATH}.tmp" "${ARCHIVE_PATH}"
fi

printf '%s  %s\n' "${BOOST_SHA256}" "${ARCHIVE_PATH}" | sha256sum --check --status ||
	die "Boost archive checksum mismatch"
if [[ ! -d "${SOURCE_DIR}" ]]; then
	tar -xf "${ARCHIVE_PATH}" -C "${SWITCH_DEPS_ROOT}/src"
fi
apply_patch_once "${SOURCE_DIR}" "${PATCH}"

if [[ ! -x "${SOURCE_DIR}/b2" ]]; then
	(
		cd "${SOURCE_DIR}"
		./bootstrap.sh --with-toolset=gcc --with-libraries=filesystem,program_options
	)
fi

printf '%s\n' \
	'using gcc : switch' \
	"    : \"${CXX}\"" \
	'    ;' > "${USER_CONFIG}"

rm -rf "${BUILD_DIR}" "${STAGE_DIR}"
mkdir -p "${BUILD_DIR}" "${STAGE_DIR}"
(
	cd "${SOURCE_DIR}"
	./b2 -q \
		-j "${BUILD_JOBS}" \
		--user-config="${USER_CONFIG}" \
		--build-dir="${BUILD_DIR}" \
		--stagedir="${STAGE_DIR}" \
		--layout=system \
		toolset=gcc-switch \
		target-os=linux \
		architecture=arm \
		address-model=64 \
		abi=aapcs \
		binary-format=elf \
		variant=release \
		link=static \
		threading=multi \
		runtime-link=static \
		cxxstd=20 \
		cxxflags="-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIC -ffunction-sections -fdata-sections -D__SWITCH__ -D_GNU_SOURCE -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64 -I${DEVKITPRO}/libnx/include" \
		--with-filesystem \
		--with-program_options \
		stage
)

boost_is_staged() {
	[[ -f "${STAGE_DIR}/lib/libboost_filesystem.a" &&
		-f "${STAGE_DIR}/lib/libboost_program_options.a" ]]
}
boost_is_staged || die "Boost build completed without the expected libraries"
rm -rf "${SWITCH_BOOST_PREFIX}"
mv "${STAGE_DIR}" "${SWITCH_BOOST_PREFIX}"
boost_is_ready || die "Boost installation validation failed"
echo "Installed Boost ${BOOST_VERSION}: ${SWITCH_BOOST_PREFIX#${ROOT}/}"

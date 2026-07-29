#!/usr/bin/env bash
# Localize Mesa's Vulkan exports so they do not collide with Cemu's dispatch table.
set -euo pipefail

DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
BIN="${DEVKITPRO}/devkitA64/bin"
LD="${BIN}/aarch64-none-elf-ld"
OBJCOPY="${BIN}/aarch64-none-elf-objcopy"

HERE="$(cd "$(dirname "$0")/../../dependencies/switch_mesa_vulkan" && pwd)"
LIBDIR="${HERE}/lib"
MERGED="${HERE}/libnvk_merged.o"
LOCAL="${HERE}/libnvk_local.o"
LOCAL_CANDIDATE="${HERE}/libnvk_local.o.tmp"
CHECKSUMS="${HERE}/share/nvk-switch/SHA256SUMS"

cleanup() {
	rm -f "${MERGED}" "${LOCAL_CANDIDATE}"
}
trap cleanup EXIT

REQUIRED_ARCHIVES=(
	libnvk.a libvulkan_wsi.a libvulkan_runtime.a libvulkan_instance.a
	libvulkan_util.a libvulkan_lite_runtime.a libvulkan_lite_instance.a
	libnil.a liblibnil_format_table.a libnak.a libnak_rs.a libnouveau_mme.a
	libnouveau_ws.a libnvidia_headers_c.a libnir.a libvtn.a libcompiler.a
	libcompiler_c_helpers.a libblake3.a libmesa_util.a libmesa_util_simd.a
	libmesa_util_c11.a
	libxmlconfig.a
)
for archive in "${REQUIRED_ARCHIVES[@]}"; do
	if [[ ! -f "${LIBDIR}/${archive}" ]]; then
		echo "Missing NVK archive: ${LIBDIR}/${archive}" >&2
		echo "See dist/switch/README.md for the required private NVK archives." >&2
		exit 1
	fi
done

(
	cd "${HERE}"
	tr -d '\r' < "${CHECKSUMS}" | sha256sum --check --strict -
)

echo "Merging NVK archives from ${LIBDIR} ..."
# Mesa 26.1's libnvk.a embeds some runtime and WSI members. Extract it in full,
# then let the archive group supply only still-unresolved dependencies.
"${LD}" -r \
	--whole-archive \
	"${LIBDIR}/libnvk.a" \
	--no-whole-archive \
	--start-group \
	"${LIBDIR}/libvulkan_wsi.a" \
	"${LIBDIR}/libvulkan_runtime.a" \
	"${LIBDIR}/libvulkan_instance.a" \
	"${LIBDIR}/libvulkan_util.a" \
	"${LIBDIR}/libvulkan_lite_runtime.a" \
	"${LIBDIR}/libvulkan_lite_instance.a" \
	"${LIBDIR}/libnil.a" \
	"${LIBDIR}/liblibnil_format_table.a" \
	"${LIBDIR}/libnak.a" \
	"${LIBDIR}/libnak_rs.a" \
	"${LIBDIR}/libnouveau_mme.a" \
	"${LIBDIR}/libnouveau_ws.a" \
	"${LIBDIR}/libnvidia_headers_c.a" \
	"${LIBDIR}/libnir.a" \
	"${LIBDIR}/libvtn.a" \
	"${LIBDIR}/libcompiler.a" \
	"${LIBDIR}/libcompiler_c_helpers.a" \
	"${LIBDIR}/libblake3.a" \
	"${LIBDIR}/libmesa_util.a" \
	"${LIBDIR}/libmesa_util_simd.a" \
	"${LIBDIR}/libmesa_util_c11.a" \
	"${LIBDIR}/libxmlconfig.a" \
	--end-group \
	-o "${MERGED}"

echo "Localizing all globals except the ICD entrypoints ..."
"${OBJCOPY}" \
	--keep-global-symbol=vk_icdGetInstanceProcAddr \
	--keep-global-symbol=vk_icdNegotiateLoaderICDInterfaceVersion \
	--keep-global-symbol=vk_icdGetPhysicalDeviceProcAddr \
	"${MERGED}" "${LOCAL_CANDIDATE}"

if [[ ! -f "${LOCAL}" ]] || ! cmp -s "${LOCAL_CANDIDATE}" "${LOCAL}"; then
	mv -f "${LOCAL_CANDIDATE}" "${LOCAL}"
fi
echo "Done: ${LOCAL}"

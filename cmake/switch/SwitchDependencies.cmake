# Switch dependencies with upstream-compatible target names.
include(FetchContent)

set(SWITCH_PORTLIBS "${DEVKITPRO}/portlibs/switch")
set(SWITCH_LIBNX    "${DEVKITPRO}/libnx")
include_directories(SYSTEM "${SWITCH_PORTLIBS}/include")

# Threads
set(CMAKE_THREAD_LIBS_INIT "-lpthread")
set(CMAKE_HAVE_THREADS_LIBRARY ON)
set(CMAKE_USE_PTHREADS_INIT ON)
find_package(Threads)
if(NOT TARGET Threads::Threads)
	add_library(Threads::Threads INTERFACE IMPORTED)
endif()

function(cemu_switch_portlib target libfile)
	add_library(${target} STATIC IMPORTED GLOBAL)
	set_target_properties(${target} PROPERTIES
		IMPORTED_LOCATION "${SWITCH_PORTLIBS}/lib/${libfile}"
		INTERFACE_INCLUDE_DIRECTORIES "${SWITCH_PORTLIBS}/include")
endfunction()

function(cemu_switch_static_import target prefix libfile)
	add_library(${target} STATIC IMPORTED GLOBAL)
	if(ARGC GREATER 3)
		set(_include_dir "${ARGV3}")
	else()
		set(_include_dir "${prefix}/include")
	endif()
	set_target_properties(${target} PROPERTIES
		IMPORTED_LOCATION "${prefix}/lib/${libfile}"
		INTERFACE_INCLUDE_DIRECTORIES "${_include_dir}")
endfunction()

# Compression and image libraries
cemu_switch_portlib(ZLIB::ZLIB   libz.a)
cemu_switch_portlib(zstd::zstd   libzstd.a)
cemu_switch_portlib(PNG::PNG     libpng16.a)
set_property(TARGET PNG::PNG APPEND PROPERTY INTERFACE_LINK_LIBRARIES ZLIB::ZLIB)

# FFmpeg's Horizon build includes Averne's NVTEGRA decoder backend.
cemu_switch_portlib(FFmpeg::avutil      libavutil.a)
cemu_switch_portlib(FFmpeg::swresample libswresample.a)
cemu_switch_portlib(FFmpeg::swscale    libswscale.a)
cemu_switch_portlib(FFmpeg::avcodec     libavcodec.a)
cemu_switch_portlib(FFmpeg::dav1d       libdav1d.a)
set_property(TARGET FFmpeg::swresample APPEND PROPERTY INTERFACE_LINK_LIBRARIES
	FFmpeg::avutil)
set_property(TARGET FFmpeg::avcodec APPEND PROPERTY INTERFACE_LINK_LIBRARIES
	FFmpeg::dav1d FFmpeg::swresample FFmpeg::swscale FFmpeg::avutil ZLIB::ZLIB Threads::Threads)
set_property(TARGET FFmpeg::swscale APPEND PROPERTY INTERFACE_LINK_LIBRARIES
	FFmpeg::avutil)

# Boost built by dist/switch/deps/build_boost.sh. Keep the portlibs fallback for
# existing development environments.
set(SWITCH_LOCAL_BOOST "${CMAKE_SOURCE_DIR}/dependencies/switch_deps/boost")
set(SWITCH_LOCAL_BOOST_INCLUDE "${CMAKE_SOURCE_DIR}/dependencies/switch_deps/src/boost_1_86_0")
set(SWITCH_BOOST_PREFIX "${SWITCH_LOCAL_BOOST}" CACHE PATH "Switch Boost installation")
set(SWITCH_BOOST_INCLUDE_DIR "${SWITCH_LOCAL_BOOST_INCLUDE}" CACHE PATH "Switch Boost headers")
set(_switch_boost_libs
	"lib/libboost_filesystem.a"
	"lib/libboost_program_options.a")

set(_switch_boost_complete ON)
if(NOT EXISTS "${SWITCH_BOOST_INCLUDE_DIR}/boost/version.hpp")
	set(_switch_boost_complete OFF)
endif()
foreach(_file IN LISTS _switch_boost_libs)
	if(NOT EXISTS "${SWITCH_BOOST_PREFIX}/${_file}")
		set(_switch_boost_complete OFF)
	endif()
endforeach()
if(NOT _switch_boost_complete
	AND "${SWITCH_BOOST_PREFIX}" STREQUAL "${SWITCH_LOCAL_BOOST}"
	AND "${SWITCH_BOOST_INCLUDE_DIR}" STREQUAL "${SWITCH_LOCAL_BOOST_INCLUDE}")
	set(_switch_portlibs_boost_complete ON)
	if(NOT EXISTS "${SWITCH_PORTLIBS}/include/boost/version.hpp")
		set(_switch_portlibs_boost_complete OFF)
	endif()
	foreach(_file IN LISTS _switch_boost_libs)
		if(NOT EXISTS "${SWITCH_PORTLIBS}/${_file}")
			set(_switch_portlibs_boost_complete OFF)
		endif()
	endforeach()
	if(_switch_portlibs_boost_complete)
		set(SWITCH_BOOST_PREFIX "${SWITCH_PORTLIBS}")
		set(SWITCH_BOOST_INCLUDE_DIR "${SWITCH_PORTLIBS}/include")
		set(_switch_boost_complete ON)
		message(STATUS "Using legacy Boost installation from ${SWITCH_PORTLIBS}")
	endif()
endif()
if(NOT _switch_boost_complete)
	message(FATAL_ERROR
		"Boost for Switch is incomplete at ${SWITCH_BOOST_PREFIX}.\n"
		"Run: bash dist/switch/deps/build_boost.sh")
endif()

add_library(Boost::headers INTERFACE IMPORTED)
set_target_properties(Boost::headers PROPERTIES
	INTERFACE_INCLUDE_DIRECTORIES "${SWITCH_BOOST_INCLUDE_DIR}")

cemu_switch_static_import(Boost::program_options "${SWITCH_BOOST_PREFIX}" libboost_program_options.a "${SWITCH_BOOST_INCLUDE_DIR}")
set_property(TARGET Boost::program_options APPEND PROPERTY INTERFACE_LINK_LIBRARIES Boost::headers)
cemu_switch_static_import(Boost::filesystem "${SWITCH_BOOST_PREFIX}" libboost_filesystem.a "${SWITCH_BOOST_INCLUDE_DIR}")
set_property(TARGET Boost::filesystem APPEND PROPERTY INTERFACE_LINK_LIBRARIES Boost::headers)

# nowide is header-only on Switch.
add_library(Boost::nowide INTERFACE IMPORTED)
set_property(TARGET Boost::nowide APPEND PROPERTY INTERFACE_LINK_LIBRARIES Boost::headers)

# fmt
FetchContent_Declare(fmt
	GIT_REPOSITORY https://github.com/fmtlib/fmt.git
	GIT_TAG        407c905e45ad75fc29bf0f9bb7c5c2fd3475976f) # 12.1.0
FetchContent_MakeAvailable(fmt)

# glm
FetchContent_Declare(glm
	GIT_REPOSITORY https://github.com/g-truc/glm.git
	GIT_TAG        0af55ccecd98d4e5a8d1fad7de25ba429d60e863) # 1.0.1
FetchContent_MakeAvailable(glm)
include_directories(SYSTEM "${glm_SOURCE_DIR}")

# RapidJSON's source tree is populated without configuring its own build.
FetchContent_Declare(rapidjson
	GIT_REPOSITORY https://github.com/Tencent/rapidjson.git
	GIT_TAG        f54b0e47a08782a6131cc3d60f94d038fa6e0a51 # v1.1.0
	SOURCE_SUBDIR  __skip_rapidjson_cmake__)
FetchContent_MakeAvailable(rapidjson)
include_directories(SYSTEM "${rapidjson_SOURCE_DIR}/include")

# pugixml
FetchContent_Declare(pugixml
	GIT_REPOSITORY https://github.com/zeux/pugixml.git
	GIT_TAG        db78afc2b7d8f043b4bc6b185635d949ea2ed2a8) # v1.14
FetchContent_MakeAvailable(pugixml)
if(NOT TARGET pugixml::pugixml AND TARGET pugixml)
	add_library(pugixml::pugixml ALIAS pugixml)
endif()
# pugixml does not expose its source include path through the build interface.
include_directories(SYSTEM "${pugixml_SOURCE_DIR}/src")

# glslang
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
set(ENABLE_OPT OFF CACHE BOOL "" FORCE) # SPIRV-Tools is not vendored.
set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
set(ENABLE_PCH OFF CACHE BOOL "" FORCE) # MSYS cannot consume glslang's absolute PCH path.
FetchContent_Declare(glslang
	GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
	GIT_TAG        1062752a891c95b2bfeed9e356562d88f9df84ac) # 15.1.0
FetchContent_MakeAvailable(glslang)
if (NOT TARGET glslang::SPIRV AND TARGET SPIRV)
	add_library(glslang::SPIRV ALIAS SPIRV)
endif()
if (NOT TARGET glslang::glslang AND TARGET glslang)
	add_library(glslang::glslang ALIAS glslang)
endif()
# glslang headers are rooted at its source directory.
include_directories(SYSTEM "${glslang_SOURCE_DIR}")
set(glslang_VERSION "15.1.0")

# The top-level build adds ZArchive after package discovery fails.

# Mesa NVK Vulkan driver
set(NVK_DIR "${CMAKE_SOURCE_DIR}/dependencies/switch_mesa_vulkan")
set(NVK_LOCAL "${NVK_DIR}/libnvk_local.o")
if(NOT EXISTS "${NVK_LOCAL}")
	message(FATAL_ERROR
		"NVK driver object not found: ${NVK_LOCAL}\n"
		"Stage the required Mesa archives, then run dist/switch/localize_nvk.sh. "
		"See dist/switch/README.md.")
endif()

add_library(SwitchVulkanDriver INTERFACE)
target_include_directories(SwitchVulkanDriver INTERFACE
	"${CMAKE_SOURCE_DIR}/dependencies/Vulkan-Headers/include")
target_link_libraries(SwitchVulkanDriver INTERFACE
	"${NVK_LOCAL}"
	# Mesa 26.2 NVK parses cubin metadata through libelf.
	"${SWITCH_PORTLIBS}/lib/libelf.a"
	# The nouveau winsys provides NVHOST and NVMAP access.
	"${SWITCH_PORTLIBS}/lib/libdrm_nouveau.a")

add_compile_definitions(VK_USE_PLATFORM_VI_NN)

# Compatibility headers and OpenSSL shim
include_directories(SYSTEM "${CMAKE_SOURCE_DIR}/dependencies/switch_shims/include")

cemu_switch_portlib(mbedtls::mbedtls libmbedtls.a)
cemu_switch_portlib(mbedtls::mbedx509 libmbedx509.a)
cemu_switch_portlib(mbedtls::mbedcrypto libmbedcrypto.a)
set_property(TARGET mbedtls::mbedtls APPEND PROPERTY INTERFACE_LINK_LIBRARIES
	mbedtls::mbedx509 mbedtls::mbedcrypto)
set_property(TARGET mbedtls::mbedx509 APPEND PROPERTY INTERFACE_LINK_LIBRARIES
	mbedtls::mbedcrypto)

add_library(switch_openssl_compat STATIC
	"${CMAKE_SOURCE_DIR}/dependencies/switch_shims/src/openssl_compat.cpp"
	"${CMAKE_SOURCE_DIR}/dependencies/switch_shims/src/sect233r1.cpp")
target_link_libraries(switch_openssl_compat PUBLIC
	mbedtls::mbedtls mbedtls::mbedx509 mbedtls::mbedcrypto)

add_library(OpenSSL::Crypto ALIAS switch_openssl_compat)
add_library(OpenSSL::SSL ALIAS switch_openssl_compat)

# libcurl
cemu_switch_portlib(CURL::libcurl libcurl.a)
set_property(TARGET CURL::libcurl APPEND PROPERTY INTERFACE_LINK_LIBRARIES
	ZLIB::ZLIB mbedtls::mbedtls mbedtls::mbedx509 mbedtls::mbedcrypto)

# Switch storage backends used by both the launcher and emulation core.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ENABLE_LIBKRB5 OFF CACHE BOOL "" FORCE)
set(ENABLE_GSSAPI OFF CACHE BOOL "" FORCE)
FetchContent_Declare(libsmb2
	GIT_REPOSITORY https://github.com/ITotalJustice/libsmb2.git
	GIT_TAG 867beea093f2863dfddea01945204f724afd6c45)
FetchContent_MakeAvailable(libsmb2)
target_compile_options(smb2 PRIVATE -UNEED_READV -UNEED_WRITEV)
target_compile_definitions(smb2 PRIVATE AES128_ECB_encrypt=smb2_AES128_ECB_encrypt)

set(USBHSFS_GPL OFF CACHE BOOL "" FORCE)
set(USBHSFS_NTFS OFF CACHE BOOL "" FORCE)
set(USBHSFS_EXT4 OFF CACHE BOOL "" FORCE)
set(USBHSFS_SXOS_DISABLE ON CACHE BOOL "" FORCE)
set(USBHSFS_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(libusbhsfs
	GIT_REPOSITORY https://github.com/ITotalJustice/libusbhsfs.git
	GIT_TAG 625269b7725a6e2a3f2724e8d45b602c1b20ead5)
FetchContent_MakeAvailable(libusbhsfs)

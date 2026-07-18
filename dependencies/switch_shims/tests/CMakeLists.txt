cmake_minimum_required(VERSION 3.21)

project(CemuSwitchShimTests LANGUAGES CXX)

include(CTest)

option(CEMU_SWITCH_SHIM_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

find_package(OpenSSL 1.1.1 REQUIRED COMPONENTS Crypto)

add_executable(cemu_switch_sect233r1_test
	sect233r1_test.cpp
	../src/sect233r1.cpp)

target_compile_features(cemu_switch_sect233r1_test PRIVATE cxx_std_17)
target_compile_definitions(cemu_switch_sect233r1_test PRIVATE OPENSSL_SUPPRESS_DEPRECATED)
target_link_libraries(cemu_switch_sect233r1_test PRIVATE OpenSSL::Crypto)

if(MSVC)
	target_compile_options(cemu_switch_sect233r1_test PRIVATE /W4)
else()
	target_compile_options(cemu_switch_sect233r1_test PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(CEMU_SWITCH_SHIM_SANITIZERS)
	if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
		target_compile_options(cemu_switch_sect233r1_test PRIVATE
			-fsanitize=address,undefined
			-fno-omit-frame-pointer)
		target_link_options(cemu_switch_sect233r1_test PRIVATE
			-fsanitize=address,undefined
			-fno-omit-frame-pointer)
	else()
		message(FATAL_ERROR "CEMU_SWITCH_SHIM_SANITIZERS requires Clang or GCC")
	endif()
endif()

if(BUILD_TESTING)
	add_test(NAME switch_shims.sect233r1 COMMAND cemu_switch_sect233r1_test)
endif()

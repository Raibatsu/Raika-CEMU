add_library(CemuResource)

set_property(TARGET CemuResource PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

cemu_use_precompiled_header(CemuResource)

enable_language(ASM)

# icon resources
target_sources(CemuResource PRIVATE
embedded/resources.cpp
embedded/resources.h
)

if(UNIX OR PLATFORM_SWITCH)
    if(NOT APPLE)
        target_sources(CemuResource PRIVATE embedded/fontawesome.S)
        # .incbin resolves through assembler include paths.
        set_property(SOURCE embedded/fontawesome.S APPEND PROPERTY COMPILE_OPTIONS
            "-I${CMAKE_CURRENT_SOURCE_DIR}/embedded")
    else()
        target_sources(CemuResource PRIVATE embedded/fontawesome_macos.S)
    endif()
endif()

target_sources(CemuResource PRIVATE CafeDefaultFont.cpp)

target_include_directories(CemuResource PUBLIC "../")

target_link_libraries(CemuResource PRIVATE CemuCommon)

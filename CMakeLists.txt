cmake_minimum_required(VERSION 3.24)

project(PixelVRPlayer
    VERSION 0.1.0
    DESCRIPTION "Native VR video player for the Valve Steam Frame"
    LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(PIXELVR_ENABLE_MEDIA "Build the FFmpeg-based media engine" ON)
option(PIXELVR_ENABLE_VALIDATION "Enable Vulkan validation layers in debug" ON)
set(PIXELVR_OPENXR_TAG "release-1.1.63" CACHE STRING
    "Git tag of KhronosGroup/OpenXR-SDK to fetch (adjust if unavailable)")

# ---------------------------------------------------------------------------
# Global settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# Position-independent code; the app is loaded by the Steam runtime loader.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
find_package(Vulkan REQUIRED)
find_package(Threads REQUIRED)

include(FetchContent)

# OpenXR loader + headers (built from source so we control the version and get a
# known-good loader that resolves the active runtime, e.g. SteamVR on the Frame).
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)

# Build the loader statically into the executable so the deployed binary is
# self-contained (no libopenxr_loader.so to ship). The static loader still
# resolves the active runtime (SteamVR) at launch.
set(DYNAMIC_LOADER OFF CACHE BOOL "" FORCE)

# Always use the loader's bundled jsoncpp (compiled in) rather than a system one.
# With a system jsoncpp, the loader links an INTERFACE target (jsoncpp_interface)
# that leaks into the static loader's export set and fails the generate step on
# newer CMake (e.g. SteamOS's 3.29). Bundling avoids that and stays portable.
set(BUILD_WITH_SYSTEM_JSONCPP OFF CACHE BOOL "" FORCE)

# This is a headless Vulkan XR client: it needs no XCB/Wayland presentation
# support in the loader. Keep only the xlib backend (the loader requires one on
# Linux) to minimize X dependencies for both native and aarch64 builds.
set(PRESENTATION_BACKEND "xlib" CACHE STRING "" FORCE)
set(BUILD_WITH_XCB_HEADERS OFF CACHE BOOL "" FORCE)
set(BUILD_WITH_WAYLAND_HEADERS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(OpenXR
    GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
    GIT_TAG ${PIXELVR_OPENXR_TAG}
    GIT_SHALLOW ON)
FetchContent_MakeAvailable(OpenXR)

# ---------------------------------------------------------------------------
# Shader compilation (GLSL -> SPIR-V at build time)
# ---------------------------------------------------------------------------
include(cmake/CompileShaders.cmake)

set(PIXELVR_SHADERS
    shaders/quad.vert
    shaders/quad.frag
    shaders/sphere.vert
    shaders/sphere.frag
    shaders/video_quad.frag
    shaders/video_sphere.frag
    shaders/resolve.vert
    shaders/resolve.frag
    shaders/model.vert
    shaders/model.frag
    shaders/ui.vert
    shaders/ui.frag
    shaders/text.vert
    shaders/text.frag)
pixelvr_compile_shaders(pixelvr_shaders
    OUTPUT_DIR "${CMAKE_BINARY_DIR}/shaders"
    SOURCES ${PIXELVR_SHADERS})

# ---------------------------------------------------------------------------
# Core library
# ---------------------------------------------------------------------------
add_library(pixelvr_core STATIC
    src/util/logging.cpp
    src/xr/xr_result.cpp
    src/xr/xr_context.cpp
    src/xr/render_model.cpp
    src/gfx/vulkan_renderer.cpp
    src/ui/settings_menu.cpp
    src/app/app.cpp)

target_include_directories(pixelvr_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_compile_definitions(pixelvr_core PUBLIC
    XR_USE_GRAPHICS_API_VULKAN
    PIXELVR_SHADER_DIR=\"${CMAKE_BINARY_DIR}/shaders\"
    $<$<AND:$<CONFIG:Debug,RelWithDebInfo>,$<BOOL:${PIXELVR_ENABLE_VALIDATION}>>:PIXELVR_USE_VALIDATION=1>)

target_link_libraries(pixelvr_core PUBLIC
    OpenXR::openxr_loader
    Vulkan::Vulkan
    Threads::Threads)

add_dependencies(pixelvr_core pixelvr_shaders)

if(PIXELVR_ENABLE_MEDIA)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
        libavcodec libavformat libavutil libswscale libswresample)
    target_sources(pixelvr_core PRIVATE src/media/media_engine.cpp)
    target_link_libraries(pixelvr_core PUBLIC PkgConfig::FFMPEG)
    target_compile_definitions(pixelvr_core PUBLIC PIXELVR_HAVE_MEDIA=1)

    # Audio output via the PulseAudio simple API (PipeWire provides this on
    # SteamOS). Optional: video-only playback if unavailable.
    pkg_check_modules(PULSE IMPORTED_TARGET libpulse-simple)
    if(PULSE_FOUND)
        target_link_libraries(pixelvr_core PUBLIC PkgConfig::PULSE)
        target_compile_definitions(pixelvr_core PUBLIC PIXELVR_HAVE_PULSE=1)
    endif()
endif()

# In-VR file browser: text rendering (FreeType) and SMB access (libsmbclient).
find_package(PkgConfig)
if(PkgConfig_FOUND)
    pkg_check_modules(FREETYPE IMPORTED_TARGET freetype2)
    if(FREETYPE_FOUND)
        target_sources(pixelvr_core PRIVATE
            src/gfx/text_atlas.cpp
            src/ui/browser.cpp)
        target_link_libraries(pixelvr_core PUBLIC PkgConfig::FREETYPE)
        target_compile_definitions(pixelvr_core PUBLIC PIXELVR_HAVE_FREETYPE=1)
    endif()

    pkg_check_modules(SMBCLIENT IMPORTED_TARGET smbclient)
    if(SMBCLIENT_FOUND)
        target_link_libraries(pixelvr_core PUBLIC PkgConfig::SMBCLIENT)
        target_compile_definitions(pixelvr_core PUBLIC PIXELVR_HAVE_SMB=1)
    endif()
endif()

# Warnings
target_compile_options(pixelvr_core PRIVATE
    -Wall -Wextra -Wpedantic -Wno-unused-parameter
    -Wno-missing-field-initializers)

# ---------------------------------------------------------------------------
# Executable
# ---------------------------------------------------------------------------
add_executable(pixelvr src/main.cpp)
target_link_libraries(pixelvr PRIVATE pixelvr_core)

install(TARGETS pixelvr RUNTIME DESTINATION bin)
install(DIRECTORY "${CMAKE_BINARY_DIR}/shaders/" DESTINATION bin/shaders)

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
message(STATUS "PixelVR configuration:")
message(STATUS "  System         : ${CMAKE_SYSTEM_NAME} / ${CMAKE_SYSTEM_PROCESSOR}")
message(STATUS "  Build type     : ${CMAKE_BUILD_TYPE}")
message(STATUS "  OpenXR tag     : ${PIXELVR_OPENXR_TAG}")
message(STATUS "  Media engine   : ${PIXELVR_ENABLE_MEDIA}")
message(STATUS "  Vulkan         : ${Vulkan_VERSION}")

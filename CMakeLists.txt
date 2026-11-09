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

# Single-header glTF/GLB parser for controller render models. Header-only, so
# populate it without add_subdirectory (we only need the include path).
FetchContent_Declare(cgltf
    GIT_REPOSITORY https://github.com/jkuhlmann/cgltf.git
    GIT_TAG v1.14
    GIT_SHALLOW ON)
FetchContent_GetProperties(cgltf)
if(NOT cgltf_POPULATED)
    FetchContent_Populate(cgltf)
endif()

# Single-header image decoder (PNG/JPEG) for render-model base-color textures.
FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG master
    GIT_SHALLOW ON)
FetchContent_GetProperties(stb)
if(NOT stb_POPULATED)
    FetchContent_Populate(stb)
endif()

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
    shaders/text.frag
    shaders/thumb.vert
    shaders/thumb.frag
    shaders/ambilight.frag
    shaders/ambilight_nv12.frag
    shaders/glass.vert
    shaders/glass_nv12.frag
    shaders/glass_rgba.frag
    shaders/mask.vert
    shaders/mask.frag)
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
    src/xr/vr_keyboard.cpp
    src/xr/vr_app_manifest.cpp
    src/net/smb_credentials.cpp
    src/net/remote_fs.cpp
    src/gfx/vulkan_renderer.cpp
    src/gfx/gltf_mesh.cpp
    src/ui/settings_menu.cpp
    src/ui/button_labels.cpp
    src/ui/thumbnail_cache.cpp
    src/app/vault_manager.cpp
    src/app/playback_memory.cpp
    src/app/browser_host.cpp
    src/plugins/plugin_host.cpp
    src/plugins/plugin_files.cpp
    src/plugins/plugin_repo.cpp
    src/app/chromium_installer.cpp
    src/app/app.cpp)

target_include_directories(pixelvr_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/openvr")
target_include_directories(pixelvr_core PRIVATE "${cgltf_SOURCE_DIR}" "${stb_SOURCE_DIR}")

# Version string shown in the UI: $PIXELVR_VERSION if set, else the nearest git
# tag (evaluated at configure time), else the project version.
# Release builds pass the tag explicitly (CI checks out shallowly).
find_package(Git QUIET)
set(PIXELVR_VERSION_STRING "v${PROJECT_VERSION}")
if(DEFINED ENV{PIXELVR_VERSION} AND NOT "$ENV{PIXELVR_VERSION}" STREQUAL "")
    set(PIXELVR_VERSION_STRING "$ENV{PIXELVR_VERSION}")
elseif(GIT_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" describe --tags --abbrev=0
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE _pixelvr_git_tag
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _pixelvr_git_result)
    if(_pixelvr_git_result EQUAL 0 AND _pixelvr_git_tag)
        set(PIXELVR_VERSION_STRING "${_pixelvr_git_tag}")
    endif()
endif()

# Bundled UI fonts (Inter, SIL OFL) are copied next to the build output; the
# installers ship them in fonts/ beside the binary.
file(GLOB PIXELVR_FONT_FILES "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/*.ttf")
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/fonts")
foreach(font ${PIXELVR_FONT_FILES})
    get_filename_component(font_name "${font}" NAME)
    configure_file("${font}" "${CMAKE_BINARY_DIR}/fonts/${font_name}" COPYONLY)
endforeach()

target_compile_definitions(pixelvr_core PUBLIC
    XR_USE_GRAPHICS_API_VULKAN
    PIXELVR_SHADER_DIR=\"${CMAKE_BINARY_DIR}/shaders\"
    PIXELVR_FONT_DIR=\"${CMAKE_BINARY_DIR}/fonts\"
    PIXELVR_VERSION_STRING=\"${PIXELVR_VERSION_STRING}\"
    $<$<AND:$<CONFIG:Debug,RelWithDebInfo>,$<BOOL:${PIXELVR_ENABLE_VALIDATION}>>:PIXELVR_USE_VALIDATION=1>)

target_link_libraries(pixelvr_core PUBLIC
    OpenXR::openxr_loader
    Vulkan::Vulkan
    Threads::Threads
    ${CMAKE_DL_LIBS})

add_dependencies(pixelvr_core pixelvr_shaders)

if(PIXELVR_ENABLE_MEDIA)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
        libavcodec libavformat libavutil libswscale libswresample)
    target_sources(pixelvr_core PRIVATE src/media/media_engine.cpp src/media/scrub_preview.cpp
                                        src/media/cover_art.cpp src/media/stereo_detect.cpp
                                        src/app/music_queue.cpp)
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

    # SFTP (libssh) and WebDAV (libcurl) shares; both optional like smbclient.
    pkg_check_modules(LIBSSH IMPORTED_TARGET libssh)
    if(LIBSSH_FOUND)
        target_link_libraries(pixelvr_core PUBLIC PkgConfig::LIBSSH)
        target_compile_definitions(pixelvr_core PUBLIC PIXELVR_HAVE_LIBSSH=1)
    endif()
    pkg_check_modules(LIBCURL IMPORTED_TARGET libcurl)
    if(LIBCURL_FOUND)
        target_link_libraries(pixelvr_core PUBLIC PkgConfig::LIBCURL)
        target_compile_definitions(pixelvr_core PUBLIC PIXELVR_HAVE_CURL=1)
    endif()
endif()

# Warnings
target_compile_options(pixelvr_core PRIVATE
    -Wall -Wextra -Wpedantic -Wno-unused-parameter
    -Wno-missing-field-initializers)

# ---------------------------------------------------------------------------
# Executable
# ---------------------------------------------------------------------------
add_executable(pixelvr src/main.cpp src/app/bundled_ffmpeg.cpp)
target_link_libraries(pixelvr PRIVATE pixelvr_core)

# Release builds embed the player's own FFmpeg 8.0 (third_party/ffmpeg:
# zero-copy dma-buf decode, AV1 via dav1d); the player unpacks the libraries at
# start and re-executes itself with them (src/app/bundled_ffmpeg.cpp), so it
# does not depend on the FFmpeg release SteamOS ships. Point this at the
# install prefix written by third_party/ffmpeg/build.sh (contains lib/).
set(PIXELVR_EMBED_FFMPEG_DIR "" CACHE PATH "FFmpeg install prefix to embed (release builds)")
# name;soname pairs: the file names the loader writes out must match the
# DT_NEEDED entries of the player binary, i.e. the FFmpeg release it was built
# against (8.0: avcodec/avformat 62, avutil 60, swscale 9, swresample 6).
set(PIXELVR_FFMPEG_LIBS
    avcodec:libavcodec.so.62 avformat:libavformat.so.62 avutil:libavutil.so.60
    swscale:libswscale.so.9 swresample:libswresample.so.6)
if(PIXELVR_EMBED_FFMPEG_DIR)
    set(_ff_files "")
    set(_ff_asm "${CMAKE_BINARY_DIR}/bundled_ffmpeg_data.S")
    set(_ff_text "  .section .rodata\n")
    set(_ff_names "")
    foreach(_pair IN LISTS PIXELVR_FFMPEG_LIBS)
        string(REPLACE ":" ";" _pair "${_pair}")
        list(GET _pair 0 _name)
        list(GET _pair 1 _so)
        set(_f "${PIXELVR_EMBED_FFMPEG_DIR}/lib/${_so}")
        if(NOT EXISTS "${_f}")
            message(FATAL_ERROR "PIXELVR_EMBED_FFMPEG_DIR: missing ${_f}")
        endif()
        list(APPEND _ff_files "${_f}")
        string(APPEND _ff_names "${_so},")
        string(APPEND _ff_text
            "  .balign 16\n  .global pvr_ffmpeg_${_name}_start\npvr_ffmpeg_${_name}_start:\n"
            "  .incbin \"${_f}\"\n"
            "  .global pvr_ffmpeg_${_name}_end\npvr_ffmpeg_${_name}_end:\n")
    endforeach()
    file(SHA256 "${PIXELVR_EMBED_FFMPEG_DIR}/lib/libavcodec.so.62" _ff_hash)
    string(SUBSTRING "${_ff_hash}" 0 16 _ff_id)
    string(APPEND _ff_text "  .section .note.GNU-stack,\"\",%progbits\n")
    file(WRITE "${_ff_asm}" "${_ff_text}")
    enable_language(ASM)
    target_sources(pixelvr PRIVATE "${_ff_asm}")
    set_source_files_properties("${_ff_asm}" PROPERTIES OBJECT_DEPENDS "${_ff_files}")
    target_compile_definitions(pixelvr PRIVATE PIXELVR_EMBED_FFMPEG=1
        PIXELVR_FFMPEG_BUNDLE_ID="${_ff_id}"
        PIXELVR_FFMPEG_SONAMES="${_ff_names}")
    message(STATUS "Embedding FFmpeg ${_ff_id} from ${PIXELVR_EMBED_FFMPEG_DIR}")
endif()

install(TARGETS pixelvr RUNTIME DESTINATION bin)
install(DIRECTORY "${CMAKE_BINARY_DIR}/shaders/" DESTINATION bin/shaders)

# Browser screen helper: drives the user's installed Chromium over the DevTools
# protocol. Built into the player (`pixelvr --pvr-browser`, so installs and
# updates need no extra file) and as a standalone pvr-browser for development.
# Needs libjpeg-turbo (part of SteamOS).
find_library(PIXELVR_TURBOJPEG_LIB turbojpeg)
find_path(PIXELVR_TURBOJPEG_INCLUDE turbojpeg.h)
if(UNIX AND PIXELVR_TURBOJPEG_LIB AND PIXELVR_TURBOJPEG_INCLUDE)
    target_sources(pixelvr PRIVATE browser/pvr_browser.cpp)
    target_include_directories(pixelvr PRIVATE "${PIXELVR_TURBOJPEG_INCLUDE}")
    target_link_libraries(pixelvr PRIVATE "${PIXELVR_TURBOJPEG_LIB}" rt)
    target_compile_definitions(pixelvr PRIVATE PIXELVR_HAVE_BROWSER_HELPER=1)
    target_compile_definitions(pixelvr_core PRIVATE PIXELVR_HAVE_BROWSER_HELPER=1)

    add_executable(pvr-browser browser/pvr_browser.cpp)
    target_compile_definitions(pvr-browser PRIVATE PVR_BROWSER_STANDALONE=1)
    target_include_directories(pvr-browser PRIVATE "${PIXELVR_TURBOJPEG_INCLUDE}")
    target_link_libraries(pvr-browser PRIVATE "${PIXELVR_TURBOJPEG_LIB}" rt)
else()
    message(STATUS "pvr-browser: libjpeg-turbo not found, browser screen disabled")
endif()

# Development tool for the SFTP / WebDAV access layer (never part of a release).
option(PIXELVR_BUILD_TOOLS "Build development tools (tools/remote_fs_test)" OFF)
if(PIXELVR_BUILD_TOOLS)
    add_executable(remote_fs_test tools/remote_fs_test.cpp)
    target_link_libraries(remote_fs_test PRIVATE pixelvr_core)
    add_executable(plugin_host_test tools/plugin_host_test.cpp)
    target_link_libraries(plugin_host_test PRIVATE pixelvr_core)
    add_executable(plugin_pentest tools/plugin_pentest.cpp)
    target_link_libraries(plugin_pentest PRIVATE pixelvr_core)
endif()

# First-party plugins (docs/concepts/PLUGIN_SYSTEM_SPEC.md). Each lands in
# build/plugins/<name>/ next to its plugin.json; they are released as separate
# .pvrplugin downloads, never inside the player package.
option(PIXELVR_BUILD_PLUGINS "Build the first-party plugins (plugins/)" ON)
if(PIXELVR_BUILD_PLUGINS AND UNIX)
    foreach(plugin example funscript deovr-remote)
        add_executable(pvr-plugin-${plugin} plugins/${plugin}/main.cpp)
        set_target_properties(pvr-plugin-${plugin} PROPERTIES
            OUTPUT_NAME ${plugin}
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/${plugin}")
        configure_file(plugins/${plugin}/plugin.json "${CMAKE_BINARY_DIR}/plugins/${plugin}/plugin.json" COPYONLY)
    endforeach()
endif()

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
message(STATUS "PixelVR configuration:")
message(STATUS "  System         : ${CMAKE_SYSTEM_NAME} / ${CMAKE_SYSTEM_PROCESSOR}")
message(STATUS "  Build type     : ${CMAKE_BUILD_TYPE}")
message(STATUS "  OpenXR tag     : ${PIXELVR_OPENXR_TAG}")
message(STATUS "  Media engine   : ${PIXELVR_ENABLE_MEDIA}")
message(STATUS "  Vulkan         : ${Vulkan_VERSION}")

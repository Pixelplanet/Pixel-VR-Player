#!/usr/bin/env bash
# Configure and build Pixel VR Player.
#
#   ./tools/build.sh            # native host build (desktop, SteamVR-streamed dev)
#   ./tools/build.sh --aarch64  # cross-compile for the Steam Frame
#
# For --aarch64, set PIXELVR_SYSROOT to an aarch64 sysroot containing the Vulkan
# loader (and FFmpeg once the media engine is enabled).
set -euo pipefail

cd "$(dirname "$0")/.."

BUILD_DIR="build"
CMAKE_ARGS=(-G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo)

if [[ "${1:-}" == "--aarch64" ]]; then
    BUILD_DIR="build-aarch64"
    CMAKE_ARGS+=(-DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64-linux.cmake)
    # PIXELVR_SYSROOT is optional: set it to a device-captured sysroot
    # (tools/fetch-sysroot.sh) for a runtime-compatible build, otherwise the
    # toolchain falls back to Debian/Ubuntu arm64 multiarch libraries.
    if [[ -n "${PIXELVR_SYSROOT:-}" ]]; then
        CMAKE_ARGS+=(-DPIXELVR_SYSROOT="${PIXELVR_SYSROOT}")
    fi
    shift
fi

cmake -B "${BUILD_DIR}" "${CMAKE_ARGS[@]}" "$@"
cmake --build "${BUILD_DIR}" --parallel

echo
echo "Built ${BUILD_DIR}/bin/pixelvr"

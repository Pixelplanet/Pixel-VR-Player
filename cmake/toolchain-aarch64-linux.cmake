# CMake toolchain for cross-compiling to aarch64 Linux (Steam Frame / SteamOS).
#
# Usage:
#   cmake -B build-aarch64 -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64-linux.cmake \
#         -DPIXELVR_SYSROOT=/path/to/aarch64-sysroot
#
# The sysroot must contain the aarch64 Vulkan loader (libvulkan.so) and, once the
# media engine is enabled, the FFmpeg libraries. A convenient way to obtain one is
# to extract it from the Steam Linux Runtime 3.0 "Sniper" aarch64 SDK container, or
# to rsync /usr and /lib from the headset over ssh.
#
# Requires the cross toolchain:
#   sudo apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(_triple aarch64-linux-gnu)

set(CMAKE_C_COMPILER ${_triple}-gcc)
set(CMAKE_CXX_COMPILER ${_triple}-g++)

# aarch64 libraries live under the multiarch triple directory.
set(CMAKE_LIBRARY_ARCHITECTURE ${_triple})

# Never search the host for target programs.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)

if(DEFINED PIXELVR_SYSROOT)
    # Preferred: build against a sysroot captured from the device
    # (tools/fetch-sysroot.sh) so the binary links against the exact FFmpeg,
    # Vulkan and X11 SONAMEs present on the Steam Frame.
    set(CMAKE_SYSROOT "${PIXELVR_SYSROOT}")
    set(CMAKE_FIND_ROOT_PATH "${PIXELVR_SYSROOT}")
    set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
    set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
    set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
    set(ENV{PKG_CONFIG_DIR} "")
    set(ENV{PKG_CONFIG_LIBDIR}
        "${PIXELVR_SYSROOT}/usr/lib/${_triple}/pkgconfig:${PIXELVR_SYSROOT}/usr/lib/pkgconfig:${PIXELVR_SYSROOT}/usr/share/pkgconfig")
    set(ENV{PKG_CONFIG_SYSROOT_DIR} "${PIXELVR_SYSROOT}")
else()
    # Fallback: Debian/Ubuntu multiarch. Install the :arm64 -dev packages and
    # link against /usr/lib/aarch64-linux-gnu. Runtime compatibility with the
    # device depends on matching library versions.
    set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
    set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
    set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
    set(ENV{PKG_CONFIG_LIBDIR} "/usr/lib/${_triple}/pkgconfig:/usr/share/pkgconfig")
endif()

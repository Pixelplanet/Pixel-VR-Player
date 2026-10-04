#!/bin/sh
# Run Pixel VR Player directly on the Steam Frame, outside Steam (e.g. over SSH),
# selecting the native SteamVR OpenXR runtime. Steam-launched shortcuts get this
# environment set up automatically, so this helper is only needed for manual runs.
#
# It is deployed next to the binary in ~/devkit-game/PixelVRPlayer by deploy.py.
#
#   ./launch.sh /home/steamos/Videos/clip_360_tb.mp4
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

# Point the OpenXR loader at SteamVR's ARM64 runtime and its libraries.
export XR_RUNTIME_JSON="${XR_RUNTIME_JSON:-/opt/steamvr/steamxr_linuxarm64.json}"
export LD_LIBRARY_PATH="/opt/steamvr/bin/linuxarm64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# Let the app find its shaders regardless of the working directory.
export PIXELVR_SHADER_DIR="${PIXELVR_SHADER_DIR:-$here/shaders}"

exec "$here/pixelvr" "$@"

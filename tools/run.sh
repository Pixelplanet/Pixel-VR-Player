#!/bin/sh
# Pixel VR Player launcher (Steam Frame). Logs to ~/pixelvr-run.log.
#
# This is the launcher shipped inside the release tarball as
# PixelVRPlayer/run.sh. It runs the app directly on the SteamOS host.
set -eu

here=$(cd "$(dirname "$0")" && pwd)

export PIXELVR_SHADER_DIR="$here/shaders"
export PIXELVR_LOG=debug

# Optional: use a locally built patched FFmpeg for zero-copy dma-buf decode.
if [ -d "$HOME/ffbuild/build/libavcodec" ]; then
    export LD_LIBRARY_PATH="$HOME/ffbuild/build/libavcodec:$HOME/ffbuild/build/libavutil:$HOME/ffbuild/build/libswresample${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    export PIXELVR_DRM_PRIME=1
fi

exec "$here/pixelvr" "$@" >>"$HOME/pixelvr-run.log" 2>&1

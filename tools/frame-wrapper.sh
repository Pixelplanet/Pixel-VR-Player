#!/bin/sh
# Steam Frame launch wrapper for Pixel VR Player.
#
# Logs to ~/pixelvr-run.log and plays the clip named in ~/media/which.txt
# (empty/absent -> blank scene). Repoint playback by editing that file.
#
# Zero-copy dma-buf video: prepend the patched libavcodec (drm_prime export) and
# enable the Vulkan YCbCr import path. Comment out the LD_LIBRARY_PATH and
# PIXELVR_DRM_PRIME exports below to fall back to system libavcodec + CPU NV12.
here=$(cd "$(dirname "$0")" && pwd)
export PIXELVR_LOG=debug
export LD_LIBRARY_PATH="$HOME/ffbuild/build/libavcodec:$HOME/ffbuild/build/libavutil:$HOME/ffbuild/build/libswresample${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export PIXELVR_DRM_PRIME=1
vid=$(cat "$HOME/media/which.txt" 2>/dev/null)
if [ -n "$vid" ] && [ -f "$vid" ]; then
    exec "$here/pixelvr.real" "$vid" >"$HOME/pixelvr-run.log" 2>&1
else
    exec "$here/pixelvr.real" >"$HOME/pixelvr-run.log" 2>&1
fi

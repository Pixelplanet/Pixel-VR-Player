#!/bin/sh
# Pixel VR Player launcher (Steam Frame). Logs to ~/.local/state/pixelvr/run.log.
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

logdir="${XDG_STATE_HOME:-$HOME/.local/state}/pixelvr"
mkdir -p "$logdir"
# Logs used to sit in the home folder: move them once.
[ -f "$HOME/pixelvr-run.log" ] && mv -f "$HOME/pixelvr-run.log" "$logdir/run.log.old" 2>/dev/null
[ -f "$HOME/pixelvr-run.log.old" ] && rm -f "$HOME/pixelvr-run.log.old"
# Start a fresh log once it passes 20 MB (the previous one is kept as run.log.old).
if [ -f "$logdir/run.log" ] && [ "$(stat -c %s "$logdir/run.log" 2>/dev/null || echo 0)" -gt 20000000 ]; then
    mv -f "$logdir/run.log" "$logdir/run.log.old"
fi
exec "$here/pixelvr" "$@" >>"$logdir/run.log" 2>&1

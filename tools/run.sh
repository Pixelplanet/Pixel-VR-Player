#!/bin/sh
# Pixel VR Player launcher (Steam Frame). Logs to logs/run.log in the player folder.
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

logdir="$here/logs"
mkdir -p "$logdir"
# Logs used to sit in the home folder (and briefly in ~/.local/state/pixelvr): move them once.
for old in "$HOME/pixelvr-run.log" "$HOME/.local/state/pixelvr/run.log"; do
    if [ -f "$old" ]; then mv -f "$old" "$logdir/run.log.old" 2>/dev/null || true; fi
done
for old in "$HOME/.local/state/pixelvr/install.log" "$HOME/pixelvr-install.log"; do
    if [ -f "$old" ] && [ ! -f "$logdir/install.log" ]; then mv -f "$old" "$logdir/install.log" 2>/dev/null || true; fi
done
# (set -eu above: every tidy-up step must be allowed to fail, or the player never starts)
rm -f "$HOME/pixelvr-run.log.old" "$HOME/.local/state/pixelvr/run.log.old" 2>/dev/null || true
rmdir "$HOME/.local/state/pixelvr" 2>/dev/null || true
# Start a fresh log once it passes 20 MB (the previous one is kept as run.log.old).
if [ -f "$logdir/run.log" ] && [ "$(stat -c %s "$logdir/run.log" 2>/dev/null || echo 0)" -gt 20000000 ]; then
    mv -f "$logdir/run.log" "$logdir/run.log.old" || true
fi
exec "$here/pixelvr" "$@" >>"$logdir/run.log" 2>&1

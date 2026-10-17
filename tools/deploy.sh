#!/usr/bin/env bash
# Deploy Pixel VR Player to a Steam Frame in Developer Mode.
#
# Thin wrapper around tools/deploy.py, which copies the binary + shaders into
# ~/devkit-game/PixelVRPlayer and registers a Non-Steam "Devkit Game" that runs
# directly on the host (no Sniper container).
#
#   ./tools/deploy.sh [--launch] [--host steamos@frame] [-- <player args>]
#
# Examples:
#   ./tools/deploy.sh --launch
#   ./tools/deploy.sh --launch -- /home/steamos/Videos/clip_360_tb.mp4
set -euo pipefail

cd "$(dirname "$0")/.."
exec python3 tools/deploy.py "$@"

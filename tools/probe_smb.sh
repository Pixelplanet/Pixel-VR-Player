#!/bin/sh
# Headless SMB streaming test: runs the media probe against a file on the NAS to
# verify smb:// streaming + decode without the headset. Usage: probe_smb.sh [url]
set -e

BIN="$HOME/pixelvr-src/build/bin/pixelvr"
LOADER_DIR="$HOME/pixelvr-src/build/_deps/openxr-build/src/loader"
export LD_LIBRARY_PATH="$LOADER_DIR:$LD_LIBRARY_PATH"
export PIXELVR_LOG=debug

URL="${1:-smb://192.168.178.10/Filme/M3GAN (2022)/M3GAN (2022) - 2022 - 2160p - HEVC - MLP.mkv}"

echo "probe url: $URL"
"$BIN" --probe "$URL" 2>&1 | grep -aiE "probe:|drm frame:|POLLERR|Decoder|codec=|error"

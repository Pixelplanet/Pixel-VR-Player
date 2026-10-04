#!/usr/bin/env bash
# Assemble a clean upload folder (binary + shaders + launch helper only) so the
# SteamOS Devkit Client's recursive "Local Folder" upload doesn't pull in the
# entire build tree.
#
#   ./tools/stage.sh [build-dir] [out-dir]
#
# Defaults: build-aarch64 -> dist/PixelVRPlayer
set -euo pipefail

cd "$(dirname "$0")/.."

BUILD_DIR="${1:-build-aarch64}"
OUT_DIR="${2:-dist/PixelVRPlayer}"

bin="${BUILD_DIR}/bin/pixelvr"
if [[ ! -x "${bin}" ]]; then
    echo "error: ${bin} not found. Build first: ./tools/build.sh --aarch64" >&2
    exit 1
fi

rm -rf "${OUT_DIR}"
mkdir -p "${OUT_DIR}/shaders"
cp "${bin}" "${OUT_DIR}/pixelvr"
cp "${BUILD_DIR}/shaders/"*.spv "${OUT_DIR}/shaders/"
cp tools/launch.sh "${OUT_DIR}/launch.sh"
chmod +x "${OUT_DIR}/pixelvr" "${OUT_DIR}/launch.sh"

echo "Staged ${OUT_DIR}:"
ls -1 "${OUT_DIR}"
cat <<EOF

Upload this folder in the SteamOS Devkit Client (Title Upload tab):
  Local Folder:  ${OUT_DIR}
  Start Command: pixelvr
  Runtime:       Steam Linux Runtime 3.0 ARM64 (Sniper)
EOF

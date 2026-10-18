#!/usr/bin/env bash
# Assemble the Steam Frame release tarball for a finished ARM64 build.
#
# Contents (extracted to ~/PixelVRPlayer):
#   PixelVRPlayer/pixelvr          self-contained binary (static OpenXR loader)
#   PixelVRPlayer/run.sh           launcher (logs to ~/pixelvr-run.log)
#   PixelVRPlayer/install.sh       one-command installer / Steam registration
#   PixelVRPlayer/shaders/*.spv    SPIR-V shaders
#   PixelVRPlayer/VERSION          release tag (when given)
#
#   ./tools/package-release.sh [build-dir] [out-tar] [version]
#
# Defaults: build -> pixelvr-linux-arm64.tar.gz
set -euo pipefail

cd "$(dirname "$0")/.."

BUILD_DIR="${1:-build}"
OUT_TAR="${2:-pixelvr-linux-arm64.tar.gz}"
VERSION="${3:-}"

bin="${BUILD_DIR}/bin/pixelvr"
if [[ ! -f "${bin}" ]]; then
    echo "error: ${bin} not found. Build first: ./tools/build.sh" >&2
    exit 1
fi
if [[ -z "$(find "${BUILD_DIR}/shaders" -maxdepth 1 -name '*.spv' -print -quit)" ]]; then
    echo "error: no shaders in ${BUILD_DIR}/shaders" >&2
    exit 1
fi

stage="$(mktemp -d)"
trap 'rm -rf "${stage}"' EXIT

mkdir -p "${stage}/PixelVRPlayer/shaders"
cp "${bin}" "${stage}/PixelVRPlayer/pixelvr"
cp "${BUILD_DIR}/shaders/"*.spv "${stage}/PixelVRPlayer/shaders/"
cp tools/run.sh "${stage}/PixelVRPlayer/run.sh"
cp tools/install.sh "${stage}/PixelVRPlayer/install.sh"
if [[ -f "tools/Install Pixel VR Player.desktop" ]]; then
    cp "tools/Install Pixel VR Player.desktop" "${stage}/PixelVRPlayer/Install Pixel VR Player.desktop"
fi
for asset in icon.png hero.png capsule.png header.png logo.png; do
    if [[ -f "assets/${asset}" ]]; then
        cp "assets/${asset}" "${stage}/PixelVRPlayer/${asset}"
    fi
done
if [[ -n "${VERSION}" ]]; then
    printf '%s\n' "${VERSION}" > "${stage}/PixelVRPlayer/VERSION"
fi
chmod +x "${stage}/PixelVRPlayer/pixelvr" \
         "${stage}/PixelVRPlayer/run.sh" \
         "${stage}/PixelVRPlayer/install.sh"
if [[ -f "${stage}/PixelVRPlayer/Install Pixel VR Player.desktop" ]]; then
    chmod +x "${stage}/PixelVRPlayer/Install Pixel VR Player.desktop"
fi

rm -f "${OUT_TAR}"
tar -C "${stage}" -czf "${OUT_TAR}" PixelVRPlayer

echo "Packaged ${OUT_TAR} ($(du -h "${OUT_TAR}" | cut -f1)):"
tar -tzf "${OUT_TAR}"

#!/usr/bin/env bash
# Assemble the Steam Frame release tarball for a finished ARM64 build.
#
# Contents (extracted to ~/PixelVRPlayer):
#   PixelVRPlayer/pixelvr          self-contained binary (static OpenXR loader)
#   PixelVRPlayer/run.sh           launcher (logs to ~/.local/state/pixelvr/run.log)
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
if compgen -G "${BUILD_DIR}/fonts/*.ttf" >/dev/null; then
    mkdir -p "${stage}/PixelVRPlayer/fonts"
    cp "${BUILD_DIR}/fonts/"*.ttf "${stage}/PixelVRPlayer/fonts/"
    cp assets/fonts/Inter-LICENSE.txt "${stage}/PixelVRPlayer/fonts/" 2>/dev/null || true
fi
# LGPL: the player embeds a patched FFmpeg; ship how to rebuild / replace it.
mkdir -p "${stage}/PixelVRPlayer/ffmpeg-source"
cp third_party/ffmpeg/README.txt third_party/ffmpeg/build.sh third_party/ffmpeg/*.patch \
   "${stage}/PixelVRPlayer/ffmpeg-source/"
cp tools/run.sh "${stage}/PixelVRPlayer/run.sh"
cp tools/install.sh "${stage}/PixelVRPlayer/install.sh"
if [[ -f "tools/update.sh" ]]; then
    cp tools/update.sh "${stage}/PixelVRPlayer/update.sh"
fi
if [[ -f "tools/Install Pixel VR Player.desktop" ]]; then
    cp "tools/Install Pixel VR Player.desktop" "${stage}/PixelVRPlayer/Install Pixel VR Player.desktop"
fi
if [[ -f "LICENSE" ]]; then
    cp "LICENSE" "${stage}/PixelVRPlayer/LICENSE"
fi
if [[ -f "THIRD_PARTY_LICENSES.md" ]]; then
    cp "THIRD_PARTY_LICENSES.md" "${stage}/PixelVRPlayer/THIRD_PARTY_LICENSES.md"
fi
for asset in icon.png hero.png capsule.png header.png logo.png; do
    if [[ -f "assets/${asset}" ]]; then
        cp "assets/${asset}" "${stage}/PixelVRPlayer/${asset}"
    fi
done
# Steam wants a small shortcut icon: ship the 256x256 version as icon.png.
if [[ -f "assets/icon-256.png" ]]; then
    cp "assets/icon-256.png" "${stage}/PixelVRPlayer/icon.png"
fi
if [[ -n "${VERSION}" ]]; then
    printf '%s\n' "${VERSION}" > "${stage}/PixelVRPlayer/VERSION"
fi
chmod +x "${stage}/PixelVRPlayer/pixelvr" \
         "${stage}/PixelVRPlayer/run.sh" \
         "${stage}/PixelVRPlayer/install.sh"
if [[ -f "${stage}/PixelVRPlayer/update.sh" ]]; then
    chmod +x "${stage}/PixelVRPlayer/update.sh"
fi
if [[ -f "${stage}/PixelVRPlayer/Install Pixel VR Player.desktop" ]]; then
    chmod +x "${stage}/PixelVRPlayer/Install Pixel VR Player.desktop"
fi

rm -f "${OUT_TAR}"
tar -C "${stage}" -czf "${OUT_TAR}" PixelVRPlayer

echo "Packaged ${OUT_TAR} ($(du -h "${OUT_TAR}" | cut -f1)):"
tar -tzf "${OUT_TAR}"

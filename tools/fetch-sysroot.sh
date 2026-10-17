#!/usr/bin/env bash
# Capture an aarch64 sysroot from a Steam Frame so the cross-build links against
# the exact FFmpeg / Vulkan / X11 / PulseAudio SONAMEs present on the device.
#
# This is the reliable path to a runtime-compatible binary: SteamOS ships
# different library versions than Ubuntu's arm64 multiarch packages, so linking
# against the device's own libraries avoids "cannot open shared object file" and
# version-mismatch failures at launch.
#
#   ./tools/fetch-sysroot.sh [ssh-host] [sysroot-dir]
#
# Defaults: host "steamos@frame", sysroot dir "sysroot".
# Then build with it:
#   PIXELVR_SYSROOT="$PWD/sysroot" ./tools/build.sh --aarch64
set -euo pipefail

cd "$(dirname "$0")/.."

HOST="${1:-${FRAME_HOST:-steamos@frame}}"
SYSROOT="${2:-sysroot}"
[[ "${HOST}" == *@* ]] || HOST="steamos@${HOST}"

SSH_KEY="${FRAME_SSH_KEY:-}"
RSYNC_RSH="ssh -o ConnectTimeout=10"
[[ -n "${SSH_KEY}" ]] && RSYNC_RSH+=" -i ${SSH_KEY}"

echo "Capturing aarch64 sysroot from ${HOST} into ${SYSROOT}/ ..."
mkdir -p "${SYSROOT}"

# Runtime libraries (the .so files the linker resolves against) and, where
# present, their pkg-config metadata. Headers usually come from the host's
# arm64 -dev packages or the x86 packages of the same API version.
paths=(
    /usr/lib
    /lib
    /usr/include
    /opt/steamvr/bin/linuxarm64
)

for p in "${paths[@]}"; do
    src="${HOST}:${p%/}/"
    dst="${SYSROOT}${p%/}/"
    echo "  ${p}"
    mkdir -p "${dst}"
    # -L would copy symlink targets; keep symlinks (-l) and copy their targets
    # only when they are relative within the tree to keep the sysroot small.
    rsync -az --copy-unsafe-links --prune-empty-dirs \
        --include='*/' \
        --include='*.so' --include='*.so.*' \
        --include='*.h' --include='*.hpp' \
        --include='*.pc' \
        --exclude='*' \
        -e "${RSYNC_RSH}" "${src}" "${dst}" || echo "    (skipped ${p})"
done

echo
echo "Sysroot ready at ${SYSROOT}/"
echo "Build against it with:"
echo "  PIXELVR_SYSROOT=\"\$PWD/${SYSROOT}\" ./tools/build.sh --aarch64"

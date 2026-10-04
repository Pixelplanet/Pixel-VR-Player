#!/usr/bin/env bash
# Builds a DMA-BUF-capable libavcodec.so.61 for the Steam Frame, natively on the
# device (GCC). We patch FFmpeg 7.0's V4L2-M2M decoder to export capture buffers
# as AV_PIX_FMT_DRM_PRIME (dmabuf) so the decoder output can be imported straight
# into Vulkan (zero-copy) and sampled through a VkSamplerYcbcrConversion. This
# handles NV12 (8-bit) and P010 (10-bit HEVC, Qualcomm UBWC) up to 4K+.
#
# Only libavcodec is overridden at runtime (via LD_LIBRARY_PATH); the system's
# libavformat/libavutil/libswscale (all FFmpeg 7.0) are used as-is.
#
# Run ON the Steam Frame:  bash build.sh   (writes to ~/ffbuild by default)
#
# LGPL: the two patches modify FFmpeg (LGPL v2.1+). The resulting library is
# LGPL; keep this directory (patches) plus the FFmpeg 7.0 source available. See
# README.txt.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
work="${1:-$HOME/ffbuild}"
jobs="${JOBS:-8}"
tarball="ffmpeg-7.0.tar.xz"
sha="4426a94dd2c814945456600c8adfc402bee65ec14a70e8c531ec9a2cd651da7b"

mkdir -p "$work"
cd "$work"

if [[ ! -f "$tarball" ]]; then
    curl -fsSL "https://ffmpeg.org/releases/$tarball" -o "$tarball.part"
    mv "$tarball.part" "$tarball"
fi
printf '%s  %s\n' "$sha" "$tarball" | sha256sum -c

rm -rf ffmpeg-7.0
tar xf "$tarball"
cd ffmpeg-7.0
patch -p1 < "$here/compat.patch"   # compiler-compat fixes (needed for modern GCC)
patch -p1 < "$here/dmabuf.patch"   # V4L2 dmabuf (AV_PIX_FMT_DRM_PRIME) export

cd "$work"
rm -rf build && mkdir build && cd build
../ffmpeg-7.0/configure \
    --disable-autodetect --disable-doc --disable-programs --disable-network \
    --disable-debug --disable-avdevice --disable-avformat --disable-avfilter \
    --disable-swscale --disable-postproc --disable-encoders --disable-muxers \
    --enable-v4l2-m2m --enable-shared --disable-static --extra-cflags=-std=gnu17
make -j"$jobs"

echo
echo "Built: $work/build/libavcodec/libavcodec.so.61"
echo "Run with: LD_LIBRARY_PATH=$work/build/libavcodec:$work/build/libavutil:$work/build/libswresample \\"
echo "          PIXELVR_DRM_PRIME=1 ./pixelvr <clip>"

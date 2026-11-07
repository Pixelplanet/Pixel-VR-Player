#!/usr/bin/env bash
# Builds the player's own FFmpeg 8.0 (LGPL) for the Steam Frame, with the
# V4L2-M2M decoder patched to export capture buffers as AV_PIX_FMT_DRM_PRIME
# (dmabuf) so decoded frames are imported straight into Vulkan (zero-copy) and
# sampled through a VkSamplerYcbcrConversion. Handles NV12 (8-bit) and P010
# (10-bit HEVC, Qualcomm UBWC) up to 8K.
#
# The whole set (libavcodec, libavformat, libavutil, libswscale, libswresample)
# is built and installed into $work/prefix. Release builds embed it into the
# player (src/app/bundled_ffmpeg.cpp), so the player no longer depends on the
# FFmpeg release SteamOS happens to ship (an OS update that moved from FFmpeg 7
# to 8 once broke every installed copy). On the device itself the dev wrapper
# may just put $work/prefix/lib first on LD_LIBRARY_PATH.
#
# Runtime dependencies outside FFmpeg: libgnutls (https for streams), libdav1d
# (AV1 in software: the Iris hardware decoder has no AV1), zlib. All ship with
# SteamOS.
#
# Run:  bash build.sh [workdir]   (default ~/ffbuild8)
#
# LGPL: the patch modifies FFmpeg (LGPL v2.1+). The resulting libraries are
# LGPL; keep this directory (patch) plus the FFmpeg 8.0 source available. See
# README.txt.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
work="${1:-$HOME/ffbuild8}"
jobs="${JOBS:-8}"
version="8.0"
tarball="ffmpeg-${version}.tar.xz"
sha="b2751fccb6cc4c77708113cd78b561059b6fa904b24162fa0be2d60273d27b8e"

mkdir -p "$work"
cd "$work"

if [[ ! -f "$tarball" ]]; then
    # Retried: a single TLS hiccup at ffmpeg.org once failed a release build.
    curl -fsSL --retry 5 --retry-delay 10 --retry-all-errors \
        "https://ffmpeg.org/releases/$tarball" -o "$tarball.part"
    mv "$tarball.part" "$tarball"
fi
printf '%s  %s\n' "$sha" "$tarball" | sha256sum -c

rm -rf "ffmpeg-${version}"
tar xf "$tarball"
cd "ffmpeg-${version}"
patch -p1 < "$here/dmabuf-8.0.patch"   # V4L2 dmabuf (AV_PIX_FMT_DRM_PRIME) export

cd "$work"
rm -rf build prefix && mkdir build && cd build
"../ffmpeg-${version}/configure" \
    --prefix="$work/prefix" \
    --disable-autodetect --disable-doc --disable-programs --disable-debug \
    --disable-avdevice --disable-avfilter --disable-encoders --disable-muxers \
    --enable-v4l2-m2m --enable-shared --disable-static \
    --enable-libdav1d --enable-zlib --enable-gnutls
make -j"$jobs"
make install

echo
echo "Installed: $work/prefix/lib (libavcodec.so.62, libavformat.so.62, libavutil.so.60,"
echo "           libswscale.so.9, libswresample.so.6)"
echo "Dev run:   LD_LIBRARY_PATH=$work/prefix/lib PIXELVR_DRM_PRIME=1 ./pixelvr <clip>"
echo "Release:   cmake -DPIXELVR_EMBED_FFMPEG_DIR=$work/prefix ..."

third_party/ffmpeg — the player's own FFmpeg 8.0 with zero-copy V4L2 decode
==========================================================================

What this is
------------
A patch applied to FFmpeg 8.0 to build the FFmpeg libraries the player ships
(libavcodec.so.62, libavformat.so.62, libavutil.so.60, libswscale.so.9,
libswresample.so.6) for the Steam Frame (Qualcomm "Iris" V4L2-M2M decoder):

  dmabuf-8.0.patch  Adds a `drm_prime` option to {h264,hevc,vp9}_v4l2m2m that
                    exports the decoder's capture buffers as AV_PIX_FMT_DRM_PRIME
                    (AVDRMFrameDescriptor: dmabuf fd + DRM fourcc + format
                    modifier + per-plane offset/pitch + crop). Includes the
                    Qualcomm compressed 10-bit format (Q10C) -> P010 with the
                    UBWC modifier, and a capture-format-change state machine for
                    mid-stream resolution changes. This lets us import the
                    decoder output straight into Vulkan (zero-copy) and sample
                    it via a VkSamplerYcbcrConversion, which fixes the swscale
                    NV12 chroma issue and unlocks HEVC/10-bit.

  dmabuf.patch      The same patch against FFmpeg 7.0 (kept for reference; the
  compat.patch      7.0 series also needed compiler-compat fixes). SteamOS moved
                    to FFmpeg 8 in November 2026, which is why the player now
                    carries its own complete set instead of overriding only
                    libavcodec on top of the system's libraries.

Provenance & license
---------------------
The patch modifies FFmpeg's own source files (libavcodec/v4l2_*.c/.h). It is
therefore covered by FFmpeg's license (LGPL v2.1 or later); the resulting
libraries are LGPL. The dmabuf technique (V4L2 VIDIOC_EXPBUF -> dmabuf) was
authored for the MatineeVR project (embedding-shapes/matineevr) as FFmpeg
modifications and is standard practice.

LGPL compliance: keep these patches in the tree and make the corresponding
FFmpeg 8.0 source (ffmpeg.org/releases/ffmpeg-8.0.tar.xz,
sha256 b2751fccb6cc4c77708113cd78b561059b6fa904b24162fa0be2d60273d27b8e) and the
FFmpeg license text available alongside any binary distribution.

How to build (on the Steam Frame, or in the release container)
---------------------------------------------------------------
    bash third_party/ffmpeg/build.sh        # -> ~/ffbuild8/prefix/lib/*.so

Dev run with the patched libraries first on the library path and the dmabuf
path enabled (the devkit launch wrapper does this):

    LD_LIBRARY_PATH=~/ffbuild8/prefix/lib PIXELVR_DRM_PRIME=1 ./pixelvr <clip>

Release builds embed the five libraries into the player binary
(-DPIXELVR_EMBED_FFMPEG_DIR=~/ffbuild8/prefix); at start the player unpacks them
to ~/.cache/pixelvr/ffmpeg/<id>/ and re-executes itself with them on
LD_LIBRARY_PATH (src/app/bundled_ffmpeg.cpp). PIXELVR_SYSTEM_FFMPEG=1 opts out.

third_party/ffmpeg — patched libavcodec for zero-copy V4L2 decode
================================================================

What this is
------------
Two patches applied to FFmpeg 7.0 to build a DMA-BUF-capable libavcodec.so.61
for the Steam Frame (Qualcomm "Iris" V4L2-M2M decoder):

  dmabuf.patch  Adds a `drm_prime` option to {h264,hevc,vp9}_v4l2m2m that exports
                the decoder's capture buffers as AV_PIX_FMT_DRM_PRIME
                (AVDRMFrameDescriptor: dmabuf fd + DRM fourcc + format modifier +
                per-plane offset/pitch + crop). Includes the Qualcomm compressed
                10-bit format (Q10C) -> P010 with the UBWC modifier, and a
                capture-format-change state machine for mid-stream resolution
                changes. This lets us import the decoder output straight into
                Vulkan (zero-copy) and sample it via a VkSamplerYcbcrConversion,
                which fixes the swscale NV12 chroma issue and unlocks HEVC/10-bit.

  compat.patch  Source-level fixes so FFmpeg 7.0 compiles with a modern C
                compiler (removes ATOMIC_VAR_INIT, adds explicit (double) casts in
                option tables, sysctl detection). No functional change.

Provenance & license
---------------------
Both patches modify FFmpeg's own source files (libavcodec/v4l2_*.c/.h, configure,
libav{util,codec}, libswresample). They are therefore covered by FFmpeg's license
(LGPL v2.1 or later); the resulting libavcodec.so.61 is LGPL. The patches were
authored by the MatineeVR project (embedding-shapes/matineevr) as FFmpeg
modifications; the technique (V4L2 VIDIOC_EXPBUF -> dmabuf) is standard.

LGPL compliance: keep these patches in the tree and make the corresponding
FFmpeg 7.0 source (ffmpeg.org/releases/ffmpeg-7.0.tar.xz,
sha256 4426a94dd2c814945456600c8adfc402bee65ec14a70e8c531ec9a2cd651da7b) and the
FFmpeg license text available alongside any binary distribution.

How to build (on the Steam Frame)
---------------------------------
    bash third_party/ffmpeg/build.sh        # -> ~/ffbuild/build/libavcodec/libavcodec.so.61

Then run pixelvr with the patched libavcodec first on the library path and the
dmabuf path enabled:

    LD_LIBRARY_PATH=~/ffbuild/build/libavcodec:~/ffbuild/build/libavutil:~/ffbuild/build/libswresample \
    PIXELVR_DRM_PRIME=1 ./pixelvr <clip>

Only libavcodec is overridden; the system's FFmpeg 7.0 avformat/avutil/swscale are
used at runtime (matching ABI).

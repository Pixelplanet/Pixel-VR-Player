#pragma once
// Release builds carry the patched FFmpeg libraries (zero-copy dma-buf decode,
// AV1 via dav1d; see third_party/ffmpeg) inside the executable, so every
// installer and updater delivers them. At start they are unpacked to
// ~/.cache/pixelvr/ffmpeg/<id>/ and the program restarts itself with them on
// LD_LIBRARY_PATH — only while the system FFmpeg is the release they were
// built against. PIXELVR_SYSTEM_FFMPEG=1 keeps the system libraries.

namespace pixelvr {

// Call first thing in main(); may not return (re-executes the program).
void useBundledFfmpeg(char** argv);

// True when this process runs on the bundled libraries.
bool usingBundledFfmpeg();

}  // namespace pixelvr

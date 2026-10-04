#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pixelvr {

// A decoded video frame in NV12 (8-bit luma plane + interleaved CbCr). YUV->RGB
// conversion happens on the GPU. `serial` increases monotonically so consumers
// can detect new frames without copying.
struct VideoFrame {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> y;   // luma plane, width * height
    std::vector<uint8_t> uv;  // interleaved CbCr, width * (height / 2)
    int colorspace = 1;       // AVColorSpace (1 = BT.709)
    bool fullRange = false;   // JPEG/full-range luma
    double pts = 0.0;         // presentation time in seconds
    uint64_t serial = 0;      // 0 = no frame yet
};

// A decoded frame delivered as a zero-copy DMA-BUF handle (the V4L2 decoder's
// AV_PIX_FMT_DRM_PRIME output). The consumer takes ownership of `fd` and must
// import it into Vulkan or close it. Handles NV12 (8-bit) and P010 (10-bit).
struct DrmVideoFrame {
    int width = 0;              // coded/padded buffer dimensions
    int height = 0;
    int fd = -1;               // dma-buf file descriptor (owned by the consumer)
    uint32_t fourcc = 0;       // DRM fourcc ('NV12', 'P010')
    uint64_t modifier = 0;     // DRM format modifier
    int64_t size = 0;          // buffer size in bytes
    int offset[2] = {0, 0};    // per-plane byte offset (luma, chroma)
    int pitch[2] = {0, 0};     // per-plane row pitch
    int crop[4] = {0, 0, 0, 0};  // left, top, right, bottom padding
    int colorspace = 1;
    bool fullRange = false;
    double pts = 0.0;
    uint64_t serial = 0;
    // Holds a reference to the decoder's AVFrame so the V4L2 capture buffer is
    // not re-queued (overwritten) while the GPU still samples this dma-buf.
    std::shared_ptr<void> keepAlive;
};

// Opens a media file and decodes its video stream on a background thread,
// publishing the most recent frame for the renderer to sample.
//
// Phase 2 baseline: software decode (libavcodec) + swscale to RGBA, paced to the
// stream's presentation timestamps. Hardware decode (Vulkan Video / V4L2) is
// layered in behind this same interface during the Phase 1 decode spike.
class MediaEngine {
public:
    MediaEngine();
    ~MediaEngine();

    MediaEngine(const MediaEngine&) = delete;
    MediaEngine& operator=(const MediaEngine&) = delete;

    bool open(const std::string& path);
    void close();

    int width() const;
    int height() const;
    double duration() const;  // seconds; 0 if unknown
    std::string codecName() const;   // decoder name + (hw)/(sw)
    uint64_t decodedFrames() const;  // total frames decoded since open

    // Copies the newest decoded frame into `out` when it is newer than
    // out.serial. Returns true if `out` was updated. Thread-safe; call every
    // render frame with the same VideoFrame instance.
    bool latestFrame(VideoFrame& out);

    // True when the decoder is emitting zero-copy DMA-BUF frames (drm_prime).
    bool drmActive() const;

    // Moves the newest DMA-BUF frame into `out` (transferring `fd` ownership)
    // when newer than out.serial. Returns true if updated. The caller must
    // import or close out.fd.
    bool latestDrmFrame(DrmVideoFrame& out);

    bool eof() const;

    // Playback control. Pausing holds the last decoded frame and stops audio.
    void setPaused(bool paused);
    void togglePause();
    bool paused() const;

    // Seeking (thread-safe; applied on the decode thread).
    void seek(double seconds);        // absolute, clamped to [0, duration]
    void seekRelative(double delta);  // relative to the current position
    double position() const;          // current playback time in seconds

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pixelvr

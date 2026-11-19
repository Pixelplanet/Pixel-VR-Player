#pragma once

#include "media/cover_art.hpp"
#include "media/stereo_detect.hpp"
#include "util/chapter.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pixelvr {

class RemoteFile;
// Opens a file on a network share (smb://, sftp://, dav(s)://) for reading: its own connection, as
// playback uses. Null on failure (`err` says why).
std::unique_ptr<RemoteFile> openRemoteMedia(const std::string& path, std::string& err);

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
    // Why the last open() failed, for the user (e.g. "no decoder for av1").
    const std::string& lastError() const;
    // Clear or trigger an open() abort flag so a background open() can be cancelled.
    void resetAbort();
    void abortOpen();
    void close();

    int width() const;
    int height() const;
    // Width of one pixel relative to its height (the file's sample aspect ratio; 1 = square).
    // The picture is shown width() * pixelAspect() wide: 1920x720 with 2:3 pixels is 16:9.
    float pixelAspect() const;
    double duration() const;  // seconds; 0 if unknown
    double frameRate() const;  // source frames per second; 0 if unknown (FS3D: per eye)
    // Frame-sequential 3D: -1 = detect (default), 0 = never, 1 = always (needs >= 40 fps); applies at
    // the next open(). frameSequential(): the open file is paired into SBS frames.
    void setFrameSequentialMode(int mode);
    bool frameSequential() const;
    std::string codecName() const;   // decoder name + (hw)/(sw)
    // Non-empty when the hardware decoder was refused while another process
    // held it ("steamwebhelper (3079)"): playback is in software because of it.
    std::string hardwareBlockedBy() const;
    // Who else has the hardware decoder open. steamHelper: a child process of Steam's web helper
    // (its GPU process decodes the videos in Steam's menus), ours to end and restarted by Steam.
    struct DecoderHolder {
        int pid = 0;
        std::string comm;
        bool steamHelper = false;
    };
    static std::vector<DecoderHolder> decoderHolderList();
    // Ends the Steam web helper processes holding the decoder (SIGTERM, SIGKILL after 1 s) and
    // waits up to timeoutMs until none of them has it open. Only on the user's request; blocks:
    // never call it on the render thread. True when no Steam helper holds the decoder any more.
    static bool freeDecoderFromSteam(int timeoutMs = 3000);
    uint64_t decodedFrames() const;  // total frames decoded since open
    uint64_t lateFrames() const;     // decoded but skipped: too late to show
    uint64_t clockWaits() const;     // times the clock waited for lagging video

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
    // Mute keeps decoding and the audio clock running but outputs silence
    // (multi-screen: only one screen is usually audible).
    void setMuted(bool muted);
    bool muted() const;
    // This screen's own volume (0..1), applied on top of the system volume.
    void setVolume(float volume);

    // Audio files (no video stream): the screen shows a still cover card.
    bool audioOnly() const;
    // 3D / VR layout from the container's metadata (unknown = -1).
    FormatHint formatHint() const;
    // The file's chapters (MKV / MP4), sorted by start; empty when it has none.
    const ChapterList& chapters() const;
    const AudioTags& audioTags() const;  // title / artist / album (audio files)

    // Seeking (thread-safe; applied on the decode thread). Seeks are frame
    // accurate: the decoder runs from the keyframe before the target and drops
    // the video and audio before it, so playback resumes at the target itself.
    void seek(double seconds);        // absolute, clamped to [0, duration]
    void seekRelative(double delta);  // relative to position() (a pending target counts)
    // Current playback time in seconds. From seek() until the first frame at the
    // target is shown this is the target, so it never flicks back meanwhile.
    double position() const;
    // A seek has not shown its first frame at the target yet (the picture is still the old one).
    bool seekPending() const;

    // Seek/close release handshake: before flushing, the decode thread asks the
    // renderer to free held dma-buf frames so the V4L2 decoder can reclaim its
    // capture buffers. The render loop polls needsFrameRelease() and calls
    // confirmFrameRelease() once it has released them.
    bool needsFrameRelease() const;
    void confirmFrameRelease();

    // --- Track selection (multiple audio/video streams + language) --------
    struct StreamInfo {
        int index = -1;        // stream index within the container
        std::string label;     // human-readable (codec + channels/size + lang)
        std::string language;  // ISO-639 code from metadata, or empty
        bool usable = true;    // subtitles: false when FFmpeg has no decoder for the track's format
        // Subtitles: what the track is for (from the file's flags, else its title).
        enum : int { kForced = 1, kHearingImpaired = 2, kCommentary = 4, kFromFile = 8, kFromServer = 16 };
        int flags = 0;
        std::string title;     // the track's own name in the file, or the sidecar's file name
        int entries = -1;      // subtitles: how many lines the track has, when the file says so
    };
    std::vector<StreamInfo> videoStreams() const;
    std::vector<StreamInfo> audioStreams() const;
    // Subtitles: the file's own tracks (index = container stream; text and picture formats) and subtitle
    // files next to a local file (.srt .ass .ssa .vtt .sup .idx; index >= kExternalSubtitleBase).
    static constexpr int kExternalSubtitleBase = 10000;
    std::vector<StreamInfo> subtitleStreams() const;
    // A subtitle file found somewhere else after open() (a network share, the vault, a media server) and
    // already read whole: `fileName`'s extension says how to read it (.srt .ass .ssa .vtt .sup; its name
    // is never logged). `info` gives label, language, flags and title. Returns the track's index in
    // subtitleStreams() (>= kExternalSubtitleBase), or -1 (a format that cannot be read from memory, or
    // the list is full). Call it from the thread that reads the track list, after open() has returned.
    int addSubtitleData(const StreamInfo& info, const std::string& fileName, std::shared_ptr<const std::string> data);
    // -1 = off. Takes effect at once; what is on at that moment is fetched from the file in the background.
    void setSubtitleStream(int index);
    int subtitleStream() const;
    // The lines to show at `seconds` ('\n' between lines; empty: nothing right now). False: subtitles off.
    // A text track that libass draws comes as a picture instead (subtitlePictureAt) and gives no lines
    // here, unless evenIfDrawn asks for the plain text all the same.
    bool subtitleAt(double seconds, std::string& out, bool evenIfDrawn = false) const;
    // Picture subtitles (Blu-ray PGS, DVD, DVB): the picture to show at `seconds`, else null. Its place is
    // given on the track's own canvas (1920x1080 for a Blu-ray, whatever the video was cropped to).
    struct SubtitlePicture {
        uint64_t id = 0;               // changes with the picture
        int canvasW = 0, canvasH = 0;
        int x = 0, y = 0, w = 0, h = 0;
        std::vector<uint8_t> rgba;     // w * h * 4, straight alpha
    };
    // Text tracks (with libass built in) come the same way: drawn on a canvas of the shape given to
    // setSubtitleFrame, by a worker thread, so the answer can be one display frame behind.
    // wait: until the picture for this very moment is drawn (headless checks).
    std::shared_ptr<const SubtitlePicture> subtitlePictureAt(double seconds, bool wait = false) const;
    // The visible picture's shape in pixels (black bars that are cut off left out): text subtitles are
    // laid out inside it. 1920 x 1080 until told otherwise.
    void setSubtitleFrame(int width, int height);
    int currentVideoStream() const;  // container index in use, or -1
    int currentAudioStream() const;  // container index in use, or -1
    // Applied on the next open(): force a container stream index (-1 = auto), or
    // auto-pick the audio track whose metadata language matches.
    void setPreferredVideoStream(int containerIndex);
    void setPreferredAudioStream(int containerIndex);
    void setPreferredAudioLanguage(const std::string& isoLang);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pixelvr

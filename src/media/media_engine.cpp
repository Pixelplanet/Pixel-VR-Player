#include "media/media_engine.hpp"

#include "media/vault_avio.hpp"
#include "media/cover_art.hpp"

#include "util/logging.hpp"
#include "util/smb_lock.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <unistd.h>

extern "C" {
#include <libavutil/stereo3d.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#ifdef PIXELVR_HAVE_PULSE
#include <pulse/error.h>
#include <pulse/simple.h>
#include <pulse/sample.h>
#endif

#ifdef PIXELVR_HAVE_SMB
#include <libsmbclient.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <cerrno>
#endif

#include "net/remote_fs.hpp"
#include "net/smb_credentials.hpp"

namespace pixelvr {

namespace {

// Other processes with the hardware decoder node open, as "comm (pid), ...".
// The iris driver budgets decoder sessions core-wide, so a foreign session
// (Steam's web helper after a trailer, a browser tab) can make an 8K session
// fail to open; this names the culprit for the log and the status line.
std::string decoderHolders() {
    std::string out;
    std::error_code ec;
    const std::filesystem::path node = std::filesystem::canonical("/dev/video-dec0", ec);
    if (ec) return out;
    const pid_t self = getpid();
    for (const auto& proc : std::filesystem::directory_iterator("/proc", ec)) {
        const std::string name = proc.path().filename().string();
        if (name.empty() || !std::isdigit(static_cast<unsigned char>(name[0]))) continue;
        const pid_t pid = static_cast<pid_t>(std::atoi(name.c_str()));
        if (pid == self) continue;
        std::error_code fdEc;
        for (const auto& fd : std::filesystem::directory_iterator(proc.path() / "fd", fdEc)) {
            std::error_code linkEc;
            if (std::filesystem::read_symlink(fd.path(), linkEc) != node) continue;
            std::string comm;
            if (std::ifstream in(proc.path() / "comm"); in) std::getline(in, comm);
            if (!out.empty()) out += ", ";
            out += (comm.empty() ? std::string("pid") : comm) + " (" + name + ")";
            break;
        }
    }
    return out;
}

#ifdef PIXELVR_HAVE_SMB
void smb_auth(SMBCCTX*, const char* srv, const char* shr, char* wg, int wglen, char* un, int unlen,
              char* pw, int pwlen) {
    std::string u, p, w;
    if (srv != nullptr && SmbCredentialsManager::instance().findCredentials(srv, shr ? shr : "", u, p, w)) {
        if (un && unlen > 0) {
            std::strncpy(un, u.c_str(), unlen - 1);
            un[unlen - 1] = '\0';
        }
        if (pw && pwlen > 0) {
            std::strncpy(pw, p.c_str(), pwlen - 1);
            pw[pwlen - 1] = '\0';
        }
        if (wg && wglen > 0 && !w.empty()) {
            std::strncpy(wg, w.c_str(), wglen - 1);
            wg[wglen - 1] = '\0';
        }
        return;
    }
    if (un != nullptr && unlen > 0) un[0] = '\0';  // guest / anonymous
    if (pw != nullptr && pwlen > 0) pw[0] = '\0';
}

// An opened smb:// file. libsmbclient is not thread-safe, so every call takes
// the process-wide SMB lock (per call, so scans and downloads interleave).
class SmbFile : public RemoteFile {
public:
    SmbFile(SMBCCTX* c, SMBCFILE* f) : ctx_(c), file_(f) {
        readFn_ = smbc_getFunctionRead(ctx_);
        lseekFn_ = smbc_getFunctionLseek(ctx_);
        closeFn_ = smbc_getFunctionClose(ctx_);
        smbc_fstat_fn fstatFn = smbc_getFunctionFstat(ctx_);
        struct stat st{};
        size_ = (fstatFn != nullptr && fstatFn(ctx_, file_, &st) == 0) ? static_cast<int64_t>(st.st_size) : -1;
    }
    ~SmbFile() override {
        std::lock_guard<std::mutex> smb(smbGlobalLock());
        if (file_ != nullptr && closeFn_ != nullptr) closeFn_(ctx_, file_);
        if (ctx_ != nullptr) smbc_free_context(ctx_, 1);
    }
    int64_t size() const override { return size_; }
    int read(uint8_t* buf, int n) override {
        std::lock_guard<std::mutex> smb(smbGlobalLock());
        return static_cast<int>(readFn_(ctx_, file_, buf, static_cast<size_t>(n)));
    }
    bool seek(int64_t pos) override {
        std::lock_guard<std::mutex> smb(smbGlobalLock());
        return lseekFn_(ctx_, file_, static_cast<off_t>(pos), SEEK_SET) >= 0;
    }

private:
    SMBCCTX* ctx_;
    SMBCFILE* file_;
    smbc_read_fn readFn_ = nullptr;
    smbc_lseek_fn lseekFn_ = nullptr;
    smbc_close_fn closeFn_ = nullptr;
    int64_t size_ = -1;
};
#endif

// Streams a network file (SMB, SFTP, WebDAV) behind a FFmpeg AVIOContext. A
// background thread prefetches the file into a ring buffer so the demux read
// callback serves from memory; network jitter then no longer stalls decoding.
// Only the prefetch thread touches the remote file (read/seek); the AVIO
// callbacks only touch the ring and request seeks.
struct ReadAhead {
    static constexpr std::size_t kCapacity = 64u << 20;  // 64 MiB cushion
    static constexpr std::size_t kChunk = 256u << 10;    // 256 KiB network reads

    std::unique_ptr<RemoteFile> source;
    int64_t fileSize = -1;

    std::mutex mutex;
    std::condition_variable dataCv;   // data available / eof / stop
    std::condition_variable spaceCv;  // space available / seek / stop
    std::thread thread;
    std::vector<uint8_t> ring;
    std::size_t head = 0;      // ring index of the next byte to read out
    std::size_t fill = 0;      // bytes currently buffered
    int64_t consumePos = 0;    // absolute offset of the next consumer byte
    int64_t prefetchPos = 0;   // absolute offset the prefetch thread reads from
    bool pendingSeek = false;
    bool eof = false;
    bool stop = false;

    bool start(std::unique_ptr<RemoteFile> src) {
        std::lock_guard<std::mutex> lk(mutex);
        source = std::move(src);
        fileSize = source->size();
        head = fill = 0;
        consumePos = prefetchPos = 0;
        pendingSeek = eof = stop = false;
        ring.assign(kCapacity, 0);
        thread = std::thread([this] { run(); });
        return true;
    }

    void run() {
        int64_t pos = 0;
        std::vector<uint8_t> chunk(kChunk);
        for (;;) {
            std::size_t want;
            bool doSeek = false;
            int64_t seekPos = 0;
            {
                std::unique_lock<std::mutex> lk(mutex);
                spaceCv.wait(lk, [this] {
                    return stop || pendingSeek || (!eof && fill < ring.size());
                });
                if (stop) return;
                if (pendingSeek) {
                    pendingSeek = false;
                    head = fill = 0;
                    eof = false;
                    pos = prefetchPos;
                    doSeek = true;
                    seekPos = pos;
                }
                if (eof) continue;
                want = std::min(ring.size() - fill, chunk.size());
            }
            if (doSeek) source->seek(seekPos);
            if (want == 0) continue;
            const int n = source->read(chunk.data(), static_cast<int>(want));
            std::lock_guard<std::mutex> lk(mutex);
            if (stop || pendingSeek) continue;  // a seek/stop superseded this read
            if (n <= 0) {
                eof = true;
                dataCv.notify_all();
                continue;
            }
            const std::size_t w = (head + fill) % ring.size();
            const std::size_t first = std::min(static_cast<std::size_t>(n), ring.size() - w);
            std::memcpy(ring.data() + w, chunk.data(), first);
            if (static_cast<std::size_t>(n) > first) {
                std::memcpy(ring.data(), chunk.data() + first,
                            static_cast<std::size_t>(n) - first);
            }
            fill += static_cast<std::size_t>(n);
            pos += n;
            prefetchPos = pos;
            dataCv.notify_all();
        }
    }

    int read(uint8_t* buf, int size) {
        if (size <= 0) return 0;
        std::unique_lock<std::mutex> lk(mutex);
        dataCv.wait(lk, [this] { return stop || fill > 0 || eof; });
        if (stop) return AVERROR_EXIT;
        if (fill == 0) return AVERROR_EOF;
        const std::size_t n = std::min(static_cast<std::size_t>(size), fill);
        const std::size_t first = std::min(n, ring.size() - head);
        std::memcpy(buf, ring.data() + head, first);
        if (n > first) std::memcpy(buf + first, ring.data(), n - first);
        head = (head + n) % ring.size();
        fill -= n;
        consumePos += static_cast<int64_t>(n);
        spaceCv.notify_all();
        return static_cast<int>(n);
    }

    int64_t seek(int64_t offset, int whence) {
        if ((whence & ~AVSEEK_FORCE) == AVSEEK_SIZE) return fileSize;
        std::lock_guard<std::mutex> lk(mutex);
        const int w = whence & ~AVSEEK_FORCE;
        int64_t target;
        if (w == SEEK_SET) target = offset;
        else if (w == SEEK_CUR) target = consumePos + offset;
        else if (w == SEEK_END) target = (fileSize >= 0) ? fileSize + offset : -1;
        else return -1;
        if (target < 0) return -1;
        consumePos = prefetchPos = target;
        head = fill = 0;  // discard data buffered from the old position
        eof = false;
        pendingSeek = true;
        spaceCv.notify_all();
        dataCv.notify_all();
        return target;
    }

    // Unblocks a consumer stalled in read() so the decode thread can exit; the
    // prefetch thread is joined later in shutdown().
    void abort() {
        std::lock_guard<std::mutex> lk(mutex);
        stop = true;
        dataCv.notify_all();
        spaceCv.notify_all();
    }

    void shutdown() {
        {
            std::lock_guard<std::mutex> lk(mutex);
            stop = true;
            dataCv.notify_all();
            spaceCv.notify_all();
        }
        if (thread.joinable()) thread.join();
        source.reset();
        ring.clear();
        ring.shrink_to_fit();
    }
};

int remote_read(void* opaque, uint8_t* buf, int size) {
    return static_cast<ReadAhead*>(opaque)->read(buf, size);
}

int64_t remote_seek(void* opaque, int64_t offset, int whence) {
    return static_cast<ReadAhead*>(opaque)->seek(offset, whence);
}

}  // namespace

// Playback clock in the VIDEO PTS timeline. It starts when the first frame is
// presented and advances with wall time, so consumer-side selection is smooth at
// any frame rate and independent of the audio PTS epoch. It freezes while paused
// and resets on seek. Frame production stays audio-paced, so the bounded frame
// queue keeps video locked to audio without assuming a shared PTS epoch.
struct PlaybackClock {
    std::atomic<double> basePts{0.0};
    std::atomic<int64_t> baseWallNs{0};  // steady_clock ns; 0 = stopped/frozen
    std::atomic<bool> running{false};

    static int64_t nowNs() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }
    void start(double pts) {
        basePts.store(pts);
        baseWallNs.store(nowNs());
        running.store(true);
    }
    void freeze() {
        if (!running.exchange(false)) return;
        const int64_t w = baseWallNs.exchange(0);
        if (w != 0) {
            basePts.store(basePts.load() + static_cast<double>(nowNs() - w) / 1e9);
        }
    }
    void resume() {
        if (running.exchange(true)) return;
        baseWallNs.store(nowNs());
    }
    void reset() {
        running.store(false);
        baseWallNs.store(0);
        basePts.store(0.0);
    }
    double now() const {
        const int64_t w = baseWallNs.load();
        if (w == 0) return basePts.load();
        return basePts.load() + static_cast<double>(nowNs() - w) / 1e9;
    }
};


// ---- Frame-packing SEI (H.264 / H.265 payload 45) -------------------------------------------------
// Frame-sequential 3D can be marked only inside the video stream: a "frame packing arrangement" SEI with
// type 5 (temporal interleaving). Hardware decoders don't report it, so the first packets are read here.
namespace fpsei {

struct Bits {
    const std::vector<uint8_t>& d;
    size_t pos = 0;  // in bits
    explicit Bits(const std::vector<uint8_t>& data) : d(data) {}
    bool more() const { return pos < d.size() * 8; }
    uint32_t bit() {
        if (!more()) return 0;
        const uint32_t b = (d[pos >> 3] >> (7 - (pos & 7))) & 1u;
        ++pos;
        return b;
    }
    uint32_t bits(int n) {
        uint32_t v = 0;
        while (n-- > 0) v = (v << 1) | bit();
        return v;
    }
    uint32_t ue() {
        int zeros = 0;
        while (more() && bit() == 0 && zeros < 31) ++zeros;
        return (1u << zeros) - 1u + bits(zeros);
    }
};

// NAL payload without emulation-prevention bytes (00 00 03 -> 00 00).
inline std::vector<uint8_t> rbsp(const uint8_t* p, size_t n) {
    std::vector<uint8_t> out;
    out.reserve(n);
    int zeros = 0;
    for (size_t i = 0; i < n; ++i) {
        if (zeros >= 2 && p[i] == 3) {
            zeros = 0;
            continue;
        }
        zeros = p[i] == 0 ? zeros + 1 : 0;
        out.push_back(p[i]);
    }
    return out;
}

// One SEI NAL (header removed): 1 = frame-sequential (rightFirst set), 0 = another packing, -1 = none.
inline int scanSei(const std::vector<uint8_t>& r, bool& rightFirst) {
    size_t i = 0;
    while (i + 2 <= r.size() && r[i] != 0x80) {
        uint32_t type = 0, size = 0;
        while (i < r.size() && r[i] == 0xFF) type += 255, ++i;
        if (i >= r.size()) break;
        type += r[i++];
        while (i < r.size() && r[i] == 0xFF) size += 255, ++i;
        if (i >= r.size()) break;
        size += r[i++];
        if (i + size > r.size()) break;
        if (type == 45) {
            std::vector<uint8_t> payload(r.begin() + static_cast<long>(i), r.begin() + static_cast<long>(i + size));
            Bits b(payload);
            b.ue();                       // frame_packing_arrangement_id
            if (b.bit()) return -1;       // cancel flag
            const uint32_t arrangement = b.bits(7);
            b.bit();                      // quincunx_sampling_flag
            const uint32_t interp = b.bits(6);  // 1: frame 0 is left, 2: frame 0 is right
            if (arrangement != 5) return 0;
            rightFirst = interp == 2;
            return 1;
        }
        i += size;
    }
    return -1;
}

// A packet (length-prefixed when lenSize > 0, else Annex B start codes).
inline int scanPacket(const uint8_t* d, size_t n, int lenSize, bool hevc, bool& rightFirst) {
    auto nal = [&](const uint8_t* p, size_t len) {
        if (len < 3) return -1;
        const int hdr = hevc ? 2 : 1;
        const int t = hevc ? (p[0] >> 1) & 0x3f : p[0] & 0x1f;
        const bool sei = hevc ? (t == 39 || t == 40) : t == 6;
        if (!sei) return -1;
        return scanSei(rbsp(p + hdr, len - static_cast<size_t>(hdr)), rightFirst);
    };
    if (lenSize > 0) {
        size_t i = 0;
        while (i + static_cast<size_t>(lenSize) <= n) {
            size_t len = 0;
            for (int k = 0; k < lenSize; ++k) len = (len << 8) | d[i + static_cast<size_t>(k)];
            i += static_cast<size_t>(lenSize);
            if (len == 0 || i + len > n) break;
            const int r = nal(d + i, len);
            if (r >= 0) return r;
            i += len;
        }
        return -1;
    }
    size_t i = 0;
    auto start = [&](size_t at) { return at + 3 <= n && d[at] == 0 && d[at + 1] == 0 && d[at + 2] == 1; };
    while (i + 3 <= n && !start(i)) ++i;
    while (i + 3 <= n) {
        const size_t b = i + 3;
        size_t e = b;
        while (e + 3 <= n && !start(e)) ++e;
        size_t end = e + 3 <= n ? e : n;
        while (end > b && d[end - 1] == 0) --end;  // trailing zero of a 4-byte start code
        const int r = nal(d + b, end - b);
        if (r >= 0) return r;
        i = e;
    }
    return -1;
}

}  // namespace fpsei

struct MediaEngine::Impl {
    AVFormatContext* format = nullptr;
    AVCodecContext* codec = nullptr;
    SwsContext* sws = nullptr;
    AVIOContext* avio = nullptr;
    std::unique_ptr<VaultAvio> vaultIo;  // encrypted vault file (decrypting reader)
    ReadAhead remoteReader;
    int videoStream = -1;
    int width = 0;
    int height = 0;
    double duration = 0.0;
    double frameRate = 0.0;
    bool httpSource = false;  // http(s): FFmpeg reconnects; read errors are retried here too
    int readRetries = 0;
    AVRational timeBase{1, 1};
    int drmDiag = 0;
    std::string codecName;
    std::string hwBlockedBy;  // who else held the hardware decoder when it was refused
    const AVCodec* videoDecoder = nullptr;  // retained for reopen-on-seek
    bool decoderDrm = false;                // apply drm_prime opts on reopen
    // Frame-sequential 3D (FS3D): left and right views alternate frame by frame (e.g. 48 fps = 24 per
    // eye). The decode thread pairs them into one full side-by-side frame, so the rest of the player
    // sees a 2x-wide SBS video at half the frame rate.
    bool frameSeq = false;
    bool frameSeqRightFirst = false;  // Matroska block_rl / inverted side data
    int frameSeqMode = -1;            // -1 detect, 0 off, 1 on (manual choice)
    double frameSeqFps = 0.0;         // the file's frame rate (both views)
    double frameSeqStart = 0.0;       // stream start: frame parity is counted from here
    int captureBuffers = 0;                 // num_capture_buffers for reopen
    std::atomic<uint64_t> decodedCount{0};

    std::vector<MediaEngine::StreamInfo> videoStreamList;
    std::vector<MediaEngine::StreamInfo> audioStreamList;
    int preferredVideoStream = -1;  // container index to force, or -1 = auto
    int preferredAudioStream = -1;
    std::string preferredLanguage;  // ISO code to auto-pick audio, or empty

    int audioStream = -1;
    AVCodecContext* audioCodec = nullptr;
    SwrContext* swr = nullptr;
    // What swr was set up for; decoded frames can differ (DTS-HD / TrueHD after a seek).
    AVChannelLayout swrInLayout{};
    int swrInFormat = -1;
    int swrInRate = 0;
    AVRational audioTimeBase{1, 1};
    bool audioEnabled = false;
    static constexpr int kOutSampleRate = 48000;
    static constexpr int kOutChannels = 2;
    std::atomic<double> masterClock{0.0};
#ifdef PIXELVR_HAVE_PULSE
    pa_simple* pa = nullptr;
    struct AudioChunk {
        std::vector<uint8_t> pcm;
        double pts = -1.0;
        double duration = 0.0;
    };
    std::mutex audioMutex;
    std::condition_variable audioCv;
    std::deque<AudioChunk> audioQueue;
    // Audio files: set by a seek (with the queue cleared) so the first chunk
    // queued afterwards re-anchors the clock and settles the display, even when
    // the target is near where the clock restarted. Guarded by audioMutex.
    bool audioReanchor = false;
    std::thread audioThread;
#endif

    std::thread thread;
    std::atomic<bool> running{false};
    std::atomic<bool> openAborted{false};
    std::atomic<bool> eof{false};
    std::atomic<bool> paused{false};
    std::atomic<bool> muted{false};  // audio keeps its clock but plays silence
    std::atomic<double> seekTarget{-1.0};  // >= 0 requests a seek (seconds)
    // What position() reports from seek() until the first frame decoded at the
    // new position is shown (-1 = report masterClock), so the time never flicks
    // back to the old position while the seek is throttled, the decoder reopens
    // or the frames before the target are decoded and dropped. Written under
    // `mutex`, together with the two serials that tell whether the frame being
    // shown comes from the newest requested seek.
    std::atomic<double> displayPos{-1.0};
    uint64_t seekSerial = 0;     // guarded by mutex; seek() requests so far
    uint64_t appliedSerial = 0;  // guarded by mutex; requests the decode thread applied
    std::atomic<bool> releaseReq{false};   // ask the consumer to free held frames
    std::condition_variable releaseCv;     // consumer signals release complete
    std::condition_variable queueCv;       // a queue slot freed / seek / stop

    // Decoded video frames awaiting display, ordered by PTS. The render thread
    // selects the frame matching the master clock each vsync (dropping late
    // frames, holding early ones) so presentation is phase-locked to the
    // compositor instead of the decode cadence. Audio still paces the decode
    // loop and drives masterClock; video is scheduled on the consumer side.
    std::mutex mutex;
    std::deque<VideoFrame> swQueue;      // CPU frames, guarded by mutex
    std::deque<DrmVideoFrame> drmQueue;  // dma-buf frames, guarded by mutex
    uint64_t serial = 0;                 // guarded by mutex; last frame id
    bool primed = false;                 // guarded by mutex; first frame shown?
    static constexpr std::size_t kMaxQueue = 6;
    std::size_t maxQueue = kMaxQueue;    // effective cap; reduced for huge frames
    std::atomic<bool> usingDrm{false};
    PlaybackClock playClock;  // video-epoch clock, anchored on the first frame
    // Audio files: no video decoder; audio anchors the clock and the screen
    // shows a still "now playing" card (cover art), delivered once.
    bool audioOnly = false;
    std::atomic<uint64_t> lateCount{0};
    std::atomic<uint64_t> clockWaitCount{0};
    FormatHint formatHint;
    ChapterList chapters;
    std::string lastError;  // set when open() fails
    AudioTags tags;
    std::atomic<float> volume{1.0f};
    VideoFrame card;
    bool cardPending = false;  // guarded by mutex

    ~Impl() { teardown(); }

    void teardown() {
#ifdef PIXELVR_HAVE_PULSE
        {
            std::lock_guard<std::mutex> lk(audioMutex);
            audioQueue.clear();
            audioReanchor = false;
        }
        audioCv.notify_all();
        if (audioThread.joinable()) {
            audioThread.join();
        }
        if (pa) {
            pa_simple_free(pa);
            pa = nullptr;
        }
#endif
        if (swr) {
            swr_free(&swr);
        }
        av_channel_layout_uninit(&swrInLayout);
        if (audioCodec) {
            avcodec_free_context(&audioCodec);
        }
        if (sws) {
            sws_freeContext(sws);
            sws = nullptr;
        }
        if (codec) {
            avcodec_free_context(&codec);
        }
        if (format) {
            avformat_close_input(&format);
        }
        if (avio != nullptr) {
            av_freep(&avio->buffer);
            avio_context_free(&avio);
        }
        vaultIo.reset();  // after the format context that read through it
        remoteReader.shutdown();
        clearQueues();
        maxQueue = kMaxQueue;
        usingDrm.store(false);
        audioEnabled = false;
        audioStream = -1;
    }

    // Drops every queued frame, closing any dma-buf handles they own. The caller
    // holds `mutex` when the decode thread is running (seek); teardown runs after
    // the thread has joined, so no lock is needed there.
    void clearQueues() {
        for (DrmVideoFrame& f : drmQueue) {
            if (f.fd >= 0) ::close(f.fd);
        }
        drmQueue.clear();
        swQueue.clear();
        primed = false;
    }

    // The newest requested seek has produced its first shown frame (or sound):
    // position() follows the playback clock again. The caller holds `mutex`.
    void settleDisplayLocked() {
        if (appliedSerial == seekSerial) displayPos.store(-1.0);
    }

    // Playback position in seconds for consumer-side frame selection, in the
    // video PTS timeline (anchored to the first presented frame).
    double currentClock() const {
        return playClock.now();
    }

    void pushFrame(int w, int h, std::vector<uint8_t>&& y, std::vector<uint8_t>&& uv,
                   int colorspace, bool fullRange, double pts) {
        std::unique_lock<std::mutex> lock(mutex);
        // Back-pressure: wait for the consumer to free a slot rather than dropping
        // frames, so the decoder is paced by presentation and cannot race ahead of
        // the playback clock.
        while (swQueue.size() >= maxQueue && running.load() &&
               seekTarget.load() < 0.0) {
            queueCv.wait_for(lock, std::chrono::milliseconds(50));
        }
        if (!running.load() || seekTarget.load() >= 0.0) {
            return;  // a stop/seek is pending: drop this stale frame
        }
        VideoFrame f;
        f.width = w;
        f.height = h;
        f.y = std::move(y);
        f.uv = std::move(uv);
        f.colorspace = colorspace;
        f.fullRange = fullRange;
        f.pts = pts;
        f.serial = ++serial;
        swQueue.push_back(std::move(f));
    }

    void pushDrm(const DrmVideoFrame& f) {
        std::unique_lock<std::mutex> lock(mutex);
        // Back-pressure (see pushFrame): pace the decoder to the consumer so a
        // post-seek or startup decode burst can't outrun the playback clock and
        // we never drop decoded hardware frames from the queue.
        while (drmQueue.size() >= maxQueue && running.load() &&
               seekTarget.load() < 0.0) {
            queueCv.wait_for(lock, std::chrono::milliseconds(50));
        }
        if (!running.load() || seekTarget.load() >= 0.0) {
            if (f.fd >= 0) ::close(f.fd);  // stop/seek pending: drop; we own the fd
            return;
        }
        DrmVideoFrame queued = f;
        queued.serial = ++serial;
        drmQueue.push_back(std::move(queued));
        usingDrm.store(true);
    }

    // Reopens the video decoder from scratch (matineevr's seek strategy): a flushed
    // V4L2 hardware decoder can wedge under rapid seeking, but a fresh one cannot.
    bool reopenDecoder() {
        if (codec != nullptr) {
            avcodec_free_context(&codec);
        }
        if (videoDecoder == nullptr || format == nullptr) return false;
        AVStream* s = format->streams[videoStream];
        codec = avcodec_alloc_context3(videoDecoder);
        if (codec == nullptr ||
            avcodec_parameters_to_context(codec, s->codecpar) < 0) {
            return false;
        }
        codec->pkt_timebase = s->time_base;
        codec->thread_count = 0;  // Multi-threaded SW decode across all CPU cores
        codec->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
        if (std::strstr(videoDecoder->name, "v4l2m2m") != nullptr) {
            av_opt_set_int(codec->priv_data, "num_output_buffers", 4, 0);
        }
        if (decoderDrm) {
            av_opt_set_int(codec->priv_data, "num_capture_buffers", captureBuffers, 0);
            av_opt_set_int(codec->priv_data, "drm_prime", 1, 0);
            codec->apply_cropping = 0;
        }
        return avcodec_open2(codec, videoDecoder, nullptr) >= 0;
    }

    void decodeLoop();
    void audioThreadLoop();
};

void MediaEngine::Impl::decodeLoop() {
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    if (packet == nullptr || frame == nullptr) {
        PIXELVR_LOG_ERROR("MediaEngine: failed to allocate FFmpeg frame/packet");
        av_packet_free(&packet);
        av_frame_free(&frame);
        return;
    }

    using clock = std::chrono::steady_clock;
    clock::time_point startTime{};
    double startPts = 0.0;
    bool haveClock = false;
    bool eofFlushed = false;
    bool wasPaused = false;
    auto lastSeekWall = clock::now() - std::chrono::seconds(1);
    // Frame-accurate seeking: the demuxer lands on the keyframe at or before
    // the target, so video frames and audio before the target are decoded and
    // dropped (-1 = not discarding). The caps bound the frozen wait on a very
    // long GOP at 8K, where the hardware decoder is only about real time.
    double videoDiscardUntil = -1.0;
    // FS3D: the first view of the current pair, waiting for its partner (index -10 = none).
    std::vector<uint8_t> fsY, fsUV;
    long long fsIdx = -10;
    double fsPts = 0.0;
    [[maybe_unused]] double audioDiscardUntil = -1.0;  // read by process_audio (Pulse builds)
    int discardCount = 0;
    constexpr int kMaxDiscardFrames = 600;
    constexpr double kMaxDiscardSec = 3.0;
    // A frame within half a frame of the target counts as the target (rounding).
    const double halfFrame = (frameRate > 0.0) ? 0.5 / frameRate : 0.02;

    int resolvedColorspace = -1;
    int resolvedFullRange = -1;
    AVStream* vStream = (format && videoStream >= 0) ? format->streams[videoStream] : nullptr;
    if (vStream && vStream->codecpar) {
        if (vStream->codecpar->color_space != AVCOL_SPC_UNSPECIFIED &&
            vStream->codecpar->color_space != AVCOL_SPC_RESERVED) {
            resolvedColorspace = static_cast<int>(vStream->codecpar->color_space);
        }
        if (vStream->codecpar->color_range != AVCOL_RANGE_UNSPECIFIED) {
            resolvedFullRange = (vStream->codecpar->color_range == AVCOL_RANGE_JPEG) ? 1 : 0;
        }
    }

    auto process_video = [&]() {
        decodedCount.fetch_add(1, std::memory_order_relaxed);
        const int w = frame->width;
        const int h = frame->height;
        const auto srcFmt = static_cast<AVPixelFormat>(frame->format);

        if (resolvedColorspace < 0) {
            if (frame->colorspace != AVCOL_SPC_UNSPECIFIED && frame->colorspace != AVCOL_SPC_RESERVED) {
                resolvedColorspace = static_cast<int>(frame->colorspace);
            } else {
                resolvedColorspace = (frame->width >= 3840 || frame->height >= 2160)
                    ? static_cast<int>(AVCOL_SPC_BT2020_NCL)
                    : static_cast<int>(AVCOL_SPC_BT709);
            }
        }
        if (resolvedFullRange < 0) {
            if (frame->color_range != AVCOL_RANGE_UNSPECIFIED) {
                resolvedFullRange = (frame->color_range == AVCOL_RANGE_JPEG) ? 1 : 0;
            } else {
                resolvedFullRange = 0;
            }
        }

        const int colorspace = (frame->colorspace != AVCOL_SPC_UNSPECIFIED &&
                                frame->colorspace != AVCOL_SPC_RESERVED)
            ? static_cast<int>(frame->colorspace)
            : resolvedColorspace;
        const bool fullRange = (frame->color_range != AVCOL_RANGE_UNSPECIFIED)
            ? (frame->color_range == AVCOL_RANGE_JPEG)
            : (resolvedFullRange == 1);

        int64_t ts = frame->best_effort_timestamp;
        if (ts == AV_NOPTS_VALUE) {
            ts = frame->pts;
        }
        const double pts = (ts == AV_NOPTS_VALUE) ? 0.0 : ts * av_q2d(timeBase);

        // After a seek, drop the frames before the target. This runs before the
        // dma-buf dup, the frame hold, the copy / repack, the pacing and the
        // queue push, so a dropped frame costs only its decode (the decoder's
        // buffer is recycled on the next receive). Past the caps the frame
        // reached so far is shown instead.
        if (videoDiscardUntil >= 0.0) {
            const bool early = ts != AV_NOPTS_VALUE && pts < videoDiscardUntil - halfFrame;
            if (early && discardCount < kMaxDiscardFrames &&
                std::chrono::duration<double>(clock::now() - lastSeekWall).count() < kMaxDiscardSec) {
                ++discardCount;
                return;
            }
            PIXELVR_LOG_INFO("MediaEngine: seek target %.3f, first frame %.3f after %d dropped%s",
                             videoDiscardUntil, pts, discardCount, early ? " (cap reached)" : "");
            videoDiscardUntil = -1.0;
        }

        const bool isDrm =
            srcFmt == AV_PIX_FMT_DRM_PRIME && frame->data[0] != nullptr;
        DrmVideoFrame drm;
        std::vector<uint8_t> y, uv;

        if (isDrm) {
            const auto* desc =
                reinterpret_cast<const AVDRMFrameDescriptor*>(frame->data[0]);
            const AVDRMLayerDescriptor& layer = desc->layers[0];
            drm.fd = ::dup(desc->objects[0].fd);
            if (drm.fd < 0) {
                return;  // could not keep the dma-buf alive across the handoff
            }
            drm.width = frame->width;
            drm.height = frame->height;
            drm.fourcc = layer.format;
            drm.modifier = desc->objects[0].format_modifier;
            drm.size = static_cast<int64_t>(desc->objects[0].size);
            drm.offset[0] = static_cast<int>(layer.planes[0].offset);
            drm.offset[1] = static_cast<int>(layer.planes[1].offset);
            drm.pitch[0] = static_cast<int>(layer.planes[0].pitch);
            drm.pitch[1] = static_cast<int>(layer.planes[1].pitch);
            drm.crop[0] = frame->crop_left;
            drm.crop[1] = frame->crop_top;
            drm.crop[2] = frame->crop_right;
            drm.crop[3] = frame->crop_bottom;
            drm.colorspace = colorspace;
            drm.fullRange = fullRange;
            drm.pts = pts;
            // Keep the decoder's frame referenced until the GPU is done with
            // this dma-buf; otherwise the V4L2 buffer is re-queued and the
            // decoder overwrites it mid-import (stale handle / GPU fault).
            if (AVFrame* hold = av_frame_clone(frame)) {
                drm.keepAlive = std::shared_ptr<void>(hold, [](void* p) {
                    AVFrame* f = static_cast<AVFrame*>(p);
                    av_frame_free(&f);
                });
            }
            if (drmDiag < 2) {
                ++drmDiag;
                PIXELVR_LOG_INFO(
                    "drm frame: %dx%d crop %d,%d,%d,%d fd=%d size=%ld mod=%#lx "
                    "fourcc=%c%c%c%c p0(%d,%d) p1(%d,%d)",
                    drm.width, drm.height, drm.crop[0], drm.crop[1], drm.crop[2],
                    drm.crop[3], drm.fd, static_cast<long>(drm.size),
                    static_cast<unsigned long>(drm.modifier),
                    static_cast<char>(drm.fourcc & 0xff),
                    static_cast<char>((drm.fourcc >> 8) & 0xff),
                    static_cast<char>((drm.fourcc >> 16) & 0xff),
                    static_cast<char>((drm.fourcc >> 24) & 0xff), drm.offset[0],
                    drm.pitch[0], drm.offset[1], drm.pitch[1]);
            }
        } else {
            y.assign(static_cast<std::size_t>(w) * h, 0);
            uv.assign(static_cast<std::size_t>(w) * (h / 2), 0);
            if (srcFmt == AV_PIX_FMT_NV12) {
                // Already NV12 (V4L2 hardware, line-padded to an aligned height).
                // Repack tightly: libswscale's NV12->NV12 drops chroma here.
                const uint8_t* srcY = frame->data[0];
                const uint8_t* srcUV = frame->data[1];
                const std::size_t lsY = static_cast<std::size_t>(frame->linesize[0]);
                const std::size_t lsUV = static_cast<std::size_t>(frame->linesize[1]);
                for (int row = 0; row < h; ++row) {
                    std::memcpy(y.data() + static_cast<std::size_t>(row) * w,
                                srcY + static_cast<std::size_t>(row) * lsY, w);
                }
                for (int row = 0; row < h / 2; ++row) {
                    std::memcpy(uv.data() + static_cast<std::size_t>(row) * w,
                                srcUV + static_cast<std::size_t>(row) * lsUV, w);
                }
            } else {
                // Convert other outputs (e.g. software yuv420p) to NV12.
                sws = sws_getCachedContext(sws, w, h, srcFmt, w, h, AV_PIX_FMT_NV12,
                                           SWS_BILINEAR, nullptr, nullptr, nullptr);
                if (sws == nullptr) {
                    PIXELVR_LOG_ERROR("MediaEngine: sws_getCachedContext failed");
                    return;
                }
                uint8_t* dstData[4] = {y.data(), uv.data(), nullptr, nullptr};
                int dstLinesize[4] = {w, w, 0, 0};
                sws_scale(sws, frame->data, frame->linesize, 0, h, dstData,
                          dstLinesize);
            }
        }

        int outW = w;
        double outPts = pts;
        if (frameSeq && !isDrm) {
            // FS3D: frames alternate views. Even frames (counted from the stream start) are the first
            // view of a pair, odd ones the second; a complete pair becomes one 2w x h SBS frame (left
            // view left) shown at the first frame's time. A lone second view (after a seek) is dropped.
            long long idx = std::llround((pts - frameSeqStart) * frameSeqFps);
            const bool firstOfPair = ((idx % 2) + 2) % 2 == 0;
            if (firstOfPair) {
                fsY = std::move(y);
                fsUV = std::move(uv);
                fsIdx = idx;
                fsPts = pts;
                return;
            }
            if (fsIdx != idx - 1 || fsY.size() != static_cast<std::size_t>(w) * h) return;
            const std::vector<uint8_t>& lY = frameSeqRightFirst ? y : fsY;
            const std::vector<uint8_t>& rY = frameSeqRightFirst ? fsY : y;
            const std::vector<uint8_t>& lUV = frameSeqRightFirst ? uv : fsUV;
            const std::vector<uint8_t>& rUV = frameSeqRightFirst ? fsUV : uv;
            std::vector<uint8_t> sy(static_cast<std::size_t>(w) * 2 * h), suv(static_cast<std::size_t>(w) * 2 * (h / 2));
            for (int row = 0; row < h; ++row) {
                std::memcpy(sy.data() + static_cast<std::size_t>(row) * 2 * w, lY.data() + static_cast<std::size_t>(row) * w, w);
                std::memcpy(sy.data() + static_cast<std::size_t>(row) * 2 * w + w, rY.data() + static_cast<std::size_t>(row) * w, w);
            }
            for (int row = 0; row < h / 2; ++row) {
                std::memcpy(suv.data() + static_cast<std::size_t>(row) * 2 * w, lUV.data() + static_cast<std::size_t>(row) * w, w);
                std::memcpy(suv.data() + static_cast<std::size_t>(row) * 2 * w + w, rUV.data() + static_cast<std::size_t>(row) * w, w);
            }
            y = std::move(sy);
            uv = std::move(suv);
            outW = 2 * w;
            outPts = fsPts;
            fsIdx = -10;
        }

        // With audio, the blocking audio writes pace the loop and provide the
        // master clock; without audio, pace video to its own PTS via wall clock.
        if (!audioEnabled) {
            if (!haveClock) {
                startTime = clock::now();
                startPts = outPts;
                haveClock = true;
            } else {
                const auto target =
                    startTime + std::chrono::duration_cast<clock::duration>(
                                    std::chrono::duration<double>(outPts - startPts));
                std::this_thread::sleep_until(target);
            }
            masterClock.store(outPts);
        }

        if (!running.load()) {
            if (drm.fd >= 0) {
                ::close(drm.fd);
            }
            return;
        }
        if (isDrm) {
            pushDrm(drm);
        } else {
            pushFrame(outW, h, std::move(y), std::move(uv), colorspace, fullRange, outPts);
        }
    };

#ifdef PIXELVR_HAVE_PULSE
    auto process_audio = [&]() {
        // The decoder's real output can differ from the stream header swr was set up from
        // (DTS-HD / TrueHD core vs. full layout after a seek, mid-stream changes). Feeding
        // swr frames with fewer planes than it expects reads past them (crash), so follow
        // the frame instead.
        if (frame->format != swrInFormat || frame->sample_rate != swrInRate ||
            av_channel_layout_compare(&frame->ch_layout, &swrInLayout) != 0) {
            AVChannelLayout outLayout;
            av_channel_layout_default(&outLayout, kOutChannels);
            SwrContext* fresh = nullptr;
            if (frame->sample_rate <= 0 || frame->ch_layout.nb_channels <= 0 ||
                swr_alloc_set_opts2(&fresh, &outLayout, AV_SAMPLE_FMT_S16, kOutSampleRate, &frame->ch_layout,
                                    static_cast<AVSampleFormat>(frame->format), frame->sample_rate, 0, nullptr) != 0 ||
                swr_init(fresh) < 0) {
                swr_free(&fresh);
                return;  // drop this frame rather than convert it wrongly
            }
            PIXELVR_LOG_INFO("MediaEngine: audio input changed to %d ch, %d Hz, %s; resampler rebuilt",
                             frame->ch_layout.nb_channels, frame->sample_rate,
                             av_get_sample_fmt_name(static_cast<AVSampleFormat>(frame->format)));
            swr_free(&swr);
            swr = fresh;
            av_channel_layout_uninit(&swrInLayout);
            av_channel_layout_copy(&swrInLayout, &frame->ch_layout);
            swrInFormat = frame->format;
            swrInRate = frame->sample_rate;
        }
        // Every plane swr will read must exist (a frame without them crashed swr_convert).
        if (frame->nb_samples <= 0 || frame->extended_data == nullptr) return;
        const int planes = av_sample_fmt_is_planar(static_cast<AVSampleFormat>(frame->format)) ? frame->ch_layout.nb_channels : 1;
        for (int pl = 0; pl < planes; ++pl) {
            if (frame->extended_data[pl] == nullptr) {
                PIXELVR_LOG_WARN("MediaEngine: audio frame without plane %d of %d dropped", pl, planes);
                return;
            }
        }
        const int maxOut = swr_get_out_samples(swr, frame->nb_samples);
        if (maxOut <= 0) {
            return;
        }
        std::vector<uint8_t> pcm(static_cast<std::size_t>(maxOut) * kOutChannels * 2);
        uint8_t* out[1] = {pcm.data()};
        const int got =
            swr_convert(swr, out, maxOut,
                        const_cast<const uint8_t**>(frame->extended_data),
                        frame->nb_samples);
        if (got <= 0) {
            return;
        }
        const std::size_t bytes = static_cast<std::size_t>(got) * kOutChannels * 2;
        pcm.resize(bytes);
        // A newer seek is pending: this sound is about to be flushed, so don't
        // queue it (repeated seeks then don't blip audio from each position).
        // Sound before the current target is dropped by the trim below.
        if (seekTarget.load() >= 0.0) {
            return;
        }
        if (!pa) {
            return;
        }

        int64_t ts = frame->best_effort_timestamp;
        if (ts == AV_NOPTS_VALUE) {
            ts = frame->pts;
        }
        double pts = (ts != AV_NOPTS_VALUE) ? (ts * av_q2d(audioTimeBase)) : -1.0;
        double dur = static_cast<double>(got) / static_cast<double>(kOutSampleRate);

        // After a seek, drop the sound before the target and trim the chunk
        // that straddles it, so audio starts exactly where the video does.
        if (audioDiscardUntil >= 0.0 && pts >= 0.0) {
            if (pts + dur <= audioDiscardUntil) {
                return;  // wholly before the target
            }
            if (pts < audioDiscardUntil) {
                const auto skipSamples =
                    static_cast<std::size_t>((audioDiscardUntil - pts) * kOutSampleRate);
                const std::size_t skip = skipSamples * kOutChannels * 2;
                if (skip > 0 && skip < pcm.size()) {
                    pcm.erase(pcm.begin(), pcm.begin() + static_cast<std::ptrdiff_t>(skip));
                    pts += static_cast<double>(skipSamples) / kOutSampleRate;
                    dur = static_cast<double>(pcm.size() / (kOutChannels * 2)) / kOutSampleRate;
                }
            }
            audioDiscardUntil = -1.0;
        }

        if (audioOnly) {
            // No video to pace the loop: hold decoding ~1.5 s ahead of playback.
            for (;;) {
                {
                    std::lock_guard<std::mutex> lk(audioMutex);
                    if (audioQueue.size() < 64) break;
                }
                if (!running.load() || seekTarget.load() >= 0.0) return;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        {
            std::lock_guard<std::mutex> lk(audioMutex);
            // Keep up to ~5s of decoded audio in RAM; never block the video decode loop.
            while (audioQueue.size() >= 250) {
                audioQueue.pop_front();
            }
            audioQueue.push_back(AudioChunk{std::move(pcm), pts, dur});
        }
        audioCv.notify_one();
    };
#endif

    auto drain = [&](AVCodecContext* ctx, bool video) {
        while (avcodec_receive_frame(ctx, frame) == 0) {
            if (video) {
                process_video();
            }
#ifdef PIXELVR_HAVE_PULSE
            else {
                process_audio();
            }
#endif
            if (!running.load()) {
                break;
            }
        }
    };

    while (running.load()) {
        // Throttle seek handling to ~8/s. Rapid scrubbing otherwise flushes the
        // hardware decoder faster than it can emit a frame, wedging it so the
        // picture freezes; a deferred seek stays in seekTarget, so the latest
        // still wins.
        double seekTo = -1.0;
        if (seekTarget.load() >= 0.0 &&
            std::chrono::duration<double>(clock::now() - lastSeekWall).count() >= 0.12) {
            lastSeekWall = clock::now();
            // Release every held dma-buf (queued frames + the renderer's live
            // frame) before flushing; the V4L2 decoder cannot reclaim its capture
            // buffers otherwise and wedges ("can't request capture buffers").
            {
                std::unique_lock<std::mutex> lock(mutex);
                // Taken under the lock with the request's serial, so a frame shown
                // later is known to belong to this seek (see settleDisplayLocked).
                seekTo = seekTarget.exchange(-1.0);
                appliedSerial = seekSerial;
                clearQueues();  // drop stale pre-seek frames
                releaseReq.store(true);
                releaseCv.wait_for(lock, std::chrono::milliseconds(150), [&] {
                    return !releaseReq.load() || !running.load();
                });
            }
            const int64_t ts = static_cast<int64_t>(seekTo * AV_TIME_BASE);
            // Never land on a keyframe after the target (demuxers with their own
            // seek2); fall back to the open bound when nothing lies before it.
            if (avformat_seek_file(format, -1, INT64_MIN, ts, ts, 0) < 0) {
                avformat_seek_file(format, -1, INT64_MIN, ts, INT64_MAX, 0);
            }
            const bool reopened = audioOnly || reopenDecoder();  // fresh decoder avoids HW wedge
            if (!reopened) {
                PIXELVR_LOG_ERROR("MediaEngine: video decoder reopen failed on seek");
            }
            PIXELVR_LOG_INFO("MediaEngine: seek %.2fs reopen=%d", seekTo, reopened);
            drmDiag = 0;  // re-log the first frames so seek recovery is visible
            if (audioCodec != nullptr) {
                avcodec_flush_buffers(audioCodec);
            }
#ifdef PIXELVR_HAVE_PULSE
            {
                std::lock_guard<std::mutex> lk(audioMutex);
                audioQueue.clear();
                audioReanchor = true;  // the next queued chunk is post-seek
            }
            if (pa) {
                int pErr = 0;
                pa_simple_flush(pa, &pErr);
            }
            audioCv.notify_all();
#endif
            haveClock = false;
            masterClock.store(seekTo);
            playClock.reset();  // the next presented frame re-anchors the clock
            videoDiscardUntil = audioOnly ? -1.0 : seekTo;
            fsIdx = -10;  // FS3D: a pair never spans a seek
            audioDiscardUntil = seekTo;
            discardCount = 0;
            eofFlushed = false;
            readRetries = 0;
            eof.store(false);
        }
        if (paused.load()) {
            if (!wasPaused) {
#ifdef PIXELVR_HAVE_PULSE
                if (pa) {
                    int pErr = 0;
                    pa_simple_flush(pa, &pErr);
                }
                audioCv.notify_all();
#endif
                wasPaused = true;
            }
            haveClock = false;  // re-establish the wall clock on resume
            playClock.freeze();  // hold the clock so video doesn't advance
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        if (wasPaused) {
            wasPaused = false;
#ifdef PIXELVR_HAVE_PULSE
            audioCv.notify_all();
#endif
        }
        playClock.resume();  // no-op unless resuming from pause
        const int readResult = av_read_frame(format, packet);
        // The I/O position at the file's known size is its real end, whatever error came with it.
        const int64_t ioSize = (format->pb != nullptr) ? avio_size(format->pb) : -1;
        const bool atFileEnd = ioSize > 0 && avio_tell(format->pb) >= ioSize;
        if (readResult >= 0) {
            readRetries = 0;
        } else if (httpSource && !eofFlushed && readRetries < 6 && !atFileEnd &&
                   (readResult != AVERROR_EOF || duration <= 0.0 || masterClock.load() < duration - 15.0)) {
            // A dropped connection far from the end is not the end of the file:
            // re-seek the I/O layer to where it stopped (this clears its sticky
            // EOF/error flag and makes the HTTP layer open a fresh request).
            ++readRetries;
            PIXELVR_LOG_WARN("MediaEngine: network read error (%d), retry %d", readResult, readRetries);
            std::this_thread::sleep_for(std::chrono::milliseconds(300 * readRetries));
            if (format->pb != nullptr) avio_seek(format->pb, avio_tell(format->pb), SEEK_SET | AVSEEK_FORCE);
            continue;
        }
        if (readResult < 0) {
            if (!eofFlushed) {
                // A seek at the very end: show the last frames rather than none.
                videoDiscardUntil = -1.0;
                audioDiscardUntil = -1.0;
                if (codec != nullptr) {
                    avcodec_send_packet(codec, nullptr);  // flush video
                    drain(codec, true);
                }
#ifdef PIXELVR_HAVE_PULSE
                if (audioEnabled) {
                    avcodec_send_packet(audioCodec, nullptr);
                    drain(audioCodec, false);
                }
#endif
                eofFlushed = true;
                if (!audioOnly) eof.store(true);
            }
#ifdef PIXELVR_HAVE_PULSE
            if (audioOnly && !eof.load()) {
                // Audio files end once the queued sound has played.
                std::lock_guard<std::mutex> lk(audioMutex);
                if (audioQueue.empty()) eof.store(true);
            }
#else
            eof.store(true);
#endif
            // Stay alive so a seek can resume instead of ending the thread.
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
            continue;
        }

        if (packet->stream_index == videoStream) {
            if (avcodec_send_packet(codec, packet) == 0) {
                drain(codec, true);
            }
        }
#ifdef PIXELVR_HAVE_PULSE
        else if (audioEnabled && packet->stream_index == audioStream) {
            if (avcodec_send_packet(audioCodec, packet) == 0) {
                drain(audioCodec, false);
            }
        }
#endif
        av_packet_unref(packet);
    }

    av_packet_free(&packet);
    av_frame_free(&frame);
}

void MediaEngine::Impl::audioThreadLoop() {
#ifdef PIXELVR_HAVE_PULSE
    // Pre-write lead time (~45ms) matching the PulseAudio buffer so samples
    // written now emerge from the speakers when playClock reaches chunk.pts.
    constexpr double kAudioLeadSec = 0.045;

    while (running.load()) {
        if (audioOnly && !paused.load()) {
            // No video frame anchors the clock: the first queued sound does
            // (again after a seek, when the clock restarts from zero).
            double anchored = -1.0;
            {
                std::lock_guard<std::mutex> lk(audioMutex);
                if (!audioQueue.empty() && audioQueue.front().pts >= 0.0) {
                    const double first = audioQueue.front().pts;
                    if (audioReanchor || !playClock.running.load() ||
                        std::abs(playClock.now() - first) > 0.5) {
                        playClock.start(first);
                        anchored = first;
                        audioReanchor = false;
                    }
                }
            }
            if (anchored >= 0.0) {
                // The sound after the seek is due: report its time from now on.
                masterClock.store(anchored, std::memory_order_relaxed);
                std::lock_guard<std::mutex> lk(mutex);
                settleDisplayLocked();
            }
        }
        if (paused.load() || !playClock.running.load(std::memory_order_relaxed)) {
            std::unique_lock<std::mutex> lk(audioMutex);
            audioCv.wait_for(lk, std::chrono::milliseconds(10));
            continue;
        }

        AudioChunk chunk;
        bool haveChunk = false;
        double waitSec = 0.0;
        {
            std::unique_lock<std::mutex> lk(audioMutex);
            if (audioQueue.empty()) {
                audioCv.wait_for(lk, std::chrono::milliseconds(10));
                continue;
            }

            const double vClock = playClock.now();
            // Drop any chunks that are already late (> 80ms behind current video clock)
            // so post-startup or post-seek audio snaps into sync with video immediately.
            while (!audioQueue.empty() && audioQueue.front().pts >= 0.0 &&
                   (audioQueue.front().pts + audioQueue.front().duration) < (vClock - 0.080)) {
                audioQueue.pop_front();
            }
            if (audioQueue.empty()) {
                continue;
            }

            const double targetPlayTime = vClock + kAudioLeadSec;
            if (audioQueue.front().pts >= 0.0 &&
                audioQueue.front().pts > targetPlayTime + 0.015) {
                // Audio is ahead of the video presentation clock (because video frames
                // are buffered in the V4L2 decoder + drmQueue). Hold until due.
                waitSec = std::min(0.015, audioQueue.front().pts - targetPlayTime);
            } else {
                chunk = std::move(audioQueue.front());
                audioQueue.pop_front();
                haveChunk = true;
            }
        }

        if (waitSec > 0.0) {
            std::this_thread::sleep_for(std::chrono::duration<double>(waitSec));
            continue;
        }

        if (haveChunk && pa != nullptr && !chunk.pcm.empty()) {
            int err = 0;
            if (muted.load(std::memory_order_relaxed)) {
                // Write silence of the same length so pacing / A-V sync is unchanged.
                std::fill(chunk.pcm.begin(), chunk.pcm.end(), 0);
            } else if (const float vol = volume.load(std::memory_order_relaxed); vol < 0.999f) {
                auto* s16 = reinterpret_cast<int16_t*>(chunk.pcm.data());
                const size_t n = chunk.pcm.size() / 2;
                const float g = vol * vol;  // roughly even loudness steps
                for (size_t i = 0; i < n; ++i) s16[i] = static_cast<int16_t>(static_cast<float>(s16[i]) * g);
            }
            pa_simple_write(pa, chunk.pcm.data(), chunk.pcm.size(), &err);
            if (audioOnly) masterClock.store(playClock.now(), std::memory_order_relaxed);
        }
    }
#endif
}

MediaEngine::MediaEngine() : impl_(std::make_unique<Impl>()) {
    avformat_network_init();
}

MediaEngine::~MediaEngine() {
    close();
}

const std::string& MediaEngine::lastError() const { return impl_->lastError; }

void MediaEngine::resetAbort() {
    if (impl_) impl_->openAborted.store(false, std::memory_order_release);
}

void MediaEngine::abortOpen() {
    if (!impl_) return;
    impl_->openAborted.store(true, std::memory_order_release);
    impl_->remoteReader.abort();
}

bool MediaEngine::open(const std::string& path) {
    close();
    if (impl_->openAborted.load(std::memory_order_acquire)) {
        impl_->lastError = "cancelled";
        return false;
    }
    impl_->lastError = "the file could not be opened";
    impl_->httpSource = false;
    impl_->readRetries = 0;

    if (vaultcrypto::isEncrypted(path)) {
        // Encrypted vault file: decrypted while reading (needs the vault unlocked).
        if (!openVaultFormat(path, impl_->format, impl_->vaultIo)) {
            impl_->lastError = vaultcrypto::hasKey() ? "the vault file could not be read" : "the vault is locked";
            close();
            return false;
        }
    } else if (isRemotePath(path)) {
        std::unique_ptr<RemoteFile> remote;
        if (path.rfind("smb://", 0) != 0) {
            std::string err;
            remote = remoteOpen(path, err);
            if (!remote) {
                impl_->lastError = err;
                PIXELVR_LOG_ERROR("MediaEngine: cannot open %s: %s", path.c_str(), err.c_str());
                return false;
            }
        } else {
#ifdef PIXELVR_HAVE_SMB
        // libsmbclient is not thread-safe; serialize context setup against the
        // browser's scans/downloads, which share Samba's global state.
        SMBCCTX* ctx = nullptr;
        SMBCFILE* file = nullptr;
        {
            std::lock_guard<std::mutex> smb(smbGlobalLock());
            ctx = smbc_new_context();
            if (ctx != nullptr) {
                smbc_setFunctionAuthDataWithContext(ctx, &smb_auth);
                if (smbc_init_context(ctx) == nullptr) {
                    smbc_free_context(ctx, 1);
                    ctx = nullptr;
                }
            }
            smbc_open_fn openFn =
                (ctx != nullptr) ? smbc_getFunctionOpen(ctx) : nullptr;
            file = (openFn != nullptr) ? openFn(ctx, path.c_str(), O_RDONLY, 0)
                                       : nullptr;
            if (file == nullptr && ctx != nullptr) {
                smbc_free_context(ctx, 1);
                ctx = nullptr;
            }
            if (file != nullptr) {
                remote = std::make_unique<SmbFile>(ctx, file);
            }
        }
        if (!remote) {
            PIXELVR_LOG_ERROR("MediaEngine: cannot open SMB stream %s", path.c_str());
            return false;
        }
#else
            impl_->lastError = "SMB support not built";
            return false;
#endif
        }
        if (impl_->openAborted.load(std::memory_order_acquire)) {
            impl_->lastError = "cancelled";
            return false;
        }
        impl_->remoteReader.start(std::move(remote));
        if (impl_->openAborted.load(std::memory_order_acquire)) {
            impl_->remoteReader.abort();
            close();
            impl_->lastError = "cancelled";
            return false;
        }

        constexpr int kBuf = 1 << 16;  // 64 KiB AVIO buffer (ring is separate)
        auto* buffer = static_cast<unsigned char*>(av_malloc(kBuf));
        impl_->avio = (buffer != nullptr)
                          ? avio_alloc_context(buffer, kBuf, 0, &impl_->remoteReader,
                                               &remote_read, nullptr, &remote_seek)
                          : nullptr;
        if (impl_->avio == nullptr) {
            av_free(buffer);
            close();
            PIXELVR_LOG_ERROR("MediaEngine: network AVIO alloc failed");
            return false;
        }
        impl_->format = avformat_alloc_context();
        if (impl_->format == nullptr) {
            close();
            return false;
        }
        impl_->format->interrupt_callback.callback = [](void* opaque) -> int {
            return static_cast<MediaEngine::Impl*>(opaque)->openAborted.load(std::memory_order_relaxed) ? 1 : 0;
        };
        impl_->format->interrupt_callback.opaque = impl_.get();
        impl_->format->pb = impl_->avio;
        impl_->format->flags |= AVFMT_FLAG_CUSTOM_IO;
        if (avformat_open_input(&impl_->format, path.c_str(), nullptr, nullptr) < 0) {
            if (impl_->openAborted.load(std::memory_order_acquire)) impl_->lastError = "cancelled";
            PIXELVR_LOG_ERROR("MediaEngine: cannot open network stream %s", path.c_str());
            close();
            return false;
        }
    } else {
    AVDictionary* httpOpts = nullptr;
    impl_->httpSource = path.rfind("http://", 0) == 0 || path.rfind("https://", 0) == 0;
    if (impl_->httpSource) {
        // Servers and NATs drop long transfers (Plex: every 10-60 s); resume
        // from the current offset instead of ending the stream.
        // (Not reconnect_at_eof: at the file's real end it reconnected twice with 0+1+3 s waits, so an
        // episode "ended" ~15 s late. A drop mid-file is "stream ends prematurely" (EIO) and is
        // covered by reconnect while the offset is below the known file size.)
        av_dict_set(&httpOpts, "reconnect", "1", 0);
        av_dict_set(&httpOpts, "reconnect_streamed", "1", 0);
        av_dict_set(&httpOpts, "reconnect_on_network_error", "1", 0);
        av_dict_set(&httpOpts, "reconnect_on_http_error", "4xx,5xx", 0);
        av_dict_set(&httpOpts, "reconnect_delay_max", "5", 0);
        av_dict_set(&httpOpts, "multiple_requests", "1", 0);
    }
    impl_->format = avformat_alloc_context();
    if (impl_->format == nullptr) {
        av_dict_free(&httpOpts);
        return false;
    }
    impl_->format->interrupt_callback.callback = [](void* opaque) -> int {
        return static_cast<MediaEngine::Impl*>(opaque)->openAborted.load(std::memory_order_relaxed) ? 1 : 0;
    };
    impl_->format->interrupt_callback.opaque = impl_.get();
    const int openRc = avformat_open_input(&impl_->format, path.c_str(), nullptr, &httpOpts);
    av_dict_free(&httpOpts);
    if (openRc < 0) {
        if (impl_->openAborted.load(std::memory_order_acquire)) impl_->lastError = "cancelled";
        PIXELVR_LOG_ERROR("MediaEngine: cannot open %s", path.c_str());
        return false;
    }
    }
    if (impl_->openAborted.load(std::memory_order_acquire) ||
        avformat_find_stream_info(impl_->format, nullptr) < 0) {
        impl_->lastError = impl_->openAborted.load(std::memory_order_acquire)
                               ? "cancelled"
                               : "unknown or damaged file format";
        PIXELVR_LOG_ERROR("MediaEngine: no stream info in %s", path.c_str());
        close();
        return false;
    }

    impl_->chapters.clear();
    for (unsigned ci = 0; ci < impl_->format->nb_chapters; ++ci) {
        const AVChapter* ch = impl_->format->chapters[ci];
        Chapter c;
        c.start = std::max(0.0, static_cast<double>(ch->start) * av_q2d(ch->time_base));
        if (const AVDictionaryEntry* t = av_dict_get(ch->metadata, "title", nullptr, 0)) c.title = t->value;
        // Many rips carry chapter marks without real names: empty, or the
        // chapter's own timecode ("00:33:20.415"), or just digits. Number those.
        const bool placeholder = std::all_of(c.title.begin(), c.title.end(), [](unsigned char ch2) {
            return std::isdigit(ch2) || ch2 == ':' || ch2 == '.' || ch2 == ',' || ch2 == ' ';
        });
        if (placeholder) c.title = "Chapter " + std::to_string(ci + 1);
        impl_->chapters.push_back(std::move(c));
    }
    std::sort(impl_->chapters.begin(), impl_->chapters.end(),
              [](const Chapter& a, const Chapter& b) { return a.start < b.start; });

    // Enumerate selectable video/audio tracks (with language metadata) so the UI
    // can offer track + language selection.
    impl_->videoStreamList.clear();
    impl_->audioStreamList.clear();
    for (unsigned si = 0; si < impl_->format->nb_streams; ++si) {
        AVStream* s = impl_->format->streams[si];
        const AVCodecParameters* cp = s->codecpar;
        std::string lang;
        if (AVDictionaryEntry* e =
                av_dict_get(s->metadata, "language", nullptr, 0)) {
            lang = e->value;
        }
        const AVCodec* dc = avcodec_find_decoder(cp->codec_id);
        char buf[160];
        if (cp->codec_type == AVMEDIA_TYPE_VIDEO && (impl_->format->streams[si]->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
            continue;  // embedded cover art: not a track to choose
        }
        if (cp->codec_type == AVMEDIA_TYPE_VIDEO) {
            std::snprintf(buf, sizeof(buf), "%s %dx%d%s%s", dc ? dc->name : "?",
                          cp->width, cp->height, lang.empty() ? "" : " ",
                          lang.c_str());
            impl_->videoStreamList.push_back({static_cast<int>(si), buf, lang});
        } else if (cp->codec_type == AVMEDIA_TYPE_AUDIO) {
            std::snprintf(buf, sizeof(buf), "%s %dch%s%s", dc ? dc->name : "?",
                          cp->ch_layout.nb_channels, lang.empty() ? "" : " ",
                          lang.c_str());
            impl_->audioStreamList.push_back({static_cast<int>(si), buf, lang});
        }
    }

    const int wantVideo =
        (impl_->preferredVideoStream >= 0) ? impl_->preferredVideoStream : -1;
    if (wantVideo >= 0 && wantVideo < static_cast<int>(impl_->format->nb_streams) &&
        impl_->format->streams[wantVideo]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
        impl_->videoStream = wantVideo;
    } else {
        impl_->videoStream = findVideoStream(impl_->format);  // cover art is not video
    }
    impl_->audioOnly = false;
    if (impl_->videoStream < 0) {
        if (av_find_best_stream(impl_->format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0) < 0) {
            impl_->lastError = "no video or audio in the file";
            PIXELVR_LOG_ERROR("MediaEngine: no video or audio stream in %s", path.c_str());
            close();
            return false;
        }
        // Audio file: the screen shows its cover art (or a placeholder) as a
        // single NV12 frame; playback runs on the audio clock.
        impl_->audioOnly = true;
        constexpr int kCardW = 1280, kCardH = 720;
        std::vector<uint8_t> cover, rgba;
        int cw = 0, ch = 0;
        decodeCoverArt(impl_->format, cover, cw, ch);
        composeAudioCard(cover, cw, ch, kCardW, kCardH, rgba, true);
        readAudioTags(impl_->format, impl_->tags);
        VideoFrame& card = impl_->card;
        card.width = kCardW;
        card.height = kCardH;
        card.y.assign(static_cast<size_t>(kCardW) * kCardH, 0);
        card.uv.assign(static_cast<size_t>(kCardW) * kCardH / 2, 128);
        if (SwsContext* sws = sws_getContext(kCardW, kCardH, AV_PIX_FMT_RGBA, kCardW, kCardH, AV_PIX_FMT_NV12,
                                             SWS_BILINEAR, nullptr, nullptr, nullptr)) {
            const uint8_t* src[4] = {rgba.data(), nullptr, nullptr, nullptr};
            const int srcStride[4] = {kCardW * 4, 0, 0, 0};
            uint8_t* dst[4] = {card.y.data(), card.uv.data(), nullptr, nullptr};
            const int dstStride[4] = {kCardW, kCardW, 0, 0};
            sws_scale(sws, src, srcStride, 0, kCardH, dst, dstStride);
            sws_freeContext(sws);
        }
        card.colorspace = AVCOL_SPC_BT709;
        card.fullRange = false;
        card.pts = 0.0;
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            card.serial = ++impl_->serial;
            impl_->cardPending = true;
        }
        impl_->width = kCardW;
        impl_->height = kCardH;
        impl_->frameRate = 0.0;
        impl_->duration = (impl_->format->duration > 0)
                              ? impl_->format->duration / static_cast<double>(AV_TIME_BASE)
                              : 0.0;
        impl_->decodedCount.store(0);
        PIXELVR_LOG_INFO("MediaEngine: %s  audio only  %.1fs  cover=%s", path.c_str(), impl_->duration,
                         cover.empty() ? "none" : "yes");
    }

    if (!impl_->audioOnly) {
        AVStream* stream = impl_->format->streams[impl_->videoStream];
        impl_->formatHint = formatFromMetadata(stream);
        impl_->timeBase = stream->time_base;
        impl_->frameRate = av_q2d(av_guess_frame_rate(impl_->format, stream, nullptr));
        PIXELVR_LOG_INFO("MediaEngine: source frame rate %.3f fps", impl_->frameRate);
        {
            // FS3D: the Matroska StereoMode (block_lr / block_rl), FFmpeg's stereo side data, or the name.
            bool fs = false, rightFirst = false;
            if (const AVDictionaryEntry* sm = av_dict_get(stream->metadata, "stereo_mode", nullptr, 0)) {
                fs = std::strcmp(sm->value, "block_lr") == 0 || std::strcmp(sm->value, "block_rl") == 0;
                rightFirst = std::strcmp(sm->value, "block_rl") == 0;
            }
            for (int k = 0; k < stream->codecpar->nb_coded_side_data; ++k) {
                const AVPacketSideData& sd = stream->codecpar->coded_side_data[k];
                if (sd.type == AV_PKT_DATA_STEREO3D && sd.size >= static_cast<int>(sizeof(AVStereo3D))) {
                    const AVStereo3D* st = reinterpret_cast<const AVStereo3D*>(sd.data);
                    if (st->type == AV_STEREO3D_FRAMESEQUENCE) {
                        fs = true;
                        rightFirst = rightFirst || (st->flags & AV_STEREO3D_FLAG_INVERT) != 0;
                    }
                }
            }
            std::string lname = path;
            for (char& ch : lname) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (lname.find("fs3d") != std::string::npos || lname.find("frame sequential") != std::string::npos ||
                lname.find("frame-sequential") != std::string::npos || lname.find("framesequential") != std::string::npos) {
                fs = true;
            }
            // Only plausible with a doubled frame rate (both views: 47.952 / 48 / 50 / 59.94 / 60 ...).
            // Marked only inside the H.264 / H.265 stream (SEI 45, temporal interleaving): read the first
            // video packets, then go back to the start. Seekable sources only (local files, shares).
            const AVCodecID cid = stream->codecpar->codec_id;
            if (!fs && impl_->frameSeqMode != 0 && impl_->frameRate >= 40.0 &&
                (cid == AV_CODEC_ID_H264 || cid == AV_CODEC_ID_HEVC) && impl_->format->pb &&
                (impl_->format->pb->seekable & AVIO_SEEKABLE_NORMAL)) {
                const bool hevc = cid == AV_CODEC_ID_HEVC;
                const uint8_t* ex = stream->codecpar->extradata;
                const int exn = stream->codecpar->extradata_size;
                int lenSize = 0;  // avcC / hvcC: length-prefixed NALs; else Annex B
                if (ex && exn > 22 && ex[0] == 1) lenSize = hevc ? (ex[21] & 3) + 1 : (ex[4] & 3) + 1;
                else if (ex && exn > 5 && !hevc && ex[0] == 1) lenSize = (ex[4] & 3) + 1;
                AVPacket* pk = av_packet_alloc();
                int videoPackets = 0, found = -1;
                bool rf = false;
                for (int k = 0; k < 120 && videoPackets < 12 && found < 0 && pk; ++k) {
                    if (av_read_frame(impl_->format, pk) < 0) break;
                    if (pk->stream_index == impl_->videoStream) {
                        ++videoPackets;
                        found = fpsei::scanPacket(pk->data, static_cast<size_t>(pk->size), lenSize, hevc, rf);
                    }
                    av_packet_unref(pk);
                }
                av_packet_free(&pk);
                const int64_t back = stream->start_time != AV_NOPTS_VALUE ? stream->start_time : 0;
                av_seek_frame(impl_->format, impl_->videoStream, back, AVSEEK_FLAG_BACKWARD);
                if (found == 1) {
                    fs = true;
                    rightFirst = rf;
                    PIXELVR_LOG_INFO("MediaEngine: frame-sequential 3D marked in the %s stream (SEI)", hevc ? "H.265" : "H.264");
                }
            }
            if (impl_->frameSeqMode == 0) fs = false;
            if (impl_->frameSeqMode == 1) fs = true;  // chosen by hand
            impl_->frameSeq = fs && impl_->frameRate >= 40.0;
            impl_->frameSeqRightFirst = rightFirst;
            impl_->frameSeqFps = impl_->frameRate;
            impl_->frameSeqStart = stream->start_time != AV_NOPTS_VALUE ? stream->start_time * av_q2d(stream->time_base) : 0.0;
            if (fs) {
                PIXELVR_LOG_INFO("MediaEngine: frame-sequential 3D%s (%s first, %.3f fps -> %.3f per eye)",
                                 impl_->frameSeq ? "" : " tagged, but the frame rate is too low: played as 2D",
                                 rightFirst ? "right" : "left", impl_->frameRate, impl_->frameRate / 2.0);
            }
        }
        const AVCodecID codecId = stream->codecpar->codec_id;

        // The Iris V4L2-M2M hardware path (H.264/HEVC/VP9) handles 8-bit here; route
        // 10-bit+ to software (swscale downconverts to NV12) until dmabuf/P010 import
        // lands. Prefer the hardware decoder by name; fall back to software so the
        // same build also runs on a desktop for development.
        const bool forceSoftware = std::getenv("PIXELVR_FORCE_SW") != nullptr;
        const bool drmPrime = std::getenv("PIXELVR_DRM_PRIME") != nullptr;
        const AVPixFmtDescriptor* srcDesc =
            av_pix_fmt_desc_get(static_cast<AVPixelFormat>(stream->codecpar->format));
        const bool highBitDepth = srcDesc != nullptr && srcDesc->comp[0].depth > 8;
        // Hardware handles 8-bit for all codecs; 10-bit only for HEVC and only via the
        // dmabuf/P010 path (drm_prime). Otherwise route high-bit-depth to software.
        const bool hwEligible =
            !highBitDepth || (drmPrime && codecId == AV_CODEC_ID_HEVC);
        const char* hwName = !hwEligible ? nullptr
                             : (codecId == AV_CODEC_ID_H264) ? "h264_v4l2m2m"
                             : (codecId == AV_CODEC_ID_HEVC) ? "hevc_v4l2m2m"
                             : (codecId == AV_CODEC_ID_VP9)  ? "vp9_v4l2m2m"
                                                             : nullptr;
        const AVCodec* decoder = (hwName != nullptr && !forceSoftware)
                                     ? avcodec_find_decoder_by_name(hwName)
                                     : nullptr;
        bool hardware = decoder != nullptr;
        if (decoder == nullptr) {
            // FFmpeg's own "av1" decoder needs a hardware accelerator, which this
            // device lacks: AV1 plays only through dav1d (software).
            decoder = (codecId == AV_CODEC_ID_AV1) ? avcodec_find_decoder_by_name("libdav1d")
                                                   : avcodec_find_decoder(codecId);
        }
        if (decoder == nullptr) {
            impl_->lastError = std::string("no decoder for ") + avcodec_get_name(codecId) + " video";
            PIXELVR_LOG_ERROR("MediaEngine: no decoder for %s", path.c_str());
            close();
            return false;
        }

        auto open_with = [&](const AVCodec* dec) -> bool {
            if (impl_->codec != nullptr) {
                avcodec_free_context(&impl_->codec);
            }
            impl_->codec = avcodec_alloc_context3(dec);
            if (impl_->codec == nullptr ||
                avcodec_parameters_to_context(impl_->codec, stream->codecpar) < 0) {
                return false;
            }
            impl_->codec->pkt_timebase = stream->time_base;
            impl_->codec->thread_count = 0;  // Auto-detect and utilize all CPU cores
            impl_->codec->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
            impl_->videoDecoder = dec;  // retained so the decode thread can reopen on seek
            if (std::strstr(dec->name, "v4l2m2m") != nullptr) {
                av_opt_set_int(impl_->codec->priv_data, "num_output_buffers", 4, 0);
            }
            // (FS3D pairs two frames into one on the CPU, so it takes the NV12 copy path.)
            impl_->decoderDrm = drmPrime && std::strstr(dec->name, "v4l2m2m") != nullptr && !impl_->frameSeq;
            if (impl_->decoderDrm) {
                // Export decoder output as DMA-BUF (AV_PIX_FMT_DRM_PRIME) for zero-copy
                // Vulkan import; requires the dmabuf-patched libavcodec. Bound the
                // decoded-buffer memory: at 8K a frame is ~40-80 MiB, so a fixed large
                // count exhausts the decoder's DMA pool and REQBUFS fails -> POLLERR.
                const int64_t px = static_cast<int64_t>(stream->codecpar->width) *
                                   stream->codecpar->height;
                const int64_t frameBytes = highBitDepth ? px * 3 : px * 3 / 2;  // P010/NV12
                constexpr int64_t kBudget = 640ll << 20;  // ~640 MiB: 8K gets 12 buffers (6 left no slack)
                impl_->captureBuffers = static_cast<int>(std::clamp<int64_t>(
                    kBudget / std::max<int64_t>(frameBytes, 1), 4, 20));
                av_opt_set_int(impl_->codec->priv_data, "num_capture_buffers",
                               impl_->captureBuffers, 0);
                av_opt_set_int(impl_->codec->priv_data, "drm_prime", 1, 0);
                impl_->codec->apply_cropping = 0;
                // Leave buffers free for the decoder's DPB; the queue holds the rest.
                // A deeper queue absorbs bursty decode output (B-frame reorder, SMB
                // jitter) so low-framerate playback doesn't underrun and judder.
                impl_->maxQueue =
                    static_cast<std::size_t>(std::clamp(impl_->captureBuffers - 5, 1, 14));
            }
            return avcodec_open2(impl_->codec, dec, nullptr) >= 0;
        };

        impl_->hwBlockedBy.clear();
        bool opened = open_with(decoder);
        if (!opened && hardware && !impl_->openAborted.load(std::memory_order_acquire)) {
            // The previous session (ours or a just-closed browser page's) may
            // still be winding down in the driver; give it a moment before
            // settling for software, which can't keep up with 8K.
            for (int attempt = 0; attempt < 4 && !opened && !impl_->openAborted.load(std::memory_order_acquire); ++attempt) {
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
                if (impl_->openAborted.load(std::memory_order_acquire)) break;
                opened = open_with(decoder);
            }
            if (opened) PIXELVR_LOG_INFO("MediaEngine: hardware decoder opened on retry");
        }
        if (!opened) {
            if (hardware) {
                // Name whoever else has the decoder open: the driver refuses a
                // big session while another process (Steam's own web helper
                // after a store trailer, say) holds one.
                const std::string holders = decoderHolders();
                impl_->hwBlockedBy = holders;
                PIXELVR_LOG_WARN("MediaEngine: hardware decoder failed, trying software%s%s",
                                 holders.empty() ? "" : " (decoder also open in: ",
                                 holders.empty() ? "" : (holders + ")").c_str());
                hardware = false;
                decoder = avcodec_find_decoder(codecId);
                if (decoder == nullptr || !open_with(decoder)) {
                    impl_->lastError = std::string("cannot decode ") + avcodec_get_name(codecId) + " video";
                    PIXELVR_LOG_ERROR("MediaEngine: failed to open decoder for %s",
                                      path.c_str());
                    close();
                    return false;
                }
            } else {
                impl_->lastError = std::string("cannot decode ") + avcodec_get_name(codecId) + " video";
                PIXELVR_LOG_ERROR("MediaEngine: failed to open decoder for %s",
                                  path.c_str());
                close();
                return false;
            }
        }

        impl_->width = impl_->codec->width * (impl_->frameSeq ? 2 : 1);  // FS3D: delivered as full SBS
        impl_->height = impl_->codec->height;
        impl_->duration = (impl_->format->duration > 0)
                              ? impl_->format->duration / static_cast<double>(AV_TIME_BASE)
                              : 0.0;

        PIXELVR_LOG_INFO("MediaEngine: %s  %dx%d  %.1fs  codec=%s (%s)", path.c_str(),
                         impl_->width, impl_->height, impl_->duration, decoder->name,
                         hardware ? "hardware" : "software");
        impl_->codecName =
            std::string(decoder->name) + (hardware ? " (hw)" : " (sw)");
        impl_->decodedCount.store(0);
    }

#ifdef PIXELVR_HAVE_PULSE
    const AVCodec* adec = nullptr;
    int wantAudio = impl_->preferredAudioStream;
    if (wantAudio < 0 && !impl_->preferredLanguage.empty()) {
        for (const auto& si : impl_->audioStreamList) {
            if (si.language == impl_->preferredLanguage) {
                wantAudio = si.index;
                break;
            }
        }
    }
    if (wantAudio >= 0 && wantAudio < static_cast<int>(impl_->format->nb_streams) &&
        impl_->format->streams[wantAudio]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
        impl_->audioStream = wantAudio;
        adec = avcodec_find_decoder(impl_->format->streams[wantAudio]->codecpar->codec_id);
    } else {
        impl_->audioStream = av_find_best_stream(impl_->format, AVMEDIA_TYPE_AUDIO,
                                                 wantAudio, -1, &adec, 0);
    }
    if (impl_->audioStream >= 0 && adec != nullptr) {
        AVStream* as = impl_->format->streams[impl_->audioStream];
        impl_->audioTimeBase = as->time_base;
        impl_->audioCodec = avcodec_alloc_context3(adec);
        if (impl_->audioCodec != nullptr &&
            avcodec_parameters_to_context(impl_->audioCodec, as->codecpar) >= 0 &&
            avcodec_open2(impl_->audioCodec, adec, nullptr) >= 0) {
            AVChannelLayout outLayout;
            av_channel_layout_default(&outLayout, MediaEngine::Impl::kOutChannels);
            if (swr_alloc_set_opts2(&impl_->swr, &outLayout, AV_SAMPLE_FMT_S16,
                                    MediaEngine::Impl::kOutSampleRate,
                                    &impl_->audioCodec->ch_layout,
                                    impl_->audioCodec->sample_fmt,
                                    impl_->audioCodec->sample_rate, 0, nullptr) == 0 &&
                swr_init(impl_->swr) >= 0) {
                av_channel_layout_copy(&impl_->swrInLayout, &impl_->audioCodec->ch_layout);
                impl_->swrInFormat = impl_->audioCodec->sample_fmt;
                impl_->swrInRate = impl_->audioCodec->sample_rate;
                pa_sample_spec spec;
                spec.format = PA_SAMPLE_S16LE;
                spec.rate = MediaEngine::Impl::kOutSampleRate;
                spec.channels = MediaEngine::Impl::kOutChannels;
                pa_buffer_attr ba;
                std::memset(&ba, 0xff, sizeof(ba));
                ba.tlength = static_cast<uint32_t>(pa_usec_to_bytes(60000, &spec));
                ba.minreq = static_cast<uint32_t>(pa_usec_to_bytes(20000, &spec));
                ba.prebuf = static_cast<uint32_t>(-1);
                ba.maxlength = static_cast<uint32_t>(-1);
                impl_->pa =
                    pa_simple_new(nullptr, "PixelVRPlayer", PA_STREAM_PLAYBACK,
                                  nullptr, "video", &spec, nullptr, &ba, nullptr);
                impl_->audioEnabled = (impl_->pa != nullptr);
            }
            av_channel_layout_uninit(&outLayout);
        }
        if (impl_->audioOnly) impl_->codecName = std::string(adec->name) + " (audio)";
        if (impl_->audioEnabled) {
            PIXELVR_LOG_INFO("MediaEngine: audio %d Hz stereo",
                             MediaEngine::Impl::kOutSampleRate);
        } else {
            PIXELVR_LOG_WARN("MediaEngine: audio unavailable, video-only playback");
        }
    }
#endif
    if (impl_->audioOnly && !impl_->audioEnabled) {
        // Nothing would pace playback: it would end at once (and a music queue
        // would race through the folder).
        impl_->lastError = "no audio output available";
        PIXELVR_LOG_ERROR("MediaEngine: audio file without audio output: %s", path.c_str());
        close();
        return false;
    }

    if (impl_->openAborted.load(std::memory_order_acquire)) {
        close();
        impl_->lastError = "cancelled";
        return false;
    }

    impl_->lastError.clear();
    impl_->eof.store(false);
    impl_->paused.store(false);
    // A fresh file starts at its beginning, with no seek carried over.
    impl_->seekTarget.store(-1.0);
    impl_->displayPos.store(-1.0);
    impl_->masterClock.store(0.0);
    impl_->seekSerial = 0;
    impl_->appliedSerial = 0;
    impl_->running.store(true);
    impl_->thread = std::thread([this] { impl_->decodeLoop(); });
#ifdef PIXELVR_HAVE_PULSE
    if (impl_->audioEnabled) {
        impl_->audioThread = std::thread([this] { impl_->audioThreadLoop(); });
    }
#endif
    return true;
}

void MediaEngine::close() {
    if (!impl_) {
        return;
    }
    impl_->running.store(false);
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->releaseReq.store(false);  // wake a decode thread in the seek handshake
        impl_->releaseCv.notify_all();
        impl_->queueCv.notify_all();     // wake a back-pressured decoder to exit
    }
#ifdef PIXELVR_HAVE_PULSE
    impl_->audioCv.notify_all();
    if (impl_->audioThread.joinable()) {
        impl_->audioThread.join();
    }
#endif
    impl_->remoteReader.abort();  // unblock a decode thread stalled in remote_read
    if (impl_->thread.joinable()) {
        impl_->thread.join();
    }
    impl_->teardown();
    impl_->audioOnly = false;
    impl_->formatHint = {};
    impl_->chapters.clear();
    impl_->tags = {};
    impl_->cardPending = false;
    impl_->card = {};
    impl_->videoStream = -1;
    impl_->width = impl_->height = 0;
    impl_->duration = 0.0;
    impl_->eof.store(false);
    impl_->paused.store(false);
}

int MediaEngine::width() const { return impl_->width; }
int MediaEngine::height() const { return impl_->height; }
std::string MediaEngine::codecName() const { return impl_->codecName; }
std::string MediaEngine::hardwareBlockedBy() const { return impl_->hwBlockedBy; }
uint64_t MediaEngine::decodedFrames() const {
    return impl_->decodedCount.load(std::memory_order_relaxed);
}
uint64_t MediaEngine::lateFrames() const { return impl_->lateCount.load(std::memory_order_relaxed); }
uint64_t MediaEngine::clockWaits() const { return impl_->clockWaitCount.load(std::memory_order_relaxed); }
double MediaEngine::duration() const { return impl_->duration; }
void MediaEngine::setFrameSequentialMode(int mode) { impl_->frameSeqMode = mode; }
bool MediaEngine::frameSequential() const { return impl_ && impl_->frameSeq; }

double MediaEngine::frameRate() const {
    return impl_ ? (impl_->frameSeq ? impl_->frameRate / 2.0 : impl_->frameRate) : 0.0;  // FS3D: per eye
}
bool MediaEngine::eof() const { return impl_->eof.load(); }

void MediaEngine::setPaused(bool paused) {
    if (impl_) impl_->paused.store(paused);
}

void MediaEngine::togglePause() {
    if (impl_) impl_->paused.store(!impl_->paused.load());
}

void MediaEngine::setMuted(bool muted) {
    impl_->muted.store(muted);
}

void MediaEngine::setVolume(float volume) {
    if (impl_) impl_->volume.store(std::clamp(volume, 0.0f, 1.0f), std::memory_order_relaxed);
}

bool MediaEngine::audioOnly() const { return impl_ && impl_->audioOnly; }
FormatHint MediaEngine::formatHint() const { return impl_->formatHint; }
const ChapterList& MediaEngine::chapters() const { return impl_->chapters; }
const AudioTags& MediaEngine::audioTags() const { return impl_->tags; }

bool MediaEngine::muted() const {
    return impl_->muted.load();
}

bool MediaEngine::paused() const {
    return impl_ && impl_->paused.load();
}

void MediaEngine::seek(double seconds) {
    if (!impl_) return;
    if (impl_->duration > 0.0 && seconds > impl_->duration) {
        seconds = impl_->duration;
    }
    if (seconds < 0.0) seconds = 0.0;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->seekTarget.store(seconds);
        impl_->displayPos.store(seconds);  // report the target until it is shown
        ++impl_->seekSerial;
    }
    impl_->queueCv.notify_all();  // wake a back-pressured decoder to handle the seek
}

void MediaEngine::seekRelative(double delta) {
    seek(position() + delta);  // a pending target, so quick repeats add up
}

double MediaEngine::position() const {
    if (!impl_) return 0.0;
    const double pending = impl_->displayPos.load();
    return pending >= 0.0 ? pending : impl_->masterClock.load();
}

std::vector<MediaEngine::StreamInfo> MediaEngine::videoStreams() const {
    return impl_ ? impl_->videoStreamList : std::vector<StreamInfo>{};
}

std::vector<MediaEngine::StreamInfo> MediaEngine::audioStreams() const {
    return impl_ ? impl_->audioStreamList : std::vector<StreamInfo>{};
}

int MediaEngine::currentVideoStream() const {
    return impl_ ? impl_->videoStream : -1;
}

int MediaEngine::currentAudioStream() const {
    return impl_ ? impl_->audioStream : -1;
}

void MediaEngine::setPreferredVideoStream(int containerIndex) {
    if (impl_) impl_->preferredVideoStream = containerIndex;
}

void MediaEngine::setPreferredAudioStream(int containerIndex) {
    if (impl_) impl_->preferredAudioStream = containerIndex;
}

void MediaEngine::setPreferredAudioLanguage(const std::string& isoLang) {
    if (impl_) impl_->preferredLanguage = isoLang;
}

bool MediaEngine::needsFrameRelease() const {
    return impl_ && impl_->releaseReq.load();
}

void MediaEngine::confirmFrameRelease() {
    if (!impl_) return;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->releaseReq.store(false);
    impl_->releaseCv.notify_all();
}

// Chooses the queued frame to display this vsync: the newest whose PTS is due
// against the master clock. Earlier (late) frames are dropped; later (early)
// frames are held for a future vsync. Returns the index to serve, or npos when
// every frame is still in the future and the screen has already been primed.
static std::size_t selectDueIndex(double clock, bool primed,
                                  const std::function<double(std::size_t)>& ptsAt,
                                  std::size_t count) {
    std::size_t due = 0;
    bool haveDue = false;
    for (std::size_t i = 0; i < count; ++i) {
        if (ptsAt(i) <= clock) {
            due = i;
            haveDue = true;
        } else {
            break;
        }
    }
    if (!haveDue) {
        return primed ? static_cast<std::size_t>(-1) : 0;  // hold, or prime
    }
    return due;
}

bool MediaEngine::latestFrame(VideoFrame& out) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->audioOnly) {  // audio file: the still card, once
        if (!impl_->cardPending) return false;
        impl_->cardPending = false;
        out = impl_->card;
        return true;
    }
    auto& q = impl_->swQueue;
    if (q.empty()) {
        return false;
    }
    double clock = impl_->currentClock();
    // Safety net: re-anchor if the clock has fallen behind the queued frames,
    // so the picture can never freeze "early forever" while decoding continues.
    if (impl_->primed && q.front().pts > clock + 0.25) {
        impl_->playClock.start(q.front().pts);
        clock = q.front().pts;
    }
    // Decoding slower than real time: let the clock wait for the video (a
    // hitch) instead of running away from it. Audio is read in the same loop,
    // so it would fall behind too and be dropped as late, for good.
    if (impl_->primed && q.back().pts < clock - 0.5) {
        impl_->playClock.start(q.front().pts);
        clock = q.front().pts;
        impl_->clockWaitCount.fetch_add(1, std::memory_order_relaxed);
    }
    const std::size_t due = selectDueIndex(
        clock, impl_->primed,
        [&](std::size_t i) { return q[i].pts; }, q.size());
    if (due == static_cast<std::size_t>(-1)) {
        return false;  // all queued frames are still in the future
    }
    impl_->lateCount.fetch_add(due, std::memory_order_relaxed);
    for (std::size_t i = 0; i < due; ++i) {
        q.pop_front();  // drop superseded (late) frames
    }
    const bool firstFrame = !impl_->primed;
    out = std::move(q.front());
    q.pop_front();
    impl_->primed = true;
    impl_->masterClock.store(out.pts, std::memory_order_relaxed);
    if (firstFrame) {
        impl_->playClock.start(out.pts);  // anchor to the first frame
        impl_->settleDisplayLocked();     // the seek's frame is up: report its time
#ifdef PIXELVR_HAVE_PULSE
        impl_->audioCv.notify_all();
#endif
    }
    impl_->queueCv.notify_one();  // slot freed: wake a back-pressured decoder
    return true;
}

bool MediaEngine::drmActive() const {
    return impl_->usingDrm.load();
}

bool MediaEngine::latestDrmFrame(DrmVideoFrame& out) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto& q = impl_->drmQueue;
    if (q.empty()) {
        return false;
    }
    double clock = impl_->currentClock();
    // Safety net: re-anchor if the clock has fallen behind the queued frames,
    // so the picture can never freeze "early forever" while decoding continues.
    if (impl_->primed && q.front().pts > clock + 0.25) {
        PIXELVR_LOG_INFO("MediaEngine: clock resync %.2f -> %.2f", clock,
                         q.front().pts);
        impl_->playClock.start(q.front().pts);
        clock = q.front().pts;
    }
    // Decoding slower than real time: the clock waits for the video (see
    // latestFrame) so audio is not dropped as late for good.
    if (impl_->primed && q.back().pts < clock - 0.5) {
        impl_->playClock.start(q.front().pts);
        clock = q.front().pts;
        impl_->clockWaitCount.fetch_add(1, std::memory_order_relaxed);
    }
    const std::size_t due = selectDueIndex(
        clock, impl_->primed,
        [&](std::size_t i) { return q[i].pts; }, q.size());
    if (due == static_cast<std::size_t>(-1)) {
        return false;  // all queued frames are still in the future
    }
    impl_->lateCount.fetch_add(due, std::memory_order_relaxed);
    for (std::size_t i = 0; i < due; ++i) {
        if (q.front().fd >= 0) ::close(q.front().fd);  // release dropped dma-buf
        q.pop_front();
    }
    if (out.fd >= 0) {
        ::close(out.fd);  // release the caller's previous unconsumed handle
    }
    const bool firstFrame = !impl_->primed;
    out = std::move(q.front());  // transfers fd + keepAlive ownership to caller
    q.pop_front();
    impl_->primed = true;
    impl_->masterClock.store(out.pts, std::memory_order_relaxed);
    if (firstFrame) {
        impl_->playClock.start(out.pts);  // anchor to the first frame
        impl_->settleDisplayLocked();     // the seek's frame is up: report its time
#ifdef PIXELVR_HAVE_PULSE
        impl_->audioCv.notify_all();
#endif
        PIXELVR_LOG_INFO("MediaEngine: prime pts=%.2f clock=%.2f", out.pts, clock);
    }
    impl_->queueCv.notify_one();  // slot freed: wake a back-pressured decoder
    return true;
}

} // namespace pixelvr

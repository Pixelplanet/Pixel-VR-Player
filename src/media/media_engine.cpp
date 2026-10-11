#include "media/media_engine.hpp"

#include "util/logging.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <unistd.h>

extern "C" {
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
#endif

#ifdef PIXELVR_HAVE_SMB
#include <libsmbclient.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <cerrno>
#endif

namespace pixelvr {

#ifdef PIXELVR_HAVE_SMB
namespace {

void smb_auth(SMBCCTX*, const char*, const char*, char*, int, char* un, int unlen,
              char* pw, int pwlen) {
    if (un != nullptr && unlen > 0) un[0] = '\0';  // guest / anonymous
    if (pw != nullptr && pwlen > 0) pw[0] = '\0';
}

// Streams an smb:// URL through libsmbclient behind a FFmpeg AVIOContext. A
// background thread prefetches the file into a ring buffer so the demux read
// callback serves from memory; network jitter then no longer stalls decoding.
// Only the prefetch thread touches the SMB handle (read/lseek); the AVIO
// callbacks only touch the ring and request seeks.
struct SmbReadAhead {
    static constexpr std::size_t kCapacity = 32u << 20;  // 32 MiB cushion
    static constexpr std::size_t kChunk = 256u << 10;    // 256 KiB network reads

    SMBCCTX* ctx = nullptr;
    SMBCFILE* file = nullptr;
    smbc_read_fn readFn = nullptr;
    smbc_lseek_fn lseekFn = nullptr;
    smbc_close_fn closeFn = nullptr;
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

    bool start(SMBCCTX* c, SMBCFILE* f) {
        ctx = c;
        file = f;
        readFn = smbc_getFunctionRead(ctx);
        lseekFn = smbc_getFunctionLseek(ctx);
        closeFn = smbc_getFunctionClose(ctx);
        smbc_fstat_fn fstatFn = smbc_getFunctionFstat(ctx);
        struct stat st{};
        fileSize = (fstatFn != nullptr && fstatFn(ctx, file, &st) == 0)
                       ? static_cast<int64_t>(st.st_size)
                       : -1;
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
                    lseekFn(ctx, file, static_cast<off_t>(pos), SEEK_SET);
                }
                if (eof) continue;
                want = std::min(ring.size() - fill, chunk.size());
            }
            if (want == 0) continue;
            const ssize_t n = readFn(ctx, file, chunk.data(), want);
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
        if (file != nullptr && closeFn != nullptr) closeFn(ctx, file);
        if (ctx != nullptr) smbc_free_context(ctx, 1);
        ctx = nullptr;
        file = nullptr;
        ring.clear();
        ring.shrink_to_fit();
    }
};

int smb_read(void* opaque, uint8_t* buf, int size) {
    return static_cast<SmbReadAhead*>(opaque)->read(buf, size);
}

int64_t smb_seek(void* opaque, int64_t offset, int whence) {
    return static_cast<SmbReadAhead*>(opaque)->seek(offset, whence);
}

}  // namespace
#endif

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

struct MediaEngine::Impl {
    AVFormatContext* format = nullptr;
    AVCodecContext* codec = nullptr;
    SwsContext* sws = nullptr;
#ifdef PIXELVR_HAVE_SMB
    AVIOContext* avio = nullptr;
    SmbReadAhead smbReader;
#endif
    int videoStream = -1;
    int width = 0;
    int height = 0;
    double duration = 0.0;
    double frameRate = 0.0;
    AVRational timeBase{1, 1};
    int drmDiag = 0;
    std::string codecName;
    const AVCodec* videoDecoder = nullptr;  // retained for reopen-on-seek
    bool decoderDrm = false;                // apply drm_prime opts on reopen
    int captureBuffers = 0;                 // num_capture_buffers for reopen
    std::atomic<uint64_t> decodedCount{0};

    int audioStream = -1;
    AVCodecContext* audioCodec = nullptr;
    SwrContext* swr = nullptr;
    AVRational audioTimeBase{1, 1};
    bool audioEnabled = false;
    static constexpr int kOutSampleRate = 48000;
    static constexpr int kOutChannels = 2;
    std::atomic<double> masterClock{0.0};
#ifdef PIXELVR_HAVE_PULSE
    pa_simple* pa = nullptr;
#endif

    std::thread thread;
    std::atomic<bool> running{false};
    std::atomic<bool> eof{false};
    std::atomic<bool> paused{false};
    std::atomic<double> seekTarget{-1.0};  // >= 0 requests a seek (seconds)
    std::atomic<bool> releaseReq{false};   // ask the consumer to free held frames
    std::condition_variable releaseCv;     // consumer signals release complete

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

    ~Impl() { teardown(); }

    void teardown() {
#ifdef PIXELVR_HAVE_PULSE
        if (pa) {
            pa_simple_free(pa);
            pa = nullptr;
        }
#endif
        if (swr) {
            swr_free(&swr);
        }
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
#ifdef PIXELVR_HAVE_SMB
        if (avio != nullptr) {
            av_freep(&avio->buffer);
            avio_context_free(&avio);
        }
        smbReader.shutdown();
#endif
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

    // Playback position in seconds for consumer-side frame selection, in the
    // video PTS timeline (anchored to the first presented frame).
    double currentClock() const {
        return playClock.now();
    }

    void pushFrame(int w, int h, std::vector<uint8_t>&& y, std::vector<uint8_t>&& uv,
                   int colorspace, bool fullRange, double pts) {
        std::lock_guard<std::mutex> lock(mutex);
        while (swQueue.size() >= maxQueue) {
            swQueue.pop_front();
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
        std::lock_guard<std::mutex> lock(mutex);
        while (drmQueue.size() >= maxQueue) {
            if (drmQueue.front().fd >= 0) ::close(drmQueue.front().fd);
            drmQueue.pop_front();
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
        if (decoderDrm) {
            av_opt_set_int(codec->priv_data, "num_capture_buffers", captureBuffers, 0);
            av_opt_set_int(codec->priv_data, "drm_prime", 1, 0);
            codec->apply_cropping = 0;
        }
        return avcodec_open2(codec, videoDecoder, nullptr) >= 0;
    }

    void decodeLoop();
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
    auto lastSeekWall = clock::now() - std::chrono::seconds(1);

    auto process_video = [&]() {
        decodedCount.fetch_add(1, std::memory_order_relaxed);
        const int w = frame->width;
        const int h = frame->height;
        const auto srcFmt = static_cast<AVPixelFormat>(frame->format);
        const int colorspace = static_cast<int>(frame->colorspace);
        const bool fullRange = frame->color_range == AVCOL_RANGE_JPEG;

        int64_t ts = frame->best_effort_timestamp;
        if (ts == AV_NOPTS_VALUE) {
            ts = frame->pts;
        }
        const double pts = (ts == AV_NOPTS_VALUE) ? 0.0 : ts * av_q2d(timeBase);

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

        // With audio, the blocking audio writes pace the loop and provide the
        // master clock; without audio, pace video to its own PTS via wall clock.
        if (!audioEnabled) {
            if (!haveClock) {
                startTime = clock::now();
                startPts = pts;
                haveClock = true;
            } else {
                const auto target =
                    startTime + std::chrono::duration_cast<clock::duration>(
                                    std::chrono::duration<double>(pts - startPts));
                std::this_thread::sleep_until(target);
            }
            masterClock.store(pts);
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
            pushFrame(w, h, std::move(y), std::move(uv), colorspace, fullRange, pts);
        }
    };

#ifdef PIXELVR_HAVE_PULSE
    std::vector<uint8_t> audioBuf;
    auto process_audio = [&]() {
        const int maxOut = swr_get_out_samples(swr, frame->nb_samples);
        if (maxOut <= 0) {
            return;
        }
        audioBuf.resize(static_cast<std::size_t>(maxOut) * kOutChannels * 2);
        uint8_t* out[1] = {audioBuf.data()};
        const int got =
            swr_convert(swr, out, maxOut,
                        const_cast<const uint8_t**>(frame->extended_data),
                        frame->nb_samples);
        if (got <= 0) {
            return;
        }
        const std::size_t bytes = static_cast<std::size_t>(got) * kOutChannels * 2;
        // Mute while actively scrubbing so audio writes don't block video previews
        // and the sound doesn't stutter across seek positions.
        if (std::chrono::duration<double>(clock::now() - lastSeekWall).count() < 0.25) {
            return;
        }
        int err = 0;
        pa_simple_write(pa, audioBuf.data(), bytes, &err);

        int64_t ts = frame->best_effort_timestamp;
        if (ts == AV_NOPTS_VALUE) {
            ts = frame->pts;
        }
        if (ts != AV_NOPTS_VALUE) {
            masterClock.store(ts * av_q2d(audioTimeBase));
        }
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
        double seekTo = seekTarget.exchange(-1.0);
        // Throttle seek handling to ~8/s. Rapid scrubbing otherwise flushes the
        // hardware decoder faster than it can emit a frame, wedging it so the
        // picture freezes; deferred seeks are re-stored so the latest still wins.
        if (seekTo >= 0.0) {
            const double since =
                std::chrono::duration<double>(clock::now() - lastSeekWall).count();
            if (since < 0.12) {
                double expected = -1.0;
                seekTarget.compare_exchange_strong(expected, seekTo);
                seekTo = -1.0;
            }
        }
        if (seekTo >= 0.0) {
            lastSeekWall = clock::now();
            // Release every held dma-buf (queued frames + the renderer's live
            // frame) before flushing; the V4L2 decoder cannot reclaim its capture
            // buffers otherwise and wedges ("can't request capture buffers").
            {
                std::unique_lock<std::mutex> lock(mutex);
                clearQueues();  // drop stale pre-seek frames
                releaseReq.store(true);
                releaseCv.wait_for(lock, std::chrono::milliseconds(150), [&] {
                    return !releaseReq.load() || !running.load();
                });
            }
            const int64_t ts = static_cast<int64_t>(seekTo * AV_TIME_BASE);
            avformat_seek_file(format, -1, INT64_MIN, ts, INT64_MAX, 0);
            const bool reopened = reopenDecoder();  // fresh decoder avoids HW wedge
            if (!reopened) {
                PIXELVR_LOG_ERROR("MediaEngine: video decoder reopen failed on seek");
            }
            PIXELVR_LOG_INFO("MediaEngine: seek %.2fs reopen=%d", seekTo, reopened);
            drmDiag = 0;  // re-log the first frames so seek recovery is visible
            if (audioCodec != nullptr) {
                avcodec_flush_buffers(audioCodec);
            }
            haveClock = false;
            masterClock.store(seekTo);
            playClock.reset();  // the next presented frame re-anchors the clock
            eofFlushed = false;
            eof.store(false);
        }
        if (paused.load()) {
            haveClock = false;  // re-establish the wall clock on resume
            playClock.freeze();  // hold the clock so video doesn't advance
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        playClock.resume();  // no-op unless resuming from pause
        const int readResult = av_read_frame(format, packet);
        if (readResult < 0) {
            if (!eofFlushed) {
                avcodec_send_packet(codec, nullptr);  // flush video
                drain(codec, true);
#ifdef PIXELVR_HAVE_PULSE
                if (audioEnabled) {
                    avcodec_send_packet(audioCodec, nullptr);
                    drain(audioCodec, false);
                }
#endif
                eofFlushed = true;
                eof.store(true);
            }
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

MediaEngine::MediaEngine() : impl_(std::make_unique<Impl>()) {
    avformat_network_init();
}

MediaEngine::~MediaEngine() {
    close();
}

bool MediaEngine::open(const std::string& path) {
    close();

#ifdef PIXELVR_HAVE_SMB
    if (path.rfind("smb://", 0) == 0) {
        SMBCCTX* ctx = smbc_new_context();
        if (ctx != nullptr) {
            smbc_setFunctionAuthDataWithContext(ctx, &smb_auth);
            if (smbc_init_context(ctx) == nullptr) {
                smbc_free_context(ctx, 1);
                ctx = nullptr;
            }
        }
        smbc_open_fn openFn = (ctx != nullptr) ? smbc_getFunctionOpen(ctx) : nullptr;
        SMBCFILE* file =
            (openFn != nullptr) ? openFn(ctx, path.c_str(), O_RDONLY, 0) : nullptr;
        if (file == nullptr) {
            if (ctx != nullptr) smbc_free_context(ctx, 1);
            PIXELVR_LOG_ERROR("MediaEngine: cannot open SMB stream %s", path.c_str());
            return false;
        }
        impl_->smbReader.start(ctx, file);

        constexpr int kBuf = 1 << 16;  // 64 KiB AVIO buffer (ring is separate)
        auto* buffer = static_cast<unsigned char*>(av_malloc(kBuf));
        impl_->avio = (buffer != nullptr)
                          ? avio_alloc_context(buffer, kBuf, 0, &impl_->smbReader,
                                               &smb_read, nullptr, &smb_seek)
                          : nullptr;
        if (impl_->avio == nullptr) {
            av_free(buffer);
            close();
            PIXELVR_LOG_ERROR("MediaEngine: SMB AVIO alloc failed");
            return false;
        }
        impl_->format = avformat_alloc_context();
        if (impl_->format == nullptr) {
            close();
            return false;
        }
        impl_->format->pb = impl_->avio;
        impl_->format->flags |= AVFMT_FLAG_CUSTOM_IO;
        if (avformat_open_input(&impl_->format, path.c_str(), nullptr, nullptr) < 0) {
            PIXELVR_LOG_ERROR("MediaEngine: cannot open SMB stream %s", path.c_str());
            close();
            return false;
        }
    } else
#endif
    if (avformat_open_input(&impl_->format, path.c_str(), nullptr, nullptr) < 0) {
        PIXELVR_LOG_ERROR("MediaEngine: cannot open %s", path.c_str());
        return false;
    }
    if (avformat_find_stream_info(impl_->format, nullptr) < 0) {
        PIXELVR_LOG_ERROR("MediaEngine: no stream info in %s", path.c_str());
        close();
        return false;
    }

    impl_->videoStream =
        av_find_best_stream(impl_->format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (impl_->videoStream < 0) {
        PIXELVR_LOG_ERROR("MediaEngine: no video stream in %s", path.c_str());
        close();
        return false;
    }

    AVStream* stream = impl_->format->streams[impl_->videoStream];
    impl_->timeBase = stream->time_base;
    impl_->frameRate = av_q2d(av_guess_frame_rate(impl_->format, stream, nullptr));
    PIXELVR_LOG_INFO("MediaEngine: source frame rate %.3f fps", impl_->frameRate);
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
        decoder = avcodec_find_decoder(codecId);
    }
    if (decoder == nullptr) {
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
        impl_->videoDecoder = dec;  // retained so the decode thread can reopen on seek
        impl_->decoderDrm = drmPrime && std::strstr(dec->name, "v4l2m2m") != nullptr;
        if (impl_->decoderDrm) {
            // Export decoder output as DMA-BUF (AV_PIX_FMT_DRM_PRIME) for zero-copy
            // Vulkan import; requires the dmabuf-patched libavcodec. Bound the
            // decoded-buffer memory: at 8K a frame is ~40-80 MiB, so a fixed large
            // count exhausts the decoder's DMA pool and REQBUFS fails -> POLLERR.
            const int64_t px = static_cast<int64_t>(stream->codecpar->width) *
                               stream->codecpar->height;
            const int64_t frameBytes = highBitDepth ? px * 3 : px * 3 / 2;  // P010/NV12
            constexpr int64_t kBudget = 320ll << 20;  // ~320 MiB capture pool
            impl_->captureBuffers = static_cast<int>(std::clamp<int64_t>(
                kBudget / std::max<int64_t>(frameBytes, 1), 4, 16));
            av_opt_set_int(impl_->codec->priv_data, "num_capture_buffers",
                           impl_->captureBuffers, 0);
            av_opt_set_int(impl_->codec->priv_data, "drm_prime", 1, 0);
            impl_->codec->apply_cropping = 0;
            // Leave buffers free for the decoder's DPB; the queue holds the rest.
            impl_->maxQueue =
                static_cast<std::size_t>(std::clamp(impl_->captureBuffers - 5, 1, 6));
        }
        return avcodec_open2(impl_->codec, dec, nullptr) >= 0;
    };

    if (!open_with(decoder)) {
        if (hardware) {
            PIXELVR_LOG_WARN("MediaEngine: hardware decoder failed, trying software");
            hardware = false;
            decoder = avcodec_find_decoder(codecId);
            if (decoder == nullptr || !open_with(decoder)) {
                PIXELVR_LOG_ERROR("MediaEngine: failed to open decoder for %s",
                                  path.c_str());
                close();
                return false;
            }
        } else {
            PIXELVR_LOG_ERROR("MediaEngine: failed to open decoder for %s",
                              path.c_str());
            close();
            return false;
        }
    }

    impl_->width = impl_->codec->width;
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

#ifdef PIXELVR_HAVE_PULSE
    const AVCodec* adec = nullptr;
    impl_->audioStream =
        av_find_best_stream(impl_->format, AVMEDIA_TYPE_AUDIO, -1, -1, &adec, 0);
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
                pa_sample_spec spec;
                spec.format = PA_SAMPLE_S16LE;
                spec.rate = MediaEngine::Impl::kOutSampleRate;
                spec.channels = MediaEngine::Impl::kOutChannels;
                impl_->pa =
                    pa_simple_new(nullptr, "PixelVRPlayer", PA_STREAM_PLAYBACK,
                                  nullptr, "video", &spec, nullptr, nullptr, nullptr);
                impl_->audioEnabled = (impl_->pa != nullptr);
            }
            av_channel_layout_uninit(&outLayout);
        }
        if (impl_->audioEnabled) {
            PIXELVR_LOG_INFO("MediaEngine: audio %d Hz stereo",
                             MediaEngine::Impl::kOutSampleRate);
        } else {
            PIXELVR_LOG_WARN("MediaEngine: audio unavailable, video-only playback");
        }
    }
#endif

    impl_->eof.store(false);
    impl_->running.store(true);
    impl_->thread = std::thread([this] { impl_->decodeLoop(); });
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
    }
#ifdef PIXELVR_HAVE_SMB
    impl_->smbReader.abort();  // unblock a decode thread stalled in smb_read
#endif
    if (impl_->thread.joinable()) {
        impl_->thread.join();
    }
    impl_->teardown();
    impl_->videoStream = -1;
    impl_->width = impl_->height = 0;
    impl_->duration = 0.0;
    impl_->eof.store(false);
}

int MediaEngine::width() const { return impl_->width; }
int MediaEngine::height() const { return impl_->height; }
std::string MediaEngine::codecName() const { return impl_->codecName; }
uint64_t MediaEngine::decodedFrames() const {
    return impl_->decodedCount.load(std::memory_order_relaxed);
}
double MediaEngine::duration() const { return impl_->duration; }
double MediaEngine::frameRate() const { return impl_ ? impl_->frameRate : 0.0; }
bool MediaEngine::eof() const { return impl_->eof.load(); }

void MediaEngine::setPaused(bool paused) {
    if (impl_) impl_->paused.store(paused);
}

void MediaEngine::togglePause() {
    if (impl_) impl_->paused.store(!impl_->paused.load());
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
    impl_->seekTarget.store(seconds);
}

void MediaEngine::seekRelative(double delta) {
    seek(position() + delta);
}

double MediaEngine::position() const {
    return impl_ ? impl_->masterClock.load() : 0.0;
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
    auto& q = impl_->swQueue;
    if (q.empty()) {
        return false;
    }
    const std::size_t due = selectDueIndex(
        impl_->currentClock(), impl_->primed,
        [&](std::size_t i) { return q[i].pts; }, q.size());
    if (due == static_cast<std::size_t>(-1)) {
        return false;  // all queued frames are still in the future
    }
    for (std::size_t i = 0; i < due; ++i) {
        q.pop_front();  // drop superseded (late) frames
    }
    const bool firstFrame = !impl_->primed;
    out = std::move(q.front());
    q.pop_front();
    impl_->primed = true;
    if (firstFrame) impl_->playClock.start(out.pts);  // anchor to the first frame
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
    const std::size_t due = selectDueIndex(
        impl_->currentClock(), impl_->primed,
        [&](std::size_t i) { return q[i].pts; }, q.size());
    if (due == static_cast<std::size_t>(-1)) {
        return false;  // all queued frames are still in the future
    }
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
    if (firstFrame) impl_->playClock.start(out.pts);  // anchor to the first frame
    return true;
}

} // namespace pixelvr

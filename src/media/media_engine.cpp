#include "media/media_engine.hpp"

#include "util/logging.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
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

// Streams an smb:// URL through libsmbclient behind a FFmpeg AVIOContext so the
// media engine can demux network files on demand, without downloading first.
struct SmbIO {
    SMBCCTX* ctx = nullptr;
    SMBCFILE* file = nullptr;
    smbc_read_fn readFn = nullptr;
    smbc_lseek_fn lseekFn = nullptr;
    smbc_fstat_fn fstatFn = nullptr;
    smbc_close_fn closeFn = nullptr;
};

void smb_auth(SMBCCTX*, const char*, const char*, char*, int, char* un, int unlen,
              char* pw, int pwlen) {
    if (un != nullptr && unlen > 0) un[0] = '\0';  // guest / anonymous
    if (pw != nullptr && pwlen > 0) pw[0] = '\0';
}

int smb_read(void* opaque, uint8_t* buf, int size) {
    auto* io = static_cast<SmbIO*>(opaque);
    const ssize_t n = io->readFn(io->ctx, io->file, buf, static_cast<size_t>(size));
    if (n < 0) return AVERROR(errno);
    if (n == 0) return AVERROR_EOF;
    return static_cast<int>(n);
}

int64_t smb_seek(void* opaque, int64_t offset, int whence) {
    auto* io = static_cast<SmbIO*>(opaque);
    if ((whence & ~AVSEEK_FORCE) == AVSEEK_SIZE) {
        struct stat st{};
        if (io->fstatFn(io->ctx, io->file, &st) < 0) return AVERROR(errno);
        return static_cast<int64_t>(st.st_size);
    }
    const off_t r = io->lseekFn(io->ctx, io->file, static_cast<off_t>(offset),
                                whence & ~AVSEEK_FORCE);
    if (r < 0) return AVERROR(errno);
    return static_cast<int64_t>(r);
}

}  // namespace
#endif

struct MediaEngine::Impl {
    AVFormatContext* format = nullptr;
    AVCodecContext* codec = nullptr;
    SwsContext* sws = nullptr;
#ifdef PIXELVR_HAVE_SMB
    AVIOContext* avio = nullptr;
    SmbIO smbIO;
#endif
    int videoStream = -1;
    int width = 0;
    int height = 0;
    double duration = 0.0;
    AVRational timeBase{1, 1};
    int drmDiag = 0;
    std::string codecName;
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

    std::mutex mutex;
    VideoFrame published;        // guarded by mutex
    DrmVideoFrame publishedDrm;  // guarded by mutex
    uint64_t serial = 0;         // guarded by mutex
    std::atomic<bool> usingDrm{false};

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
        if (smbIO.file != nullptr && smbIO.closeFn != nullptr) {
            smbIO.closeFn(smbIO.ctx, smbIO.file);
        }
        if (smbIO.ctx != nullptr) {
            smbc_free_context(smbIO.ctx, 1);
        }
        smbIO = SmbIO{};
#endif
        if (publishedDrm.fd >= 0) {
            ::close(publishedDrm.fd);
            publishedDrm.fd = -1;
        }
        usingDrm.store(false);
        audioEnabled = false;
        audioStream = -1;
    }

    void publish(int w, int h, std::vector<uint8_t>&& y, std::vector<uint8_t>&& uv,
                 int colorspace, bool fullRange, double pts) {
        std::lock_guard<std::mutex> lock(mutex);
        published.width = w;
        published.height = h;
        published.y = std::move(y);
        published.uv = std::move(uv);
        published.colorspace = colorspace;
        published.fullRange = fullRange;
        published.pts = pts;
        published.serial = ++serial;
    }

    void publishDrm(const DrmVideoFrame& f) {
        std::lock_guard<std::mutex> lock(mutex);
        if (publishedDrm.fd >= 0) {
            ::close(publishedDrm.fd);
        }
        publishedDrm = f;
        publishedDrm.serial = ++serial;
        usingDrm.store(true);
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
            publishDrm(drm);
        } else {
            publish(w, h, std::move(y), std::move(uv), colorspace, fullRange, pts);
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
        const double seekTo = seekTarget.exchange(-1.0);
        if (seekTo >= 0.0) {
            const int64_t ts = static_cast<int64_t>(seekTo * AV_TIME_BASE);
            avformat_seek_file(format, -1, INT64_MIN, ts, INT64_MAX, 0);
            avcodec_flush_buffers(codec);
            if (audioCodec != nullptr) {
                avcodec_flush_buffers(audioCodec);
            }
            haveClock = false;
            masterClock.store(seekTo);
            eof.store(false);
        }
        if (paused.load()) {
            haveClock = false;  // re-establish the wall clock on resume
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        const int readResult = av_read_frame(format, packet);
        if (readResult < 0) {
            avcodec_send_packet(codec, nullptr);  // flush video
            drain(codec, true);
#ifdef PIXELVR_HAVE_PULSE
            if (audioEnabled) {
                avcodec_send_packet(audioCodec, nullptr);
                drain(audioCodec, false);
            }
#endif
            eof.store(true);
            break;
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
        impl_->smbIO.ctx = ctx;
        impl_->smbIO.file = file;
        impl_->smbIO.readFn = smbc_getFunctionRead(ctx);
        impl_->smbIO.lseekFn = smbc_getFunctionLseek(ctx);
        impl_->smbIO.fstatFn = smbc_getFunctionFstat(ctx);
        impl_->smbIO.closeFn = smbc_getFunctionClose(ctx);

        constexpr int kBuf = 1 << 16;  // 64 KiB streaming buffer
        auto* buffer = static_cast<unsigned char*>(av_malloc(kBuf));
        impl_->avio = (buffer != nullptr)
                          ? avio_alloc_context(buffer, kBuf, 0, &impl_->smbIO,
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
        if (drmPrime && std::strstr(dec->name, "v4l2m2m") != nullptr) {
            // Export decoder output as DMA-BUF (AV_PIX_FMT_DRM_PRIME) for zero-copy
            // Vulkan import; requires the dmabuf-patched libavcodec. Extra capture
            // buffers give the decoder headroom while the renderer holds frames.
            av_opt_set_int(impl_->codec->priv_data, "num_capture_buffers", 16, 0);
            av_opt_set_int(impl_->codec->priv_data, "drm_prime", 1, 0);
            impl_->codec->apply_cropping = 0;
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

bool MediaEngine::latestFrame(VideoFrame& out) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->serial == 0 || impl_->published.serial == out.serial) {
        return false;
    }
    out = impl_->published;
    return true;
}

bool MediaEngine::drmActive() const {
    return impl_->usingDrm.load();
}

bool MediaEngine::latestDrmFrame(DrmVideoFrame& out) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->serial == 0 || impl_->publishedDrm.serial == out.serial ||
        impl_->publishedDrm.fd < 0) {
        return false;
    }
    if (out.fd >= 0) {
        ::close(out.fd);  // release the caller's previous unconsumed handle
    }
    out = impl_->publishedDrm;
    impl_->publishedDrm.fd = -1;  // fd ownership transferred to the caller
    impl_->publishedDrm.keepAlive.reset();  // frame ref transferred to the caller
    return true;
}

} // namespace pixelvr

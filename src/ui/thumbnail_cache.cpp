#include "ui/thumbnail_cache.hpp"
#include "app/vault_manager.hpp"
#include "util/logging.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

#ifdef PIXELVR_HAVE_MEDIA
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}
#endif

namespace fs = std::filesystem;

namespace pixelvr {

constexpr int kThumbW = 256;
constexpr int kThumbH = 144;
constexpr std::size_t kThumbBytes = kThumbW * kThumbH * 4;

struct DiskMetaHeader {
    uint32_t magic = 0x5056524D; // 'PVRM'
    int32_t width = 0;
    int32_t height = 0;
    double fps = 0.0;
    double durationSec = 0.0;
};

static uint64_t fnv1a64(const std::string& str) {
    uint64_t hash = 14695981039346656037ULL;
    for (char c : str) {
        hash ^= static_cast<uint8_t>(c);
        hash *= 1099511628211ULL;
    }
    return hash;
}

ThumbnailCache::ThumbnailCache() {
    const char* home = std::getenv("HOME");
    std::string base = (home != nullptr) ? home : "/home/steamos";
    cacheDir_ = base + "/.cache/pixelvr/thumbs";
    std::error_code ec;
    fs::create_directories(cacheDir_, ec);

    worker_ = std::thread([this] { workerLoop(); });
}

ThumbnailCache::~ThumbnailCache() {
    stop_.store(true);
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void ThumbnailCache::request(const std::string& videoPath, bool highPriority) {
    if (videoPath.empty() || videoPath[0] != '/') return;
    std::lock_guard<std::mutex> lk(mutex_);
    if (cache_.find(videoPath) != cache_.end()) {
        return;
    }
    if (highPriority) {
        // If already queued, move it to the front so visible items decode immediately
        auto it = std::find(queue_.begin(), queue_.end(), videoPath);
        if (it != queue_.end()) {
            queue_.erase(it);
        }
        queuedPaths_[videoPath] = true;
        queue_.push_front(videoPath);
        cv_.notify_one();
        return;
    }
    if (queuedPaths_.find(videoPath) != queuedPaths_.end()) {
        return;
    }
    queuedPaths_[videoPath] = true;
    queue_.push_back(videoPath);
    cv_.notify_one();
}

bool ThumbnailCache::get(const std::string& videoPath, std::vector<uint8_t>& outRgba) {
    if (videoPath.empty()) return false;
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = cache_.find(videoPath);
    if (it != cache_.end() && it->second.size() == kThumbBytes) {
        outRgba = it->second;
        return true;
    }
    return false;
}

bool ThumbnailCache::getMeta(const std::string& videoPath, MediaMeta& outMeta) {
    if (videoPath.empty()) return false;
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = metaCache_.find(videoPath);
    if (it != metaCache_.end() && it->second.ready) {
        outMeta = it->second;
        return true;
    }
    return false;
}

void ThumbnailCache::clearVaultThumbs(const std::string& vaultDir) {
    if (vaultDir.empty()) return;
    std::lock_guard<std::mutex> lk(mutex_);
    for (auto it = cache_.begin(); it != cache_.end(); ) {
        if (it->first.rfind(vaultDir, 0) == 0) {
            it = cache_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = metaCache_.begin(); it != metaCache_.end(); ) {
        if (it->first.rfind(vaultDir, 0) == 0) {
            it = metaCache_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = queuedPaths_.begin(); it != queuedPaths_.end(); ) {
        if (it->first.rfind(vaultDir, 0) == 0) {
            it = queuedPaths_.erase(it);
        } else {
            ++it;
        }
    }
}

bool ThumbnailCache::extractThumbnailAndMeta(const std::string& videoPath,
                                             std::vector<uint8_t>& outRgba,
                                             MediaMeta& outMeta) {
#ifdef PIXELVR_HAVE_MEDIA
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, videoPath.c_str(), nullptr, nullptr) < 0 || !fmt) {
        return false;
    }
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        avformat_close_input(&fmt);
        return false;
    }

    int vIdx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vIdx < 0) {
        avformat_close_input(&fmt);
        return false;
    }

    AVStream* st = fmt->streams[vIdx];
    outMeta.width = st->codecpar->width;
    outMeta.height = st->codecpar->height;
    if (st->avg_frame_rate.den > 0 && st->avg_frame_rate.num > 0) {
        outMeta.fps = av_q2d(st->avg_frame_rate);
    } else if (st->r_frame_rate.den > 0 && st->r_frame_rate.num > 0) {
        outMeta.fps = av_q2d(st->r_frame_rate);
    }
    if (fmt->duration > 0) {
        outMeta.durationSec = static_cast<double>(fmt->duration) / AV_TIME_BASE;
    } else if (st->duration > 0 && st->time_base.den > 0) {
        outMeta.durationSec = static_cast<double>(st->duration) * av_q2d(st->time_base);
    }
    outMeta.ready = true;

    // Find standard software decoder for thumbnail extraction so we never touch V4L2/DRM hardware slots
    const AVCodec* dec = avcodec_find_decoder(st->codecpar->codec_id);
    if (dec && dec->name && std::strstr(dec->name, "v4l2") != nullptr) {
        if (st->codecpar->codec_id == AV_CODEC_ID_HEVC) {
            if (const AVCodec* sw = avcodec_find_decoder_by_name("hevc")) dec = sw;
        } else if (st->codecpar->codec_id == AV_CODEC_ID_H264) {
            if (const AVCodec* sw = avcodec_find_decoder_by_name("h264")) dec = sw;
        }
    }
    if (!dec) {
        avformat_close_input(&fmt);
        return false;
    }

    AVCodecContext* ctx = avcodec_alloc_context3(dec);
    if (!ctx) {
        avformat_close_input(&fmt);
        return false;
    }
    avcodec_parameters_to_context(ctx, st->codecpar);
    ctx->thread_count = 2;
    ctx->skip_loop_filter = AVDISCARD_ALL;
    if (avcodec_open2(ctx, dec, nullptr) < 0) {
        avcodec_free_context(&ctx);
        avformat_close_input(&fmt);
        return false;
    }

    // Seek ~10% into the clip (clamped between 3s and 30s) to avoid black intros
    if (outMeta.durationSec > 6.0) {
        double targetSec = std::clamp(outMeta.durationSec * 0.10, 3.0, 30.0);
        int64_t ts = static_cast<int64_t>(targetSec * AV_TIME_BASE);
        av_seek_frame(fmt, -1, ts, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(ctx);
    }

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    bool gotFrame = false;

    auto decodeAttempt = [&]() {
        int packetsRead = 0;
        while (packetsRead < 300 && av_read_frame(fmt, pkt) >= 0) {
            if (pkt->stream_index == vIdx) {
                ++packetsRead;
                if (avcodec_send_packet(ctx, pkt) == 0) {
                    if (avcodec_receive_frame(ctx, frame) == 0) {
                        gotFrame = true;
                        av_packet_unref(pkt);
                        break;
                    }
                }
            }
            av_packet_unref(pkt);
        }
    };

    decodeAttempt();
    if (!gotFrame && outMeta.durationSec > 6.0) {
        // Fallback to start of file if seek landed near EOF
        av_seek_frame(fmt, -1, 0, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(ctx);
        decodeAttempt();
    }

    bool ok = false;
    if (gotFrame && frame->width > 0 && frame->height > 0) {
        // For Side-by-Side (SBS) 3D or VR180 videos (aspect >= 1.95 or width == 2*height),
        // crop the left eye (left half of frame) so the thumbnail shows a normal single view
        // instead of a squished double-eye image!
        int srcW = frame->width;
        int srcH = frame->height;
        const float ar = static_cast<float>(srcW) / static_cast<float>(srcH);
        std::string lowerPath = videoPath;
        for (char& c : lowerPath) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const bool isSbs = (lowerPath.find("sbs") != std::string::npos ||
                            lowerPath.find("vr180") != std::string::npos ||
                            lowerPath.find("180") != std::string::npos ||
                            ar >= 2.6f);
        const bool isTb = (lowerPath.find("tb") != std::string::npos ||
                           lowerPath.find("ou") != std::string::npos ||
                           lowerPath.find("overunder") != std::string::npos ||
                           lowerPath.find("htab") != std::string::npos);
        if (isSbs && srcW >= 2 * kThumbW) {
            srcW = frame->width / 2;
        } else if (isTb && srcH >= 2 * kThumbH) {
            srcH = frame->height / 2;
        }

        SwsContext* sws = sws_getContext(
            srcW, srcH, static_cast<AVPixelFormat>(frame->format),
            kThumbW, kThumbH, AV_PIX_FMT_RGBA,
            SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (sws) {
            outRgba.resize(kThumbBytes);
            uint8_t* dstData[4] = {outRgba.data(), nullptr, nullptr, nullptr};
            int dstLinesize[4] = {kThumbW * 4, 0, 0, 0};
            sws_scale(sws, frame->data, frame->linesize, 0, srcH, dstData, dstLinesize);
            sws_freeContext(sws);
            ok = true;
        }
    }

    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    avformat_close_input(&fmt);
    return ok;
#else
    (void)videoPath;
    (void)outRgba;
    (void)outMeta;
    return false;
#endif
}

void ThumbnailCache::workerLoop() {
    while (!stop_.load()) {
        std::string path;
        {
            std::unique_lock<std::mutex> lk(mutex_);
            cv_.wait(lk, [this] { return stop_.load() || !queue_.empty(); });
            if (stop_.load()) break;
            path = queue_.front();
            queue_.pop_front();
            queuedPaths_.erase(path);
        }

        if (path.empty() || path[0] != '/') {
            continue;
        }

        const bool inVault = VaultManager::instance().isVaultPath(path);
        const std::string targetCacheDir = inVault ? VaultManager::instance().thumbsDir() : cacheDir_;
        const std::string baseHash = std::to_string(fnv1a64(path));
        const std::string cacheFile = targetCacheDir + "/" + baseHash + ".rgba";
        const std::string metaFile = targetCacheDir + "/" + baseHash + ".meta";

        std::error_code ec;
        if (fs::is_regular_file(cacheFile, ec) && fs::file_size(cacheFile, ec) == kThumbBytes) {
            std::ifstream in(cacheFile, std::ios::binary);
            std::vector<uint8_t> data(kThumbBytes);
            if (in.read(reinterpret_cast<char*>(data.data()), kThumbBytes)) {
                MediaMeta meta{};
                if (fs::is_regular_file(metaFile, ec) &&
                    fs::file_size(metaFile, ec) == sizeof(DiskMetaHeader)) {
                    std::ifstream min(metaFile, std::ios::binary);
                    DiskMetaHeader hdr{};
                    if (min.read(reinterpret_cast<char*>(&hdr), sizeof(hdr)) &&
                        hdr.magic == 0x5056524D) {
                        meta.width = hdr.width;
                        meta.height = hdr.height;
                        meta.fps = hdr.fps;
                        meta.durationSec = hdr.durationSec;
                        meta.ready = true;
                    }
                }
                if (!meta.ready) {
                    // Quick metadata probe if .meta was not cached yet
                    std::vector<uint8_t> dummy;
                    extractThumbnailAndMeta(path, dummy, meta);
                    if (meta.ready) {
                        DiskMetaHeader hdr{0x5056524D, meta.width, meta.height, meta.fps, meta.durationSec};
                        std::ofstream mout(metaFile, std::ios::binary);
                        mout.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
                    }
                }
                std::lock_guard<std::mutex> lk(mutex_);
                cache_[path] = std::move(data);
                if (meta.ready) metaCache_[path] = meta;
                continue;
            }
        }

        std::vector<uint8_t> rgba;
        MediaMeta meta{};
        const bool ok = extractThumbnailAndMeta(path, rgba, meta);
        if (meta.ready) {
            DiskMetaHeader hdr{0x5056524D, meta.width, meta.height, meta.fps, meta.durationSec};
            std::ofstream mout(metaFile, std::ios::binary);
            mout.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
            std::lock_guard<std::mutex> lk(mutex_);
            metaCache_[path] = meta;
        }
        if (ok && rgba.size() == kThumbBytes) {
            {
                std::ofstream out(cacheFile, std::ios::binary);
                out.write(reinterpret_cast<const char*>(rgba.data()), kThumbBytes);
            }
            std::lock_guard<std::mutex> lk(mutex_);
            cache_[path] = std::move(rgba);
            PIXELVR_LOG_INFO("Thumbnail generated for %s (%dx%d, %.0fs)",
                             path.c_str(), meta.width, meta.height, meta.durationSec);
        } else {
            PIXELVR_LOG_WARN("Thumbnail generation failed for %s", path.c_str());
        }
    }
}

} // namespace pixelvr

#include "ui/thumbnail_cache.hpp"
#include "media/vault_avio.hpp"
#include "app/vault_crypto.hpp"
#include "app/vault_manager.hpp"
#include "util/logging.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

#ifdef PIXELVR_HAVE_MEDIA
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}
#include "media/cover_art.hpp"
#include "media/stereo_detect.hpp"
#endif

namespace fs = std::filesystem;

namespace pixelvr {

constexpr int kThumbW = ThumbnailCache::kWidth;
constexpr int kThumbH = ThumbnailCache::kHeight;
// Thumbnails kept in memory (~330 KB each); older ones reload from the disk cache.
constexpr std::size_t kMaxCached = 300;
constexpr std::size_t kThumbBytes = kThumbW * kThumbH * 4;

struct DiskMetaHeader {
    uint32_t magic = 0x5056524D; // 'PVRM'
    int32_t width = 0;
    int32_t height = 0;
    double fps = 0.0;
    double durationSec = 0.0;
};

// .meta files: the header ('PVR2': followed by the format hints), then the
// source file's path. Older files: 'PVRM' header, then the path or nothing.
constexpr uint32_t kMetaMagicV1 = 0x5056524D;  // 'PVRM'
constexpr uint32_t kMetaMagicV2 = 0x32525650;  // 'PVR2'
struct DiskMetaHints {
    int32_t projection = -1;
    int32_t stereo = -1;
};
static void writeMeta(const std::string& metaFile, const ThumbnailCache::MediaMeta& meta, const std::string& sourcePath) {
    const DiskMetaHeader hdr{kMetaMagicV2, meta.width, meta.height, meta.fps, meta.durationSec};
    const DiskMetaHints hints{meta.projectionHint, meta.stereoHint};
    std::ofstream out(metaFile, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    out.write(reinterpret_cast<const char*>(&hints), sizeof(hints));
    out.write(sourcePath.data(), static_cast<std::streamsize>(sourcePath.size()));
}

// Removes thumbnails nobody needs any more: ones whose source file is gone,
// and ones not shown for a long time (unknown source, network shares).
// Local path whose file is known to be gone. Files on removable drives
// (/run/media/<user>/<drive>/...) only count while that drive is mounted.
static bool sourceDeleted(const std::string& path) {
    std::error_code ec;
    if (path.rfind("/run/media/", 0) == 0 || path.rfind("/media/", 0) == 0) {
        const int depth = path.rfind("/run/media/", 0) == 0 ? 4 : 2;  // slashes after the leading one, up to the drive folder
        size_t pos = 0;
        for (int i = 0; i < depth && pos != std::string::npos; ++i) pos = path.find('/', pos + 1);
        if (pos == std::string::npos || !fs::exists(path.substr(0, pos), ec)) return false;
    }
    return !fs::exists(path, ec);
}

static void pruneThumbnails(const std::string& dir) {
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec)) return;
    using namespace std::chrono_literals;
    const auto now = fs::file_time_type::clock::now();
    constexpr auto kUnknownMaxAge = 30 * 24h;   // no recorded source
    constexpr auto kUnusedMaxAge = 180 * 24h;   // e.g. SMB paths we can't check cheaply
    int removed = 0;
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(dir, ec)) files.push_back(e.path());
    for (const fs::path& p : files) {
        if (p.extension() != ".rgba" && p.extension() != ".meta") continue;
        const fs::path rgba = fs::path(p).replace_extension(".rgba");
        const fs::path meta = fs::path(p).replace_extension(".meta");
        if (p.extension() == ".meta" && fs::exists(rgba, ec)) continue;  // handled with its .rgba
        std::error_code te;
        const fs::path stamp = fs::exists(rgba, te) ? rgba : meta;
        const auto age = now - fs::last_write_time(stamp, te);
        std::string source;
        if (fs::file_size(meta, te) > sizeof(DiskMetaHeader)) {
            std::ifstream in(meta, std::ios::binary);
            uint32_t magic = 0;
            in.read(reinterpret_cast<char*>(&magic), sizeof(magic));
            in.seekg(sizeof(DiskMetaHeader) + (magic == kMetaMagicV2 ? sizeof(DiskMetaHints) : 0));
            source.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        bool drop;
        if (!source.empty() && source[0] == '/') drop = sourceDeleted(source) || age > kUnusedMaxAge;
        else if (!source.empty()) drop = age > kUnusedMaxAge;
        else drop = age > kUnknownMaxAge;
        if (drop) {
            fs::remove(rgba, te);
            fs::remove(meta, te);
            ++removed;
        }
    }
    if (removed > 0) PIXELVR_LOG_INFO("Thumbnails: removed %d unused from %s", removed, dir.c_str());
}

// Size and modification time in one number: tells whether a file changed.
static long long fileStamp(const std::string& path) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    const auto mtime = fs::last_write_time(path, ec).time_since_epoch().count();
    return ec ? -1 : static_cast<long long>(size) * 1000003LL ^ static_cast<long long>(mtime);
}

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
    if (const auto f = failed_.find(videoPath); f != failed_.end()) {
        if (f->second == fileStamp(videoPath)) return;  // unchanged: it would fail again
        failed_.erase(f);  // changed since (e.g. its download finished): try again
    }
    // Background requests only fill in what is unknown: with the in-memory cap,
    // re-loading evicted pixels for every listed file would churn forever.
    // Visible cards ask with high priority and get their pixels back.
    if (!highPriority) {
        if (const auto m = metaCache_.find(videoPath); m != metaCache_.end() && m->second.ready) return;
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

void ThumbnailCache::remember(const std::string& path, std::vector<uint8_t>&& rgba) {
    if (cache_.find(path) == cache_.end()) cacheOrder_.push_back(path);
    cache_[path] = std::move(rgba);
    while (cache_.size() > kMaxCached && !cacheOrder_.empty()) {
        cache_.erase(cacheOrder_.front());
        cacheOrder_.pop_front();
    }
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

template <class Pred>
void ThumbnailCache::forgetIf(Pred pred) {
    std::lock_guard<std::mutex> lk(mutex_);
    cacheOrder_.erase(std::remove_if(cacheOrder_.begin(), cacheOrder_.end(), pred), cacheOrder_.end());
    for (auto it = cache_.begin(); it != cache_.end();) it = pred(it->first) ? cache_.erase(it) : std::next(it);
    for (auto it = metaCache_.begin(); it != metaCache_.end();) it = pred(it->first) ? metaCache_.erase(it) : std::next(it);
    for (auto it = queuedPaths_.begin(); it != queuedPaths_.end();) it = pred(it->first) ? queuedPaths_.erase(it) : std::next(it);
    for (auto it = failed_.begin(); it != failed_.end();) it = pred(it->first) ? failed_.erase(it) : std::next(it);
    queue_.erase(std::remove_if(queue_.begin(), queue_.end(), pred), queue_.end());
}

void ThumbnailCache::clearPrivateThumbs() {
    forgetIf([](const std::string& p) { return VaultManager::isPrivatePath(p); });
}

void ThumbnailCache::forgetUnder(const std::string& path) {
    if (path.empty()) return;
    forgetIf([&](const std::string& p) {
        return p == path || (p.size() > path.size() && p.compare(0, path.size(), path) == 0 && p[path.size()] == '/');
    });
}

bool ThumbnailCache::extractThumbnailAndMeta(const std::string& videoPath,
                                             std::vector<uint8_t>& outRgba,
                                             MediaMeta& outMeta) {
#ifdef PIXELVR_HAVE_MEDIA
    std::unique_ptr<VaultAvio> vaultIo;  // encrypted vault file: decrypted while reading (outlives fmt)
    AVFormatContext* fmt = nullptr;
    const bool encrypted = vaultcrypto::isEncrypted(videoPath);
    if (encrypted ? !openVaultFormat(videoPath, fmt, vaultIo)
                  : (avformat_open_input(&fmt, videoPath.c_str(), nullptr, nullptr) < 0 || !fmt)) {
        return false;
    }
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        avformat_close_input(&fmt);
        return false;
    }

    int vIdx = findVideoStream(fmt);  // cover art is not video
    if (vIdx < 0) {
        // Audio file: the same card the player shows (cover art or a note).
        const bool audio = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0) >= 0;
        if (audio) {
            if (fmt->duration > 0) outMeta.durationSec = static_cast<double>(fmt->duration) / AV_TIME_BASE;
            outMeta.ready = true;
            std::vector<uint8_t> cover;
            int cw = 0, ch = 0;
            decodeCoverArt(fmt, cover, cw, ch);
            composeAudioCard(cover, cw, ch, kThumbW, kThumbH, outRgba);
        }
        avformat_close_input(&fmt);
        return audio;
    }

    AVStream* st = fmt->streams[vIdx];
    outMeta.width = st->codecpar->width;
    outMeta.height = st->codecpar->height;
    const FormatHint hint = formatFromMetadata(st);
    outMeta.projectionHint = hint.projection;
    outMeta.stereoHint = hint.stereo;
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
    // AV1: only dav1d works here (FFmpeg's own decoder needs hardware support).
    const AVCodec* dec = (st->codecpar->codec_id == AV_CODEC_ID_AV1) ? avcodec_find_decoder_by_name("libdav1d")
                                                                     : avcodec_find_decoder(st->codecpar->codec_id);
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

    // No stereo metadata: look at the picture (stereo pairs repeat in halves).
    if (gotFrame && outMeta.stereoHint < 0) outMeta.stereoHint = std::max(0, stereoFromPicture(frame));
    bool ok = false;
    if (gotFrame && frame->width > 0 && frame->height > 0) {
        // For Side-by-Side (SBS) 3D or VR180 videos (aspect >= 1.95 or width == 2*height),
        // crop the left eye (left half of frame) so the thumbnail shows a normal single view
        // instead of a squished double-eye image!
        int srcW = frame->width;
        int srcH = frame->height;
        const float ar = static_cast<float>(srcW) / static_cast<float>(srcH);
        std::string lowerPath = encrypted && vaultIo ? vaultIo->reader.name() : videoPath;
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

namespace {

bool readBytes(const std::string& file, std::string& out) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

void writeBytes(const std::string& file, const std::string& data) {
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
    }
    std::error_code ec;
    fs::permissions(file, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
}

// Vault thumbnails can't name their source (that would be readable without the key): they go after
// half a year without being shown.
void pruneVaultThumbs(const std::string& dir) {
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec)) return;
    using namespace std::chrono_literals;
    const auto now = fs::file_time_type::clock::now();
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(dir, ec)) files.push_back(e.path());
    for (const fs::path& p : files) {
        if (p.extension() != ".rgba") continue;
        std::error_code te;
        if (now - fs::last_write_time(p, te) > 180 * 24h) {
            fs::remove(p, te);
            fs::remove(fs::path(p).replace_extension(".meta"), te);
        }
    }
}

}  // namespace

void ThumbnailCache::handlePrivate(const std::string& path) {
    if (!vaultcrypto::hasKey()) return;  // locked: nothing is read or made
    VaultManager& vm = VaultManager::instance();
    const std::string dir = vm.thumbsDir();
    std::error_code ec;
    fs::create_directories(dir, ec);
    fs::permissions(dir, fs::perms::owner_all, fs::perm_options::replace, ec);
    // Encrypted files: keyed by their random name (stays the same when moved inside the vault or to the
    // SD card); plain leftovers in the vault by path.
    const std::string key = vaultcrypto::hasExtension(path) ? fs::path(path).filename().string() : path;
    const std::string base = dir + "/" + std::to_string(fnv1a64(key));
    MediaMeta meta{};
    auto parseMeta = [&](const std::string& plain) {
        if (plain.size() < sizeof(DiskMetaHeader) + sizeof(DiskMetaHints)) return;
        DiskMetaHeader hdr{};
        DiskMetaHints hints{};
        std::memcpy(&hdr, plain.data(), sizeof(hdr));
        std::memcpy(&hints, plain.data() + sizeof(hdr), sizeof(hints));
        if (hdr.magic != kMetaMagicV2) return;
        meta.width = hdr.width;
        meta.height = hdr.height;
        meta.fps = hdr.fps;
        meta.durationSec = hdr.durationSec;
        meta.projectionHint = hints.projection;
        meta.stereoHint = hints.stereo;
        meta.ready = true;
    };
    auto saveMeta = [&] {
        const DiskMetaHeader hdr{kMetaMagicV2, meta.width, meta.height, meta.fps, meta.durationSec};
        const DiskMetaHints hints{meta.projectionHint, meta.stereoHint};
        std::string plain(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
        plain.append(reinterpret_cast<const char*>(&hints), sizeof(hints));
        std::string blob;
        if (vaultcrypto::encryptBlob(plain, blob)) writeBytes(base + ".meta", blob);
    };
    std::string blob, plain;
    if (readBytes(base + ".rgba", blob) && vaultcrypto::decryptBlob(blob, plain) && plain.size() == kThumbBytes) {
        std::string mb, mp;
        if (readBytes(base + ".meta", mb) && vaultcrypto::decryptBlob(mb, mp)) parseMeta(mp);
        if (!meta.ready) {
            std::vector<uint8_t> dummy;
            extractThumbnailAndMeta(path, dummy, meta);
            if (meta.ready) saveMeta();
        }
        fs::last_write_time(base + ".rgba", fs::file_time_type::clock::now(), ec);
        std::vector<uint8_t> data(plain.begin(), plain.end());
        std::lock_guard<std::mutex> lk(mutex_);
        if (!vaultcrypto::hasKey()) return;  // locked while reading
        remember(path, std::move(data));
        if (meta.ready) metaCache_[path] = meta;
        return;
    }
    std::vector<uint8_t> rgba;
    const bool ok = extractThumbnailAndMeta(path, rgba, meta);
    if (meta.ready) saveMeta();
    if (ok && rgba.size() == kThumbBytes) {
        std::string enc;
        if (vaultcrypto::encryptBlob(std::string(rgba.begin(), rgba.end()), enc)) writeBytes(base + ".rgba", enc);
    }
    std::lock_guard<std::mutex> lk(mutex_);
    if (!vaultcrypto::hasKey()) return;
    if (meta.ready) metaCache_[path] = meta;
    if (ok && rgba.size() == kThumbBytes) remember(path, std::move(rgba));
    else failed_[path] = fileStamp(path);
}

void ThumbnailCache::workerLoop() {
    // Once per start, before any thumbnails are made.
    pruneThumbnails(cacheDir_);
    pruneVaultThumbs(VaultManager::instance().thumbsDir());
    {
        // Before v1.19 vault thumbnails were plain pictures inside the vault folder: gone for good.
        std::error_code ec;
        fs::remove_all(VaultManager::instance().internalVaultDir() + "/.thumbs", ec);
    }
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

        if (VaultManager::isPrivatePath(path)) {
            handlePrivate(path);
            continue;
        }
        const std::string targetCacheDir = cacheDir_;
        const std::string baseHash = std::to_string(fnv1a64(path));
        const std::string cacheFile = targetCacheDir + "/" + baseHash + ".rgba";
        const std::string metaFile = targetCacheDir + "/" + baseHash + ".meta";

        std::error_code ec;
        if (fs::is_regular_file(cacheFile, ec) && fs::file_size(cacheFile, ec) == kThumbBytes) {
            std::ifstream in(cacheFile, std::ios::binary);
            std::vector<uint8_t> data(kThumbBytes);
            if (in.read(reinterpret_cast<char*>(data.data()), kThumbBytes)) {
                MediaMeta meta{};
                const auto metaSize = fs::is_regular_file(metaFile, ec) ? fs::file_size(metaFile, ec) : 0;
                // Older .meta files lack the format hints: probe those again.
                if (metaSize >= sizeof(DiskMetaHeader) + sizeof(DiskMetaHints)) {
                    std::ifstream min(metaFile, std::ios::binary);
                    DiskMetaHeader hdr{};
                    DiskMetaHints hints{};
                    if (min.read(reinterpret_cast<char*>(&hdr), sizeof(hdr)) && hdr.magic == kMetaMagicV2 &&
                        min.read(reinterpret_cast<char*>(&hints), sizeof(hints))) {
                        meta.width = hdr.width;
                        meta.height = hdr.height;
                        meta.fps = hdr.fps;
                        meta.durationSec = hdr.durationSec;
                        meta.projectionHint = hints.projection;
                        meta.stereoHint = hints.stereo;
                        meta.ready = true;
                    }
                }
                if (!meta.ready) {
                    // Quick metadata probe if .meta was not cached yet (or is old)
                    std::vector<uint8_t> dummy;
                    extractThumbnailAndMeta(path, dummy, meta);
                    if (meta.ready) writeMeta(metaFile, meta, path);
                }
                fs::last_write_time(cacheFile, fs::file_time_type::clock::now(), ec);  // recently used
                std::lock_guard<std::mutex> lk(mutex_);
                remember(path, std::move(data));
                if (meta.ready) metaCache_[path] = meta;
                continue;
            }
        }

        std::vector<uint8_t> rgba;
        MediaMeta meta{};
        bool ok = extractThumbnailAndMeta(path, rgba, meta);
        {
            // Moved into the vault (or deleted) while it was being decoded: nothing public is written.
            std::error_code gec;
            if (VaultManager::isPrivatePath(path) || !fs::exists(path, gec)) continue;
        }
        if (meta.ready) {
            writeMeta(metaFile, meta, path);
            std::lock_guard<std::mutex> lk(mutex_);
            metaCache_[path] = meta;
        }
        if (ok && rgba.size() == kThumbBytes) {
            {
                std::ofstream out(cacheFile, std::ios::binary);
                out.write(reinterpret_cast<const char*>(rgba.data()), kThumbBytes);
            }
            std::lock_guard<std::mutex> lk(mutex_);
            remember(path, std::move(rgba));
            PIXELVR_LOG_INFO("Thumbnail generated for %s (%dx%d, %.0fs)",
                             path.c_str(), meta.width, meta.height, meta.durationSec);
        } else {
            PIXELVR_LOG_WARN("Thumbnail generation failed for %s", path.c_str());
            std::lock_guard<std::mutex> lk(mutex_);
            failed_[path] = fileStamp(path);  // e.g. no decoder: asking again every frame floods the log
        }
    }
}

} // namespace pixelvr

#include "media/letterbox.hpp"

#include "app/vault_crypto.hpp"

#include "util/logging.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#ifdef PIXELVR_HAVE_MEDIA
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
}
#include "media/vault_avio.hpp"
#endif

namespace fs = std::filesystem;

namespace pixelvr {

namespace {

// Identity of a local file's content (size + mtime); streams: none.
std::string keyOf(const std::string& path) {
    std::error_code ec;
    if (!path.empty() && path[0] == '/') {
        const auto size = fs::file_size(path, ec);
        const auto mt = fs::last_write_time(path, ec).time_since_epoch().count();
        return path + "|" + std::to_string(ec ? 0 : size) + "|" + std::to_string(ec ? 0 : mt);
    }
    const size_t q = path.find('?');  // no tokens in the key
    return q == std::string::npos ? path : path.substr(0, q);
}

#ifdef PIXELVR_HAVE_MEDIA
// Black rows from the top (dir = +1) or bottom (dir = -1) of an 8/16-bit luma plane.
int blackRows(const AVFrame* f, int dir, int shift, int blackLevel) {
    const int w = f->width, h = f->height;
    const int step = std::max(1, w / 256);
    int rows = 0;
    for (int i = 0; i < h / 2; ++i) {
        const int y = dir > 0 ? i : h - 1 - i;
        const uint8_t* row = f->data[0] + static_cast<ptrdiff_t>(y) * f->linesize[0];
        long sum = 0;
        int maxv = 0, n = 0;
        for (int x = 0; x < w; x += step, ++n) {
            const int v = shift >= 0 ? (reinterpret_cast<const uint16_t*>(row)[x] >> shift) : row[x];
            sum += v;
            maxv = std::max(maxv, v);
        }
        // Black: dark on average and nowhere bright (dark film content has detail somewhere).
        if (n == 0 || sum / n > blackLevel + 8 || maxv > blackLevel + 26) break;
        ++rows;
    }
    return rows;
}

// Mean luma of the middle third: is this frame bright enough to judge?
int middleBrightness(const AVFrame* f, int shift) {
    const int w = f->width, h = f->height;
    long sum = 0;
    long n = 0;
    for (int y = h / 3; y < 2 * h / 3; y += std::max(1, h / 60)) {
        const uint8_t* row = f->data[0] + static_cast<ptrdiff_t>(y) * f->linesize[0];
        for (int x = 0; x < w; x += std::max(1, w / 120), ++n) sum += shift >= 0 ? (reinterpret_cast<const uint16_t*>(row)[x] >> shift) : row[x];
    }
    return n ? static_cast<int>(sum / n) : 0;
}
#endif

}  // namespace

float LetterboxDetector::measure(const std::string& path, int* framesJudged) {
    if (framesJudged) *framesJudged = 0;
#ifdef PIXELVR_HAVE_MEDIA
    AVFormatContext* fmt = nullptr;
    std::unique_ptr<VaultAvio> vaultIo;  // encrypted vault file: decrypted while reading (outlives fmt)
    if (vaultcrypto::isEncrypted(path) ? !openVaultFormat(path, fmt, vaultIo)
                                       : avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) {
        return 0.0f;
    }
    float out = 0.0f;
    do {
        if (avformat_find_stream_info(fmt, nullptr) < 0) break;
        const int vs = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (vs < 0 || (fmt->streams[vs]->disposition & AV_DISPOSITION_ATTACHED_PIC)) break;
        AVStream* st = fmt->streams[vs];
        // Software decoders only (the hardware one belongs to playback); AV1 via dav1d.
        const AVCodec* dec = st->codecpar->codec_id == AV_CODEC_ID_AV1 ? avcodec_find_decoder_by_name("libdav1d")
                                                                        : avcodec_find_decoder(st->codecpar->codec_id);
        if (dec && std::strstr(dec->name, "v4l2")) {
            if (st->codecpar->codec_id == AV_CODEC_ID_HEVC) dec = avcodec_find_decoder_by_name("hevc");
            else if (st->codecpar->codec_id == AV_CODEC_ID_H264) dec = avcodec_find_decoder_by_name("h264");
        }
        if (!dec) break;
        AVCodecContext* cc = avcodec_alloc_context3(dec);
        avcodec_parameters_to_context(cc, st->codecpar);
        cc->thread_count = 2;
        if (avcodec_open2(cc, dec, nullptr) < 0) {
            avcodec_free_context(&cc);
            break;
        }
        const double dur = fmt->duration > 0 ? fmt->duration / static_cast<double>(AV_TIME_BASE) : 0.0;
        AVPacket* pk = av_packet_alloc();
        AVFrame* fr = av_frame_alloc();
        int judged = 0, bar = 1 << 30, height = 0;
        int minTop = 1 << 30, minBottom = 1 << 30;
        const double spots[] = {0.12, 0.25, 0.38, 0.5, 0.62, 0.75, 0.88, 0.95};
        for (double at : spots) {
            if (dur > 20.0) {
                av_seek_frame(fmt, -1, static_cast<int64_t>(at * dur * AV_TIME_BASE), AVSEEK_FLAG_BACKWARD);
                avcodec_flush_buffers(cc);
            } else if (at > 0.12) {
                break;  // short clips: one look
            }
            bool got = false;
            for (int k = 0; k < 400 && !got && av_read_frame(fmt, pk) >= 0; ++k) {
                if (pk->stream_index == vs && avcodec_send_packet(cc, pk) >= 0) got = avcodec_receive_frame(cc, fr) >= 0;
                av_packet_unref(pk);
            }
            static const bool debug = std::getenv("PIXELVR_LB_DEBUG") != nullptr;
            if (debug) std::fprintf(stderr, "lb: at %.2f got=%d fmt=%d %dx%d\n", at, got ? 1 : 0, got ? fr->format : -1, fr->width, fr->height);
            if (!got || fr->width <= 0 || fr->height <= 0) continue;
            const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(fr->format));
            if (!d || (d->flags & AV_PIX_FMT_FLAG_RGB) || (d->flags & AV_PIX_FMT_FLAG_HWACCEL)) continue;
            // 16-bit samples: shift down to 8 bits (P010 keeps them in the top bits, yuv420p10 in the bottom).
            const int wide = d->comp[0].depth > 8 ? (d->comp[0].shift > 0 ? 8 : d->comp[0].depth - 8) : -1;
            const int black = fr->color_range == AVCOL_RANGE_JPEG ? 0 : 16;
            if (debug) std::fprintf(stderr, "lb:   middle %d black %d wide %d top %d bottom %d\n", middleBrightness(fr, wide), black, wide, blackRows(fr, +1, wide, black), blackRows(fr, -1, wide, black));
            if (middleBrightness(fr, wide) < black + 22) continue;  // too dark to tell bars from the film
            const int top = blackRows(fr, +1, wide, black), bottom = blackRows(fr, -1, wide, black);
            height = fr->height;
            ++judged;
            minTop = std::min(minTop, top);
            minBottom = std::min(minBottom, bottom);
            bar = std::min(bar, std::min(top, bottom));
        }
        av_frame_free(&fr);
        av_packet_free(&pk);
        avcodec_free_context(&cc);
        if (framesJudged) *framesJudged = judged;
        // Bars are the same size at the top and at the bottom. Judged on the smallest of each over all
        // frames: one frame with more black on one side (end credits, a dark sky) says nothing.
        const bool asymmetric = judged > 0 && std::abs(minTop - minBottom) > height / 25;
        // Needs a few judged frames; small bars (< 1.5 %) are not worth a crop. The cut goes 2 rows into
        // the picture: the bar's last row is usually a soft, half-dark one, and rows of bar left on
        // the screen show as a black line along its top and bottom edge.
        if (judged >= (dur > 20.0 ? 3 : 1) && height > 0 && bar < (1 << 30) && !asymmetric) {
            const int crop = bar + 2;
            const float frac = static_cast<float>(crop) / static_cast<float>(height);
            out = frac >= 0.015f ? std::min(frac, 0.3f) : 0.0f;
        }
    } while (false);
    avformat_close_input(&fmt);
    return out;
#else
    (void)path;
    return 0.0f;
#endif
}

LetterboxDetector::LetterboxDetector() {
    const char* home = std::getenv("HOME");
    const std::string dir = std::string(home ? home : "/tmp") + "/.cache/pixelvr/";
    file_ = dir + "letterbox3.tsv";  // (3: cut 2 rows inside the picture; symmetry judged over all frames)
    std::error_code ec;
    fs::remove(dir + "letterbox.tsv", ec);
    fs::remove(dir + "letterbox2.tsv", ec);
    load();
    worker_ = std::thread([this] { loop(); });
}

LetterboxDetector::~LetterboxDetector() {
    {
        std::lock_guard<std::mutex> l(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void LetterboxDetector::load() {
    std::ifstream in(file_);
    std::string line;
    while (std::getline(in, line)) {
        const size_t tab = line.rfind('\t');
        if (tab == std::string::npos) continue;
        results_[line.substr(0, tab)] = std::strtof(line.c_str() + tab + 1, nullptr);
    }
}

void LetterboxDetector::forgetUnder(const std::string& path) {
    if (path.empty()) return;
    std::lock_guard<std::mutex> lk(mu_);
    bool changed = false;
    for (auto it = results_.begin(); it != results_.end();) {
        const std::string& k = it->first;  // "<path>|size|mtime"
        const bool under = k.size() > path.size() && k.compare(0, path.size(), path) == 0 &&
                           (k[path.size()] == '|' || k[path.size()] == '/');
        if (under) {
            it = results_.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    if (changed) save();
}

void LetterboxDetector::save() {
    std::error_code ec;
    fs::create_directories(fs::path(file_).parent_path(), ec);
    std::ofstream out(file_ + ".tmp", std::ios::trunc);
    for (const auto& kv : results_) out << kv.first << '\t' << kv.second << '\n';
    out.close();
    fs::rename(file_ + ".tmp", file_, ec);
}

void LetterboxDetector::request(const std::string& path, bool keepPrivate) {
    if (path.empty() || !(path[0] == '/' || path.rfind("http://", 0) == 0 || path.rfind("https://", 0) == 0)) return;
    std::lock_guard<std::mutex> l(mu_);
    const std::string key = keyOf(path);
    if (results_.count(key) || privateResults_.count(key) ||
        std::find(queue_.begin(), queue_.end(), path) != queue_.end()) {
        return;
    }
    if (keepPrivate) privateQueued_.push_back(path);
    queue_.push_back(path);
    cv_.notify_all();
}

bool LetterboxDetector::result(const std::string& path, float& frac) {
    std::lock_guard<std::mutex> l(mu_);
    const std::string key = keyOf(path);
    auto it = results_.find(key);
    if (it == results_.end()) {
        it = privateResults_.find(key);
        if (it == privateResults_.end()) return false;
    }
    frac = it->second;
    return true;
}

void LetterboxDetector::loop() {
    for (;;) {
        std::string path;
        {
            std::unique_lock<std::mutex> l(mu_);
            cv_.wait(l, [this] { return stop_ || !queue_.empty(); });
            if (stop_) return;
            path = queue_.front();
        }
        int judged = 0;
        const float frac = measure(path, &judged);
        PIXELVR_LOG_INFO("Letterbox: %.1f %% top and bottom (%d frames judged)", frac * 100.0f, judged);
        std::lock_guard<std::mutex> l(mu_);
        if (!queue_.empty()) queue_.pop_front();
        const auto priv = std::find(privateQueued_.begin(), privateQueued_.end(), path);
        if (priv != privateQueued_.end()) {  // a vault file: nothing about it goes to disk
            privateQueued_.erase(priv);
            privateResults_[keyOf(path)] = frac;
        } else {
            results_[keyOf(path)] = frac;
            save();
        }
    }
}

}  // namespace pixelvr

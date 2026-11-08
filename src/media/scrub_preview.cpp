#include "media/scrub_preview.hpp"

#include "util/logging.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

namespace pixelvr {

namespace {

// Open demuxer + software decoder for one clip, owned by the worker thread.
struct Decoder {
    AVFormatContext* fmt = nullptr;
    AVCodecContext* ctx = nullptr;
    AVPacket* pkt = nullptr;
    AVFrame* frame = nullptr;
    int stream = -1;

    ~Decoder() { close(); }

    void close() {
        if (frame) av_frame_free(&frame);
        if (pkt) av_packet_free(&pkt);
        if (ctx) avcodec_free_context(&ctx);
        if (fmt) avformat_close_input(&fmt);
        stream = -1;
    }

    bool open(const std::string& path) {
        close();
        // Plex / Jellyfin streams: a stalled server must not hang the preview worker for good.
        AVDictionary* opts = nullptr;
        if (path.rfind("http://", 0) == 0 || path.rfind("https://", 0) == 0) av_dict_set(&opts, "rw_timeout", "10000000", 0);
        const int rc = avformat_open_input(&fmt, path.c_str(), nullptr, &opts);
        av_dict_free(&opts);
        if (rc < 0 || !fmt) return false;
        if (avformat_find_stream_info(fmt, nullptr) < 0) return false;
        stream = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (stream < 0) return false;
        const AVCodecParameters* par = fmt->streams[stream]->codecpar;
        // Software decoders only: the V4L2 hardware slots belong to playback.
        const AVCodec* dec = avcodec_find_decoder(par->codec_id);
        if (dec && dec->name && std::strstr(dec->name, "v4l2") != nullptr) {
            if (par->codec_id == AV_CODEC_ID_HEVC) dec = avcodec_find_decoder_by_name("hevc");
            else if (par->codec_id == AV_CODEC_ID_H264) dec = avcodec_find_decoder_by_name("h264");
        }
        if (!dec) return false;
        ctx = avcodec_alloc_context3(dec);
        if (!ctx) return false;
        avcodec_parameters_to_context(ctx, par);
        ctx->thread_count = 2;
        ctx->skip_frame = AVDISCARD_NONKEY;  // keyframes are enough for a preview
        ctx->skip_loop_filter = AVDISCARD_ALL;
        if (avcodec_open2(ctx, dec, nullptr) < 0) return false;
        pkt = av_packet_alloc();
        frame = av_frame_alloc();
        return pkt && frame;
    }

    // Decodes the keyframe at or before `seconds` into a kWidth x kHeight RGBA image.
    bool decodeAt(double seconds, int stereo, std::vector<uint8_t>& out) {
        if (!fmt || !ctx) return false;
        const AVStream* st = fmt->streams[stream];
        const int64_t ts = (st->time_base.num > 0)
            ? static_cast<int64_t>(seconds / av_q2d(st->time_base))
            : static_cast<int64_t>(seconds * AV_TIME_BASE);
        if (av_seek_frame(fmt, stream, ts, AVSEEK_FLAG_BACKWARD) < 0) return false;
        avcodec_flush_buffers(ctx);
        bool got = false;
        for (int packets = 0; packets < 240 && !got && av_read_frame(fmt, pkt) >= 0; ) {
            if (pkt->stream_index == stream) {
                ++packets;
                if (avcodec_send_packet(ctx, pkt) == 0 && avcodec_receive_frame(ctx, frame) == 0) {
                    got = true;
                }
            }
            av_packet_unref(pkt);
        }
        if (!got || frame->width <= 0 || frame->height <= 0) return false;

        int srcW = frame->width;
        int srcH = frame->height;
        if (stereo == 1) srcW /= 2;       // left eye of side-by-side
        else if (stereo == 2) srcH /= 2;  // top eye of top-bottom
        SwsContext* sws = sws_getContext(srcW, srcH, static_cast<AVPixelFormat>(frame->format),
                                         ScrubPreview::kWidth, ScrubPreview::kHeight,
                                         AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws) return false;
        out.resize(static_cast<std::size_t>(ScrubPreview::kWidth) * ScrubPreview::kHeight * 4);
        uint8_t* dst[4] = {out.data(), nullptr, nullptr, nullptr};
        int dstStride[4] = {ScrubPreview::kWidth * 4, 0, 0, 0};
        sws_scale(sws, frame->data, frame->linesize, 0, srcH, dst, dstStride);
        sws_freeContext(sws);
        return true;
    }
};

}  // namespace

ScrubPreview::ScrubPreview() : worker_(&ScrubPreview::workerLoop, this) {}

ScrubPreview::~ScrubPreview() {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void ScrubPreview::setSource(const std::string& path, int stereoMode) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (path == path_ && stereoMode == stereo_) return;
    path_ = path;
    stereo_ = stereoMode;
    ++sourceGen_;
    hasRequest_ = false;
    resultNew_ = false;
    cv_.notify_all();
}

void ScrubPreview::request(double seconds) {
    // Quantize so hovering still doesn't re-decode every frame.
    const double q = std::floor(std::max(0.0, seconds) / 2.0) * 2.0;
    std::lock_guard<std::mutex> lk(mutex_);
    if (path_.empty()) return;
    if (hasRequest_ && requestSec_ == q) return;
    if (!hasRequest_ && resultSec_ == q && !result_.empty()) return;  // already showing it
    requestSec_ = q;
    hasRequest_ = true;
    cv_.notify_all();
}

bool ScrubPreview::fetch(std::vector<uint8_t>& rgba, double& seconds) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!resultNew_) return false;
    rgba = result_;
    seconds = resultSec_;
    resultNew_ = false;
    return true;
}

void ScrubPreview::workerLoop() {
    Decoder dec;
    uint64_t openedGen = 0;
    while (true) {
        std::unique_lock<std::mutex> lk(mutex_);
        cv_.wait(lk, [&] { return stop_ || sourceGen_ != openedGen || hasRequest_; });
        if (stop_) break;
        if (sourceGen_ != openedGen) {
            // (Re)open the new clip first; pending requests are served after.
            openedGen = sourceGen_;
            const std::string path = path_;
            lk.unlock();
            dec.close();
            if (!path.empty() && !dec.open(path)) {
                PIXELVR_LOG_WARN("Scrub preview: cannot open %s", path.c_str());
                dec.close();
            }
            continue;
        }
        const double sec = requestSec_;
        const int stereo = stereo_;
        const uint64_t gen = sourceGen_;
        hasRequest_ = false;
        lk.unlock();

        std::vector<uint8_t> img;
        if (!dec.decodeAt(sec, stereo, img)) continue;
        lk.lock();
        if (sourceGen_ != gen) continue;  // clip changed while decoding
        result_ = std::move(img);
        resultSec_ = sec;
        resultNew_ = true;
    }
}

}  // namespace pixelvr

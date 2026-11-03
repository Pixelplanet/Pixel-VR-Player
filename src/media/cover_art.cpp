#include "media/cover_art.hpp"

#include <algorithm>
#include <cmath>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

namespace pixelvr {

bool isAttachedPicture(const AVFormatContext* fmt, int streamIndex) {
    if (!fmt || streamIndex < 0 || streamIndex >= static_cast<int>(fmt->nb_streams)) return false;
    return (fmt->streams[streamIndex]->disposition & AV_DISPOSITION_ATTACHED_PIC) != 0;
}

int findVideoStream(AVFormatContext* fmt) {
    if (!fmt) return -1;
    const int best = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (best >= 0 && !isAttachedPicture(fmt, best)) return best;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && !isAttachedPicture(fmt, static_cast<int>(i))) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

namespace {

// Scales RGBA (or any decoded frame) to w x h RGBA.
bool scaleTo(const uint8_t* const src[4], const int srcStride[4], int sw, int sh, AVPixelFormat fmt, int w, int h,
             std::vector<uint8_t>& out) {
    SwsContext* sws = sws_getContext(sw, sh, fmt, w, h, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws) return false;
    out.assign(static_cast<size_t>(w) * h * 4, 0);
    uint8_t* dst[4] = {out.data(), nullptr, nullptr, nullptr};
    int dstStride[4] = {w * 4, 0, 0, 0};
    sws_scale(sws, src, srcStride, 0, sh, dst, dstStride);
    sws_freeContext(sws);
    return true;
}

bool scaleRgba(const std::vector<uint8_t>& in, int iw, int ih, int w, int h, std::vector<uint8_t>& out) {
    const uint8_t* src[4] = {in.data(), nullptr, nullptr, nullptr};
    const int stride[4] = {iw * 4, 0, 0, 0};
    return scaleTo(src, stride, iw, ih, AV_PIX_FMT_RGBA, w, h, out);
}

float smooth(float edge0, float edge1, float x) {
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// Signed distance to a rotated ellipse-ish note head (approximate).
float noteHead(float px, float py, float cx, float cy, float r) {
    const float c = std::cos(-0.45f), s = std::sin(-0.45f);
    const float dx = px - cx, dy = py - cy;
    const float x = dx * c - dy * s, y = dx * s + dy * c;
    return std::sqrt((x / 1.25f) * (x / 1.25f) + y * y) - r;
}

}  // namespace

bool decodeCoverArt(AVFormatContext* fmt, std::vector<uint8_t>& rgba, int& width, int& height) {
    if (!fmt) return false;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        AVStream* st = fmt->streams[i];
        if (!(st->disposition & AV_DISPOSITION_ATTACHED_PIC) || st->attached_pic.size <= 0) continue;
        const AVCodec* dec = avcodec_find_decoder(st->codecpar->codec_id);
        if (!dec) continue;
        AVCodecContext* ctx = avcodec_alloc_context3(dec);
        if (!ctx) continue;
        bool ok = false;
        AVFrame* frame = av_frame_alloc();
        if (frame && avcodec_parameters_to_context(ctx, st->codecpar) >= 0 && avcodec_open2(ctx, dec, nullptr) >= 0 &&
            avcodec_send_packet(ctx, &st->attached_pic) == 0) {
            avcodec_send_packet(ctx, nullptr);
            if (avcodec_receive_frame(ctx, frame) == 0 && frame->width > 0 && frame->height > 0) {
                width = std::min(frame->width, 1024);
                height = std::max(1, frame->height * width / frame->width);
                ok = scaleTo(frame->data, frame->linesize, frame->width, frame->height,
                             static_cast<AVPixelFormat>(frame->format), width, height, rgba);
            }
        }
        av_frame_free(&frame);
        avcodec_free_context(&ctx);
        if (ok) return true;
    }
    return false;
}

void composeAudioCard(const std::vector<uint8_t>& cover, int coverW, int coverH, int w, int h,
                      std::vector<uint8_t>& out) {
    out.assign(static_cast<size_t>(w) * h * 4, 255);
    if (!cover.empty() && coverW > 0 && coverH > 0) {
        // Background: the cover blurred (tiny, then scaled back up) and darkened.
        std::vector<uint8_t> tiny, bg;
        if (scaleRgba(cover, coverW, coverH, 24, 14, tiny) && scaleRgba(tiny, 24, 14, w, h, bg)) {
            for (size_t i = 0; i < bg.size(); i += 4) {
                for (int c = 0; c < 3; ++c) out[i + c] = static_cast<uint8_t>(bg[i + c] * 0.42f);
            }
        }
        // Foreground: the cover itself, as large as fits with a margin.
        const float fit = std::min(0.80f * h / coverH, 0.86f * w / coverW);
        const int fw = std::max(1, static_cast<int>(coverW * fit));
        const int fh = std::max(1, static_cast<int>(coverH * fit));
        std::vector<uint8_t> fg;
        if (scaleRgba(cover, coverW, coverH, fw, fh, fg)) {
            const int x0 = (w - fw) / 2, y0 = (h - fh) / 2;
            // Soft shadow under the cover.
            const int sh = std::max(4, h / 60);
            for (int y = std::max(0, y0 - sh); y < std::min(h, y0 + fh + 2 * sh); ++y) {
                for (int x = std::max(0, x0 - sh); x < std::min(w, x0 + fw + sh); ++x) {
                    const float dx = std::max({static_cast<float>(x0 - x), 0.0f, static_cast<float>(x - (x0 + fw - 1))});
                    const float dy = std::max({static_cast<float>(y0 + sh / 2 - y), 0.0f,
                                               static_cast<float>(y - (y0 + fh - 1 + sh / 2))});
                    const float k = 1.0f - 0.55f * (1.0f - smooth(0.0f, static_cast<float>(sh), std::sqrt(dx * dx + dy * dy)));
                    uint8_t* p = &out[(static_cast<size_t>(y) * w + x) * 4];
                    for (int c = 0; c < 3; ++c) p[c] = static_cast<uint8_t>(p[c] * k);
                }
            }
            for (int y = 0; y < fh; ++y) {
                std::copy_n(&fg[static_cast<size_t>(y) * fw * 4], static_cast<size_t>(fw) * 4,
                            &out[(static_cast<size_t>(y0 + y) * w + x0) * 4]);
            }
        }
        return;
    }
    // No cover: a dark gradient with a large beamed music note.
    const float u = static_cast<float>(h) / 100.0f;  // drawing unit
    const float cx = w * 0.5f, cy = h * 0.5f;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float t = (static_cast<float>(x) / w + static_cast<float>(y) / h) * 0.5f;
            float r = 22 + 30 * t, g = 26 + 14 * t, b = 48 + 40 * t;
            const float px = x - cx, py = y - cy;
            // Two heads, two stems, a beam (all relative to the centre).
            float d = std::min(noteHead(px, py, -13 * u, 17 * u, 7 * u), noteHead(px, py, 13 * u, 11 * u, 7 * u));
            const float stem1 = std::max(std::abs(px - (-5.5f * u)) - 1.6f * u, std::abs(py - (-3 * u)) - 20 * u);
            const float stem2 = std::max(std::abs(px - 20.5f * u) - 1.6f * u, std::abs(py - (-9 * u)) - 20 * u);
            // Beam: a slanted band from the left stem top to the right one.
            const float beamY = -23 * u + (px + 5.5f * u) * (-6.0f / 26.0f);
            const float beam = std::max(std::abs(py - beamY) - 3.2f * u, std::abs(px - 7.5f * u) - 14.6f * u);
            d = std::min({d, stem1, stem2, beam});
            const float a = 1.0f - smooth(-0.8f, 0.8f, d);
            r += (225 - r) * a;
            g += (230 - g) * a;
            b += (245 - b) * a;
            uint8_t* p = &out[(static_cast<size_t>(y) * w + x) * 4];
            p[0] = static_cast<uint8_t>(r);
            p[1] = static_cast<uint8_t>(g);
            p[2] = static_cast<uint8_t>(b);
            p[3] = 255;
        }
    }
}

}  // namespace pixelvr

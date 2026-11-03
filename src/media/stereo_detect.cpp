#include "media/stereo_detect.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/spherical.h>
#include <libavutil/stereo3d.h>
#include <libswscale/swscale.h>
}

namespace pixelvr {

FormatHint formatFromMetadata(const AVStream* stream) {
    FormatHint hint;
    if (!stream || !stream->codecpar) return hint;
    const AVCodecParameters* cp = stream->codecpar;
    if (const AVPacketSideData* sd =
            av_packet_side_data_get(cp->coded_side_data, cp->nb_coded_side_data, AV_PKT_DATA_STEREO3D)) {
        const auto* s3d = reinterpret_cast<const AVStereo3D*>(sd->data);
        switch (s3d->type) {
            case AV_STEREO3D_2D: hint.stereo = 0; break;
            case AV_STEREO3D_SIDEBYSIDE:
            case AV_STEREO3D_SIDEBYSIDE_QUINCUNX: hint.stereo = 1; break;
            case AV_STEREO3D_TOPBOTTOM: hint.stereo = 2; break;
            default: break;  // frame / line / column interleaving: not supported
        }
    }
    if (const AVPacketSideData* sd =
            av_packet_side_data_get(cp->coded_side_data, cp->nb_coded_side_data, AV_PKT_DATA_SPHERICAL)) {
        const auto* sph = reinterpret_cast<const AVSphericalMapping*>(sd->data);
        if (sph->projection == AV_SPHERICAL_EQUIRECTANGULAR || sph->projection == AV_SPHERICAL_EQUIRECTANGULAR_TILE) {
            // Bounds crop the sphere (0.32 fixed point): half the width = 180.
            const double cropped = (static_cast<double>(sph->bound_left) + sph->bound_right) / 4294967296.0;
            hint.projection = cropped > 0.4 ? 2 : 1;
        }
    }
    return hint;
}

int stereoFromPicture(const AVFrame* frame, float* scores) {
    if (!frame || frame->width < 64 || frame->height < 36) return -1;
    constexpr int W = 192, H = 108;
    SwsContext* sws = sws_getContext(frame->width, frame->height, static_cast<AVPixelFormat>(frame->format), W, H,
                                     AV_PIX_FMT_GRAY8, SWS_AREA, nullptr, nullptr, nullptr);
    if (!sws) return -1;
    std::vector<uint8_t> g(static_cast<size_t>(W) * H);
    uint8_t* dst[4] = {g.data(), nullptr, nullptr, nullptr};
    int dstStride[4] = {W, 0, 0, 0};
    sws_scale(sws, frame->data, frame->linesize, 0, frame->height, dst, dstStride);
    sws_freeContext(sws);
    auto at = [&](int x, int y) { return static_cast<int>(g[static_cast<size_t>(y) * W + x]); };

    // Detail: mean absolute deviation from the mean (flat / black frames can't tell).
    double mean = 0.0;
    for (uint8_t v : g) mean += v;
    mean /= static_cast<double>(g.size());
    double dev = 0.0;
    for (uint8_t v : g) dev += std::abs(v - mean);
    dev /= static_cast<double>(g.size());
    if (dev < 10.0) return -1;

    // Smallest mean difference between two half-images, trying small
    // horizontal shifts (the disparity between the eyes).
    auto halfDiff = [&](bool sideBySide) {
        const int hw = sideBySide ? W / 2 : W;
        const int hh = sideBySide ? H : H / 2;
        const int ox = sideBySide ? W / 2 : 0;
        const int oy = sideBySide ? 0 : H / 2;
        double best = 1e9;
        for (int s = -6; s <= 6; ++s) {
            double sum = 0.0;
            int n = 0;
            for (int y = 2; y < hh - 2; ++y) {
                for (int x = 8; x < hw - 8; ++x) {
                    sum += std::abs(at(x, y) - at(x + ox + s, y + oy));
                    ++n;
                }
            }
            best = std::min(best, sum / std::max(n, 1));
        }
        return best / dev;  // relative to the picture's own contrast
    };
    const double sbs = halfDiff(true);
    const double tb = halfDiff(false);
    if (scores) {
        scores[0] = static_cast<float>(sbs);
        scores[1] = static_cast<float>(tb);
    }
    // A clear match that clearly beats the other layout.
    // Measured: stereo pairs ~0.05-0.25 against ~1.2-1.6 for the other
    // layout; normal video ~0.7-1.4 for both.
    constexpr double kMatch = 0.30;
    if (sbs < kMatch && sbs * 2.5 < tb) return 1;
    if (tb < kMatch && tb * 2.5 < sbs) return 2;
    return 0;
}

}  // namespace pixelvr

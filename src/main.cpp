#include "app/app.hpp"
#include "app/bundled_ffmpeg.hpp"
#include "util/logging.hpp"
#include "xr/openxr_headers.hpp"

#include <csignal>
#include <string>
#include <vector>

#ifdef PIXELVR_HAVE_MEDIA
#include "media/media_engine.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}
#endif

namespace {

pixelvr::App* g_app = nullptr;

void on_signal(int) {
    if (g_app != nullptr) {
        g_app->requestExit();
    }
}

// Headless: lists the OpenXR runtime's instance extensions (no session needed),
// so we can see which controller render-model extensions are available.
int run_xrext() {
    uint32_t count = 0;
    if (XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, 0, &count,
                                                         nullptr))) {
        PIXELVR_LOG_ERROR("xrext: enumerate failed");
        return 1;
    }
    std::vector<XrExtensionProperties> props(count,
                                             {XR_TYPE_EXTENSION_PROPERTIES});
    if (XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, count, &count,
                                                         props.data()))) {
        PIXELVR_LOG_ERROR("xrext: enumerate(2) failed");
        return 1;
    }
    PIXELVR_LOG_INFO("xrext: %u extensions", count);
    for (const auto& p : props) {
        PIXELVR_LOG_INFO("xrext: %s v%u", p.extensionName, p.extensionVersion);
    }
    return 0;
}

#ifdef PIXELVR_HAVE_MEDIA
// Headless decode probe: opens the media (including smb:// streams) and samples
// a few decoded frames, reporting chroma statistics. Runs without OpenXR so the
// decode/streaming path can be verified on-device without the headset.
int run_probe(const std::string& path) {
    pixelvr::MediaEngine media;
    if (!media.open(path)) {
        PIXELVR_LOG_ERROR("probe: open failed: %s", path.c_str());
        return 1;
    }
    PIXELVR_LOG_INFO("probe: opened %s  %dx%d  %.1fs", path.c_str(), media.width(),
                     media.height(), media.duration());
    pixelvr::VideoFrame frame;
    int got = 0;
    for (int i = 0; i < 600 && got < 5; ++i) {
        if (media.latestFrame(frame) && !frame.uv.empty()) {
            unsigned long sum = 0;
            unsigned mn = 255, mx = 0;
            std::size_t near0 = 0;
            for (std::uint8_t b : frame.uv) {
                sum += b;
                if (b < mn) mn = b;
                if (b > mx) mx = b;
                if (b < 8) ++near0;
            }
            ++got;
            PIXELVR_LOG_INFO(
                "probe: frame %d  %dx%d cs=%d range=%d  chroma min/max/mean=%u/%u/"
                "%.1f near0=%.2f",
                got, frame.width, frame.height, frame.colorspace,
                static_cast<int>(frame.fullRange), mn, mx,
                static_cast<double>(sum) / frame.uv.size(),
                static_cast<double>(near0) / frame.uv.size());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    media.close();
    PIXELVR_LOG_INFO("probe: done (%d frames sampled)", got);
    return got > 0 ? 0 : 2;
}
#endif

} // namespace

#ifdef PIXELVR_HAVE_BROWSER_HELPER
int pvrBrowserMain(int argc, char** argv);  // browser/pvr_browser.cpp
#endif

int main(int argc, char** argv) {
    const std::string first = (argc > 1) ? argv[1] : "";
#ifdef PIXELVR_HAVE_BROWSER_HELPER
    // The browser-screen helper runs as this same executable (see BrowserHost).
    if (first == "--pvr-browser") return pvrBrowserMain(argc - 1, argv + 1);
#endif
    // Release builds: restart on the bundled, patched FFmpeg (zero-copy decode).
    pixelvr::useBundledFfmpeg(argv);

    if (first == "--xrext") {
        return run_xrext();
    }

#ifdef PIXELVR_HAVE_MEDIA
    if (first == "--probe") {
        return run_probe(argc > 2 ? argv[2] : "");
    }
    if (first == "--ffmpeg-info") {  // which FFmpeg this process runs on
        const AVCodec* hw = avcodec_find_decoder_by_name("h264_v4l2m2m");
        const AVClass* cls = hw ? hw->priv_class : nullptr;
        const bool zeroCopy = cls && av_opt_find(&cls, "drm_prime", nullptr, 0, AV_OPT_SEARCH_FAKE_OBJ);
        std::printf("libavcodec %u.%u.%u%s, zero-copy (drm_prime): %s, AV1 (dav1d): %s\n",
                    avcodec_version() >> 16, (avcodec_version() >> 8) & 0xFF, avcodec_version() & 0xFF,
                    pixelvr::usingBundledFfmpeg() ? " (bundled)" : "", zeroCopy ? "yes" : "no",
                    avcodec_find_decoder_by_name("libdav1d") ? "yes" : "no");
        return 0;
    }
#endif

    const std::string mediaPath = first;
    if (pixelvr::usingBundledFfmpeg()) PIXELVR_LOG_INFO("Using the bundled FFmpeg (zero-copy decode)");

    pixelvr::App app;
    g_app = &app;
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    if (!app.init(mediaPath)) {
        PIXELVR_LOG_ERROR("Initialization failed");
        return 1;
    }
    return app.run();
}

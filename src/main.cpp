#include "app/app.hpp"
#include "util/logging.hpp"
#include "xr/openxr_headers.hpp"

#include <csignal>
#include <string>
#include <vector>

#ifdef PIXELVR_HAVE_MEDIA
#include "media/media_engine.hpp"

#include <chrono>
#include <cstdint>
#include <thread>
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

int main(int argc, char** argv) {
    const std::string first = (argc > 1) ? argv[1] : "";

    if (first == "--xrext") {
        return run_xrext();
    }

#ifdef PIXELVR_HAVE_MEDIA
    if (first == "--probe") {
        return run_probe(argc > 2 ? argv[2] : "");
    }
#endif

    const std::string mediaPath = first;

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

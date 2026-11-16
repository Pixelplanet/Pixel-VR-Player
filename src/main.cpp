#include <unistd.h>
#include "app/app.hpp"
#include "app/bundled_ffmpeg.hpp"
#include "app/vault_manager.hpp"
#include "util/logging.hpp"
#include "xr/vr_app_manifest.hpp"
#include "xr/openxr_headers.hpp"

#include <csignal>
#include <string>
#include <vector>

#ifdef PIXELVR_HAVE_MEDIA
#include "media/media_engine.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
// Writes a subtitle picture as a .ppm (laid over mid grey, so white letters and their dark outline both
// show) for looking at what was drawn. `covered` gets the number of pixels that are not transparent.
bool dumpSubtitlePicture(const pixelvr::MediaEngine::SubtitlePicture& pic, const std::string& file, size_t& covered) {
    covered = 0;
    const size_t n = static_cast<size_t>(pic.w) * pic.h;
    std::vector<std::uint8_t> rgb(n * 3);
    for (size_t i = 0; i < n && i * 4 + 3 < pic.rgba.size(); ++i) {
        const unsigned a = pic.rgba[i * 4 + 3];
        if (a > 0) ++covered;
        for (int c = 0; c < 3; ++c) rgb[i * 3 + c] = static_cast<std::uint8_t>((pic.rgba[i * 4 + c] * a + 128u * (255 - a)) / 255);
    }
    if (file.empty()) return true;
    std::FILE* f = std::fopen(file.c_str(), "wb");
    if (f == nullptr) return false;
    std::fprintf(f, "P6\n%d %d\n255\n", pic.w, pic.h);
    const bool ok = std::fwrite(rgb.data(), 1, rgb.size(), f) == rgb.size();
    std::fclose(f);
    return ok;
}

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
    // PIXELVR_PROBE_SEEK=<seconds>: jump there first (as a resumed film does).
    if (const char* seekEnv = std::getenv("PIXELVR_PROBE_SEEK")) media.seek(std::atof(seekEnv));
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
    // Subtitle files outside the file (next to a video on a network share): read here at once, the way
    // the player's worker does after the start. PIXELVR_PROBE_SUB_URL=<http address>: one more, fetched
    // like a media server's external subtitle (PIXELVR_PROBE_SUB_CODEC: its format, srt when not given).
    {
        std::vector<pixelvr::SubtitleSource> extra = pixelvr::subsources::findNextTo(path);
        if (const char* url = std::getenv("PIXELVR_PROBE_SUB_URL")) {
            pixelvr::ServerSubtitle s;
            s.id = 1;
            s.url = url;
            const char* codec = std::getenv("PIXELVR_PROBE_SUB_CODEC");
            s.codec = codec ? codec : "srt";
            for (auto& f : pixelvr::subsources::fetchFromServer({s}, 1)) extra.push_back(std::move(f));
        }
        for (const auto& s : extra) {
            pixelvr::MediaEngine::StreamInfo info;
            info.label = s.label;
            info.language = s.language;
            info.flags = s.flags;
            const int index = media.addSubtitleData(info, s.fileName, s.data);
            PIXELVR_LOG_INFO("probe: subtitle file from outside: %zu bytes -> track %d", s.data ? s.data->size() : 0, index);
        }
    }
    // Subtitles: every track, and the first line each usable one shows within the first seconds.
    for (const auto& st : media.subtitleStreams()) {
        PIXELVR_LOG_INFO("probe: subtitle track %d  [%s] %s%s", st.index, st.language.c_str(), st.label.c_str(),
                         st.usable ? "" : "  (not usable)");
        if (!st.usable) continue;
        media.setSubtitleStream(st.index);
        // PIXELVR_PROBE_SUB_AT=<seconds>: look there instead of at the start (films start without lines).
        const char* atEnv = std::getenv("PIXELVR_PROBE_SUB_AT");
        const double base = atEnv ? std::atof(atEnv) : 0.0;
        // PIXELVR_PROBE_SUB_FRAME=<width>x<height>: the visible picture text subtitles are laid out in.
        int frameW = 0, frameH = 0;
        if (const char* fr = std::getenv("PIXELVR_PROBE_SUB_FRAME"); fr && std::sscanf(fr, "%dx%d", &frameW, &frameH) == 2) {
            media.setSubtitleFrame(frameW, frameH);
        }
        // PIXELVR_PROBE_SUB_DUMP=<folder>: every picture found below is written there as a .ppm.
        const char* dumpDir = std::getenv("PIXELVR_PROBE_SUB_DUMP");
        media.seek(base);
        // Let it play for a few seconds (frames are taken and dropped), then ask for some moments.
        pixelvr::DrmVideoFrame drm;
        for (int i = 0; i < 280; ++i) {
            media.latestFrame(frame);
            if (media.latestDrmFrame(drm) && drm.fd >= 0) {
                ::close(drm.fd);
                drm.fd = -1;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        // (0.0: what began before the seek target and is still on there)
        for (double off : {0.0, 1.7, 2.5, 3.5, 4.5, 5.5, 6.5}) {
            const double t = base + off;
            std::string text;
            media.subtitleAt(t, text, true);
            if (const auto pic = media.subtitlePictureAt(t, true)) {
                // A text track drawn by libass gives both: the picture's size and place, and the plain words.
                std::string what = "picture " + std::to_string(pic->w) + "x" + std::to_string(pic->h) + " at " +
                                   std::to_string(pic->x) + "," + std::to_string(pic->y) + " on " + std::to_string(pic->canvasW) +
                                   "x" + std::to_string(pic->canvasH);
                size_t covered = 0;
                std::string file;
                if (dumpDir != nullptr) {
                    char name[96];
                    std::snprintf(name, sizeof(name), "/sub-%d-%07d.ppm", st.index, static_cast<int>(t * 1000.0));
                    file = std::string(dumpDir) + name;
                }
                const bool wrote = dumpSubtitlePicture(*pic, file, covered);
                what += ", " + std::to_string(covered) + " px drawn";
                if (!file.empty()) what += wrote ? " -> " + file : " (could not write " + file + ")";
                text = text.empty() ? what : what + "  \"" + text + "\"";
            }
            for (char& c : text) {
                if (c == '\n') c = '|';
            }
            PIXELVR_LOG_INFO("probe:   at %.1f s (playing %.1f s): %s", t, media.position(), text.empty() ? "-" : text.c_str());
        }
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
    // Short-lived helper started by App::init (see vr_app_manifest.hpp).
    if (first == "--steamvr-prelaunch") return pixelvr::runSteamVrPrelaunchHelper();
    if (first == "--steamvr-unregister") return pixelvr::runSteamVrUnregister();
    // install.sh --uninstall: where the vault is and how big; decrypting it into a normal folder.
    if (first == "--vault-info") return pixelvr::VaultManager::runCli("info", "");
    if (first == "--vault-export") return pixelvr::VaultManager::runCli("export", argc > 2 ? argv[2] : "");
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

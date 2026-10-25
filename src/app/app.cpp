#include "app/app.hpp"
#include "app/vault_manager.hpp"
#include "net/smb_credentials.hpp"
#include "xr/vr_keyboard.hpp"
#include "util/ipv4.hpp"

#include "util/logging.hpp"

#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace pixelvr {

namespace {

// Quaternion/pose helpers (xyzw) for composing controller-model placement.
void quatRotate(const float q[4], const float v[3], float o[3]) {
    const float ux = q[0], uy = q[1], uz = q[2], w = q[3];
    const float c1x = uy * v[2] - uz * v[1] + w * v[0];
    const float c1y = uz * v[0] - ux * v[2] + w * v[1];
    const float c1z = ux * v[1] - uy * v[0] + w * v[2];
    o[0] = v[0] + 2.0f * (uy * c1z - uz * c1y);
    o[1] = v[1] + 2.0f * (uz * c1x - ux * c1z);
    o[2] = v[2] + 2.0f * (ux * c1y - uy * c1x);
}
void quatMul(const float a[4], const float b[4], float o[4]) {
    o[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    o[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    o[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    o[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}
// out = A ∘ B composed as q = qA*qB, t = qA*tB + tA.
void composePose(const float pa[3], const float qa[4], const float pb[3],
                 const float qb[4], float po[3], float qo[4]) {
    quatMul(qa, qb, qo);
    float r[3];
    quatRotate(qa, pb, r);
    po[0] = r[0] + pa[0];
    po[1] = r[1] + pa[1];
    po[2] = r[2] + pa[2];
}

// Normalized lerp between quaternions (shortest path) for pose smoothing.
void quatNlerp(const float a[4], const float b[4], float t, float o[4]) {
    const float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    const float s = dot < 0.0f ? -1.0f : 1.0f;
    float len = 0.0f;
    for (int i = 0; i < 4; ++i) {
        o[i] = a[i] * (1.0f - t) + b[i] * s * t;
        len += o[i] * o[i];
    }
    len = std::sqrt(len);
    if (len > 1e-6f)
        for (int i = 0; i < 4; ++i) o[i] /= len;
}

std::string to_lower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

bool contains(const std::string& s, const char* token) {
    return s.find(token) != std::string::npos;
}

// True if `needle` appears not immediately preceded or followed by a digit, so a marker like
// "360" matches but a resolution like "3600" (or "3600p") does not.
bool contains_num(const std::string& s, const char* needle) {
    const std::string n = needle;
    std::size_t pos = 0;
    while ((pos = s.find(n, pos)) != std::string::npos) {
        const bool prevDigit = (pos > 0 && s[pos - 1] >= '0' && s[pos - 1] <= '9');
        const std::size_t after = pos + n.size();
        const bool nextDigit = (after < s.size() && s[after] >= '0' && s[after] <= '9');
        if (!prevDigit && !nextDigit) return true;
        pos = (after > pos ? after : pos + 1);
    }
    return false;
}

// Returns true if filename or path patterns indicate VR180 projection
bool is_vr180_text(const std::string& lower) {
    if (contains(lower, "vr180") || contains(lower, "180vr") ||
        contains(lower, "180x180") || contains(lower, "fisheye") ||
        contains_num(lower, "180")) {
        return true;
    }

    // VR resolution tokens commonly used for VR180 dual fisheye (vertical resolution or full frame)
    const bool hasVrRes = contains_num(lower, "3600") || contains(lower, "3600p") ||
                          contains(lower, "-3600") || contains(lower, "_3600") || contains(lower, ".3600") ||
                          contains_num(lower, "2880") || contains(lower, "2880p") ||
                          contains(lower, "-2880") || contains(lower, "_2880") || contains(lower, ".2880") ||
                          contains_num(lower, "2700") || contains(lower, "2700p") ||
                          contains(lower, "-2700") || contains(lower, "_2700") || contains(lower, ".2700") ||
                          contains_num(lower, "3000") || contains(lower, "3000p") ||
                          contains_num(lower, "5400") || contains(lower, "5400p") ||
                          contains_num(lower, "5760") || contains(lower, "5760p") ||
                          contains(lower, "7200x3600") || contains(lower, "5760x2880") ||
                          contains(lower, "5400x2700") || contains(lower, "6000x3000");

    // Well-known VR video studios / naming patterns
    const bool hasVrStudio = contains(lower, "wvr") || contains(lower, "wankz") ||
                             contains(lower, "czechvr") || contains(lower, "vrhush") ||
                             contains(lower, "vrbangers") || contains(lower, "realjamvr") ||
                             contains(lower, "badoinkvr") || contains(lower, "badoink") ||
                             contains(lower, "milfvr") || contains(lower, "povr") ||
                             contains(lower, "vrcosplay") || contains(lower, "vrsmash") ||
                             contains(lower, "vrconk") || contains(lower, "slr_") ||
                             contains(lower, "slr-") || contains(lower, "_slr_") ||
                             contains(lower, "-slr-") || contains(lower, ".slr.") ||
                             contains(lower, "swallowvr") || contains(lower, "stripzvr") ||
                             contains(lower, "twistysvr") || contains(lower, "vrphoria") ||
                             contains(lower, "kinkvr") || contains(lower, "lustreality") ||
                             contains(lower, "joymiivr") || contains(lower, "naughtyamericavr");

    const bool hasGenericVr = contains(lower, "_vr_") || contains(lower, "-vr-") ||
                              contains(lower, ".vr.") || contains(lower, " vr ");

    // Check if filename explicitly says 360 (which overrides VR studio / VR generic marker)
    const bool isExplicit360 = contains_num(lower, "360") || contains(lower, "equirect") ||
                               contains(lower, "vr360") || contains(lower, "360vr") ||
                               contains(lower, "360x180");

    if (!isExplicit360 && (hasVrRes || hasVrStudio || (hasGenericVr && (hasVrRes || contains(lower, "3d") || contains(lower, "sbs"))))) {
        return true;
    }

    return false;
}

bool is_vr360_text(const std::string& lower) {
    if (contains(lower, "vr180") || contains(lower, "180vr") ||
        contains(lower, "180x180") || contains(lower, "fisheye") ||
        contains_num(lower, "180")) {
        return false;
    }
    if (contains_num(lower, "360") || contains(lower, "equirect") ||
        contains(lower, "vr360") || contains(lower, "360vr") ||
        contains(lower, "360x180") || contains(lower, "sphere")) {
        return true;
    }
    return false;
}

bool is_vr180_media(const std::string& pathOrName, int w, int h) {
    const std::string lower = to_lower(pathOrName);
    if (is_vr180_text(lower)) return true;

    // Check dimensions if probed (dual fisheye SBS aspect ~2.0:1)
    if (w > 0 && h > 0) {
        const float ar = static_cast<float>(w) / static_cast<float>(h);
        const bool is360 = is_vr360_text(lower);
        if (!is360) {
            // Exact signature VR180 dual fisheye resolutions
            if ((w == 7200 && h == 3600) || (w == 5760 && h == 2880) ||
                (w == 5400 && h == 2700) || (w == 6000 && h == 3000) ||
                (w == 4800 && h == 2400) || (w == 7680 && h == 3840 && (contains(lower, "vr") || contains(lower, "180")))) {
                return true;
            }
            // General 2:1 dual fisheye with vertical resolution >= 2600 (not standard flat)
            if (h >= 2600 && ar >= 1.90f && ar <= 2.10f) {
                return true;
            }
            // If name has any vr marker or 3d and ar is ~2.0
            if ((contains(lower, "vr") || contains(lower, "3d")) && ar >= 1.85f && ar <= 2.15f) {
                return true;
            }
        }
    }
    return false;
}

bool is_vr360_media(const std::string& pathOrName, int w, int h) {
    const std::string lower = to_lower(pathOrName);
    if (is_vr180_media(pathOrName, w, h)) return false;
    return is_vr360_text(lower);
}

// Infers projection and stereo layout from common filename conventions, e.g.
// "clip_360_tb.mp4", "movie_180_sbs.mkv", "trailer_vr180.mp4". `stereoFromName`
// reports whether the stereo layout was stated explicitly (so the caller can
// refine it from the frame aspect ratio otherwise).
void detect_format(const std::string& path, VulkanRenderer::ProjectionMode& proj,
                   VulkanRenderer::StereoMode& stereo, bool& stereoFromName) {
    const std::string p = to_lower(path);

    if (is_vr180_text(p)) {
        proj = VulkanRenderer::ProjectionMode::Equirect180;
    } else if (is_vr360_text(p)) {
        proj = VulkanRenderer::ProjectionMode::Equirect360;
    } else {
        proj = VulkanRenderer::ProjectionMode::Flat;
    }

    stereoFromName = true;
    if (contains(p, "_tb") || contains(p, "-tb") || contains(p, "_ou") ||
        contains(p, "overunder") || contains(p, "over-under") ||
        contains(p, "htab") || contains(p, "h-tab")) {
        stereo = VulkanRenderer::StereoMode::TopBottom;
    } else if (contains(p, "sbs") || contains(p, "_lr") ||
               contains(p, "sidebyside") || contains(p, "side-by-side") ||
               contains(p, "hsbs") || contains(p, "half-sbs")) {
        stereo = VulkanRenderer::StereoMode::SideBySide;  // incl. HSBS / half-SBS
    } else if (contains(p, "3d")) {
        // Bare "3D" with no explicit layout: half-SBS dominates movie rips.
        stereo = VulkanRenderer::StereoMode::SideBySide;
    } else if (proj == VulkanRenderer::ProjectionMode::Equirect180) {
        // VR180 almost universally defaults to SideBySide 3D
        stereo = VulkanRenderer::StereoMode::SideBySide;
        stereoFromName = false;  // allow refining from aspect ratio once frame is opened
    } else {
        stereo = VulkanRenderer::StereoMode::Mono;
        stereoFromName = false;  // no explicit layout; refine from aspect ratio
    }
}

} // namespace

bool App::init(const std::string& mediaPath) {
    if (!xr_.createInstanceAndSystem("Pixel VR Player")) {
        return false;
    }
    if (!renderer_.createDevice(xr_)) {
        return false;
    }
    if (!xr_.createSession(renderer_.graphicsBinding())) {
        return false;
    }
    if (!xr_.createReferenceSpace()) {
        return false;
    }
    if (!xr_.createActions()) {
        PIXELVR_LOG_WARN("Controller input unavailable; continuing without it");
    }
    renderModels_.init(xr_.instance(), xr_.session(), xr_.renderModelEnabled());

    const std::vector<int64_t> formats = xr_.enumerateSwapchainFormats();
    if (formats.empty()) {
        PIXELVR_LOG_ERROR("Runtime reported no swapchain formats");
        return false;
    }
    colorFormat_ = renderer_.chooseColorFormat(formats);
    if (!xr_.createSwapchains(colorFormat_, settings_.resScale())) {
        return false;
    }
    if (!renderer_.createRenderResources(xr_.swapchains())) {
        return false;
    }

#ifdef PIXELVR_HAVE_MEDIA
    if (!mediaPath.empty()) {
        std::error_code ec;
        if (std::filesystem::is_directory(mediaPath, ec)) {
#ifdef PIXELVR_HAVE_FREETYPE
            browser_.openPath(mediaPath);
#endif
        } else {
            loadMedia(mediaPath);
        }
    }
#else
    (void)mediaPath;
#endif
    // No clip requested: open the file browser instead of a test video.
    if (mediaPath.empty()) {
#ifdef PIXELVR_HAVE_FREETYPE
        browser_.open();
#endif
    }

    PIXELVR_LOG_INFO("Initialization complete");
    return true;
}

void App::applyResolutionScale(float scale) {
    if (colorFormat_ == 0) return;
    PIXELVR_LOG_INFO("Applying resolution scale %.2fx...", scale);
    renderer_.waitIdle();
    if (!xr_.createSwapchains(colorFormat_, scale)) {
        PIXELVR_LOG_ERROR("Failed to recreate OpenXR swapchains with scale %.2fx", scale);
        return;
    }
    if (!renderer_.recreateSwapchainTargets(xr_.swapchains())) {
        PIXELVR_LOG_ERROR("Failed to recreate Vulkan render targets for swapchain");
        return;
    }
    PIXELVR_LOG_INFO("Resolution scale applied successfully: %dx%d per eye",
                     xr_.swapchains().empty() ? 0 : xr_.swapchains()[0].width,
                     xr_.swapchains().empty() ? 0 : xr_.swapchains()[0].height);
}

#ifdef PIXELVR_HAVE_MEDIA
void App::loadMedia(const std::string& path) {
    const bool sameFile = (path == currentMediaPath_);
    currentMediaPath_ = path;
    VulkanRenderer::ProjectionMode proj = VulkanRenderer::ProjectionMode::Flat;
    VulkanRenderer::StereoMode stereo = VulkanRenderer::StereoMode::Mono;
    bool stereoFromName = false;
    detect_format(path, proj, stereo, stereoFromName);

    renderer_.releaseVideoFrame();  // free the previous clip's dma-buf before reopening
    renderer_.setSphereZoom(1.0f);  // reset the 360/180 viewpoint per clip
    renderer_.setSpherePitch(0.0f);
    media_.setPreferredAudioLanguage(settings_.audioLanguage());
    if (!sameFile) {  // a new file starts from automatic track selection
        media_.setPreferredVideoStream(-1);
        media_.setPreferredAudioStream(-1);
    }
    mediaOpen_ = media_.open(path);
    if (!mediaOpen_) {
        // Clear stale video from the previous clip so the renderer shows black
        // instead of the last decoded frame of the old file.
        renderer_.clearVideo();
        PIXELVR_LOG_WARN("Failed to open %s", path.c_str());
        return;
    }
    hudDismissed_ = false;
    lastControllerActivity_ = std::chrono::steady_clock::now();
    VrKeyboard::instance().hide();
    actionMenu_.visible = false;
    smbModal_.active = false;
    smbModal_.editingField = SmbModalState::Field::None;
    browser_.setOpen(false);
    downloadsWindow_ = false;
    pendingDl_.clear();

    // Refine the stereo layout from the frame aspect ratio when the filename did
    // not state it: once the projection is a VR type the aspect is unambiguous
    // (360 mono 2:1, SBS 4:1, OU 1:1; 180 mono 1:1, SBS 2:1, OU 1:2).
    const int w = media_.width();
    const int h = media_.height();

    // If filename did not indicate VR, check if probed dimensions reveal VR180
    if (proj == VulkanRenderer::ProjectionMode::Flat && h > 0) {
        if (is_vr180_media(path, w, h)) {
            proj = VulkanRenderer::ProjectionMode::Equirect180;
            if (!stereoFromName) {
                stereo = VulkanRenderer::StereoMode::SideBySide;
            }
        }
    }

    if (!stereoFromName && h > 0) {
        const float ar = static_cast<float>(w) / static_cast<float>(h);
        if (proj == VulkanRenderer::ProjectionMode::Equirect360) {
            stereo = ar >= 3.0f   ? VulkanRenderer::StereoMode::SideBySide
                     : ar <= 1.3f ? VulkanRenderer::StereoMode::TopBottom
                                  : VulkanRenderer::StereoMode::Mono;
        } else if (proj == VulkanRenderer::ProjectionMode::Equirect180) {
            stereo = ar >= 1.6f   ? VulkanRenderer::StereoMode::SideBySide
                     : ar <= 0.7f ? VulkanRenderer::StereoMode::TopBottom
                                  : VulkanRenderer::StereoMode::Mono;
        } else {  // Flat: full-frame SBS roughly doubles the width.
            stereo = ar >= 2.9f ? VulkanRenderer::StereoMode::SideBySide
                                : VulkanRenderer::StereoMode::Mono;
        }
    }

    renderer_.setProjectionMode(proj);
    renderer_.setStereoMode(stereo);
    settings_.setProjectionMode(static_cast<int>(proj));
    settings_.setStereoMode(static_cast<int>(stereo));
    // Publish selectable tracks for the settings Audio/Video section.
    std::vector<std::pair<int, std::string>> vlist, alist;
    for (const auto& s : media_.videoStreams()) vlist.emplace_back(s.index, s.label);
    for (const auto& s : media_.audioStreams()) alist.emplace_back(s.index, s.label);
    settings_.setStreams(vlist, alist, media_.currentVideoStream(),
                         media_.currentAudioStream());
    static const char* const kProj[] = {"flat", "360", "180"};
    static const char* const kStereo[] = {"mono", "SBS", "TB"};
    PIXELVR_LOG_INFO("Loaded %s (%dx%d, %s/%s)", path.c_str(), w, h,
                     kProj[static_cast<int>(proj)],
                     kStereo[static_cast<int>(stereo)]);
}

void App::switchTracks(bool byLanguage) {
    if (currentMediaPath_.empty() || !mediaOpen_) return;
    const double pos = media_.position();
    if (byLanguage) {
        media_.setPreferredAudioLanguage(settings_.audioLanguage());
        media_.setPreferredAudioStream(-1);
    } else {
        media_.setPreferredAudioLanguage("");
        media_.setPreferredAudioStream(settings_.selectedAudioStream());
    }
    media_.setPreferredVideoStream(settings_.selectedVideoStream());
    loadMedia(currentMediaPath_);  // same file: keeps the preferences set above
    if (pos > 0.5 && media_.duration() > 0.0) media_.seek(pos);
}

void App::updateStats() {
    using clock = std::chrono::steady_clock;
    const auto now = clock::now();
    ++statsFrameAccum_;
    if (lastStatsUpdate_.time_since_epoch().count() == 0) {
        lastStatsUpdate_ = now;
        statsLastDecoded_ = media_.decodedFrames();
        return;
    }
    const double elapsed =
        std::chrono::duration<double>(now - lastStatsUpdate_).count();
    if (elapsed < 0.5) {
        return;  // refresh the readout ~twice a second
    }

    const double fps = statsFrameAccum_ / elapsed;
    const uint64_t dec = media_.decodedFrames();
    if (dec >= statsLastDecoded_) {
        const double decFps =
            static_cast<double>(dec - statsLastDecoded_) / elapsed;
        decFpsEma_ = (decFpsEma_ > 0.0) ? decFpsEma_ * 0.6 + decFps * 0.4 : decFps;
    } else {
        decFpsEma_ = 0.0;  // decodedFrames reset on a new clip: restart the average
    }
    statsFrameAccum_ = 0;
    statsLastDecoded_ = dec;
    lastStatsUpdate_ = now;

    // Aggregate CPU load from /proc/stat deltas.
    float cpuPct = -1.0f;
    {
        std::ifstream f("/proc/stat");
        std::string cpu;
        unsigned long long u = 0, n = 0, s = 0, idle = 0, io = 0, irq = 0,
                           sirq = 0, steal = 0;
        if (f >> cpu >> u >> n >> s >> idle >> io >> irq >> sirq >> steal) {
            const unsigned long long idleAll = idle + io;
            const unsigned long long total =
                u + n + s + idle + io + irq + sirq + steal;
            if (cpuPrevTotal_ != 0 && total > cpuPrevTotal_) {
                const unsigned long long dtot = total - cpuPrevTotal_;
                const unsigned long long didle = idleAll - cpuPrevIdle_;
                cpuPct = 100.0f * static_cast<float>(dtot - didle) /
                         static_cast<float>(dtot);
            }
            cpuPrevTotal_ = total;
            cpuPrevIdle_ = idleAll;
        }
    }

    // RAM usage from /proc/meminfo.
    long memTotalKb = 0, memAvailKb = 0;
    {
        std::ifstream f("/proc/meminfo");
        std::string key, unit;
        long val = 0;
        while (f >> key >> val >> unit) {
            if (key == "MemTotal:") memTotalKb = val;
            else if (key == "MemAvailable:") { memAvailKb = val; break; }
        }
    }

    // Adreno GPU: the Frame runs the Turnip/msm driver (no kgsl sysfs), so report
    // the GPU clock from devfreq (the governor scales it with load; pegged at max
    // means GPU-bound).
    long gpuCurMhz = 0, gpuMaxMhz = 0;
    {
        std::ifstream cf("/sys/class/devfreq/3d00000.gpu/cur_freq");
        std::ifstream mf("/sys/class/devfreq/3d00000.gpu/max_freq");
        long cur = 0, mx = 0;
        if (cf >> cur) gpuCurMhz = cur / 1000000;
        if (mf >> mx) gpuMaxMhz = mx / 1000000;
    }

    char buf[160];
    statsLines_.clear();
    std::snprintf(buf, sizeof(buf), "FPS %.1f   (%.1f ms)", fps,
                  fps > 0.0 ? 1000.0 / fps : 0.0);
    statsLines_.emplace_back(buf);
    if (!xr_.swapchains().empty()) {
        std::snprintf(buf, sizeof(buf), "Render %dx%d (%.2fx)",
                      xr_.swapchains()[0].width, xr_.swapchains()[0].height,
                      settings_.resScale());
        statsLines_.emplace_back(buf);
    }
    if (mediaOpen_) {
        std::snprintf(buf, sizeof(buf), "Video %dx%d  %s", media_.width(),
                      media_.height(), media_.codecName().c_str());
        statsLines_.emplace_back(buf);
        std::snprintf(buf, sizeof(buf), "Decode %.1f / %.2f fps", decFpsEma_,
                      media_.frameRate());
        statsLines_.emplace_back(buf);
    }
    if (cpuPct >= 0.0f) {
        std::snprintf(buf, sizeof(buf), "CPU %.0f%%", cpuPct);
        statsLines_.emplace_back(buf);
    }
    if (gpuMaxMhz > 0) {
        std::snprintf(buf, sizeof(buf), "GPU %ld/%ld MHz", gpuCurMhz, gpuMaxMhz);
    } else {
        std::snprintf(buf, sizeof(buf), "GPU n/a");
    }
    statsLines_.emplace_back(buf);
    if (memTotalKb > 0) {
        std::snprintf(buf, sizeof(buf), "RAM %ld / %ld MB",
                      (memTotalKb - memAvailKb) / 1024, memTotalKb / 1024);
        statsLines_.emplace_back(buf);
    }
}
#endif

int App::navStep(float axisY, std::chrono::steady_clock::time_point now) {
    using namespace std::chrono_literals;
    if (axisY > 0.38f || axisY < -0.38f) {
        const int dir = (axisY > 0.0f) ? -1 : 1;  // push up = toward the top row
        if (navArmed_) {  // first press: step now, then pause before repeating
            navArmed_ = false;
            navNextRepeat_ = now + 350ms;
            return dir;
        }
        if (now >= navNextRepeat_) {  // held down: auto-repeat
            navNextRepeat_ = now + 120ms;
            return dir;
        }
        return 0;
    }
    if (axisY > -0.20f && axisY < 0.20f) navArmed_ = true;  // released: re-arm
    return 0;
}

int App::navStepX(float axisX, std::chrono::steady_clock::time_point now) {
    using namespace std::chrono_literals;
    if (axisX > 0.38f || axisX < -0.38f) {
        const int dir = (axisX > 0.0f) ? 1 : -1;  // push right = next col, push left = prev col
        if (navArmedX_) {
            navArmedX_ = false;
            navNextRepeatX_ = now + 350ms;
            return dir;
        }
        if (now >= navNextRepeatX_) {
            navNextRepeatX_ = now + 140ms;
            return dir;
        }
        return 0;
    }
    if (axisX > -0.20f && axisX < 0.20f) navArmedX_ = true;
    return 0;
}

void App::performVaultLock() {
    VaultManager::instance().lock();
    renderer_.setVaultUnlocked(false);
    actionMenu_.visible = false;
    renderer_.setActionMenu({});

#ifdef PIXELVR_HAVE_MEDIA
    if (mediaOpen_ && VaultManager::instance().isVaultPath(currentMediaPath_)) {
        media_.close();
        mediaOpen_ = false;
        renderer_.clearVideo();
    }
#endif

#ifdef PIXELVR_HAVE_FREETYPE
    if (browser_.inVault()) {
        browser_.openHome();
    }
    browser_.clearFinished();
    downloadToast_ = {};
    downloadToastText_.clear();
    renderer_.clearCardThumbnails();
    thumbnailCache_.clearVaultThumbs(VaultManager::instance().vaultDir());
#endif

    if (settings_.section() == 5) {
        settings_.setSection(0);
    }
    settings_.refresh();

    PIXELVR_LOG_INFO("Vault locked and sanitized");
}

void App::dismissHud(const XrInputState& in, std::chrono::steady_clock::time_point nowT) {
    hudDismissed_ = true;
    hudDismissTime_ = nowT;
    lastControllerActivity_ = nowT - std::chrono::seconds(10);
    renderer_.setControlBar(false);
    for (int h = 0; h < 2; ++h) {
        if (in.controllers[h].active) {
            for (int k = 0; k < 3; ++k) activityAnchorPos_[h][k] = in.controllers[h].position[k];
            for (int k = 0; k < 4; ++k) activityAnchorOrient_[h][k] = in.controllers[h].orientation[k];
        }
    }
    PIXELVR_LOG_INFO("Player HUD dismissed");
}

void App::openSmbModal(bool isNew, const std::string& smbPath) {
#ifdef PIXELVR_HAVE_FREETYPE
    smbModal_.active = true;
    smbModal_.isNew = isNew;
    smbModal_.editingField = SmbModalState::Field::None;

    if (isNew) {
        smbModal_.origHost.clear();
        smbModal_.origShare.clear();
        smbModal_.host.clear();
        smbModal_.share.clear();
        smbModal_.anon = false;
        smbModal_.user.clear();
        smbModal_.pass.clear();
        smbModal_.workgroup = "WORKGROUP";
        smbModal_.label.clear();
    } else {
        std::string rest = smbPath;
        if (rest.rfind("smb://", 0) == 0) rest = rest.substr(6);
        while (!rest.empty() && rest.back() == '/') rest.pop_back();
        auto slash = rest.find('/');
        std::string host = (slash == std::string::npos) ? rest : rest.substr(0, slash);
        std::string share = (slash == std::string::npos) ? "" : rest.substr(slash + 1);

        smbModal_.origHost = host;
        smbModal_.origShare = share;
        smbModal_.host = host;
        smbModal_.share = share;
        smbModal_.anon = false;
        smbModal_.user.clear();
        smbModal_.pass.clear();
        smbModal_.workgroup = "WORKGROUP";
        smbModal_.label.clear();

        SmbShare existing;
        if (SmbCredentialsManager::instance().findShare(host, share, existing)) {
            smbModal_.anon = existing.anonymous;
            smbModal_.user = existing.username;
            smbModal_.pass = existing.password;
            smbModal_.workgroup = existing.workgroup.empty() ? "WORKGROUP" : existing.workgroup;
            smbModal_.label = existing.name;
        }
    }

    actionMenu_.visible = true;
    actionMenu_.page = 4;
    actionMenu_.selectedOption = 0;
    updateSmbModalMenu();
#endif
}

void App::updateSmbModalMenu() {
#ifdef PIXELVR_HAVE_FREETYPE
    actionMenu_.title = smbModal_.isNew ? "Add SMB Network Share" : "Edit SMB Network Share";
    actionMenu_.subtitle = "Point & click a field to enter text via keyboard";
    actionMenu_.options.clear();
    actionMenu_.editingRow = -1;
    actionMenu_.cursorPos = 0;

    const bool kbActive = VrKeyboard::instance().isOpen();
    const std::string liveText = VrKeyboard::instance().currentText();
    const int liveCur = VrKeyboard::instance().cursorPos();

    auto fieldVal = [&](SmbModalState::Field f, int rowIdx, const std::string& saved, const std::string& placeholder, bool mask) {
        if (kbActive && smbModal_.editingField == f) {
            actionMenu_.editingRow = rowIdx;
            actionMenu_.cursorPos = liveCur;
            return mask ? std::string(liveText.length(), '*') : liveText;
        }
        if (saved.empty()) return placeholder;
        return mask ? std::string(saved.length(), '*') : saved;
    };

    actionMenu_.options.push_back("Server / Host: " + fieldVal(SmbModalState::Field::Host, 0, smbModal_.host, "[ Enter IP or Hostname ]", false));
    actionMenu_.options.push_back("Share Name: " + fieldVal(SmbModalState::Field::Share, 1, smbModal_.share, "[ All Shares ]", false));
    actionMenu_.options.push_back("Authentication: " + std::string(smbModal_.anon ? "Guest / Anonymous" : "Username & Password"));
    actionMenu_.options.push_back("Username: " + fieldVal(SmbModalState::Field::User, 3, smbModal_.user, "[ Enter Username ]", false));
    actionMenu_.options.push_back("Password: " + fieldVal(SmbModalState::Field::Pass, 4, smbModal_.pass, "[ Enter Password ]", true));
    actionMenu_.options.push_back("Workgroup: " + fieldVal(SmbModalState::Field::Workgroup, 5, smbModal_.workgroup, "WORKGROUP", false));
    actionMenu_.options.push_back("Nickname: " + fieldVal(SmbModalState::Field::Label, 6, smbModal_.label, "[ Optional Nickname ]", false));

    actionMenu_.options.push_back("[ Save SMB Share ]");
    if (!smbModal_.isNew) {
        actionMenu_.options.push_back("[ Delete Share ]");
    }
    actionMenu_.options.push_back("[ Cancel ]");
#endif
}

void App::saveSmbModal() {
#ifdef PIXELVR_HAVE_FREETYPE
    while (!smbModal_.host.empty() &&
           (std::isspace(static_cast<unsigned char>(smbModal_.host.front())) ||
            smbModal_.host.front() == '/' || smbModal_.host.front() == '\\')) {
        smbModal_.host.erase(smbModal_.host.begin());
    }
    while (!smbModal_.host.empty() &&
           (std::isspace(static_cast<unsigned char>(smbModal_.host.back())) ||
            smbModal_.host.back() == '/' || smbModal_.host.back() == '\\')) {
        smbModal_.host.pop_back();
    }
    if (smbModal_.host.empty()) {
        actionMenu_.subtitle = "Error: Server IP or Hostname is required!";
        return;
    }
    if (!smbModal_.isNew && (smbModal_.host != smbModal_.origHost || smbModal_.share != smbModal_.origShare)) {
        SmbCredentialsManager::instance().deleteShare(smbModal_.origHost, smbModal_.origShare);
    }
    SmbShare s;
    s.host = smbModal_.host;
    s.share = smbModal_.share;
    s.username = smbModal_.user;
    s.password = smbModal_.pass;
    s.workgroup = smbModal_.workgroup.empty() ? "WORKGROUP" : smbModal_.workgroup;
    s.name = smbModal_.label.empty() ? (s.host + (s.share.empty() ? "" : ("/" + s.share))) : smbModal_.label;
    s.id = s.host + (s.share.empty() ? "" : ("/" + s.share));
    s.anonymous = smbModal_.anon;

    SmbCredentialsManager::instance().addOrUpdateShare(s);

    smbModal_.active = false;
    smbModal_.editingField = SmbModalState::Field::None;
    VrKeyboard::instance().hide();
    actionMenu_.visible = false;
    browser_.openNetwork();
#endif
}

void App::deleteSmbModal() {
#ifdef PIXELVR_HAVE_FREETYPE
    if (!smbModal_.isNew) {
        SmbCredentialsManager::instance().deleteShare(smbModal_.origHost, smbModal_.origShare);
    }
    smbModal_.active = false;
    smbModal_.editingField = SmbModalState::Field::None;
    VrKeyboard::instance().hide();
    actionMenu_.visible = false;
    browser_.openNetwork();
#endif
}

void App::openKeyboard(const std::string& prompt,
                       const std::string& initialText,
                       bool isPassword,
                       const XrInputState& in,
                       std::function<void(const std::string&)> onDone,
                       std::function<void()> onCancel) {
    kbOnDone_ = std::move(onDone);
    kbOnCancel_ = std::move(onCancel);
    float stagePos[3]{0.0f, 1.65f, 0.0f};
    float stageFwd[3]{0.0f, 0.0f, -1.0f};
    const float* fwd = renderer_.screenForward();
    xr_.transformToStage(in.headPos, fwd, stagePos, stageFwd);
    VrKeyboard::instance().show(prompt, initialText, isPassword, 256,
                                in.headPos, fwd, stagePos, stageFwd);
}

void App::openActionMenu() {
#ifdef PIXELVR_HAVE_FREETYPE
    const Browser::Entry* fe = browser_.focused();
    actionMenu_.visible = true;
    actionMenu_.page = 0;
    actionMenu_.selectedOption = 0;
    actionMenu_.options.clear();
    actionMenu_.renameBuffer.clear();

    const bool inBookmarks = (browser_.currentPath() == "bookmarks://");
    if (inBookmarks) {
        actionMenu_.title = "Bookmarks";
        actionMenu_.subtitle = (fe && !fe->isBack) ? fe->label : "Bookmarks Overview";
        if (fe && !fe->isBack) {
            actionMenu_.options.push_back("Open Bookmark");
            actionMenu_.options.push_back("Remove Bookmark");
        }
        actionMenu_.options.push_back("Cancel");
        return;
    }

    const bool hasFocused = (fe != nullptr && !fe->isBack && fe->path != "smb://__add_share__");
    if (!hasFocused) {
        actionMenu_.title = "Folder Actions";
        actionMenu_.subtitle = browser_.currentPath();
        if (browser_.isCurrentBookmarked()) {
            actionMenu_.options.push_back("Remove from Bookmarks");
        } else {
            actionMenu_.options.push_back("Add to Bookmarks");
        }
        actionMenu_.options.push_back("Cancel");
        return;
    }

    actionMenu_.title = fe->isDir ? "Folder Actions" : "File Actions";
    actionMenu_.subtitle = fe->label;

    const bool isSmbRoot = (browser_.currentPath() == "smb://" || browser_.currentPath().empty());
    const bool isSmbEntry = (fe->path.rfind("smb://", 0) == 0);

    if (isSmbRoot && isSmbEntry) {
        actionMenu_.options.push_back("Open Share");
        actionMenu_.options.push_back("Edit SMB Share");
        actionMenu_.options.push_back("Delete SMB Share");
        if (browser_.isBookmarked(fe->path)) {
            actionMenu_.options.push_back("Remove from Bookmarks");
        } else {
            actionMenu_.options.push_back("Add to Bookmarks");
        }
    } else {
        const std::string itemPath = (fe && fe->isDir) ? fe->path : browser_.currentPath();
        if (browser_.isBookmarked(itemPath)) {
            actionMenu_.options.push_back("Remove from Bookmarks");
        } else {
            actionMenu_.options.push_back(fe->isDir ? "Add to Bookmarks" : "Add Folder to Bookmarks");
        }
        if (!browser_.onNetwork()) {
            if (VaultManager::instance().isUnlocked() && !browser_.inVault()) {
                actionMenu_.options.push_back("Move to Vault");
            }
            if (fe && !fe->isDir && fe->isVideo) {
                actionMenu_.options.push_back("Change 3D / VR Format");
            }
            actionMenu_.options.push_back("Move To...");
            actionMenu_.options.push_back("Rename");
            actionMenu_.options.push_back("Delete");
        }
    }
    actionMenu_.options.push_back("Cancel");
#endif
}

int App::run() {
    using namespace std::chrono_literals;

    while (!exitRequested_) {
        if (!xr_.pollEvents(exitRequested_) || exitRequested_) {
            break;
        }

        if (xr_.sessionRunning()) {
            // Compute frame delta-time for continuous seek (F4).
            const auto now = std::chrono::steady_clock::now();
            float dt = 0.0f;
            if (lastFrameTime_.time_since_epoch().count() > 0) {
                dt = std::chrono::duration<float>(now - lastFrameTime_).count();
                dt = std::clamp(dt, 0.0f, 0.1f);  // cap to avoid huge jumps
            }
            lastFrameTime_ = now;

#ifdef PIXELVR_HAVE_MEDIA
            // Release held dma-buf frames when the decoder is about to seek so it
            // can reclaim its V4L2 capture buffers (otherwise it wedges).
            if (mediaOpen_ && media_.needsFrameRelease()) {
                renderer_.releaseVideoFrame();
                media_.confirmFrameRelease();
            }
            if (mediaOpen_ && media_.drmActive()) {
                // Zero-copy dma-buf path: import the decoder's frame directly.
                if (media_.latestDrmFrame(drmFrame_)) {
                    renderer_.updateVideoDmabuf(drmFrame_);
                }
            } else if (mediaOpen_ && media_.latestFrame(frame_)) {
                renderer_.updateVideoTexture(frame_.width, frame_.height,
                                             frame_.y.data(), frame_.uv.data(),
                                             frame_.colorspace, frame_.fullRange);
            }
#endif
            xr_.renderFrame([this](uint32_t viewIndex, uint32_t imageIndex,
                                   const XrView& view, const ViewSwapchain& swapchain) {
                renderer_.renderView(viewIndex, imageIndex, view, swapchain);
            });

            // Apply controller input that renderFrame synced this frame.
            const auto nowT = std::chrono::steady_clock::now();
            XrInputState in = xr_.input();

            // Poll native SteamVR virtual keyboard events if active
            VrKeyboard::instance().poll(
                [this](const std::string& text) {
                    if (kbOnDone_) {
                        auto cb = std::move(kbOnDone_);
                        kbOnDone_ = nullptr;
                        kbOnCancel_ = nullptr;
                        cb(text);
                    } else if (settings_.isOpen()) {
                        settings_.onKeyboardSubmit(text);
                    }
                },
                [this]() {
                    if (kbOnCancel_) {
                        auto cb = std::move(kbOnCancel_);
                        kbOnDone_ = nullptr;
                        kbOnCancel_ = nullptr;
                        cb();
                    } else {
                        kbOnDone_ = nullptr;
                        if (settings_.isOpen()) {
                            settings_.onKeyboardCancel();
                        }
                    }
                });

            const bool kbOpen = VrKeyboard::instance().isOpen();
            if (kbOpen) {
                const int kbDx = navStepX(in.thumbstickX, nowT);
                if (kbDx != 0) {
                    VrKeyboard::instance().moveCursor(kbDx);
                    xr_.triggerHaptic(0, 0.012f, 0.0f, 0.35f);
                    xr_.triggerHaptic(1, 0.012f, 0.0f, 0.35f);
                }
                in.thumbstickX = 0.0f;
                in.thumbstickY = 0.0f;
                in.select = false;
                in.selectHeld = false;
                in.togglePlay = false;
                in.browserToggle = false;
                if (smbModal_.active && smbModal_.editingField != SmbModalState::Field::None) {
                    updateSmbModalMenu();
                }
            }
            std::string kbDisplayText = VrKeyboard::instance().currentText();
            renderer_.setKeyboardModal(kbOpen,
                                       VrKeyboard::instance().description(),
                                       kbDisplayText,
                                       VrKeyboard::instance().isPassword(),
                                       kbOpen ? VrKeyboard::instance().transform() : nullptr,
                                       VrKeyboard::instance().cursorPos());

            // Custom combo recording listener (active while recording new 6-button combo in Vault & Privacy)
            if (VaultManager::instance().isRecordingCombo()) {
                for (int b : {XrInputState::BtnA, XrInputState::BtnB, XrInputState::BtnX, XrInputState::BtnY}) {
                    if (in.buttonDown[b]) {
                        const bool finished = VaultManager::instance().feedRecordButton(b);
                        if (finished) {
                            xr_.triggerHaptic(0, 0.050f, 0.0f, 0.8f);
                            xr_.triggerHaptic(1, 0.050f, 0.0f, 0.8f);
                        } else {
                            xr_.triggerHaptic(0, 0.020f, 0.0f, 0.4f);
                            xr_.triggerHaptic(1, 0.020f, 0.0f, 0.4f);
                        }
                        settings_.refresh();
                        break;
                    }
                }
                in.togglePlay = false;
                in.browserToggle = false;
                in.recenter = false;
            } else if (settings_.isListening()) {
                for (int b = 0; b < XrInputState::BtnCount; ++b) {
                    if (in.buttonDown[b]) {
                        settings_.assignListening(b);
                        break;
                    }
                }
                in.togglePlay = false;
                in.browserToggle = false;
                in.recenter = false;
            } else {
                in.togglePlay = in.buttonDown[settings_.bindPlay()];
                in.browserToggle = in.buttonDown[settings_.bindMenu()];
                in.recenter = in.buttonDown[settings_.bindRecenter()];
            }

            // Discrete grip full-press cheat code detection:
            // Holding full press of either grip button (bottomed out) and entering the button sequence.
            const bool gripFullHeld = in.gripFullPressHeld[0] || in.gripFullPressHeld[1];
            static bool sPrevGripFullHeld = false;
            if (gripFullHeld != sPrevGripFullHeld) {
                sPrevGripFullHeld = gripFullHeld;
                PIXELVR_LOG_INFO("Grip full press state changed: %s (L val: %.2f, R val: %.2f)",
                                 gripFullHeld ? "HELD" : "RELEASED",
                                 in.gripValue[0], in.gripValue[1]);
            }

            // Whirligig-style Quick Path controller button shortcut
            const int qpBtn = settings_.bindQuickPath();
            const bool quickPathPressed =
                !settings_.isListening() && !gripFullHeld &&
                !VaultManager::instance().isRecordingCombo() && !kbOpen &&
                qpBtn >= 0 && qpBtn < XrInputState::BtnCount && in.buttonDown[qpBtn];
            if (quickPathPressed) {
                browser_.openBookmarks();
                downloadsWindow_ = false;
                settings_.close();
                browser_.setOpen(true);
                xr_.triggerHaptic(1, 0.040f, 0.0f, 0.6f);
            }

            if (gripFullHeld && !settings_.isListening() && !VaultManager::instance().isRecordingCombo()) {
                const int h = in.gripFullPressHeld[1] ? 1 : 0;
                for (int b : {XrInputState::BtnA, XrInputState::BtnB, XrInputState::BtnX, XrInputState::BtnY}) {
                    if (in.buttonDown[b]) {
                        PIXELVR_LOG_INFO("Combo input button press: %s (hand %s)",
                                         (b == XrInputState::BtnA) ? "A" :
                                         (b == XrInputState::BtnB) ? "B" :
                                         (b == XrInputState::BtnX) ? "X" : "Y",
                                         h == 0 ? "left" : "right");
                        auto res = VaultManager::instance().feedInput(b, true, nowT);
                        if (res == VaultManager::ComboResult::StepMatched) {
                            PIXELVR_LOG_INFO("Vault combo step matched");
                            xr_.triggerHaptic(h, 0.015f, 0.0f, 0.4f);
                        } else if (res == VaultManager::ComboResult::Unlocked) {
                            PIXELVR_LOG_INFO("Vault combo SUCCESS: UNLOCKED");
                            xr_.triggerHaptic(h, 0.060f, 0.0f, 0.9f);
                            renderer_.setVaultUnlocked(true);
                            if (settings_.isOpen()) {
                                settings_.refresh();
                            } else {
                                downloadsWindow_ = false;
                                settings_.close();
                                browser_.openVault();
                            }
                        } else if (res == VaultManager::ComboResult::Locked) {
                            PIXELVR_LOG_INFO("Vault combo: RELOCKED");
                            xr_.triggerHaptic(h, 0.060f, 0.0f, 0.7f);
                            performVaultLock();
                        } else {
                            PIXELVR_LOG_INFO("Vault combo reset / mismatch");
                        }
                        break;
                    }
                }
                // Suppress normal button functions while grip is bottomed out
                in.togglePlay = false;
                in.browserToggle = false;
                in.recenter = false;
            }

            // X is the dedicated download button: it downloads the highlighted SMB entry.
            // Y (or configured move button) is the dedicated File Actions Submenu button.
            const bool downloadPressed =
                !settings_.isListening() && !settings_.isOpen() && !gripFullHeld &&
                !VaultManager::instance().isRecordingCombo() &&
                in.buttonDown[XrInputState::BtnX];

            const int configuredMoveBtn = VaultManager::instance().moveButton();
            const bool actionBtnPressed =
                !settings_.isListening() && !settings_.isOpen() && !gripFullHeld &&
                !VaultManager::instance().isRecordingCombo() &&
                (in.buttonDown[XrInputState::BtnY] ||
                 (configuredMoveBtn >= 0 && configuredMoveBtn < XrInputState::BtnCount &&
                  in.buttonDown[configuredMoveBtn]));

            // Grip full-press click: activate laser pointer on this hand.
            // Exactly one controller has an active laser pointer at any time.
            if (!settings_.isListening() && !VaultManager::instance().isRecordingCombo()) {
                for (int h = 0; h < 2; ++h) {
                    if (in.gripFullPressDown[h]) {
                        if (!renderer_.laserOn(h)) {
                            renderer_.setLaserOn(h, true);
                            xr_.triggerHaptic(h, 0.035f, 0.0f, 0.6f);
                            PIXELVR_LOG_INFO("Laser switched to %s controller",
                                             h == 0 ? "left" : "right");
                        }
                    }
                }
            }
            bool interacted = false;
            bool browserOpen = false;
            bool settingsOpen = false;

            // Fetch the runtime-provided controller models once a controller is
            // tracked (for left 0 and right 1 hands).
            if (in.controllers[0].active || in.controllers[1].active) {
                renderModels_.poll();
                for (int h = 0; h < 2; ++h) {
                    if (!controllerModelLoaded_[h] && renderModels_.assetReady(h)) {
                        controllerModelLoaded_[h] =
                            renderer_.loadControllerModel(h, renderModels_.gltf(h));
                    }
                }
            }

            // Drive each model's per-node animation (button presses, trigger pull,
            // thumbstick tilt) from the runtime's render-model state.
            for (int h = 0; h < 2; ++h) {
                if (controllerModelLoaded_[h]) {
                    std::vector<RenderModelSystem::NodeState> ns;
                    if (renderModels_.nodeStates(h, xr_.displayTime(), ns)) {
                        const auto& names = renderModels_.nodeNames(h);
                        std::vector<VulkanRenderer::NodeStateVis> vis;
                        vis.reserve(ns.size());
                        for (std::size_t i = 0; i < ns.size(); ++i) {
                            VulkanRenderer::NodeStateVis v;
                            v.name = (i < names.size()) ? names[i] : std::string();
                            for (int k = 0; k < 3; ++k) v.pos[k] = ns[i].pos[k];
                            for (int k = 0; k < 4; ++k) v.quat[k] = ns[i].quat[k];
                            v.visible = ns[i].visible;
                            vis.push_back(v);
                        }
                        renderer_.setControllerNodeStates(h, vis);
                    }
                }
            }

            // Live settings + place the screen in front on first head tracking.
            renderer_.setSwapEyes(settings_.swapEyes());
            renderer_.setDrawModels(settings_.drawModels());
            renderer_.setUiAppearance(settings_.uiTextScale(),
                                      settings_.uiTextColorIdx(),
                                      settings_.uiAccentColorIdx(),
                                      settings_.uiDistance());
            renderer_.setAmbilightIntensity(settings_.ambilightIntensity());
            renderer_.setUiFollowMode(settings_.uiFollowMode());
            renderer_.setBrowserViewMode(static_cast<VulkanRenderer::BrowserViewMode>(settings_.browserViewMode()));
            renderer_.setBrowserQuickPath(browser_.isCurrentBookmarked());
            if (!didInitialRecenter_ && in.headValid) {
                renderer_.recenterScreen(in.headPos, in.headForward);
                renderer_.setScreenCurvature(settings_.screenCurvature());
                didInitialRecenter_ = true;
            }

            // Controller activity: moving a controller past a small threshold (or
            // any button/stick input) keeps the laser, models and controls shown;
            // after a few seconds of stillness they fade away.
            const bool wasHudDismissed = hudDismissed_;
            const auto timeSinceDismiss = nowT - hudDismissTime_;
            const bool inDismissCooldown = hudDismissed_ && (timeSinceDismiss < std::chrono::milliseconds(500));
            bool activity = false;
            for (int h = 0; h < 2; ++h) {
                const auto& c = in.controllers[h];
                if (!c.active) {
                    activityAnchorValid_[h] = false;
                    continue;
                }
                if (!activityAnchorValid_[h]) {
                    activityAnchorValid_[h] = true;
                    for (int k = 0; k < 3; ++k) activityAnchorPos_[h][k] = c.position[k];
                    for (int k = 0; k < 4; ++k) activityAnchorOrient_[h][k] = c.orientation[k];
                    continue;
                }
                float dp = 0.0f;
                for (int k = 0; k < 3; ++k) {
                    const float d = c.position[k] - activityAnchorPos_[h][k];
                    dp += d * d;
                }
                float od = 0.0f;
                for (int k = 0; k < 4; ++k) od += c.orientation[k] * activityAnchorOrient_[h][k];
                const float moveDist = std::sqrt(dp);
                const float rotDiff = std::fabs(od);

                if (hudDismissed_) {
                    // Controller movement MUST NEVER un-hide the HUD when dismissed.
                    // Keep activity anchors updated so they don't accumulate delta.
                    for (int k = 0; k < 3; ++k) activityAnchorPos_[h][k] = c.position[k];
                    for (int k = 0; k < 4; ++k) activityAnchorOrient_[h][k] = c.orientation[k];
                } else if (moveDist > 0.06f || rotDiff < 0.985f) {
                    // Normal threshold: filters micro-tremors from resting hands
                    activity = true;
                    for (int k = 0; k < 3; ++k) activityAnchorPos_[h][k] = c.position[k];
                    for (int k = 0; k < 4; ++k) activityAnchorOrient_[h][k] = c.orientation[k];
                }
            }
            const bool stickMoved = (std::fabs(in.thumbstickX) > 0.25f ||
                                     std::fabs(in.thumbstickY) > 0.25f ||
                                     std::fabs(in.thumbstickRX) > 0.25f ||
                                     std::fabs(in.thumbstickRY) > 0.25f);
            // Only digital full grip press (gripFullPressDown) or physical buttons wake the HUD.
            // Soft analog squeeze (gripDown / BtnGrip) is ignored to prevent accidental wake-up from resting hands.
            bool anyButtonPressed = (in.togglePlay || in.browserToggle || in.recenter ||
                                     in.gripFullPressDown[0] || in.gripFullPressDown[1] ||
                                     stickMoved);
            for (int b = 0; b < XrInputState::BtnCount; ++b) {
                if (b == XrInputState::BtnGrip) continue;
                if (in.buttonDown[b]) {
                    anyButtonPressed = true;
                    break;
                }
            }
            if (!inDismissCooldown && in.select) {
                anyButtonPressed = true;
            }

            if (anyButtonPressed) {
                activity = true;
                if (!inDismissCooldown) {
                    hudDismissed_ = false;
                }
            } else if (!hudDismissed_ && (in.select || in.selectHeld)) {
                activity = true;
            }
            if (activity) {
                lastControllerActivity_ = nowT;
                VaultManager::instance().recordActivity(nowT);
            }
            if (VaultManager::instance().tick(nowT, in.headValid && xr_.sessionFocused())) {
                performVaultLock();
            }
            const bool controllersEngaged =
                (nowT - lastControllerActivity_) < std::chrono::milliseconds(3000);

#ifdef PIXELVR_HAVE_FREETYPE
            if (in.browserToggle) {
                if (actionMenu_.visible) {
                    actionMenu_.visible = false;
                    smbModal_.active = false;
                    smbModal_.editingField = SmbModalState::Field::None;
                    VrKeyboard::instance().hide();
                } else if (downloadsWindow_) {
                    downloadsWindow_ = false;
                } else if (settings_.isOpen()) {
                    settings_.close();
                } else if (browser_.isOpen()) {
                    browser_.setOpen(false);
                    smbModal_.active = false;
                    smbModal_.editingField = SmbModalState::Field::None;
                    VrKeyboard::instance().hide();
                } else if (mediaOpen_ && renderer_.controlBarVisible()) {
                    // HUD is visible during playback: B/Y dismisses the HUD!
                    dismissHud(in, nowT);
                } else {
                    browser_.open();
                }
            }
            // Downloads run on their own thread and keep going while the browser
            // is closed; poll every frame so completion is handled regardless of
            // whether the panel is visible.
            browser_.setDownloadDir(settings_.downloadDir());
            {
                std::string donePath;
                if (browser_.poll(donePath) == Browser::Result::Play &&
                    !donePath.empty()) {
#ifdef PIXELVR_HAVE_MEDIA
                    loadMedia(donePath);
#endif
                    browser_.setOpen(false);
                }
            }
            if (browser_.downloadActive()) {
                downloadToastText_ = browser_.downloadText();
                downloadToast_ = nowT + std::chrono::seconds(10);
                // Show the downloads window on the rising edge only if the user
                // is not already browsing; do not close the browser.
                if (!dlWasActive_) {
                    dlWasActive_ = true;
                    if (!browser_.isOpen()) downloadsWindow_ = true;
                }
            } else {
                dlWasActive_ = false;
            }
            const bool dlRecent = browser_.downloadActive() || browser_.hasQueuedDownloads() || nowT < downloadToast_;
            bool dlFailed = false;
            for (const auto& j : browser_.downloadJobs()) {
                if (j.state == 2) dlFailed = true;
            }
            // No head-locked download text (it followed the view); progress lives
            // in the Downloads window and the control-bar badge.
            const bool dlAnyActive = browser_.downloadActive() || browser_.hasQueuedDownloads();
            renderer_.setDownloadStatus(false, "");
            renderer_.setDownloadsBadge(dlAnyActive,
                                        !dlAnyActive &&
                                            browser_.hasDownloads() && dlRecent,
                                        !dlAnyActive && dlFailed);
            // Downloads window: job list plus action buttons at the bottom.
            std::vector<std::string> drows;
            std::vector<std::string> dbtns;
            if (browser_.downloadActive() || browser_.hasQueuedDownloads()) {
                dbtns.push_back(browser_.downloadPaused() ? "Resume All" : "Pause All");
                dbtns.push_back("Cancel Active");
            }
            dbtns.push_back("Clear Completed");
            std::vector<VulkanRenderer::DownloadItem> djList;
            int activeDlCount = 0;
            {
                const auto& jobs = browser_.downloadJobs();
                for (std::size_t i = 0; i < jobs.size(); ++i) {
                    const auto& j = jobs[i];
                    if (j.state == 0 || j.state == 4) ++activeDlCount;
                    std::string nm = j.name;
                    if (nm.size() > 30) nm = nm.substr(0, 29) + "~";
                    char buf[192];
                    VulkanRenderer::DownloadItem di;
                    di.label = j.name;
                    di.state = j.state;
                    di.progress = (j.total > 0.0) ? static_cast<float>(static_cast<double>(j.bytes) / static_cast<double>(j.total)) : (j.state == 1 ? 1.0f : 0.0f);
                    if (j.state == 0) {
                        const int pct =
                            j.total > 0 ? static_cast<int>(j.bytes * 100 / j.total) : 0;
                        const double speedMb = browser_.downloadSpeed() / (1024.0 * 1024.0);
                        std::string etaStr = "";
                        if (j.total > 0 && !browser_.downloadPaused() && browser_.downloadSpeed() >= 100.0 * 1024.0) {
                            double eta = static_cast<double>(j.total - j.bytes) / browser_.downloadSpeed();
                            if (eta < 0.0) eta = 0.0;
                            const int secs = static_cast<int>(eta);
                            char ebuf[32];
                            std::snprintf(ebuf, sizeof(ebuf), " \xB7 ETA %02d:%02d", secs / 60, secs % 60);
                            etaStr = ebuf;
                        } else if (browser_.downloadPaused()) {
                            etaStr = " \xB7 [PAUSED]";
                        }
                        char det[160];
                        if (speedMb > 0.05) {
                            std::snprintf(det, sizeof(det), "Downloading \xB7 %.1f / %.1f MB (%d%%) \xB7 %.1f MB/s%s",
                                          j.bytes / 1048576.0, j.total / 1048576.0, pct, speedMb, etaStr.c_str());
                        } else {
                            std::snprintf(det, sizeof(det), "Downloading \xB7 %.1f / %.1f MB (%d%%)%s",
                                          j.bytes / 1048576.0, j.total / 1048576.0, pct, etaStr.c_str());
                        }
                        di.detail = det;
                        std::snprintf(buf, sizeof(buf), "%s  %s", nm.c_str(), det);
                    } else if (j.state == 1) {
                        std::snprintf(buf, sizeof(buf), "%s  done", nm.c_str());
                        if (j.total > 0) {
                            char cdet[96];
                            std::snprintf(cdet, sizeof(cdet), "Completed \xB7 %.1f MB \xB7 Stored in %s",
                                          j.total / 1048576.0, settings_.downloadDir().c_str());
                            di.detail = cdet;
                        } else {
                            di.detail = "Completed \xB7 Stored locally";
                        }
                    } else if (j.state == 3) {
                        std::snprintf(buf, sizeof(buf), "%s  stopped", nm.c_str());
                        di.detail = "Paused / Stopped";
                    } else if (j.state == 4) {
                        std::snprintf(buf, sizeof(buf), "%s  queued", nm.c_str());
                        di.detail = "Queued in download queue (click to cancel)";
                    } else {
                        std::snprintf(buf, sizeof(buf), "%s  failed", nm.c_str());
                        di.detail = "Transfer Failed";
                    }
                    drows.push_back(buf);
                    djList.push_back(std::move(di));
                }
            }
            renderer_.setActiveDownloadCount(activeDlCount);
            const bool dlWin = downloadsWindow_ && !settings_.isOpen() &&
                               pendingDl_.empty();
            renderer_.setDownloadsModal(false);
            renderer_.setDownloads(
                dlWin, "Downloads", drows,
                browser_.downloadActive()
                    ? browser_.downloadText()
                    : "Trigger: Play Completed  |  B: Close Hub",
                dbtns, djList);

            // Folder-download confirmation: a modal with the size and two buttons.
            if (!pendingDl_.empty()) {
                bool confirmNow = false;
                if (pendingDlFresh_) {
                    pendingDlFresh_ = false;  // ignore the press that opened it
                } else if (in.togglePlay) {
                    confirmNow = true;
                } else if (in.browserToggle) {
                    pendingDl_.clear();
                }
                if (!pendingDl_.empty()) {
                    char sz[96];
                    std::snprintf(sz, sizeof(sz), "Size: %.2f GB%s",
                                  browser_.measuredSize() / 1073741824.0,
                                  browser_.measuring() ? "  (scanning...)" : "");
                    std::vector<std::string> crows{"Download this folder?",
                                                   pendingDlName_, sz};
                    std::vector<std::string> cbtns{"Download", "Cancel"};
                    renderer_.setDownloadsModal(true);
                    renderer_.setDownloads(true, "Confirm Download", crows,
                                           "A: Download        B: Cancel", cbtns);
                    const int cbi = renderer_.downloadsButtonHovered();
                    if (in.select) {
                        PIXELVR_LOG_INFO("confirm click: btn=%d hovered=%d", cbi,
                                         renderer_.downloadsHovered());
                    }
                    if (in.select && cbi == 0) {
                        confirmNow = true;
                    } else if (in.select && cbi == 1) {
                        pendingDl_.clear();
                    }
                }
                if (confirmNow && !pendingDl_.empty()) {
                    browser_.startDownload(pendingDl_, pendingDlName_, true);
                    pendingDl_.clear();
                }
            } else {
                renderer_.setDownloadsModal(false);
            }

            // Publish the browser panel when Files tab is active in the Hub
            if (browser_.isOpen() && !dlWin && !settings_.isOpen()) {
                // Prioritize visible cards on current page so their thumbnails decode immediately
                const int visFirst = renderer_.browserFirst();
                const int visibleSlots = (renderer_.browserViewMode() == VulkanRenderer::BrowserViewMode::BigCards) ? 6 : 12;
                for (int s = 0; s < visibleSlots; ++s) {
                    const int idx = visFirst + s;
                    if (idx >= 0 && idx < static_cast<int>(browser_.entries().size())) {
                        const auto& e = browser_.entries()[idx];
                        const std::string thumbTarget = !e.previewPath.empty()
                            ? e.previewPath
                            : (e.isVideo ? e.path : "");
                        if (!thumbTarget.empty() && thumbTarget.rfind("smb://", 0) != 0) {
                            thumbnailCache_.request(thumbTarget, /*highPriority=*/true);
                        }
                    }
                }

                std::vector<VulkanRenderer::BrowserItem> items;
                items.reserve(browser_.entries().size());
                for (const auto& e : browser_.entries()) {
                    VulkanRenderer::BrowserItem item;
                    item.name = e.label;
                    item.detail = e.detail;
                    item.isDir = e.isDir;
                    item.isVideo = e.isVideo;
                    item.isBack = e.isBack;
                    item.path = e.path;
                    item.previewPath = e.previewPath;

                    const std::string thumbTarget = !e.previewPath.empty()
                        ? e.previewPath
                        : (e.isVideo ? e.path : "");

                    ThumbnailCache::MediaMeta meta{};
                    bool hasMeta = false;
                    if (!thumbTarget.empty() && thumbTarget.rfind("smb://", 0) != 0) {
                        thumbnailCache_.request(thumbTarget);
                        hasMeta = thumbnailCache_.getMeta(thumbTarget, meta);
                    }

                    auto classifyMediaBadge = [&](const std::string& pathOrName, int w, int h) -> std::string {
                        const std::string lower = to_lower(pathOrName);
                        const bool is180 = is_vr180_media(pathOrName, w, h);
                        const bool is360 = !is180 && is_vr360_media(pathOrName, w, h);
                        const bool isTb  = contains(lower, "_tb") || contains(lower, "-tb") ||
                                           contains(lower, "_ou") || contains(lower, "overunder") ||
                                           contains(lower, "htab");
                        const bool isSbs = contains(lower, "sbs") || contains(lower, "_lr") ||
                                           contains(lower, "sidebyside") || contains(lower, "3d");

                        if (is180) {
                            if (w >= 7000 || h >= 3500 || contains(lower, "8k") || contains(lower, "3600") || contains(lower, "4320")) {
                                return "VR180 8K";
                            }
                            if (w >= 5000 || h >= 2500 || contains(lower, "6k") || contains(lower, "5k") ||
                                contains(lower, "2880") || contains(lower, "2700") || contains(lower, "5400") || contains(lower, "5760")) {
                                return "VR180 6K";
                            }
                            if (w >= 3600 || h >= 1800 || contains(lower, "4k") || contains(lower, "2160") || contains(lower, "1920")) {
                                return "VR180 4K";
                            }
                            return (isSbs || isTb) ? "VR180 3D" : "VR180";
                        }

                        if (is360) {
                            if (w >= 7000 || h >= 3500 || contains(lower, "8k")) {
                                return "VR360 8K";
                            }
                            if (w >= 5000 || h >= 2500 || contains(lower, "6k")) {
                                return "VR360 6K";
                            }
                            if (w >= 3600 || h >= 1800 || contains(lower, "4k")) {
                                return "VR360 4K";
                            }
                            return (isSbs || isTb) ? "VR360 3D" : "VR360";
                        }

                        if (isTb)  return "3D TB";
                        if (isSbs) return "3D SBS";

                        if (w >= 7000 || h >= 3500 || contains(lower, "8k") || contains(lower, "4320p")) {
                            return "FLAT 8K";
                        }
                        if (w >= 3600 || h >= 2000 || contains(lower, "4k") ||
                            contains(lower, "2160p") || contains(lower, "uhd")) {
                            return "FLAT 4K";
                        }
                        if (w >= 2500 || h >= 1400 || contains(lower, "1440p") || contains(lower, "2k")) {
                            return "FLAT 1440P";
                        }
                        if (w >= 1800 || h >= 1000 || contains(lower, "1080p") || contains(lower, "fhd")) {
                            return "FLAT 1080P";
                        }
                        if (w >= 1200 || h >= 700 || contains(lower, "720p")) {
                            return "FLAT 720P";
                        }
                        return "FLAT 2D";
                    };

                    if (e.isBack) {
                        item.tag = "UP";
                    } else if (e.isNetwork) {
                        item.tag = "SMB";
                    } else if (e.isDir) {
                        // Folders always show FOLDER in the badge; format of contained
                        // video is added to the detail line below.
                        item.tag = "FOLDER";
                        if (!e.previewPath.empty()) {
                            const std::string innerFmt = classifyMediaBadge(
                                e.previewPath, hasMeta ? meta.width : 0, hasMeta ? meta.height : 0);
                            if (!item.detail.empty()) {
                                item.detail = innerFmt + " \xB7 " + item.detail;
                            } else {
                                item.detail = innerFmt;
                            }
                        }
                    } else if (e.isVideo) {
                        item.tag = classifyMediaBadge(
                            e.path.empty() ? e.label : e.path,
                            hasMeta ? meta.width : 0, hasMeta ? meta.height : 0);
                    } else {
                        item.tag = "FILE";
                    }

                    // Format file size
                    if (e.size > 0) {
                        char sbuf[32];
                        constexpr long long kGB = 1024LL * 1024LL * 1024LL;
                        constexpr long long kMB = 1024LL * 1024LL;
                        if (e.size >= kGB) {
                            std::snprintf(sbuf, sizeof(sbuf), "%.2f GB", static_cast<double>(e.size) / static_cast<double>(kGB));
                        } else if (e.size >= kMB) {
                            std::snprintf(sbuf, sizeof(sbuf), "%.1f MB", static_cast<double>(e.size) / static_cast<double>(kMB));
                        } else {
                            std::snprintf(sbuf, sizeof(sbuf), "%.0f KB", static_cast<double>(e.size) / 1024.0);
                        }
                        item.sizeTag = sbuf;
                    } else if (e.isDir) {
                        item.sizeTag = "Folder";
                    } else if (e.isBack) {
                        item.sizeTag = "";
                    } else {
                        item.sizeTag = "--";
                    }

                    if (hasMeta) {
                        if (meta.durationSec > 0.5) {
                            const int tot = static_cast<int>(meta.durationSec + 0.5);
                            const int h = tot / 3600, m = (tot % 3600) / 60, s = tot % 60;
                            char dbuf[24];
                            if (h > 0) std::snprintf(dbuf, sizeof(dbuf), "%d:%02d:%02d", h, m, s);
                            else std::snprintf(dbuf, sizeof(dbuf), "%d:%02d", m, s);
                            item.durationTag = dbuf;
                        }
                        if (meta.width > 0 && meta.height > 0) {
                            char specBuf[96];
                            if (meta.fps > 1.0) {
                                std::snprintf(specBuf, sizeof(specBuf), "%dx%d \xB7 %.0f FPS",
                                              meta.width, meta.height, meta.fps);
                            } else {
                                std::snprintf(specBuf, sizeof(specBuf), "%dx%d",
                                              meta.width, meta.height);
                            }
                            if (!item.detail.empty()) {
                                item.detail = std::string(specBuf) + " \xB7 " + item.detail;
                            } else {
                                item.detail = specBuf;
                            }
                            char rbuf[48];
                            if (meta.fps > 1.0) {
                                std::snprintf(rbuf, sizeof(rbuf), "%dx%d %.0ffps", meta.width, meta.height, meta.fps);
                            } else {
                                std::snprintf(rbuf, sizeof(rbuf), "%dx%d", meta.width, meta.height);
                            }
                            item.resolutionTag = rbuf;
                        }
                    }
                    if (e.isVideo) {
                        if (item.durationTag.empty()) item.durationTag = "--";
                        if (item.resolutionTag.empty()) item.resolutionTag = "--";
                    }
                    items.push_back(std::move(item));
                }

                renderer_.setBrowserNetwork(browser_.onNetwork());
                renderer_.setBrowserQuickFilter(browser_.activeFilter());
                std::string status = browser_.status();
                if (browser_.onNetwork() && !browser_.isBusy()) {
                    if (status.empty()) {
                        status = "A/Trigger: Stream  \xB7  X: Download to Local";
                    }
                } else if (!browser_.onNetwork() && !browser_.isBusy()) {
                    if (status.empty()) {
                        status = "A/Trigger: Play  \xB7  Y: Actions";
                    }
                }
                renderer_.setListColumnVisibility(
                    settings_.listShowFormat(),
                    settings_.listShowRes(),
                    settings_.listShowSize(),
                    settings_.listShowDuration()
                );
                int restoredFirst = 0;
                if (browser_.consumeRestoredFirst(restoredFirst)) {
                    renderer_.setBrowserFirst(restoredFirst);
                }
                renderer_.setBrowserItems(true, browser_.title(), items,
                                         browser_.selected(), status);
                if (!browser_.isScanning()) {
                    browser_.setFirst(renderer_.browserFirst());
                }

                // Feed ready thumbnails to the visible slots (6 for BigCards, 12 for MiniCards, 8 for List)
                const int curFirst = renderer_.browserFirst();
                const int totalItems = static_cast<int>(items.size());
                const int numSlots = (renderer_.browserViewMode() == VulkanRenderer::BrowserViewMode::BigCards) ? 6 :
                                     (renderer_.browserViewMode() == VulkanRenderer::BrowserViewMode::MiniCards ? 12 : 8);
                for (int slot = 0; slot < numSlots; ++slot) {
                    const int idx = curFirst + slot;
                    if (idx >= 0 && idx < totalItems) {
                        const std::string thumbTarget = !items[idx].previewPath.empty()
                            ? items[idx].previewPath
                            : (items[idx].isVideo ? items[idx].path : "");
                        if (!thumbTarget.empty()) {
                            std::vector<uint8_t> rgba;
                            if (thumbnailCache_.get(thumbTarget, rgba)) {
                                renderer_.updateCardThumbnail(slot, thumbTarget, rgba.data(), 256, 144);
                            }
                        }
                    }
                }
                browserOpen = true;
            } else {
                renderer_.setBrowserItems(false, "", {}, 0, "");
            }
            renderer_.setActionMenu(actionMenu_);
#endif

            // Settings panel overlay
            if (settings_.isOpen()) {
                std::vector<std::string> sRows;
                std::vector<uint8_t> sEnabled;
                sRows.reserve(settings_.rows().size());
                sEnabled.reserve(settings_.rows().size());
                for (const auto& r : settings_.rows()) {
                    sRows.push_back(r.value.empty() ? r.label
                                                    : (r.label + " : " + r.value));
                    sEnabled.push_back(r.enabled ? 1 : 0);
                }
                renderer_.setSettingsSection(settings_.section());
                renderer_.setSettings(true, settings_.title(), sRows,
                                      settings_.selected(), settings_.status(),
                                      sEnabled);
                settingsOpen = true;
            } else {
                renderer_.setSettings(false, "", {}, 0, "");
            }

            // Feed controller poses to the renderer (models, pointer ray, cursor);
            // it raycasts the pointer against UI panels and reports hit rows/buttons.
            {
                // Align each controller model to its runtime render-model space
                // (where the system draws the real controller).
                for (int h = 0; h < 2; ++h) {
                    if (!in.controllers[h].gripActive) continue;
                    float mPos[3], mQuat[4];
                    if (renderModels_.locate(h, xr_.appSpace(), xr_.displayTime(), mPos, mQuat)) {
                        const float* gq = in.controllers[h].gripOrientation;
                        const float* gt = in.controllers[h].gripPosition;
                        const float giq[4] = {-gq[0], -gq[1], -gq[2], gq[3]};
                        float git[3];
                        quatRotate(giq, gt, git);
                        git[0] = -git[0]; git[1] = -git[1]; git[2] = -git[2];
                        composePose(git, giq, mPos, mQuat, modelCorrPos_[h], modelCorrQuat_[h]);
                        modelCorrValid_[h] = true;
                    }
                }
                const bool uiPanelActive = browserOpen || settingsOpen ||
                                           downloadsWindow_ || !pendingDl_.empty() || actionMenu_.visible || smbModal_.active;
                // The Transport HUD is only visible during playback when the
                // Unified Spatial Hub and modals are closed.
                const bool barBlocked = browserOpen || settingsOpen || downloadsWindow_ ||
                                        !pendingDl_.empty() || actionMenu_.visible || smbModal_.active;
                // Controls (buttons + timeline) show only while a controller is
                // actively moving; they fade after a few seconds of stillness.
                renderer_.setControlBar(!barBlocked && controllersEngaged && !hudDismissed_ &&
                                        (in.controllers[0].active ||
                                         in.controllers[1].active));
                // Keep laser active when UI panels, activity, or virtual keyboard are active!
                const bool kbActive = VrKeyboard::instance().isOpen();
                if (kbActive) {
                    // Auto-focus the laser on whichever hand is actually aiming at the
                    // keyboard, so the user never has to grip to "activate" a pointer.
                    // The keyboard's local transform and the controller aim poses live in
                    // the same space the renderer raycasts in, so we can score each hand by
                    // how directly it points at the keyboard centre.
                    const float* kb = VrKeyboard::instance().transform();
                    const float kbCenter[3] = { kb[3], kb[7], kb[11] };
                    float score[2] = { -2.0f, -2.0f };
                    for (int h = 0; h < 2; ++h) {
                        if (!in.controllers[h].active) continue;
                        const float fwdLocal[3] = { 0.0f, 0.0f, -1.0f };
                        float fwd[3];
                        quatRotate(in.controllers[h].orientation, fwdLocal, fwd);
                        float d[3] = { kbCenter[0] - in.controllers[h].position[0],
                                       kbCenter[1] - in.controllers[h].position[1],
                                       kbCenter[2] - in.controllers[h].position[2] };
                        const float dl = std::sqrt(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
                        if (dl > 1e-4f) { d[0] /= dl; d[1] /= dl; d[2] /= dl; }
                        score[h] = fwd[0]*d[0] + fwd[1]*d[1] + fwd[2]*d[2];
                    }
                    const int cur = renderer_.laserOn(1) ? 1 : (renderer_.laserOn(0) ? 0 : -1);
                    const int best = (score[1] >= score[0]) ? 1 : 0;
                    if (score[best] > 0.55f && in.controllers[best].active) {
                        // A hand is pointing at the keyboard: give it the laser, but only
                        // switch away from the current hand when it is clearly better (hysteresis).
                        if (cur < 0 || best == cur || score[best] > score[cur] + 0.12f) {
                            if (!renderer_.laserOn(best)) renderer_.setLaserOn(best, true);
                        }
                    } else if (!renderer_.laserOn(0) && !renderer_.laserOn(1)) {
                        // Neither hand aims at the keyboard yet: make sure some laser is on.
                        if (in.controllers[1].active) renderer_.setLaserOn(1, true);
                        else if (in.controllers[0].active) renderer_.setLaserOn(0, true);
                    } else if (renderer_.laserOn(1) && !in.controllers[1].active && in.controllers[0].active) {
                        renderer_.setLaserOn(0, true);
                    } else if (renderer_.laserOn(0) && !in.controllers[0].active && in.controllers[1].active) {
                        renderer_.setLaserOn(1, true);
                    }
                }
                const bool showLaser = (uiPanelActive || (controllersEngaged && !hudDismissed_) || kbActive);
                // Build each controller's render state: corrected model pose,
                // low-pass smoothed (the raw hand pose jitters), while the aim
                // pose stays raw so the laser remains responsive.
                auto build = [&](int h) {
                    const XrInputState::ControllerPose& c = in.controllers[h];
                    VulkanRenderer::ControllerVis v;
                    v.active = c.active && showLaser;
                    for (int k = 0; k < 3; ++k) v.position[k] = c.position[k];
                    for (int k = 0; k < 4; ++k) v.orientation[k] = c.orientation[k];
                    v.gripActive = c.gripActive;
                    float gp[3], gq[4];
                    if (c.gripActive && modelCorrValid_[h]) {
                        composePose(c.gripPosition, c.gripOrientation,
                                    modelCorrPos_[h], modelCorrQuat_[h], gp, gq);
                    } else {
                        for (int k = 0; k < 3; ++k) gp[k] = c.gripPosition[k];
                        for (int k = 0; k < 4; ++k) gq[k] = c.gripOrientation[k];
                    }
                    if (c.gripActive) {
                        const float a = 0.4f;  // lower = smoother (more lag)
                        if (!modelSmoothInit_[h]) {
                            modelSmoothInit_[h] = true;
                            for (int k = 0; k < 3; ++k) modelSmoothPos_[h][k] = gp[k];
                            for (int k = 0; k < 4; ++k) modelSmoothQuat_[h][k] = gq[k];
                        } else {
                            for (int k = 0; k < 3; ++k)
                                modelSmoothPos_[h][k] +=
                                    (gp[k] - modelSmoothPos_[h][k]) * a;
                            quatNlerp(modelSmoothQuat_[h], gq, a,
                                      modelSmoothQuat_[h]);
                        }
                        for (int k = 0; k < 3; ++k)
                            v.gripPosition[k] = modelSmoothPos_[h][k];
                        for (int k = 0; k < 4; ++k)
                            v.gripOrientation[k] = modelSmoothQuat_[h][k];
                    } else {
                        for (int k = 0; k < 3; ++k) v.gripPosition[k] = gp[k];
                        for (int k = 0; k < 4; ++k) v.gripOrientation[k] = gq[k];
                    }
                    return v;
                };
                renderer_.setControllers(build(0), build(1));
            }

            // Spatial haptic feedback on pointer hover transitions
            static int lastBrowserHov = -1;
            static int lastPageBtnHov = -1;
            static int lastSidebarHov = -1;
            static int lastSettingsHov = -1;
            static int lastCtrlBtnHov = -1;
            const int curBrowserHov = renderer_.browserHovered();
            const int curPageBtnHov = renderer_.browserPageBtnHovered();
            const int curSidebarHov = renderer_.browserSidebarHovered();
            const int curSettingsHov = renderer_.settingsHovered();
            const int curCtrlBtnHov = renderer_.pointerButton();
            const int pH = renderer_.pointerHand();
            const int activeH = (pH >= 0 && pH <= 1) ? pH : 1;

            if (settings_.hoverHapticsEnabled() &&
                ((curBrowserHov != lastBrowserHov && curBrowserHov >= 0) ||
                 (curPageBtnHov != lastPageBtnHov && curPageBtnHov >= 0) ||
                 (curSidebarHov != lastSidebarHov && curSidebarHov >= 0) ||
                 (curSettingsHov != lastSettingsHov && curSettingsHov >= 0) ||
                 (curCtrlBtnHov != lastCtrlBtnHov && curCtrlBtnHov >= 0))) {
                xr_.triggerHaptic(activeH, 0.008f, 0.0f, settings_.hoverHapticsStrength());
            }
            lastBrowserHov = curBrowserHov;
            lastPageBtnHov = curPageBtnHov;
            lastSidebarHov = curSidebarHov;
            lastSettingsHov = curSettingsHov;
            lastCtrlBtnHov = curCtrlBtnHov;

#ifdef PIXELVR_HAVE_FREETYPE
            // Unified Spatial Hub left navigation rail clicks (shared across Files, Downloads, Settings)
            const bool hubActive = (browserOpen || settingsOpen || downloadsWindow_) &&
                                   pendingDl_.empty() && !actionMenu_.visible;
            const int sbHover = renderer_.browserSidebarHovered();
            if (hubActive && (in.togglePlay || in.select) && sbHover >= 0) {
                if (sbHover == 0) {
                    // Main Tab: Files
                    downloadsWindow_ = false;
                    settings_.close();
                    if (!browser_.isOpen()) browser_.open();
                } else if (sbHover == 1) {
                    // Main Tab: Downloads
                    settings_.close();
                    browser_.setOpen(true);
                    downloadsWindow_ = true;
                } else if (sbHover == 2) {
                    // Main Tab: Settings
                    downloadsWindow_ = false;
                    settings_.open();
                } else if (sbHover >= 3 && sbHover <= 8) {
                    const int sub = sbHover - 3;
                    if (settings_.isOpen()) {
                        if (sub <= 4) {
                            settings_.setSection(sub);
                        } else if (sub == 5 && VaultManager::instance().isUnlocked()) {
                            settings_.setSection(5);
                        }
                    } else if (downloadsWindow_) {
                        if (sub <= 2) renderer_.setDownloadsFilter(sub);
                    } else if (browser_.isOpen()) {
                        if (sub == 0) browser_.openHome();
                        else if (sub == 1) browser_.openVideos();
                        else if (sub == 2) browser_.openNetwork();
                        else if (sub == 3) browser_.openDownloads();
                        else if (sub == 4) browser_.openBookmarks();
                        else if (sub == 5 && VaultManager::instance().isUnlocked()) browser_.openVault();
                    }
                } else if (sbHover == 9 || sbHover == 8) {
                    // Lock Vault button
                    if (VaultManager::instance().isUnlocked()) {
                        xr_.triggerHaptic(0, 0.050f, 0.0f, 0.7f);
                        xr_.triggerHaptic(1, 0.050f, 0.0f, 0.7f);
                        performVaultLock();
                    }
                }
                interacted = true;
            }

            // Downloads view interaction inside Unified Spatial Hub
            if (dlWin && (in.select || in.togglePlay) && sbHover < 0) {
                const int bi = renderer_.downloadsButtonHovered();
                if (bi >= 0) {
                    if (browser_.downloadActive() || browser_.hasQueuedDownloads()) {
                        if (bi == 0) browser_.togglePause();
                        else if (bi == 1) browser_.stopDownload();
                        else if (bi == 2) browser_.clearFinished();
                    } else if (bi == 0) {
                        browser_.clearFinished();
                    }
                    interacted = true;
                } else {
                    const int hov = renderer_.downloadsHovered();
                    const auto& jobs = browser_.downloadJobs();
                    if (hov >= 0 && hov < static_cast<int>(jobs.size())) {
                        const auto& j = jobs[static_cast<std::size_t>(hov)];
                        if (j.state == 1 && !j.localPath.empty()) {
                            if (j.isDir) {
                                browser_.openPath(j.localPath);
                                downloadsWindow_ = false;
                            } else {
#ifdef PIXELVR_HAVE_MEDIA
                                loadMedia(j.localPath);
                                downloadsWindow_ = false;
                                browser_.setOpen(false);
#endif
                            }
                            interacted = true;
                        } else if (j.state == 4) {
                            browser_.cancelQueued(j.uri);
                            xr_.triggerHaptic(0, 0.020f, 0.0f, 0.4f);
                            xr_.triggerHaptic(1, 0.020f, 0.0f, 0.4f);
                            interacted = true;
                        }
                    }
                }
            }

            // File Action Submenu or Files view interaction inside Unified Spatial Hub
            if (actionMenu_.visible) {
                if ((!browser_.isOpen() && actionMenu_.page != 5) || downloadsWindow_ || settings_.isOpen() || !pendingDl_.empty()) {
                    actionMenu_.visible = false;
                } else {
                    const Browser::Entry* fe = browser_.focused();

                    // Button Y (or configured move button) toggles/cancels
                    if (actionBtnPressed) {
                        actionMenu_.visible = false;
                    } else if (actionMenu_.page == 0 || actionMenu_.page == 1 || actionMenu_.page == 5) {
                        const int dy = navStep(in.thumbstickY, nowT);
                        if (dy != 0) {
                            actionMenu_.selectedOption += dy;
                            const int maxOpt = static_cast<int>(actionMenu_.options.size()) - 1;
                            if (actionMenu_.selectedOption < 0) actionMenu_.selectedOption = 0;
                            if (actionMenu_.selectedOption > maxOpt) actionMenu_.selectedOption = maxOpt;
                        }
                        const int hov = renderer_.actionMenuBtnHovered();
                        if (hov >= 0) actionMenu_.selectedOption = hov;

                        if (in.select || in.togglePlay) {
                            int chosen = (hov >= 0) ? hov : actionMenu_.selectedOption;
                            if (chosen >= 0 && chosen < static_cast<int>(actionMenu_.options.size())) {
                                const std::string opt = actionMenu_.options[chosen];
                                if (actionMenu_.page == 0) {
                                    if (opt == "Open Share" || opt == "Open Bookmark") {
                                        std::string playPath;
                                        if (browser_.activate(playPath) == Browser::Result::Play && !playPath.empty()) {
#ifdef PIXELVR_HAVE_MEDIA
                                            loadMedia(playPath);
#endif
                                            browser_.setOpen(false);
                                        }
                                        actionMenu_.visible = false;
                                    } else if (opt == "Edit SMB Share") {
                                        if (fe) {
                                            openSmbModal(false, fe->path);
                                        }
                                    } else if (opt == "Delete SMB Share") {
                                        if (fe) {
                                            std::string rest = fe->path;
                                            if (rest.rfind("smb://", 0) == 0) rest = rest.substr(6);
                                            while (!rest.empty() && rest.back() == '/') rest.pop_back();
                                            auto slash = rest.find('/');
                                            std::string host = (slash == std::string::npos) ? rest : rest.substr(0, slash);
                                            std::string share = (slash == std::string::npos) ? "" : rest.substr(slash + 1);
                                            SmbCredentialsManager::instance().deleteShare(host, share);
                                            browser_.openNetwork();
                                        }
                                        actionMenu_.visible = false;
                                    } else if (opt == "Go to Bookmarks" || opt == "Go to Quick Path") {
                                        browser_.openBookmarks();
                                        actionMenu_.visible = false;
                                    } else if (opt == "Add to Bookmarks" || opt == "Add Folder to Bookmarks" ||
                                               opt == "Set as Quick Path" || opt == "Set Folder as Quick Path") {
                                        const std::string targetPath = (fe && fe->isDir) ? fe->path : browser_.currentPath();
                                        browser_.addBookmark(targetPath, fe ? fe->label : "");
                                        xr_.triggerHaptic(0, 0.060f, 0.0f, 0.8f);
                                        xr_.triggerHaptic(1, 0.060f, 0.0f, 0.8f);
                                        actionMenu_.visible = false;
                                    } else if (opt == "Remove Bookmark" || opt == "Remove from Bookmarks") {
                                        const std::string targetPath = (fe && !fe->isBack) ? fe->path : browser_.currentPath();
                                        browser_.removeBookmark(targetPath);
                                        xr_.triggerHaptic(0, 0.050f, 0.0f, 0.7f);
                                        xr_.triggerHaptic(1, 0.050f, 0.0f, 0.7f);
                                        actionMenu_.visible = false;
                                    } else if (opt == "Move to Vault") {
                                        std::string newPath;
                                        if (browser_.moveFocusedToVault(newPath)) {
                                            xr_.triggerHaptic(0, 0.050f, 0.0f, 0.7f);
                                            xr_.triggerHaptic(1, 0.050f, 0.0f, 0.7f);
                                        }
                                        actionMenu_.visible = false;
                                    } else if (opt == "Change 3D / VR Format") {
                                        actionMenu_.page = 5;
                                        actionMenu_.title = "3D & VR Format";
                                        actionMenu_.subtitle = fe ? fe->label : "";
                                        actionMenu_.options.clear();
                                        actionMenu_.options.push_back("2D Flat (Standard)");
                                        actionMenu_.options.push_back("3D Side-by-Side (SBS)");
                                        actionMenu_.options.push_back("3D Top-Bottom (Over-Under)");
                                        actionMenu_.options.push_back("VR 180° (Side-by-Side)");
                                        actionMenu_.options.push_back("VR 360° (Equirectangular)");
                                        actionMenu_.options.push_back("Cancel");
                                        actionMenu_.selectedOption = 0;
                                    } else if (opt == "Move To...") {
                                        actionMenu_.page = 1;
                                        actionMenu_.title = "Move Destination";
                                        actionMenu_.subtitle = fe ? fe->label : "";
                                        actionMenu_.options.clear();
                                        actionMenu_.options.push_back("Videos (~/Videos)");
                                        actionMenu_.options.push_back("Downloads (~/Downloads)");
                                        actionMenu_.options.push_back("Home (~/)");
                                        if (!browser_.parentDir().empty()) {
                                            actionMenu_.options.push_back("Parent Folder (..)");
                                        }
                                        actionMenu_.options.push_back("Cancel");
                                        actionMenu_.selectedOption = 0;
                                    } else if (opt == "Rename") {
                                        actionMenu_.visible = false;
                                        if (fe) {
                                            std::string curName = fe->label;
                                            while (!curName.empty() && (curName.back() == '/' || curName.back() == '\\')) {
                                                curName.pop_back();
                                            }
                                            const bool isDir = fe->isDir;
                                            openKeyboard(
                                                isDir ? "Rename Folder" : "Rename File",
                                                curName,
                                                false,
                                                in,
                                                [this](const std::string& newText) {
                                                    std::string clean = newText;
                                                    while (!clean.empty() && (std::isspace(static_cast<unsigned char>(clean.front())) || clean.front() == '/' || clean.front() == '\\')) {
                                                        clean.erase(clean.begin());
                                                    }
                                                    while (!clean.empty() && (std::isspace(static_cast<unsigned char>(clean.back())) || clean.back() == '/' || clean.back() == '\\')) {
                                                        clean.pop_back();
                                                    }
                                                    if (!clean.empty()) {
                                                        if (browser_.renameFocused(clean)) {
                                                            xr_.triggerHaptic(0, 0.050f, 0.0f, 0.7f);
                                                            xr_.triggerHaptic(1, 0.050f, 0.0f, 0.7f);
                                                        }
                                                    }
                                                });
                                        }
                                    } else if (opt == "Delete") {
                                        actionMenu_.page = 2;
                                        actionMenu_.title = (fe && fe->isDir) ? "Delete Folder?" : "Delete File?";
                                        actionMenu_.subtitle = "Permanently delete \"" + (fe ? fe->label : "") + "\"" +
                                                               ((fe && fe->isDir) ? " and all contents?" : "?");
                                        actionMenu_.options.clear();
                                        actionMenu_.options.push_back("Yes, Delete");
                                        actionMenu_.options.push_back("Cancel");
                                        actionMenu_.selectedOption = 1; // Default to Cancel for safety
                                    } else if (opt == "Cancel") {
                                        actionMenu_.visible = false;
                                    }
                                } else if (actionMenu_.page == 1) {
                                    if (opt == "Cancel") {
                                        actionMenu_.visible = false;
                                    } else {
                                        std::string targetDir;
                                        if (opt == "Videos (~/Videos)") targetDir = browser_.videosDir();
                                        else if (opt == "Downloads (~/Downloads)") targetDir = browser_.downloadsDir();
                                        else if (opt == "Home (~/)") targetDir = browser_.homeDir();
                                        else if (opt == "Parent Folder (..)") targetDir = browser_.parentDir();

                                        if (!targetDir.empty()) {
                                            std::string newPath;
                                            if (browser_.moveFocusedTo(targetDir, newPath)) {
                                                xr_.triggerHaptic(0, 0.050f, 0.0f, 0.7f);
                                                xr_.triggerHaptic(1, 0.050f, 0.0f, 0.7f);
                                            }
                                        }
                                        actionMenu_.visible = false;
                                    }
                                } else if (actionMenu_.page == 5) {
                                    if (opt == "2D Flat (Standard)") {
                                        settings_.setProjectionMode(0);
                                        settings_.setStereoMode(0);
                                        renderer_.setProjectionMode(VulkanRenderer::ProjectionMode::Flat);
                                        renderer_.setStereoMode(VulkanRenderer::StereoMode::Mono);
                                    } else if (opt == "3D Side-by-Side (SBS)") {
                                        settings_.setProjectionMode(0);
                                        settings_.setStereoMode(1);
                                        renderer_.setProjectionMode(VulkanRenderer::ProjectionMode::Flat);
                                        renderer_.setStereoMode(VulkanRenderer::StereoMode::SideBySide);
                                    } else if (opt == "3D Top-Bottom (Over-Under)") {
                                        settings_.setProjectionMode(0);
                                        settings_.setStereoMode(2);
                                        renderer_.setProjectionMode(VulkanRenderer::ProjectionMode::Flat);
                                        renderer_.setStereoMode(VulkanRenderer::StereoMode::TopBottom);
                                    } else if (opt == "VR 180° (Side-by-Side)") {
                                        settings_.setProjectionMode(2);
                                        settings_.setStereoMode(1);
                                        renderer_.setProjectionMode(VulkanRenderer::ProjectionMode::Equirect180);
                                        renderer_.setStereoMode(VulkanRenderer::StereoMode::SideBySide);
                                    } else if (opt == "VR 360° (Equirectangular)") {
                                        settings_.setProjectionMode(1);
                                        settings_.setStereoMode(0);
                                        renderer_.setProjectionMode(VulkanRenderer::ProjectionMode::Equirect360);
                                        renderer_.setStereoMode(VulkanRenderer::StereoMode::Mono);
                                    }
                                    xr_.triggerHaptic(0, 0.040f, 0.0f, 0.6f);
                                    xr_.triggerHaptic(1, 0.040f, 0.0f, 0.6f);
                                    actionMenu_.visible = false;
                                }
                            }
                            interacted = true;
                        }
                    } else if (actionMenu_.page == 2) {
                        const int dx = navStepX(in.thumbstickX, nowT);
                        const int dy = navStep(in.thumbstickY, nowT);
                        if (dx != 0 || dy != 0) {
                            actionMenu_.selectedOption = (actionMenu_.selectedOption == 0) ? 1 : 0;
                        }
                        const int hov = renderer_.actionMenuBtnHovered();
                        if (hov >= 0) actionMenu_.selectedOption = hov;

                        if (in.select || in.togglePlay) {
                            int chosen = (hov >= 0) ? hov : actionMenu_.selectedOption;
                            if (chosen == 0) {
                                std::string delName;
                                if (browser_.deleteFocused(delName)) {
                                    xr_.triggerHaptic(0, 0.050f, 0.0f, 0.7f);
                                    xr_.triggerHaptic(1, 0.050f, 0.0f, 0.7f);
                                }
                                actionMenu_.visible = false;
                            } else {
                                actionMenu_.visible = false;
                            }
                            interacted = true;
                        }
                    } else if (actionMenu_.page == 4) {
                        // SMB Share Editor Modal
                        const int dy = navStep(in.thumbstickY, nowT);
                        const int maxOpt = static_cast<int>(actionMenu_.options.size()) - 1;
                        if (dy != 0) {
                            actionMenu_.selectedOption += dy;
                            if (actionMenu_.selectedOption < 0) actionMenu_.selectedOption = 0;
                            if (actionMenu_.selectedOption > maxOpt) actionMenu_.selectedOption = maxOpt;
                        }
                        const int hov = renderer_.actionMenuBtnHovered();
                        if (hov >= 0) actionMenu_.selectedOption = hov;

                        if (in.select || in.togglePlay) {
                            int chosen = (hov >= 0) ? hov : actionMenu_.selectedOption;
                            if (chosen >= 0) {
                                xr_.triggerHaptic(0, 0.025f, 0.0f, 0.5f);
                                xr_.triggerHaptic(1, 0.025f, 0.0f, 0.5f);

                                auto openKeyboardForField = [&](SmbModalState::Field field, const std::string& desc, const std::string& currentVal, bool isPass) {
                                    smbModal_.editingField = field;
                                    openKeyboard(
                                        desc,
                                        currentVal,
                                        isPass,
                                        in,
                                        [this, field](const std::string& text) {
                                            if (!smbModal_.active) return;
                                            if (field == SmbModalState::Field::Host) {
                                                std::string h = text;
                                                if (h.rfind("smb://", 0) == 0) h = h.substr(6);
                                                while (!h.empty() && (std::isspace(static_cast<unsigned char>(h.front())) || h.front() == '/' || h.front() == '\\')) h.erase(h.begin());
                                                while (!h.empty() && (std::isspace(static_cast<unsigned char>(h.back())) || h.back() == '/' || h.back() == '\\')) h.pop_back();
                                                smbModal_.host = h;
                                            } else if (field == SmbModalState::Field::Share) {
                                                std::string s = text;
                                                while (!s.empty() && (s.front() == '/' || s.front() == '\\')) s.erase(s.begin());
                                                while (!s.empty() && (s.back() == '/' || s.back() == '\\')) s.pop_back();
                                                smbModal_.share = s;
                                            } else if (field == SmbModalState::Field::User) {
                                                smbModal_.user = text;
                                            } else if (field == SmbModalState::Field::Pass) {
                                                smbModal_.pass = text;
                                            } else if (field == SmbModalState::Field::Workgroup) {
                                                smbModal_.workgroup = text.empty() ? "WORKGROUP" : text;
                                            } else if (field == SmbModalState::Field::Label) {
                                                smbModal_.label = text;
                                            }
                                            smbModal_.editingField = SmbModalState::Field::None;
                                            updateSmbModalMenu();
                                        },
                                        [this]() {
                                            if (smbModal_.active) {
                                                smbModal_.editingField = SmbModalState::Field::None;
                                                updateSmbModalMenu();
                                            }
                                        });
                                };

                                if (chosen == 0) {
                                    openKeyboardForField(SmbModalState::Field::Host, "SMB Server IP / Hostname", smbModal_.host, false);
                                } else if (chosen == 1) {
                                    openKeyboardForField(SmbModalState::Field::Share, "SMB Share Name (empty for all)", smbModal_.share, false);
                                } else if (chosen == 2) {
                                    smbModal_.anon = !smbModal_.anon;
                                    updateSmbModalMenu();
                                } else if (chosen == 3) {
                                    openKeyboardForField(SmbModalState::Field::User, "SMB Username", smbModal_.user, false);
                                } else if (chosen == 4) {
                                    openKeyboardForField(SmbModalState::Field::Pass, "SMB Password", smbModal_.pass, true);
                                } else if (chosen == 5) {
                                    openKeyboardForField(SmbModalState::Field::Workgroup, "SMB Workgroup / Domain", smbModal_.workgroup, false);
                                } else if (chosen == 6) {
                                    openKeyboardForField(SmbModalState::Field::Label, "SMB Nickname", smbModal_.label, false);
                                } else if (chosen == 7) {
                                    // Save SMB Share
                                    saveSmbModal();
                                } else if (chosen == 8) {
                                    if (smbModal_.isNew) {
                                        // Cancel
                                        actionMenu_.visible = false;
                                        smbModal_.active = false;
                                        smbModal_.editingField = SmbModalState::Field::None;
                                        VrKeyboard::instance().hide();
                                    } else {
                                        // Delete Share
                                        deleteSmbModal();
                                    }
                                } else if (chosen == 9) {
                                    // Cancel
                                    actionMenu_.visible = false;
                                    smbModal_.active = false;
                                    smbModal_.editingField = SmbModalState::Field::None;
                                    VrKeyboard::instance().hide();
                                }
                            }
                            interacted = true;
                        }
                    }
                }
            } else if (actionBtnPressed && browser_.isOpen() && !downloadsWindow_ &&
                       !settings_.isOpen() && pendingDl_.empty()) {
                openActionMenu();
                interacted = true;
            } else if (browser_.isOpen() && !browser_.isScanning() && !downloadsWindow_ &&
                       !settings_.isOpen() && pendingDl_.empty()) {
                // 2D grid thumbstick navigation: row steps depend on columns per row
                const int cols = (renderer_.browserViewMode() == VulkanRenderer::BrowserViewMode::MiniCards) ? 4 :
                                 (renderer_.browserViewMode() == VulkanRenderer::BrowserViewMode::CompactList) ? 1 : 3;
                const int dy = navStep(in.thumbstickY, nowT);
                const int dx = navStepX(in.thumbstickX, nowT);
                if (dy != 0 || dx != 0) {
                    const int delta = dy * cols + dx;
                    browser_.move(delta);
                    renderer_.scrollBrowser(0);
                }

                // Check header pagination & action buttons
                const int pbHover = renderer_.browserPageBtnHovered();
                if ((in.togglePlay || in.select) && pbHover >= 0 && sbHover < 0) {
                    const int pageStep = (renderer_.browserViewMode() == VulkanRenderer::BrowserViewMode::BigCards) ? 2 :
                                         (renderer_.browserViewMode() == VulkanRenderer::BrowserViewMode::MiniCards ? 3 : 8);
                    if (pbHover == 0) {
                        renderer_.scrollBrowser(-pageStep);
                        browser_.setFirst(renderer_.browserFirst());
                        browser_.setSelected(renderer_.browserFirst());
                    } else if (pbHover == 1) {
                        renderer_.scrollBrowser(pageStep);
                        browser_.setFirst(renderer_.browserFirst());
                        browser_.setSelected(renderer_.browserFirst());
                    } else if (pbHover == 2) {
                        // Cycle View Mode: BigCards (0) -> MiniCards (1) -> CompactList (2)
                        int curMode = settings_.browserViewMode();
                        curMode = (curMode + 1) % 3;
                        settings_.setBrowserViewMode(curMode);
                        settings_.saveConfig();
                        renderer_.setBrowserViewMode(static_cast<VulkanRenderer::BrowserViewMode>(curMode));
                        renderer_.scrollBrowser(0);
                        xr_.triggerHaptic(1, 0.025f, 0.0f, 0.5f);
                    } else if (pbHover == 3) {
                        const std::string cur = browser_.currentPath();
                        if (cur != "bookmarks://") {
                            if (browser_.isBookmarked(cur)) {
                                browser_.removeBookmark(cur);
                                renderer_.setBrowserQuickPath(false);
                                xr_.triggerHaptic(1, 0.040f, 0.0f, 0.5f);
                            } else {
                                browser_.addBookmark(cur);
                                renderer_.setBrowserQuickPath(true);
                                xr_.triggerHaptic(1, 0.040f, 0.0f, 0.7f);
                            }
                        }
                    } else if (pbHover == 4) {
                        // Dedicated Up button in header: activate parent navigation
                        if (!browser_.entries().empty() && browser_.entries()[0].isBack) {
                            browser_.setSelected(0);
                            std::string playPath;
                            browser_.activate(playPath);
                            xr_.triggerHaptic(1, 0.025f, 0.0f, 0.5f);
                        }
                    }
                    interacted = true;
                } else if (sbHover < 0) {
                    const bool stickActive = (std::fabs(in.thumbstickY) > 0.25f || std::fabs(in.thumbstickX) > 0.25f);
                    const int hovered = renderer_.browserHovered();
                    if (!stickActive && hovered >= 0) {
                        renderer_.setBrowserSelected(hovered);
                        browser_.setSelected(hovered);
                    }
                    // Open the highlighted entry.
                    if ((in.togglePlay || in.select) &&
                        renderer_.pointerButton() < 0 && hovered >= 0) {
                        std::string playPath;
                        Browser::Result res = browser_.activate(playPath);
                        if (res == Browser::Result::AddSmbShare) {
                            openSmbModal(true);
                        } else if (res == Browser::Result::Play && !playPath.empty()) {
#ifdef PIXELVR_HAVE_MEDIA
                            loadMedia(playPath);
#endif
                            browser_.setOpen(false);
                        }
                    } else if ((in.togglePlay || in.select) &&
                               renderer_.pointerButton() < 0 && stickActive) {
                        std::string playPath;
                        Browser::Result res = browser_.activate(playPath);
                        if (res == Browser::Result::AddSmbShare) {
                            openSmbModal(true);
                        } else if (res == Browser::Result::Play && !playPath.empty()) {
#ifdef PIXELVR_HAVE_MEDIA
                            loadMedia(playPath);
#endif
                            browser_.setOpen(false);
                        }
                    }
                }
            }
            if ((browser_.isOpen() || downloadsWindow_) && in.recenter && in.headValid) {
                renderer_.recenterScreen(in.headPos, in.headForward);
            }

            // X: download the highlighted entry when browsing SMB shares. Folders ask for
            // confirmation (with the measured size) first; plain files download immediately.
            if (downloadPressed && browser_.isOpen() && !downloadsWindow_ && !settings_.isOpen() &&
                pendingDl_.empty() && !actionMenu_.visible && browser_.onNetwork()) {
                const Browser::Entry* fe = browser_.focused();
                if (fe != nullptr && fe->isDir && !fe->isBack) {
                    pendingDl_ = fe->path;
                    pendingDlName_ = fe->label;
                    pendingDlFresh_ = true;
                    browser_.measureFolder(fe->path);
                } else {
                    browser_.downloadFocused();
                }
            }
#endif

            if (settings_.isOpen() && sbHover < 0) {
                if (const int d = navStep(in.thumbstickY, nowT)) {
                    settings_.move(d * 2);
                }
                if (const int dx = navStepX(in.thumbstickX, nowT)) {
                    settings_.move(dx);
                }

                const int hovered = renderer_.settingsHovered();
                if (hovered >= 0) {
                    renderer_.setSettingsSelected(hovered);
                    settings_.setSelected(hovered);
                }
                // Trigger activates the selection made by the laser or the stick.
                if (in.select && (hovered >= 0 || std::fabs(in.thumbstickY) > 0.25f)) {
                    auto act = settings_.activate();
                    if (act == SettingsMenu::Action::LockVaultAction) {
                        xr_.triggerHaptic(0, 0.050f, 0.0f, 0.7f);
                        xr_.triggerHaptic(1, 0.050f, 0.0f, 0.7f);
                        performVaultLock();
                    } else if (act == SettingsMenu::Action::RecenterScreen) {
                        if (in.headValid) {
                            renderer_.recenterScreen(in.headPos, in.headForward);
                        }
                        } else if (act == SettingsMenu::Action::CycleProjection) {
                            int pm = settings_.projectionMode();
                            renderer_.setProjectionMode(
                                pm == 0 ? VulkanRenderer::ProjectionMode::Flat :
                                pm == 1 ? VulkanRenderer::ProjectionMode::Equirect360 :
                                          VulkanRenderer::ProjectionMode::Equirect180);
                        } else if (act == SettingsMenu::Action::CycleStereo) {
                            int sm = settings_.stereoMode();
                            renderer_.setStereoMode(
                                sm == 0 ? VulkanRenderer::StereoMode::Mono :
                                sm == 1 ? VulkanRenderer::StereoMode::SideBySide :
                                          VulkanRenderer::StereoMode::TopBottom);
                        } else if (act == SettingsMenu::Action::CycleDistance) {
                            renderer_.adjustScreenDistance(settings_.screenDistance() - 3.0f);
                        } else if (act == SettingsMenu::Action::CycleCurvature) {
                            renderer_.setScreenCurvature(settings_.screenCurvature());
                        }
#ifdef PIXELVR_HAVE_MEDIA
                        else if (act == SettingsMenu::Action::PickVideoStream ||
                                 act == SettingsMenu::Action::PickAudioStream) {
                            switchTracks(false);
                        } else if (act == SettingsMenu::Action::PickLanguage) {
                            switchTracks(true);
                        }
#endif
                        else if (act == SettingsMenu::Action::ToggleHoverHaptics) {
                            const int pH = renderer_.pointerHand();
                            const int activeH = (pH >= 0 && pH <= 1) ? pH : 1;
                            xr_.triggerHaptic(activeH, 0.020f, 0.0f, settings_.hoverHapticsEnabled() ? settings_.hoverHapticsStrength() : 0.2f);
                        } else if (act == SettingsMenu::Action::CycleHoverHapticsStrength) {
                            const int pH = renderer_.pointerHand();
                            const int activeH = (pH >= 0 && pH <= 1) ? pH : 1;
                            xr_.triggerHaptic(activeH, 0.025f, 0.0f, settings_.hoverHapticsStrength());
                        } else if (act == SettingsMenu::Action::CycleResScale) {
                            applyResolutionScale(settings_.resScale());
                            const int pH = renderer_.pointerHand();
                            const int activeH = (pH >= 0 && pH <= 1) ? pH : 1;
                            xr_.triggerHaptic(activeH, 0.030f, 0.0f, settings_.hoverHapticsStrength());
                        }
                }
                if (in.recenter && in.headValid) {
                    renderer_.recenterScreen(in.headPos, in.headForward);
                }
            }

            // Cinema Transport Dock is only active when the Unified Spatial Hub is closed.
            const bool barOk = !browserOpen && !settingsOpen && !downloadsWindow_ &&
                               pendingDl_.empty() && !actionMenu_.visible && !smbModal_.active;
            if (barOk && renderer_.controlBarVisible() && !wasHudDismissed) {
                // Point-and-click the media control bar with the trigger.
                const int btn = renderer_.pointerButton();
                // The trigger clicks the bar; A does too (A is the reliable
                // press -- the analog trigger does not always report a click).
                if ((in.select || in.togglePlay) && btn >= 0) {
                    PIXELVR_LOG_INFO("control bar click: button=%d", btn);
                    const auto cb = static_cast<VulkanRenderer::CtrlButton>(btn);
                    if (cb == VulkanRenderer::CtrlButton::Stop) {
#ifdef PIXELVR_HAVE_MEDIA
                        media_.close();
                        mediaOpen_ = false;
                        renderer_.clearVideo();
#endif
#ifdef PIXELVR_HAVE_FREETYPE
                        browser_.open();  // large file selection
#endif
                    } else if (cb == VulkanRenderer::CtrlButton::Settings) {
                        settings_.open();
                    } else if (cb == VulkanRenderer::CtrlButton::Recenter) {
                        if (in.headValid) {
                            renderer_.recenterScreen(in.headPos, in.headForward);
                        }
                    } else if (cb == VulkanRenderer::CtrlButton::Downloads ||
                               cb == VulkanRenderer::CtrlButton::Files) {
#ifdef PIXELVR_HAVE_FREETYPE
                        browser_.open();
#endif
                    } else if (cb == VulkanRenderer::CtrlButton::Download) {
                        // Download the entry highlighted in the file browser;
                        // folders ask for confirmation (with the size) first.
                        if (browser_.isOpen() && pendingDl_.empty()) {
                            const Browser::Entry* fe = browser_.focused();
                            if (fe != nullptr && fe->isDir && !fe->isBack &&
                                browser_.onNetwork()) {
                                pendingDl_ = fe->path;
                                pendingDlName_ = fe->label;
                                pendingDlFresh_ = true;
                                browser_.measureFolder(fe->path);
                            } else {
                                browser_.downloadFocused();
                            }
                        }
                    }
#ifdef PIXELVR_HAVE_MEDIA
                    else if (mediaOpen_ &&
                             cb == VulkanRenderer::CtrlButton::PlayPause) {
                        media_.togglePause();
                    } else if (mediaOpen_ &&
                               cb == VulkanRenderer::CtrlButton::SeekBack) {
                        media_.seekRelative(-10.0);
                    } else if (mediaOpen_ &&
                               cb == VulkanRenderer::CtrlButton::SeekFwd) {
                        media_.seekRelative(10.0);
                    } else if (cb == VulkanRenderer::CtrlButton::FormatToggle) {
                        actionMenu_.visible = true;
                        actionMenu_.page = 5;
                        actionMenu_.title = "Playback Format";
                        actionMenu_.subtitle = "Select 3D / VR Projection";
                        actionMenu_.options.clear();
                        actionMenu_.options.push_back("2D Flat (Standard)");
                        actionMenu_.options.push_back("3D Side-by-Side (SBS)");
                        actionMenu_.options.push_back("3D Top-Bottom (Over-Under)");
                        actionMenu_.options.push_back("VR 180° (Side-by-Side)");
                        actionMenu_.options.push_back("VR 360° (Equirectangular)");
                        actionMenu_.options.push_back("Cancel");
                        actionMenu_.selectedOption = 0;
                        xr_.triggerHaptic(0, 0.020f, 0.0f, 0.4f);
                        xr_.triggerHaptic(1, 0.020f, 0.0f, 0.4f);
                    } else if (cb == VulkanRenderer::CtrlButton::AudioToggle) {
                        if (mediaOpen_ && media_.audioStreams().size() > 1) {
                            const auto& streams = media_.audioStreams();
                            int curStream = media_.currentAudioStream();
                            int nextIdx = 0;
                            for (size_t i = 0; i < streams.size(); ++i) {
                                if (streams[i].index == curStream) {
                                    nextIdx = static_cast<int>((i + 1) % streams.size());
                                    break;
                                }
                            }
                            int nextStreamIndex = streams[nextIdx].index;
                            settings_.setSelectedAudioStream(nextStreamIndex);
                            media_.setPreferredAudioStream(nextStreamIndex);
                            media_.setPreferredAudioLanguage("");
                            switchTracks(false);
                            xr_.triggerHaptic(0, 0.02f, 0.0f, 0.5f);
                            xr_.triggerHaptic(1, 0.02f, 0.0f, 0.5f);
                        }
                    } else if (cb == VulkanRenderer::CtrlButton::ScreenToggle) {
                        if (settings_.projectionMode() == 0) {
                            settings_.setCurvatureIdx(settings_.curvatureIdx() + 1);
                            renderer_.setScreenCurvature(settings_.screenCurvature());
                        } else {
                            renderer_.setSpherePitch(0.0f);
                            renderer_.setSphereZoom(1.0f);
                        }
                    } else if (cb == VulkanRenderer::CtrlButton::HideHud) {
                        dismissHud(in, nowT);
                    }
#endif
                    interacted = true;
                }
                // Click outside the control dock in empty space immediately hides the HUD
                if (in.select && !renderer_.pointerActive()) {
                    dismissHud(in, nowT);
                    interacted = true;
                }
                if (in.select && renderer_.pointerOnTimeToggle()) {
                    renderer_.toggleTimeDisplay();  // total <-> time remaining
                    interacted = true;
                }
            }
            // Viewpoint / transport controls only when no panel is open (the
            // thumbstick drives in-panel navigation otherwise).
            if (barOk && !browserOpen) {
                if (in.recenter && in.headValid) {
                    renderer_.recenterScreen(in.headPos, in.headForward);
                    interacted = true;
                }
                // Viewpoint: flat screens move nearer/farther (thumbstick Y);
                // immersive 360/180 uses the RIGHT stick -- up/down tilts (angle),
                // left/right pushes content nearer/farther (distance).
                if (settings_.projectionMode() == 0) {
                    if (in.thumbstickY > 0.15f || in.thumbstickY < -0.15f) {
                        renderer_.adjustScreenDistance(in.thumbstickY * 0.04f);
                        interacted = true;
                    }
                    // When pointing at the Screen Curve pill on the HUD, thumbstick X smoothly adjusts curvature!
                    if (renderer_.pointerButton() == static_cast<int>(VulkanRenderer::CtrlButton::ScreenToggle) &&
                        std::fabs(in.thumbstickX) > 0.15f && dt > 0.0f) {
                        renderer_.adjustScreenCurvature(in.thumbstickX * dt * 0.65f);
                        interacted = true;
                    }
                } else if (dt > 0.0f) {
                    if (std::fabs(in.thumbstickRY) > 0.15f) {
                        renderer_.adjustSpherePitch(in.thumbstickRY * dt * 0.6f);
                        interacted = true;
                    }
                    if (std::fabs(in.thumbstickRX) > 0.15f) {
                        renderer_.adjustSphereZoom(in.thumbstickRX * dt * 0.6f);
                        interacted = true;
                    }
                }
#ifdef PIXELVR_HAVE_MEDIA
                if (in.togglePlay && mediaOpen_) {
                    media_.togglePause();
                    interacted = true;
                }
                if (mediaOpen_) {
                    // Right stick left/right steps -/+30s (flat only; in 360/180 the
                    // right stick drives the viewpoint instead).
                    constexpr double kStep = 30.0;
                    const bool sphereMode = settings_.projectionMode() != 0;
                    if (!sphereMode && in.thumbstickRX > 0.6f && seekStepArmed_) {
                        media_.seekRelative(kStep);
                        seekStepArmed_ = false;
                        interacted = true;
                    } else if (!sphereMode && in.thumbstickRX < -0.6f && seekStepArmed_) {
                        media_.seekRelative(-kStep);
                        seekStepArmed_ = false;
                        interacted = true;
                    } else if (std::fabs(in.thumbstickRX) < 0.3f) {
                        seekStepArmed_ = true;
                    }

                    // Scrub: accumulate a target while dragging and commit it with a
                    // single seek on release. Seeking continuously floods the
                    // hardware decoder (a reopen each) and stalls it; one seek per
                    // gesture stays responsive.
                    bool scrubInput = false;
                    if (!wasHudDismissed && renderer_.pointerOnTimeline() && in.selectHeld &&
                        media_.duration() > 0.0) {
                        scrubTarget_ = renderer_.pointerScrub() * media_.duration();
                        scrubInput = true;
                    } else if (std::fabs(in.thumbstickLX) > 0.15f && dt > 0.0f &&
                               media_.duration() > 0.0) {
                        if (!scrubbing_) scrubTarget_ = media_.position();
                        const float ax = in.thumbstickLX;
                        const float t = (std::fabs(ax) - 0.15f) / 0.85f;
                        const double rate =
                            std::copysign(static_cast<double>(t * t) * 60.0, ax);
                        scrubTarget_ = std::clamp(scrubTarget_ + rate * dt, 0.0,
                                                  media_.duration());
                        scrubInput = true;
                    }
                    if (scrubInput) {
                        scrubbing_ = true;
                        interacted = true;
                    } else if (scrubbing_) {
                        media_.seek(scrubTarget_);  // commit the scrub with one seek
                        scrubbing_ = false;
                    }
                }
                if (interacted) {
                    lastControllerActivity_ = nowT;
                }
                const bool paused = mediaOpen_ && media_.paused();
                const bool controlsUp =
                    mediaOpen_ &&
                    (paused || (controllersEngaged && (in.controllers[0].active ||
                                                       in.controllers[1].active)));
                float progress = 0.0f;
                double elapsed = 0.0, total = 0.0;
                if (mediaOpen_ && media_.duration() > 0.0) {
                    total = media_.duration();
                    elapsed = scrubbing_ ? scrubTarget_ : media_.position();
                    progress = static_cast<float>(elapsed / total);
                }
                std::string fLabel = "2D FLAT";
                if (settings_.projectionMode() == 0) {
                    fLabel = (settings_.stereoMode() == 0) ? "2D FLAT" :
                             (settings_.stereoMode() == 1) ? "3D SBS" : "3D TB";
                } else if (settings_.projectionMode() == 2) {
                    fLabel = (settings_.stereoMode() == 1) ? "VR 180 SBS" :
                             (settings_.stereoMode() == 2) ? "VR 180 TB" : "180 MONO";
                } else {
                    fLabel = (settings_.stereoMode() == 1) ? "360 SBS" : "360 MONO";
                }

                std::string aLabel = "AUDIO";
                bool aEnabled = false;
                if (mediaOpen_) {
                    const auto aStreams = media_.audioStreams();
                    const int curA = media_.currentAudioStream();
                    for (const auto& as : aStreams) {
                        if (as.index == curA) {
                            std::string l = as.language;
                            for (char& c : l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                            if (l.empty() || l == "und" || l == "undef" || l == "undefined") {
                                aLabel = (as.label.empty() || as.label == "und" || as.label == "undef") ? "DEFAULT" : as.label;
                            } else {
                                aLabel = as.language;
                            }
                            for (char& c : aLabel) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                            if (aLabel.size() > 8) aLabel = aLabel.substr(0, 8);
                            break;
                        }
                    }
                    // Only enable audio cycling if there is more than 1 audio stream
                    aEnabled = (aStreams.size() > 1);
                }

                std::string sLabel = "RESET HORIZON";
                if (settings_.projectionMode() == 0) {
                    const int cPct = static_cast<int>(renderer_.screenCurvature() * 100.0f + 0.5f);
                    if (cPct <= 2) {
                        sLabel = "SCREEN: FLAT";
                    } else {
                        char cbuf[32];
                        std::snprintf(cbuf, sizeof(cbuf), "CURVED: %d%%", cPct);
                        sLabel = cbuf;
                    }
                }

                renderer_.setHudLabels(fLabel, aLabel, sLabel, true, aEnabled);
                renderer_.setTransport(controlsUp, progress, paused, elapsed, total);
#else
                (void)interacted;
#endif
            }

#ifdef PIXELVR_HAVE_MEDIA
            if (settings_.showStats()) {
                updateStats();
            }
            // Always-on periodic perf log so decode-bound vs render-bound stalls
            // show up in the run log even without the on-screen overlay.
            if (mediaOpen_) {
                ++perfFrames_;
                if (perfLastLog_.time_since_epoch().count() == 0) {
                    perfLastLog_ = now;
                    perfLastDecoded_ = media_.decodedFrames();
                } else {
                    const double since =
                        std::chrono::duration<double>(now - perfLastLog_).count();
                    if (since >= 2.0) {
                        const uint64_t dec = media_.decodedFrames();
                        const double decFps =
                            (dec >= perfLastDecoded_)
                                ? static_cast<double>(dec - perfLastDecoded_) / since
                                : 0.0;
                        PIXELVR_LOG_INFO(
                            "PERF: render %.1f fps, decode %.1f/%.2f fps",
                            perfFrames_ / since, decFps, media_.frameRate());
                        perfFrames_ = 0;
                        perfLastDecoded_ = dec;
                        perfLastLog_ = now;
                    }
                }
            }
#endif
            renderer_.setStats(settings_.showStats(), statsLines_);
        } else {
            // Idle until the runtime tells us to start rendering.
            std::this_thread::sleep_for(50ms);
        }
    }

    if (renderer_.device() != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(renderer_.device());
    }
    VrKeyboard::instance().shutdown();
    renderModels_.shutdown();  // destroy XR model handles before the session
    PIXELVR_LOG_INFO("Shutting down");
    return 0;
}

} // namespace pixelvr

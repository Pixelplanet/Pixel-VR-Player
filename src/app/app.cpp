#include "app/app.hpp"
#include "app/file_ops.hpp"
#include "app/vault_crypto.hpp"
#include "app/vault_manager.hpp"
#include "util/sdcard.hpp"
#include "net/remote_fs.hpp"
#include "net/smb_credentials.hpp"
#include "xr/vr_keyboard.hpp"
#include "util/ipv4.hpp"

#include "util/logging.hpp"
#include "util/media_files.hpp"

#include <unistd.h>

#include <algorithm>
#include <random>
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

// Format hints come from the file name only, never from an URL's query (token).
std::string formatName(const std::string& path) {
    if (path.rfind("http", 0) == 0) return path.substr(0, path.find('?'));
    // Encrypted vault file: the original name (stored inside it) carries the hints.
    if (path.size() > 5 && path.compare(path.size() - 5, 5, vaultcrypto::kExtension) == 0) {
        const std::string n = vaultcrypto::storedName(path);
        if (!n.empty()) return path.substr(0, path.find_last_of('/') + 1) + n;
    }
    return path;
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

// The app's stereo values: 0 mono, 1 SBS, 2 TB, 3 frame-sequential (FS3D). FS3D frames reach the
// renderer already paired into full side-by-side frames, so it draws them as SBS.
VulkanRenderer::StereoMode rendererStereo(int s) {
    return s == 3 ? VulkanRenderer::StereoMode::SideBySide : static_cast<VulkanRenderer::StereoMode>(std::clamp(s, 0, 2));
}

constexpr float kArrangeGap = 0.04f;  // metres between arranged screens when not snapped

// Size of a screen as quad.vert draws it: the 3.2 x 1.8 quad bent into an arc of
// curvature * 1.75 rad, then scaled by (widthScale * scale, scale, widthScale * scale).
struct ScreenExtent {
    float hx = 0.0f;  // half the chord width
    float hy = 0.0f;  // half height
    float ez = 0.0f;  // how far the side edges come toward the viewer
};
ScreenExtent screenExtent(float eyeAspect, float scale, float curvature) {
    const float widthScale = eyeAspect / (16.0f / 9.0f);
    ScreenExtent e;
    e.hx = 1.6f * widthScale * scale;
    e.hy = 0.9f * scale;
    if (curvature > 0.005f) {
        const float a = curvature * 1.75f;
        const float r = 3.2f / a;
        e.hx = widthScale * scale * r * std::sin(0.5f * a);
        e.ez = widthScale * scale * r * (1.0f - std::cos(0.5f * a));
    }
    return e;
}

float wrapAngle(float a) { return std::atan2(std::sin(a), std::cos(a)); }

} // namespace

App::App() = default;

App::~App() {
#ifdef PIXELVR_HAVE_MEDIA
    for (int slot = 0; slot < VulkanRenderer::kMaxVideoSlots; ++slot) {
        cancelPendingOpen(slot, true);  // exit: wait for the thread (the engine goes away next)
    }
#endif
    plugins_.shutdownAll();  // plugins stop their devices before anything else goes away
    watchdogRunning_.store(false);
    if (watchdogThread_.joinable()) {
        watchdogThread_.join();
    }
}

void App::watchdogLoop() {
    using namespace std::chrono_literals;
    auto lastPresentTime = std::chrono::steady_clock::now();
    bool prevAbsent = false;
    bool lockDeferred = false;  // headset off during a vault move: lock as soon as the move is over

    while (watchdogRunning_.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(250ms);
        if (!watchdogRunning_.load(std::memory_order_relaxed)) break;

        const auto now = std::chrono::steady_clock::now();
        const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();

        const int64_t lastHeartbeat = lastFrameHeartbeatMs_.load(std::memory_order_relaxed);
        const bool heartbeatStalled = (lastHeartbeat > 0) && ((nowMs - lastHeartbeat) > 2000);

        const bool vrPresent = VrKeyboard::instance().isUserPresent();
        const bool xrPresent = xr_.userPresent();
        const bool userPresent = vrPresent && xrPresent;

        const bool isAbsent = !userPresent || heartbeatStalled;

        // The lock that a running vault move held back: due once the move is over and nobody is back.
        if (lockDeferred && isAbsent && !FileOps::instance().relocationPending()) {
            lockDeferred = false;
            if (VaultManager::instance().isUnlocked() && VaultManager::instance().autoLockHeadsetOff()) {
                pendingAutoLock_.store(true, std::memory_order_release);
            }
        }
        if (!isAbsent) {
            lastPresentTime = now;
            lockDeferred = false;
            if (prevAbsent) {
                PIXELVR_LOG_INFO("Watchdog: user returned / frame loop resumed (vrPresent=%d xrPresent=%d hbDelta=%lldms)",
                                 (int)vrPresent, (int)xrPresent, static_cast<long long>(nowMs - lastHeartbeat));
                prevAbsent = false;
            }
        } else {
            const auto absentElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - lastPresentTime).count();
            if (!prevAbsent && absentElapsedMs >= 3000) {
                prevAbsent = true;
                PIXELVR_LOG_INFO("Watchdog: user absent for %lld ms (vrPresent=%d xrPresent=%d stalled=%d) -> triggering auto-lock & pause",
                                 static_cast<long long>(absentElapsedMs), (int)vrPresent, (int)xrPresent, (int)heartbeatStalled);

                // From here, not the frame loop: that may be stalled while the
                // headset is off, and a page's audio would keep playing.
                if (pauseOnRemoval_.load(std::memory_order_relaxed)) {
                    // screenKind_ mirrors the screens (the main thread owns them);
                    // pausing an engine / host is thread-safe.
                    for (int slot = 0; slot < VulkanRenderer::kMaxVideoSlots; ++slot) {
                        const int kind = screenKind_[slot].load(std::memory_order_acquire);
                        if (kind == 2) {
                            webHosts_[slot].pauseMedia(pauseLivestreams_.load(std::memory_order_relaxed));
                        } else if (kind == 1) {
#ifdef PIXELVR_HAVE_MEDIA
                            screens_[slot].engine.setPaused(true);
#endif
                        }
                    }
                }
                // Only with "Auto-Lock on Headset Removal" on, and never while a vault relocation is running
                // (an SD card vault transfer can take 30+ minutes while the headset sits on a desk).
                if (VaultManager::instance().isUnlocked() && VaultManager::instance().autoLockHeadsetOff()) {
                    if (FileOps::instance().relocationPending()) lockDeferred = true;
                    else pendingAutoLock_.store(true, std::memory_order_release);
                }
            }
        }
    }
}

namespace {
// Byte offset in UTF-8 `s` of the JavaScript (UTF-16) string index `units`.
std::size_t utf16ToByteOffset(const std::string& s, int units) {
    std::size_t i = 0;
    while (i < s.size() && units > 0) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        const std::size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : 4;
        units -= len == 4 ? 2 : 1;  // characters beyond the BMP are two units
        i += len;
    }
    return std::min(i, s.size());
}
}  // namespace

bool App::init(const std::string& mediaPath) {
    // The fonts take about a second each: build them while the runtime starts.
    renderer_.prebuildFonts();
    // Before we connect: SteamVR's "Up next" launch, like Steam's own VR games.
    steamVrPrelaunch();
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
    // Buttons are assigned in SteamVR now. Old in-app choices are dropped from
    // the config files (no longer written); custom ones get a one-time notice.
    {
        VaultManager& vault = VaultManager::instance();
        if (settings_.legacyBindingsCustom() || vault.legacyMoveButtonCustom()) {
            controlsNoticePending_ = true;
            PIXELVR_LOG_INFO("Custom in-app button choices found: reset (buttons are assigned in SteamVR)");
        }
        if (settings_.legacyBindingsSeen()) settings_.saveConfig();
        if (vault.legacyMoveButtonSeen()) vault.saveConfig();
    }
    renderModels_.init(xr_.instance(), xr_.session(), xr_.renderModelEnabled());

    const std::vector<int64_t> formats = xr_.enumerateSwapchainFormats();
    if (formats.empty()) {
        PIXELVR_LOG_ERROR("Runtime reported no swapchain formats");
        return false;
    }
    colorFormat_ = renderer_.chooseColorFormat(formats);
    // Native panel resolution: the UI and flat screens are composition layers
    // (sharp regardless of this), so supersampling the eye buffers only ever
    // helped 180/360 video, and not enough to justify the GPU cost.
    if (!xr_.createSwapchains(colorFormat_, 1.0f)) {
        return false;
    }
    if (!renderer_.createRenderResources(xr_.swapchains())) {
        return false;
    }
    // UI composition layer: the panel UI is drawn once into this image and the
    // compositor places it in the world, sharp at any eye-buffer resolution.
    // Best effort: without it the UI is drawn into the eye buffers as before.
    renderer_.createLayerResources(xr_, colorFormat_);

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
    browser_.setDownloadDir(settings_.downloadDir());
    plugins_.init();
    settings_.setPluginHost(&plugins_);
    settings_.setPluginRepo(&pluginRepo_);
    // No clip requested: open the file browser instead of a test video.
    if (mediaPath.empty()) {
#ifdef PIXELVR_HAVE_FREETYPE
        browser_.open();
#endif
    }

    PIXELVR_LOG_INFO("Initialization complete");
    return true;
}

#ifdef PIXELVR_HAVE_MEDIA
void App::cancelPendingOpen(int slot, bool wait) {
    if (slot < 0 || slot >= VulkanRenderer::kMaxVideoSlots) return;
    ScreenState& sc = screens_[slot];
    sc.hasDeferredLoad = false;
    sc.hasDeferredPlex = false;
    sc.deferredPath.clear();
    if (sc.openThread.joinable()) {
        sc.engine.abortOpen();
        if (wait || sc.openThreadDone.load(std::memory_order_acquire)) {
            sc.openThread.join();
            if (sc.openAbandoned || sc.opening) sc.engine.close();  // an open that finished after all
            sc.openAbandoned = false;
        } else {
            sc.openAbandoned = true;  // reaped by pollMediaOpens once the network call returns
        }
    }
    sc.opening = false;
    sc.openState.store(0, std::memory_order_relaxed);
    sc.openPlex = false;
    sc.openSeekPos = -1.0;
    sc.openStartPaused = false;
}

void App::loadMedia(const std::string& path, int slot, bool fromQueue) {
    lastLoadFailed_ = false;
    if (slot < 0 || !multiScreenEnabled()) slot = multiScreenEnabled() ? selectedScreen_ : 0;
    {
        // 360/180 is immersive: it always plays on the main screen.
        VulkanRenderer::ProjectionMode p = VulkanRenderer::ProjectionMode::Flat;
        VulkanRenderer::StereoMode st = VulkanRenderer::StereoMode::Mono;
        bool fromName = false;
        detect_format(formatName(path), p, st, fromName);
        if (slot > 0 && p != VulkanRenderer::ProjectionMode::Flat) slot = 0;
        // Immersive plays alone, and its decoder must open with the hardware
        // to itself: every other screen (a 1080p clip, a browser page with a
        // playing video) still holds a decoder session, and an 8K session on
        // top of one is refused ("current session not supported") so the clip
        // would silently fall back to software decoding. Close them first.
        if (p != VulkanRenderer::ProjectionMode::Flat) {
            for (int i = 1; i < VulkanRenderer::kMaxVideoSlots; ++i) {
                if (screens_[i].open || screens_[i].opening) closeMedia(i);
            }
        }
    }
    if (screens_[slot].web) clearSlotKeepLayout(slot);  // a file replaces a browser screen
    ScreenState& sc = screens_[slot];
    // A reload of the same file keeps its place and format; while it is still opening for the
    // first time it is not "the same file" yet (the resume position must still apply).
    const bool sameFile = (path == sc.path) && (sc.open || (sc.opening && sc.openSameFile));
    cancelPendingOpen(slot);
    if (sc.openAbandoned) {
        // The cancelled open still holds the engine: start this one as soon as it lets go. Until
        // then the screen counts as loading (Stop / Back cancel it, it is not a free slot).
        sc.hasDeferredLoad = true;
        sc.deferredPath = path;
        sc.deferredFromQueue = fromQueue;
        sc.openStartedAt = std::chrono::steady_clock::now();
        sc.open = false;
        sc.opening = true;
        sc.openSameFile = false;
        sc.path = path;
        if (slot > 0) {  // an extra screen needs its place for the "Loading..." text to show
            VulkanRenderer::ScreenPlacement pl = renderer_.screenPlacement(slot);
            if (!pl.active) {
                placeNewScreen(slot);
                pl = renderer_.screenPlacement(slot);
                pl.active = true;
                renderer_.setScreenPlacement(slot, pl);
                joinArrangement(slot);
            }
        }
        if (!fromQueue) {  // what a normal load does to the UI, now and not when the engine frees up
            hudDismissed_ = false;
            lastControllerActivity_ = std::chrono::steady_clock::now();
            if (textEntry_.active) cancelKeyboard();
            actionMenu_.visible = false;
            smbModal_.active = false;
            smbModal_.editingField = SmbModalState::Field::None;
#ifdef PIXELVR_HAVE_FREETYPE
            browser_.setOpen(false);
#endif
            downloadsWindow_ = false;
            pendingDl_.clear();
            selectScreen(slot);
        }
        return;
    }
    if (!sameFile) {
        rememberPlaybackPosition(slot);  // the clip being replaced
        playbackMemory_.save();
        if (!fromQueue) sc.openQueueTries = 0;  // the queue's own retries keep counting
    }
    if (!sameFile) renderer_.clearScrubPreview();  // stale frame from the previous clip
    if (sc.webSuspended && !plexLoading_ && !sameFile) {  // another file replaces the Plex hand-off
        webHosts_[slot].stop();
        sc.webSuspended = false;
    }
    sc.open = false;
    sc.path = path;
    pluginNamePath_[slot].clear();  // re-read the reported name (a vault file may have been renamed)
    const bool privatePath = VaultManager::isPrivatePath(path) || sc.webSuspended;
    if (!sameFile) sc.fingerprint = privatePath ? std::string() : PlaybackMemory::fingerprintOf(path);
    const PlaybackMemory::Entry* remembered = privatePath ? nullptr : playbackMemory_.find(path, sc.fingerprint);
    VulkanRenderer::ProjectionMode proj = VulkanRenderer::ProjectionMode::Flat;
    VulkanRenderer::StereoMode stereo = VulkanRenderer::StereoMode::Mono;
    bool stereoFromName = false;
    detect_format(formatName(path), proj, stereo, stereoFromName);
    if (remembered != nullptr && remembered->projection >= 0 && remembered->stereo >= 0) {
        proj = static_cast<VulkanRenderer::ProjectionMode>(remembered->projection);
        stereo = rendererStereo(remembered->stereo);
    }

    renderer_.releaseVideoFrame(slot);  // free the previous clip's dma-buf before reopening
    if (!sameFile) renderer_.clearVideo(slot);
    if (slot == 0 && !sameFile) {
        renderer_.setSphereZoom(1.0f);  // reset the 360/180 viewpoint per clip
        renderer_.setSpherePitch(0.0f);
        renderer_.setSphereYawOffset(0.0f);
    }
    sc.engine.setPreferredAudioLanguage(settings_.audioLanguage());
    if (!sameFile) {  // a new file starts from automatic track selection
        sc.engine.setPreferredVideoStream(-1);
        sc.engine.setPreferredAudioStream(-1);
    }
    if (plexLoading_ && plexAudioStream_ >= 0) {  // Plex / Jellyfin: the audio track chosen on the page
        sc.engine.setPreferredAudioLanguage("");
        sc.engine.setPreferredAudioStream(plexAudioStream_);
    }
    // Frame-sequential 3D: a format picked by hand for this file decides; otherwise detect it.
    if (!sameFile) {
        sc.engine.setFrameSequentialMode(remembered == nullptr || remembered->stereo < 0 ? -1 : (remembered->stereo == 3 ? 1 : 0));
    }

    // Place the screen in the layout right away and close the hub so the user
    // sees the "Loading..." indicator on the screen while the background worker
    // opens the stream (especially over slow SMB / SFTP / HTTP connections).
    // (A reload of the playing file, e.g. an audio-track switch, keeps the format it plays in:
    // finishLoadMedia reads sc.projection / sc.stereo back.)
    if (!sameFile) {
        sc.projection = static_cast<int>(proj);
        sc.stereo = static_cast<int>(stereo);
        if (slot == 0) {
            renderer_.setProjectionMode(proj);
            renderer_.setStereoMode(stereo);
        } else {
            VulkanRenderer::ScreenPlacement pl = renderer_.screenPlacement(slot);
            if (!pl.active) {
                placeNewScreen(slot);
                pl = renderer_.screenPlacement(slot);
            }
            pl.active = true;
            pl.stereo = stereo;
            renderer_.setScreenPlacement(slot, pl);
        }
        if (proj == VulkanRenderer::ProjectionMode::Flat) joinArrangement(slot);
    }

    if (!fromQueue && !startingDeferredLoad_) {  // a track change of the queue leaves the UI alone
        hudDismissed_ = false;
        lastControllerActivity_ = std::chrono::steady_clock::now();
        if (textEntry_.active) cancelKeyboard();
        actionMenu_.visible = false;
        smbModal_.active = false;
        smbModal_.editingField = SmbModalState::Field::None;
#ifdef PIXELVR_HAVE_FREETYPE
        browser_.setOpen(false);
#endif
        downloadsWindow_ = false;
        pendingDl_.clear();
        selectScreen(slot);
    }

    std::string dispName = sc.webSuspended && !plexTitles_[slot].empty()
                               ? plexTitles_[slot]
                               : std::filesystem::path(formatName(path)).filename().string();
    if (dispName.empty()) dispName = std::filesystem::path(path).filename().string();
    renderer_.setScreenTitle(slot, "Loading " + dispName + "...", 60.0f);

    sc.openFromQueue = fromQueue;
    sc.openSameFile = sameFile;
    sc.openStartedAt = std::chrono::steady_clock::now();
    sc.opening = true;
    sc.openState.store(0, std::memory_order_release);
    sc.engine.resetAbort();
    sc.openThreadDone.store(false, std::memory_order_release);
    sc.openThread = std::thread([&sc, path] {
        const bool ok = sc.engine.open(path);
        sc.openState.store(ok ? 1 : 2, std::memory_order_release);
        sc.openThreadDone.store(true, std::memory_order_release);
    });
}

void App::pollMediaOpens(std::chrono::steady_clock::time_point now) {
    for (int slot = 0; slot < VulkanRenderer::kMaxVideoSlots; ++slot) {
        ScreenState& sc = screens_[slot];
        if (sc.openAbandoned) {
            // A cancelled open: once its thread is back, the engine is free again.
            if (sc.openThreadDone.load(std::memory_order_acquire)) {
                if (sc.openThread.joinable()) sc.openThread.join();
                sc.engine.close();
                sc.openState.store(0, std::memory_order_relaxed);
                sc.openAbandoned = false;
                if (sc.hasDeferredPlex) {
                    const BrowserHost::PlexPlay play = sc.deferredPlex;
                    sc.hasDeferredPlex = false;
                    startPlexPlay(slot, play);
                } else if (sc.hasDeferredLoad) {
                    const std::string next = sc.deferredPath;
                    const bool fromQueue = sc.deferredFromQueue;
                    sc.hasDeferredLoad = false;
                    sc.deferredPath.clear();
                    sc.opening = false;
                    sc.path.clear();  // a fresh load, not "the same file"
                    startingDeferredLoad_ = true;
                    loadMedia(next, slot, fromQueue);
                    startingDeferredLoad_ = false;
                }
            } else if (sc.hasDeferredLoad) {
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - sc.openStartedAt).count();
                std::string name = std::filesystem::path(formatName(sc.deferredPath)).filename().string();
                renderer_.setScreenTitle(slot, "Loading " + name + std::string(static_cast<std::size_t>((ms / 400) % 4), '.'), 2.0f);
            }
            continue;
        }
        if (!sc.opening) continue;
        const int st = sc.openState.load(std::memory_order_acquire);
        if (st == 0) {
            // Keep the loading indicator alive with animated dots while the background open runs.
            const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - sc.openStartedAt).count();
            const int dots = static_cast<int>((elapsedMs / 400) % 4);
            std::string dispName = sc.webSuspended && !plexTitles_[slot].empty()
                                       ? plexTitles_[slot]
                                       : std::filesystem::path(formatName(sc.path)).filename().string();
            if (dispName.empty()) dispName = std::filesystem::path(sc.path).filename().string();
            renderer_.setScreenTitle(slot, "Loading " + dispName + std::string(static_cast<std::size_t>(dots), '.'), 2.0f);
            continue;
        }
        if (sc.openThread.joinable()) sc.openThread.join();
        sc.opening = false;
        sc.openState.store(0, std::memory_order_relaxed);
        sc.open = (st == 1);
        renderer_.setScreenTitle(slot, std::string(), 0.0f);
        finishLoadMedia(slot);
    }
}

void App::finishLoadMedia(int slot) {
    ScreenState& sc = screens_[slot];
    const std::string path = sc.path;
    const bool fromQueue = sc.openFromQueue;
    const bool sameFile = sc.openSameFile;
    const bool isPlex = sc.openPlex;
    const BrowserHost::PlexPlay plexPlay = sc.openPlexPlay;
    const double explicitSeek = sc.openSeekPos;
    const bool startPaused = sc.openStartPaused;
    sc.openPlex = false;
    sc.openSeekPos = -1.0;
    sc.openStartPaused = false;

    if (!sc.open) {
        // Clear stale video from the previous clip so the renderer shows black
        // instead of the last decoded frame of the old file.
        const std::string why = sc.engine.lastError();
        lastLoadFailed_ = true;
        if (fromQueue) {  // the queue moves on to the next track
            PIXELVR_LOG_WARN("Failed to open %s: %s", path.c_str(), why.c_str());
            const int next = sc.queue.next(false);
            if (next >= 0 && sc.openQueueTries + 1 < sc.queue.size()) {
                ++sc.openQueueTries;
                playQueueTrack(slot, next);
            } else {
                closeMedia(slot);
            }
            return;
        }
        if (returnFromPlex(slot)) {
            PIXELVR_LOG_WARN("Failed to open %s: %s", path.c_str(), why.c_str());
            return;
        }
        closeMedia(slot);
        PIXELVR_LOG_WARN("Failed to open %s: %s", path.c_str(), why.c_str());
#ifdef PIXELVR_HAVE_FREETYPE
        if (why != "cancelled") {
            // Say so in the file browser instead of silently doing nothing.
            browser_.setStatus("Can't play " + std::filesystem::path(path).filename().string() + ": " + why);
            if (!fromQueue) browser_.setOpen(true);
        }
#endif
        return;
    }
    sc.openQueueTries = 0;
    sc.engine.setPaused(startPaused);

    const bool privatePath = VaultManager::isPrivatePath(path) || sc.webSuspended;
    const PlaybackMemory::Entry* remembered = privatePath ? nullptr : playbackMemory_.find(path, sc.fingerprint);
    VulkanRenderer::ProjectionMode proj = VulkanRenderer::ProjectionMode::Flat;
    VulkanRenderer::StereoMode stereo = VulkanRenderer::StereoMode::Mono;
    bool stereoFromName = false;
    detect_format(formatName(path), proj, stereo, stereoFromName);

    // Refine the stereo layout from the frame aspect ratio when the filename did
    // not state it: once the projection is a VR type the aspect is unambiguous
    // (360 mono 2:1, SBS 4:1, OU 1:1; 180 mono 1:1, SBS 2:1, OU 1:2).
    const int w = sc.engine.width();
    const int h = sc.engine.height();

    // If filename did not indicate VR, check if probed dimensions reveal VR180
    if (proj == VulkanRenderer::ProjectionMode::Flat && h > 0) {
        if (is_vr180_media(formatName(path), w, h)) {
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

    // No layout in the name: the file's own 3D / VR metadata, else what the
    // picture check found (now, or once the thumbnail worker has looked).
    // Skip opening additional SMB/SFTP/WebDAV connections for thumbnail/letterbox
    // probes while streaming a remote file so playback doesn't contend on smbGlobalLock().
    const bool remoteStream = isRemotePath(path);
    sc.formatPending = false;
    if (!stereoFromName && !sc.engine.audioOnly()) {
        const FormatHint meta = sc.engine.formatHint();
        int hintStereo = meta.stereo;
        if (meta.projection > 0 && proj == VulkanRenderer::ProjectionMode::Flat) {
            proj = static_cast<VulkanRenderer::ProjectionMode>(meta.projection);
            if (meta.stereo < 0) hintStereo = (meta.projection == 2) ? 1 : 0;  // VR180 is usually SBS
        }
        if (hintStereo < 0 && proj == VulkanRenderer::ProjectionMode::Flat &&
            stereo == VulkanRenderer::StereoMode::Mono) {
            ThumbnailCache::MediaMeta tm;
            if (thumbnailCache_.getMeta(path, tm) && tm.stereoHint >= 0) {
                hintStereo = tm.stereoHint;
            } else if (!sc.webSuspended && !remoteStream) {
                sc.formatPending = true;
                thumbnailCache_.request(path, true);
            }
        }
        if (hintStereo >= 0) {
            stereo = static_cast<VulkanRenderer::StereoMode>(hintStereo);
            if (hintStereo > 0) PIXELVR_LOG_INFO("Format from the file: stereo %d", hintStereo);
        }
    }
    // A format the viewer picked by hand for this file wins over detection.
    sc.formatByHand = false;
    if (remembered != nullptr && remembered->projection >= 0 && remembered->stereo >= 0) {
        proj = static_cast<VulkanRenderer::ProjectionMode>(remembered->projection);
        stereo = rendererStereo(remembered->stereo);
        sc.formatByHand = true;
        sc.formatPending = false;
    } else if (sameFile) {
        proj = static_cast<VulkanRenderer::ProjectionMode>(sc.projection);
        stereo = rendererStereo(sc.stereo);
    }
    // Dimension-based detection can turn a file immersive only after opening:
    // move it to the main screen in that case.
    if (slot > 0 && proj != VulkanRenderer::ProjectionMode::Flat) {
        sc.engine.close();
        sc.open = false;
        sc.path.clear();
        sc.muted = false;
        // Free the extra screen completely (frame, placement, layout position).
        renderer_.clearVideo(slot);
        VulkanRenderer::ScreenPlacement freed = renderer_.screenPlacement(slot);
        freed.active = false;
        renderer_.setScreenPlacement(slot, freed);
        leaveArrangement(slot);
        loadMedia(path, 0);
        return;
    }
    if (sameFile && sc.stereo == 3 && !sc.engine.frameSequential()) {
        renderer_.setScreenTitle(slot, "Not frame-sequential: needs a doubled frame rate (48 / 50 / 60 fps)", 6.0f);
        stereo = VulkanRenderer::StereoMode::Mono;
    }
    sc.projection = static_cast<int>(proj);
    sc.stereo = sc.engine.frameSequential() ? 3 : static_cast<int>(stereo);  // FS3D shows as its own format
    renderer_.setPixelAspect(slot, sc.engine.pixelAspect());  // non-square pixels: the display shape
    // Letterbox bars: measured in the background (not for the vault or remote network shares).
    sc.letterbox = 0.0f;
    renderer_.setLetterbox(slot, 0.0f);
    sc.letterboxPending = (!privatePath || sc.webSuspended) && !remoteStream;
    if (VaultManager::isPrivatePath(path)) sc.letterboxPending = false;
    if (sc.letterboxPending) letterboxDetector_.request(path);
    if (slot == 0) {
        renderer_.setProjectionMode(proj);
        renderer_.setStereoMode(stereo);
        if (proj != VulkanRenderer::ProjectionMode::Flat) {
            // Immersive plays alone: the other screens close (usually asked
            // first, see playFromBrowser; this catches a format found only
            // now, from the picture size).
            for (int i = 1; i < VulkanRenderer::kMaxVideoSlots; ++i) {
                if (screens_[i].open || screens_[i].opening) closeMedia(i);
            }
        }
    } else {
        VulkanRenderer::ScreenPlacement pl = renderer_.screenPlacement(slot);
        if (!pl.active) {
            placeNewScreen(slot);
            pl = renderer_.screenPlacement(slot);
        }
        pl.active = true;
        pl.stereo = stereo;
        renderer_.setScreenPlacement(slot, pl);
    }
    if (proj == VulkanRenderer::ProjectionMode::Flat) joinArrangement(slot);
    // Explicit seek (track switch / FS3D toggle) or resume where this file was left.
    if (explicitSeek > 0.5 && (sc.engine.duration() <= 0.0 || explicitSeek < sc.engine.duration())) {
        sc.engine.seek(explicitSeek);
    } else if (!sameFile && !fromQueue && remembered != nullptr && remembered->position > 0.0 &&
               (sc.engine.duration() <= 0.0 || remembered->position < sc.engine.duration())) {
        sc.engine.seek(remembered->position);
        PIXELVR_LOG_INFO("Resuming %s at %.1fs", path.c_str(), remembered->position);
    }
    // Audio files play through the folder they came from.
    if (sc.engine.audioOnly()) {
        if (!fromQueue || sc.queue.indexOf(path) < 0) buildMusicQueue(slot, path);
        else sc.queue.setCurrent(sc.queue.indexOf(path));
        const int curTrack = sc.queue.current();
        if (curTrack < sc.musicFirst || curTrack >= sc.musicFirst + VulkanRenderer::kMusicRows) {
            sc.musicFirst = std::max(0, curTrack - 2);  // keep the playing track in view
        }
    } else if (!sc.queue.empty()) {
        sc.queue.clear();
    }
    // The loaded screen becomes the selected one (publishes its track lists and
    // format to the settings / HUD). The queue moving on keeps the selection.
    if (!fromQueue || slot == selectedScreen_) selectScreen(slot);
    static const char* const kProj[] = {"flat", "360", "180"};
    static const char* const kStereo[] = {"mono", "SBS", "TB"};
    PIXELVR_LOG_INFO("Loaded %s on screen %d (%dx%d, %s/%s)", path.c_str(), slot + 1, w, h,
                     kProj[static_cast<int>(proj)],
                     kStereo[static_cast<int>(stereo)]);
    // Software decoding forced by someone else's decoder session: say so,
    // since at 4K+ that means stutter the viewer would otherwise blame on us.
    const std::string blockedBy = sc.engine.hardwareBlockedBy();
    if (!blockedBy.empty() && static_cast<long long>(w) * h >= 3840LL * 2000LL) {
        const std::string msg = "Software decoding: the hardware decoder is held by " + blockedBy +
                                " - close that video (Steam overlay / store) or restart Steam";
        renderer_.setScreenTitle(slot, msg, 12.0f);
        PIXELVR_LOG_WARN("%s", msg.c_str());
    }
    if (isPlex && sc.webSuspended) {
        if (plexPlay.offset > 0.5 && (sc.engine.duration() <= 0.0 || plexPlay.offset < sc.engine.duration() - 5.0)) {
            sc.engine.seek(plexPlay.offset);
        }
        const auto now = std::chrono::steady_clock::now();
        hudDismissed_ = false;  // the dock shows, with the hint above it
        lastControllerActivity_ = now;
        renderer_.setDockHint(plexPlay.service == "jellyfin" ? "Press Stop to return to Jellyfin"
                                                             : "Press Stop to return to Plex",
                              8.0f);
        renderer_.setScreenTitle(slot, plexPlay.episodeInfo, plexPlay.episodeInfo.empty() ? 0.0f : 5.0f);
        sc.plexReportAt = now + std::chrono::seconds(10);
        webHosts_[slot].plexTimeline("playing", static_cast<int64_t>(plexPlay.offset * 1000.0));
        PIXELVR_LOG_INFO("Plex: playing \"%s\" on screen %d", plexPlay.title.c_str(), slot + 1);
    }
}

void App::switchTracks(bool byLanguage) {
    if (cur().path.empty() || !cur().open) return;  // not while it is still opening (the open thread reads these)
    const double pos = cur().engine.position();
    const bool paused = cur().engine.paused();
    if (byLanguage) {
        cur().engine.setPreferredAudioLanguage(settings_.audioLanguage());
        cur().engine.setPreferredAudioStream(-1);
    } else {
        cur().engine.setPreferredAudioLanguage("");
        cur().engine.setPreferredAudioStream(settings_.selectedAudioStream());
    }
    cur().engine.setPreferredVideoStream(settings_.selectedVideoStream());
    loadMedia(cur().path);  // same file: keeps the preferences set above
    cur().openSeekPos = pos;
    cur().openStartPaused = paused;
}

void App::updateStats() {
    using clock = std::chrono::steady_clock;
    const auto now = clock::now();
    ++statsFrameAccum_;
    if (lastStatsUpdate_.time_since_epoch().count() == 0) {
        lastStatsUpdate_ = now;
        statsLastDecoded_ = cur().engine.decodedFrames();
        return;
    }
    const double elapsed =
        std::chrono::duration<double>(now - lastStatsUpdate_).count();
    if (elapsed < 0.5) {
        return;  // refresh the readout ~twice a second
    }

    const double fps = statsFrameAccum_ / elapsed;
    const uint64_t dec = cur().engine.decodedFrames();
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
        // Adaptive resolution: the size rendered this frame, of the swapchain's.
        const ViewSwapchain& esc = xr_.swapchains()[0];
        // What is really rendered per eye, and why: the panel is 2160x2160.
        const int rw = esc.renderWidth > 0 ? esc.renderWidth : esc.width;
        const int rh = esc.renderHeight > 0 ? esc.renderHeight : esc.height;
        if (esc.width <= 2160) {
            std::snprintf(buf, sizeof(buf), "Eye %dx%d (native)", rw, rh);
        } else if (xr_.renderScale().fixed()) {
            std::snprintf(buf, sizeof(buf), "Eye %dx%d (fixed, above native)", rw, rh);
        } else {
            std::snprintf(buf, sizeof(buf), "Eye %dx%d (adaptive, up to %dx%d)", rw, rh, esc.width, esc.height);
        }
        statsLines_.emplace_back(buf);
    }
    if (cur().web) {
        // Browser screen: Chromium decodes on the CPU.
        const BrowserHost::VideoInfo& vi = webHosts_[selectedScreen_].videoInfo();
        if (vi.width > 0) {
            std::string codec = vi.codecs.substr(0, vi.codecs.find(" ("));  // "av01.0.08M.08"
            std::snprintf(buf, sizeof(buf), "Web video %dx%d  %s (CPU)", vi.width, vi.height,
                          codec.empty() ? "?" : codec.c_str());
            statsLines_.emplace_back(buf);
            std::snprintf(buf, sizeof(buf), "Dropped %d / %d frames", vi.dropped, vi.frames);
            statsLines_.emplace_back(buf);
        }
    } else if (cur().open) {
        std::snprintf(buf, sizeof(buf), "Video %dx%d  %s", cur().engine.width(),
                      cur().engine.height(), cur().engine.codecName().c_str());
        statsLines_.emplace_back(buf);
        std::snprintf(buf, sizeof(buf), "Decode %.1f / %.2f fps", decFpsEma_,
                      cur().engine.frameRate());
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
    // GPU time per frame (timestamp queries, ~0.5 s average) and the eye
    // pass's MSAA / visibility-mask state.
    {
        const VulkanRenderer::GpuTimes& gt = renderer_.gpuTimesRecent();
        if (gt.frames > 0) {
            std::snprintf(buf, sizeof(buf), "GPU eye %.2f  mdl %.2f  ui %.2f  vid %.2f = %.2f ms", gt.eye,
                          gt.models, gt.ui, gt.video, gt.total);
            statsLines_.emplace_back(buf);
            char msaa[16];
            if (renderer_.eyeMsaaSamples() <= 1 || gt.msaaPasses == 0) {
                std::snprintf(msaa, sizeof(msaa), "off");
            } else if (gt.msaaPasses == gt.eyePasses) {
                std::snprintf(msaa, sizeof(msaa), "%dx", renderer_.eyeMsaaSamples());
            } else {
                std::snprintf(msaa, sizeof(msaa), "mixed");
            }
            std::snprintf(buf, sizeof(buf), "Eye MSAA %s  mask %s", msaa,
                          gt.maskPasses == 0 ? "off" : (gt.maskPasses == gt.eyePasses ? "on" : "partial"));
            statsLines_.emplace_back(buf);
        } else if (!renderer_.gpuTimingSupported()) {
            statsLines_.emplace_back("GPU times n/a");
        }
    }
    if (memTotalKb > 0) {
        std::snprintf(buf, sizeof(buf), "RAM %ld / %ld MB",
                      (memTotalKb - memAvailKb) / 1024, memTotalKb / 1024);
        statsLines_.emplace_back(buf);
    }
    const auto fbStats = xr_.viewFallbackStats();
    if (fbStats.zeroLayerFrames > 0 || fbStats.posePartialFrames > 0 || fbStats.poseFullFallbackFrames > 0) {
        std::snprintf(buf, sizeof(buf), "Fallback: 0-layer %llu, partial %llu, full %llu",
                      static_cast<unsigned long long>(fbStats.zeroLayerFrames),
                      static_cast<unsigned long long>(fbStats.posePartialFrames),
                      static_cast<unsigned long long>(fbStats.poseFullFallbackFrames));
        statsLines_.emplace_back(buf);
    }
    const uint64_t ycbcrRebuilds = renderer_.ycbcrRebuildCount();
    if (ycbcrRebuilds > 0) {
        std::snprintf(buf, sizeof(buf), "YCbCr Rebuilds: %llu",
                      static_cast<unsigned long long>(ycbcrRebuilds));
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

namespace {
// File-name order with numbers compared by value ("2 x" before "10 x").
bool naturalLess(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (std::isdigit(static_cast<unsigned char>(a[i])) && std::isdigit(static_cast<unsigned char>(b[j]))) {
            size_t ie = i, je = j;
            while (ie < a.size() && std::isdigit(static_cast<unsigned char>(a[ie]))) ++ie;
            while (je < b.size() && std::isdigit(static_cast<unsigned char>(b[je]))) ++je;
            const std::string na = a.substr(i, ie - i), nb = b.substr(j, je - j);
            const unsigned long long va = std::strtoull(na.c_str(), nullptr, 10);
            const unsigned long long vb = std::strtoull(nb.c_str(), nullptr, 10);
            if (va != vb) return va < vb;
            i = ie;
            j = je;
            continue;
        }
        const int ca = std::tolower(static_cast<unsigned char>(a[i]));
        const int cb = std::tolower(static_cast<unsigned char>(b[j]));
        if (ca != cb) return ca < cb;
        ++i;
        ++j;
    }
    return a.size() - i < b.size() - j;
}

std::string formatClock(double sec) {
    if (sec <= 0.0) return {};
    const int s = static_cast<int>(sec + 0.5);
    char buf[24];
    if (s >= 3600) std::snprintf(buf, sizeof(buf), "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
    else std::snprintf(buf, sizeof(buf), "%d:%02d", s / 60, s % 60);
    return buf;
}
}  // namespace

void App::buildMusicQueue(int slot, const std::string& path) {
#ifdef PIXELVR_HAVE_MEDIA
    // The audio files next to it: as listed in the file browser when it is
    // showing that folder (its sort order, SMB too), else from the disk.
    std::vector<std::string> paths;
    bool fromBrowser = false;
    for (const Browser::Entry& e : browser_.entries()) {
        if (e.path == path) fromBrowser = true;
    }
    if (fromBrowser) {
        for (const Browser::Entry& e : browser_.entries()) {
            if (!e.isDir && isAudioFile(e.path)) paths.push_back(e.path);
        }
    } else if (!path.empty() && path[0] == '/') {
        std::error_code ec;
        const std::filesystem::path dir = std::filesystem::path(path).parent_path();
        for (const auto& it : std::filesystem::directory_iterator(dir, ec)) {
            const std::string name = it.path().filename().string();
            if (!name.empty() && name[0] != '.' && it.is_regular_file(ec) && isAudioFile(name)) {
                paths.push_back(it.path().string());
            }
        }
        std::sort(paths.begin(), paths.end(), [](const std::string& a, const std::string& b) {
            return naturalLess(std::filesystem::path(a).filename().string(), std::filesystem::path(b).filename().string());
        });
    }
    if (std::find(paths.begin(), paths.end(), path) == paths.end()) paths = {path};
    const int at = static_cast<int>(std::find(paths.begin(), paths.end(), path) - paths.begin());
    screens_[slot].queue.set(paths, at);
    PIXELVR_LOG_INFO("Music: %d tracks queued on screen %d", static_cast<int>(paths.size()), slot + 1);
#else
    (void)slot;
    (void)path;
#endif
}

void App::playQueueTrack(int slot, int index) {
#ifdef PIXELVR_HAVE_MEDIA
    MusicQueue& q = screens_[slot].queue;
    if (index >= 0 && index < q.size()) {
        const std::string next = q.track(index).path;
        if (!next.empty()) {
            q.setCurrent(index);
            loadMedia(next, slot, true);
            return;
        }
    }
    closeMedia(slot);  // nothing in the queue plays
#else
    (void)slot;
    (void)index;
#endif
}

void App::musicPrevious(int slot) {
#ifdef PIXELVR_HAVE_MEDIA
    ScreenState& sc = screens_[slot];
    // Like any player: back to the start, unless just started.
    const int prev = sc.queue.previous();
    if (sc.engine.position() > 3.0 || prev < 0 || prev == sc.queue.current()) sc.engine.seek(0.0);
    else playQueueTrack(slot, prev);
#else
    (void)slot;
#endif
}

void App::musicNext(int slot) {
#ifdef PIXELVR_HAVE_MEDIA
    const int next = screens_[slot].queue.next(false);
    if (next >= 0) playQueueTrack(slot, next);
#else
    (void)slot;
#endif
}

void App::updateMusicView(int slot) {
#ifdef PIXELVR_HAVE_MEDIA
    ScreenState& sc = screens_[slot];
    VulkanRenderer::MusicView v;
    if (!sc.open || sc.web || !sc.engine.audioOnly()) {
        renderer_.setMusicView(slot, v);
        return;
    }
    const MusicQueue& q = sc.queue;
    const int count = q.size();
    const int current = q.current();
    const AudioTags& tags = sc.engine.audioTags();
    const MusicQueue::Track playing = q.track(current);
    v.active = true;
    v.title = !tags.title.empty() ? tags.title : playing.title;
    v.artist = tags.artist;
    v.album = tags.album;
    v.count = count;
    v.shuffle = q.shuffle();
    v.repeat = static_cast<int>(q.repeat());
    v.paused = sc.engine.paused();
    const double total = q.totalDuration();
    v.queueInfo = std::to_string(count) + (count == 1 ? " track" : " tracks");
    if (total > 0.0) v.queueInfo += "  \xB7  " + formatClock(total);
    sc.musicFirst = std::clamp(sc.musicFirst, 0, std::max(0, count - VulkanRenderer::kMusicRows));
    v.first = sc.musicFirst;
    for (int i = v.first; i < std::min(count, v.first + VulkanRenderer::kMusicRows); ++i) {
        const MusicQueue::Track t = q.track(i);
        v.rows.push_back({t.title, t.artist, formatClock(t.duration), i == current});
    }
    renderer_.setMusicView(slot, v);
#else
    (void)slot;
#endif
}

void App::rememberPlaybackPosition(int slot) {
#ifdef PIXELVR_HAVE_MEDIA
    if (slot < 0) slot = selectedScreen_;
    ScreenState& sc = screens_[slot];
    if (!sc.open || sc.web || sc.webSuspended || sc.path.empty() || VaultManager::isPrivatePath(sc.path)) {
        return;
    }
    // Songs start from the top; long audio (audiobooks, mixes) resumes.
    if (sc.engine.audioOnly() && sc.engine.duration() < 20.0 * 60.0) return;
    playbackMemory_.setPosition(sc.path, sc.engine.position(), sc.engine.duration(), sc.fingerprint);
#else
    (void)slot;
#endif
}

void App::clearSlotKeepLayout(int slot) {
    const auto it = std::find(arrangement_.order.begin(), arrangement_.order.end(), slot);
    const int pos = (it != arrangement_.order.end()) ? static_cast<int>(it - arrangement_.order.begin()) : -1;
    const bool wasArranged = arrangementActive_;
    const bool wasBroken = arrangementBroken_;
    closeMedia(slot);  // takes it out of the layout
    if (pos >= 0 && wasArranged) {
        std::vector<int>& o = arrangement_.order;
        o.insert(o.begin() + std::min<int>(pos, static_cast<int>(o.size())), slot);
        arrangementActive_ = true;
        arrangementBroken_ = wasBroken;
        applyArrangement(arrangement_);
    }
}

void App::closeMedia(int slot) {
    if (slot < 0) slot = selectedScreen_;
    ScreenState& sc = screens_[slot];
#ifdef PIXELVR_HAVE_MEDIA
    cancelPendingOpen(slot);
#endif
    renderer_.setScreenTitle(slot, std::string(), 0.0f);
    sc.letterbox = 0.0f;
    sc.letterboxPending = false;
    renderer_.setLetterbox(slot, 0.0f);
    renderer_.setPixelAspect(slot, 1.0f);
    if (sc.webSuspended) {
#ifdef PIXELVR_HAVE_MEDIA
        webHosts_[slot].plexTimeline("stopped", sc.openAbandoned ? 0 : static_cast<int64_t>(sc.engine.position() * 1000.0));
#endif
        webHosts_[slot].stop();
        renderer_.setDockHint(std::string(), 0.0f);
    }
    if (sc.web) {
        if (kbWebSlot_ == slot) cancelKeyboard();
        webHosts_[slot].stop();
        renderer_.setWebSlot(slot, false);
        sc.web = false;
        if (webMouseSlot_ == slot) {
            webMouseSlot_ = -1;
            webMouseDown_ = false;
        }
    } else {
#ifdef PIXELVR_HAVE_MEDIA
        rememberPlaybackPosition(slot);
        playbackMemory_.save();
        if (slot == selectedScreen_) scrubPreview_.setSource(std::string(), 0);
        if (!sc.openAbandoned) sc.engine.close();  // else: closed when that open returns (pollMediaOpens)
        // Hand the last frame back to the hardware decoder, so it can free
        // the closed clip's buffers (else the next big video finds no room).
        renderer_.releaseVideoFrame(slot);
#endif
    }
    sc.webSuspended = false;
#ifdef PIXELVR_HAVE_MEDIA
    sc.queue.clear();
#endif
    sc.musicFirst = 0;
    renderer_.setMusicView(slot, {});
    sc.open = false;
    sc.path.clear();
    sc.muted = false;  // the next clip on this screen starts audible
    sc.projection = 0;
    sc.stereo = 0;
    renderer_.clearVideo(slot);
    if (slot == 0) {
        renderer_.setProjectionMode(VulkanRenderer::ProjectionMode::Flat);
        renderer_.setStereoMode(VulkanRenderer::StereoMode::Mono);
    } else {
        VulkanRenderer::ScreenPlacement pl = renderer_.screenPlacement(slot);
        pl.active = false;
        renderer_.setScreenPlacement(slot, pl);
    }
    if (slot == selectedScreen_) {
        renderer_.clearScrubPreview();
        // Hand control to another open screen (main first), else stay on main.
        int next = 0;
        for (int i = 0; i < VulkanRenderer::kMaxVideoSlots; ++i) {
            if (screens_[i].open || screens_[i].opening) {
                next = i;
                break;
            }
        }
        selectScreen(next);
    }
    leaveArrangement(slot);
}

void App::beginWebPlay(int slot, const BrowserHost::PlexPlay& play) {
    // Wake the server on a worker (first bytes of the stream), then startPlexPlay() opens it on
    // the main thread quickly. Without this the open blocked the frame loop while the server's
    // disk spun up (user report 2026-11-08: Jellyfin froze the headset ~15 s).
    for (size_t p = 0; p < pendingWebPlays_.size(); ++p) {
        if (pendingWebPlays_[p].slot == slot) pendingWebPlays_.erase(pendingWebPlays_.begin() + static_cast<long>(p--));
    }
    PendingWebPlay pw;
    pw.slot = slot;
    pw.play = play;
    pw.since = std::chrono::steady_clock::now();
    pw.state = std::make_shared<std::atomic<int>>(0);
    std::thread([url = play.url, state = pw.state] {
        std::string err;
        bool ok = false;
        if (std::unique_ptr<RemoteFile> f = remoteOpen(url, err)) {
            std::vector<uint8_t> buf(64 * 1024);
            ok = f->read(buf.data(), static_cast<int>(buf.size())) > 0;
        }
        state->store(ok ? 1 : 2);
    }).detach();
    pendingWebPlays_.push_back(std::move(pw));
    renderer_.setScreenTitle(slot, play.service == "jellyfin" ? "Waiting for Jellyfin to start the video..."
                                                             : "Waiting for Plex to start the video...",
                             90.0f);
    PIXELVR_LOG_INFO("Web play: waiting for the server (\"%s\")", play.title.c_str());
}

void App::startPlexPlay(int slot, const BrowserHost::PlexPlay& play) {
#ifdef PIXELVR_HAVE_MEDIA
    ScreenState& sc = screens_[slot];
    cancelPendingOpen(slot);  // e.g. a track-switch reload still running
    if (sc.openAbandoned) {
        // A cancelled open still holds this screen's engine: the hand-off starts when it lets go
        // (with its whole context: the page stays, Stop returns to it).
        sc.hasDeferredPlex = true;
        sc.deferredPlex = play;
        return;
    }
    if (kbWebSlot_ == slot) cancelKeyboard();
    if (webMouseSlot_ == slot) {
        webMouseSlot_ = -1;
        webMouseDown_ = false;
    }
    renderer_.setWebSlot(slot, false);
    sc.web = false;
    sc.webSuspended = true;
    sc.path.clear();
    sc.plexEpisode = play.episode;
    sc.plexService = play.service == "jellyfin" ? "jellyfin" : "plex";
    sc.plexAwaitingNext = false;
    sc.plexNavManual = false;
    webHosts_[slot].takePlexNextNone();  // a stale answer must not end this one
    plexTitles_[slot] = play.title;
    plexFiles_[slot] = play.file;
    plexLoading_ = true;
    plexAudioStream_ = play.audioStream;
    loadMedia(play.url, slot);
    plexLoading_ = false;
    plexAudioStream_ = -1;
    sc.openPlex = true;
    sc.openPlexPlay = play;
    if (play.audioStream >= 0) PIXELVR_LOG_INFO("Web play: audio track %d as chosen on the page", play.audioStream);
#else
    (void)slot;
    (void)play;
#endif
}

bool App::returnFromPlex(int slot) {
#ifdef PIXELVR_HAVE_MEDIA
    if (slot < 0) slot = selectedScreen_;
    ScreenState& sc = screens_[slot];
    if (!sc.webSuspended) return false;
    cancelPendingOpen(slot);
    renderer_.setScreenTitle(slot, std::string(), 0.0f);
    sc.plexAwaitingNext = false;  // a next episode still on its way is dropped
    for (size_t p = 0; p < pendingWebPlays_.size(); ++p) {
        if (pendingWebPlays_[p].slot == slot) pendingWebPlays_.erase(pendingWebPlays_.begin() + static_cast<long>(p--));
    }
    webHosts_[slot].plexReturn(sc.openAbandoned ? 0 : static_cast<int64_t>(sc.engine.position() * 1000.0));
    renderer_.setDockHint(std::string(), 0.0f);
    if (!sc.openAbandoned) sc.engine.close();
    renderer_.releaseVideoFrame(slot);
    renderer_.clearVideo(slot);
    sc.webSuspended = false;
    sc.open = true;
    sc.web = true;
    sc.path = kWebPath;
    sc.projection = 0;
    sc.stereo = 0;
    if (slot == 0) {
        renderer_.setProjectionMode(VulkanRenderer::ProjectionMode::Flat);
        renderer_.setStereoMode(VulkanRenderer::StereoMode::Mono);
    } else {
        VulkanRenderer::ScreenPlacement pl = renderer_.screenPlacement(slot);
        pl.stereo = VulkanRenderer::StereoMode::Mono;
        renderer_.setScreenPlacement(slot, pl);
    }
    renderer_.setWebSlot(slot, true);
    if (arrangementActive_ && inArrangement(slot)) applyArrangement(arrangement_);
    selectScreen(slot);
    webMouseDown_ = false;
    webHoverValid_ = false;
    return true;
#else
    (void)slot;
    return false;
#endif
}

bool App::updatePlexPlayback(int slot) {
#ifdef PIXELVR_HAVE_MEDIA
    ScreenState& sc = screens_[slot];
    const double duration = sc.engine.duration();
    const auto now = std::chrono::steady_clock::now();
    if (sc.plexAwaitingNext && sc.plexNavManual) {
        // The screen's arrow asked for another episode: none (or no answer) keeps this one playing.
        const bool none = webHosts_[slot].takePlexNextNone();
        if (none || now - sc.plexNextSince > std::chrono::seconds(20)) {
            sc.plexAwaitingNext = sc.plexNavManual = false;
            renderer_.setScreenTitle(slot, none ? "There is no such episode" : "The server did not answer", 3.0f);
        }
    }
    if (sc.engine.eof() && (duration <= 0.0 || sc.engine.position() >= duration - 5.0)) {
        if (sc.plexEpisode && !sc.plexAwaitingNext) {
            // An episode ended: the helper reports it and hands off the next one on this screen.
            sc.plexAwaitingNext = true;
            sc.plexNextSince = now;
            webHosts_[slot].plexNext(static_cast<int64_t>((duration > 0.0 ? duration : sc.engine.position()) * 1000.0));
            renderer_.setScreenTitle(slot, "Starting the next episode...", 20.0f);
            PIXELVR_LOG_INFO("Web play: episode ended on screen %d, asking for the next one", slot + 1);
            return false;
        }
        if (sc.plexAwaitingNext) {
            if (!webHosts_[slot].takePlexNextNone() && now - sc.plexNextSince < std::chrono::seconds(20)) return false;
            PIXELVR_LOG_INFO("Web play: no next episode, back to the page");
            sc.plexAwaitingNext = false;
            renderer_.setScreenTitle(slot, std::string(), 0.0f);
        }
        returnFromPlex(slot);
        return true;
    }
    if (now >= sc.plexReportAt) {
        sc.plexReportAt = now + std::chrono::seconds(10);
        webHosts_[slot].plexTimeline(sc.engine.paused() ? "paused" : "playing",
                                     static_cast<int64_t>(sc.engine.position() * 1000.0));
    }
#else
    (void)slot;
#endif
    return false;
}

void App::performVaultLock() {
    VaultManager::instance().lock();
    renderer_.setVaultUnlocked(false);
    actionMenu_.visible = false;
    renderer_.setActionMenu({});

#ifdef PIXELVR_HAVE_MEDIA
    for (int slot = 0; slot < VulkanRenderer::kMaxVideoSlots; ++slot) {
        if ((screens_[slot].open || screens_[slot].opening) && VaultManager::isPrivatePath(screens_[slot].path)) {  // any encrypted file too
            closeMedia(slot);
        } else if (screens_[slot].open && screens_[slot].web && !webHosts_[slot].kiosk() &&
                   webHosts_[slot].incognito() && !settings_.privateBrowsing()) {
            // Close browser screens that were opened in incognito while the vault was unlocked.
            closeMedia(slot);
        }
    }
#endif

#ifdef PIXELVR_HAVE_FREETYPE
    if (picking() && (VaultManager::isPrivatePath(pick_.srcPath) || browser_.inVault())) cancelPick(false);
    if (hubNoticePrivate_) hubNotice_.clear();  // e.g. "Moved 8 Mile.mkv to the vault"
    {
        // Leave the vault without popping the browser open over a video.
        const bool wasOpen = browser_.isOpen();
        if (browser_.inVault()) {
            browser_.openHome();
        } else if (browser_.currentLocation() == "bookmarks://") {
            browser_.openBookmarks();  // drop the vault bookmarks from the list
        }
        if (!wasOpen) browser_.setOpen(false);
    }
    browser_.clearFinished();
    downloadToast_ = {};
    downloadToastText_.clear();
    renderer_.clearCardThumbnails();
    thumbnailCache_.clearPrivateThumbs();
    badgeNameCache_.clear();
#endif
    vaultCardSeen_ = false;

    if (settings_.section() == 4 || settings_.section() == 20) {  // Vault & Privacy is hidden once locked
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

void App::closeHub() {
#ifdef PIXELVR_HAVE_FREETYPE
    if (picking()) cancelPick(false);
    browser_.setOpen(false);
#endif
    settings_.close();
    downloadsWindow_ = false;
    pendingDl_.clear();
    actionMenu_.visible = false;
    smbModal_.active = false;
    smbModal_.editingField = SmbModalState::Field::None;
    if (textEntry_.active) cancelKeyboard();
}

void App::cycleHubTab(int dir) {
#ifdef PIXELVR_HAVE_FREETYPE
    // Files, Downloads, Settings. Not the web shortcuts: landing on one would
    // open a page on every press. (Never Lock Vault or anything the vault hides.)
    const int n = 3;
    const int curTab = settings_.isOpen() ? 2 : (downloadsWindow_ ? 1 : 0);
    const int next = ((curTab + dir) % n + n) % n;
    if (next == 0) {  // the same as clicking the tab
        downloadsWindow_ = false;
        settings_.close();
        if (!browser_.isOpen()) browser_.open();
    } else if (next == 1) {
        settings_.close();
        browser_.setOpen(true);
        downloadsWindow_ = true;
    } else {
        downloadsWindow_ = false;
        settings_.open();
    }
    xr_.triggerHaptic(renderer_.pointerHand() == 0 ? 0 : 1, 0.015f, 0.0f, 0.4f);
#else
    (void)dir;
#endif
}

void App::selectNextScreen(int dir) {
    if (!multiScreenEnabled() || openScreenCount() < 2) return;
    constexpr int n = VulkanRenderer::kMaxVideoSlots;
    for (int k = 1; k < n; ++k) {
        const int s = ((selectedScreen_ + dir * k) % n + n) % n;
        if (screens_[s].open || screens_[s].opening) {
            selectScreen(s);
            return;
        }
    }
}

void App::cycleAudioTrack() {
#ifdef PIXELVR_HAVE_MEDIA
    if (cur().open && cur().engine.audioStreams().size() > 1) {
        const auto& streams = cur().engine.audioStreams();
        int curStream = cur().engine.currentAudioStream();
        int nextIdx = 0;
        for (size_t i = 0; i < streams.size(); ++i) {
            if (streams[i].index == curStream) {
                nextIdx = static_cast<int>((i + 1) % streams.size());
                break;
            }
        }
        int nextStreamIndex = streams[nextIdx].index;
        settings_.setSelectedAudioStream(nextStreamIndex);
        cur().engine.setPreferredAudioStream(nextStreamIndex);
        cur().engine.setPreferredAudioLanguage("");
        switchTracks(false);
        xr_.triggerHaptic(0, 0.02f, 0.0f, 0.5f);
        xr_.triggerHaptic(1, 0.02f, 0.0f, 0.5f);
    }
#endif
}

void App::openTracksMenu() {
#ifdef PIXELVR_HAVE_MEDIA
    if (!cur().open || cur().web) return;
    const auto audio = cur().engine.audioStreams();
    const auto video = cur().engine.videoStreams();
    if (audio.size() < 2 && video.size() < 2) return;
    actionMenu_.visible = true;
    actionMenu_.page = 11;
    actionMenu_.title = video.size() > 1 ? "Tracks" : "Audio Track";
    actionMenu_.subtitle = "The video reloads at the same position";
    actionMenu_.options.clear();
    trackRows_.clear();
    actionMenu_.selectedOption = 0;
    const int curAudio = cur().engine.currentAudioStream();
    const int curVideo = cur().engine.currentVideoStream();
    auto upper = [](std::string s) {
        for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return s;
    };
    // At most 10 rows fit the menu: audio first (what people switch), then video tracks.
    for (std::size_t i = 0; i < audio.size() && trackRows_.size() < 10; ++i) {
        const bool on = audio[i].index == curAudio;
        const std::string shortName = audio[i].language.empty() ? "Audio " + std::to_string(i + 1) : upper(audio[i].language);
        actionMenu_.options.push_back(shortName + "|" + audio[i].label + (on ? "  (playing)" : ""));
        if (on) actionMenu_.selectedOption = static_cast<int>(trackRows_.size());
        trackRows_.push_back({true, audio[i].index});
    }
    for (std::size_t i = 0; video.size() > 1 && i < video.size() && trackRows_.size() < 10; ++i) {
        const bool on = video[i].index == curVideo;
        actionMenu_.options.push_back("Video " + std::to_string(i + 1) + "|" + video[i].label + (on ? "  (playing)" : ""));
        trackRows_.push_back({false, video[i].index});
    }
    actionMenu_.options.push_back("Cancel");
    xr_.triggerHaptic(0, 0.020f, 0.0f, 0.4f);
    xr_.triggerHaptic(1, 0.020f, 0.0f, 0.4f);
#endif
}

void App::chooseFormat(int projection, int stereo) {
    formatStoredOnly_ = false;
#ifdef PIXELVR_HAVE_MEDIA
    const bool playing = formatMenuPath_.empty() || ((cur().open || cur().opening) && cur().path == formatMenuPath_);
    if (playing && applyFormat(projection, stereo)) return;
    if (formatMenuPath_.empty()) return;  // from the dock: nothing to store for
    // A file picked in the browser that is not on the selected screen: remembered for its next play.
    formatStoredOnly_ = true;
    if (VaultManager::isPrivatePath(formatMenuPath_)) return;  // nothing about vault files is stored
    playbackMemory_.setFormat(formatMenuPath_, projection, stereo, PlaybackMemory::fingerprintOf(formatMenuPath_));
    playbackMemory_.save();
    showNotice("Format saved: used the next time this file plays");
#else
    (void)projection;
    (void)stereo;
#endif
}

void App::openFormatMenu() {
    formatMenuPath_.clear();  // from the dock: the playing screen
    actionMenu_.visible = true;
    actionMenu_.page = 5;
    actionMenu_.title = "Playback Format";
    actionMenu_.subtitle = "Select 3D / VR Projection";
    actionMenu_.options.clear();
    // "short|full": the menu shows the short name and, beside it, what it stands for.
    actionMenu_.options.push_back("2D|Flat, standard video");
    actionMenu_.options.push_back("3D SBS|Side-by-side 3D");
    actionMenu_.options.push_back("3D TB|Top-bottom 3D (over-under)");
    actionMenu_.options.push_back("3D FS|Frame-sequential 3D (FS3D)");
    if (selectedScreen_ == 0) {  // immersive only on the main screen
        actionMenu_.options.push_back("VR180|180° 2D (mono)");
        actionMenu_.options.push_back("VR180 3D|180° side-by-side 3D");
        actionMenu_.options.push_back("VR360|360° 2D (mono)");
        actionMenu_.options.push_back("VR360 3D|360° top-bottom 3D");
        actionMenu_.options.push_back("VR360 SBS|360° side-by-side 3D");
    }
    actionMenu_.options.push_back("Cancel");
    actionMenu_.selectedOption = 0;
    xr_.triggerHaptic(0, 0.020f, 0.0f, 0.4f);
    xr_.triggerHaptic(1, 0.020f, 0.0f, 0.4f);
}

void App::downloadFocusedEntry() {
#ifdef PIXELVR_HAVE_FREETYPE
    // Folders ask for confirmation (with the measured size) first; plain
    // files download immediately.
    if (!browser_.isOpen() || !browser_.onNetwork()) return;
    const Browser::Entry* fe = browser_.focused();
    if (fe != nullptr && fe->isDir && !fe->isBack) {
        pendingDl_ = fe->path;
        pendingDlName_ = fe->label;
        pendingDlFresh_ = true;
        browser_.measureFolder(fe->path);
    } else {
        browser_.downloadFocused();
    }
#endif
}

void App::refreshControlsInfo() {
    if (xr_.bindingsGeneration() != bindingsGenSeen_ || !buttonsPageShown_) {
        PIXELVR_LOG_INFO("Controls page: re-reading bindings (gen %u)", xr_.bindingsGeneration());
    }
    std::vector<std::pair<std::string, std::string>> info;
    // The actions nothing works without: when SteamVR's active binding lacks one,
    // the page says how to get the default back.
    constexpr InputAction kEssential[] = {InputAction::Select, InputAction::PlayOk, InputAction::Back,
                                          InputAction::MainMenu};
    std::string missing;
    int missingCount = 0;
    for (int i = 0; i < static_cast<int>(InputAction::Count); ++i) {
        const InputAction a = static_cast<InputAction>(i);
        std::string bound = xr_.describeBinding(a);
        if (bound == "Not bound" && std::find(std::begin(kEssential), std::end(kEssential), a) != std::end(kEssential)) {
            missing += (missing.empty() ? "" : ", ") + std::string(XrContext::actionLabel(a));
            ++missingCount;
        }
        info.emplace_back(XrContext::actionLabel(a), std::move(bound));
    }
    std::string notice;
    if (!missing.empty()) {
        if (missingCount > 2) missing = "Main buttons";
        notice = missing + " not bound: choose the default binding in SteamVR > Controllers > "
                           "Manage Controller Bindings";
    } else if (const bool l = xr_.handActive(0), r = xr_.handActive(1); !l || !r) {
        notice = (!l && !r) ? std::string("Both controllers are off: turn them on to see their buttons")
                            : std::string(!l ? "Left" : "Right") +
                                  " controller is off: turn it on to see its buttons";
    }
    controlsWarn_ = !missing.empty();
    settings_.setControlsInfo(std::move(info), std::move(notice));
    bindingsGenSeen_ = xr_.bindingsGeneration();
}

void App::updateButtonLabels(std::chrono::steady_clock::time_point nowT, bool laserOnWeb) {
    const bool shown = settings_.buttonLabels();
    // Bound sources can only be listed after a focused sync; re-read when the
    // bindings generation moves, and every 2 s while the labels show (SteamVR
    // can switch bindings without telling the app). Cheap: one call per action.
    const bool stale = !labelSourcesRead_ || xr_.bindingsGeneration() != labelSourcesGen_ ||
                       (shown && nowT - labelSourcesReadT_ > std::chrono::seconds(2));
    if (stale && xr_.sessionFocused()) {
        labelSources_.assign(static_cast<std::size_t>(InputAction::Count), {});
        for (int i = 0; i < static_cast<int>(InputAction::Count); ++i) {
            labelSources_[static_cast<std::size_t>(i)] = xr_.boundSourcePaths(static_cast<InputAction>(i));
        }
        labelSourcesRead_ = true;
        labelSourcesGen_ = xr_.bindingsGeneration();
        labelSourcesReadT_ = nowT;
        // Which button each action's label goes to; logged when it changes.
        std::string mapping;
        for (int i = 0; i < static_cast<int>(InputAction::Count); ++i) {
            for (const std::string& p : labelSources_[static_cast<std::size_t>(i)]) {
                int hand = 1;
                std::string component, detail;
                mapping += mapping.empty() ? "" : "; ";
                mapping += std::string(XrContext::actionLabel(static_cast<InputAction>(i))) + " <- " + p;
                if (parseLabelSource(p, hand, component, detail)) {
                    mapping += " = " + std::string(hand == 0 ? "left " : "right ") + component +
                               (detail.empty() ? "" : " (" + detail + ")");
                } else {
                    mapping += " (no label)";
                }
            }
        }
        if (mapping != labelMappingLogged_) {
            PIXELVR_LOG_INFO("Button labels: %s", mapping.empty() ? "nothing bound yet" : mapping.c_str());
            labelMappingLogged_ = mapping;
        }
    }

    ButtonLabelState st;
    bool menus = settings_.isOpen() || downloadsWindow_ || actionMenu_.visible || !pendingDl_.empty() ||
                 smbModal_.active;
#ifdef PIXELVR_HAVE_FREETYPE
    menus = menus || browser_.isOpen();
#endif
    st.context = textEntry_.active ? ButtonLabelState::Context::Keyboard
                                   : (menus ? ButtonLabelState::Context::Menus : ButtonLabelState::Context::Watching);
    st.dialog = actionMenu_.visible || !pendingDl_.empty() || smbModal_.active;
    // Only the two-button delete confirm takes left / right; the other dialogs
    // step up / down only.
    st.dialogLeftRight = actionMenu_.visible && actionMenu_.page == 2 && pendingDl_.empty();
#ifdef PIXELVR_HAVE_FREETYPE
    // Options opens File Actions on the Files tab and closes an open action
    // menu; it does nothing in Settings, Downloads or the download confirm.
    st.optionsWork = !settings_.isOpen() && !downloadsWindow_ && pendingDl_.empty() &&
                     (actionMenu_.visible || browser_.isOpen());
    st.recenterWorks = browser_.isOpen() || downloadsWindow_ || settings_.isOpen();
    // Downloads (laser only) and the download confirm read neither stick nor D-pad.
    st.noStick = !pendingDl_.empty() ||
                 (downloadsWindow_ && !settings_.isOpen() && !actionMenu_.visible);
#else
    st.recenterWorks = settings_.isOpen();
#endif
    st.laserOnWeb = laserOnWeb;
    st.mediaOpen = cur().open;
    st.web = cur().open && cur().web;
#ifdef PIXELVR_HAVE_MEDIA
    st.music = cur().open && !cur().web && cur().engine.audioOnly();
#endif
    st.sphere = settings_.projectionMode() != 0;
    st.panShift = st.sphere && panShiftLatched_;
    st.controlsVisible = cur().open && !hudDismissed_ && renderer_.controlBarVisible();
    renderer_.setButtonLabels(shown, buildButtonLabelCards(labelSources_, st));
}

void App::openSmbModal(bool isNew, const std::string& smbPath) {
    actionMenu_.subtitle.clear();  // no stale error from a previous edit
    smbDeleteArmedUntil_ = {};
    confirmEditorDelete_ = false;
#ifdef PIXELVR_HAVE_FREETYPE
    smbModal_.active = true;
    smbModal_.isNew = isNew;
    smbModal_.editingField = SmbModalState::Field::None;

    if (isNew) {
        smbModal_.origHost.clear();
        smbModal_.origShare.clear();
        smbModal_.origProtocol = "smb";
        smbModal_.origPort = 0;
        smbModal_.protocol = "smb";
        smbModal_.port.clear();
        smbModal_.host.clear();
        smbModal_.share.clear();
        smbModal_.anon = false;
        smbModal_.user.clear();
        smbModal_.pass.clear();
        smbModal_.workgroup = "WORKGROUP";
        smbModal_.label.clear();
    } else {
        std::string protocol = "smb";
        int port = 0;
        std::string host, share;
        RemoteUri ru;
        if (parseRemoteUri(smbPath, ru)) {
            protocol = ru.protocol;
            port = ru.port;
            host = ru.host;
            share = ru.path;
            while (!share.empty() && share.front() == '/') share.erase(share.begin());
            while (!share.empty() && share.back() == '/') share.pop_back();
        } else {
            std::string rest = smbPath;
            if (rest.rfind("smb://", 0) == 0) rest = rest.substr(6);
            while (!rest.empty() && rest.back() == '/') rest.pop_back();
            auto slash = rest.find('/');
            host = (slash == std::string::npos) ? rest : rest.substr(0, slash);
            share = (slash == std::string::npos) ? "" : rest.substr(slash + 1);
        }

        smbModal_.origHost = host;
        smbModal_.origShare = share;
        smbModal_.origProtocol = protocol;
        smbModal_.origPort = port;
        smbModal_.protocol = protocol;
        smbModal_.port = port > 0 ? std::to_string(port) : std::string();
        smbModal_.host = host;
        smbModal_.share = share;
        smbModal_.anon = false;
        smbModal_.user.clear();
        smbModal_.pass.clear();
        smbModal_.workgroup = "WORKGROUP";
        smbModal_.label.clear();

        SmbShare existing;
        if (SmbCredentialsManager::instance().findShare(host, share, existing, protocol, port)) {
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
    smbDeleteArmedUntil_ = {};  // any other change in the editor disarms "press Delete again"
#ifdef PIXELVR_HAVE_FREETYPE
    actionMenu_.title = smbModal_.isNew ? "Add Network Share" : "Edit Network Share";
    if (actionMenu_.subtitle.rfind("Error", 0) != 0 || !smbModal_.host.empty()) {
        actionMenu_.subtitle = "Point at a field and pull the trigger to type  \xB7  B: back";
    }
    actionMenu_.options.clear();
    actionMenu_.editingRow = -1;
    actionMenu_.cursorPos = 0;

    const bool kbActive = textEntry_.active;
    const std::string& liveText = textEntry_.text;
    const int liveCur = static_cast<int>(textEntry_.cursor);

    // Plain values (the renderer shows placeholders for empty fields). Option
    // ids are fixed: 0 host, 1 share / folder, 2 auth ("guest"/"account"), 3 user,
    // 4 password, 5 workgroup (SMB) or port, 6 nickname, 7 save, 8 cancel,
    // 9 delete (empty for a new share), 10 type (SMB / SFTP / WebDAV / WebDAV (https)).
    auto fieldVal = [&](SmbModalState::Field f, int rowIdx, const std::string& saved, bool mask) {
        if (kbActive && smbModal_.editingField == f) {
            actionMenu_.editingRow = rowIdx;
            actionMenu_.cursorPos = liveCur;
            return mask ? std::string(liveText.length(), '*') : liveText;
        }
        return mask ? std::string(saved.length(), '*') : saved;
    };

    actionMenu_.options.push_back(fieldVal(SmbModalState::Field::Host, 0, smbModal_.host, false));
    actionMenu_.options.push_back(fieldVal(SmbModalState::Field::Share, 1, smbModal_.share, false));
    actionMenu_.options.push_back(smbModal_.anon ? "guest" : "account");
    actionMenu_.options.push_back(fieldVal(SmbModalState::Field::User, 3, smbModal_.user, false));
    actionMenu_.options.push_back(fieldVal(SmbModalState::Field::Pass, 4, smbModal_.pass, true));
    if (smbModal_.protocol == "smb") {
        actionMenu_.options.push_back(fieldVal(SmbModalState::Field::Workgroup, 5, smbModal_.workgroup, false));
    } else {
        actionMenu_.options.push_back(fieldVal(SmbModalState::Field::Port, 5, smbModal_.port, false));
    }
    actionMenu_.options.push_back(fieldVal(SmbModalState::Field::Label, 6, smbModal_.label, false));
    actionMenu_.options.push_back("Save");
    actionMenu_.options.push_back("Cancel");
    actionMenu_.options.push_back(smbModal_.isNew ? "" : "Delete");
    actionMenu_.options.push_back(smbModal_.protocol == "sftp"   ? "SFTP"
                                  : smbModal_.protocol == "dav"  ? "WebDAV"
                                  : smbModal_.protocol == "davs" ? "WebDAV (https)"
                                                                 : "SMB");
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
    const int port = smbModal_.protocol == "smb" ? 0 : std::atoi(smbModal_.port.c_str());
    if (port < 0 || port > 65535) {
        actionMenu_.subtitle = "Error: the port must be between 1 and 65535";
        return;
    }
    if (!smbModal_.isNew && (smbModal_.host != smbModal_.origHost || smbModal_.share != smbModal_.origShare ||
                             smbModal_.protocol != smbModal_.origProtocol || port != smbModal_.origPort)) {
        SmbCredentialsManager::instance().deleteShare(smbModal_.origHost, smbModal_.origShare, smbModal_.origProtocol,
                                                      smbModal_.origPort);
    }
    SmbShare s;
    s.protocol = smbModal_.protocol;
    s.port = port;
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
    if (textEntry_.active) cancelKeyboard();
    actionMenu_.visible = false;
    browser_.openNetwork();
#endif
}

void App::deleteShareEntry(const std::string& path) {
    RemoteUri ru;
    if (parseRemoteUri(path, ru)) {
        std::string folder = ru.path;
        while (!folder.empty() && folder.front() == '/') folder.erase(folder.begin());
        while (!folder.empty() && folder.back() == '/') folder.pop_back();
        SmbCredentialsManager::instance().deleteShare(ru.host, folder, ru.protocol, ru.port);
        return;
    }
    std::string rest = path;
    if (rest.rfind("smb://", 0) == 0) rest = rest.substr(6);
    while (!rest.empty() && rest.back() == '/') rest.pop_back();
    const auto slash = rest.find('/');
    SmbCredentialsManager::instance().deleteShare(slash == std::string::npos ? rest : rest.substr(0, slash),
                                                  slash == std::string::npos ? std::string() : rest.substr(slash + 1));
}

void App::deleteSmbModal() {
#ifdef PIXELVR_HAVE_FREETYPE
    if (!smbModal_.isNew) {
        SmbCredentialsManager::instance().deleteShare(smbModal_.origHost, smbModal_.origShare, smbModal_.origProtocol,
                                                      smbModal_.origPort);
    }
    smbModal_.active = false;
    smbModal_.editingField = SmbModalState::Field::None;
    if (textEntry_.active) cancelKeyboard();
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
    (void)in;
    kbOnDone_ = std::move(onDone);
    kbOnCancel_ = std::move(onCancel);
    textEntry_ = {};
    kbRepeat_ = {};
    kbFieldDrag_ = false;
    textEntry_.active = true;
    textEntry_.prompt = prompt;
    textEntry_.text = initialText;
    textEntry_.cursor = initialText.size();
    textEntry_.password = isPassword;
}

void App::openWebKeyboard(int slot, const BrowserHost::FocusEvent* field, const XrInputState& in) {
    openKeyboard("Typing on the page", field ? field->value : std::string(), field && field->password, in, nullptr);
    if (field) textEntry_.cursor = utf16ToByteOffset(field->value, field->cursor);
    kbWebSlot_ = slot;
}

void App::webCaretTo(std::size_t byteOffset) {
    if (kbWebSlot_ < 0) return;
    const std::string& t = textEntry_.text;
    const std::size_t from = std::min(textEntry_.cursor, t.size());
    const std::size_t to = std::min(byteOffset, t.size());
    int chars = 0;  // characters between the two (UTF-8 lead bytes)
    for (std::size_t i = std::min(from, to); i < std::max(from, to); ++i) {
        if ((static_cast<unsigned char>(t[i]) & 0xC0) != 0x80) ++chars;
    }
    for (int k = 0; k < chars; ++k) webHosts_[kbWebSlot_].key(to < from ? "Left" : "Right");
}

void App::commitKeyboard() {
    const std::string text = textEntry_.text;
    textEntry_ = {};
    kbWebSlot_ = -1;
    auto cb = std::move(kbOnDone_);
    kbOnDone_ = nullptr;
    kbOnCancel_ = nullptr;
    if (cb) cb(text);
    if (smbModal_.active) {
        smbModal_.editingField = SmbModalState::Field::None;
        updateSmbModalMenu();
    }
}

void App::cancelKeyboard() {
    textEntry_ = {};
    kbWebSlot_ = -1;
    auto cb = std::move(kbOnCancel_);
    kbOnDone_ = nullptr;
    kbOnCancel_ = nullptr;
    if (cb) cb();
    if (smbModal_.active) {
        smbModal_.editingField = SmbModalState::Field::None;
        updateSmbModalMenu();
    }
}

std::chrono::milliseconds App::repeatInterval(std::chrono::steady_clock::duration held) {
    using namespace std::chrono_literals;
    // ~11 steps/s at first, ~25 steps/s after a second of holding.
    return held < 1200ms ? 90ms : 40ms;
}

void App::moveTextCursor(int delta) {
    std::string& t = textEntry_.text;
    std::size_t& c = textEntry_.cursor;
    if (c > t.size()) c = t.size();
    if (kbWebSlot_ >= 0) {  // the page's caret moves too
        for (int k = 0; k < std::abs(delta); ++k) webHosts_[kbWebSlot_].key(delta < 0 ? "Left" : "Right");
    }
    for (; delta < 0 && c > 0; ++delta) {
        do { --c; } while (c > 0 && (static_cast<unsigned char>(t[c]) & 0xC0) == 0x80);
    }
    for (; delta > 0 && c < t.size(); --delta) {
        do { ++c; } while (c < t.size() && (static_cast<unsigned char>(t[c]) & 0xC0) == 0x80);
    }
}

void App::pressKeyboardKey(int key) {
    constexpr std::size_t kMaxChars = 256;
    std::string& t = textEntry_.text;
    std::size_t& c = textEntry_.cursor;
    if (c > t.size()) c = t.size();
    const int code = VulkanRenderer::keyboardKeyCode(key);
    if (kbWebSlot_ >= 0) {
        // Typing into a page: every key goes to it at once; our field mirrors
        // it (corrected by the page's own value right after).
        BrowserHost& host = webHosts_[kbWebSlot_];
        if (code == 0 || code == 4) {
            const std::string ch = VulkanRenderer::keyboardKeyText(key, textEntry_.shift, textEntry_.symbols);
            if (!ch.empty()) host.text(ch);
        } else if (code == 1) {
            host.key("Backspace");
        } else if (code == 7) {
            host.key("Enter");  // stays open while the field keeps focus
            return;
        }
    }
    switch (code) {
        case 0:    // character
        case 4: {  // space
            const std::string ch = VulkanRenderer::keyboardKeyText(key, textEntry_.shift, textEntry_.symbols);
            if (!ch.empty() && t.size() < kMaxChars) {
                t.insert(c, ch);
                c += ch.size();
                if (!textEntry_.symbols) textEntry_.shift = false;  // one-shot shift
            }
            break;
        }
        case 1: {  // backspace: remove the whole UTF-8 character before the cursor
            if (c == 0) break;
            std::size_t p = c;
            do { --p; } while (p > 0 && (static_cast<unsigned char>(t[p]) & 0xC0) == 0x80);
            t.erase(p, c - p);
            c = p;
            break;
        }
        case 2: textEntry_.shift = !textEntry_.shift; break;
        case 3: textEntry_.symbols = !textEntry_.symbols; break;
        case 5: moveTextCursor(-1); break;
        case 6: moveTextCursor(1); break;
        case 7: commitKeyboard(); break;
        case 8: cancelKeyboard(); break;
        default: break;
    }
}

// ---------------------------------------------------------------------------
// Dock transport (video engine or a browser screen's page video)
// ---------------------------------------------------------------------------
bool App::curPaused() {
    if (cur().web) {
        const BrowserHost::Media& m = webHosts_[selectedScreen_].media();
        return m.present && m.paused;
    }
#ifdef PIXELVR_HAVE_MEDIA
    return cur().engine.paused();
#else
    return false;
#endif
}

double App::curPosition() {
    if (cur().web) return webHosts_[selectedScreen_].media().position;
#ifdef PIXELVR_HAVE_MEDIA
    return cur().engine.position();
#else
    return 0.0;
#endif
}

double App::curDuration() {
    if (cur().web) {
        const BrowserHost::Media& m = webHosts_[selectedScreen_].media();
        if (!m.present) return 0.0;
        if (m.live) return (m.seekEnd - m.seekStart > 30.0) ? m.seekEnd : 0.0;  // live edge at the end
        return m.duration;
    }
#ifdef PIXELVR_HAVE_MEDIA
    return cur().engine.duration();
#else
    return 0.0;
#endif
}

double App::curTimelineStart() {
    if (cur().web) {
        const BrowserHost::Media& m = webHosts_[selectedScreen_].media();
        return (m.present && m.live) ? m.seekStart : 0.0;
    }
    return 0.0;
}

int App::curLive() {
    if (!cur().web) return 0;
    const BrowserHost::Media& m = webHosts_[selectedScreen_].media();
    if (!m.present || !m.live) return 0;
    // Like YouTube: "behind" once more than a few seconds from the edge.
    return (m.seekEnd <= m.seekStart || m.seekEnd - m.position < 15.0) ? 2 : 1;
}

void App::curTogglePause() {
    if (cur().web) {
        webHosts_[selectedScreen_].mediaToggle();
        return;
    }
#ifdef PIXELVR_HAVE_MEDIA
    cur().engine.togglePause();
#endif
}

// Streaming services run ads as a separate short video; seeking it from our
// timeline breaks the ad player and the film never starts. A kiosk page whose
// current video is under three minutes is left alone.
bool App::webSeekBlocked() const {
    const BrowserHost& host = webHosts_[selectedScreen_];
    const BrowserHost::Media& m = host.media();
    return host.kiosk() && m.present && !m.live && m.duration > 0.0 && m.duration < 180.0 &&
           host.startUrl().find("youtube.com") == std::string::npos;
}

void App::curSeek(double t) {
    scrubHoldPos_ = -1.0;  // a newer seek replaces any scrub display hold
    if (cur().web) {
        if (!webSeekBlocked()) webHosts_[selectedScreen_].mediaSeek(t);
        return;
    }
#ifdef PIXELVR_HAVE_MEDIA
    cur().engine.seek(t);
#endif
}

void App::curSeekRelative(double d) {
    scrubHoldPos_ = -1.0;  // a newer seek replaces any scrub display hold
    if (cur().web) {
        if (!webSeekBlocked()) webHosts_[selectedScreen_].mediaSeekRelative(d);
        return;
    }
#ifdef PIXELVR_HAVE_MEDIA
    cur().engine.seekRelative(d);
#endif
}

// ---------------------------------------------------------------------------
// Multi-screen helpers
// ---------------------------------------------------------------------------
bool App::multiScreenEnabled() const {
    return settings_.multiScreen();
}

int App::openScreenCount() const {
    int n = 0;
    for (const ScreenState& sc : screens_) n += (sc.open || sc.opening) ? 1 : 0;
    return n;
}

int App::freeScreenSlot() const {
    for (int slot = 1; slot < VulkanRenderer::kMaxVideoSlots; ++slot) {
        if (!screens_[slot].open && !screens_[slot].opening) return slot;
    }
    return -1;
}

void App::installPlugin(const std::string& file) {
    std::string message;
    const bool ok = plugins_.installPackage(file, message);
    PIXELVR_LOG_INFO("Plugin install: %s", message.c_str());
    // The result on the Plugins page (no floating text): the new plugin is listed there, still off.
    browser_.setOpen(false);
    settings_.openPluginsPage();
    settings_.showStatus(message);
    (void)ok;
}

void App::updatePlugins() {
    std::vector<PluginScreenSnapshot> snap(VulkanRenderer::kMaxVideoSlots);
    for (int slot = 0; slot < VulkanRenderer::kMaxVideoSlots; ++slot) {
        const ScreenState& sc = screens_[slot];
        PluginScreenSnapshot& s = snap[static_cast<size_t>(slot)];
        if (!sc.open || sc.path.empty()) {
            pluginNamePath_[slot].clear();
            pluginNameReal_[slot].clear();
        }
        s.selected = slot == selectedScreen_;
        s.audio = !sc.muted;
        if (sc.web && !sc.webSuspended) {
            s.kind = PluginScreenSnapshot::Kind::Web;
            continue;
        }
#ifdef PIXELVR_HAVE_MEDIA
        if (!sc.open || sc.path.empty()) continue;
        s.kind = PluginScreenSnapshot::Kind::Video;
        s.path = sc.path;
        const bool plex = sc.webSuspended;
        // Any encrypted file is a vault item, also one outside the vault folder (e.g. in a
        // folder moved out of the vault): plugins see it only with the vault grant.
        const bool encrypted = sc.path.size() > 5 &&
            sc.path.compare(sc.path.size() - 5, 5, vaultcrypto::kExtension) == 0;
        // Encrypted vault file: its real name (stored inside it), so scripts next to it
        // ("8 Mile.funscript") match by name. Read once per load, not every frame.
        if (pluginNamePath_[slot] != sc.path) {
            pluginNamePath_[slot] = sc.path;
            pluginNameReal_[slot] = PluginHost::sanitizePath(formatName(sc.path));
        }
        const std::string& clean = pluginNameReal_[slot];
        const std::string base = clean.substr(clean.find_last_of('/') + 1);
        s.fileName = plex ? (plexFiles_[slot].empty() ? plexTitles_[slot] : plexFiles_[slot]) : base;
        s.title = plex ? plexTitles_[slot] : base.substr(0, base.find_last_of('.'));
        if (plex) s.source = sc.plexService.empty() ? "plex" : sc.plexService;
        else if (sc.path.rfind("smb://", 0) == 0) s.source = "smb";
        else if (sc.path.rfind("sftp://", 0) == 0) s.source = "sftp";
        else if (sc.path.rfind("dav://", 0) == 0 || sc.path.rfind("davs://", 0) == 0) s.source = "webdav";
        else if (sc.path.find("://") != std::string::npos) s.source = "http";
        else s.source = "local";
        s.isPrivate = encrypted || VaultManager::isPrivatePath(sc.path);
        s.duration = sc.engine.duration();
        s.position = sc.engine.position();
        s.playing = !sc.engine.paused() && !sc.engine.eof();
        s.ended = sc.engine.eof();
        s.scrubbing = (slot == selectedScreen_) && pluginScrubbing_;
        s.projection = sc.projection;
        s.stereo = sc.stereo;
#endif
    }
    PluginAppSnapshot app;
    app.focused = xr_.sessionFocused();
    app.worn = xr_.userPresent();
    app.vaultUnlocked = VaultManager::instance().isUnlocked();
    plugins_.update(snap, app);
    renderer_.setTransportHeat(plugins_.heatBins(selectedScreen_, 160));
    renderer_.setHeatStyle(settings_.heatStyle());
    {
        std::string label;
        float value = -1.0f;
        plugins_.gauge(label, value);
        renderer_.setPluginGauge(label, value);
    }
    PluginHost::ControlCommand cmd;
    while (plugins_.takeControl(cmd)) {
#ifdef PIXELVR_HAVE_MEDIA
        if (cmd.screen < 0 || cmd.screen >= VulkanRenderer::kMaxVideoSlots || !screens_[cmd.screen].open) continue;
        MediaEngine& e = screens_[cmd.screen].engine;
        if (cmd.action == "play") e.setPaused(false);
        else if (cmd.action == "pause") e.setPaused(true);
        else if (cmd.action == "seek") e.seek(cmd.position);
#endif
    }
    const auto now = std::chrono::steady_clock::now();
    std::string n;
    {
        std::string file, error;
        if (pluginRepo_.takeReady(file, error)) {
            if (error.empty()) {
                installPlugin(file);  // verified download: installs, shows the result on the Plugins page
            } else {
                settings_.openPluginsPage();
                settings_.showStatus(error);
            }
        }
    }
    while (plugins_.takeNotice(n)) {
        // On the plugin pages' status line, and above the control panel (no floating text).
        settings_.showPluginNotice(n);
        if (!(settings_.isOpen() && settings_.onPluginPage())) renderer_.setDockHint(n, 5.0f);
    }
    static std::chrono::steady_clock::time_point nextPageRefresh{};
    if (settings_.isOpen() && settings_.onPluginPage() && now >= nextPageRefresh) {
        nextPageRefresh = now + std::chrono::milliseconds(500);
        settings_.refresh();  // plugin status lines change on their own
    }
}

void App::applyAudioFollow() {
    if (!settings_.audioFollow() || !multiScreenEnabled()) return;
    for (int s = 0; s < VulkanRenderer::kMaxVideoSlots; ++s) screens_[s].muted = (s != selectedScreen_);
}

void App::selectScreen(int slot) {
    if (slot < 0 || slot >= VulkanRenderer::kMaxVideoSlots) slot = 0;
    if (slot != selectedScreen_) renderer_.clearScrubPreview();
    selectedScreen_ = slot;
    applyAudioFollow();
    ScreenState& sc = cur();
    // The settings mirror always describes the selected screen, so the HUD,
    // format picker and track lists act on it.
    settings_.setProjectionMode(sc.projection);
    settings_.setStereoMode(sc.stereo);
#ifdef PIXELVR_HAVE_MEDIA
    if (sc.open && !sc.web && !sc.openAbandoned) {
        std::vector<std::pair<int, std::string>> vlist, alist;
        for (const auto& st : sc.engine.videoStreams()) vlist.emplace_back(st.index, st.label);
        for (const auto& st : sc.engine.audioStreams()) alist.emplace_back(st.index, st.label);
        settings_.setStreams(vlist, alist, sc.engine.currentVideoStream(),
                             sc.engine.currentAudioStream());
    }
#endif
}

bool App::applyFormat(int projection, int stereo) {
    if (selectedScreen_ > 0 && projection != 0) return false;  // extra screens are flat
    ScreenState& sc = cur();
#ifdef PIXELVR_HAVE_MEDIA
    // Frame-sequential on / off changes how the file is decoded: re-open it where it was.
    if (sc.open && !sc.web && (stereo == 3) != sc.engine.frameSequential()) {
        const double pos = sc.engine.position();
        const bool paused = sc.engine.paused();
        sc.projection = projection;
        sc.stereo = stereo;
        sc.engine.setFrameSequentialMode(stereo == 3 ? 1 : 0);
        loadMedia(sc.path, selectedScreen_);
        sc.openSeekPos = pos;
        sc.openStartPaused = paused;
        return true;
    }
#endif
    sc.projection = projection;
    sc.stereo = stereo;
    settings_.setProjectionMode(projection);
    settings_.setStereoMode(stereo);
    applyLetterbox(selectedScreen_);  // top-bottom 3D: no letterbox crop
    if (selectedScreen_ == 0) {
        renderer_.setProjectionMode(static_cast<VulkanRenderer::ProjectionMode>(projection));
        renderer_.setStereoMode(rendererStereo(stereo));
#ifdef PIXELVR_HAVE_MEDIA
        if (projection != 0) {  // immersive plays alone
            for (int i = 1; i < VulkanRenderer::kMaxVideoSlots; ++i) {
                if (screens_[i].open) screens_[i].engine.setPaused(true);
            }
        }
#endif
    } else {
        VulkanRenderer::ScreenPlacement pl = renderer_.screenPlacement(selectedScreen_);
        pl.stereo = rendererStereo(stereo);
        renderer_.setScreenPlacement(selectedScreen_, pl);
    }
    return true;
}

void App::placeNewScreen(int slot) {
    renderer_.setScreenPlacement(slot, newScreenPlacement(slot));
}

VulkanRenderer::ScreenPlacement App::newScreenPlacement(int slot) const {
    // On the same arc as the open screens, just past the outermost one on its
    // side (screen 2 left, screen 3 right), so it is never hidden behind one.
    const float* pivot = renderer_.screenPivot();
    const float baseYaw = renderer_.screenYaw();
    const float pitch = renderer_.recenterPitch();
    const float dist = renderer_.screenDistance();
    const float curv = renderer_.screenCurvature();
    constexpr float kScale = 0.9f;
    const bool left = (slot == 1);
    float edge = 0.0f;  // outer edge yaw, relative to the view
    bool any = false;
    for (int s = 0; s < VulkanRenderer::kMaxVideoSlots; ++s) {
        if (s == slot || !screens_[s].open) continue;
        const VulkanRenderer::ScreenPlacement& o = renderer_.screenPlacement(s);
        float yaw = baseYaw;
        float scale = 1.0f;
        float c = curv;
        float d = dist;
        if (o.active) {
            yaw = o.yaw;
            scale = o.scale;
            c = o.curvature;
            const float dx = o.center[0] - pivot[0];
            const float dz = o.center[2] - pivot[2];
            d = std::sqrt(dx * dx + dz * dz);
        }
        const ScreenExtent e = screenExtent(screenEyeAspect(s), scale, c);
        const float half = std::atan(e.hx / std::max(d - e.ez, 0.5f));
        const float rel = wrapAngle(yaw - baseYaw);
        const float outer = left ? rel + half : rel - half;
        if (!any || (left ? outer > edge : outer < edge)) edge = outer;
        any = true;
    }
    const ScreenExtent me = screenExtent(screenEyeAspect(slot), kScale, curv);
    const float myHalf = std::atan(me.hx / std::max(dist - me.ez, 0.5f));
    const float gapA = 2.0f * kArrangeGap / dist;
    float rel = 0.0f;
    if (any) rel = left ? edge + gapA + myHalf : edge - gapA - myHalf;
    const float yaw = baseYaw + std::clamp(rel, -2.6f, 2.6f);
    VulkanRenderer::ScreenPlacement pl;
    pl.active = true;
    pl.yaw = yaw;
    pl.pitch = pitch;
    pl.center[0] = pivot[0] - std::sin(yaw) * std::cos(pitch) * dist;
    pl.center[1] = pivot[1] + std::sin(pitch) * dist;
    pl.center[2] = pivot[2] - std::cos(yaw) * std::cos(pitch) * dist;
    pl.scale = kScale;
    pl.curvature = curv;
    pl.stereo = rendererStereo(screens_[slot].stereo);
    return pl;
}

void App::recenterView(const XrInputState& in) {
    const float oldPivot[3] = {renderer_.screenPivot()[0], renderer_.screenPivot()[1],
                               renderer_.screenPivot()[2]};
    const float oldYaw = renderer_.screenYaw();
    renderer_.recenterScreen(in.headPos, in.headForward);
    if (!multiScreenEnabled()) return;
    // Carry free-placed screens along: rotate them about the old view pivot by
    // the change in yaw, then move them with the pivot.
    const float* newPivot = renderer_.screenPivot();
    const float dYaw = renderer_.screenYaw() - oldYaw;
    const float c = std::cos(dYaw);
    const float sn = std::sin(dYaw);
    for (int slot = 0; slot < VulkanRenderer::kMaxVideoSlots; ++slot) {
        VulkanRenderer::ScreenPlacement pl = renderer_.screenPlacement(slot);
        if (!pl.active) continue;
        const float rx = pl.center[0] - oldPivot[0];
        const float ry = pl.center[1] - oldPivot[1];
        const float rz = pl.center[2] - oldPivot[2];
        pl.center[0] = newPivot[0] + rx * c + rz * sn;
        pl.center[1] = newPivot[1] + ry;
        pl.center[2] = newPivot[2] - rx * sn + rz * c;
        pl.yaw += dYaw;
        renderer_.setScreenPlacement(slot, pl);
    }
    if (arrangementActive_) {
        arrangement_.yaw = renderer_.screenYaw();
        arrangement_.pitch = renderer_.recenterPitch();
        applyArrangement(arrangement_);
    }
}

bool App::decoderBudgetAllows(const std::string& path) const {
    // Rough hardware decode budget in pixels per second (~8K30 / 2x 4K60).
    // TODO(M5): calibrate on the Steam Frame and refine per codec.
    constexpr double kBudget = 1.0e9;
    double used = 0.0;
#ifdef PIXELVR_HAVE_MEDIA
    for (const ScreenState& sc : screens_) {
        if (sc.opening && !sc.web) {  // still loading: what its thumbnail says, else 1080p30
            double est = 1920.0 * 1080.0 * 30.0;
#ifdef PIXELVR_HAVE_FREETYPE
            ThumbnailCache::MediaMeta m;
            if (const_cast<ThumbnailCache&>(thumbnailCache_).getMeta(sc.path, m) && m.width > 0) {
                est = static_cast<double>(m.width) * m.height * (m.fps > 0.0 ? m.fps : 30.0);
            }
#endif
            used += est;
            continue;
        }
        if (!sc.open) continue;
        const double fps = sc.engine.frameRate() > 0.0 ? sc.engine.frameRate() : 30.0;
        used += static_cast<double>(sc.engine.width()) * sc.engine.height() * fps;
    }
#endif
    double add = 1920.0 * 1080.0 * 30.0;  // unknown file: assume 1080p30
#ifdef PIXELVR_HAVE_FREETYPE
    ThumbnailCache::MediaMeta meta;
    if (const_cast<ThumbnailCache&>(thumbnailCache_).getMeta(path, meta) && meta.width > 0) {
        add = static_cast<double>(meta.width) * meta.height * (meta.fps > 0.0 ? meta.fps : 30.0);
    }
#else
    (void)path;
#endif
    PIXELVR_LOG_INFO("Decoder budget: %.0f + %.0f of %.0f Mpx/s", used / 1e6, add / 1e6, kBudget / 1e6);
    return used + add <= kBudget;
}

void App::openPlayTargetMenu(const std::string& path) {
    pendingPlayPath_ = path;
    actionMenu_.visible = true;
    actionMenu_.page = 6;
    actionMenu_.title = "Play on which screen?";
    actionMenu_.subtitle = std::filesystem::path(path).filename().string();
    // One entry per screen slot ("Replace" = playing now, "Add" = empty), then
    // Cancel; the renderer draws them as live preview tiles.
    actionMenu_.options.clear();
    for (int slot = 0; slot < VulkanRenderer::kMaxVideoSlots; ++slot) {
        actionMenu_.options.push_back((screens_[slot].open || screens_[slot].opening) ? "Replace" : "Add");
    }
    actionMenu_.options.push_back("Cancel");
    actionMenu_.selectedOption = selectedScreen_;
    // Show the screens where they are (arranged or freely placed), seen from
    // the view. Only while all of them are reasonably in front: a screen far
    // to the side or behind would squeeze the preview, so the plain row of
    // tiles is used instead.
    constexpr float kMaxYawOff = 1.22f;    // ~70 degrees
    constexpr float kMaxPitchOff = 0.87f;  // ~50 degrees
    const float* pivot = renderer_.screenPivot();
    const float anchorYaw = arrangementActive_ ? arrangement_.yaw : renderer_.screenYaw();
    const float anchorPitch = arrangementActive_ ? arrangement_.pitch : renderer_.recenterPitch();
    bool spatial = !(screens_[0].open && screens_[0].projection != 0);
    VulkanRenderer::ScreenPlacement pls[VulkanRenderer::kMaxVideoSlots];
    for (int slot = 0; slot < VulkanRenderer::kMaxVideoSlots && spatial; ++slot) {
        if (!screens_[slot].open && !screens_[slot].opening) continue;  // a loading screen has its place
        VulkanRenderer::ScreenPlacement pl = renderer_.screenPlacement(slot);
        if (!pl.active) {
            if (slot > 0) continue;  // not placed (should not happen for an open screen)
            // Main screen on its recenter-anchored pose.
            const float* fw = renderer_.screenForward();
            const float d = renderer_.screenDistance();
            for (int k = 0; k < 3; ++k) pl.center[k] = pivot[k] + fw[k] * d;
            pl.yaw = renderer_.screenYaw();
            pl.pitch = renderer_.recenterPitch();
            pl.scale = 1.0f;
            pl.curvature = renderer_.screenCurvature();
            pl.stereo = rendererStereo(screens_[slot].stereo);
            pl.active = true;
        }
        const float dx = pl.center[0] - pivot[0];
        const float dy = pl.center[1] - pivot[1];
        const float dz = pl.center[2] - pivot[2];
        const float len = std::max(std::sqrt(dx * dx + dy * dy + dz * dz), 1e-3f);
        const float yawOff = wrapAngle(std::atan2(-dx, -dz) - anchorYaw);
        const float pitchOff = std::asin(std::clamp(dy / len, -1.0f, 1.0f)) - anchorPitch;
        if (std::fabs(yawOff) > kMaxYawOff || std::fabs(pitchOff) > kMaxPitchOff) spatial = false;
        pls[slot] = pl;
    }
    if (spatial && !arrangementActive_) {
        // Free placement: empty slots appear as "+" tiles where a new screen
        // would open (screen 2 left of the others, screen 3 right).
        for (int slot = 1; slot < VulkanRenderer::kMaxVideoSlots; ++slot) {
            if (!screens_[slot].open) pls[slot] = newScreenPlacement(slot);
        }
    }
    if (spatial && arrangementActive_) {
        // Layout in use: show it as it will be once a screen is added, with
        // the next free slot as a "+" tile in the spot it will join (other free
        // slots are left out so the preview always matches the result).
        int freeSlot = -1;
        for (int slot = 0; slot < VulkanRenderer::kMaxVideoSlots; ++slot) {
            if (screens_[slot].open) continue;
            if (freeSlot < 0) freeSlot = slot;
            else actionMenu_.options[static_cast<std::size_t>(slot)] = "-";
        }
        if (freeSlot >= 0 && !inArrangement(freeSlot)) {
            Arrangement joined = arrangement_;
            addToArrangement(joined, freeSlot);  // as joinArrangement will: the preview matches
            computeArrangement(joined, pls);
        }
    }
    if (spatial) {
        renderer_.setArrangePreview(pls, pivot, anchorYaw, anchorPitch, -1, arrangementActive_ && arrangement_.snap);
    }
    renderer_.setPickerSpatial(spatial);
}

bool App::looksImmersive(const std::string& path) {
    // What is known before opening: a format picked by hand, the name, then
    // the thumbnail worker's probe (metadata or picture size).
    if (!VaultManager::isPrivatePath(path)) {
        const PlaybackMemory::Entry* rem = playbackMemory_.find(path, PlaybackMemory::fingerprintOf(path));
        if (rem != nullptr && rem->projection >= 0) return rem->projection != 0;
    }
    VulkanRenderer::ProjectionMode p = VulkanRenderer::ProjectionMode::Flat;
    VulkanRenderer::StereoMode st = VulkanRenderer::StereoMode::Mono;
    bool fromName = false;
    const std::string name = formatName(path);  // an encrypted file's real name, never the random one
    detect_format(name, p, st, fromName);
    if (p != VulkanRenderer::ProjectionMode::Flat) return true;
#ifdef PIXELVR_HAVE_FREETYPE
    ThumbnailCache::MediaMeta tm;
    if (thumbnailCache_.getMeta(path, tm)) {
        if (tm.projectionHint > 0) return true;
        if (tm.height > 0 && is_vr180_media(name, tm.width, tm.height)) return true;
    }
#endif
    return false;
}

void App::playFromBrowser(const std::string& path) {
    // With several screens available and something already playing, ask where.
    // While a 180/360 video plays it is alone, so a new file replaces it.
    const bool immersiveMain = screens_[0].open && screens_[0].projection != 0;
    if (immersiveMain) {
#ifdef PIXELVR_HAVE_MEDIA
        loadMedia(path, 0);
#endif
#ifdef PIXELVR_HAVE_FREETYPE
        if (!lastLoadFailed_) browser_.setOpen(false);
#endif
        return;
    }
    if (multiScreenEnabled() && openScreenCount() > 0 && looksImmersive(path)) {
        // 180/360 plays alone, so there is no screen to pick: say what happens.
        pendingPlayPath_ = path;
        actionMenu_.visible = true;
        actionMenu_.page = 10;
        actionMenu_.title = "Play the VR video?";
        actionMenu_.subtitle = openScreenCount() > 1
                                   ? "180° and 360° videos play on their own, so the other screens close."
                                   : "180° and 360° videos play on their own, so this replaces what is playing.";
        actionMenu_.options = {"Play", "Cancel"};
        actionMenu_.selectedOption = 0;
        return;
    }
    if (multiScreenEnabled() && openScreenCount() > 0) {
        openPlayTargetMenu(path);
        return;
    }
#ifdef PIXELVR_HAVE_MEDIA
    loadMedia(path);
#endif
#ifdef PIXELVR_HAVE_FREETYPE
    if (!lastLoadFailed_) browser_.setOpen(false);
#endif
}

void App::ensureScreenPlacement(int slot) {
    VulkanRenderer::ScreenPlacement pl = renderer_.screenPlacement(slot);
    if (pl.active) return;
    if (slot > 0) {
        placeNewScreen(slot);
        return;
    }
    // Main screen: take over its recenter-anchored pose.
    const float* pv = renderer_.screenPivot();
    const float* fw = renderer_.screenForward();
    const float d = renderer_.screenDistance();
    for (int k = 0; k < 3; ++k) pl.center[k] = pv[k] + fw[k] * d;
    pl.yaw = renderer_.screenYaw();
    pl.pitch = renderer_.recenterPitch();
    pl.scale = 1.0f;
    pl.curvature = renderer_.screenCurvature();
    pl.stereo = rendererStereo(screens_[slot].stereo);
    pl.active = true;
    renderer_.setScreenPlacement(slot, pl);
}

void App::startGrabbingScreen(int slot, const XrInputState& in) {
    ensureScreenPlacement(slot);
    const VulkanRenderer::ScreenPlacement& pl = renderer_.screenPlacement(slot);
    const int hand = renderer_.pointerHand() == 0 ? 0 : 1;
    const float* hit = renderer_.pointerScreenHit();
    const float* origin = in.controllers[hand].position;
    const float dx = hit[0] - origin[0];
    const float dy = hit[1] - origin[1];
    const float dz = hit[2] - origin[2];
    // Keep the grabbed point under the laser: remember its distance along the
    // ray and the screen centre's offset from it.
    movingDistance_ = std::clamp(std::sqrt(dx * dx + dy * dy + dz * dz), 0.8f, 8.0f);
    for (int k = 0; k < 3; ++k) {
        grabOffset_[k] = pl.center[k] - hit[k];
        grabStart_[k] = pl.center[k];
    }
    grabStartScale_ = pl.scale;
    movingScreen_ = slot;
}

void App::applyLetterbox(int slot) {
    if (slot < 0 || slot >= VulkanRenderer::kMaxVideoSlots) return;
    ScreenState& sc = screens_[slot];
    // Top-bottom 3D has bars inside each half: the whole-frame measurement does not fit it.
    const float lb = (settings_.removeBlackBars() && sc.open && sc.stereo != 2) ? sc.letterbox : 0.0f;
    renderer_.setLetterbox(slot, lb);
    if (lb > 0.0f) PIXELVR_LOG_INFO("Letterbox: screen %d leaves out %.1f %% top and bottom", slot + 1, lb * 100.0f);
    if (arrangementActive_ && inArrangement(slot)) applyArrangement(arrangement_);
}

float App::screenEyeAspect(int slot) const {
    float aspect = 16.0f / 9.0f;
#ifdef PIXELVR_HAVE_MEDIA
    const ScreenState& sc = screens_[slot];
    if (sc.open && sc.engine.width() > 0 && sc.engine.height() > 0) {
        aspect = sc.engine.pixelAspect() * static_cast<float>(sc.engine.width()) /
                 static_cast<float>(sc.engine.height());
        if ((sc.stereo == 1 || sc.stereo == 3) && aspect >= 2.6f) aspect *= 0.5f;  // full SBS (FS3D arrives as full SBS)
        else if (sc.stereo == 2 && aspect <= 1.2f) aspect *= 2.0f;  // full TB
        aspect /= std::max(0.4f, 1.0f - 2.0f * renderer_.letterboxFor(slot));  // black bars left out
    }
#else
    (void)slot;
#endif
    return aspect;
}

void App::computeArrangement(const Arrangement& a,
                             VulkanRenderer::ScreenPlacement out[VulkanRenderer::kMaxVideoSlots]) const {
    for (int slot = 0; slot < VulkanRenderer::kMaxVideoSlots; ++slot) out[slot] = {};
    const int n = static_cast<int>(a.order.size());
    if (n == 0) return;
    ArrangePreset preset = a.preset;
    if (preset == ArrangePreset::TwoBelowOneAbove && n != 3) preset = ArrangePreset::SideBySide;

    // Rows of positions (top row first, each left to right) and base sizes.
    std::vector<std::vector<int>> rows;
    std::vector<float> scale(static_cast<std::size_t>(n), 1.0f);
    int anchorPos = -1;  // position centred on the view (else the middle of its row)
    switch (preset) {
        case ArrangePreset::SideBySide: {
            rows.emplace_back();
            for (int i = 0; i < n; ++i) rows.back().push_back(i);
            std::fill(scale.begin(), scale.end(), n >= 3 ? 0.72f : (n == 2 ? 0.90f : 1.0f));
            // The middle screen sits dead centre (in line with the controls);
            // wider or narrower side screens extend outward from it.
            if (n == 3) anchorPos = 1;
            break;
        }
        case ArrangePreset::Stacked:
            for (int i = 0; i < n; ++i) rows.push_back({i});
            std::fill(scale.begin(), scale.end(), n >= 3 ? 0.58f : (n == 2 ? 0.75f : 1.0f));
            break;
        case ArrangePreset::TwoBelowOneAbove:
            rows = {{0}, {1, 2}};
            std::fill(scale.begin(), scale.end(), 0.66f);
            break;
        case ArrangePreset::Cinema:
            // Main screen (position 0) big in front, the others smaller beside it.
            if (n >= 3) rows = {{1, 0, 2}};
            else if (n == 2) rows = {{1, 0}};
            else rows = {{0}};
            std::fill(scale.begin(), scale.end(), 0.62f);
            scale[0] = 1.0f;
            anchorPos = 0;
            break;
    }

    const float gap = a.snap ? 0.0f : kArrangeGap;
    std::vector<ScreenExtent> ext(static_cast<std::size_t>(n));
    std::vector<float> curv(static_cast<std::size_t>(n));
    // Browser screens are taller than video screens of the same width (the
    // page plus its toolbar); rows must make room for that.
    std::vector<float> hFactor(static_cast<std::size_t>(n), 1.0f);
    for (int i = 0; i < n; ++i) {
        const int slot = a.order[static_cast<std::size_t>(i)];
        curv[i] = a.curvature;
        hFactor[i] = renderer_.webScreenHeightFactor(slot);
        ext[i] = screenExtent(screenEyeAspect(slot), scale[i] * a.scale, curv[i]);
        ext[i].hy *= hFactor[i];
    }
    // Curved screens on a curved wall form one big curve: each screen bends
    // with the wall's radius (a.dist, centred on the viewer) instead of its own.
    const bool wallCurve = !a.flat && a.curvature > 0.005f && n > 1;
    std::vector<float> arcHalf(static_cast<std::size_t>(n), 0.0f);
    if (wallCurve) {
        for (int i = 0; i < n; ++i) {
            const float aspect = screenEyeAspect(a.order[static_cast<std::size_t>(i)]);
            arcHalf[i] = screenExtent(aspect, scale[i] * a.scale, 0.0f).hx;  // half the arc length
            curv[i] = std::min(1.0f, 2.0f * arcHalf[i] / (1.75f * a.dist));
            ext[i] = screenExtent(aspect, scale[i] * a.scale, curv[i]);
            ext[i].hy *= hFactor[i];
        }
    }
    std::vector<float> rowHalf(rows.size(), 0.0f);
    for (std::size_t r = 0; r < rows.size(); ++r) {
        for (int i : rows[r]) rowHalf[r] = std::max(rowHalf[r], ext[i].hy);
    }
    auto biggest = [&](const std::vector<int>& row) {
        int big = row[0];
        for (int i : row) {
            if (ext[i].hx > ext[big].hx) big = i;
        }
        return big;
    };
    const float* pivot = renderer_.screenPivot();
    auto put = [&](int pos, const float c[3], float yaw, float pitch, float roll = 0.0f) {
        const int slot = a.order[static_cast<std::size_t>(pos)];
        VulkanRenderer::ScreenPlacement& pl = out[slot];
        pl.active = true;
        for (int k = 0; k < 3; ++k) pl.center[k] = c[k];
        pl.yaw = yaw;
        pl.pitch = pitch;
        pl.roll = roll;
        pl.scale = scale[pos] * a.scale;
        pl.curvature = curv[pos];
        pl.stereo = rendererStereo(screens_[slot].stereo);
    };

    if (!a.flat) {
        // Curved wall: a cylinder around the viewer. Within a row each screen
        // turns to face the viewer and neighbours sit exactly the angle apart
        // their edges subtend (snapped edges touch); rows stack straight up.
        // The wall is laid out level and then tilted by a.pitch as one piece,
        // so neighbours stay edge to edge when it sits above or below the eyes
        // (tilting each screen on its own would splay them apart).
        const float sp = 0.0f;
        const float cp = 1.0f;
        const float tsp = std::sin(a.pitch);
        const float tcp = std::cos(a.pitch);
        float total = gap * static_cast<float>(rows.size() - 1);
        for (float hh : rowHalf) total += 2.0f * hh;
        float y = 0.5f * total;
        for (std::size_t r = 0; r < rows.size(); ++r) {
            const float rowY = y - rowHalf[r];
            y -= 2.0f * rowHalf[r] + gap;
            const std::vector<int>& row = rows[r];
            // Horizontal distance of the side-edge midpoints from the viewer: the
            // row's largest screen sits at a.dist, smaller ones move back so
            // their edges are as far out as its edges and meet them.
            const int big = biggest(row);
            const float bz = std::max((a.dist - ext[big].ez) * cp - rowY * sp, 0.3f);
            const float edgeR2 = bz * bz + ext[big].hx * ext[big].hx;
            std::vector<float> di(row.size()), yawRel(row.size());
            const float gapY = wallCurve ? gap / a.dist : gap / bz;
            float yaw = 0.0f;
            float ref = 0.0f;
            for (std::size_t k = 0; k < row.size(); ++k) {
                const ScreenExtent& e = ext[row[k]];
                float half = 0.0f;
                if (wallCurve) {
                    // On the wall's cylinder: arc length / radius.
                    di[k] = a.dist;
                    half = arcHalf[row[k]] / a.dist;
                } else {
                    const float depth = std::sqrt(std::max(edgeR2 - e.hx * e.hx, 0.09f));
                    di[k] = e.ez + (depth + rowY * sp) / cp;
                    half = std::atan(e.hx / depth);
                }
                yawRel[k] = yaw - half;  // positive yaw = left, so walk toward negative
                yaw -= 2.0f * half + gapY;
                if (row[k] == anchorPos) ref = yawRel[k];
            }
            if (std::find(row.begin(), row.end(), anchorPos) == row.end()) ref = 0.5f * (yaw + gapY);
            for (std::size_t k = 0; k < row.size(); ++k) {
                // Pose = Ry(a.yaw) * Rx(a.pitch) * Ry(phi): turned phi around the
                // level wall, then the whole wall tilted and turned.
                const float phi = yawRel[k] - ref;
                const float sf = std::sin(phi), cf = std::cos(phi);
                const float sa = std::sin(a.yaw), ca = std::cos(a.yaw);
                auto toWorld = [&](float x, float y, float z, float o[3]) {
                    const float x1 = x * cf + z * sf, z1 = -x * sf + z * cf;   // Ry(phi)
                    const float y2 = y * tcp - z1 * tsp, z2 = y * tsp + z1 * tcp;  // Rx(pitch)
                    o[0] = x1 * ca + z2 * sa;                                    // Ry(a.yaw)
                    o[1] = y2;
                    o[2] = -x1 * sa + z2 * ca;
                };
                float lc[3], ex[3], ey[3], ez[3];
                toWorld(0.0f, rowY, -di[k], lc);
                toWorld(1.0f, 0.0f, 0.0f, ex);
                toWorld(0.0f, 1.0f, 0.0f, ey);
                toWorld(0.0f, 0.0f, 1.0f, ez);
                const float c[3] = {pivot[0] + lc[0], pivot[1] + lc[1], pivot[2] + lc[2]};
                // Back to yaw / pitch / roll (Ry * Rx * Rz): the screen normal
                // gives yaw and pitch, the right and up axes give the roll.
                const float yw = std::atan2(ez[0], ez[2]);
                const float pt = std::asin(std::clamp(-ez[1], -1.0f, 1.0f));
                const float rl = std::atan2(ex[1], ey[1]);
                put(row[k], c, yw, pt, rl);
            }
        }
    } else {
        // Flat wall: one plane facing the viewer, all screens turned alike.
        const float cy = std::cos(a.yaw), sy = std::sin(a.yaw);
        const float cp = std::cos(a.pitch), sp = std::sin(a.pitch);
        const float f[3] = {-sy * cp, sp, -cy * cp};
        const float rt[3] = {cy, 0.0f, -sy};
        const float up[3] = {rt[1] * f[2] - rt[2] * f[1], rt[2] * f[0] - rt[0] * f[2],
                             rt[0] * f[1] - rt[1] * f[0]};
        float total = gap * static_cast<float>(rows.size() - 1);
        for (float hh : rowHalf) total += 2.0f * hh;
        float y = 0.5f * total;
        for (std::size_t r = 0; r < rows.size(); ++r) {
            const float rowY = y - rowHalf[r];
            y -= 2.0f * rowHalf[r] + gap;
            const std::vector<int>& row = rows[r];
            const int big = biggest(row);
            std::vector<float> xs(row.size());
            float x = 0.0f;
            float ref = 0.0f;
            for (std::size_t k = 0; k < row.size(); ++k) {
                xs[k] = x + ext[row[k]].hx;
                x += 2.0f * ext[row[k]].hx + gap;
                if (row[k] == anchorPos) ref = xs[k];
            }
            if (std::find(row.begin(), row.end(), anchorPos) == row.end()) ref = 0.5f * (x - gap);
            for (std::size_t k = 0; k < row.size(); ++k) {
                // Curved screens: bring smaller ones forward so the side edges
                // of neighbours are equally near the viewer.
                const float toward = ext[big].ez - ext[row[k]].ez;
                const float lx = xs[k] - ref;
                float c[3];
                for (int j = 0; j < 3; ++j) {
                    c[j] = pivot[j] + f[j] * (a.dist - toward) + rt[j] * lx + up[j] * rowY;
                }
                put(row[k], c, a.yaw, a.pitch);
            }
        }
    }
}

void App::applyArrangement(const Arrangement& a) {
    VulkanRenderer::ScreenPlacement pls[VulkanRenderer::kMaxVideoSlots];
    computeArrangement(a, pls);
    for (int slot : a.order) renderer_.setScreenPlacement(slot, pls[slot]);
    arrangement_ = a;
    arrangementActive_ = !a.order.empty();
}

bool App::inArrangement(int slot) const {
    return std::find(arrangement_.order.begin(), arrangement_.order.end(), slot) != arrangement_.order.end();
}

void App::joinArrangement(int slot) {
    if (!multiScreenEnabled()) return;
    if (screens_[0].open && screens_[0].projection != 0) return;  // immersive plays alone
    if (!arrangementActive_) {
        if (arrangementBroken_) return;  // free placement until all screens close
        // Start the layout (side by side, edge to edge unless the user picked
        // another one) in front of the view with the screens already open.
        arrangement_.order.clear();
        int order[VulkanRenderer::kMaxVideoSlots];
        renderer_.screenOrderLeftToRight(order);
        for (int s : order) {
            if (s != slot && screens_[s].open) arrangement_.order.push_back(s);
        }
        arrangement_.yaw = renderer_.screenYaw();
        arrangement_.pitch = renderer_.recenterPitch();
        arrangement_.dist = renderer_.screenDistance();
        arrangement_.scale = 1.0f;
        arrangement_.curvature = renderer_.screenCurvature();
    }
    if (!inArrangement(slot)) addToArrangement(arrangement_, slot);
    applyArrangement(arrangement_);
}

void App::addToArrangement(Arrangement& a, int slot) const {
    // A third screen: two below, one above is the standard (three in a row is wide and small).
    if (a.order.size() == 2 && a.preset == ArrangePreset::SideBySide && !rowOfThreeChosen_) {
        a.preset = ArrangePreset::TwoBelowOneAbove;
    }
    if (a.preset == ArrangePreset::TwoBelowOneAbove && a.order.size() == 2) {
        // On top (position 0): the two playing screens stay side by side at {1, 2}.
        a.order.insert(a.order.begin(), slot);
    } else {
        a.order.push_back(slot);
    }
}

void App::leaveArrangement(int slot) {
    if (!inArrangement(slot)) return;
    std::vector<int>& o = arrangement_.order;
    o.erase(std::find(o.begin(), o.end(), slot));
    if (arrangementActive_ && !o.empty()) applyArrangement(arrangement_);
    else arrangementActive_ = false;
    if (o.empty()) arrangementBroken_ = false;  // next screens start a fresh layout
}

void App::moveArrangementTo(int slot, const float target[3]) {
    // A few fixed-point steps: shift the anchor by the direction / distance
    // error of the grabbed screen until it lands on the target.
    const float* pv = renderer_.screenPivot();
    auto polar = [&](const float p[3], float& yaw, float& pitch, float& r) {
        const float dx = p[0] - pv[0];
        const float dy = p[1] - pv[1];
        const float dz = p[2] - pv[2];
        r = std::max(std::sqrt(dx * dx + dy * dy + dz * dz), 1e-3f);
        yaw = std::atan2(-dx, -dz);
        pitch = std::asin(std::clamp(dy / r, -1.0f, 1.0f));
    };
    float ty = 0.0f, tp = 0.0f, tr = 0.0f;
    polar(target, ty, tp, tr);
    VulkanRenderer::ScreenPlacement pls[VulkanRenderer::kMaxVideoSlots];
    for (int it = 0; it < 4; ++it) {
        computeArrangement(arrangement_, pls);
        float y = 0.0f, p = 0.0f, r = 0.0f;
        polar(pls[slot].center, y, p, r);
        arrangement_.yaw += wrapAngle(ty - y);
        arrangement_.pitch = std::clamp(arrangement_.pitch + (tp - p), -1.2f, 1.2f);
        arrangement_.dist = std::clamp(arrangement_.dist + (tr - r), 1.0f, 10.0f);
    }
    applyArrangement(arrangement_);
}

void App::showChromiumDialog() {
    actionMenu_.visible = true;
    actionMenu_.page = 9;
    actionMenu_.title = "Chromium needed";
    actionMenu_.options.clear();
    switch (chromiumInstaller_.state()) {
        case ChromiumInstaller::State::Running: {
            const int pct = chromiumInstaller_.percent();
            // At 100% flatpak still sets the app up for a few seconds.
            actionMenu_.subtitle = pct >= 100 ? std::string("Finishing the install...")
                                   : pct >= 0 ? "Installing Chromium... " + std::to_string(pct) + "%"
                                              : std::string("Installing Chromium...");
            actionMenu_.options = {"Hide", "Cancel install"};
            break;
        }
        case ChromiumInstaller::State::Failed:
            actionMenu_.subtitle = "Install failed: " + chromiumInstaller_.error();
            actionMenu_.options = {"Try again", "Close"};
            break;
        default:
            actionMenu_.subtitle = "YouTube and the web browser use Chromium. Install it from Flathub (about 185 MB)?";
            actionMenu_.options = {"Install Chromium", "Cancel"};
            break;
    }
    if (actionMenu_.selectedOption >= static_cast<int>(actionMenu_.options.size())) actionMenu_.selectedOption = 0;
}

void App::openWebScreen(const std::string& url, bool kiosk, bool forceIncognito) {
    settings_.close();
    downloadsWindow_ = false;
    pendingWebUrl_ = url;
    pendingWebKiosk_ = kiosk;
    pendingWebIncognito_ = forceIncognito;
    // A 180/360 video plays alone: the page replaces it, like a new file does.
    const bool immersiveMain = screens_[0].open && screens_[0].projection != 0;
    if (!immersiveMain && multiScreenEnabled() && openScreenCount() > 0) {
        // The picker lives in the Files hub (it is dismissed without it), like
        // picking a screen for a file; choosing a screen closes the hub.
#ifdef PIXELVR_HAVE_FREETYPE
        browser_.setOpen(true);
#endif
        openPlayTargetMenu(kWebPath);
        const bool wantIncog = !kiosk && (forceIncognito || settings_.privateBrowsing() ||
                                          (VaultManager::instance().isUnlocked() && settings_.vaultForceIncognito()));
        actionMenu_.subtitle = kiosk ? "YouTube" : (wantIncog ? "Web browser (Incognito)" : "Web browser");
        return;
    }
#ifdef PIXELVR_HAVE_FREETYPE
    browser_.setOpen(false);
#endif
    loadWeb(0, url, kiosk, forceIncognito);
}

void App::loadWeb(int slot, const std::string& url, bool kiosk, bool forceIncognito) {
    if (!multiScreenEnabled()) slot = 0;
    const bool wantIncognito = !kiosk && (forceIncognito || settings_.privateBrowsing() ||
                                          (VaultManager::instance().isUnlocked() && settings_.vaultForceIncognito()));
    if (screens_[slot].web && webHosts_[slot].kiosk() == kiosk && webHosts_[slot].incognito() == wantIncognito) {
        webHosts_[slot].openTab(url);
        selectScreen(slot);
        return;
    }
    if (screens_[slot].open || screens_[slot].opening) clearSlotKeepLayout(slot);  // also a load in flight
    BrowserHost::Options opt;
    opt.kiosk = kiosk;
    opt.incognito = wantIncognito;
    if (!webHosts_[slot].start(url, 1280, 720, opt)) {
        PIXELVR_LOG_WARN("Web screen: browser helper unavailable");
        return;
    }
    ScreenState& sc = screens_[slot];
    sc.open = true;
    sc.web = true;
    sc.path = kWebPath;
    sc.projection = 0;
    sc.stereo = 0;
    sc.muted = false;
    if (slot == 0) {
        renderer_.setProjectionMode(VulkanRenderer::ProjectionMode::Flat);
        renderer_.setStereoMode(VulkanRenderer::StereoMode::Mono);
    } else {
        VulkanRenderer::ScreenPlacement pl = renderer_.screenPlacement(slot);
        if (!pl.active) {
            placeNewScreen(slot);
            pl = renderer_.screenPlacement(slot);
        }
        pl.active = true;
        pl.stereo = VulkanRenderer::StereoMode::Mono;
        renderer_.setScreenPlacement(slot, pl);
    }
    renderer_.setWebSlot(slot, true);
    joinArrangement(slot);
    selectScreen(slot);
    webMouseDown_ = false;
    webHoverValid_ = false;
}

void App::openArrangeMenu() {
    if (screens_[0].open && screens_[0].projection != 0) return;  // immersive plays alone
    int order[VulkanRenderer::kMaxVideoSlots];
    renderer_.screenOrderLeftToRight(order);
    int n = 0;
    for (int slot : order) n += screens_[slot].open ? 1 : 0;
    arrangePresets_.clear();
    if (n >= 2) {
        if (n == 3) arrangePresets_.push_back(ArrangePreset::TwoBelowOneAbove);  // the standard for three
        arrangePresets_.push_back(ArrangePreset::SideBySide);
        arrangePresets_.push_back(ArrangePreset::Stacked);
        arrangePresets_.push_back(ArrangePreset::Cinema);
    }
    // Keep the snap / style choice of the last layout; build around the view.
    arrangeDraft_ = arrangement_;
    arrangeDraft_.yaw = renderer_.screenYaw();
    arrangeDraft_.pitch = renderer_.recenterPitch();
    arrangeDraft_.dist = renderer_.screenDistance();
    arrangeDraft_.scale = 1.0f;
    {
        // All screens of a layout share one curvature: the selected screen's.
        const VulkanRenderer::ScreenPlacement& sel = renderer_.screenPlacement(selectedScreen_);
        arrangeDraft_.curvature = sel.active ? sel.curvature : renderer_.screenCurvature();
    }
    ArrangePreset start = arrangePresets_.empty() ? ArrangePreset::SideBySide : arrangePresets_[0];
    if (arrangementActive_ &&
        std::find(arrangePresets_.begin(), arrangePresets_.end(), arrangement_.preset) != arrangePresets_.end()) {
        start = arrangement_.preset;
    }
    selectArrangePreset(start);
    actionMenu_.visible = true;
    actionMenu_.page = 8;
    actionMenu_.title = "Arrange Screens";
    actionMenu_.subtitle = n >= 2 ? "Pick a layout, use the arrows to swap screens, then Apply"
                                  : "Open a second screen to use a layout";
    actionMenu_.selectedOption = 0;
    refreshArrangeMenu();
}

void App::selectArrangePreset(ArrangePreset preset) {
    arrangeDraft_.preset = preset;
    int order[VulkanRenderer::kMaxVideoSlots];
    renderer_.screenOrderLeftToRight(order);
    std::vector<int> lr;  // open screens, left to right as they sit now
    for (int slot : order) {
        if (screens_[slot].open) lr.push_back(slot);
    }
    std::vector<int> sortedNow = lr;
    std::vector<int> sortedArr = arrangement_.order;
    std::sort(sortedNow.begin(), sortedNow.end());
    std::sort(sortedArr.begin(), sortedArr.end());
    if (arrangementActive_ && arrangement_.preset == preset && sortedNow == sortedArr) {
        arrangeDraft_.order = arrangement_.order;  // start from what is on show
    } else if ((preset == ArrangePreset::Cinema || preset == ArrangePreset::TwoBelowOneAbove) &&
               screens_[selectedScreen_].open) {
        // The selected screen becomes the main / top one.
        arrangeDraft_.order = {selectedScreen_};
        for (int slot : lr) {
            if (slot != selectedScreen_) arrangeDraft_.order.push_back(slot);
        }
    } else {
        arrangeDraft_.order = lr;
    }
}

void App::refreshArrangeMenu() {
    // Options: one per preset, then fixed ids (see the renderer's hub::arrangeButton).
    actionMenu_.options.clear();
    int presetIdx = -1;
    for (std::size_t i = 0; i < arrangePresets_.size(); ++i) {
        switch (arrangePresets_[i]) {
            case ArrangePreset::SideBySide: actionMenu_.options.push_back("Side by side"); break;
            case ArrangePreset::TwoBelowOneAbove: actionMenu_.options.push_back("Two below, one above"); break;
            case ArrangePreset::Stacked: actionMenu_.options.push_back("Stacked"); break;
            case ArrangePreset::Cinema: actionMenu_.options.push_back("Main + sides"); break;
        }
        if (arrangePresets_[i] == arrangeDraft_.preset) presetIdx = static_cast<int>(i);
    }
    actionMenu_.options.push_back(arrangeDraft_.snap ? "Snap edges: On" : "Snap edges: Off");
    actionMenu_.options.push_back(arrangeDraft_.flat ? "Style: Flat wall" : "Style: Curved");
    actionMenu_.options.push_back("Apply");
    actionMenu_.options.push_back("Cancel");

    VulkanRenderer::ScreenPlacement pls[VulkanRenderer::kMaxVideoSlots];
    if (presetIdx >= 0) computeArrangement(arrangeDraft_, pls);
    renderer_.setArrangePreview(pls, renderer_.screenPivot(), arrangeDraft_.yaw, arrangeDraft_.pitch,
                                presetIdx, arrangeDraft_.snap);
}

void App::openActionMenu() {
#ifdef PIXELVR_HAVE_FREETYPE
    const Browser::Entry* fe = browser_.focused();
    actionMenu_.visible = true;
    actionMenu_.page = 0;
    actionMenu_.selectedOption = 0;
    actionMenu_.options.clear();
    actionMenu_.renameBuffer.clear();
    pendingActionPath_ = fe ? fe->path : std::string();  // the row the menu is for (the list may change)
    if (FileOps::instance().relocationPending() && browser_.inVault()) {
        actionMenu_.title = "Vault";
        actionMenu_.subtitle = "The vault is being moved: wait until that is done";
        actionMenu_.options.push_back("Cancel");
        return;
    }

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

    const bool hasFocused = (fe != nullptr && !fe->isBack && fe->path != "smb://__add_share__" &&
                             (!fe->path.empty() || fe->isNetwork));
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
    const bool isSmbEntry = isRemotePath(fe->path);

    // Network entries download from here (this used to be the X button, which
    // is Recenter now).
    if (browser_.onNetwork()) {
        actionMenu_.options.push_back(fe->isDir ? "Download Folder" : "Download");
    }
    if (isSmbRoot && isSmbEntry) {
        actionMenu_.options.push_back("Open Share");
        actionMenu_.options.push_back("Edit Network Share");
        actionMenu_.options.push_back("Delete Network Share");
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
        if (!browser_.onNetwork() && browser_.isFileActionTarget(*fe)) {
            const VaultManager& vm = VaultManager::instance();
            if (vm.isUnlocked() && vm.available() && !browser_.inVault() && !vaultcrypto::hasExtension(fe->path)) {
                actionMenu_.options.push_back("Move to Vault");
            }
            if (fe && !fe->isDir && fe->isVideo && !VaultManager::isPrivatePath(fe->path)) {  // nothing is stored for vault files
                actionMenu_.options.push_back("Change 3D / VR Format");
            }
            actionMenu_.options.push_back("Move");
            actionMenu_.options.push_back("Copy");
            actionMenu_.options.push_back("Rename");
            actionMenu_.options.push_back("Delete");
        }
    }
    actionMenu_.options.push_back("Cancel");
#endif
}

// ---------------------------------------------------------------------------
// Move / Copy: choosing the target folder in the browser, and the footer notices.
// ---------------------------------------------------------------------------
namespace {

bool pathIsUnder(const std::string& p, const std::string& dir) {
    return p == dir || (p.size() > dir.size() && p.compare(0, dir.size(), dir) == 0 && p[dir.size()] == '/');
}

}  // namespace

void App::showNotice(const std::string& text, bool warn, bool priv, double seconds) {
    hubNotice_ = text;
    hubNoticeWarn_ = warn;
    hubNoticePrivate_ = priv;
    hubNoticeUntil_ = std::chrono::steady_clock::now() +
                   std::chrono::milliseconds(static_cast<long long>(seconds * 1000.0));
}

std::vector<std::string> App::sidecarPatterns() const {
    std::vector<std::string> pats = FileOps::defaultSidecarPatterns();
    for (const auto& p : plugins_.plugins()) {
        for (const std::string& s : p.sidecars) {
            if (std::find(pats.begin(), pats.end(), s) == pats.end()) pats.push_back(s);
        }
    }
    return pats;
}

void App::startPick(PickState::Kind kind, const Browser::Entry& e) {
#ifdef PIXELVR_HAVE_FREETYPE
    pick_ = {};
    if (FileOps::instance().relocationPending()) {
        showNotice("The vault is being moved: wait until that is done", true);
        return;
    }
    if (kind == PickState::Kind::Move) {
        // A playing file (or one inside a folder) keeps its reader open: stop it first.
        for (int i = 0; i < VulkanRenderer::kMaxVideoSlots; ++i) {
            if (screens_[i].open && !screens_[i].path.empty() && pathIsUnder(screens_[i].path, e.path)) {
                showNotice("Stop the video first: it is playing", true);
                return;
            }
        }
    }
    pick_.kind = kind;
    pick_.srcPath = e.path;
    pick_.srcIsDir = e.isDir;
    pick_.srcLabel = e.label;
    while (!pick_.srcLabel.empty() && pick_.srcLabel.back() == '/') pick_.srcLabel.pop_back();
    if (!e.isDir) pick_.sidecars = FileOps::sidecarsOf(e.path, sidecarPatterns());
    if (kind == PickState::Kind::Move) {
        for (const std::string& sc : pick_.sidecars) {
            for (int i = 0; i < VulkanRenderer::kMaxVideoSlots; ++i) {
                if (screens_[i].open && screens_[i].path == sc) {
                    pick_ = {};
                    showNotice("Stop the video first: it is playing", true);
                    return;
                }
            }
        }
    }
    hubNotice_.clear();
#else
    (void)kind;
    (void)e;
#endif
}

void App::startDownloadDirPick() {
#ifdef PIXELVR_HAVE_FREETYPE
    pick_ = {};
    pick_.kind = PickState::Kind::DownloadDir;
    pick_.srcLabel = "downloads";
    const std::string start = settings_.downloadDir();
    settings_.close();
    downloadsWindow_ = false;
    std::error_code ec;
    if (!start.empty() && std::filesystem::is_directory(start, ec)) browser_.openPath(start);
    else browser_.openHome();
#endif
}

void App::startVaultMove(bool lockWhenDone) {
#ifdef PIXELVR_HAVE_FREETYPE
    VaultManager& vm = VaultManager::instance();
    if (!vm.isUnlocked()) return;
    if (FileOps::instance().busy()) {
        settings_.showStatus("Wait for the running move / copy to finish first.");
        return;
    }
    const bool toCard = vm.location() == VaultManager::Location::Internal;
    const std::string card = sdCardMount();
    if (toCard && card.empty()) {
        settings_.showStatus("Insert an SD card first.");
        return;
    }
    if (!vm.available()) {
        settings_.showStatus("Insert the SD card with your vault first.");
        return;
    }
    if (vm.migrating()) {
        settings_.setVaultMoveNotice("Wait until the vault files are encrypted, then move the vault.");
        return;
    }
    // Vault films keep their readers open: close them, the files move.
    for (int i = 0; i < VulkanRenderer::kMaxVideoSlots; ++i) {
        if ((screens_[i].open || screens_[i].opening) && VaultManager::isPrivatePath(screens_[i].path)) closeMedia(i);
    }
    if (picking()) cancelPick(false);
    FileOps::Request r;
    r.relocateVault = true;
    r.lockWhenDone = lockWhenDone;
    r.mode = FileOps::Mode::Copy;
    r.sources.push_back(vm.vaultDir());
    r.destDir = toCard ? VaultManager::cardVaultDirFor(card) : vm.internalVaultDir();
    r.toSdCard = toCard;
    if (toCard) {
        std::random_device rd;
        char id[17];
        for (int i = 0; i < 8; ++i) std::snprintf(id + 2 * i, 3, "%02x", static_cast<unsigned>(rd() & 0xFF));
        r.cardId = id;
    }
    r.label = "the vault";
    FileOps::instance().queue(r);
    settings_.setVaultMoveStatus("Moving the vault...");
    xr_.triggerHaptic(0, 0.050f, 0.0f, 0.7f);
    xr_.triggerHaptic(1, 0.050f, 0.0f, 0.7f);
#else
    (void)lockWhenDone;
#endif
}

void App::cancelPick(bool backToSettings) {
    const bool wasDownloadDir = pick_.kind == PickState::Kind::DownloadDir;
    pick_ = {};
    if (wasDownloadDir && backToSettings) {
        settings_.open();
        settings_.setSection(1);
    }
}

std::string App::pickReason() const {
#ifdef PIXELVR_HAVE_FREETYPE
    if (!picking()) return {};
    const std::string loc = browser_.currentLocation();
    if (browser_.isScanning()) return "Opening the folder...";
    if (!browser_.canCreateFolder()) return "Open a folder on this headset";
    if (pick_.kind == PickState::Kind::DownloadDir) {
        if (browser_.inVault() || VaultManager::isPrivatePath(loc)) return "Downloads can't go into the vault";
        if (removableMissing(loc)) return "The SD card was removed";
        if (::access(loc.c_str(), W_OK) != 0) return "This folder is read-only";
        return {};
    }
    return FileOps::checkTarget(pick_.kind == PickState::Kind::Copy ? FileOps::Mode::Copy : FileOps::Mode::Move,
                                pick_.srcPath, pick_.srcIsDir, loc);
#else
    return {};
#endif
}

void App::confirmPick() {
#ifdef PIXELVR_HAVE_FREETYPE
    if (!picking()) return;
    if (pick_.kind != PickState::Kind::DownloadDir) {
        if (FileOps::instance().relocationPending()) {
            showNotice("The vault is being moved: wait until that is done", true);
            return;
        }
        // Something may have started playing it since the pick began.
        if (pick_.kind == PickState::Kind::Move) {
            for (int i = 0; i < VulkanRenderer::kMaxVideoSlots; ++i) {
                if (!screens_[i].open || screens_[i].path.empty()) continue;
                bool hit = pathIsUnder(screens_[i].path, pick_.srcPath);
                for (const std::string& s : pick_.sidecars) hit = hit || screens_[i].path == s;
                if (hit) {
                    showNotice("Stop the video first: it is playing", true);
                    return;
                }
            }
        }
    }
    const std::string why = pickReason();
    if (!why.empty()) {
        showNotice(why, true);
        return;
    }
    const std::string loc = browser_.currentLocation();
    if (pick_.kind == PickState::Kind::DownloadDir) {
        settings_.setCustomDownloadDir(loc);
        browser_.setDownloadDir(settings_.downloadDir());
        showNotice("Downloads go to " + FileOps::placeName(loc) + " now");
        pick_ = {};
        settings_.open();
        settings_.setSection(1);
        xr_.triggerHaptic(1, 0.040f, 0.0f, 0.7f);
        return;
    }
    FileOps::Request r;
    r.mode = pick_.kind == PickState::Kind::Copy ? FileOps::Mode::Copy : FileOps::Mode::Move;
    r.sources.push_back(pick_.srcPath);
    for (const std::string& s : pick_.sidecars) r.sources.push_back(s);
    r.destDir = loc;
    r.label = pick_.srcLabel;
    FileOps::instance().queue(r);
    pick_ = {};
    xr_.triggerHaptic(0, 0.050f, 0.0f, 0.7f);
    xr_.triggerHaptic(1, 0.050f, 0.0f, 0.7f);
#endif
}

void App::drainFileOps() {
#ifdef PIXELVR_HAVE_FREETYPE
    FileOps::Result res;
    bool any = false;
    while (FileOps::instance().takeResult(res)) {
        any = true;
        for (const FileOps::Moved& m : res.moved) {
            // Nothing about a file that went into the vault may stay in the public records.
            thumbnailCache_.forgetUnder(m.from);
            if (res.intoPrivate && !res.relocated) {
                playbackMemory_.forgetUnder(m.from);
                letterboxDetector_.forgetUnder(m.from);
                browser_.dropBookmarksUnder(m.from);
                // The download folder went with it: downloads must not recreate it in plain.
                const std::string custom = settings_.customDownloadDir();
                if (!custom.empty() && pathIsUnder(custom, m.from)) {
                    settings_.setCustomDownloadDir("");
                    browser_.setDownloadDir(settings_.downloadDir());
                }
            } else {
                browser_.retargetBookmarks(m.from, m.to);
            }
        }
        if (!res.moved.empty()) playbackMemory_.save();
        const bool hide = res.privateText && !VaultManager::instance().isUnlocked();
        showNotice(hide ? (res.ok ? "Done" : "Stopped") : res.message, !res.ok && !hide, res.privateText);
        if (res.relocated) {
            renderer_.clearCardThumbnails();
            vaultCardSeen_ = VaultManager::instance().location() == VaultManager::Location::SdCard;
            // The browser may still show the old vault folder.
            for (const FileOps::Moved& m : res.moved) {
                if (browser_.isOpen() && pathIsUnder(browser_.currentLocation(), m.from)) browser_.openVault();
            }
            if (res.lockWhenDone && VaultManager::instance().isUnlocked()) {
                performVaultLock();  // the locked state is the confirmation: no notice names a vault
                hubNotice_.clear();
            } else {
                VaultManager::instance().recordActivity(std::chrono::steady_clock::now());  // not "idle since the start"
                settings_.refresh();
            }
        } else if (!res.ok && res.relocation && VaultManager::instance().isUnlocked()) {
            settings_.setVaultMoveNotice(res.message);
        }
    }
    if (any && browser_.isOpen()) browser_.rescan();  // moved files appear / disappear
#endif
}

void App::updateBrowserFooter(std::chrono::steady_clock::time_point now) {
#ifdef PIXELVR_HAVE_FREETYPE
    VulkanRenderer::BrowserPickBar bar;
    std::string text;
    bool warn = false;
    // Choosing a folder only lives in the open file browser (tabs, a playing video, the menu end it).
    if (picking() && (!browser_.isOpen() || downloadsWindow_ || settings_.isOpen())) cancelPick(false);
    // A fresh notice (e.g. why Move was refused) wins over both passive pick reasons and the progress line.
    if (!hubNotice_.empty()) {
        if (now >= hubNoticeUntil_ || (hubNoticePrivate_ && !VaultManager::instance().isUnlocked())) {
            hubNotice_.clear();
        } else {
            text = hubNotice_;
            warn = hubNoticeWarn_;
        }
    }
    if (picking()) {
        bar.active = true;
        const bool copy = pick_.kind == PickState::Kind::Copy;
        if (pick_.kind == PickState::Kind::DownloadDir) {
            bar.text = "Download folder: open the folder to use";
            bar.confirmLabel = "Use this folder";
        } else {
            const int n = static_cast<int>(pick_.sidecars.size());
            std::string name = pick_.srcLabel;
            if (name.size() > 34) {  // keep the start and the end (the extension), bytes are fine for a cut
                std::size_t head = 20, tail = name.size() - 11;
                while (head > 0 && (static_cast<unsigned char>(name[head]) & 0xC0) == 0x80) --head;
                while (tail < name.size() && (static_cast<unsigned char>(name[tail]) & 0xC0) == 0x80) ++tail;
                name = name.substr(0, head) + "..." + name.substr(tail);
            }
            bar.text = std::string(copy ? "Copying " : "Moving ") + name +
                       (n > 0 ? " (+" + std::to_string(n) + (n == 1 ? " file)" : " files)") : std::string());
            bar.confirmLabel = copy ? "Copy here" : "Move here";
        }
        const std::string why = pickReason();
        bar.canConfirm = why.empty();
        bar.hint = "Trigger: Open folder  |  B: Up  |  Y: Cancel";
        // Don't treat still standing in the starting folder as an error warning in the footer
        // (clicking the disabled "Move here" pill still shows the refusal notice via confirmPick()).
        if (text.empty() && !why.empty() && why != "Opening the folder..." && why != "It is already in this folder") {
            text = why;
            warn = true;
        }
    }
    const FileOps::Status st = FileOps::instance().status();
    const bool lockedNow = !VaultManager::instance().isUnlocked();
    // Speed and time left from the last few seconds (EMA), short so it fits the footer.
    static uint64_t sLastDone = 0;
    static double sSpeed = 0.0;
    static auto sLastAt = now;
    std::string rate;
    int pct = 0;
    if (st.busy) {
        const double dt = std::chrono::duration<double>(now - sLastAt).count();
        if (st.done < sLastDone) sLastDone = 0;  // a new job
        if (dt >= 0.5) {
            const double inst = static_cast<double>(st.done - sLastDone) / dt;
            sSpeed = sSpeed <= 0.0 ? inst : sSpeed * 0.7 + inst * 0.3;
            sLastDone = st.done;
            sLastAt = now;
        }
        pct = st.total > 0 ? std::min(100, static_cast<int>(100.0 * static_cast<double>(st.done) / static_cast<double>(st.total))) : 0;
        static std::string sRate;
        static auto sRateAt = now;
        char b[64];
        if (now - sRateAt < std::chrono::seconds(3) && !sRate.empty()) {
            rate = sRate;  // steady text: it changes every few seconds, not every frame
        } else if (sSpeed > 1e5 && st.total > st.done) {
            sRateAt = now;
            const double left = static_cast<double>(st.total - st.done) / sSpeed;
            if (left >= 3600) std::snprintf(b, sizeof b, "  %.0f MB/s  %dh %02dm left", sSpeed / 1e6, static_cast<int>(left / 3600), static_cast<int>(left / 60) % 60);
            else std::snprintf(b, sizeof b, "  %.0f MB/s  %d min left", sSpeed / 1e6, std::max(1, static_cast<int>(left / 60 + 0.5)));
            rate = sRate = b;
        }
    } else {
        sLastDone = 0;
        sSpeed = 0.0;
        sLastAt = now;
    }
    if (text.empty() && st.busy && !(lockedNow && (st.privateLabel || st.relocating))) {  // a lock says nothing
        text = std::string(st.relocating ? "Vault move" : (st.copy ? "Copying " : "Moving ") + st.label) + ": " +
               std::to_string(pct) + " %" + rate + (st.waiting > 0 ? "  +" + std::to_string(st.waiting) : std::string());
    }
    settings_.pollVaultMeasure();
    settings_.setVaultMoveStatus(st.busy && st.relocating && FileOps::instance().relocationCancelling()
        ? std::string("Stopping the vault move...")
        : st.busy && st.relocating
        ? "Moving the vault: " + std::to_string(pct) + " %" + rate +
              ". Auto-lock is paused during the transfer (you can take the headset off)."
        : std::string());
    renderer_.setBrowserPickBar(bar);
    renderer_.setBrowserNotice(text, warn);
#else
    (void)now;
#endif
}

int App::run() {
    using namespace std::chrono_literals;

    lastFrameHeartbeatMs_.store(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count(),
        std::memory_order_relaxed);
    watchdogRunning_.store(true);
    watchdogThread_ = std::thread(&App::watchdogLoop, this);
    playbackMemory_.load();

    while (!exitRequested_) {
        if (pendingAutoLock_.exchange(false, std::memory_order_acquire)) {
            PIXELVR_LOG_INFO("App: applying pending auto-lock");
            performVaultLock();
        }

        if (!xr_.pollEvents(exitRequested_) || exitRequested_) {
            break;
        }
        // No frames run without a session: plugins still learn it lost focus.
        if (!xr_.sessionRunning()) updatePlugins();

        if (xr_.sessionRunning()) {
            const auto now = std::chrono::steady_clock::now();
            lastFrameHeartbeatMs_.store(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now.time_since_epoch()).count(),
                std::memory_order_relaxed);
            // Compute frame delta-time for continuous seek (F4).
            float dt = 0.0f;
            if (lastFrameTime_.time_since_epoch().count() > 0) {
                dt = std::chrono::duration<float>(now - lastFrameTime_).count();
                dt = std::clamp(dt, 0.0f, 0.1f);  // cap to avoid huge jumps
            }
            lastFrameTime_ = now;

#ifdef PIXELVR_HAVE_MEDIA
            pollMediaOpens(now);
            // Feed every open screen's newest frame into its renderer slot. Held
            // dma-buf frames are released when a decoder is about to seek so it
            // can reclaim its V4L2 capture buffers (otherwise it wedges).
            const auto feedT0 = std::chrono::steady_clock::now();
            for (int slot = 0; slot < VulkanRenderer::kMaxVideoSlots; ++slot) {
                ScreenState& sc = screens_[slot];
                if (!sc.open || sc.web) continue;
                if (sc.engine.needsFrameRelease()) {
                    renderer_.releaseVideoFrame(slot, true);  // seeking: keep the picture up
                    sc.engine.confirmFrameRelease();
                }
                if (sc.engine.drmActive()) {
                    // Zero-copy dma-buf path: import the decoder's frame directly.
                    if (sc.engine.latestDrmFrame(sc.drmFrame)) {
                        renderer_.updateVideoDmabuf(sc.drmFrame, slot);
                    }
                } else if (sc.engine.latestFrame(sc.frame)) {
                    renderer_.updateVideoTexture(sc.frame.width, sc.frame.height,
                                                 sc.frame.y.data(), sc.frame.uv.data(),
                                                 sc.frame.colorspace, sc.frame.fullRange, slot);
                }
            }
            perfFeedMs_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - feedT0).count();
#endif
            const auto frameT0 = std::chrono::steady_clock::now();
            xr_.renderFrame(
                [this](uint32_t viewIndex, uint32_t imageIndex,
                       const XrView& view, const ViewSwapchain& swapchain) {
                    const auto viewT0 = std::chrono::steady_clock::now();
                    renderer_.renderView(viewIndex, imageIndex, view, swapchain);
                    perfViewCpuMs_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - viewT0).count();
                },
                [this](XrContext& xr, const XrView* views, uint32_t viewCount,
                       std::vector<CompositionLayer>& out) {
                    const auto uiT0 = std::chrono::steady_clock::now();
                    renderer_.renderLayersBefore(xr, views, viewCount, out);
                    perfViewCpuMs_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - uiT0).count();
                },
                [this](XrContext& xr, const XrView* views, uint32_t viewCount,
                       std::vector<CompositionLayer>& out) {
                    const auto scT0 = std::chrono::steady_clock::now();
                    renderer_.renderLayersAfter(xr, views, viewCount, out);
                    perfViewCpuMs_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - scT0).count();
                });
            perfFrameMs_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frameT0).count();

            // Apply controller input that renderFrame synced this frame.
            const auto nowT = std::chrono::steady_clock::now();
            XrInputState in = xr_.input();
#ifdef PIXELVR_HAVE_MEDIA
            // Plex / Jellyfin episodes: previous / next arrows on the screen while the laser is on it.
            for (int s = 0; s < VulkanRenderer::kMaxVideoSlots; ++s) {
                const ScreenState& es = screens_[s];
                renderer_.setScreenNav(s, es.open && es.webSuspended && es.plexEpisode && !es.plexAwaitingNext);
            }
            if (in.select) {
                int ns = -1, side = -1;
                if (renderer_.screenNavHit(ns, side) && screens_[ns].webSuspended && screens_[ns].plexEpisode &&
                    !screens_[ns].plexAwaitingNext) {
                    ScreenState& es = screens_[ns];
                    const int64_t ms = static_cast<int64_t>(es.engine.position() * 1000.0);
                    if (side == 0) webHosts_[ns].plexPrev(ms);
                    else webHosts_[ns].plexNext(ms);
                    es.plexAwaitingNext = true;
                    es.plexNavManual = true;
                    es.plexNextSince = nowT;
                    renderer_.setScreenTitle(ns, side == 0 ? "Previous episode..." : "Next episode...", 20.0f);
                    xr_.triggerHaptic(renderer_.pointerHand() == 0 ? 0 : 1, 0.02f, 0.0f, 0.4f);
                    PIXELVR_LOG_INFO("Web play: %s episode asked for on screen %d", side == 0 ? "previous" : "next", ns + 1);
                    in.select = in.togglePlay = false;  // the arrow took the trigger
                }
            }
            if (controlsNoticePending_ && xr_.sessionFocused()) {  // once the user can see it
                controlsNoticePending_ = false;
                renderer_.setDockHint("Button choices moved to SteamOS: SteamVR Settings > Controllers. "
                                      "Your custom buttons were reset.",
                                      20.0f);
            }
#endif
            if (!in.selectHeld) volumeDragging_ = false;  // a volume-bar drag ends with the trigger
            {
                // Laser smoothing (One Euro): steady while the hand is still,
                // quick on fast moves. Everything downstream (hit tests, the
                // beam, grabbing) uses this ray.
                static const OneEuroParams kPos[4] = {{}, {6.0f, 20.0f, 2.5f}, {3.0f, 15.0f, 2.5f}, {2.0f, 10.0f, 2.5f}};
                static const OneEuroParams kRot[4] = {{}, {3.0f, 3.0f, 2.5f}, {1.5f, 2.5f, 2.5f}, {0.8f, 2.0f, 2.5f}};
                const int lvl = std::clamp(settings_.laserSmoothing(), 0, 3);
                for (int h = 0; h < 2; ++h) {
                    XrInputState::ControllerPose& c = in.controllers[h];
                    if (!c.active || lvl == 0) {
                        aimFilter_[h].reset();
                        continue;
                    }
                    aimFilter_[h].filter(c.position, c.orientation, dt, kPos[lvl], kRot[lvl]);
                }
            }

            // Vault unlock combo gate: a full grip press held on either hand (or a
            // combo in progress / just released). Decided before anything reads
            // the buttons, so a combo step (B = Back, ...) can't also act on the
            // keyboard, a grab or a web page. comboEdge itself is never cleared.
            const bool gripFullHeld = in.gripFullPressHeld[0] || in.gripFullPressHeld[1];
            static bool sPrevGripFullHeld = false;
            if (gripFullHeld != sPrevGripFullHeld) {
                if (!gripFullHeld && sPrevGripFullHeld) {
                    lastGripFullRelease_ = nowT;
                }
                sPrevGripFullHeld = gripFullHeld;
                PIXELVR_LOG_INFO("Grip full press state changed: %s (L val: %.2f, R val: %.2f)",
                                 gripFullHeld ? "HELD" : "RELEASED",
                                 in.gripValue[0], in.gripValue[1]);
            }
            const bool gripGracePeriod = (lastGripFullRelease_.time_since_epoch().count() > 0) &&
                                         ((nowT - lastGripFullRelease_) < 300ms);
            const bool comboActive = VaultManager::instance().isComboEntryActive(nowT);
            const bool comboGate = gripFullHeld || comboActive || gripGracePeriod;
            const bool recordingCombo = VaultManager::instance().isRecordingCombo();
            // While a new combo is being recorded only comboEdge (the symbols) and
            // Select (the Cancel row) act: no button may close or leave Settings.
            if (comboGate || recordingCombo) in.clearButtons();

            // In-app keyboard (text entry). It owns all controller input while open:
            // trigger / Play / OK press the key under the laser, stick left/right
            // moves the cursor, Back / Main Menu cancel.
            const bool kbOpen = textEntry_.active;
            if (kbOpen) {
                if (in.back || in.mainMenu) {
                    cancelKeyboard();
                } else {
                    using namespace std::chrono_literals;
                    const int hand = renderer_.pointerHand() == 0 ? 0 : 1;
                    // Thumbstick left/right: step, then accelerate while held.
                    const int stickDir = (in.thumbstickX > 0.45f) ? 1 : (in.thumbstickX < -0.45f ? -1 : 0);
                    if (stickDir == 0) {
                        kbRepeat_.stickDir = 0;
                    } else if (stickDir != kbRepeat_.stickDir) {
                        kbRepeat_.stickDir = stickDir;
                        kbRepeat_.start = nowT;
                        kbRepeat_.next = nowT + 380ms;
                        moveTextCursor(stickDir);
                    } else if (nowT >= kbRepeat_.next) {
                        kbRepeat_.next = nowT + repeatInterval(nowT - kbRepeat_.start);
                        moveTextCursor(stickDir);
                    }

                    const int key = renderer_.keyboardKeyHovered();
                    const bool pressed = in.select || in.togglePlay;
                    if (pressed && textEntry_.password && renderer_.keyboardRevealHovered()) {
                        textEntry_.reveal = !textEntry_.reveal;
                        xr_.triggerHaptic(hand, 0.015f, 0.0f, 0.45f);
                    } else if (pressed && key >= 0) {
                        xr_.triggerHaptic(hand, 0.015f, 0.0f, 0.45f);
                        pressKeyboardKey(key);
                        // Cursor arrows and Delete repeat while the trigger is held.
                        const int code = VulkanRenderer::keyboardKeyCode(key);
                        if (textEntry_.active && (code == 1 || code == 5 || code == 6)) {
                            kbRepeat_.key = key;
                            kbRepeat_.start = nowT;
                            kbRepeat_.next = nowT + 380ms;
                        }
                    } else if (kbRepeat_.key >= 0) {
                        if (!in.selectHeld || key != kbRepeat_.key) {
                            kbRepeat_.key = -1;  // released or pointer moved off the key
                        } else if (nowT >= kbRepeat_.next) {
                            kbRepeat_.next = nowT + repeatInterval(nowT - kbRepeat_.start);
                            pressKeyboardKey(kbRepeat_.key);
                            xr_.triggerHaptic(hand, 0.006f, 0.0f, 0.25f);
                        }
                    }

                    // Click into the text field to place the cursor there; keep
                    // the trigger held and sweep to drag it.
                    if (pressed && renderer_.keyboardFieldHovered()) {
                        const int pos = renderer_.keyboardCursorAtPointer();
                        if (pos >= 0) {
                            webCaretTo(static_cast<std::size_t>(pos));
                            textEntry_.cursor = static_cast<std::size_t>(pos);
                            kbFieldDrag_ = true;
                            xr_.triggerHaptic(hand, 0.010f, 0.0f, 0.35f);
                        }
                    } else if (kbFieldDrag_) {
                        if (!in.selectHeld) {
                            kbFieldDrag_ = false;
                        } else if (renderer_.keyboardFieldHovered()) {
                            const int pos = renderer_.keyboardCursorAtPointer();
                            if (pos >= 0 && static_cast<std::size_t>(pos) != textEntry_.cursor) {
                                webCaretTo(static_cast<std::size_t>(pos));
                                textEntry_.cursor = static_cast<std::size_t>(pos);
                                xr_.triggerHaptic(hand, 0.004f, 0.0f, 0.2f);
                            }
                        }
                    }
                }
                in.thumbstickX = 0.0f;
                in.thumbstickY = 0.0f;
                in.select = false;
                in.selectHeld = false;
                in.clearButtons();  // every action is the keyboard's while it is open
                if (smbModal_.active && smbModal_.editingField != SmbModalState::Field::None) {
                    updateSmbModalMenu();
                }
            }
            renderer_.setKeyboardModal(textEntry_.active, textEntry_.prompt, textEntry_.text,
                                       textEntry_.password, nullptr,
                                       static_cast<int>(textEntry_.cursor));
            renderer_.setKeyboardModes(textEntry_.shift, textEntry_.symbols);
            renderer_.setKeyboardReveal(textEntry_.password, textEntry_.password && textEntry_.reveal);

            // Multi-screen grab: while the trigger is held on an unlocked screen it
            // follows the laser (grab point stays under it) and turns to face the
            // viewer. Stick up/down = distance, left/right = size; release drops it.
            if (movingScreen_ >= 0) {
                VulkanRenderer::ScreenPlacement pl = renderer_.screenPlacement(movingScreen_);
                if (!pl.active || !screens_[movingScreen_].open) {
                    movingScreen_ = -1;
                } else {
                    const int hand = (renderer_.laserOn(0) && !renderer_.laserOn(1)) ? 0 : 1;
                    const auto& ctl = in.controllers[hand];
                    if (ctl.active) {
                        const bool group = groupedScreen(movingScreen_);
                        if (std::fabs(in.thumbstickY) > 0.2f) {
                            movingDistance_ = std::clamp(movingDistance_ + in.thumbstickY * dt * 1.5f, 1.0f, 8.0f);
                        }
                        if (std::fabs(in.thumbstickX) > 0.2f) {
                            const float k = 1.0f + in.thumbstickX * dt * 0.8f;
                            if (group) arrangement_.scale = std::clamp(arrangement_.scale * k, 0.4f, 2.2f);
                            else pl.scale = std::clamp(pl.scale * k, 0.35f, 2.5f);
                        }
                        const float fwdLocal[3] = {0.0f, 0.0f, -1.0f};
                        float fwd[3];
                        quatRotate(ctl.orientation, fwdLocal, fwd);
                        for (int k = 0; k < 3; ++k) {
                            pl.center[k] = ctl.position[k] + fwd[k] * movingDistance_ + grabOffset_[k];
                        }
                        if (group) {
                            // Snapped screens move as one wall, edges kept together.
                            moveArrangementTo(movingScreen_, pl.center);
                            pl = renderer_.screenPlacement(movingScreen_);
                        }
                        const float d[3] = {pl.center[0] - in.headPos[0], pl.center[1] - in.headPos[1],
                                            pl.center[2] - in.headPos[2]};
                        const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                        if (len > 1e-3f && !group) {
                            pl.yaw = std::atan2(-d[0], -d[2]);
                            pl.pitch = std::asin(std::clamp(d[1] / len, -1.0f, 1.0f));
                            pl.roll = 0.0f;
                        }
                        if (!group) renderer_.setScreenPlacement(movingScreen_, pl);
                    }
                    // Releasing the trigger drops the screen (Back / Main Menu too).
                    if (!in.selectHeld || in.back || in.mainMenu) {
                        // Moving one screen of an unsnapped layout ends that layout
                        // (new screens then get the default placement).
                        if (!groupedScreen(movingScreen_) && inArrangement(movingScreen_)) {
                            const float mx = pl.center[0] - grabStart_[0];
                            const float my = pl.center[1] - grabStart_[1];
                            const float mz = pl.center[2] - grabStart_[2];
                            if (mx * mx + my * my + mz * mz > 0.02f * 0.02f ||
                                std::fabs(pl.scale - grabStartScale_) > 0.01f) {
                                arrangementActive_ = false;
                                arrangementBroken_ = true;
                            }
                        }
                        movingScreen_ = -1;
                        xr_.triggerHaptic(hand, 0.030f, 0.0f, 0.5f);
                    }
                }
                in.select = false;
                in.selectHeld = false;
                in.clearButtons();  // the grab owns the input
                in.thumbstickX = 0.0f;
                in.thumbstickY = 0.0f;
                lastControllerActivity_ = nowT;
            }

            // Web browser screen: page frames in, laser as the mouse while the
            // screens are locked (unlocked, trigger grabs it like any screen).
            // Trigger = click (a press only becomes a drag after moving past a
            // small slop, so laser jitter doesn't select text), thumbstick =
            // scroll, B = back. Close it with the dock's X like any screen.
            BrowserHost::tickIdle();
            // Chromium install: progress in its dialog; when done, open the
            // browser screen that asked for it.
            {
                const ChromiumInstaller::State before = chromiumInstaller_.state();
                chromiumInstaller_.poll();
                const ChromiumInstaller::State now = chromiumInstaller_.state();
                const bool dialogUp = actionMenu_.visible && actionMenu_.page == 9;
                if (now == ChromiumInstaller::State::Done) {
                    chromiumInstaller_.reset();
                    if (dialogUp) actionMenu_.visible = false;
#ifdef PIXELVR_HAVE_FREETYPE
                    browser_.setOpen(false);
#endif
                    if (installWebSlot_ >= 0 && !screens_[installWebSlot_].open) {
                        loadWeb(installWebSlot_, installWebUrl_, installWebKiosk_);
                        installWebSlot_ = -1;
                    } else if (installWebSlot_ >= 0) {
                        // Something plays there now (the dialog was hidden):
                        // don't replace it, ask.
                        installWebSlot_ = -1;
                        if (multiScreenEnabled()) {
                            openWebScreen(installWebUrl_, installWebKiosk_);  // which screen?
                        } else {
                            actionMenu_.visible = true;
                            actionMenu_.page = 9;
                            actionMenu_.title = "Chromium is installed";
                            actionMenu_.subtitle = installWebKiosk_ ? "Open YouTube now? It replaces what is playing."
                                                                    : "Open the browser now? It replaces what is playing.";
                            actionMenu_.options = {installWebKiosk_ ? "Open YouTube" : "Open browser", "Not now"};
                            actionMenu_.selectedOption = 0;
                        }
                    }
                } else if (now == ChromiumInstaller::State::Failed && before == ChromiumInstaller::State::Running) {
                    showChromiumDialog();  // show why, offer to try again
                } else if (now == ChromiumInstaller::State::Running && dialogUp) {
                    showChromiumDialog();  // progress
                }
            }
            for (int slot = 0; slot < VulkanRenderer::kMaxVideoSlots; ++slot) {
                if (!screens_[slot].web && !screens_[slot].webSuspended) continue;
                BrowserHost& host = webHosts_[slot];
                host.setPlexNative(true);  // Plex videos always play in our player
                host.poll();
                // The page behind a Plex video went away: keep the video playing;
                // returning to the dead page then closes the screen below.
                if (!host.running() && screens_[slot].webSuspended) continue;
                if (!host.running()) {  // helper or Chromium died
                    const std::string why = host.lastError();
                    const bool missing = why.rfind("Chromium is not installed", 0) == 0;
                    if (missing) {  // remember what to open once it is installed
                        installWebSlot_ = slot;
                        installWebUrl_ = host.startUrl();
                        installWebKiosk_ = host.kiosk();
                    }
                    closeMedia(slot);
#ifdef PIXELVR_HAVE_FREETYPE
                    if (missing) {
                        showChromiumDialog();  // offer to install it right here
                    } else if (!why.empty()) {
                        browser_.setStatus("Browser: " + why);
                        browser_.setOpen(true);
                    }
#endif
                    continue;
                }
                if (const uint8_t* rgba = host.takeFrame()) {
                    const float before = renderer_.webScreenHeightFactor(slot);
                    renderer_.updateWebFrame(slot, rgba, host.frameWidth(), host.frameHeight());
                    // The page's height (toolbar shown / hidden, first frame) is
                    // only known now: re-lay the arrangement around it.
                    if (arrangementActive_ && std::fabs(renderer_.webScreenHeightFactor(slot) - before) > 1e-3f &&
                        std::find(arrangement_.order.begin(), arrangement_.order.end(), slot) !=
                            arrangement_.order.end()) {
                        applyArrangement(arrangement_);
                    }
                }
                BrowserHost::PlexPlay plexPlay;
                while (host.takePlexPlay(plexPlay)) {
                    // (A screen that finished an episode takes the next one's hand-off too.)
                    if (!screens_[slot].web && !screens_[slot].plexAwaitingNext) continue;
                    // Plex Web restores the play queue another window is already playing natively.
                    const std::string item = plexPlay.url.substr(0, plexPlay.url.find('?'));
                    bool playing = false;
                    for (int o = 0; o < VulkanRenderer::kMaxVideoSlots; ++o) {
                        const std::string& op = screens_[o].path;
                        playing = playing || (o != slot && screens_[o].webSuspended && op.substr(0, op.find('?')) == item);
                    }
                    if (playing) host.plexRelease();
                    else beginWebPlay(slot, plexPlay);
                }
                // Hand-offs waiting for the server (a sleeping disk can take 15 s+): start them
                // once the first bytes arrived; the frame loop never waits on the network.
                for (size_t p = 0; p < pendingWebPlays_.size();) {
                    PendingWebPlay& pw = pendingWebPlays_[p];
                    if (pw.slot != slot) {
                        ++p;
                        continue;
                    }
                    const int st = pw.state->load();
                    const bool timedOut = std::chrono::steady_clock::now() - pw.since > std::chrono::seconds(90);
                    if (!screens_[slot].web && !screens_[slot].webSuspended) {
                        pendingWebPlays_.erase(pendingWebPlays_.begin() + static_cast<long>(p));  // page closed meanwhile
                    } else if (!screens_[slot].web && !screens_[slot].plexAwaitingNext) {
                        pendingWebPlays_.erase(pendingWebPlays_.begin() + static_cast<long>(p));  // returned to the page meanwhile
                    } else if (st != 0 || timedOut) {
                        const BrowserHost::PlexPlay play = pw.play;
                        pendingWebPlays_.erase(pendingWebPlays_.begin() + static_cast<long>(p));
                        if (st == 1) {
                            startPlexPlay(slot, play);
                        } else {
                            PIXELVR_LOG_WARN("Web play: the server did not send \"%s\"", play.title.c_str());
                            renderer_.setScreenTitle(slot, "The server did not start the video. Try again.", 6.0f);
                            host.plexRelease();
                        }
                    } else {
                        ++p;
                    }
                }
                BrowserHost::SaveFile saveFile;
                while (host.takeSaveFile(saveFile)) {
                    browser_.setDownloadDir(settings_.downloadDir());
                    browser_.startDownload(saveFile.url, saveFile.name, false);
                }
                // Text fields: a click into one opens the keyboard, which then
                // types straight into the page; it closes when the field does.
                BrowserHost::FocusEvent fe;
                while (host.takeFocusEvent(fe)) {
                    if (fe.focused && kbWebSlot_ == slot) {
                        textEntry_.text = fe.value;
                        textEntry_.cursor = utf16ToByteOffset(fe.value, fe.cursor);
                        if (textEntry_.password != fe.password) textEntry_.reveal = false;
                        textEntry_.password = fe.password;
                    } else if (fe.focused && fe.byClick && !textEntry_.active && slot == webMouseSlot_) {
                        openWebKeyboard(slot, &fe, in);
                    } else if (!fe.focused && kbWebSlot_ == slot) {
                        cancelKeyboard();
                    }
                }
                int fieldCursor = 0;
                std::string fieldValue;
                if (host.takeFieldValue(fieldCursor, fieldValue) && kbWebSlot_ == slot) {
                    textEntry_.text = fieldValue;
                    textEntry_.cursor = utf16ToByteOffset(fieldValue, fieldCursor);
                }
            }
            bool hubShown = settings_.isOpen() || downloadsWindow_;
#ifdef PIXELVR_HAVE_FREETYPE
            hubShown = hubShown || browser_.isOpen();
#endif
            // In menus the D-pad steps like the stick: a full stick push while
            // held, so navStep / navStepX give it the same first step and repeat.
            if ((hubShown || actionMenu_.visible || !pendingDl_.empty()) && !textEntry_.active &&
                movingScreen_ < 0) {
                const float dy = in.dpadUpHeld ? 1.0f : (in.dpadDownHeld ? -1.0f : 0.0f);
                const float dx = in.dpadRightHeld ? 1.0f : (in.dpadLeftHeld ? -1.0f : 0.0f);
                if (dy != 0.0f && std::fabs(in.thumbstickY) < 0.5f) in.thumbstickY = dy;
                if (dx != 0.0f && std::fabs(in.thumbstickX) < 0.5f) in.thumbstickX = dx;
                dpadRepeatDir_ = 0;  // the playback repeat starts afresh
            }
            bool laserOnWeb = false;  // the D-pad leaves web pages alone (for now)
            (void)laserOnWeb;
            if (!hubShown && !actionMenu_.visible && !textEntry_.active && !screensUnlocked_ &&
                movingScreen_ < 0) {
                constexpr float kClickSlop = 16.0f;  // page pixels
                int ws = -1;
                float u = 0.0f, v = 0.0f;
                const bool onWeb = renderer_.webPointer(ws, u, v) && screens_[ws].web;
                laserOnWeb = onWeb;
                const float rawStickY = in.thumbstickY;  // before the page takes it (latch below)
                // A press stays with the screen it started on.
                if (onWeb && !webMouseDown_ && ws != webMouseSlot_) {
                    webMouseSlot_ = ws;
                    webHoverValid_ = false;
                }
                BrowserHost* host = (webMouseSlot_ >= 0 && screens_[webMouseSlot_].web) ? &webHosts_[webMouseSlot_]
                                                                                       : nullptr;
                if (host && onWeb && ws == webMouseSlot_) {
                    const float rx = u * static_cast<float>(host->frameWidth());
                    const float ry = v * static_cast<float>(host->frameHeight());
                    if (!webHoverValid_) {
                        webMouseX_ = rx;
                        webMouseY_ = ry;
                        webHoverValid_ = true;
                    } else {  // light smoothing against hand jitter
                        webMouseX_ += (rx - webMouseX_) * 0.5f;
                        webMouseY_ += (ry - webMouseY_) * 0.5f;
                    }
                    if (in.select) {
                        if (webMouseSlot_ != selectedScreen_) selectScreen(webMouseSlot_);
                        webDownX_ = webMouseX_;
                        webDownY_ = webMouseY_;
                        webDragging_ = false;
                        host->mouseMove(webDownX_, webDownY_);
                        host->mouseDown(webDownX_, webDownY_);
                        webMouseDown_ = true;
                        in.select = false;
                        in.togglePlay = false;
                    } else if (webMouseDown_) {
                        const float dx = webMouseX_ - webDownX_;
                        const float dy = webMouseY_ - webDownY_;
                        if (!webDragging_ && dx * dx + dy * dy > kClickSlop * kClickSlop) webDragging_ = true;
                        if (webDragging_) host->mouseMove(webMouseX_, webMouseY_);
                    } else {
                        host->mouseMove(webMouseX_, webMouseY_);
                    }
                    if (std::fabs(in.thumbstickY) > 0.2f) {
                        webWheelAccum_ += in.thumbstickY * dt * 1800.0f;  // wheel units (120 per notch)
                        const int steps = static_cast<int>(webWheelAccum_);
                        if (steps != 0) {
                            host->wheel(webMouseX_, webMouseY_, steps);
                            webWheelAccum_ -= static_cast<float>(steps);
                        }
                    }
                    // On a page the stick never moves the screen, also below the
                    // scroll threshold (the screen push starts at 0.15).
                    in.thumbstickY = 0.0f;
                    if (in.back) {  // browser Back, and nothing else
                        host->back();
                        in.back = false;
                    }
                } else if (!onWeb) {
                    webHoverValid_ = false;
                }
                // The stick stays with the page while it is held after scrolling
                // there, and briefly after the laser leaves: a frame where the hit
                // test slips off the screen (Plex's menus reach its edge) must not
                // nudge the screen.
                const bool stickHeld = std::fabs(rawStickY) > 0.15f;
                if (onWeb) {
                    webStickUntil_ = nowT + std::chrono::milliseconds(400);
                    if (stickHeld) webStickLatched_ = true;
                }
                if (!stickHeld) webStickLatched_ = false;
                if (!onWeb && (webStickLatched_ || nowT < webStickUntil_)) in.thumbstickY = 0.0f;
                if (host && webMouseDown_ && !in.selectHeld) {
                    // A press that never left the slop is a clean click where it started.
                    if (webDragging_) host->mouseUp(webMouseX_, webMouseY_);
                    else host->mouseUp(webDownX_, webDownY_);
                    webMouseDown_ = false;
                    webDragging_ = false;
                }
            }

#ifdef PIXELVR_HAVE_MEDIA
            // Music view on a screen: buttons and queue rows; the stick scrolls
            // the queue while pointing at it.
            if (!hubShown && !actionMenu_.visible && !textEntry_.active && !screensUnlocked_ && movingScreen_ < 0) {
                int ms = -1;
                const int mh = renderer_.musicHover(ms);
                if (ms >= 0 && screens_[ms].open && screens_[ms].engine.audioOnly()) {
                    ScreenState& sc = screens_[ms];
                    using VR = VulkanRenderer;
                    if (in.select && mh != VR::kMusicNone && mh != VR::kMusicList) {
                        if (ms != selectedScreen_) selectScreen(ms);
                        if (mh == VR::kMusicShuffle) {
                            sc.queue.setShuffle(!sc.queue.shuffle());
                        } else if (mh == VR::kMusicRepeat) {
                            sc.queue.cycleRepeat();
                        } else if (mh == VR::kMusicPlay) {
                            sc.engine.togglePause();
                        } else if (mh == VR::kMusicPrev) {
                            musicPrevious(ms);
                        } else if (mh == VR::kMusicNext) {
                            musicNext(ms);
                        } else if (mh >= 0) {
                            playQueueTrack(ms, mh);  // a queue row
                        }
                        xr_.triggerHaptic(renderer_.pointerHand() == 0 ? 0 : 1, 0.015f, 0.0f, 0.4f);
                        in.select = false;
                        in.togglePlay = false;
                    }
                    if ((mh == VR::kMusicList || (mh >= 0 && mh < VR::kMusicShuffle)) && std::fabs(in.thumbstickY) > 0.3f) {
                        musicScrollAccum_ += in.thumbstickY * dt * 9.0f;  // rows per second at full tilt
                        const int steps = static_cast<int>(musicScrollAccum_);
                        if (steps != 0) {
                            sc.musicFirst -= steps;  // stick up = toward the start
                            musicScrollAccum_ -= static_cast<float>(steps);
                        }
                        in.thumbstickY = 0.0f;
                    }
                }
            }
#endif

            // Custom combo recording listener (active while recording new 6-button combo in Vault & Privacy)
            // The four face actions record the symbols (comboEdge is never cleared;
            // every other button was cleared at the combo gate above).
            if (recordingCombo) {
                if (in.comboEdge >= 0) {
                    const bool finished = VaultManager::instance().feedRecordButton(in.comboEdge);
                    if (finished) {
                        xr_.triggerHaptic(0, 0.050f, 0.0f, 0.8f);
                        xr_.triggerHaptic(1, 0.050f, 0.0f, 0.8f);
                    } else {
                        xr_.triggerHaptic(0, 0.020f, 0.0f, 0.4f);
                        xr_.triggerHaptic(1, 0.020f, 0.0f, 0.4f);
                    }
                    settings_.refresh();
                }
            }

            // Suppress laser pointer hovering, clicking, and dim the laser beam during combo entry
            renderer_.setPointerSuppressed(comboGate);

            // Whirligig-style Quick Path controller button shortcut
            const bool quickPathPressed = !comboGate && !recordingCombo && !kbOpen && in.bookmarks;
            if (quickPathPressed) {
                browser_.openBookmarks();
                downloadsWindow_ = false;
                settings_.close();
                browser_.setOpen(true);
                xr_.triggerHaptic(1, 0.040f, 0.0f, 0.6f);
            }

            if (comboGate && !recordingCombo) {
                const int h = in.gripFullPressHeld[1] ? 1 : 0;
                if (const int b = in.comboEdge; b >= 0) {
                    PIXELVR_LOG_INFO("Combo input button press: %s (hand %s)",
                                     (b == XrInputState::ComboPlay) ? "A" :
                                     (b == XrInputState::ComboBack) ? "B" :
                                     (b == XrInputState::ComboRecenter) ? "X" : "Y",
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
                }
                // Suppress normal button, trigger, and thumbstick functions while combo is active
                // (the buttons were already cleared above).
                in.select = false;
                in.selectHeld = false;
                in.thumbstickX = 0.0f;
                in.thumbstickY = 0.0f;
                in.thumbstickLX = 0.0f;
                in.thumbstickLY = 0.0f;
                in.thumbstickRX = 0.0f;
                in.thumbstickRY = 0.0f;
            }

            // "Options" opens / closes File Actions for the focused entry (it does
            // nothing in Settings).
            const bool actionBtnPressed = !settings_.isOpen() && !comboGate && !recordingCombo && in.options;

            // Grip full-press click: activate laser pointer on this hand.
            // Exactly one controller has an active laser pointer at any time.
            // The press itself opens the combo gate (gripFullPressDown implies
            // gripFullPressHeld), so only an entry already in progress blocks it.
            if (!recordingCombo && !comboActive) {
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
            renderer_.setAmbilightReach(settings_.glowReach());
            renderer_.setAmbilightAllScreens(settings_.ambilightAllScreens());
            renderer_.setRoomColor(settings_.roomColor());
            {
                std::vector<VulkanRenderer::RailShortcut> rail;
                for (const SettingsMenu::RailShortcut& sc : settings_.railShortcuts()) {
                    rail.push_back({sc.name, sc.kiosk});
                }
                renderer_.setRailShortcuts(rail);
            }
            renderer_.setUiFollowMode(settings_.uiFollowMode());
            renderer_.setPanelCurve(settings_.panelCurve());
            renderer_.setUiOpacity(settings_.uiOpacity());
            renderer_.setPanelStyle(settings_.panelStyle());

            // Multi-screen bookkeeping: turning the setting off returns to the
            // classic single main screen; audio follows the chosen mode.
            if (!multiScreenEnabled()) {
                for (int slot = 1; slot < VulkanRenderer::kMaxVideoSlots; ++slot) {
                    if (screens_[slot].open || screens_[slot].opening) closeMedia(slot);
                }
                if (selectedScreen_ != 0) selectScreen(0);
                if (renderer_.screenPlacement(0).active) {
                    VulkanRenderer::ScreenPlacement pl0 = renderer_.screenPlacement(0);
                    pl0.active = false;
                    renderer_.setScreenPlacement(0, pl0);
                }
                movingScreen_ = -1;
                arrangementActive_ = false;
                arrangementBroken_ = false;
                arrangement_.order.clear();
            }
#ifdef PIXELVR_HAVE_MEDIA
            for (int slot = 0; slot < VulkanRenderer::kMaxVideoSlots; ++slot) {
                ScreenState& sc = screens_[slot];
                if (!sc.open) continue;
                // Each screen plays its own audio; mute / volume per screen (the
                // audio-follow button just sets the mutes on selection).
                const bool mute = sc.muted;
                if (sc.web) {
                    webHosts_[slot].setMuted(mute);
                    webHosts_[slot].setVolume(sc.volume);
                    continue;
                }
                if (sc.webSuspended && updatePlexPlayback(slot)) continue;
                sc.engine.setMuted(mute);
                sc.engine.setVolume(sc.volume);
                // Music: the next track when one ends.
                if (sc.engine.audioOnly() && sc.engine.eof() && !sc.queue.empty()) {
                    const int next = sc.queue.next(true);
                    if (next >= 0) {
                        playQueueTrack(slot, next);
                    } else if (!sc.engine.paused()) {
                        sc.engine.setPaused(true);  // end of the queue: stop on the last track
                    }
                }
            }
            for (int slot = 0; slot < VulkanRenderer::kMaxVideoSlots; ++slot) {
                updateMusicView(slot);
                ScreenState& sc = screens_[slot];
                if (sc.letterboxPending && sc.open) {
                    float lb = 0.0f;
                    if (letterboxDetector_.result(sc.path, lb)) {
                        sc.letterboxPending = false;
                        sc.letterbox = lb;
                        applyLetterbox(slot);
                    }
                }
                ThumbnailCache::MediaMeta tm;
                if (sc.formatPending && sc.open && thumbnailCache_.getMeta(sc.path, tm)) {
                    // The picture check finished after the file opened.
                    sc.formatPending = false;
                    if (tm.stereoHint > 0 && sc.stereo == 0 && sc.projection == 0 && !sc.formatByHand) {
                        PIXELVR_LOG_INFO("Format from the picture: stereo %d for %s", tm.stereoHint, sc.path.c_str());
                        sc.stereo = tm.stereoHint;
                        const auto st = static_cast<VulkanRenderer::StereoMode>(tm.stereoHint);
                        if (slot == 0) {
                            renderer_.setStereoMode(st);  // the main screen uses the global mode
                        } else {
                            VulkanRenderer::ScreenPlacement pl = renderer_.screenPlacement(slot);
                            pl.stereo = st;
                            renderer_.setScreenPlacement(slot, pl);
                        }
                        if (slot == selectedScreen_) settings_.setStereoMode(sc.stereo);
                        if (arrangementActive_ && inArrangement(slot)) applyArrangement(arrangement_);
                    }
                }
            }
            renderer_.setVolumeDisplay(cur().volume, cur().muted);
            renderer_.setAudioFollow(settings_.audioFollow());
#endif
            renderer_.setMultiScreen(multiScreenEnabled() && openScreenCount() > 0, selectedScreen_);
            if (!multiScreenEnabled() || openScreenCount() == 0) screensUnlocked_ = false;
            renderer_.setScreenControls(multiScreenEnabled() && (cur().open || cur().opening), selectedScreen_ + 1,
                                        cur().muted, screensUnlocked_, settings_.screenNumbers());
            renderer_.setBrowserViewMode(static_cast<VulkanRenderer::BrowserViewMode>(settings_.browserViewMode()));
            renderer_.setBrowserQuickPath(browser_.isCurrentBookmarked());
            renderer_.setBrowserCanCreateFolder(browser_.canCreateFolder());
#ifdef PIXELVR_HAVE_FREETYPE
            drainFileOps();
            updateBrowserFooter(nowT);
            {
                // Vault on the SD card: pulled while unlocked -> lock the whole vault (user decision);
                // put back while its placeholder is shown -> show the vault.
                VaultManager& vm = VaultManager::instance();
                static auto sNextCardCheck = nowT;
                if (vm.isUnlocked() && vm.location() == VaultManager::Location::SdCard && nowT >= sNextCardCheck) {
                    sNextCardCheck = nowT + std::chrono::milliseconds(700);
                    const bool here = vm.available();
                    if (here) {
                        vaultCardSeen_ = true;
                        if (browser_.currentLocation() == Browser::kVaultMissing) {
                            const bool wasOpen = browser_.isOpen();
                            browser_.openVault();
                            if (!wasOpen) browser_.setOpen(false);
                        }
                    } else if (vaultCardSeen_) {
                        performVaultLock();
                        showNotice("SD card removed", true);
                    }
                }
            }
#endif
            {
                // Vault page: keep the encryption progress current.
                static int sMigShown = -2;
                const VaultManager& vm = VaultManager::instance();
                const int mig = vm.migrating() ? vm.migrationDone() : -1;
                if (mig != sMigShown) {
                    sMigShown = mig;
                    if (settings_.section() == 4) settings_.refresh();
                }
            }
            {
                static std::string sCrumbLoc;
                if (browser_.currentLocation() != sCrumbLoc) {  // path levels only change with the folder
                    sCrumbLoc = browser_.currentLocation();
                    std::vector<std::string> labels;
                    for (const Browser::Crumb& c : browser_.crumbs()) labels.push_back(c.label);
                    renderer_.setBrowserCrumbs(std::move(labels));
                }
            }
#ifdef PIXELVR_HAVE_FREETYPE
            {
                static const char* kSortLabels[3] = {"Name", "Newest", "Largest"};
                browser_.setSortMode(settings_.browserSort());  // no-op when unchanged
                renderer_.setBrowserSortLabel(kSortLabels[settings_.browserSort() % 3]);
            }
#endif
            if (!didInitialRecenter_ && in.headValid) {
                renderer_.setScreenDistance(settings_.screenDistance());
                renderer_.recenterScreen(in.headPos, in.headForward);  // initial
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
            // While watching media with the HUD hidden (either dismissed or auto-faded),
            // stick actions (scrub, +-30s step, screen distance, 180/360 zoom/tilt/pan),
            // Recenter (X), +-30s skip, and Bumper hold (pan/tilt shift) execute silently
            // without waking the HUD or clearing hudDismissed_.
            const bool watching = !hubShown && !actionMenu_.visible && pendingDl_.empty() && !textEntry_.active;
            const bool hudCurrentlyShown = !hudDismissed_ && renderer_.controlBarVisible();
            const bool rawStickX = std::fabs(in.thumbstickX) > 0.25f || std::fabs(in.thumbstickLX) > 0.25f ||
                                   std::fabs(in.thumbstickRX) > 0.25f;
            const bool rawStickY = std::fabs(in.thumbstickY) > 0.25f || std::fabs(in.thumbstickLY) > 0.25f ||
                                   std::fabs(in.thumbstickRY) > 0.25f;
            const bool rawStickMoved = rawStickX || rawStickY;
            // Settings > Controls & Input (off = any stick use brings the controls back, as always):
            // left / right is scrub and skip; up / down is distance and zoom; with a bumper held in
            // 360/180 both axes pan and tilt the view, which goes with zoom.
            // In 360/180 the view gestures (zoom, bumper + stick pan / tilt) never brought the
            // controls back and still don't; there one push counts by its main direction, as the
            // action code takes it.
            const bool sphereNow = settings_.projectionMode() != 0;
            const bool panShift = sphereNow && (in.previousHeld || in.nextHeld || panShiftLatched_);
            const bool quietX = panShift ? true : settings_.silentScrub();
            const bool quietY = sphereNow ? true : settings_.silentZoom();
            bool wakeX = rawStickX, wakeY = rawStickY;
            if (sphereNow) {
                const float ax = std::max({std::fabs(in.thumbstickX), std::fabs(in.thumbstickLX), std::fabs(in.thumbstickRX)});
                const float ay = std::max({std::fabs(in.thumbstickY), std::fabs(in.thumbstickLY), std::fabs(in.thumbstickRY)});
                wakeX = rawStickX && ax >= ay;
                wakeY = rawStickY && ay > ax;
            }
            const bool stickMoved = !watching || hudCurrentlyShown
                                        ? rawStickMoved
                                        : ((wakeX && !quietX) || (wakeY && !quietY));
            // Only the digital full grip press or an action wakes the HUD (a soft
            // squeeze from a resting hand doesn't). Show / Hide UI toggles it
            // explicitly below; the combo gate and the keyboard consumed theirs.
            const bool toggleUiPressed = in.toggleUi;
            in.toggleUi = false;
            // Back with the controls hidden does nothing at all (it must not
            // bring them back only for the next Back to hide them again).
            const bool backPressed = in.back;
            if (hudDismissed_) in.back = false;
            const bool recenterPressed = in.recenter;
            const bool skipBackPressed = in.skipBack;
            const bool skipFwdPressed = in.skipForward;
            const bool prevPressed = in.previous;
            const bool nextPressed = in.next;
            bool musicActive = false;
#ifdef PIXELVR_HAVE_MEDIA
            musicActive = cur().open && !cur().web && cur().engine.audioOnly();
#endif
            const bool quietRecenter = watching && settings_.silentRecenter();
            const bool quietSkip = watching && settings_.silentScrub();
            const bool quietBumper = watching && !musicActive && sphereNow;  // 360/180: the pan / tilt shift
            const bool silentPlaybackAction = (quietRecenter && recenterPressed) ||
                                              (quietSkip && (skipBackPressed || skipFwdPressed)) ||
                                              (quietBumper && (prevPressed || nextPressed));
            if (quietRecenter) in.recenter = false;
            if (quietSkip) {
                in.skipBack = false;
                in.skipForward = false;
            }
            if (quietBumper) {
                in.previous = false;
                in.next = false;
            }
            const bool anyActionPressed = in.anyButton();
            in.back = backPressed;
            in.recenter = recenterPressed;
            in.skipBack = skipBackPressed;
            in.skipForward = skipFwdPressed;
            in.previous = prevPressed;
            in.next = nextPressed;
            bool anyButtonPressed = (anyActionPressed ||
                                     in.gripFullPressDown[0] || in.gripFullPressDown[1] ||
                                     stickMoved);
            if (!inDismissCooldown && in.select) {
                anyButtonPressed = true;
            }

            if (anyButtonPressed) {
                activity = true;
                if (!inDismissCooldown) {
                    hudDismissed_ = false;
                }
            } else if (!hudDismissed_ && (in.select || in.selectHeld || (hudCurrentlyShown && silentPlaybackAction))) {
                activity = true;
            }
            if (activity) {
                lastControllerActivity_ = nowT;
                VaultManager::instance().recordActivity(nowT);
            } else if (rawStickMoved || silentPlaybackAction) {
                VaultManager::instance().recordActivity(nowT);
            }
            pauseOnRemoval_.store(settings_.pauseOnRemoval(), std::memory_order_relaxed);
            for (int slot = 0; slot < VulkanRenderer::kMaxVideoSlots; ++slot) {
                const ScreenState& sc = screens_[slot];
                screenKind_[slot].store(!sc.open ? 0 : (sc.web ? 2 : 1), std::memory_order_release);
            }
            VulkanRenderer::setKeyboardLayout(settings_.keyboardLayout());
            pauseLivestreams_.store(settings_.pauseLivestreams(), std::memory_order_relaxed);
            if (!FileOps::instance().relocationPending() &&
                VaultManager::instance().tick(nowT, in.headValid && xr_.sessionFocused())) {
                performVaultLock();
            }
            // Show / Hide UI: the player HUD on / off (ignored with the hub open).
            if (toggleUiPressed && !hubShown && !actionMenu_.visible && pendingDl_.empty()) {
                if (!hudDismissed_ && renderer_.controlBarVisible()) {
                    dismissHud(in, nowT);
                } else {  // shown like any wake
                    hudDismissed_ = false;
                    lastControllerActivity_ = nowT;
                    VaultManager::instance().recordActivity(nowT);
                }
            }
            const bool controllersEngaged =
                (nowT - lastControllerActivity_) < std::chrono::milliseconds(3000);

#ifdef PIXELVR_HAVE_FREETYPE
            // Main Menu: open the hub, or close everything that is open.
            if (in.mainMenu) {
                in.mainMenu = false;
                if (browser_.isOpen() || settings_.isOpen() || downloadsWindow_ || actionMenu_.visible ||
                    !pendingDl_.empty()) {
                    closeHub();
                } else {
                    browser_.open();  // last tab / location
                }
            }
            // Show Controls: the floating button labels on / off, remembered like
            // Settings > Controls & Input > Button Labels. (The keyboard and the
            // combo gate consumed it.) The Buttons pages stay in Settings.
            if (in.showControls) {
                in.showControls = false;
                settings_.setButtonLabels(!settings_.buttonLabels());
                PIXELVR_LOG_INFO("Button labels %s", settings_.buttonLabels() ? "on" : "off");
            }
            // Back: one step back, never opens anything. (The keyboard, a grab and
            // a web page under the laser took theirs already.)
            if (in.back) {
                in.back = false;
                if (!pendingDl_.empty()) {
                    pendingDl_.clear();  // just the confirmation, the hub stays
                } else if (actionMenu_.visible) {
                    actionMenu_.visible = false;
                    smbModal_.active = false;
                    smbModal_.editingField = SmbModalState::Field::None;
                    if (textEntry_.active) cancelKeyboard();
                } else if (downloadsWindow_) {
                    downloadsWindow_ = false;
                } else if (settings_.isOpen()) {
                    // Up one level on a sub-page; from a top-level page B leaves Settings.
                    if (settings_.navigateBack()) xr_.triggerHaptic(renderer_.pointerHand() == 0 ? 0 : 1, 0.025f, 0.0f, 0.5f);
                    else settings_.close();
                } else if (browser_.isOpen()) {
                    // A folder still loading shows a lone "Scanning..." row (no Up
                    // entry): it is not the root, so ignore Back until it lands.
                    // (The scan thread can finish before this frame's poll.)
                    const auto& ents = browser_.entries();
                    const bool loading = browser_.isScanning() ||
                                         (ents.size() == 1 && !ents[0].isBack && ents[0].path.empty() &&
                                          ents[0].label == "Scanning...");
                    if (loading) {
                        // still loading: ignore
                    } else if (!ents.empty() && ents[0].isBack) {
                        // Up a folder, like the header's Up button.
                        browser_.setSelected(0);
                        std::string playPath;
                        browser_.activate(playPath);
                        xr_.triggerHaptic(renderer_.pointerHand() == 0 ? 0 : 1, 0.025f, 0.0f, 0.5f);
                    } else if (picking()) {  // the root while choosing a folder: stop choosing
                        cancelPick();
                    } else {  // the root of a location: close the hub
                        closeHub();
                    }
                } else if (cur().opening) {
#ifdef PIXELVR_HAVE_MEDIA
                    if (!returnFromPlex(selectedScreen_)) closeMedia(selectedScreen_);
#endif
                    if (openScreenCount() == 0) browser_.open();
                } else if (cur().open && renderer_.controlBarVisible()) {
                    dismissHud(in, nowT);  // HUD shown during playback: hide it
                }
            }
            // Previous / Next in the hub: the rail tabs (Files, Downloads,
            // Settings, then the web shortcuts), wrapping. On the Buttons pages
            // they turn the page first, then move on past the first / last one.
            if ((in.previous || in.next) && (browser_.isOpen() || settings_.isOpen() || downloadsWindow_) &&
                pendingDl_.empty() && !actionMenu_.visible) {
                const int dir = in.next ? 1 : -1;
                if (!(settings_.isOpen() && settings_.turnButtonsPage(dir, false))) cycleHubTab(dir);
                in.previous = in.next = false;
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
            updatePlugins();
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
            // The head-locked line carries short notices instead (software
            // decoding because another process holds the hardware decoder).
            // (No head-locked text line: messages go on their screen or above the control panel.)
            renderer_.setDownloadStatus(false, std::string());
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
                    di.paused = (j.state == 0 && browser_.downloadPaused());
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
                            // Where this job went (not the current setting), shortened like a path in the UI.
                            std::string where = std::filesystem::path(j.localPath).parent_path().string();
                            if (const char* home = std::getenv("HOME"); home && *home && where.rfind(home, 0) == 0) {
                                where = "~" + where.substr(std::strlen(home));
                            }
                            const std::string card = sdCardMount();
                            if (!card.empty() && where.rfind(card, 0) == 0) where = "SD card" + where.substr(card.size());
                            char mb[32];
                            std::snprintf(mb, sizeof(mb), "%.1f MB", j.total / 1048576.0);
                            di.detail = std::string("Completed \xB7 ") + mb + " \xB7 Stored in " + where;
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
                        di.detail = j.error.empty() ? std::string("Transfer Failed") : "Failed \xB7 " + j.error;
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
                    : "Stick: Scroll  |  Trigger: Play Completed  |  B: Back",
                dbtns, djList);

            // Folder-download confirmation: a modal with the size and two buttons.
            if (!pendingDl_.empty()) {
                bool confirmNow = false;
                if (pendingDlFresh_) {
                    pendingDlFresh_ = false;  // ignore the press that opened it
                } else if (in.togglePlay) {
                    confirmNow = true;  // (Back cancels it in the Back chain above)
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
                        if (!thumbTarget.empty() && !isRemotePath(thumbTarget)) {
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
                    if (!thumbTarget.empty() && !isRemotePath(thumbTarget)) {
                        thumbnailCache_.request(thumbTarget);
                        hasMeta = thumbnailCache_.getMeta(thumbTarget, meta);
                    }

                    // Same order as opening the file: a format picked by hand, a layout in
                    // the name, the file's 3D tag / picture check, else flat by resolution.
                    auto layoutBadge = [](int proj, int st) -> std::string {
                        if (proj == 2) return st > 0 ? "VR180 3D" : "VR180";
                        if (proj == 1) return st > 0 ? "VR360 3D" : "VR360";
                        if (st == 1) return "3D SBS";
                        if (st == 2) return "3D TB";
                        if (st == 3) return "3D FS";
                        return {};  // flat 2D: resolution badge
                    };
                    auto classifyMediaBadge = [&](const std::string& pathOrName, int w, int h) -> std::string {
                        const std::string lower = to_lower(pathOrName);
                        const PlaybackMemory::Entry* byHand =
                            VaultManager::isPrivatePath(pathOrName) ? nullptr : playbackMemory_.find(pathOrName);
                        const bool manual = byHand != nullptr && byHand->projection >= 0 && byHand->stereo >= 0;
                        if (manual) {
                            const std::string b = layoutBadge(byHand->projection, byHand->stereo);
                            if (!b.empty()) return b;
                        }
                        auto flatBadge = [&]() -> std::string {
                            if (w >= 7000 || h >= 3500 || contains(lower, "8k") || contains(lower, "4320p")) return "8K";
                            if (w >= 3600 || h >= 2000 || contains(lower, "4k") ||
                                contains(lower, "2160p") || contains(lower, "uhd")) return "4K";
                            if (w >= 2500 || h >= 1400 || contains(lower, "1440p") || contains(lower, "2k")) return "1440p";
                            if (w >= 1800 || h >= 1000 || contains(lower, "1080p") || contains(lower, "fhd")) return "1080p";
                            if (w >= 1200 || h >= 700 || contains(lower, "720p")) return "720p";
                            return "2D";
                        };
                        if (manual) return flatBadge();
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

                        if (contains(lower, "fs3d") || contains(lower, "frame sequential")) return "3D FS";
                        if (isTb)  return "3D TB";
                        if (isSbs) return "3D SBS";
                        if (hasMeta) {  // 3D tag in the file, else the picture check
                            const std::string b = layoutBadge(std::max(0, meta.projectionHint), meta.stereoHint);
                            if (!b.empty()) return b;
                        }
                        return flatBadge();
                    };

                    if (e.isBack) {
                        item.tag = "UP";
                    } else if (e.isNetwork) {
                        item.tag = "NETWORK";
                    } else if (e.isDir) {
                        // Folders always show FOLDER in the badge; format of contained
                        // video is added to the detail line below.
                        item.tag = "FOLDER";
                        if (!e.previewPath.empty()) {
                            std::string previewName = e.previewPath;
                            if (vaultcrypto::hasExtension(previewName)) {  // encrypted: judge by its real name
                                auto it = badgeNameCache_.find(e.previewPath);
                                if (it == badgeNameCache_.end()) {
                                    if (badgeNameCache_.size() > 2000) badgeNameCache_.clear();
                                    it = badgeNameCache_.emplace(e.previewPath, formatName(e.previewPath)).first;
                                }
                                previewName = it->second;
                                if (vaultcrypto::hasExtension(previewName)) previewName = "video";  // name not readable
                            }
                            const std::string innerFmt = classifyMediaBadge(
                                previewName, hasMeta ? meta.width : 0, hasMeta ? meta.height : 0);
                            if (!item.detail.empty()) {
                                item.detail = innerFmt + " \xB7 " + item.detail;
                            } else {
                                item.detail = innerFmt;
                            }
                        }
                    } else if (e.isVideo) {
                        // An encrypted file is judged by its real name (the label), never the random one.
                        item.tag = classifyMediaBadge(
                            (e.path.empty() || vaultcrypto::hasExtension(e.path)) ? e.label : e.path,
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
                // Only real status messages go here; the renderer draws the
                // controls hint itself on the right of the footer.
                std::string status = browser_.status();
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
                                renderer_.updateCardThumbnail(slot, thumbTarget, rgba.data(), ThumbnailCache::kWidth,
                                                              ThumbnailCache::kHeight);
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

            // Buttons pages: re-read the live bindings when they open, after focus
            // returns or the profile changes, and about once a second while shown
            // (SteamVR can switch bindings without telling the app).
            {
                const bool shown = settings_.isOpen() && settings_.showingButtons();
                if (shown && (!buttonsPageShown_ || xr_.bindingsGeneration() != bindingsGenSeen_ ||
                              (xr_.sessionFocused() && nowT - lastControlsReadT_ > std::chrono::seconds(1)))) {
                    refreshControlsInfo();
                    lastControlsReadT_ = nowT;
                }
                buttonsPageShown_ = shown;
                const bool plugPager = settings_.isOpen() && settings_.pluginPages() > 1;
                renderer_.setSettingsPager(shown ? settings_.buttonsPage() : (plugPager ? settings_.pluginPage() : -1),
                                           shown ? settings_.buttonsPages() : (plugPager ? settings_.pluginPages() : 0));
                renderer_.setSettingsStatusWarn(shown && controlsWarn_);
            }
            // Settings panel overlay
            if (settings_.isOpen()) {
                std::vector<VulkanRenderer::SettingsRowVis> sRows;
                sRows.reserve(settings_.rows().size());
                for (const auto& r : settings_.rows()) {
                    VulkanRenderer::SettingsRowVis v;
                    v.label = r.label;
                    v.value = r.value;
                    v.kind = static_cast<int>(r.kind);
                    v.steps = r.steps;
                    v.stepIdx = r.stepIdx;
                    v.enabled = r.enabled;
                    v.options = r.options;
                    sRows.push_back(std::move(v));
                }
                renderer_.setSettingsSection(settings_.section());
                renderer_.setSettingsCompact(settings_.compactPage());
                renderer_.setSettingsHeaderButton(settings_.headerButton());
                renderer_.setSettingsCrumbTargets(settings_.crumbTargets());
                renderer_.setSettings(true, settings_.title(), sRows,
                                      settings_.selected(), settings_.status());
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
                // While typing on the in-app keyboard, make sure a laser is on
                // (on whichever hand is active) so keys can always be pointed at.
                const bool kbActive = textEntry_.active;
                if (kbActive && !renderer_.laserOn(0) && !renderer_.laserOn(1)) {
                    if (in.controllers[1].active) renderer_.setLaserOn(1, true);
                    else if (in.controllers[0].active) renderer_.setLaserOn(0, true);
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
                    v.modelSpace = c.gripActive && modelCorrValid_[h];
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
            updateButtonLabels(nowT, laserOnWeb);
            // Click feedback on the panel (the renderer checks the laser is on it).
            if (in.select || in.togglePlay) renderer_.notifyPress();

            // Spatial haptic feedback on pointer hover transitions
            static int lastBrowserHov = -1;
            static int lastPageBtnHov = -1;
            static int lastSidebarHov = -1;
            static int lastSettingsHov = -1;
            static int lastSettingsPagerHov = -1;
            static int lastCtrlBtnHov = -1;
            const int curBrowserHov = renderer_.browserHovered();
            const int curPageBtnHov = renderer_.browserPageBtnHovered();
            const int curSidebarHov = renderer_.browserSidebarHovered();
            const int curSettingsHov = renderer_.settingsHovered();
            const int curSettingsPagerHov = renderer_.settingsPagerHovered();
            const int curCtrlBtnHov = renderer_.pointerButton();
            const int pH = renderer_.pointerHand();
            const int activeH = (pH >= 0 && pH <= 1) ? pH : 1;

            if (settings_.hoverHapticsEnabled() &&
                ((curBrowserHov != lastBrowserHov && curBrowserHov >= 0) ||
                 (curPageBtnHov != lastPageBtnHov && curPageBtnHov >= 0) ||
                 (curSidebarHov != lastSidebarHov && curSidebarHov >= 0) ||
                 (curSettingsHov != lastSettingsHov && curSettingsHov >= 0) ||
                 (curSettingsPagerHov != lastSettingsPagerHov && curSettingsPagerHov >= 0) ||
                 (curCtrlBtnHov != lastCtrlBtnHov && curCtrlBtnHov >= 0))) {
                xr_.triggerHaptic(activeH, 0.008f, 0.0f, settings_.hoverHapticsStrength());
            }
            lastBrowserHov = curBrowserHov;
            lastPageBtnHov = curPageBtnHov;
            lastSidebarHov = curSidebarHov;
            lastSettingsHov = curSettingsHov;
            lastSettingsPagerHov = curSettingsPagerHov;
            lastCtrlBtnHov = curCtrlBtnHov;

#ifdef PIXELVR_HAVE_FREETYPE
            // Unified Spatial Hub left navigation rail clicks (shared across Files, Downloads, Settings)
            const bool hubActive = (browserOpen || settingsOpen || downloadsWindow_) &&
                                   pendingDl_.empty() && !actionMenu_.visible;
            const int sbHover = renderer_.browserSidebarHovered();
            // The hub's close button (top right): like the Menu button, for laser-only use.
            if (hubActive && (in.togglePlay || in.select) && renderer_.hubCloseHovered()) {
                closeHub();
                xr_.triggerHaptic(renderer_.pointerHand() == 0 ? 0 : 1, 0.025f, 0.0f, 0.5f);
                in.select = in.togglePlay = false;
                interacted = true;
            }
            if (hubActive && (in.togglePlay || in.select) && sbHover >= 0) {
                if ((sbHover >= 10 && sbHover < 10 + static_cast<int>(settings_.railShortcuts().size())) ||
                    (sbHover >= 20 && sbHover < 20 + static_cast<int>(settings_.railShortcuts().size()))) {
                    // A web shortcut tab (Settings > UI & Appearance): a streaming
                    // service as a kiosk page, or the full browser (with optional Incog badge).
                    const bool forceIncog = (sbHover >= 20);
                    const int scIdx = forceIncog ? (sbHover - 20) : (sbHover - 10);
                    const SettingsMenu::RailShortcut sc = settings_.railShortcuts()[scIdx];
                    if (!sc.url.empty()) {
                        PIXELVR_LOG_INFO("Shortcut \"%s\": opening %s (forceIncog=%d)", sc.name.c_str(), sc.url.c_str(), forceIncog ? 1 : 0);
                        openWebScreen(sc.url, sc.kiosk, forceIncog);
                    } else {
                        // No address yet (e.g. the user's Jellyfin server): ask for it, save it, open it.
                        PIXELVR_LOG_INFO("Shortcut \"%s\": no address set, asking", sc.name.c_str());
                        const int idx = sc.index;
                        const bool kiosk = sc.kiosk;
                        openKeyboard(sc.name + " address (e.g. 192.168.1.10:8096)", "", false, in,
                                     [this, idx, kiosk, forceIncog](const std::string& t) {
                                         settings_.setShortcutUrl(idx, t);
                                         const auto& list = settings_.shortcuts();
                                         if (idx >= 0 && idx < static_cast<int>(list.size()) && !list[static_cast<size_t>(idx)].url.empty()) {
                                             PIXELVR_LOG_INFO("Shortcut: opening %s", list[static_cast<size_t>(idx)].url.c_str());
                                             openWebScreen(list[static_cast<size_t>(idx)].url, kiosk, forceIncog);
                                         }
                                     });
                    }
                } else if (sbHover == 0) {
                    // Main Tab: Files
                    downloadsWindow_ = false;
                    settings_.close();
                    if (!browser_.isOpen()) browser_.open();
                } else if (sbHover == 1) {
                    // Main Tab: Downloads
                    if (picking()) cancelPick(false);
                    settings_.close();
                    browser_.setOpen(true);
                    downloadsWindow_ = true;
                } else if (sbHover == 2) {
                    // Main Tab: Settings
                    if (picking()) cancelPick(false);
                    downloadsWindow_ = false;
                    settings_.open();
                } else if (sbHover >= 3 && sbHover <= 8) {
                    const int sub = sbHover - 3;
                    if (settings_.isOpen()) {
                        if (sub <= 3) {
                            settings_.setSection(sub);
                        } else if (sub == 4) {
                            settings_.openPluginsPage();
                        } else if (sub == 5 && VaultManager::instance().isUnlocked()) {
                            settings_.setSection(4);
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
                // The rail took this click: a menu it just opened (screen
                // picker) must not take it too and pick a screen on its own.
                in.select = false;
                in.togglePlay = false;
            }

            // Downloads view interaction inside Unified Spatial Hub
            // More than four downloads: the stick scrolls the list.
            if (dlWin && sbHover < 0 && pendingDl_.empty() && !actionMenu_.visible) {
                if (const int d = navStep(in.thumbstickY, nowT)) renderer_.scrollDownloads(d);
            }
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
                    const int jobBtn = renderer_.downloadsJobButtonHovered();
                    if (hov >= 0 && hov < static_cast<int>(jobs.size()) && jobBtn == 1) {
                        // Per-job X: stop the active transfer, otherwise drop the
                        // row (queued items leave the queue, partial files go).
                        if (jobs[static_cast<std::size_t>(hov)].state == 0) {
                            browser_.stopDownload();
                        } else {
                            browser_.removeJob(hov);
                        }
                        xr_.triggerHaptic(renderer_.pointerHand() == 0 ? 0 : 1, 0.020f, 0.0f, 0.4f);
                        interacted = true;
                    } else if (hov >= 0 && hov < static_cast<int>(jobs.size()) && jobBtn == 0 &&
                               (jobs[static_cast<std::size_t>(hov)].state == 2 || jobs[static_cast<std::size_t>(hov)].state == 3)) {
                        browser_.retryJob(hov);  // same place as before; refused while its SD card is out
                        xr_.triggerHaptic(renderer_.pointerHand() == 0 ? 0 : 1, 0.020f, 0.0f, 0.4f);
                        interacted = true;
                    } else if (hov >= 0 && hov < static_cast<int>(jobs.size()) && jobBtn == 0 &&
                               jobs[static_cast<std::size_t>(hov)].state == 0) {
                        browser_.togglePause();  // per-job pause / resume
                        xr_.triggerHaptic(renderer_.pointerHand() == 0 ? 0 : 1, 0.020f, 0.0f, 0.4f);
                        interacted = true;
                    } else if (hov >= 0 && hov < static_cast<int>(jobs.size())) {
                        const auto& j = jobs[static_cast<std::size_t>(hov)];
                        if (j.state == 1 && !j.localPath.empty()) {
                            if (j.isDir) {
                                browser_.openPath(j.localPath);
                                downloadsWindow_ = false;
                            } else {
                                // Like a file from the file browser: the screen
                                // picker / VR question appear in the Files hub.
                                downloadsWindow_ = false;
                                browser_.setOpen(true);
                                playFromBrowser(j.localPath);
                                // The picker it may open must not take this click too.
                                in.select = false;
                                in.togglePlay = false;
                            }
                            interacted = true;
                        }
                    }
                }
            }

            // File Action Submenu or Files view interaction inside Unified Spatial Hub
            if (actionMenu_.visible) {
                if ((!browser_.isOpen() && actionMenu_.page != 5 && actionMenu_.page != 8 && actionMenu_.page != 9 &&
                     actionMenu_.page != 11) ||
                    downloadsWindow_ || settings_.isOpen() || !pendingDl_.empty()) {
                    actionMenu_.visible = false;
                } else {
                    const Browser::Entry* fe = browser_.focused();

                    // Button Y (or configured move button) toggles/cancels
                    if (actionBtnPressed) {
                        actionMenu_.visible = false;
                    } else if (actionMenu_.page == 0 || actionMenu_.page == 1 || actionMenu_.page >= 5) {
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
                            if (actionMenu_.page == 8 && chosen >= 100) {
                                // Swap arrow between two screens in the preview.
                                const int sa = (chosen - 100) / 3;
                                const int sb = (chosen - 100) % 3;
                                std::vector<int>& o = arrangeDraft_.order;
                                auto ia = std::find(o.begin(), o.end(), sa);
                                auto ib = std::find(o.begin(), o.end(), sb);
                                if (ia != o.end() && ib != o.end()) {
                                    std::iter_swap(ia, ib);
                                    refreshArrangeMenu();
                                    xr_.triggerHaptic(renderer_.pointerHand() == 0 ? 0 : 1, 0.020f, 0.0f, 0.45f);
                                }
                                actionMenu_.selectedOption = -1;
                                chosen = -1;
                            }
                            if (chosen >= 0 && chosen < static_cast<int>(actionMenu_.options.size())) {
                                const std::string opt = actionMenu_.options[chosen];
                                if (actionMenu_.page == 0) {
                                    if (opt == "Download" || opt == "Download Folder") {
                                        actionMenu_.visible = false;
                                        downloadFocusedEntry();
                                    } else if (opt == "Open Share" || opt == "Open Bookmark") {
                                        std::string playPath;
                                        actionMenu_.visible = false;  // before: the picker may reopen it
                                        if (browser_.activate(playPath) == Browser::Result::Play && !playPath.empty()) {
                                            playFromBrowser(playPath);
                                        }
                                    } else if (opt == "Edit Network Share") {
                                        if (fe) {
                                            openSmbModal(false, fe->path);
                                        }
                                    } else if (opt == "Delete Network Share") {
                                        // A saved share and its login: never on a single click.
                                        if (fe && fe->path == pendingActionPath_) {
                                            confirmShareDelete_ = true;
                                            confirmEditorDelete_ = false;
                                            actionMenu_.page = 2;
                                            actionMenu_.title = "Delete Network Share?";
                                            actionMenu_.subtitle = "Remove the saved share \"" + fe->label + "\" and its login?";
                                            actionMenu_.options.clear();
                                            actionMenu_.options.push_back("Yes, Delete");
                                            actionMenu_.options.push_back("Cancel");
                                            actionMenu_.selectedOption = 1;
                                        } else {
                                            actionMenu_.visible = false;
                                        }
                                    } else if (opt == "Go to Bookmarks" || opt == "Go to Quick Path") {
                                        browser_.openBookmarks();
                                        actionMenu_.visible = false;
                                    } else if (opt == "Add to Bookmarks" || opt == "Add Folder to Bookmarks" ||
                                               opt == "Set as Quick Path" || opt == "Set Folder as Quick Path") {
                                        const std::string targetPath = (fe && fe->isDir) ? fe->path : browser_.currentPath();
                                        // A file's label would name the FOLDER bookmark after the file (in the
                                        // vault: its real name, written to the plain bookmarks file).
                                        browser_.addBookmark(targetPath, (fe && fe->isDir) ? fe->label : "");
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
                                        actionMenu_.visible = false;
                                        if (fe && fe->path != pendingActionPath_) {
                                            showNotice("The folder changed meanwhile: nothing was moved", true);
                                        } else if (fe && browser_.isFileActionTarget(*fe)) {
                                            startPick(PickState::Kind::Move, *fe);  // checks playing files, collects sidecars
                                            if (picking()) {
                                                FileOps::Request r;
                                                r.mode = FileOps::Mode::Move;
                                                r.sources.push_back(pick_.srcPath);
                                                for (const std::string& s : pick_.sidecars) r.sources.push_back(s);
                                                r.destDir = VaultManager::instance().vaultDir();
                                                r.label = pick_.srcLabel;
                                                const std::string why = FileOps::checkTarget(r.mode, r.sources[0], pick_.srcIsDir, r.destDir);
                                                pick_ = {};
                                                if (why.empty()) {
                                                    FileOps::instance().queue(r);
                                                    xr_.triggerHaptic(0, 0.050f, 0.0f, 0.7f);
                                                    xr_.triggerHaptic(1, 0.050f, 0.0f, 0.7f);
                                                } else {
                                                    showNotice(why, true);
                                                }
                                            }
                                        }
                                    } else if (opt == "Change 3D / VR Format") {
                                        formatMenuPath_ = pendingActionPath_;  // this file, not the playing one
                                        actionMenu_.page = 5;
                                        actionMenu_.title = "3D & VR Format";
                                        actionMenu_.subtitle = fe ? fe->label : "";
                                        actionMenu_.options.clear();
                                        actionMenu_.options.push_back("2D|Flat, standard video");
                                        actionMenu_.options.push_back("3D SBS|Side-by-side 3D");
                                        actionMenu_.options.push_back("3D TB|Top-bottom 3D (over-under)");
                                        actionMenu_.options.push_back("3D FS|Frame-sequential 3D (FS3D)");
                                        actionMenu_.options.push_back("VR180|180° 2D (mono)");
                                        actionMenu_.options.push_back("VR180 3D|180° side-by-side 3D");
                                        actionMenu_.options.push_back("VR360|360° 2D (mono)");
                                        actionMenu_.options.push_back("VR360 3D|360° top-bottom 3D");
                                        actionMenu_.options.push_back("VR360 SBS|360° side-by-side 3D");
                                        actionMenu_.options.push_back("Cancel");
                                        actionMenu_.selectedOption = 0;
                                    } else if (opt == "Move" || opt == "Copy") {
                                        actionMenu_.visible = false;
                                        if (fe && fe->path != pendingActionPath_) {
                                            showNotice("The folder changed meanwhile: choose it again", true);
                                        } else if (fe && browser_.isFileActionTarget(*fe)) {
                                            startPick(opt == "Move" ? PickState::Kind::Move : PickState::Kind::Copy, *fe);
                                            if (picking()) xr_.triggerHaptic(1, 0.040f, 0.0f, 0.6f);
                                        }
                                    } else if (opt == "Rename") {
                                        actionMenu_.visible = false;
                                        if (fe && browser_.isFileActionTarget(*fe)) {
                                            pendingActionPath_ = fe->path;
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
                                                    const Browser::Entry* still = browser_.focused();
                                                    if (!still || still->path != pendingActionPath_) {
                                                        // The list changed while typing: never rename another file.
                                                        showNotice("The folder changed meanwhile: nothing was renamed", true);
                                                    } else if (!clean.empty()) {
                                                        if (browser_.renameFocused(clean)) {
                                                            xr_.triggerHaptic(0, 0.050f, 0.0f, 0.7f);
                                                            xr_.triggerHaptic(1, 0.050f, 0.0f, 0.7f);
                                                        }
                                                    }
                                                });
                                        }
                                    } else if (opt == "Delete") {
                                        confirmShareDelete_ = false;
                                        confirmEditorDelete_ = false;
                                        pendingActionPath_ = fe ? fe->path : std::string();
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
                                } else if (actionMenu_.page == 10) {
                                    // 180/360 with other screens open.
                                    actionMenu_.visible = false;
                                    if (opt == "Play") {
                                        for (int i = 0; i < VulkanRenderer::kMaxVideoSlots; ++i) {
                                            if (screens_[i].open) closeMedia(i);
                                        }
#ifdef PIXELVR_HAVE_MEDIA
                                        loadMedia(pendingPlayPath_, 0);
#endif
#ifdef PIXELVR_HAVE_FREETYPE
                                        if (!lastLoadFailed_) browser_.setOpen(false);
#endif
                                    }
                                } else if (actionMenu_.page == 9) {
                                    // Chromium install dialog.
                                    if (opt == "Open YouTube" || opt == "Open browser") {
                                        actionMenu_.visible = false;
                                        openWebScreen(installWebUrl_, installWebKiosk_);
                                    } else if (opt == "Install Chromium" || opt == "Try again") {
                                        chromiumInstaller_.reset();
                                        chromiumInstaller_.start();
                                        showChromiumDialog();
                                    } else if (opt == "Cancel install") {
                                        chromiumInstaller_.cancel();
                                        chromiumInstaller_.reset();
                                        installWebSlot_ = -1;
                                        actionMenu_.visible = false;
                                    } else {  // Cancel / Close / Hide (the install keeps going)
                                        if (chromiumInstaller_.state() != ChromiumInstaller::State::Running) {
                                            chromiumInstaller_.reset();
                                            installWebSlot_ = -1;
                                        }
                                        actionMenu_.visible = false;
                                    }
                                } else if (actionMenu_.page == 11) {
                                    // Tracks: pick one; the file reloads at the same position.
                                    actionMenu_.visible = false;
#ifdef PIXELVR_HAVE_MEDIA
                                    if (chosen < static_cast<int>(trackRows_.size()) && cur().open) {
                                        const TrackRow tr = trackRows_[static_cast<std::size_t>(chosen)];
                                        if (tr.audio) {
                                            settings_.setSelectedAudioStream(tr.index);
                                            if (tr.index != cur().engine.currentAudioStream()) switchTracks(false);
                                        } else {
                                            settings_.setSelectedVideoStream(tr.index);
                                            if (tr.index != cur().engine.currentVideoStream()) switchTracks(false);
                                        }
                                        xr_.triggerHaptic(renderer_.pointerHand() == 0 ? 0 : 1, 0.02f, 0.0f, 0.5f);
                                    }
#endif
                                } else if (actionMenu_.page == 5) {
                                    if (opt == "2D|Flat, standard video") {
                                        chooseFormat(0, 0);
                                    } else if (opt == "3D SBS|Side-by-side 3D") {
                                        chooseFormat(0, 1);
                                    } else if (opt == "3D TB|Top-bottom 3D (over-under)") {
                                        chooseFormat(0, 2);
                                    } else if (opt == "3D FS|Frame-sequential 3D (FS3D)") {
                                        chooseFormat(0, 3);
                                    } else if (opt == "VR180|180° 2D (mono)") {
                                        chooseFormat(2, 0);
                                    } else if (opt == "VR180 3D|180° side-by-side 3D") {
                                        chooseFormat(2, 1);
                                    } else if (opt == "VR360|360° 2D (mono)") {
                                        chooseFormat(1, 0);
                                    } else if (opt == "VR360 3D|360° top-bottom 3D") {
                                        chooseFormat(1, 2);
                                    } else if (opt == "VR360 SBS|360° side-by-side 3D") {
                                        chooseFormat(1, 1);
                                    }
#ifdef PIXELVR_HAVE_MEDIA
                                    if (opt != "Cancel" && !formatStoredOnly_ && (cur().open || cur().opening) &&
                                        !VaultManager::isPrivatePath(cur().path)) {
                                        playbackMemory_.setFormat(cur().path, cur().projection, cur().stereo,
                                                                  cur().fingerprint);
                                        playbackMemory_.save();
                                        cur().formatByHand = true;
                                        cur().formatPending = false;
                                    }
#endif
                                    xr_.triggerHaptic(0, 0.040f, 0.0f, 0.6f);
                                    xr_.triggerHaptic(1, 0.040f, 0.0f, 0.6f);
                                    actionMenu_.visible = false;
                                } else if (actionMenu_.page == 8) {
                                    // 0..P-1 = presets, then Snap, Style, Apply, Cancel.
                                    const int presetCount = static_cast<int>(arrangePresets_.size());
                                    const int id = chosen - presetCount;
                                    if (chosen < presetCount) {
                                        selectArrangePreset(arrangePresets_[static_cast<std::size_t>(chosen)]);
                                        refreshArrangeMenu();
                                    } else if (id == 0) {
                                        arrangeDraft_.snap = !arrangeDraft_.snap;
                                        refreshArrangeMenu();
                                    } else if (id == 1) {
                                        arrangeDraft_.flat = !arrangeDraft_.flat;
                                        refreshArrangeMenu();
                                    } else if (id == 2) {
                                        if (presetCount > 0) {
                                            applyArrangement(arrangeDraft_);
                                            if (arrangeDraft_.order.size() == 3) {  // what a third screen joins next time
                                                if (arrangeDraft_.preset == ArrangePreset::SideBySide) rowOfThreeChosen_ = true;
                                                else if (arrangeDraft_.preset == ArrangePreset::TwoBelowOneAbove) rowOfThreeChosen_ = false;
                                            }
                                            xr_.triggerHaptic(renderer_.pointerHand() == 0 ? 0 : 1, 0.030f, 0.0f, 0.5f);
                                        }
                                        actionMenu_.visible = false;
                                    } else {
                                        actionMenu_.visible = false;
                                    }
                                } else if (actionMenu_.page == 6) {
                                    // Screen picker: 0..2 = screen slot tile, 3 = Cancel.
                                    if (chosen < VulkanRenderer::kMaxVideoSlots) {
                                        const bool occupied = screens_[chosen].open || screens_[chosen].opening;
                                        if (!occupied && !decoderBudgetAllows(pendingPlayPath_)) {
                                            actionMenu_.subtitle = "Not enough decoder capacity - close a screen first";
                                        } else {
                                            if (pendingPlayPath_ == kWebPath) {
                                                loadWeb(chosen, pendingWebUrl_, pendingWebKiosk_, pendingWebIncognito_);
                                            } else {
#ifdef PIXELVR_HAVE_MEDIA
                                                loadMedia(pendingPlayPath_, chosen);
#endif
                                            }
                                            actionMenu_.visible = false;
                                            if (!lastLoadFailed_) browser_.setOpen(false);
                                        }
                                    } else {
                                        actionMenu_.visible = false;
                                    }
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
                            const Browser::Entry* del = browser_.focused();
                            if (confirmEditorDelete_) {
                                confirmEditorDelete_ = false;
                                if (chosen == 0) {
                                    deleteSmbModal();
                                    xr_.triggerHaptic(1, 0.050f, 0.0f, 0.7f);
                                } else {  // back to the editor
                                    actionMenu_.page = 4;
                                    actionMenu_.subtitle.clear();
                                    updateSmbModalMenu();
                                    actionMenu_.selectedOption = 9;
                                }
                            } else if (confirmShareDelete_) {
                                confirmShareDelete_ = false;
                                if (chosen == 0 && del && del->path == pendingActionPath_) {
                                    deleteShareEntry(del->path);
                                    browser_.openNetwork();
                                    xr_.triggerHaptic(1, 0.050f, 0.0f, 0.7f);
                                }
                                actionMenu_.visible = false;
                            } else if (chosen == 0 && (!del || del->path != pendingActionPath_ || !browser_.isFileActionTarget(*del))) {
                                showNotice("The folder changed meanwhile: nothing was deleted", true);
                                actionMenu_.visible = false;
                            } else if (chosen == 0) {
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
                            if (smbModal_.isNew && actionMenu_.selectedOption == 9) actionMenu_.selectedOption += dy;
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
                                                if (const std::size_t sch = h.find("://"); sch != std::string::npos) h = h.substr(sch + 3);
                                                while (!h.empty() && (std::isspace(static_cast<unsigned char>(h.front())) || h.front() == '/' || h.front() == '\\')) h.erase(h.begin());
                                                while (!h.empty() && (std::isspace(static_cast<unsigned char>(h.back())) || h.back() == '/' || h.back() == '\\')) h.pop_back();
                                                if (smbModal_.protocol != "smb") {  // "host:port"
                                                    const std::size_t colon = h.rfind(':');
                                                    if (colon != std::string::npos && h.find(':') == colon && colon + 1 < h.size() &&
                                                        h.find_first_not_of("0123456789", colon + 1) == std::string::npos) {
                                                        smbModal_.port = h.substr(colon + 1);
                                                        h = h.substr(0, colon);
                                                    }
                                                }
                                                smbModal_.host = h;
                                            } else if (field == SmbModalState::Field::Port) {
                                                std::string d;
                                                for (char c : text) {
                                                    if (c >= '0' && c <= '9') d += c;
                                                }
                                                smbModal_.port = d;
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
                                    openKeyboardForField(SmbModalState::Field::Host, "Server IP / Hostname", smbModal_.host, false);
                                } else if (chosen == 1) {
                                    openKeyboardForField(SmbModalState::Field::Share,
                                                         smbModal_.protocol == "smb" ? "SMB Share Name (empty for all)"
                                                                                     : "Start folder (empty for the root)",
                                                         smbModal_.share, false);
                                } else if (chosen == 2) {
                                    // Pointing at a segment picks it; stick + A toggles.
                                    const int seg = renderer_.actionMenuKeyHovered();
                                    smbModal_.anon = (seg >= 0) ? (seg == 0) : !smbModal_.anon;
                                    updateSmbModalMenu();
                                } else if ((chosen == 3 || chosen == 4) && smbModal_.anon) {
                                    // Guest access needs no credentials.
                                } else if (chosen == 3) {
                                    openKeyboardForField(SmbModalState::Field::User, "Username", smbModal_.user, false);
                                } else if (chosen == 4) {
                                    openKeyboardForField(SmbModalState::Field::Pass, "Password", smbModal_.pass, true);
                                } else if (chosen == 5) {
                                    if (smbModal_.protocol == "smb") {
                                        openKeyboardForField(SmbModalState::Field::Workgroup, "SMB Workgroup / Domain", smbModal_.workgroup, false);
                                    } else {
                                        openKeyboardForField(SmbModalState::Field::Port, "Port (empty for the default)", smbModal_.port, false);
                                    }
                                } else if (chosen == 6) {
                                    openKeyboardForField(SmbModalState::Field::Label, "Nickname", smbModal_.label, false);
                                } else if (chosen == 7) {
                                    // Save SMB Share
                                    saveSmbModal();
                                } else if (chosen == 8) {
                                    // Cancel (footer button or header X)
                                    actionMenu_.visible = false;
                                    smbModal_.active = false;
                                    smbModal_.editingField = SmbModalState::Field::None;
                                    if (textEntry_.active) cancelKeyboard();
                                } else if (chosen == 9 && !smbModal_.isNew) {
                                    // Asks once more: the second press within 4 s deletes.
                                    // The same question as "Delete Network Share" in the file menu;
                                    // Cancel comes back to the editor as it was.
                                    confirmEditorDelete_ = true;
                                    confirmShareDelete_ = false;
                                    actionMenu_.page = 2;
                                    actionMenu_.title = "Delete Network Share?";
                                    actionMenu_.subtitle = "Remove the saved share \"" + smbModal_.host +
                                                           (smbModal_.share.empty() ? "" : "/" + smbModal_.share) + "\" and its login?";
                                    actionMenu_.options.clear();
                                    actionMenu_.options.push_back("Yes, Delete");
                                    actionMenu_.options.push_back("Cancel");
                                    actionMenu_.selectedOption = 1;
                                } else if (chosen == 10) {
                                    static const char* const kProtocols[4] = {"smb", "sftp", "dav", "davs"};
                                    const int seg = renderer_.actionMenuKeyHovered();
                                    int cur = 0;
                                    for (int k = 0; k < 4; ++k) {
                                        if (smbModal_.protocol == kProtocols[k]) cur = k;
                                    }
                                    const std::string next = kProtocols[seg >= 0 ? seg : (cur + 1) % 4];
                                    if (next != smbModal_.protocol) {
                                        smbModal_.protocol = next;
                                        smbModal_.port.clear();
                                    }
                                    updateSmbModalMenu();
                                }
                            }
                            interacted = true;
                        }
                    }
                }
            } else if (actionBtnPressed && browser_.isOpen() && !downloadsWindow_ &&
                       !settings_.isOpen() && pendingDl_.empty()) {
                if (picking()) cancelPick();
                else openActionMenu();
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
                const int crumbHover = renderer_.browserCrumbHovered();
                if ((in.togglePlay || in.select) && crumbHover >= 0 && sbHover < 0) {
                    browser_.openCrumb(crumbHover);  // jump to that level of the folder path
                    xr_.triggerHaptic(1, 0.025f, 0.0f, 0.5f);
                    interacted = true;
                } else if ((in.togglePlay || in.select) && pbHover >= 0 && sbHover < 0) {
                    if (pbHover == 7) {
                        confirmPick();
                    } else if (pbHover == 8) {
                        cancelPick();
                        xr_.triggerHaptic(1, 0.025f, 0.0f, 0.5f);
                    } else if (pbHover == 2) {
                        // Cycle View Mode: BigCards (0) -> MiniCards (1) -> CompactList (2)
                        int curMode = settings_.browserViewMode();
                        curMode = (curMode + 1) % 3;
                        settings_.setBrowserViewMode(curMode);
                        settings_.saveConfig();
                        renderer_.setBrowserViewMode(static_cast<VulkanRenderer::BrowserViewMode>(curMode));
                        renderer_.scrollBrowser(0);
                        xr_.triggerHaptic(1, 0.025f, 0.0f, 0.5f);
                    } else if (pbHover == 5) {
                        // Cycle sort order: Name -> Newest -> Largest
                        settings_.setBrowserSort(settings_.browserSort() + 1);
                        browser_.setSortMode(settings_.browserSort());
                        renderer_.setBrowserFirst(0);
                        renderer_.scrollBrowser(0);
                        xr_.triggerHaptic(1, 0.025f, 0.0f, 0.5f);
                    } else if (pbHover == 6 && FileOps::instance().relocationPending() && browser_.inVault()) {
                        showNotice("The vault is being moved: wait until that is done", true);
                    } else if (pbHover == 6) {
                        openKeyboard("New Folder", "", false, in, [this](const std::string& text) {
                            std::string name = text;
                            while (!name.empty() && std::isspace(static_cast<unsigned char>(name.front()))) name.erase(name.begin());
                            while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back()))) name.pop_back();
                            if (!name.empty() && browser_.createFolder(name)) xr_.triggerHaptic(1, 0.040f, 0.0f, 0.6f);
                        });
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
                    const Browser::Entry* pickRow = browser_.focused();
                    const bool pickBlocks = picking() && pickRow && !pickRow->isDir && !pickRow->isBack && !pickRow->isNetwork;
                    if ((in.togglePlay || in.select) && pickBlocks && renderer_.pointerButton() < 0 &&
                        (hovered >= 0 || stickActive)) {
                        // Choosing a target folder: files don't open (and nothing plays meanwhile).
                        xr_.triggerHaptic(1, 0.015f, 0.0f, 0.3f);
                    } else if ((in.togglePlay || in.select) &&
                        renderer_.pointerButton() < 0 && hovered >= 0) {
                        std::string playPath;
                        Browser::Result res = browser_.activate(playPath);
                        if (picking() && res != Browser::Result::Navigated) {
                            // only folders while choosing
                        } else if (res == Browser::Result::AddSmbShare) {
                            openSmbModal(true);
                        } else if (res == Browser::Result::Play && !playPath.empty()) {
                            playFromBrowser(playPath);
                        } else if (res == Browser::Result::InstallPlugin) {
                            installPlugin(playPath);
                        }
                    } else if ((in.togglePlay || in.select) &&
                               renderer_.pointerButton() < 0 && stickActive) {
                        std::string playPath;
                        Browser::Result res = browser_.activate(playPath);
                        if (picking() && res != Browser::Result::Navigated) {
                            // only folders while choosing
                        } else if (res == Browser::Result::AddSmbShare) {
                            openSmbModal(true);
                        } else if (res == Browser::Result::Play && !playPath.empty()) {
                            playFromBrowser(playPath);
                        } else if (res == Browser::Result::InstallPlugin) {
                            installPlugin(playPath);
                        }
                    }
                }
            }
            if ((browser_.isOpen() || downloadsWindow_) && in.recenter && in.headValid) {
                recenterView(in);
            }

#endif

            if (settings_.isOpen() && sbHover < 0) {
                if (const int d = navStep(in.thumbstickY, nowT)) {
                    settings_.move(d * (settings_.compactPage() ? 1 : 2));  // list pages have one column
                }
                if (const int dx = navStepX(in.thumbstickX, nowT)) {
                    // The Buttons pages are read-only: left / right turns the page.
                    if (!settings_.turnButtonsPage(dx, true)) settings_.move(dx);
                }
                // Header pager (< n / m >) on the Buttons pages.
                if (const int pg = renderer_.settingsPagerHovered(); pg >= 0 && (in.select || in.togglePlay)) {
                    if (settings_.turnButtonsPage(pg == 0 ? -1 : 1, true)) {
                        xr_.triggerHaptic(renderer_.pointerHand() == 0 ? 0 : 1, 0.025f, 0.0f, 0.5f);
                    }
                    in.select = in.togglePlay = false;
                }

                // The header path: a click on a parent page goes there.
                if (const int crumbTo = renderer_.settingsCrumbHovered(); crumbTo >= 0 && (in.select || in.togglePlay)) {
                    if (settings_.navigateUpTo(crumbTo)) xr_.triggerHaptic(renderer_.pointerHand() == 0 ? 0 : 1, 0.025f, 0.0f, 0.5f);
                    in.select = in.togglePlay = false;
                }
                const int hovered = renderer_.settingsHovered();
                if (hovered >= 0) {
                    renderer_.setSettingsSelected(hovered);
                    settings_.setSelected(hovered);
                }
                if (in.select && renderer_.settingsHeaderButtonHovered()) {  // "Install plugins"
                    settings_.openPluginStore();
                    xr_.triggerHaptic(renderer_.pointerHand() == 0 ? 0 : 1, 0.02f, 0.0f, 0.4f);
                    in.select = in.togglePlay = false;
                }
                // Shortcut list: a held trigger on a line's handle drags it; the drop point
                // follows the laser and the list (= rail) order changes on release.
                settings_.tickShortcuts();
                settings_.tickPlugins();
                const auto& setRows = settings_.rows();
                auto shortcutRowAt = [&](int r) {
                    return settings_.compactPage() && r >= 0 && r < static_cast<int>(setRows.size()) &&
                           setRows[static_cast<size_t>(r)].kind == SettingsMenu::Kind::Shortcut;
                };
                if (shortcutDragFrom_ >= 0) {
                    if (!settings_.compactPage()) {
                        shortcutDragFrom_ = shortcutDragTo_ = -1;
                    } else if (in.selectHeld) {
                        if (shortcutRowAt(hovered)) shortcutDragTo_ = hovered;
                    } else {
                        const int from = setRows[static_cast<size_t>(shortcutDragFrom_)].data;
                        const int to = shortcutRowAt(shortcutDragTo_) ? setRows[static_cast<size_t>(shortcutDragTo_)].data : from;
                        PIXELVR_LOG_INFO("Shortcuts: moved %d -> %d", from, to);
                        settings_.shortcutMove(from, to);
                        shortcutDragFrom_ = shortcutDragTo_ = -1;
                    }
                    in.select = in.togglePlay = false;  // the drag owns the trigger
                }
                renderer_.setSettingsDrag(shortcutDragFrom_, shortcutDragTo_);
                if (in.select && settings_.compactPage() && hovered >= 0 && hovered < static_cast<int>(setRows.size()) &&
                    setRows[static_cast<size_t>(hovered)].kind == SettingsMenu::Kind::PluginLine) {
                    const int idx = setRows[static_cast<size_t>(hovered)].data;
                    switch (renderer_.settingsSegmentHovered()) {
                        case 1: settings_.pluginToggle(idx); break;
                        case 4: settings_.openPluginUpdate(idx); break;  // the green update icon
                        case 3:
                            if (settings_.pluginRemoveArmed(idx)) {
                                std::string message;
                                plugins_.uninstall(idx, message);
                                settings_.openPluginsPage();
                                settings_.showStatus(message);
                            }
                            break;
                        default: settings_.pluginEdit(idx); break;  // Edit, or the name / status
                    }
                    xr_.triggerHaptic(renderer_.pointerHand() == 0 ? 0 : 1, 0.02f, 0.0f, 0.4f);
                    in.select = in.togglePlay = false;
                }
                if (in.select && shortcutRowAt(hovered)) {
                    const int idx = setRows[static_cast<size_t>(hovered)].data;
                    switch (renderer_.settingsSegmentHovered()) {
                        case 0: shortcutDragFrom_ = shortcutDragTo_ = hovered; break;  // the grab handle
                        case 1: settings_.shortcutToggle(idx); break;
                        case 3: settings_.shortcutDelete(idx); break;
                        default: settings_.shortcutEdit(idx); break;  // Edit, or the name / address
                    }
                    xr_.triggerHaptic(renderer_.pointerHand() == 0 ? 0 : 1, 0.02f, 0.0f, 0.4f);
                    in.select = in.togglePlay = false;
                }
                // Trigger activates the selection made by the laser or the stick;
                // Play / OK activates the selected row, pointed at or not.
                // (List pages show no stick selection: there only the row under the laser acts, so a
                // stray stick flick + Play can never start "Move vault" or "Install".)
                const bool listPage = settings_.compactPage();
                if (((in.select && (hovered >= 0 || std::fabs(in.thumbstickY) > 0.25f)) || in.togglePlay) &&
                    (!listPage || hovered >= 0)) {
                    const int seg = (hovered >= 0) ? renderer_.settingsSegmentHovered() : -1;
                    auto act = (seg >= 0) ? settings_.activateSegment(seg) : settings_.activate();
                        if (act == SettingsMenu::Action::PickDownloadFolder) {
                            startDownloadDirPick();
                        } else if (act == SettingsMenu::Action::StartVaultMoveLock) {
                            startVaultMove(true);
                        } else if (act == SettingsMenu::Action::StartVaultMoveUnlock) {
                            startVaultMove(false);
                        } else if (act == SettingsMenu::Action::CancelVaultMove) {
                            if (FileOps::instance().relocationPending()) {
                                FileOps::instance().cancelRelocation();
                                settings_.setVaultMoveStatus("Stopping vault move...");
                                xr_.triggerHaptic(0, 0.040f, 0.0f, 0.6f);
                                xr_.triggerHaptic(1, 0.040f, 0.0f, 0.6f);
                            }
                        } else if (act == SettingsMenu::Action::CycleDistance) {
                            renderer_.setScreenDistance(settings_.screenDistance());
                            if (arrangementActive_) {
                                arrangement_.dist = renderer_.screenDistance();
                                applyArrangement(arrangement_);
                            }
                        } else if (act == SettingsMenu::Action::CycleCurvature) {
                            renderer_.setScreenCurvature(settings_.screenCurvature());
                            if (arrangementActive_) {
                                arrangement_.curvature = renderer_.screenCurvature();
                                applyArrangement(arrangement_);
                            }
                        }
#ifdef PIXELVR_HAVE_MEDIA
                        else if (act == SettingsMenu::Action::PickVideoStream ||
                                 act == SettingsMenu::Action::PickAudioStream) {
                            switchTracks(false);
                        } else if (act == SettingsMenu::Action::PickLanguage) {
                            switchTracks(true);
                        }
#endif
                        else if (act == SettingsMenu::Action::EditShortcutName || act == SettingsMenu::Action::AddShortcut) {
                            // Add opens the new entry's page and asks for its name straight away.
                            const int idx = settings_.shortcutIdx();
                            const auto& list = settings_.shortcuts();
                            const std::string cur = (act == SettingsMenu::Action::AddShortcut || idx < 0 ||
                                                     idx >= static_cast<int>(list.size()))
                                                        ? std::string()
                                                        : list[static_cast<size_t>(idx)].name;
                            openKeyboard("Shortcut name", cur, false, in,
                                         [this, idx](const std::string& t) { settings_.setShortcutName(idx, t); });
                        } else if (act == SettingsMenu::Action::EditShortcutUrl) {
                            const int idx = settings_.shortcutIdx();
                            const auto& list = settings_.shortcuts();
                            const std::string cur = (idx >= 0 && idx < static_cast<int>(list.size())) ? list[static_cast<size_t>(idx)].url
                                                                                                         : std::string();
                            openKeyboard("Shortcut address (e.g. 192.168.1.10:8096 or https://...)", cur, false, in,
                                         [this, idx](const std::string& t) { settings_.setShortcutUrl(idx, t); });
                        } else if (act == SettingsMenu::Action::ToggleRemoveBlackBars) {
                            for (int s = 0; s < VulkanRenderer::kMaxVideoSlots; ++s) applyLetterbox(s);
                        } else if (act == SettingsMenu::Action::InstallRepoPlugin) {
                            const auto entries = pluginRepo_.entries();
                            const int ei = settings_.repoEntryToInstall();
                            if (ei >= 0 && ei < static_cast<int>(entries.size())) {
                                pluginRepo_.download(entries[static_cast<size_t>(ei)]);
                                settings_.refresh();
                                settings_.showStatus("Downloading " + entries[static_cast<size_t>(ei)].name + "...");
                            }
                        } else if (act == SettingsMenu::Action::InstallPluginPackage) {
                            if (!settings_.pluginPackageToInstall().empty()) installPlugin(settings_.pluginPackageToInstall());
                        } else if (act == SettingsMenu::Action::EditPluginText) {
                            openKeyboard("Plugin setting", settings_.editingPluginText(), false, in,
                                         [this](const std::string& t) { settings_.submitPluginText(t); });
                        }
                        else if (act == SettingsMenu::Action::ToggleHoverHaptics) {
                            const int pH = renderer_.pointerHand();
                            const int activeH = (pH >= 0 && pH <= 1) ? pH : 1;
                            xr_.triggerHaptic(activeH, 0.020f, 0.0f, settings_.hoverHapticsEnabled() ? settings_.hoverHapticsStrength() : 0.2f);
                        } else if (act == SettingsMenu::Action::CycleHoverHapticsStrength) {
                            const int pH = renderer_.pointerHand();
                            const int activeH = (pH >= 0 && pH <= 1) ? pH : 1;
                            xr_.triggerHaptic(activeH, 0.025f, 0.0f, settings_.hoverHapticsStrength());
                        }
                }
                if (in.recenter && in.headValid) {
                    recenterView(in);
                }
            }

            // Cinema Transport Dock is only active when the Unified Spatial Hub is closed.
            const bool barOk = !browserOpen && !settingsOpen && !downloadsWindow_ &&
                               pendingDl_.empty() && !actionMenu_.visible && !smbModal_.active;
            // Multi-screen: point at a screen and pull the trigger to select it; the
            // dock (including its screen row) then controls that screen.
            if (barOk && multiScreenEnabled() && movingScreen_ < 0) {
                const int ps = renderer_.pointerScreen();
                if (ps >= 0 && renderer_.pointerButton() < 0 && !renderer_.pointerOnTimeline()) {
                    if (in.select) {
                        if (ps != selectedScreen_) selectScreen(ps);
                        if (screensUnlocked_) startGrabbingScreen(ps, in);
                        xr_.triggerHaptic(renderer_.pointerHand() == 0 ? 0 : 1, 0.020f, 0.0f, 0.45f);
                        in.select = false;
                        interacted = true;
                    }
                }
            }
            if (barOk && renderer_.controlBarVisible() && !wasHudDismissed) {
                // Point-and-click the media control bar with the trigger.
                const int btn = renderer_.pointerButton();
                // Only the trigger clicks the bar: Play / OK always plays / pauses
                // during playback, wherever the laser points.
                if (in.select && btn >= 0) {
                    PIXELVR_LOG_INFO("control bar click: button=%d", btn);
                    const auto cb = static_cast<VulkanRenderer::CtrlButton>(btn);
                    if (cb == VulkanRenderer::CtrlButton::Stop) {
#ifdef PIXELVR_HAVE_MEDIA
                        if (!returnFromPlex(selectedScreen_)) closeMedia();
#endif
#ifdef PIXELVR_HAVE_FREETYPE
                        if (openScreenCount() == 0) browser_.open();  // large file selection
#endif
                    } else if (cb == VulkanRenderer::CtrlButton::Recenter) {
                        if (in.headValid) {
                            recenterView(in);
                        }
                    } else if (cb == VulkanRenderer::CtrlButton::Files) {
#ifdef PIXELVR_HAVE_FREETYPE
                        browser_.open();
#endif
                    }
#ifdef PIXELVR_HAVE_MEDIA
                    else if (cur().open &&
                             cb == VulkanRenderer::CtrlButton::PlayPause) {
                        curTogglePause();
                    } else if (cur().open &&
                               cb == VulkanRenderer::CtrlButton::SeekBack) {
                        curSeekRelative(-10.0);
                    } else if (cur().open &&
                               cb == VulkanRenderer::CtrlButton::SeekFwd) {
                        curSeekRelative(10.0);
                    } else if (cur().web && cb == VulkanRenderer::CtrlButton::FormatToggle) {
                        webHosts_[selectedScreen_].toggleFullscreen();  // F11
                    } else if (cur().web && cb == VulkanRenderer::CtrlButton::AudioToggle) {
                        if (!textEntry_.active) {
                            openWebKeyboard(selectedScreen_, nullptr, in);
                            webHosts_[selectedScreen_].requestFocusReport();  // fill in the field, if any
                        }
                    } else if (cb == VulkanRenderer::CtrlButton::FormatToggle) {
                        openFormatMenu();
                    } else if (cb == VulkanRenderer::CtrlButton::AudioToggle) {
                        openTracksMenu();
                    } else if (cb == VulkanRenderer::CtrlButton::ScreenToggle) {
                        if (settings_.projectionMode() == 0 &&
                            (selectedScreen_ > 0 || renderer_.screenPlacement(selectedScreen_).active)) {
                            VulkanRenderer::ScreenPlacement pl = renderer_.screenPlacement(selectedScreen_);
                            pl.curvature = (pl.curvature >= 0.99f) ? 0.0f : std::min(1.0f, pl.curvature + 0.25f);
                            if (groupedScreen(selectedScreen_)) {
                                // A snapped wall bends together; on a curved wall the
                                // curve follows the wall, so it is just on / off there.
                                const bool wall = !arrangement_.flat && arrangement_.order.size() > 1;
                                arrangement_.curvature = !wall ? pl.curvature
                                                       : (arrangement_.curvature > 0.005f ? 0.0f : 1.0f);
                                applyArrangement(arrangement_);
                            } else {
                                renderer_.setScreenPlacement(selectedScreen_, pl);
                            }
                        } else if (settings_.projectionMode() == 0) {
                            settings_.setCurvatureIdx(settings_.curvatureIdx() + 1);
                            renderer_.setScreenCurvature(settings_.screenCurvature());
                        } else {
                            renderer_.setSpherePitch(0.0f);
                            renderer_.setSphereYawOffset(0.0f);
                            renderer_.setSphereZoom(1.0f);
                        }
                    } else if (cb == VulkanRenderer::CtrlButton::HideHud) {
                        dismissHud(in, nowT);
                    } else if (cb == VulkanRenderer::CtrlButton::Volume) {
                        // On the bar: jump there and follow while the trigger is
                        // held; on the speaker icon: mute / unmute.
                        const float lv = renderer_.volumeLevelAtPointer(false);
                        if (lv >= 0.0f) {
                            cur().volume = lv;
                            if (lv > 0.0f) cur().muted = false;
                            volumeDragging_ = true;
                        } else {
                            cur().muted = !cur().muted;
                        }
                    } else if (cb == VulkanRenderer::CtrlButton::ScreenAudioFollow) {
                        settings_.setAudioFollow(!settings_.audioFollow());
                        if (settings_.audioFollow()) {
                            applyAudioFollow();
                        } else {
                            for (ScreenState& s : screens_) s.muted = false;  // back to all screens heard
                        }
                    } else if (cb == VulkanRenderer::CtrlButton::ScreenLock) {
                        screensUnlocked_ = !screensUnlocked_;
                    } else if (cb == VulkanRenderer::CtrlButton::ScreenNumbers) {
                        settings_.setScreenNumbers(!settings_.screenNumbers());
                    } else if (cb == VulkanRenderer::CtrlButton::ScreenLayout) {
                        openArrangeMenu();
                    } else if (cb == VulkanRenderer::CtrlButton::ScreenClose) {
                        if (!returnFromPlex(selectedScreen_)) closeMedia(selectedScreen_);
#ifdef PIXELVR_HAVE_FREETYPE
                        if (openScreenCount() == 0) browser_.open();
#endif
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
                    if (curLive() != 0) webHosts_[selectedScreen_].mediaGoLive();  // "LIVE": back to the edge
                    else renderer_.toggleTimeDisplay();  // total <-> time remaining
                    interacted = true;
                }
            }
            // Viewpoint / transport controls only when no panel is open (the
            // thumbstick drives in-panel navigation otherwise).
            if (barOk && !browserOpen) {
                const bool hudShown = !wasHudDismissed && renderer_.controlBarVisible();
                const bool sphereMode = settings_.projectionMode() != 0;
                // The shift stays on until the sticks are back in the middle: letting go of the
                // bumper first must not turn the rest of a pan into a scrub or a 30 s step.
                const bool sticksCentred = std::fabs(in.thumbstickX) < 0.2f && std::fabs(in.thumbstickY) < 0.2f &&
                                           std::fabs(in.thumbstickLX) < 0.2f && std::fabs(in.thumbstickRX) < 0.2f;
                const bool bumperHeldNow = sphereMode && (in.previousHeld || in.nextHeld);
                if (bumperHeldNow) panShiftLatched_ = true;
                else if (sticksCentred || !sphereMode) panShiftLatched_ = false;
                const bool bumperShift = bumperHeldNow || (sphereMode && panShiftLatched_);
                if (bumperShift) seekStepArmed_ = false;
                if (in.recenter && in.headValid) {
                    recenterView(in);
                    if (hudShown) interacted = true;
                }
                // Dragging the volume bar (started by a click on it above).
                if (volumeDragging_) {
                    const float lv = in.selectHeld ? renderer_.volumeLevelAtPointer(true) : -1.0f;
                    if (lv < 0.0f) {
                        volumeDragging_ = false;
                    } else {
                        cur().volume = lv;
                        if (lv > 0.0f) cur().muted = false;
                        interacted = true;
                    }
                }
                // Pointing at the VOLUME pill, the stick sets this screen's volume.
                if (hudShown &&
                    renderer_.pointerButton() == static_cast<int>(VulkanRenderer::CtrlButton::Volume) &&
                    std::fabs(in.thumbstickX) > 0.15f && dt > 0.0f) {
                    cur().volume = std::clamp(cur().volume + in.thumbstickX * dt * 0.6f, 0.0f, 1.0f);
                    if (cur().volume > 0.0f) cur().muted = false;
                    // The stick is taken: no screen push, scrubbing or seeking meanwhile.
                    in.thumbstickY = 0.0f;
                    in.thumbstickX = in.thumbstickLX = in.thumbstickRX = 0.0f;
                    interacted = true;
                }
                // Viewpoint: flat screens move nearer/farther (thumbstick Y);
                // immersive 360/180 uses thumbstick Y to zoom by default, and
                // holding Bumper (Previous/Next) + thumbstick X/Y pans yaw and tilts pitch.
                if (!sphereMode) {
                    if (in.thumbstickY > 0.15f || in.thumbstickY < -0.15f) {
                        if (groupedScreen(selectedScreen_)) {
                            // Snapped layout: the whole wall moves nearer / farther.
                            arrangement_.dist = std::clamp(arrangement_.dist + in.thumbstickY * 0.04f, 1.0f, 10.0f);
                            applyArrangement(arrangement_);
                        } else if (selectedScreen_ > 0 || renderer_.screenPlacement(selectedScreen_).active) {
                            // Free-placed screen: push it along the line from the head.
                            VulkanRenderer::ScreenPlacement pl = renderer_.screenPlacement(selectedScreen_);
                            float d[3] = {pl.center[0] - in.headPos[0], pl.center[1] - in.headPos[1],
                                          pl.center[2] - in.headPos[2]};
                            const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                            if (len > 1e-3f) {
                                const float nl = std::clamp(len + in.thumbstickY * 0.04f, 1.0f, 10.0f);
                                for (int k = 0; k < 3; ++k) pl.center[k] = in.headPos[k] + d[k] / len * nl;
                                renderer_.setScreenPlacement(selectedScreen_, pl);
                            }
                        } else {
                            renderer_.adjustScreenDistance(in.thumbstickY * 0.04f);
                        }
                        if (hudShown) interacted = true;
                    }
                    // When pointing at the Screen Curve pill on the HUD, thumbstick X smoothly adjusts curvature!
                    if (hudShown &&
                        renderer_.pointerButton() == static_cast<int>(VulkanRenderer::CtrlButton::ScreenToggle) &&
                        std::fabs(in.thumbstickX) > 0.15f && dt > 0.0f) {
                        renderer_.adjustScreenCurvature(in.thumbstickX * dt * 0.65f);
                        in.thumbstickX = in.thumbstickLX = in.thumbstickRX = 0.0f;  // no scrubbing too
                        interacted = true;
                    }
                } else if (dt > 0.0f) {
                    // 360/180: unshifted Stick Y zooms (consistent with Flat screen distance);
                    // holding Bumper + Stick X/Y pans yaw and tilts pitch. Silent when HUD is hidden.
                    if (bumperHeldNow) {
                        if (std::fabs(in.thumbstickY) > 0.15f) renderer_.adjustSpherePitch(in.thumbstickY * dt * 0.6f);
                        if (std::fabs(in.thumbstickX) > 0.15f) renderer_.adjustSphereYaw(in.thumbstickX * dt * 0.6f);
                    } else if (!bumperShift && !scrubbing_ && std::fabs(in.thumbstickY) > 0.15f &&
                               std::fabs(in.thumbstickY) > std::fabs(in.thumbstickX)) {
                        // Zoom only on a mostly vertical push: a scrub (left / right) never zooms too.
                        renderer_.adjustSphereZoom(in.thumbstickY * dt * 0.6f);
                    }
                    if (hudShown && (std::fabs(in.thumbstickY) > 0.15f || (bumperShift && std::fabs(in.thumbstickX) > 0.15f))) {
                        interacted = true;
                    }
                }
#ifdef PIXELVR_HAVE_MEDIA
                if (in.togglePlay && cur().open) {
                    curTogglePause();
                    interacted = true;
                }
                // D-pad: Up / Down = volume of the selected screen, Left / Right =
                // seek 10 s like the dock buttons; held, it repeats. Web pages are
                // left alone for now.
                {
                    using namespace std::chrono_literals;
                    const bool held[5] = {false, in.dpadUpHeld, in.dpadDownHeld, in.dpadLeftHeld, in.dpadRightHeld};
                    int dir = in.dpadUp ? 1 : in.dpadDown ? 2 : in.dpadLeft ? 3 : in.dpadRight ? 4 : 0;
                    if (cur().web || laserOnWeb) {
                        dir = 0;
                        dpadRepeatDir_ = 0;
                    } else if (dir != 0) {
                        dpadRepeatDir_ = dir;
                        dpadNextRepeat_ = nowT + 350ms;
                    } else if (dpadRepeatDir_ != 0 && held[dpadRepeatDir_]) {
                        if (nowT >= dpadNextRepeat_) {
                            dpadNextRepeat_ = nowT + 200ms;
                            dir = dpadRepeatDir_;
                        }
                    } else {
                        dpadRepeatDir_ = 0;
                    }
                    if ((dir == 1 || dir == 2) && cur().open) {
                        cur().volume = std::clamp(cur().volume + (dir == 1 ? 0.05f : -0.05f), 0.0f, 1.0f);
                        if (cur().volume > 0.0f) cur().muted = false;
                        hudDismissed_ = false;  // the dock shows the level
                        interacted = true;
                    } else if ((dir == 3 || dir == 4) && cur().open) {
                        curSeekRelative(dir == 3 ? -10.0 : 10.0);
                        hudDismissed_ = false;  // the dock shows the time
                        interacted = true;
                    }
                }
                // Previous / Next: the music view's track buttons (video: nothing yet).
                if ((in.previous || in.next) && cur().open && !cur().web && cur().engine.audioOnly()) {
                    if (in.previous) musicPrevious(selectedScreen_);
                    else musicNext(selectedScreen_);
                    interacted = true;
                }
                // Actions with no default binding (SteamVR chords / long presses):
                // each runs what its dock button or menu entry does.
                if (in.mute) {
                    cur().muted = !cur().muted;
                    interacted = true;
                }
                if (in.stop && (cur().open || cur().opening)) {
                    if (!returnFromPlex(selectedScreen_)) closeMedia();
#ifdef PIXELVR_HAVE_FREETYPE
                    if (openScreenCount() == 0) browser_.open();
#endif
                    interacted = true;
                }
                if (in.nextScreen || in.previousScreen) {
                    selectNextScreen(in.nextScreen ? 1 : -1);
                    interacted = true;
                }
                if ((in.skipBack || in.skipForward) && cur().open) {
                    curSeekRelative(in.skipForward ? 30.0 : -30.0);
                    if (hudShown) interacted = true;
                }
                if (in.lockScreens && multiScreenEnabled() && cur().open) {
                    screensUnlocked_ = !screensUnlocked_;
                    interacted = true;
                }
                if (in.audioTrack && !cur().web) {
                    cycleAudioTrack();
                    interacted = true;
                }
                if (in.format3d && cur().open && !cur().web) {
                    openFormatMenu();
                    interacted = true;
                }
                // After a scrub commits on a web screen, the bar keeps showing the
                // target until the page reports a position near it (its time trails
                // the seek) or a moment has passed, so it never flicks back first.
                // Local files need no hold: the engine reports the target itself.
                const auto shownPosition = [&]() {
                    const double p = curPosition();
                    if (scrubHoldPos_ < 0.0 || !cur().web) return p;
                    if (scrubHoldScreen_ != selectedScreen_ || nowT >= scrubHoldUntil_ ||
                        std::fabs(p - scrubHoldPos_) < 0.5) {
                        scrubHoldPos_ = -1.0;  // caught up (or gave up): follow the player
                        return p;
                    }
                    return scrubHoldPos_;
                };
                if (cur().open) {
                    // Unified stick mapping across Flat and 360/180 (when Bumper is not held):
                    // Left stick X steps -/+30s; Right stick X scrubs continuously.
                    constexpr double kStep = 30.0;
                    const bool stepPush = !sphereMode || std::fabs(in.thumbstickLX) >= std::fabs(in.thumbstickLY);
                    if (!stepPush) {
                        // a zoom push: no step
                    } else if (!bumperShift && in.thumbstickLX > 0.6f && seekStepArmed_) {
                        curSeekRelative(kStep);
                        seekStepArmed_ = false;
                        if (hudShown) interacted = true;
                    } else if (!bumperShift && in.thumbstickLX < -0.6f && seekStepArmed_) {
                        curSeekRelative(-kStep);
                        seekStepArmed_ = false;
                        if (hudShown) interacted = true;
                    } else if (std::fabs(in.thumbstickLX) < 0.3f) {
                        seekStepArmed_ = true;
                    }

                    // Scrub: accumulate a target while dragging and commit it with a
                    // single seek on release. Seeking continuously floods the
                    // hardware decoder (a reopen each) and stalls it; one seek per
                    // gesture stays responsive.
                    bool scrubInput = false;
                    const bool timelineDrag = !wasHudDismissed && renderer_.pointerOnTimeline() && in.selectHeld;
                    if (timelineDrag && curDuration() > 0.0) {
                        scrubTarget_ = curTimelineStart() + renderer_.pointerScrub() * (curDuration() - curTimelineStart());
                        scrubInput = true;
                    } else if (!bumperShift && std::fabs(in.thumbstickRX) > 0.15f && dt > 0.0f &&
                               curDuration() > 0.0 &&
                               (!sphereMode || scrubbing_ || std::fabs(in.thumbstickRX) >= std::fabs(in.thumbstickRY))) {
                        if (!scrubbing_) scrubTarget_ = shownPosition();
                        const float ax = in.thumbstickRX;
                        const float t = (std::fabs(ax) - 0.15f) / 0.85f;
                        const double rate =
                            std::copysign(static_cast<double>(t * t) * 60.0, ax);
                        scrubTarget_ = std::clamp(scrubTarget_ + rate * dt, curTimelineStart(), curDuration());
                        scrubInput = true;
                    }
                    // Plugins (device outputs) pause while the user scrubs: stick or timeline drag.
                    pluginScrubbing_ = scrubInput || timelineDrag;
                    if (scrubInput) {
                        scrubbing_ = true;
                        if (hudShown || timelineDrag) interacted = true;
                    } else if (scrubbing_) {
                        curSeek(scrubTarget_);  // commit the scrub with one seek
                        scrubbing_ = false;
                        if (cur().web && !webSeekBlocked()) {  // only when the seek was sent
                            scrubHoldPos_ = scrubTarget_;
                            scrubHoldScreen_ = selectedScreen_;
                            scrubHoldUntil_ = nowT + std::chrono::milliseconds(1500);
                        }
                    }
                }
                if (interacted) {
                    lastControllerActivity_ = nowT;
                }
                const bool paused = (cur().open && curPaused()) || cur().opening;
                const bool controlsUp =
                    (cur().open || cur().opening) && !hudDismissed_ &&
                    (paused || (controllersEngaged && (in.controllers[0].active ||
                                                       in.controllers[1].active)));
                float progress = 0.0f;
                double elapsed = 0.0, total = 0.0;
                if (cur().open && curDuration() > 0.0) {
                    // Livestreams: the rewind window, live edge on the right.
                    const double start = curTimelineStart();
                    total = std::max(curDuration() - start, 0.001);
                    elapsed = scrubbing_ ? scrubTarget_ : shownPosition();
                    progress = static_cast<float>(std::clamp((elapsed - start) / total, 0.0, 1.0));
                }
                renderer_.setTransportLive(cur().open ? curLive() : 0);
                {
                    // Chapters of the selected screen (handed over when they change).
                    const ChapterList* chapters = nullptr;
                    std::string key;
                    if (cur().open && cur().web) {
                        chapters = &webHosts_[selectedScreen_].chapters();
                        key = "w" + std::to_string(selectedScreen_) + ":" +
                              std::to_string(webHosts_[selectedScreen_].chaptersVersion());
                    }
#ifdef PIXELVR_HAVE_MEDIA
                    else if (cur().open) {
                        chapters = &cur().engine.chapters();
                        key = "f" + std::to_string(selectedScreen_) + ":" + cur().path;
                    }
#endif
                    if (key != chapterKey_) {
                        chapterKey_ = key;
                        renderer_.setTransportChapters(chapters ? *chapters : ChapterList{});
                    }
                }
                std::string fLabel = "2D Flat";
                if (settings_.projectionMode() == 0) {
                    fLabel = (settings_.stereoMode() == 0) ? "2D Flat" :
                             (settings_.stereoMode() == 1) ? "3D SBS" :
                             (settings_.stereoMode() == 3) ? "3D FS" : "3D TB";
                } else if (settings_.projectionMode() == 2) {  // the menu's short names
                    fLabel = (settings_.stereoMode() == 1) ? "VR180 3D" :
                             (settings_.stereoMode() == 2) ? "VR180 TB" : "VR180";
                } else {
                    fLabel = (settings_.stereoMode() == 2) ? "VR360 3D" :
                             (settings_.stereoMode() == 1) ? "VR360 SBS" : "VR360";
                }

                std::string aLabel = "Audio";
                bool aEnabled = false;
                if (cur().open && !cur().web && !cur().openAbandoned) {  // only an engine that is this screen's
                    const auto aStreams = cur().engine.audioStreams();
                    const int curA = cur().engine.currentAudioStream();
                    for (const auto& as : aStreams) {
                        if (as.index == curA) {
                            std::string l = as.language;
                            for (char& c : l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                            if (l.empty() || l == "und" || l == "undef" || l == "undefined") {
                                aLabel = (as.label.empty() || as.label == "und" || as.label == "undef") ? "Default" : as.label;
                            } else {
                                // Language codes read best in capitals ("ENG").
                                aLabel = as.language;
                                for (char& c : aLabel) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                            }
                            if (aLabel.size() > 8) aLabel = aLabel.substr(0, 8);
                            aLabel = "Audio: " + aLabel;
                            break;
                        }
                    }
                    // Only enable audio cycling if there is more than 1 audio stream
                    aEnabled = (aStreams.size() > 1) || cur().engine.videoStreams().size() > 1;
                }

                std::string sLabel = "Reset Horizon";
                if (settings_.projectionMode() == 0) {
                    const VulkanRenderer::ScreenPlacement& selPl = renderer_.screenPlacement(selectedScreen_);
                    const float curv = (selectedScreen_ > 0 || selPl.active) ? selPl.curvature
                                                                            : renderer_.screenCurvature();
                    const int cPct = static_cast<int>(curv * 100.0f + 0.5f);
                    if (cPct <= 2) {
                        sLabel = "Screen: Flat";
                    } else {
                        char cbuf[32];
                        std::snprintf(cbuf, sizeof(cbuf), "Curved %d%%", cPct);
                        sLabel = cbuf;
                    }
                }

                if (cur().web) {  // browser screens: F11 and the keyboard instead
                    fLabel = "Fullscreen";
                    aLabel = "Keyboard";
                    aEnabled = true;
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
            // Timeline hover preview: follow the current clip/stereo layout, ask
            // for the hovered time and upload whatever the worker has finished.
            // Plex / Jellyfin streams (webSuspended, http) get it too: without it scrubbing showed
            // nothing until release (user report). The worker opens its own connection.
            scrubPreview_.setSource(cur().open && !cur().web && (!cur().webSuspended || isHttpUrl(cur().path)) &&
                                            !isRemotePath(cur().path)
                                        ? cur().path
                                        : std::string(),
                                    settings_.stereoMode() == 3 ? 0 : settings_.stereoMode());  // FS3D: one full view
            if (cur().open && renderer_.pointerOnTimeline() && cur().engine.duration() > 0.0) {
                scrubPreview_.request(renderer_.pointerScrub() * cur().engine.duration());
            }
            {
                std::vector<uint8_t> pv;
                double pvSec = 0.0;
                if (scrubPreview_.fetch(pv, pvSec)) {
                    renderer_.updateScrubPreview(pv.data(), ScrubPreview::kWidth,
                                                 ScrubPreview::kHeight, pvSec);
                }
            }
            // Keep the resume position fresh so a crash or power-off loses little.
            if (openScreenCount() > 0 && nowT - lastPlaybackRemember_ > std::chrono::seconds(10)) {
                lastPlaybackRemember_ = nowT;
                for (int slot = 0; slot < VulkanRenderer::kMaxVideoSlots; ++slot) {
                    rememberPlaybackPosition(slot);
                }
                playbackMemory_.save();
            }
            // Always-on periodic perf log so decode-bound vs render-bound stalls
            // show up in the run log even without the on-screen overlay.
            if (cur().open) {
                ++perfFrames_;
                if (perfLastLog_.time_since_epoch().count() == 0) {
                    perfLastLog_ = now;
                    perfLastDecoded_ = cur().engine.decodedFrames();
                    renderer_.takeGpuTimes();  // start the GPU averages with the window
                } else {
                    const double since =
                        std::chrono::duration<double>(now - perfLastLog_).count();
                    if (since >= 2.0) {
                        const uint64_t dec = cur().engine.decodedFrames();
                        const double decFps =
                            (dec >= perfLastDecoded_)
                                ? static_cast<double>(dec - perfLastDecoded_) / since
                                : 0.0;
                        // Where a frame's time goes (per rendered frame): feeding video
                        // frames (import + resolve), the whole XR frame (incl. the
                        // runtime's wait), waiting for the GPU, and drawing on the CPU.
                        double gpuWaitMs = 0.0, unusedMs = 0.0;
                        renderer_.takePerfTimes(gpuWaitMs, unusedMs);
                        const double n = std::max(1, perfFrames_);
                        long gpuMhz = 0;
                        {
                            std::ifstream cf("/sys/class/devfreq/3d00000.gpu/cur_freq");
                            long hz = 0;
                            if (cf >> hz) gpuMhz = hz / 1000000;
                        }
                        const uint64_t late = cur().engine.lateFrames();
                        const uint64_t waits = cur().engine.clockWaits();
                        // GPU time per frame from timestamp queries (averaged over
                        // this window) and the eye pass's MSAA / visibility-mask state.
                        const VulkanRenderer::GpuTimes gt = renderer_.takeGpuTimes();
                        char gpuBuf[200];
                        if (gt.frames > 0) {
                            std::snprintf(gpuBuf, sizeof(gpuBuf),
                                          "gpu eye %.2f ms, models %.2f ms, ui %.2f ms, video %.2f ms, total %.2f ms",
                                          gt.eye, gt.models, gt.ui, gt.video, gt.total);
                        } else {
                            std::snprintf(gpuBuf, sizeof(gpuBuf), "gpu n/a");
                        }
                        char msaaBuf[24];
                        if (renderer_.eyeMsaaSamples() <= 1 || gt.msaaPasses == 0) {
                            std::snprintf(msaaBuf, sizeof(msaaBuf), "off");
                        } else if (gt.msaaPasses == gt.eyePasses) {
                            std::snprintf(msaaBuf, sizeof(msaaBuf), "%dx", renderer_.eyeMsaaSamples());
                        } else {
                            std::snprintf(msaaBuf, sizeof(msaaBuf), "mixed");
                        }
                        const char* maskStr = gt.maskPasses == 0                ? "off"
                                              : gt.maskPasses == gt.eyePasses ? "on"
                                                                              : "partial";
                        PIXELVR_LOG_INFO(
                            "PERF: render %.1f fps, decode %.1f/%.2f fps | per frame: feed %.1f ms, frame %.1f ms, "
                            "gpu wait %.1f ms, draw %.1f ms | gpu %ld MHz | late %llu, clock waits %llu | eye %dx%d "
                            "| %s | msaa %s, mask %s",
                            perfFrames_ / since, decFps, cur().engine.frameRate(), perfFeedMs_ / n,
                            perfFrameMs_ / n, gpuWaitMs / n, perfViewCpuMs_ / n, gpuMhz,
                            static_cast<unsigned long long>(late - perfLastLate_),
                            static_cast<unsigned long long>(waits - perfLastWaits_),
                            xr_.swapchains().empty() ? 0 : xr_.swapchains()[0].renderWidth,
                            xr_.swapchains().empty() ? 0 : xr_.swapchains()[0].renderHeight, gpuBuf, msaaBuf,
                            maskStr);
                        perfLastLate_ = late;
                        perfLastWaits_ = waits;
                        perfFeedMs_ = perfFrameMs_ = perfViewCpuMs_ = 0.0;
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

    for (int slot = 0; slot < VulkanRenderer::kMaxVideoSlots; ++slot) {
        rememberPlaybackPosition(slot);
    }
    playbackMemory_.save();
    if (renderer_.device() != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(renderer_.device());
    }
    watchdogRunning_.store(false);
    if (watchdogThread_.joinable()) {
        watchdogThread_.join();
    }
    VrKeyboard::instance().shutdown();
    renderModels_.shutdown();  // destroy XR model handles before the session
    PIXELVR_LOG_INFO("Shutting down");
    return 0;
}

} // namespace pixelvr

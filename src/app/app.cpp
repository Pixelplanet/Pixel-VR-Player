#include "app/app.hpp"
#include "app/vault_manager.hpp"
#include "net/smb_credentials.hpp"
#include "xr/vr_keyboard.hpp"
#include "util/ipv4.hpp"

#include "util/logging.hpp"
#include "util/media_files.hpp"

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
    watchdogRunning_.store(false);
    if (watchdogThread_.joinable()) {
        watchdogThread_.join();
    }
}

void App::watchdogLoop() {
    using namespace std::chrono_literals;
    auto lastPresentTime = std::chrono::steady_clock::now();
    bool prevAbsent = false;

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

        if (!isAbsent) {
            lastPresentTime = now;
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
                if (VaultManager::instance().isUnlocked()) {
                    pendingAutoLock_.store(true, std::memory_order_release);
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
void App::loadMedia(const std::string& path, int slot, bool fromQueue) {
    lastLoadFailed_ = false;
    if (slot < 0 || !multiScreenEnabled()) slot = multiScreenEnabled() ? selectedScreen_ : 0;
    {
        // 360/180 is immersive: it always plays on the main screen.
        VulkanRenderer::ProjectionMode p = VulkanRenderer::ProjectionMode::Flat;
        VulkanRenderer::StereoMode st = VulkanRenderer::StereoMode::Mono;
        bool fromName = false;
        detect_format(path, p, st, fromName);
        if (slot > 0 && p != VulkanRenderer::ProjectionMode::Flat) slot = 0;
        // Immersive plays alone, and its decoder must open with the hardware
        // to itself: every other screen (a 1080p clip, a browser page with a
        // playing video) still holds a decoder session, and an 8K session on
        // top of one is refused ("current session not supported") so the clip
        // would silently fall back to software decoding. Close them first.
        if (p != VulkanRenderer::ProjectionMode::Flat) {
            for (int i = 1; i < VulkanRenderer::kMaxVideoSlots; ++i) {
                if (screens_[i].open) closeMedia(i);
            }
        }
    }
    if (screens_[slot].web) clearSlotKeepLayout(slot);  // a file replaces a browser screen
    ScreenState& sc = screens_[slot];
    const bool sameFile = (path == sc.path);
    if (!sameFile) {
        rememberPlaybackPosition(slot);  // the clip being replaced
        playbackMemory_.save();
    }
    if (!sameFile) renderer_.clearScrubPreview();  // stale frame from the previous clip
    sc.path = path;
    const bool privatePath = VaultManager::instance().isVaultPath(path);
    if (!sameFile) sc.fingerprint = privatePath ? std::string() : PlaybackMemory::fingerprintOf(path);
    const PlaybackMemory::Entry* remembered = privatePath ? nullptr : playbackMemory_.find(path, sc.fingerprint);
    VulkanRenderer::ProjectionMode proj = VulkanRenderer::ProjectionMode::Flat;
    VulkanRenderer::StereoMode stereo = VulkanRenderer::StereoMode::Mono;
    bool stereoFromName = false;
    detect_format(path, proj, stereo, stereoFromName);

    renderer_.releaseVideoFrame(slot);  // free the previous clip's dma-buf before reopening
    if (slot == 0) {
        renderer_.setSphereZoom(1.0f);  // reset the 360/180 viewpoint per clip
        renderer_.setSpherePitch(0.0f);
    }
    sc.engine.setPreferredAudioLanguage(settings_.audioLanguage());
    if (!sameFile) {  // a new file starts from automatic track selection
        sc.engine.setPreferredVideoStream(-1);
        sc.engine.setPreferredAudioStream(-1);
    }
    sc.open = sc.engine.open(path);
    if (!sc.open) {
        // Clear stale video from the previous clip so the renderer shows black
        // instead of the last decoded frame of the old file.
        const std::string why = sc.engine.lastError();
        lastLoadFailed_ = true;
        if (fromQueue) {  // the queue moves on to the next track (playQueueTrack)
            PIXELVR_LOG_WARN("Failed to open %s: %s", path.c_str(), why.c_str());
            return;
        }
        closeMedia(slot);
        PIXELVR_LOG_WARN("Failed to open %s: %s", path.c_str(), why.c_str());
#ifdef PIXELVR_HAVE_FREETYPE
        // Say so in the file browser instead of silently doing nothing.
        browser_.setStatus("Can't play " + std::filesystem::path(path).filename().string() + ": " + why);
        if (!fromQueue) browser_.setOpen(true);
#endif
        return;
    }
    sc.engine.setPaused(false);
    if (!fromQueue) {  // a track change of the queue leaves the UI alone
        hudDismissed_ = false;
        lastControllerActivity_ = std::chrono::steady_clock::now();
        if (textEntry_.active) cancelKeyboard();
        actionMenu_.visible = false;
        smbModal_.active = false;
        smbModal_.editingField = SmbModalState::Field::None;
        browser_.setOpen(false);
        downloadsWindow_ = false;
        pendingDl_.clear();
    }

    // Refine the stereo layout from the frame aspect ratio when the filename did
    // not state it: once the projection is a VR type the aspect is unambiguous
    // (360 mono 2:1, SBS 4:1, OU 1:1; 180 mono 1:1, SBS 2:1, OU 1:2).
    const int w = sc.engine.width();
    const int h = sc.engine.height();

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

    // No layout in the name: the file's own 3D / VR metadata, else what the
    // picture check found (now, or once the thumbnail worker has looked).
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
            } else {
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
        stereo = static_cast<VulkanRenderer::StereoMode>(remembered->stereo);
        sc.formatByHand = true;
        sc.formatPending = false;
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
    sc.projection = static_cast<int>(proj);
    sc.stereo = static_cast<int>(stereo);
    if (slot == 0) {
        renderer_.setProjectionMode(proj);
        renderer_.setStereoMode(stereo);
        if (proj != VulkanRenderer::ProjectionMode::Flat) {
            // Immersive plays alone: the other screens close (usually asked
            // first, see playFromBrowser; this catches a format found only
            // now, from the picture size).
            for (int i = 1; i < VulkanRenderer::kMaxVideoSlots; ++i) {
                if (screens_[i].open) closeMedia(i);
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
    // Resume where this file was left (track switches reopen the same file and
    // restore their own position).
    if (!sameFile && !fromQueue && remembered != nullptr && remembered->position > 0.0 &&
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
    if (!fromQueue) selectScreen(slot);
    static const char* const kProj[] = {"flat", "360", "180"};
    static const char* const kStereo[] = {"mono", "SBS", "TB"};
    PIXELVR_LOG_INFO("Loaded %s on screen %d (%dx%d, %s/%s)", path.c_str(), slot + 1, w, h,
                     kProj[static_cast<int>(proj)],
                     kStereo[static_cast<int>(stereo)]);
    // Software decoding forced by someone else's decoder session: say so,
    // since at 4K+ that means stutter the viewer would otherwise blame on us.
    const std::string blockedBy = sc.engine.hardwareBlockedBy();
    if (!blockedBy.empty() && static_cast<long long>(w) * h >= 3840LL * 2000LL) {
        notice_ = "Software decoding: the hardware decoder is held by " + blockedBy +
                  " - close that video (Steam overlay / store) or restart Steam";
        noticeUntil_ = std::chrono::steady_clock::now() + std::chrono::seconds(12);
        PIXELVR_LOG_WARN("%s", notice_.c_str());
    }
}

void App::switchTracks(bool byLanguage) {
    if (cur().path.empty() || !cur().open) return;
    const double pos = cur().engine.position();
    if (byLanguage) {
        cur().engine.setPreferredAudioLanguage(settings_.audioLanguage());
        cur().engine.setPreferredAudioStream(-1);
    } else {
        cur().engine.setPreferredAudioLanguage("");
        cur().engine.setPreferredAudioStream(settings_.selectedAudioStream());
    }
    cur().engine.setPreferredVideoStream(settings_.selectedVideoStream());
    loadMedia(cur().path);  // same file: keeps the preferences set above
    if (pos > 0.5 && cur().engine.duration() > 0.0) cur().engine.seek(pos);
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
        std::snprintf(buf, sizeof(buf), "Render %dx%d",
                      xr_.swapchains()[0].width, xr_.swapchains()[0].height);
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
    // Tracks that can't be opened are skipped; give up once every track failed.
    for (int tries = 0; tries < q.size() && index >= 0; ++tries) {
        const std::string next = q.track(index).path;
        if (next.empty()) break;
        q.setCurrent(index);
        loadMedia(next, slot, true);
        if (!lastLoadFailed_) return;
        index = q.next(false);
    }
    closeMedia(slot);  // nothing in the queue plays
#else
    (void)slot;
    (void)index;
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
    if (!sc.open || sc.web || sc.path.empty() || VaultManager::instance().isVaultPath(sc.path)) {
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
        sc.engine.close();
        // Hand the last frame back to the hardware decoder, so it can free
        // the closed clip's buffers (else the next big video finds no room).
        renderer_.releaseVideoFrame(slot);
#endif
    }
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
            if (screens_[i].open) {
                next = i;
                break;
            }
        }
        selectScreen(next);
    }
    leaveArrangement(slot);
}

void App::performVaultLock() {
    VaultManager::instance().lock();
    renderer_.setVaultUnlocked(false);
    actionMenu_.visible = false;
    renderer_.setActionMenu({});

#ifdef PIXELVR_HAVE_MEDIA
    for (int slot = 0; slot < VulkanRenderer::kMaxVideoSlots; ++slot) {
        if (screens_[slot].open && VaultManager::instance().isVaultPath(screens_[slot].path)) {
            closeMedia(slot);
        }
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

    if (settings_.section() == 4) {  // Vault & Privacy is hidden once locked
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
    actionMenu_.subtitle.clear();  // no stale error from a previous edit
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
    // ids are fixed: 0 host, 1 share, 2 auth ("guest"/"account"), 3 user,
    // 4 password, 5 workgroup, 6 nickname, 7 save, 8 cancel, 9 delete.
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
    actionMenu_.options.push_back(fieldVal(SmbModalState::Field::Workgroup, 5, smbModal_.workgroup, false));
    actionMenu_.options.push_back(fieldVal(SmbModalState::Field::Label, 6, smbModal_.label, false));
    actionMenu_.options.push_back("Save");
    actionMenu_.options.push_back("Cancel");
    if (!smbModal_.isNew) {
        actionMenu_.options.push_back("Delete");
    }
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
    if (textEntry_.active) cancelKeyboard();
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

void App::curSeek(double t) {
    if (cur().web) {
        webHosts_[selectedScreen_].mediaSeek(t);
        return;
    }
#ifdef PIXELVR_HAVE_MEDIA
    cur().engine.seek(t);
#endif
}

void App::curSeekRelative(double d) {
    if (cur().web) {
        webHosts_[selectedScreen_].mediaSeekRelative(d);
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
    for (const ScreenState& sc : screens_) n += sc.open ? 1 : 0;
    return n;
}

int App::freeScreenSlot() const {
    for (int slot = 1; slot < VulkanRenderer::kMaxVideoSlots; ++slot) {
        if (!screens_[slot].open) return slot;
    }
    return -1;
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
    if (sc.open) {
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
    sc.projection = projection;
    sc.stereo = stereo;
    settings_.setProjectionMode(projection);
    settings_.setStereoMode(stereo);
    if (selectedScreen_ == 0) {
        renderer_.setProjectionMode(static_cast<VulkanRenderer::ProjectionMode>(projection));
        renderer_.setStereoMode(static_cast<VulkanRenderer::StereoMode>(stereo));
#ifdef PIXELVR_HAVE_MEDIA
        if (projection != 0) {  // immersive plays alone
            for (int i = 1; i < VulkanRenderer::kMaxVideoSlots; ++i) {
                if (screens_[i].open) screens_[i].engine.setPaused(true);
            }
        }
#endif
    } else {
        VulkanRenderer::ScreenPlacement pl = renderer_.screenPlacement(selectedScreen_);
        pl.stereo = static_cast<VulkanRenderer::StereoMode>(stereo);
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
    pl.stereo = static_cast<VulkanRenderer::StereoMode>(screens_[slot].stereo);
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
        actionMenu_.options.push_back(screens_[slot].open ? "Replace" : "Add");
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
        if (!screens_[slot].open) continue;
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
            pl.stereo = static_cast<VulkanRenderer::StereoMode>(screens_[slot].stereo);
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
            joined.order.push_back(freeSlot);
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
    if (!VaultManager::instance().isVaultPath(path)) {
        const PlaybackMemory::Entry* rem = playbackMemory_.find(path, PlaybackMemory::fingerprintOf(path));
        if (rem != nullptr && rem->projection >= 0) return rem->projection != 0;
    }
    VulkanRenderer::ProjectionMode p = VulkanRenderer::ProjectionMode::Flat;
    VulkanRenderer::StereoMode st = VulkanRenderer::StereoMode::Mono;
    bool fromName = false;
    detect_format(path, p, st, fromName);
    if (p != VulkanRenderer::ProjectionMode::Flat) return true;
#ifdef PIXELVR_HAVE_FREETYPE
    ThumbnailCache::MediaMeta tm;
    if (thumbnailCache_.getMeta(path, tm)) {
        if (tm.projectionHint > 0) return true;
        if (tm.height > 0 && is_vr180_media(path, tm.width, tm.height)) return true;
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
    pl.stereo = static_cast<VulkanRenderer::StereoMode>(screens_[slot].stereo);
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

float App::screenEyeAspect(int slot) const {
    float aspect = 16.0f / 9.0f;
#ifdef PIXELVR_HAVE_MEDIA
    const ScreenState& sc = screens_[slot];
    if (sc.open && sc.engine.width() > 0 && sc.engine.height() > 0) {
        aspect = static_cast<float>(sc.engine.width()) / static_cast<float>(sc.engine.height());
        if (sc.stereo == 1 && aspect >= 2.6f) aspect *= 0.5f;       // full SBS
        else if (sc.stereo == 2 && aspect <= 1.2f) aspect *= 2.0f;  // full TB
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
    for (int i = 0; i < n; ++i) {
        const int slot = a.order[static_cast<std::size_t>(i)];
        curv[i] = a.curvature;
        ext[i] = screenExtent(screenEyeAspect(slot), scale[i] * a.scale, curv[i]);
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
        pl.stereo = static_cast<VulkanRenderer::StereoMode>(screens_[slot].stereo);
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
    if (!inArrangement(slot)) arrangement_.order.push_back(slot);
    applyArrangement(arrangement_);
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

void App::openWebScreen(const std::string& url, bool kiosk) {
    settings_.close();
    downloadsWindow_ = false;
    pendingWebUrl_ = url;
    pendingWebKiosk_ = kiosk;
    if (multiScreenEnabled() && openScreenCount() > 0) {
        // The picker lives in the Files hub (it is dismissed without it), like
        // picking a screen for a file; choosing a screen closes the hub.
#ifdef PIXELVR_HAVE_FREETYPE
        browser_.setOpen(true);
#endif
        openPlayTargetMenu(kWebPath);
        actionMenu_.subtitle = kiosk ? "YouTube" : "Web browser";
        return;
    }
#ifdef PIXELVR_HAVE_FREETYPE
    browser_.setOpen(false);
#endif
    loadWeb(0, url, kiosk);
}

void App::loadWeb(int slot, const std::string& url, bool kiosk) {
    if (!multiScreenEnabled()) slot = 0;
    if (screens_[slot].web && webHosts_[slot].kiosk() == kiosk) {  // same kind: keep its pages
        webHosts_[slot].openTab(url);
        selectScreen(slot);
        return;
    }
    if (screens_[slot].open) clearSlotKeepLayout(slot);
    BrowserHost::Options opt;
    opt.kiosk = kiosk;
    opt.incognito = settings_.privateBrowsing();
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
        arrangePresets_.push_back(ArrangePreset::SideBySide);
        if (n == 3) arrangePresets_.push_back(ArrangePreset::TwoBelowOneAbove);
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

            // In-app keyboard (text entry). It owns all controller input while open:
            // trigger / A press the key under the laser, stick left/right moves
            // the cursor, B / menu cancels.
            const bool kbOpen = textEntry_.active;
            if (kbOpen) {
                if (in.buttonDown[settings_.bindMenu()] || in.buttonDown[XrInputState::BtnB]) {
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
                    if (pressed && key >= 0) {
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
                in.togglePlay = false;
                in.browserToggle = false;
                in.buttonDown[settings_.bindMenu()] = false;
                in.buttonDown[XrInputState::BtnB] = false;
                if (smbModal_.active && smbModal_.editingField != SmbModalState::Field::None) {
                    updateSmbModalMenu();
                }
            }
            renderer_.setKeyboardModal(textEntry_.active, textEntry_.prompt, textEntry_.text,
                                       textEntry_.password, nullptr,
                                       static_cast<int>(textEntry_.cursor));
            renderer_.setKeyboardModes(textEntry_.shift, textEntry_.symbols);

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
                    // Releasing the trigger drops the screen (B cancels the grab too).
                    if (!in.selectHeld || in.buttonDown[XrInputState::BtnB] ||
                        in.buttonDown[settings_.bindMenu()]) {
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
                in.togglePlay = false;
                in.browserToggle = false;
                in.recenter = false;
                in.thumbstickX = 0.0f;
                in.thumbstickY = 0.0f;
                in.buttonDown[XrInputState::BtnB] = false;
                in.buttonDown[settings_.bindMenu()] = false;
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
                if (!screens_[slot].web) continue;
                BrowserHost& host = webHosts_[slot];
                host.poll();
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
                    renderer_.updateWebFrame(slot, rgba, host.frameWidth(), host.frameHeight());
                }
                // Text fields: a click into one opens the keyboard, which then
                // types straight into the page; it closes when the field does.
                BrowserHost::FocusEvent fe;
                while (host.takeFocusEvent(fe)) {
                    if (fe.focused && kbWebSlot_ == slot) {
                        textEntry_.text = fe.value;
                        textEntry_.cursor = utf16ToByteOffset(fe.value, fe.cursor);
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
            if (!hubShown && !actionMenu_.visible && !textEntry_.active && !screensUnlocked_ &&
                movingScreen_ < 0) {
                constexpr float kClickSlop = 16.0f;  // page pixels
                int ws = -1;
                float u = 0.0f, v = 0.0f;
                const bool onWeb = renderer_.webPointer(ws, u, v) && screens_[ws].web;
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
                        in.thumbstickY = 0.0f;
                    }
                    if (in.buttonDown[XrInputState::BtnB]) {
                        host->back();
                        in.buttonDown[XrInputState::BtnB] = false;
                    }
                } else if (!onWeb) {
                    webHoverValid_ = false;
                }
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
                            // Like any player: back to the start, unless just started.
                            const int prev = sc.queue.previous();
                            if (sc.engine.position() > 3.0 || prev < 0 || prev == sc.queue.current()) sc.engine.seek(0.0);
                            else playQueueTrack(ms, prev);
                        } else if (mh == VR::kMusicNext) {
                            const int next = sc.queue.next(false);
                            if (next >= 0) playQueueTrack(ms, next);
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

            // Suppress laser pointer hovering, clicking, and dim the laser beam during combo entry
            renderer_.setPointerSuppressed(comboGate);

            // Whirligig-style Quick Path controller button shortcut
            const int qpBtn = settings_.bindQuickPath();
            const bool quickPathPressed =
                !settings_.isListening() && !comboGate &&
                !VaultManager::instance().isRecordingCombo() && !kbOpen &&
                qpBtn >= 0 && qpBtn < XrInputState::BtnCount && in.buttonDown[qpBtn];
            if (quickPathPressed) {
                browser_.openBookmarks();
                downloadsWindow_ = false;
                settings_.close();
                browser_.setOpen(true);
                xr_.triggerHaptic(1, 0.040f, 0.0f, 0.6f);
            }

            if (comboGate && !settings_.isListening() && !VaultManager::instance().isRecordingCombo()) {
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
                // Suppress normal button, trigger, and thumbstick functions while combo is active
                in.select = false;
                in.selectHeld = false;
                in.togglePlay = false;
                in.browserToggle = false;
                in.recenter = false;
                in.thumbstickX = 0.0f;
                in.thumbstickY = 0.0f;
                in.thumbstickLX = 0.0f;
                in.thumbstickLY = 0.0f;
                in.thumbstickRX = 0.0f;
                in.thumbstickRY = 0.0f;
            }

            // X is the dedicated download button: it downloads the highlighted SMB entry.
            // Y (or configured move button) is the dedicated File Actions Submenu button.
            const bool downloadPressed =
                !settings_.isListening() && !settings_.isOpen() && !comboGate &&
                !VaultManager::instance().isRecordingCombo() &&
                in.buttonDown[XrInputState::BtnX];

            const int configuredMoveBtn = VaultManager::instance().moveButton();
            const bool actionBtnPressed =
                !settings_.isListening() && !settings_.isOpen() && !comboGate &&
                !VaultManager::instance().isRecordingCombo() &&
                (in.buttonDown[XrInputState::BtnY] ||
                 (configuredMoveBtn >= 0 && configuredMoveBtn < XrInputState::BtnCount &&
                  in.buttonDown[configuredMoveBtn]));

            // Grip full-press click: activate laser pointer on this hand.
            // Exactly one controller has an active laser pointer at any time.
            if (!settings_.isListening() && !comboGate && !VaultManager::instance().isRecordingCombo()) {
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
            {
                std::vector<VulkanRenderer::RailShortcut> rail;
                for (const SettingsMenu::RailShortcut* sc : settings_.railShortcuts()) {
                    rail.push_back({sc->name, sc->kiosk});
                }
                renderer_.setRailShortcuts(rail);
            }
            renderer_.setUiFollowMode(settings_.uiFollowMode());
            renderer_.setPanelCurve(settings_.panelCurve());
            renderer_.setUiOpacity(settings_.uiOpacity());

            // Multi-screen bookkeeping: turning the setting off returns to the
            // classic single main screen; audio follows the chosen mode.
            if (!multiScreenEnabled()) {
                for (int slot = 1; slot < VulkanRenderer::kMaxVideoSlots; ++slot) {
                    if (screens_[slot].open) closeMedia(slot);
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
            renderer_.setScreenControls(multiScreenEnabled() && cur().open, selectedScreen_ + 1,
                                        cur().muted, screensUnlocked_, settings_.screenFrames());
            renderer_.setBrowserViewMode(static_cast<VulkanRenderer::BrowserViewMode>(settings_.browserViewMode()));
            renderer_.setBrowserQuickPath(browser_.isCurrentBookmarked());
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
            pauseOnRemoval_.store(settings_.pauseOnRemoval(), std::memory_order_relaxed);
            for (int slot = 0; slot < VulkanRenderer::kMaxVideoSlots; ++slot) {
                const ScreenState& sc = screens_[slot];
                screenKind_[slot].store(!sc.open ? 0 : (sc.web ? 2 : 1), std::memory_order_release);
            }
            VulkanRenderer::setKeyboardLayout(settings_.keyboardLayout());
            pauseLivestreams_.store(settings_.pauseLivestreams(), std::memory_order_relaxed);
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
                    if (textEntry_.active) cancelKeyboard();
                } else if (downloadsWindow_) {
                    downloadsWindow_ = false;
                } else if (settings_.isOpen()) {
                    settings_.close();
                } else if (browser_.isOpen()) {
                    browser_.setOpen(false);
                    smbModal_.active = false;
                    smbModal_.editingField = SmbModalState::Field::None;
                    if (textEntry_.active) cancelKeyboard();
                } else if (cur().open && renderer_.controlBarVisible()) {
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
            // The head-locked line carries short notices instead (software
            // decoding because another process holds the hardware decoder).
            const bool noticeOn = !notice_.empty() && std::chrono::steady_clock::now() < noticeUntil_;
            renderer_.setDownloadStatus(noticeOn, noticeOn ? notice_ : std::string());
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
                if (sbHover >= 10 && sbHover < 10 + static_cast<int>(settings_.railShortcuts().size())) {
                    // A web shortcut tab (Settings > UI & Appearance): a streaming
                    // service as a kiosk page, or the full browser.
                    const SettingsMenu::RailShortcut& sc = *settings_.railShortcuts()[sbHover - 10];
                    openWebScreen(sc.url, sc.kiosk);
                } else if (sbHover == 0) {
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
                        if (sub <= 3 || (sub == 4 && VaultManager::instance().isUnlocked())) {
                            settings_.setSection(sub);
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
#ifdef PIXELVR_HAVE_MEDIA
                                loadMedia(j.localPath);
                                downloadsWindow_ = false;
                                browser_.setOpen(false);
#endif
                            }
                            interacted = true;
                        }
                    }
                }
            }

            // File Action Submenu or Files view interaction inside Unified Spatial Hub
            if (actionMenu_.visible) {
                if ((!browser_.isOpen() && actionMenu_.page != 5 && actionMenu_.page != 8 && actionMenu_.page != 9) ||
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
                                } else if (actionMenu_.page == 5) {
                                    if (opt == "2D Flat (Standard)") {
                                        applyFormat(0, 0);
                                    } else if (opt == "3D Side-by-Side (SBS)") {
                                        applyFormat(0, 1);
                                    } else if (opt == "3D Top-Bottom (Over-Under)") {
                                        applyFormat(0, 2);
                                    } else if (opt == "VR 180° (Side-by-Side)") {
                                        applyFormat(2, 1);
                                    } else if (opt == "VR 360° (Mono)") {
                                        applyFormat(1, 0);
                                    } else if (opt == "VR 360° 3D (Top-Bottom)") {
                                        applyFormat(1, 2);
                                    }
#ifdef PIXELVR_HAVE_MEDIA
                                    if (opt != "Cancel" && cur().open &&
                                        !VaultManager::instance().isVaultPath(cur().path)) {
                                        playbackMemory_.setFormat(cur().path,
                                                                  settings_.projectionMode(),
                                                                  settings_.stereoMode(), cur().fingerprint);
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
                                            xr_.triggerHaptic(renderer_.pointerHand() == 0 ? 0 : 1, 0.030f, 0.0f, 0.5f);
                                        }
                                        actionMenu_.visible = false;
                                    } else {
                                        actionMenu_.visible = false;
                                    }
                                } else if (actionMenu_.page == 6) {
                                    // Screen picker: 0..2 = screen slot tile, 3 = Cancel.
                                    if (chosen < VulkanRenderer::kMaxVideoSlots) {
                                        const bool occupied = screens_[chosen].open;
                                        if (!occupied && !decoderBudgetAllows(pendingPlayPath_)) {
                                            actionMenu_.subtitle = "Not enough decoder capacity - close a screen first";
                                        } else {
                                            if (pendingPlayPath_ == kWebPath) {
                                                loadWeb(chosen, pendingWebUrl_, pendingWebKiosk_);
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
                                    // Pointing at a segment picks it; stick + A toggles.
                                    const int seg = renderer_.actionMenuKeyHovered();
                                    smbModal_.anon = (seg >= 0) ? (seg == 0) : !smbModal_.anon;
                                    updateSmbModalMenu();
                                } else if ((chosen == 3 || chosen == 4) && smbModal_.anon) {
                                    // Guest access needs no credentials.
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
                                    // Cancel (footer button or header X)
                                    actionMenu_.visible = false;
                                    smbModal_.active = false;
                                    smbModal_.editingField = SmbModalState::Field::None;
                                    if (textEntry_.active) cancelKeyboard();
                                } else if (chosen == 9 && !smbModal_.isNew) {
                                    deleteSmbModal();
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
                    } else if (pbHover == 5) {
                        // Cycle sort order: Name -> Newest -> Largest
                        settings_.setBrowserSort(settings_.browserSort() + 1);
                        browser_.setSortMode(settings_.browserSort());
                        renderer_.setBrowserFirst(0);
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
                            playFromBrowser(playPath);
                        }
                    } else if ((in.togglePlay || in.select) &&
                               renderer_.pointerButton() < 0 && stickActive) {
                        std::string playPath;
                        Browser::Result res = browser_.activate(playPath);
                        if (res == Browser::Result::AddSmbShare) {
                            openSmbModal(true);
                        } else if (res == Browser::Result::Play && !playPath.empty()) {
                            playFromBrowser(playPath);
                        }
                    }
                }
            }
            if ((browser_.isOpen() || downloadsWindow_) && in.recenter && in.headValid) {
                recenterView(in);
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
                    const int seg = renderer_.settingsSegmentHovered();
                    auto act = (seg >= 0) ? settings_.activateSegment(seg) : settings_.activate();
                        if (act == SettingsMenu::Action::CycleDistance) {
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
                // The trigger clicks the bar; A does too (A is the reliable
                // press -- the analog trigger does not always report a click).
                if ((in.select || in.togglePlay) && btn >= 0) {
                    PIXELVR_LOG_INFO("control bar click: button=%d", btn);
                    const auto cb = static_cast<VulkanRenderer::CtrlButton>(btn);
                    if (cb == VulkanRenderer::CtrlButton::Stop) {
#ifdef PIXELVR_HAVE_MEDIA
                        closeMedia();
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
                        actionMenu_.visible = true;
                        actionMenu_.page = 5;
                        actionMenu_.title = "Playback Format";
                        actionMenu_.subtitle = "Select 3D / VR Projection";
                        actionMenu_.options.clear();
                        actionMenu_.options.push_back("2D Flat (Standard)");
                        actionMenu_.options.push_back("3D Side-by-Side (SBS)");
                        actionMenu_.options.push_back("3D Top-Bottom (Over-Under)");
                        if (selectedScreen_ == 0) {  // immersive only on the main screen
                            actionMenu_.options.push_back("VR 180° (Side-by-Side)");
                            actionMenu_.options.push_back("VR 360° (Mono)");
                            actionMenu_.options.push_back("VR 360° 3D (Top-Bottom)");
                        }
                        actionMenu_.options.push_back("Cancel");
                        actionMenu_.selectedOption = 0;
                        xr_.triggerHaptic(0, 0.020f, 0.0f, 0.4f);
                        xr_.triggerHaptic(1, 0.020f, 0.0f, 0.4f);
                    } else if (cb == VulkanRenderer::CtrlButton::AudioToggle) {
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
                            renderer_.setSphereZoom(1.0f);
                        }
                    } else if (cb == VulkanRenderer::CtrlButton::HideHud) {
                        dismissHud(in, nowT);
                    } else if (cb == VulkanRenderer::CtrlButton::ScreenMute ||
                               cb == VulkanRenderer::CtrlButton::Volume) {
                        cur().muted = !cur().muted;
                    } else if (cb == VulkanRenderer::CtrlButton::ScreenAudioFollow) {
                        settings_.setAudioFollow(!settings_.audioFollow());
                        if (settings_.audioFollow()) {
                            applyAudioFollow();
                        } else {
                            for (ScreenState& s : screens_) s.muted = false;  // back to all screens heard
                        }
                    } else if (cb == VulkanRenderer::CtrlButton::ScreenLock) {
                        screensUnlocked_ = !screensUnlocked_;
                    } else if (cb == VulkanRenderer::CtrlButton::ScreenFrames) {
                        settings_.setScreenFrames(!settings_.screenFrames());
                    } else if (cb == VulkanRenderer::CtrlButton::ScreenLayout) {
                        openArrangeMenu();
                    } else if (cb == VulkanRenderer::CtrlButton::ScreenClose) {
                        closeMedia(selectedScreen_);
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
                if (in.recenter && in.headValid) {
                    recenterView(in);
                    interacted = true;
                }
                // Pointing at the VOLUME pill, the stick sets this screen's volume.
                if (renderer_.pointerButton() == static_cast<int>(VulkanRenderer::CtrlButton::Volume) &&
                    std::fabs(in.thumbstickX) > 0.15f && dt > 0.0f) {
                    cur().volume = std::clamp(cur().volume + in.thumbstickX * dt * 0.6f, 0.0f, 1.0f);
                    if (cur().volume > 0.0f) cur().muted = false;
                    // The stick is taken: no screen push, scrubbing or seeking meanwhile.
                    in.thumbstickY = 0.0f;
                    in.thumbstickX = in.thumbstickLX = in.thumbstickRX = 0.0f;
                    interacted = true;
                }
                // Viewpoint: flat screens move nearer/farther (thumbstick Y);
                // immersive 360/180 uses the RIGHT stick -- up/down tilts (angle),
                // left/right pushes content nearer/farther (distance).
                if (settings_.projectionMode() == 0) {
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
                        interacted = true;
                    }
                    // When pointing at the Screen Curve pill on the HUD, thumbstick X smoothly adjusts curvature!
                    if (renderer_.pointerButton() == static_cast<int>(VulkanRenderer::CtrlButton::ScreenToggle) &&
                        std::fabs(in.thumbstickX) > 0.15f && dt > 0.0f) {
                        renderer_.adjustScreenCurvature(in.thumbstickX * dt * 0.65f);
                        in.thumbstickX = in.thumbstickLX = in.thumbstickRX = 0.0f;  // no scrubbing too
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
                if (in.togglePlay && cur().open) {
                    curTogglePause();
                    interacted = true;
                }
                if (cur().open) {
                    // Right stick left/right steps -/+30s (flat only; in 360/180 the
                    // right stick drives the viewpoint instead).
                    constexpr double kStep = 30.0;
                    const bool sphereMode = settings_.projectionMode() != 0;
                    if (!sphereMode && in.thumbstickRX > 0.6f && seekStepArmed_) {
                        curSeekRelative(kStep);
                        seekStepArmed_ = false;
                        interacted = true;
                    } else if (!sphereMode && in.thumbstickRX < -0.6f && seekStepArmed_) {
                        curSeekRelative(-kStep);
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
                        curDuration() > 0.0) {
                        scrubTarget_ = curTimelineStart() + renderer_.pointerScrub() * (curDuration() - curTimelineStart());
                        scrubInput = true;
                    } else if (std::fabs(in.thumbstickLX) > 0.15f && dt > 0.0f &&
                               curDuration() > 0.0) {
                        if (!scrubbing_) scrubTarget_ = curPosition();
                        const float ax = in.thumbstickLX;
                        const float t = (std::fabs(ax) - 0.15f) / 0.85f;
                        const double rate =
                            std::copysign(static_cast<double>(t * t) * 60.0, ax);
                        scrubTarget_ = std::clamp(scrubTarget_ + rate * dt, curTimelineStart(), curDuration());
                        scrubInput = true;
                    }
                    if (scrubInput) {
                        scrubbing_ = true;
                        interacted = true;
                    } else if (scrubbing_) {
                        curSeek(scrubTarget_);  // commit the scrub with one seek
                        scrubbing_ = false;
                    }
                }
                if (interacted) {
                    lastControllerActivity_ = nowT;
                }
                const bool paused = cur().open && curPaused();
                const bool controlsUp =
                    cur().open &&
                    (paused || (controllersEngaged && (in.controllers[0].active ||
                                                       in.controllers[1].active)));
                float progress = 0.0f;
                double elapsed = 0.0, total = 0.0;
                if (cur().open && curDuration() > 0.0) {
                    // Livestreams: the rewind window, live edge on the right.
                    const double start = curTimelineStart();
                    total = std::max(curDuration() - start, 0.001);
                    elapsed = scrubbing_ ? scrubTarget_ : curPosition();
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
                             (settings_.stereoMode() == 1) ? "3D SBS" : "3D TB";
                } else if (settings_.projectionMode() == 2) {
                    fLabel = (settings_.stereoMode() == 1) ? "VR180 SBS" :
                             (settings_.stereoMode() == 2) ? "VR180 TB" : "180 Mono";
                } else {
                    fLabel = (settings_.stereoMode() == 1) ? "360 SBS" : "360 Mono";
                }

                std::string aLabel = "Audio";
                bool aEnabled = false;
                if (cur().open) {
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
                    aEnabled = (aStreams.size() > 1);
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
            scrubPreview_.setSource(cur().open && !cur().web ? cur().path : std::string(),
                                    settings_.stereoMode());
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
                        PIXELVR_LOG_INFO(
                            "PERF: render %.1f fps, decode %.1f/%.2f fps | per frame: feed %.1f ms, frame %.1f ms, "
                            "gpu wait %.1f ms, draw %.1f ms | gpu %ld MHz | late %llu, clock waits %llu",
                            perfFrames_ / since, decFps, cur().engine.frameRate(), perfFeedMs_ / n,
                            perfFrameMs_ / n, gpuWaitMs / n, perfViewCpuMs_ / n, gpuMhz,
                            static_cast<unsigned long long>(late - perfLastLate_),
                            static_cast<unsigned long long>(waits - perfLastWaits_));
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

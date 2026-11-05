#pragma once

#include "gfx/text_atlas.hpp"
#include "util/chapter.hpp"
#include "xr/openxr_headers.hpp"
#include "xr/xr_context.hpp"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace pixelvr {

struct DrmVideoFrame;
struct GltfMesh;

// Owns the Vulkan device (created through OpenXR) and the per-eye render
// resources. For Phase 0/1 it draws a pose-anchored test triangle into each eye
// swapchain image; later phases render the video projection meshes here.
class VulkanRenderer {
public:
    VulkanRenderer() = default;
    ~VulkanRenderer();

    VulkanRenderer(const VulkanRenderer&) = delete;
    VulkanRenderer& operator=(const VulkanRenderer&) = delete;

    // Creates the Vulkan instance, physical device, logical device, and queue
    // via the OpenXR XR_KHR_vulkan_enable2 helpers on `xr`.
    bool createDevice(XrContext& xr);

    // The graphics binding to pass to XrContext::createSession.
    const XrGraphicsBindingVulkan2KHR& graphicsBinding() const { return binding_; }

    // Picks a preferred color format from the runtime-supported list.
    int64_t chooseColorFormat(const std::vector<int64_t>& runtimeFormats) const;

    // Builds the render pass, pipeline, image views, and framebuffers for the
    // provided per-eye swapchains.
    bool createRenderResources(const std::vector<ViewSwapchain>& swapchains);

    // Recreates the MSAA target, depth target, image views, and framebuffers
    // when swapchains are recreated or dynamically resized for supersampling.
    bool recreateSwapchainTargets(const std::vector<ViewSwapchain>& swapchains);

    void waitIdle() const;

    // Records and submits the draw for one eye. Invoked by XrContext::renderFrame.
    void renderView(uint32_t viewIndex, uint32_t imageIndex, const XrView& view,
                    const ViewSwapchain& swapchain);

    // Composition layers: the panel UI (hub, dock, modals, keyboard) and the
    // flat video / browser screens are drawn once into their own swapchains
    // and the compositor places them in the world as quads / cylinders, so
    // they stay sharp at any eye-buffer resolution (and video is downsampled
    // with a proper filter). Call after createRenderResources; the eye passes
    // then skip what the layers show.
    bool createLayerResources(XrContext& xr, int64_t colorFormat);
    void setUiLayerEnabled(bool on) { uiLayerEnabled_ = on; }
    bool uiLayerLive() const { return uiLayerEnabled_ && uiLayerReady_; }
    // XrContext::renderFrame callbacks: the UI layer before the eye passes
    // (its GPU work overlaps them), the screen and cursor layers after (they
    // need this frame's screen poses).
    void renderLayersBefore(XrContext& xr, const XrView* views, uint32_t viewCount,
                            std::vector<CompositionLayer>& out);
    void renderLayersAfter(XrContext& xr, const XrView* views, uint32_t viewCount,
                           std::vector<CompositionLayer>& out);

    // Uploads a decoded NV12 frame (luma + interleaved chroma) into the video
    // textures, creating or resizing them as needed. Safe to call once per frame.
    void updateVideoTexture(int width, int height, const uint8_t* y,
                            const uint8_t* uv, int colorspace, bool fullRange,
                            int slot = 0);

    // Imports a decoded DMA-BUF frame (drm_prime) as a YCbCr image sampled
    // through a VkSamplerYcbcrConversion, replacing the CPU NV12 upload. Resolves
    // ownership of frame.fd (imports it into Vulkan or closes it) and clears it.
    void updateVideoDmabuf(DrmVideoFrame& frame, int slot = 0);
    // Releases the imported dma-buf frame and the decoder's capture-buffer ref
    // while keeping the resolved image on screen; call before the decoder seeks
    // or a new clip opens so the hardware decoder can reclaim its buffers.
    // keepPicture (seeking): a 180/360 frame drawn straight from the decoder
    // is first copied, so it stays on screen until the next frame arrives.
    void releaseVideoFrame(int slot = 0, bool keepPicture = false);
    // Video slots: slot 0 is the main screen (also 360/180); slots 1..2 are the
    // extra flat screens of multi-screen mode.
    static constexpr int kMaxVideoSlots = 3;
    bool hasVideo(int slot = 0) const { return videoSlots_[slot].hasVideo; }
    void clearVideo(int slot = 0) { videoSlots_[slot].hasVideo = false; }

    enum class ProjectionMode { Flat, Equirect360, Equirect180 };
    enum class StereoMode { Mono, SideBySide, TopBottom };
    void setProjectionMode(ProjectionMode mode) { projectionMode_ = mode; }
    void setStereoMode(StereoMode mode) { stereoMode_ = mode; }
    // 360/180 viewpoint: zoom (>1 = content farther) and vertical tilt (radians).
    void adjustSphereZoom(float delta) {
        sphereZoom_ += delta;
        if (sphereZoom_ < 0.5f) sphereZoom_ = 0.5f;
        if (sphereZoom_ > 2.5f) sphereZoom_ = 2.5f;
    }
    void adjustSpherePitch(float delta) {
        spherePitch_ += delta;
        if (spherePitch_ < -0.6f) spherePitch_ = -0.6f;
        if (spherePitch_ > 0.6f) spherePitch_ = 0.6f;
    }
    void setSphereZoom(float z) { sphereZoom_ = z; }
    void setSpherePitch(float p) { spherePitch_ = p; }
    float sphereZoom() const { return sphereZoom_; }
    float spherePitch() const { return spherePitch_; }
    void setSwapEyes(bool on) { swapEyes_ = on; }

    // Multi-screen placement. Slots 1..2 are extra flat screens and are drawn
    // only while `active`; slot 0 (the main screen) follows the recenter pose
    // unless its placement is `active` (the user moved it).
    struct ScreenPlacement {
        bool active = false;
        float center[3] = {0.0f, 0.0f, -3.0f};  // app space, metres
        float yaw = 0.0f;                         // same convention as screenYaw
        float pitch = 0.0f;
        float roll = 0.0f;                        // tilted layouts: screens turn with the wall
        float scale = 1.0f;                       // 1 = the default 3.2 m wide screen
        float curvature = 0.0f;
        StereoMode stereo = StereoMode::Mono;     // slot 0 uses setStereoMode()
    };
    void setScreenPlacement(int slot, const ScreenPlacement& p) {
        if (slot >= 0 && slot < kMaxVideoSlots) screenPlacements_[slot] = p;
    }
    const ScreenPlacement& screenPlacement(int slot) const { return screenPlacements_[slot]; }
    // Arrange menu (action menu page 8): the poses the draft layout would give
    // each screen, built around the view (pivot / yaw / pitch), drawn as a live
    // preview. `preset` = highlighted list entry, `snap` = Snap edges toggle.
    void setArrangePreview(const ScreenPlacement pl[kMaxVideoSlots], const float pivot[3], float yaw,
                           float pitch, int preset, bool snap) {
        for (int i = 0; i < kMaxVideoSlots; ++i) arrangePreview_[i] = pl[i];
        for (int k = 0; k < 3; ++k) arrangePivot_[k] = pivot[k];
        arrangeYaw_ = yaw;
        arrangePitch_ = pitch;
        arrangePreset_ = preset;
        arrangeSnap_ = snap;
    }
    // "Play on which screen?" (page 6): true = show the screens as arranged
    // (preview set with setArrangePreview) instead of a row of tiles.
    void setPickerSpatial(bool spatial) { pickerSpatial_ = spatial; }
    // Picker: an empty slot with no preview tile of its own gets the "Add" card.
    bool pickerAddCardShown() const {
        for (int slot = 0; slot < kMaxVideoSlots; ++slot) {
            if (slot < static_cast<int>(actionMenu_.options.size()) &&
                actionMenu_.options[static_cast<std::size_t>(slot)] == "Add" && !arrangePreview_[slot].active) {
                return true;
            }
        }
        return false;
    }
    // Multi-screen mode: outlines + "selected" highlight; ambilight follows the
    // selected screen.
    // Video screen under the laser (multi-screen mode, no UI in the way), or -1.
    int pointerScreen() const { return pointerScreen_; }
    // Screen slots ordered left-to-right as they sit around the viewer (for the
    // "play on which screen?" picker tiles).
    void screenOrderLeftToRight(int out[kMaxVideoSlots]) const;
    // World-space point where the laser hits that screen.
    const float* pointerScreenHit() const { return pointerHit_; }
    void setMultiScreen(bool active, int selectedSlot) {
        multiScreenActive_ = active;
        selectedScreen_ = (selectedSlot >= 0 && selectedSlot < kMaxVideoSlots) ? selectedSlot : 0;
    }
    void setDrawModels(bool on) { drawModels_ = on; }

    // Debug statistics overlay (FPS, decode, system load). Drawn head-locked.
    void setStats(bool visible, const std::vector<std::string>& lines);

    // Background download status line, drawn head-locked near the bottom edge so
    // downloads are visible after the browser panel is closed.
    void setDownloadStatus(bool visible, const std::string& text) {
        downloadStatusVisible_ = visible;
        downloadStatusText_ = text;
    }

    // Repositions the flat screen in front of the given head pose (headForward is
    // a world-space direction); the current viewing distance is kept.
    void recenterScreen(const float headPos[3], const float headForward[3]);
    const float* screenPivot() const { return screenPivot_; }
    float screenYaw() const { return screenYaw_; }
    float recenterPitch() const { return recenterPitch_; }
    float screenDistance() const { return screenDistance_; }
    const float* screenForward() const { return screenForward_; }
    // Moves the flat screen farther (positive) or nearer (negative), clamped.
    void adjustScreenDistance(float delta);
    void setScreenDistance(float d) { screenDistance_ = d < 1.0f ? 1.0f : (d > 10.0f ? 10.0f : d); }
    // Parametric curvature for cinema screen: 0.0 = flat plane, 1.0 = full cylinder wrap.
    void setScreenCurvature(float c) { screenCurvature_ = c < 0.0f ? 0.0f : (c > 1.0f ? 1.0f : c); }
    float screenCurvature() const { return screenCurvature_; }
    void adjustScreenCurvature(float delta) {
        setScreenCurvature(screenCurvature_ + delta);
    }


    // Transport HUD: progress in [0,1]. Drawn head-locked when visible.
    void setTransport(bool visible, float progress, bool paused,
                      double elapsedSec = 0.0, double durationSec = 0.0) {
        transportVisible_ = visible;
        transportProgress_ = progress;
        transportPaused_ = paused;
        transportElapsed_ = elapsedSec;
        transportDuration_ = durationSec;
    }
    // The VOLUME pill: the selected screen's volume (0..1) and mute.
    void setVolumeDisplay(float volume, bool muted) {
        volumeDisplay_ = volume;
        volumeMuted_ = muted;
    }

    // Music view, drawn on a screen that plays an audio file: track info and
    // buttons beside the cover (part of the video frame), the queue on the
    // right. Rows hold only the visible window of the queue.
    struct MusicView {
        bool active = false;
        std::string title;
        std::string artist;
        std::string album;
        std::string queueInfo;  // e.g. "12 tracks  ·  48 min"
        struct Row {
            std::string title;
            std::string artist;
            std::string duration;
            bool current = false;
        };
        std::vector<Row> rows;
        int first = 0;   // queue index of rows[0]
        int count = 0;   // whole queue
        bool shuffle = false;
        int repeat = 0;  // 0 off, 1 all, 2 one
        bool paused = false;
    };
    static constexpr int kMusicRows = 10;
    // musicHover() ids besides queue indices (0..count-1).
    enum MusicHit { kMusicNone = -1, kMusicShuffle = 100000, kMusicPrev, kMusicPlay, kMusicNext, kMusicRepeat,
                    kMusicList };
    void setMusicView(int slot, const MusicView& view) {
        if (slot >= 0 && slot < kMaxVideoSlots) musicViews_[slot] = view;
    }
    // What the laser points at on a music view (and on which screen).
    int musicHover(int& slot) const {
        slot = musicHoverSlot_;
        return musicHoverId_;
    }

    // Livestream: 0 = no, 1 = live but behind, 2 = at the live edge. The
    // timeline's right end is then live and the readout shows "LIVE".
    void setTransportLive(int live) { transportLive_ = live; }
    // Chapters of the selected screen's video: marks on the timeline, the
    // current chapter's title above it, the hovered one's in the scrub bubble.
    void setTransportChapters(const ChapterList& chapters) { transportChapters_ = chapters; }
    // True when the pointer is over the clickable total/remaining time readout.
    bool pointerOnTimeToggle() const { return pointerOnTimeToggle_; }
    void toggleTimeDisplay() { timeShowRemaining_ = !timeShowRemaining_; }

    struct BrowserItem {
        std::string name;
        std::string path;
        std::string detail;
        std::string tag;
        std::string durationTag;
        std::string sizeTag;
        std::string resolutionTag;
        std::string codecTag;
        std::string previewPath;
        bool isDir = false;
        bool isVideo = false;
        bool isBack = false;
    };

    struct DownloadItem {
        std::string label;
        int state = 0;          // 0=active, 1=done, 2=failed, 3=stopped, 4=queued
        float progress = 0.0f;  // 0.0 to 1.0
        std::string detail;
        bool paused = false;    // active job is paused
    };

    // File browser overlay: rich spatial card grid + sidebar navigation
    void setBrowserItems(bool visible, const std::string& title,
                         const std::vector<BrowserItem>& items, int selected,
                         const std::string& status);
    void setBrowser(bool visible, const std::string& title,
                    const std::vector<std::string>& rows, int selected,
                    const std::string& status);
    void updateCardThumbnail(int slot, const std::string& path, const uint8_t* rgba, int width, int height);
    void clearCardThumbnails();
    // Web browser screens: a slot marked as web shows its pvr-browser page
    // instead of video, with the slot's usual placement.
    void setWebSlot(int slot, bool web) {
        if (slot >= 0 && slot < kMaxVideoSlots) webSlots_[slot] = web;
    }
    void updateWebFrame(int slot, const uint8_t* rgba, int width, int height);
    // Laser position on a web screen (u right, v down, 0..1) and which slot;
    // false when the laser is not on one.
    bool webPointer(int& slot, float& u, float& v) const {
        slot = webPointerSlot_;
        u = webPointerU_;
        v = webPointerV_;
        return webPointerSlot_ >= 0;
    }
    // Timeline hover preview image (RGBA), shown above the scrub time bubble.
    void updateScrubPreview(const uint8_t* rgba, int width, int height, double seconds);
    void clearScrubPreview();
    int browserFirst() const { return browserFirst_; }
    void setBrowserFirst(int first) { browserFirst_ = first < 0 ? 0 : first; }
    int browserSidebarHovered() const { return browserSidebarHovered_; }
    int browserPageBtnHovered() const { return browserPageBtnHovered_; }
    int pointerHand() const { return pointerHand_; }
    void setBrowserQuickFilter(int f) { browserQuickFilter_ = f; }
    int browserQuickFilter() const { return browserQuickFilter_; }
    void setDownloadsFilter(int f) { downloadsFilter_ = f; }
    int downloadsFilter() const { return downloadsFilter_; }
    void setActiveDownloadCount(int c) { activeDownloadCount_ = c; }
    void setSettingsSection(int s) { settingsSection_ = s; }
    void setVaultUnlocked(bool u) { vaultUnlocked_ = u; }
    bool vaultUnlocked() const { return vaultUnlocked_; }
    void setPointerSuppressed(bool s) { pointerSuppressed_ = s; }
    bool pointerSuppressed() const { return pointerSuppressed_; }
    uint64_t ycbcrRebuildCount() const { return ycbcrRebuildCount_; }
    void setUiAppearance(float textScale, int textColorIdx, int accentColorIdx,
                         float uiDistance) {
        uiTextScale_ = textScale;
        uiTextColorIdx_ = textColorIdx;
        uiAccentColorIdx_ = accentColorIdx;
        uiDistance_ = uiDistance;
    }
    // The web shortcuts shown as main tabs on the hub's rail (0..4).
    struct RailShortcut {
        std::string name;
        bool kiosk = true;   // Play icon (streaming page) vs. Globe (full browser)
    };
    void setRailShortcuts(const std::vector<RailShortcut>& shortcuts);
    void setAmbilightIntensity(float intensity) {
        ambilightIntensity_ = intensity < 0.0f ? 0.0f : (intensity > 1.0f ? 1.0f : intensity);
    }
    float ambilightIntensity() const { return ambilightIntensity_; }

    enum class BrowserViewMode : int {
        BigCards = 0,    // 3x2, 6 per page (Large cards with full 16:9 thumbnail)
        MiniCards = 1,   // 4x3, 12 per page (Compact cards with 16:9 thumbnail)
        CompactList = 2  // 1x8, 8 per page (Single-column detailed configurable list view)
    };
    void setBrowserViewMode(BrowserViewMode mode) { browserViewMode_ = mode; }
    BrowserViewMode browserViewMode() const { return browserViewMode_; }
    void setBrowserQuickPath(bool isCurrentQuickPath) { browserIsQuickPath_ = isCurrentQuickPath; }
    void setBrowserBookmarked(bool isCurrentBookmarked) { browserIsQuickPath_ = isCurrentBookmarked; }
    bool browserIsQuickPath() const { return browserIsQuickPath_; }
    bool browserIsBookmarked() const { return browserIsQuickPath_; }
    void setListColumnVisibility(bool showFormat, bool showRes, bool showSize, bool showDuration) {
        listShowFormat_ = showFormat;
        listShowRes_ = showRes;
        listShowSize_ = showSize;
        listShowDuration_ = showDuration;
    }

    struct KeyboardModalData {
        bool visible = false;
        std::string prompt;
        std::string text;
        bool isPassword = false;
        int cursorPos = 0;
        bool hasTransform = false;
        float transform[3][4]{};
        bool shift = false;    // in-app keyboard: next letter upper-case
        bool symbols = false;  // in-app keyboard: symbols page
    };
    void setKeyboardModes(bool shift, bool symbols) {
        keyboardModal_.shift = shift;
        keyboardModal_.symbols = symbols;
    }
    // Index into the in-app keyboard layout under the pointer, or -1.
    int keyboardKeyHovered() const { return keyboardKeyHovered_; }
    // True while the pointer is over the keyboard's text field.
    bool keyboardFieldHovered() const { return keyboardFieldHovered_; }
    // Byte offset of the caret position nearest to where the pointer hits the
    // text field (from the last drawn frame), or -1 when not over the field.
    int keyboardCursorAtPointer() const;
    // Code / character a key index produces (see hub::keyboardKeys); used by the app.
    static int keyboardKeyCode(int index);
    static std::string keyboardKeyText(int index, bool shift, bool symbols);  // UTF-8
    static void setKeyboardLayout(int layout);  // 0 = English, 1 = German
    void setKeyboardModal(bool visible, const std::string& prompt,
                          const std::string& text, bool isPassword = false,
                          const float* transform = nullptr,
                          int cursorPos = -1) {
        keyboardModal_.visible = visible;
        keyboardModal_.prompt = prompt;
        keyboardModal_.text = text;
        keyboardModal_.isPassword = isPassword;
        keyboardModal_.cursorPos = (cursorPos >= 0) ? cursorPos : static_cast<int>(text.size());
        if (transform && visible) {
            std::memcpy(keyboardModal_.transform, transform, sizeof(keyboardModal_.transform));
            keyboardModal_.hasTransform = true;
        } else {
            keyboardModal_.hasTransform = false;
        }
    }
    const KeyboardModalData& keyboardModal() const { return keyboardModal_; }

    void setUiFollowMode(int mode) { uiFollowMode_ = mode; }
    // Main panel curvature: 0 = flat, 1 = wraps around the viewer at the panel
    // distance; values in between bend more gently.
    // Panel surface opacity (1 = solid, lower = more see-through glass).
    void setUiOpacity(float o) { uiOpacity_ = o < 0.3f ? 0.3f : (o > 1.0f ? 1.0f : o); }
    void setPanelCurve(float amount) { panelCurve_ = amount < 0.0f ? 0.0f : (amount > 1.0f ? 1.0f : amount); }
    // Label of the file browser's sort button (e.g. "Name", "Newest").
    void setBrowserSortLabel(const std::string& label) { browserSortLabel_ = label; }
    int uiFollowMode() const { return uiFollowMode_; }

    bool pointerActive() const { return pointerActive_; }

    // Downloads overlay: modern glassmorphic window with job cards
    void setDownloads(bool visible, const std::string& title,
                      const std::vector<std::string>& rows,
                      const std::string& status,
                      const std::vector<std::string>& buttons = {},
                      const std::vector<DownloadItem>& jobs = {}) {
        downloadsVisible_ = visible;
        downloadsTitle_ = title;
        downloadsRows_ = rows;
        downloadsStatus_ = status;
        downloadsButtons_ = buttons;  // content-sized buttons along the bottom
        downloadJobs_ = jobs;
    }
    // Draws a stronger modal backdrop behind the panel (for confirmations).
    void setDownloadsModal(bool modal) { downloadsModal_ = modal; }
    // The downloads-window list row / button under the pointer, or -1.
    int downloadsHovered() const { return downloadsHovered_; }
    int downloadsButtonHovered() const { return downloadsBtnHovered_; }
    // Per-job button on the hovered download card: 0 = pause/resume (Play when
    // finished), 1 = cancel/remove; -1 when the pointer is elsewhere.
    int downloadsJobButtonHovered() const { return downloadsJobBtnHovered_; }

    // File Action Submenu (opened by button Y in file browser)
    struct ActionMenuData {
        bool visible = false;
        int page = 0; // 0: Main, 1: MoveDestinations, 2: DeleteConfirm, 3: Rename
        std::string title;
        std::string subtitle;
        std::vector<std::string> options;
        int selectedOption = -1;
        int editingRow = -1;
        int cursorPos = 0;
        std::string renameBuffer;
    };
    void setActionMenu(const ActionMenuData& data) { actionMenu_ = data; }
    const ActionMenuData& actionMenu() const { return actionMenu_; }
    int actionMenuBtnHovered() const { return actionMenuBtnHovered_; }
    int actionMenuKeyHovered() const { return actionMenuKeyHovered_; }
    void setBrowserNetwork(bool onNet) { browserOnNetwork_ = onNet; }

    // Tracked controllers: aim poses used to draw pointer rays, controller
    // models and the UI cursor. Index 0 = left, 1 = right. Call once per frame,
    // after setBrowser, so the pointer can be raycast against the current panel.
    struct ControllerVis {
        bool active = false;
        float position[3] = {0.0f, 0.0f, 0.0f};
        float orientation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        // Grip pose (controller body) for the render model; the aim pose above
        // drives the pointer ray and UI cursor.
        bool gripActive = false;
        float gripPosition[3] = {0.0f, 0.0f, 0.0f};
        float gripOrientation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    };
    void setControllers(const ControllerVis& left, const ControllerVis& right);

    // Replaces the procedural controller geometry with the runtime-provided glTF
    // render model (parsed from the GLB bytes) for hand (0 = left, 1 = right).
    bool loadControllerModel(int hand, const std::vector<uint8_t>& glb);
    bool loadControllerModel(const std::vector<uint8_t>& glb) {
        return loadControllerModel(1, glb);
    }

    // Per-node animated transforms from xrGetRenderModelStateEXT, matched to
    // submeshes by node name (drives button-press motion on the model).
    struct NodeStateVis {
        std::string name;
        float pos[3] = {0.0f, 0.0f, 0.0f};
        float quat[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        bool visible = true;
    };
    void setControllerNodeStates(int hand, const std::vector<NodeStateVis>& states);
    void setControllerNodeStates(const std::vector<NodeStateVis>& states) {
        setControllerNodeStates(1, states);
    }

    enum class UiShape : int {
        RoundedBox = 0,
        Play = 1,
        Pause = 2,
        SeekBack = 3,
        SeekFwd = 4,
        Files = 5,
        Settings = 6,
        Recenter = 7,
        Cursor = 8,
        Close = 9,
        Checkmark = 10,
        Stop = 11,
        Download = 12,
        List = 13,
        GlowRing = 14,
        Speaker = 15,
        Headset = 16,
        CurvedDisplay = 17,
        Chevron = 18,
        Home = 19,
        SoftGlow = 20,  // smooth halo / shadow around a rounded box
        Star = 21,
        Lock = 22,
        Sort = 23,       // three bars of decreasing length
        SpeakerMuted = 26,  // speaker with a cross
        Move = 27,          // four-way arrows
        Unlocked = 28,      // open padlock
        Layout = 29,        // three screens: two below, one above
        Frame = 30,         // hollow screen outline
        SwapH = 31,         // double-headed arrow, horizontal
        SwapV = 32,         // double-headed arrow, vertical
        Globe = 33,         // web: circle with meridian + parallels
        Backspace = 34,     // key pointing left with a cross
        Shuffle = 35,
        SkipBack = 36,      // previous track
        SkipFwd = 37,       // next track
        Repeat = 38,
        AudioFollow = 39,   // speaker with an arrow: audio follows the selection
        GlassPanel = 25, // rounded box with bevel + eye-dependent reflection sheen
    };

    // The browser row currently under the pointer, or -1 if none.
    int pointerRow() const { return pointerRow_; }
    int browserHovered() const { return browserHovered_; }
    void scrollBrowser(int deltaRows);

    // Updates just the highlighted browser row (used by the pointer each frame).
    void setBrowserSelected(int index) { browserSelected_ = index; }

    enum class CtrlButton {
        SeekBack = 0,
        PlayPause,
        SeekFwd,
        Stop,
        Files,         // "HUB": opens the file browser / downloads / settings
        FormatToggle,
        AudioToggle,
        ScreenToggle,
        Recenter,      // recenter screen/view from HUD
        HideHud,       // immediately hide player controls
        ScreenMute,    // multi-screen row: mute / unmute the selected screen
        ScreenAudioFollow,  // multi-screen row: audio follows the selected screen
        ScreenLock,    // multi-screen row: unlock / lock all screens for grabbing
        ScreenFrames,  // multi-screen row: show / hide the borders around screens
        ScreenLayout,  // multi-screen row: arrange screens with a preset
        ScreenClose,   // multi-screen row: close the selected screen
        Volume,        // left pill: this screen's volume (stick = adjust, click = mute)
        Count
    };
    // Multi-screen row under the dock (shown while Multiple Screens is on and a
    // video plays): which screen the dock controls and its mute state.
    void setScreenControls(bool visible, int screenNumber, bool muted, bool unlocked, bool frames) {
        screenControlsVisible_ = visible;
        screenControlsNumber_ = screenNumber;
        screenControlsMuted_ = muted;
        screensUnlocked_ = unlocked;
        screenFramesOn_ = frames;
    }
    void setControlBar(bool visible) { controlBarVisible_ = visible; }
    void setAudioFollow(bool on) { audioFollow_ = on; }
    // Perf log: milliseconds spent waiting for the GPU (a view's previous use)
    // and in renderView overall since the last call; then reset.
    void takePerfTimes(double& gpuWaitMs, double& viewCpuMs) {
        gpuWaitMs = perfGpuWaitMs_;
        viewCpuMs = perfViewMs_;
        perfGpuWaitMs_ = perfViewMs_ = 0.0;
    }
    bool controlBarVisible() const { return controlBarVisible_; }
    void setHudLabels(const std::string& format, const std::string& audio,
                      const std::string& screen, bool formatEnabled = true,
                      bool audioEnabled = true) {
        hudFormat_ = format;
        hudAudio_ = audio;
        hudScreen_ = screen;
        hudFormatEnabled_ = formatEnabled;
        hudAudioEnabled_ = audioEnabled;
    }

    // Per-hand laser pointer toggle (grip full press). Only one controller has
    // an active laser pointer at any time. When a hand enables its laser, the
    // other hand's laser is turned off.
    void setLaserOn(int hand, bool on) {
        if (hand >= 0 && hand < 2) {
            laserOn_[hand] = on;
            if (on) laserOn_[1 - hand] = false;
        }
    }
    bool laserOn(int hand) const {
        return (hand >= 0 && hand < 2) ? laserOn_[hand] : false;
    }

    // The control-bar button currently under the pointer, or -1 if none.
    int pointerButton() const { return pointerButton_; }
    // True when the pointer is over the draggable timeline; pointerScrub() is
    // then the target position in [0,1].
    bool pointerOnTimeline() const { return pointerOnTimeline_; }
    float pointerScrub() const { return pointerScrub_; }

    // Badge on the control-bar Downloads button: a count while downloading, or a
    // green tick once all downloads are done (drives the on-button indicator).
    void setDownloadsBadge(bool active, bool allDone, bool failed = false) {
        downloadsActive_ = active;
        downloadsAllDone_ = allDone;
        downloadsFailed_ = failed;
    }

    // One settings card. `kind` mirrors SettingsMenu::Kind: 0 choice (stepped
    // slider), 1 toggle (switch), 2 action (chevron), 3 binding (key chip),
    // 4 colour (swatches), 5 option (checkmark when active), 6 segmented
    // (every option shown as a pill; `options` holds the labels).
    struct SettingsRowVis {
        std::string label;
        std::string value;
        int kind = 2;
        int steps = 0;
        int stepIdx = 0;
        bool enabled = true;
        std::vector<std::string> options;
    };
    static constexpr int kSettingsKindSegmented = 6;

    // Settings overlay panel (re-uses panel & pointer interaction)
    void setSettings(bool visible, const std::string& title,
                     const std::vector<SettingsRowVis>& rows, int selected,
                     const std::string& status);
    bool settingsVisible() const { return settingsVisible_; }
    int settingsRow() const { return settingsRow_; }
    int settingsHovered() const { return settingsHovered_; }
    // Segment of a segmented settings row under the pointer, or -1.
    int settingsSegmentHovered() const { return settingsSegmentHovered_; }
    void setSettingsSelected(int index) { settingsSelected_ = index; }

    VkDevice device() const { return device_; }

private:
    bool createRenderPass(VkFormat format);
    VkSampleCountFlagBits chooseSampleCount() const;
    bool createMsaaTarget(int32_t width, int32_t height);
    bool createDepthTarget(int32_t width, int32_t height);
    bool initText();
    bool uploadFontAtlas(const FontAtlas& atlas, VkImage& image, VkDeviceMemory& memory,
                         VkImageView& view);
    bool createSamplerAndDescriptors();
    bool createPipelines();
    bool buildPipeline(const std::string& vertName, const std::string& fragName,
                       VkPipelineLayout layout, VkPipeline& out, bool blend = false,
                       VkRenderPass pass = VK_NULL_HANDLE,
                       VkSampleCountFlagBits samples =
                           VK_SAMPLE_COUNT_FLAG_BITS_MAX_ENUM,
                       const VkPipelineVertexInputStateCreateInfo* vertexInput = nullptr,
                       VkColorComponentFlags writeMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT);
    struct VideoSlot;
    bool createVideoResources(VideoSlot& v, int width, int height);
    void destroyVideoResources(VideoSlot& v);
    bool createYcbcrPipeline(VideoSlot& v, uint32_t fourcc, int colorspace, bool fullRange);
    void destroyYcbcr(VideoSlot& v);
    // `mips`: a full mip chain (flat screens shown small); 180/360 needs none.
    bool createResolveTarget(VideoSlot& v, int width, int height, bool mips = true);
    void destroyRetiredFrames(VideoSlot& v, std::size_t keep);  // oldest first, down to `keep`
    void destroyResolveTarget(VideoSlot& v);
    bool createModelPipeline();
    // Content-sized layout for the downloads-window buttons (centered, along the
    // bottom). Fills centre-x and half-width per button.
    void downloadsButtonLayout(std::vector<float>& cx,
                               std::vector<float>& halfW) const;
    bool importDmabufImage(VideoSlot& v, const DrmVideoFrame& frame, VkImage& image,
                           VkDeviceMemory& memory, VkImageView& view,
                           bool& fdConsumed);
    // `acquireOnly`: just hand the decoder's frame to the GPU (direct 180/360).
    void resolveFrame(VideoSlot& v, VkImage importedImage, bool acquireOnly = false, bool acquired = false);
    void updatePointer();
    uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const;
    VkShaderModule loadShaderModule(const std::string& filename);

    struct ViewTargets {
        int32_t width = 0;
        int32_t height = 0;
        std::vector<VkImageView> imageViews;
        std::vector<VkFramebuffer> framebuffers;
    };

    // One UI-layer pass: which image, the pixel sub-rect drawn, and the
    // panel-space window (metres) that maps onto it.
    struct UiLayerPass {
        uint32_t imageIndex = 0;
        int32_t px = 0, py = 0, pw = 0, ph = 0;
        float cx = 0.0f, cy = 0.0f, hw = 1.0f, hh = 1.0f;
        float headPos[3] = {0.0f, 0.0f, 0.0f};
    };
    // Shared body of renderView (eye pass, layer == nullptr) and renderUiLayer.
    void recordView(uint32_t viewIndex, uint32_t imageIndex, const XrView& view,
                    const UiLayerPass* layer);
    float panelBendRadius(float panelDist) const {
        const bool curved = panelCurve_ > 0.01f && !(uiLayerLive() && !uiLayerCylinder_);
        return curved ? panelDist / panelCurve_ : 0.0f;
    }
    bool uiLayerEnabled_ = true;
    bool uiLayerReady_ = false;
    bool uiLayerCylinder_ = false;   // runtime can bend the layer (else it stays flat)
    VkRenderPass uiLayerPass_ = VK_NULL_HANDLE;
    ViewSwapchain uiSwapchain_;
    bool createLayerTargets(const ViewSwapchain& sc, ViewTargets& out);
    void destroyLayerTargets(ViewTargets& t);
    // A flat screen shown as a composition layer: the video / page is copied
    // into a swapchain sized to its angular size (so the compositor never
    // minifies it much), re-done whenever a new frame arrives.
    struct ScreenLayer {
        ViewSwapchain sc;
        ViewTargets targets;
        uint64_t drawnSerial = 0;   // frameSerial_ last copied
        bool web = false;
    };
    ScreenLayer screenLayers_[kMaxVideoSlots];
    bool screenLayerLive_[kMaxVideoSlots] = {};   // eye passes skip these screens
    uint64_t frameSerial_[kMaxVideoSlots] = {};   // bumps with every new video / page frame
    ViewSwapchain cursorSwapchain_;                // pointer cursor over layer screens
    ViewTargets cursorTargets_;
    void recordScreenCopy(int slot, const ScreenLayer& L, uint32_t imageIndex);
    VkPipeline uiPipelineLayer_ = VK_NULL_HANDLE;
    // Writes alpha only (no blend): stamps alpha = coverage over a glass panel
    // so the layer is opaque there. SteamVR shows the layers underneath only
    // where a layer's alpha is exactly zero, so partial alpha has to stay
    // confined to anti-aliased edges.
    VkPipeline uiPipelineLayerAlpha_ = VK_NULL_HANDLE;
    VkPipeline textPipelineLayer_ = VK_NULL_HANDLE;
    VkPipeline thumbPipelineLayer_ = VK_NULL_HANDLE;
    VkPipeline pipelineLayer_ = VK_NULL_HANDLE;   // NV12 video quad (screen picker preview)
    // Frosted-glass backdrop: the blurred video behind a panel (layer pass only).
    VkPipelineLayout glassPipelineLayout_ = VK_NULL_HANDLE;  // NV12 textures + GlassPush
    VkPipeline glassPipelineLayer_ = VK_NULL_HANDLE;
    ViewTargets uiLayerTargets_;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    uint32_t queueFamilyIndex_ = 0;
    VkQueue queue_ = VK_NULL_HANDLE;

    XrGraphicsBindingVulkan2KHR binding_{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};

    VkFormat colorFormat_ = VK_FORMAT_UNDEFINED;
    VkSampleCountFlagBits sampleCount_ = VK_SAMPLE_COUNT_1_BIT;
    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipeline ambilightPipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout spherePipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline spherePipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout uiPipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline uiPipeline_ = VK_NULL_HANDLE;

    float ambilightIntensity_ = 0.55f;

    ProjectionMode projectionMode_ = ProjectionMode::Flat;
    StereoMode stereoMode_ = StereoMode::Mono;
    bool swapEyes_ = false;
    bool drawModels_ = false;

    bool statsVisible_ = false;
    std::vector<std::string> statsLines_;
    bool downloadStatusVisible_ = false;
    std::string downloadStatusText_;

    // Downloads overlay (list of jobs).
    bool downloadsVisible_ = false;
    std::string downloadsTitle_;
    std::vector<std::string> downloadsRows_;
    std::string downloadsStatus_;
    bool downloadsActive_ = false;
    bool downloadsAllDone_ = false;
    bool downloadsFailed_ = false;
    bool downloadsModal_ = false;
    std::vector<std::string> downloadsButtons_;
    std::vector<DownloadItem> downloadJobs_;
    int downloadsBtnHovered_ = -1;
    int downloadsHovered_ = -1;
    int downloadsJobBtnHovered_ = -1;
    // Whether a job with `state` is listed under the current downloads filter
    // (0 = all, 1 = active/paused/queued, 2 = completed).
    bool downloadJobVisible(int state) const {
        if (downloadsFilter_ == 1) return state == 0 || state == 3 || state == 4;
        if (downloadsFilter_ == 2) return state == 1;
        return true;
    }

    // Action menu state
    ActionMenuData actionMenu_{};
    int actionMenuBtnHovered_ = -1;
    int actionMenuKeyHovered_ = -1;
    int keyboardKeyHovered_ = -1;
    bool keyboardFieldHovered_ = false;
    float keyboardFieldHitX_ = 0.0f;  // panel-space x of the pointer on the field
    // Caret stops of the drawn text field: (panel x, byte offset), left to right.
    std::vector<std::pair<float, int>> kbCaretStops_;
    bool browserOnNetwork_ = false;

    // Flat-screen placement in the app reference space (updated by recenter and
    // thumbstick distance control).
    float screenPivot_[3] = {0.0f, 0.0f, 0.0f};
    float screenForward_[3] = {0.0f, 0.0f, -1.0f};
    float screenYaw_ = 0.0f;
    float recenterPitch_ = 0.0f;  // head pitch at recenter (screen/controls tilt)
    float sphereYaw_ = 0.0f;  // recenter longitude offset for 360/180 content
    float spherePitch_ = 0.0f;  // 360/180 viewpoint tilt (radians)
    float sphereZoom_ = 1.0f;   // 360/180 FOV scale (>1 pushes content farther)
    float screenDistance_ = 3.0f;
    float screenCurvature_ = 0.0f;  // 0.0 = flat plane (default), 1.0 = deep cinema curve

    // Transport HUD state.
    bool transportVisible_ = false;
    float transportProgress_ = 0.0f;
    bool transportPaused_ = false;
    double transportElapsed_ = 0.0;   // seconds played
    double transportDuration_ = 0.0;  // seconds total
    bool timeShowRemaining_ = false;  // right readout shows time-left vs. total
    bool pointerOnTimeToggle_ = false;
    int transportLive_ = 0;
    ChapterList transportChapters_;
    float volumeDisplay_ = 1.0f;
    bool volumeMuted_ = false;
    MusicView musicViews_[kMaxVideoSlots];
    int musicHoverSlot_ = -1;
    int musicHoverId_ = kMusicNone;
    std::string hudFormat_ = "2D Flat";
    std::string hudAudio_ = "Audio";
    std::string hudScreen_ = "Curved";
    bool hudFormatEnabled_ = true;
    bool hudAudioEnabled_ = true;
    float sphereFollowYaw_ = 0.0f;
    bool sphereFollowInit_ = false;

    struct CardThumbnail {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
        std::string path;
        bool ready = false;
    };
    // Slots 0..11: file browser cards; slot 12: timeline scrub preview.
    static constexpr int kCardThumbSlots = 12;
    static constexpr int kScrubThumbSlot = 12;
    CardThumbnail cardThumbs_[13];
    void releaseThumbSlot(int slot);
    void releaseWebTexture(int slot);
    struct WebTexture {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkBuffer staging = VK_NULL_HANDLE;
        VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
        void* stagingPtr = nullptr;
        VkDescriptorSet set = VK_NULL_HANDLE;
        int width = 0;
        int height = 0;
        uint32_t mipLevels = 1;    // full chain: a small browser screen minifies cleanly
        bool initialized = false;  // image has been written once (layout is shader-read)
    };
    WebTexture webTex_[kMaxVideoSlots];
    bool webSlots_[kMaxVideoSlots] = {};
    int webPointerSlot_ = -1;
    float webPointerU_ = 0.0f;
    float webPointerV_ = 0.0f;
    float cardHoverAnim_[12]{};
    // Motion: eased 0..1 states advanced once per frame (the layer pass, or
    // the first eye when the UI is drawn in-scene).
    float hubOpenAnim_ = 0.0f;                    // hub / modal scale-in
    float dockOpenAnim_ = 0.0f;                   // dock slide-in from below
    float dockHoverAnim_[32]{};                   // per transport button
    float railHoverAnim_[8]{};                    // per rail main tab
    std::chrono::steady_clock::time_point animLastT_{};
    VkPipeline thumbPipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool thumbPool_ = VK_NULL_HANDLE;
    BrowserViewMode browserViewMode_ = BrowserViewMode::BigCards;
    bool browserIsQuickPath_ = false;
    KeyboardModalData keyboardModal_{};
    int uiFollowMode_ = 0; // 0 = World Lock (Default, zero drift/tilt), 1 = Smooth Gaze Follow
    float panelCurve_ = 0.0f;
    float uiOpacity_ = 1.0f;
    std::string browserSortLabel_ = "Name";
    bool listShowFormat_ = true;
    bool listShowRes_ = true;
    bool listShowSize_ = true;
    bool listShowDuration_ = true;

    // Text rendering (FreeType glyph atlas) + file browser overlay.
    FontAtlas fontAtlas_;
    VkImage fontImage_ = VK_NULL_HANDLE;
    VkDeviceMemory fontMemory_ = VK_NULL_HANDLE;
    VkImageView fontView_ = VK_NULL_HANDLE;
    VkSampler fontSampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout textSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool textPool_ = VK_NULL_HANDLE;
    VkDescriptorSet textSet_ = VK_NULL_HANDLE;
    // Heading weight (Inter SemiBold when bundled); textSetBold_ == textSet_
    // when no bold font could be loaded.
    FontAtlas fontAtlasBold_;
    VkImage fontImageBold_ = VK_NULL_HANDLE;
    VkDeviceMemory fontMemoryBold_ = VK_NULL_HANDLE;
    VkImageView fontViewBold_ = VK_NULL_HANDLE;
    VkDescriptorSet textSetBold_ = VK_NULL_HANDLE;
    VkPipelineLayout textPipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline textPipeline_ = VK_NULL_HANDLE;
    bool textReady_ = false;
    // Per-pass glyph instance buffers (host-visible, persistently mapped): text
    // is appended during renderView and drawn in a few instanced batches.
    // [0], [1] = eyes, [2] = UI layer pass.
    static constexpr int kRenderPasses = 4;   // + [3] = screen / cursor layer copies
    VkBuffer glyphBuffer_[kRenderPasses] = {};
    VkDeviceMemory glyphMemory_[kRenderPasses] = {};
    void* glyphMapped_[kRenderPasses] = {};

    bool browserVisible_ = false;
    std::string browserTitle_;
    std::string browserStatus_;
    int browserSelected_ = 0;
    int browserHovered_ = -1;  // pointer hover target (independent of scroll)
    int browserFirst_ = 0;     // scroll offset for visible window
    std::vector<BrowserItem> browserItems_;
    int browserSidebarHovered_ = -1;
    int browserPageBtnHovered_ = -1;
    int browserQuickFilter_ = 0;  // 0=Local, 1=Videos, 2=SMB, 3=Downloads, 4=Bookmarks, 5=Vault
    int downloadsFilter_ = 0;  // 0=All, 1=Active, 2=Completed
    int activeDownloadCount_ = 0;
    int settingsSection_ = 0;
    float uiTextScale_ = 1.20f;
    int uiTextColorIdx_ = 0;
    int uiAccentColorIdx_ = 0;
    float uiDistance_ = 1.9f;
    std::vector<RailShortcut> railShortcuts_ = {{"YouTube", true}, {"Browser", false}};

    // Settings panel overlay
    bool settingsVisible_ = false;
    std::string settingsTitle_;
    std::string settingsStatus_;
    std::vector<SettingsRowVis> settingsRows_;
    int settingsSelected_ = 0;
    int settingsHovered_ = -1;
    int settingsSegmentHovered_ = -1;
    int settingsRow_ = -1;

    // Tracked controllers + pointer (raycast against UI panels).
    ControllerVis controllers_[2];
    bool pointerActive_ = false;
    int pointerRow_ = -1;
    int pointerHand_ = -1;
    // Whether each hand's laser pointer is on. Only one controller has an active
    // laser pointer at any time. Defaults to right controller active, left off.
    bool laserOn_[2] = {false, true};
    float pointerHit_[3] = {0.0f, 0.0f, 0.0f};
    // Hit in panel-local metres when the pointer is on the UI panel (the
    // cursor is then drawn into the UI layer rather than into the eye buffers).
    bool pointerOnPanel_ = false;
    float pointerPanel_[2] = {0.0f, 0.0f};
    bool controlBarVisible_ = false;
    bool vaultUnlocked_ = false;
    bool pointerSuppressed_ = false;
    int pointerButton_ = -1;
    bool pointerOnTimeline_ = false;
    float pointerScrub_ = 0.0f;

    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    // One command buffer + fence per pass (two eyes + the UI layer) so the CPU
    // can record the next pass while the GPU renders the previous one;
    // commandBuffer_ is the one being recorded.
    VkCommandBuffer commandBuffer_ = VK_NULL_HANDLE;
    VkCommandBuffer viewCommandBuffers_[kRenderPasses] = {};
    VkFence viewFences_[kRenderPasses] = {};
    bool viewFenceInFlight_[kRenderPasses] = {};

    // Video texture sampling: descriptor set + sampler shared by both eyes.
    VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;

    // NV12 video textures (luma R8 full-res + chroma RG8 half-res) and staging.

    // YUV->RGB coefficients + luma range/scale, derived from the frame metadata.


    // All GPU state of one playing video: CPU NV12 upload textures, the zero-copy
    // dma-buf import with its YCbCr conversion + pipelines, and the mipmapped
    // RGBA resolve target the video pipelines sample.
    struct VideoSlot {
        VkDescriptorSet descriptorSet = VK_NULL_HANDLE;  // CPU NV12 path

        // NV12 video textures (luma R8 full-res + chroma RG8 half-res) and staging.
        VkImage lumaImage = VK_NULL_HANDLE;
        VkDeviceMemory lumaMemory = VK_NULL_HANDLE;
        VkImageView lumaView = VK_NULL_HANDLE;
        VkImage chromaImage = VK_NULL_HANDLE;
        VkDeviceMemory chromaMemory = VK_NULL_HANDLE;
        VkImageView chromaView = VK_NULL_HANDLE;
        int videoWidth = 0;
        int videoHeight = 0;
        uint32_t lumaMipLevels = 1;
        uint32_t chromaMipLevels = 1;
        bool videoLayoutInitialized = false;
        bool hasVideo = false;
        float videoAspect = 16.0f / 9.0f;

        // YUV->RGB coefficients + luma range/scale, derived from the frame metadata.
        float conversion[4] = {1.402f, -0.344f, -0.714f, 1.772f};
        float range[4] = {16.0f / 255.0f, 255.0f / 219.0f, 1.0f, 0.0f};

        VkBuffer stagingBuffer = VK_NULL_HANDLE;
        VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
        VkDeviceSize stagingSize = 0;
        void* stagingMapped = nullptr;

        // Zero-copy dma-buf video: a YCbCr conversion + immutable sampler baked into
        // its own descriptor set layout and pipelines, rebuilt when the frame format
        // or colour metadata changes. The imported image is single-buffered and
        // retired on the next import.
        VkSamplerYcbcrConversion ycbcrConversion = VK_NULL_HANDLE;
        VkSampler ycbcrSampler = VK_NULL_HANDLE;
        VkDescriptorSetLayout ycbcrSetLayout = VK_NULL_HANDLE;
        VkDescriptorPool ycbcrPool = VK_NULL_HANDLE;
        VkDescriptorSet ycbcrSet = VK_NULL_HANDLE;
        VkPipelineLayout ycbcrQuadLayout = VK_NULL_HANDLE;
        VkPipeline ycbcrQuadPipeline = VK_NULL_HANDLE;
        VkPipeline ycbcrQuadPipelineLayer = VK_NULL_HANDLE;  // UI layer pass variant
        VkPipelineLayout glassLayout = VK_NULL_HANDLE;        // resolve set + GlassPush
        VkPipeline glassPipelineLayer = VK_NULL_HANDLE;       // frosted-glass backdrop
        VkPipeline ycbcrAmbilightPipeline = VK_NULL_HANDLE;
        VkPipelineLayout ycbcrSphereLayout = VK_NULL_HANDLE;
        VkPipeline ycbcrSpherePipeline = VK_NULL_HANDLE;
        VkFormat ycbcrFormat = VK_FORMAT_UNDEFINED;
        uint32_t ycbcrKeyFourcc = 0;
        int ycbcrKeyColorspace = -1;
        bool ycbcrKeyFullRange = false;
        VkImage drmImage = VK_NULL_HANDLE;
        VkDeviceMemory drmMemory = VK_NULL_HANDLE;
        VkImageView drmView = VK_NULL_HANDLE;
        std::shared_ptr<void> drmKeepAlive;  // decoder frame ref for the live image
        bool usingDmabuf = false;
        float drmUvScaleX = 1.0f;  // visible/coded, applied to UVs to drop padding
        float drmUvScaleY = 1.0f;

        // Minification mip chain: the imported YCbCr frame is resolved into this
        // RGBA image (with mipmaps) each frame, then sampled trilinearly.
        VkRenderPass resolvePass = VK_NULL_HANDLE;
        VkPipelineLayout resolveLayout = VK_NULL_HANDLE;
        VkPipeline resolvePipeline = VK_NULL_HANDLE;
        VkSampler resolveSampler = VK_NULL_HANDLE;
        VkDescriptorSetLayout resolveSetLayout = VK_NULL_HANDLE;
        VkDescriptorPool resolvePool = VK_NULL_HANDLE;
        VkDescriptorSet resolveSet = VK_NULL_HANDLE;
        VkImage resolveImage = VK_NULL_HANDLE;
        VkDeviceMemory resolveMemory = VK_NULL_HANDLE;
        VkImageView resolveView = VK_NULL_HANDLE;        // all mips (sampled)
        VkImageView resolveTargetView = VK_NULL_HANDLE;  // mip 0 (render target)
        VkFramebuffer resolveFbo = VK_NULL_HANDLE;
        int resolveW = 0;
        int resolveH = 0;
        uint32_t resolveMips = 1;
        // The resolve runs asynchronously on its own command buffer: the next
        // one waits for this fence (long done by then), the render thread never.
        VkCommandBuffer resolveCmd = VK_NULL_HANDLE;
        VkFence resolveFence = VK_NULL_HANDLE;
        bool resolvePending = false;
        // 180/360 draws straight from the decoder's frame (no resolve): one
        // descriptor set per recent frame, and the frames that rendering in
        // flight may still sample (destroyed a few frames later).
        static constexpr int kDirectRing = 4;
        VkDescriptorSet directSets[kDirectRing] = {};
        int directIdx = 0;
        bool directActive = false;
        bool directMode = false;    // the clip is drawn straight from the decoder (180/360)
        int directW = 0, directH = 0;  // its frame size
        VkPipelineLayout ycbcrSphereDirectLayout = VK_NULL_HANDLE;
        VkPipeline ycbcrSphereDirectPipeline = VK_NULL_HANDLE;
        struct RetiredFrame {
            VkImage image = VK_NULL_HANDLE;
            VkDeviceMemory memory = VK_NULL_HANDLE;
            VkImageView view = VK_NULL_HANDLE;
            std::shared_ptr<void> keepAlive;  // the decoder's buffer
        };
        std::deque<RetiredFrame> retired;
    };
    VideoSlot videoSlots_[kMaxVideoSlots];
    ScreenPlacement screenPlacements_[kMaxVideoSlots];
    ScreenPlacement arrangePreview_[kMaxVideoSlots];
    int screenDrawOrder_[kMaxVideoSlots] = {0, 1, 2};  // far to near, shared by both eyes
    float arrangePivot_[3] = {0.0f, 0.0f, 0.0f};
    float arrangeYaw_ = 0.0f;
    float arrangePitch_ = 0.0f;
    int arrangePreset_ = -1;
    bool arrangeSnap_ = true;
    // Swap arrows between neighbouring screens in the arrange preview (panel
    // coords, refreshed while drawing; hit-tested on the next frame).
    struct SwapArrow {
        float x = 0.0f;
        float y = 0.0f;
        bool vertical = false;
        int slotA = 0;
        int slotB = 0;
    };
    SwapArrow arrangeArrows_[4];
    int arrangeArrowCount_ = 0;
    // Preview tile outlines (x0, x1, y0, y1 in panel coords) for hit-testing.
    float arrangeTileRect_[kMaxVideoSlots][4] = {};
    bool arrangeTileValid_[kMaxVideoSlots] = {};
    bool pickerSpatial_ = false;  // screen picker shows the arranged screens
    bool multiScreenActive_ = false;
    int selectedScreen_ = 0;
    int pointerScreen_ = -1;
    bool screenControlsVisible_ = false;
    bool audioFollow_ = false;
    double perfGpuWaitMs_ = 0.0;
    double perfViewMs_ = 0.0;
    int screenControlsNumber_ = 1;
    bool screenControlsMuted_ = false;
    bool screensUnlocked_ = false;  // screens can be grabbed with the laser
    bool screenFramesOn_ = true;    // borders around screens (multi-screen)
    // Last drawn flat-screen geometry per slot (for laser hit-testing): centre,
    // yaw/pitch and half-size in metres; valid only while the slot is drawn.
    struct ScreenHitInfo {
        bool valid = false;
        float center[3] = {0.0f, 0.0f, 0.0f};
        float yaw = 0.0f;
        float pitch = 0.0f;
        float roll = 0.0f;
        float halfW = 0.0f;   // half the (arc) width
        float halfH = 0.0f;
        float bendR = 0.0f;   // curved screens: radius of the arc (0 = flat)
    };
    ScreenHitInfo screenHit_[kMaxVideoSlots];

    uint64_t ycbcrRebuildCount_ = 0;

    VkCommandBuffer uploadCmd_ = VK_NULL_HANDLE;
    VkFence uploadFence_ = VK_NULL_HANDLE;

    std::vector<ViewTargets> viewTargets_;

    // Shared multisampled color target, resolved into each eye's swapchain image.
    VkImage msaaImage_ = VK_NULL_HANDLE;
    VkDeviceMemory msaaMemory_ = VK_NULL_HANDLE;
    VkImageView msaaView_ = VK_NULL_HANDLE;

    // Shared depth buffer, reused across eyes (they render sequentially). Gives
    // the 3D controller models correct self-occlusion instead of showing through.
    VkImage depthImage_ = VK_NULL_HANDLE;
    VkDeviceMemory depthMemory_ = VK_NULL_HANDLE;
    VkImageView depthView_ = VK_NULL_HANDLE;
    VkFormat depthFormat_ = VK_FORMAT_D32_SFLOAT;

    float clearColor_[4] = {0.02f, 0.02f, 0.05f, 1.0f};
    bool dmabufCapable_ = false;
    bool queueFamilyForeign_ = false;
    PFN_vkGetMemoryFdPropertiesKHR getMemoryFdProperties_ = nullptr;

    // Zero-copy dma-buf video: a YCbCr conversion + immutable sampler baked into
    // its own descriptor set layout and pipelines, rebuilt when the frame format
    // or colour metadata changes. The imported image is single-buffered and
    // retired on the next import (renderView has waited on its fence by then).

    // Minification mip chain: the imported YCbCr frame is resolved into this RGBA
    // image (with mipmaps) each frame, then sampled trilinearly by the video
    // pipelines so 4K content doesn't shimmer when shown on a small screen.

    // Lit, textured 3D controller render models. The glTF geometry is loaded
    // separately for left (0) and right (1) hands into handModels_[2].
    // Drawn with the depth buffer so near parts occlude far ones.
    VkPipelineLayout modelLayout_ = VK_NULL_HANDLE;
    VkPipeline modelPipeline_ = VK_NULL_HANDLE;

    // Procedural capsule fallback buffers (used before glTF is fetched)
    VkBuffer capsuleVertexBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory capsuleVertexMemory_ = VK_NULL_HANDLE;
    VkBuffer capsuleIndexBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory capsuleIndexMemory_ = VK_NULL_HANDLE;
    uint32_t capsuleIndexCount_ = 0;

    struct ModelTexture {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
    };
    struct ModelSubmesh {
        uint32_t firstIndex = 0;
        uint32_t indexCount = 0;
        VkDescriptorSet set = VK_NULL_HANDLE;  // base-color texture descriptor
        float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        std::string nodeName;  // matched to an animated render-model node state
    };
    struct HandModel {
        VkBuffer vertexBuffer = VK_NULL_HANDLE;
        VkDeviceMemory vertexMemory = VK_NULL_HANDLE;
        VkBuffer indexBuffer = VK_NULL_HANDLE;
        VkDeviceMemory indexMemory = VK_NULL_HANDLE;
        uint32_t indexCount = 0;
        VkIndexType indexType = VK_INDEX_TYPE_UINT32;
        std::vector<ModelTexture> textures;
        std::vector<ModelSubmesh> submeshes;
        std::vector<NodeStateVis> nodeStates;
        bool ready = false;
    };
    HandModel handModels_[2];  // 0 = left, 1 = right

    VkDescriptorSetLayout modelSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool modelDescPool_ = VK_NULL_HANDLE;
    VkSampler modelSampler_ = VK_NULL_HANDLE;
    ModelTexture modelWhite_;  // 1x1 white fallback for untextured submeshes

    void destroyHandModel(HandModel& hm);
    bool uploadHandMesh(int hand, const GltfMesh& mesh);
    bool createModelTexture(const uint8_t* rgba, int width, int height,
                            ModelTexture& out);
};

} // namespace pixelvr

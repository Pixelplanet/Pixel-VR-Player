#pragma once

#include "gfx/vulkan_renderer.hpp"
#include "xr/xr_context.hpp"
#include "xr/render_model.hpp"

#ifdef PIXELVR_HAVE_MEDIA
#include "media/media_engine.hpp"
#include "media/scrub_preview.hpp"
#endif

#ifdef PIXELVR_HAVE_FREETYPE
#include "ui/browser.hpp"
#include "ui/thumbnail_cache.hpp"
#endif

#include "ui/settings_menu.hpp"
#include "app/playback_memory.hpp"
#include "app/browser_host.hpp"
#include "app/music_queue.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <thread>

namespace pixelvr {

// Owns the top-level objects and runs the frame loop. Later phases add the
// scene, UI, and media sources here.
class App {
public:
    App();
    ~App();
    bool init(const std::string& mediaPath = "");
    int run();
    void requestExit() { exitRequested_ = true; }

private:
    // Declaration order matters for teardown: the XR session (xr_) must be
    // destroyed before the Vulkan device (renderer_), so renderer_ is declared
    // first and therefore destroyed last.
    VulkanRenderer renderer_;
    XrContext xr_;
    RenderModelSystem renderModels_;
    bool exitRequested_ = false;

    // Transport HUD auto-hide timer.
    std::chrono::steady_clock::time_point uiUntil_{};
    // Frame timing for continuous seek (F4).
    std::chrono::steady_clock::time_point lastFrameTime_{};

    // Controller activity: auto-hide the laser, models and controls when the
    // user holds the controllers still for a few seconds.
    std::chrono::steady_clock::time_point lastControllerActivity_{};
    float activityAnchorPos_[2][3] = {{0, 0, 0}, {0, 0, 0}};
    float activityAnchorOrient_[2][4] = {{0, 0, 0, 1}, {0, 0, 0, 1}};
    bool activityAnchorValid_[2] = {false, false};
    bool seekStepArmed_ = true;  // right-stick +/-30s debounce
    bool navArmed_ = true;       // menu nav: armed for the next stick press
    std::chrono::steady_clock::time_point navNextRepeat_{};  // nav auto-repeat
    bool navArmedX_ = true;      // horizontal grid nav
    std::chrono::steady_clock::time_point navNextRepeatX_{};
    bool scrubbing_ = false;     // dragging the timeline / stick-scrubbing
    double scrubTarget_ = 0.0;   // pending scrub position (seconds)
    std::chrono::steady_clock::time_point perfLastLog_{};
    int perfFrames_ = 0;
    uint64_t perfLastDecoded_ = 0;

    // Place the screen in front of the user on the first valid head pose.
    bool didInitialRecenter_ = false;
    bool controllerModelLoaded_[2] = {false, false};  // glTF model uploaded per hand
    // Local grip->render-model-space correction per hand so each model sits
    // exactly where the runtime renders it.
    bool modelCorrValid_[2] = {false, false};
    float modelCorrPos_[2][3] = {{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}};
    float modelCorrQuat_[2][4] = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f, 1.0f}};
    // Background download status line: shown while active and briefly after a
    // download finishes (so success/failure is visible with the browser closed).
    std::chrono::steady_clock::time_point downloadToast_{};
    std::string downloadToastText_;
    // Pending folder-download confirmation (SMB): uri + display name.
    std::string pendingDl_;
    std::string pendingDlName_;
    bool pendingDlFresh_ = false;  // suppress the same-frame A press
    bool downloadsWindow_ = false;  // downloads list window toggled from the bar
    bool dlWasActive_ = false;      // edge detect: auto-open the window on start
    bool hudDismissed_ = false;     // manually dismissed player controls / HUD
    std::chrono::steady_clock::time_point hudDismissTime_{}; // timestamp of manual dismissal
    void dismissHud(const XrInputState& in, std::chrono::steady_clock::time_point nowT);
    // Low-pass smoothed model pose per hand (the raw hand pose jitters; the
    // system's own controller model is visibly smoothed).
    bool modelSmoothInit_[2] = {false, false};
    float modelSmoothPos_[2][3] = {{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}};
    float modelSmoothQuat_[2][4] = {{0.0f, 0.0f, 0.0f, 1.0f},
                                    {0.0f, 0.0f, 0.0f, 1.0f}};

    // Debug statistics (recomputed a few times per second).
    std::chrono::steady_clock::time_point lastStatsUpdate_{};
    int statsFrameAccum_ = 0;
    uint64_t statsLastDecoded_ = 0;
    double decFpsEma_ = 0.0;  // smoothed decode fps (the raw window is bursty)
    unsigned long long cpuPrevIdle_ = 0;
    unsigned long long cpuPrevTotal_ = 0;
    std::vector<std::string> statsLines_;
    void updateStats();
    int64_t colorFormat_ = 0;
    void applyResolutionScale(float scale);
    // Menu nav stick auto-repeat: -1/+1 to step (initial press + hold), else 0.
    int navStep(float axisY, std::chrono::steady_clock::time_point now);
    int navStepX(float axisX, std::chrono::steady_clock::time_point now);
    void performVaultLock();
    // Closes the current clip and returns the renderer to flat/mono so the hub
    // panel stops behaving as if 180/360 content were still playing.
    void closeMedia(int slot = -1);
    // Stores the current clip's position in playbackMemory_ (never for vault files).
    void rememberPlaybackPosition(int slot = -1);
    VulkanRenderer::ActionMenuData actionMenu_{};
    void openActionMenu();

    struct SmbModalState {
        bool active = false;
        bool isNew = true;
        std::string origHost;
        std::string origShare;
        std::string host;
        std::string share;
        bool anon = false;
        std::string user;
        std::string pass;
        std::string workgroup = "WORKGROUP";
        std::string label;
        enum class Field { None, Host, Share, User, Pass, Workgroup, Label } editingField = Field::None;
    } smbModal_{};
    void openSmbModal(bool isNew, const std::string& smbPath = "");
    void updateSmbModalMenu();
    void saveSmbModal();
    void deleteSmbModal();

    std::function<void(const std::string&)> kbOnDone_;
    std::function<void()> kbOnCancel_;
    // In-app keyboard (text entry for SMB fields and rename). Replaces the
    // SteamVR system keyboard, which needed undocumented compositor hooks and
    // could leave SteamVR holding controller focus.
    struct TextEntry {
        bool active = false;
        std::string prompt;
        std::string text;
        std::size_t cursor = 0;  // byte offset (UTF-8 aware moves)
        bool password = false;
        bool shift = false;
        bool symbols = false;
    } textEntry_;
    void commitKeyboard();
    void cancelKeyboard();
    void moveTextCursor(int delta);
    void pressKeyboardKey(int key);
    // Hold-to-repeat for keyboard keys (cursor arrows, Delete) and the stick:
    // first repeat after a pause, then faster the longer it is held.
    struct KeyRepeat {
        int key = -1;  // keyboard key index being held, or -1
        int stickDir = 0;  // thumbstick direction being held, or 0
        std::chrono::steady_clock::time_point start{};
        std::chrono::steady_clock::time_point next{};
    } kbRepeat_;
    bool kbFieldDrag_ = false;  // trigger held after clicking into the text field
    static std::chrono::milliseconds repeatInterval(std::chrono::steady_clock::duration held);
    void openKeyboard(const std::string& prompt,
                      const std::string& initialText,
                      bool isPassword,
                      const XrInputState& in,
                      std::function<void(const std::string&)> onDone,
                      std::function<void()> onCancel = nullptr);

#ifdef PIXELVR_HAVE_FREETYPE
    Browser browser_;
    ThumbnailCache thumbnailCache_;
#endif

    SettingsMenu settings_;
    PlaybackMemory playbackMemory_;  // resume position + format override per file
    std::chrono::steady_clock::time_point lastPlaybackRemember_{};

    // Multi-screen: one player per video slot (slot 0 = main screen, also used
    // for 360/180; slots 1..2 = extra flat screens). Everything that used to act
    // on "the" video acts on the selected screen via cur().
    struct ScreenState {
#ifdef PIXELVR_HAVE_MEDIA
        MediaEngine engine;
        VideoFrame frame;
        DrmVideoFrame drmFrame;
#endif
        bool open = false;
        std::string path;    // for resume and reloading on a track change
        bool muted = false;  // user mute (the audio mode may mute it as well)
        int projection = 0;  // 0 flat, 1 360, 2 180 (extra screens: always 0)
        int stereo = 0;      // 0 mono, 1 SBS, 2 TB
        bool web = false;    // shows the web browser instead of a video
        float volume = 1.0f; // this screen's own volume (on top of the system volume)
#ifdef PIXELVR_HAVE_MEDIA
        MusicQueue queue;    // audio files: the folder they were opened from
#endif
        int musicFirst = 0;  // first queue row shown in the music view
        std::string fingerprint;     // file identity for the playback memory
        bool formatPending = false;  // 3D layout still to come from the picture check
        bool formatByHand = false;   // the viewer's remembered choice applies
    };
    ScreenState screens_[VulkanRenderer::kMaxVideoSlots];
    int selectedScreen_ = 0;
    ScreenState& cur() { return screens_[selectedScreen_]; }
    bool multiScreenEnabled() const;
    int openScreenCount() const;
    int freeScreenSlot() const;  // first unused extra slot, or -1
    void selectScreen(int slot);
    // Applies a projection/stereo format to the selected screen (renderer +
    // settings mirror). Extra screens accept flat formats only.
    bool applyFormat(int projection, int stereo);
    // Default placement for a new extra screen: beside the open ones (left for
    // screen 2, right for screen 3) so it never lands behind another screen.
    void placeNewScreen(int slot);
    VulkanRenderer::ScreenPlacement newScreenPlacement(int slot) const;  // where placeNewScreen puts it
    // Recenter the view; in multi-screen mode the whole arrangement moves along.
    void recenterView(const XrInputState& in);
    // Rough check that another stream fits the hardware decoder budget.
    bool decoderBudgetAllows(const std::string& path) const;
    // "Play on which screen?" picker with live preview tiles.
    void openPlayTargetMenu(const std::string& path);
    // Gives a screen an explicit placement (the main screen starts from its
    // recenter-anchored pose) so it can be moved or resized freely.
    void ensureScreenPlacement(int slot);
    // Unlocked screens can be grabbed with the laser (hold trigger) and dragged.
    bool screensUnlocked_ = false;
    float grabOffset_[3] = {0.0f, 0.0f, 0.0f};  // screen centre minus grab point
    void startGrabbingScreen(int slot, const XrInputState& in);
    float grabStart_[3] = {0.0f, 0.0f, 0.0f};  // grabbed screen centre at grab start
    float grabStartScale_ = 1.0f;
    // Screen arrangements. A layout (preset + which screen sits in which
    // position + snap / style) becomes screen poses in computeArrangement(),
    // which feeds both the Arrange menu's live preview and the real screens.
    enum class ArrangePreset { SideBySide, TwoBelowOneAbove, Stacked, Cinema };
    struct Arrangement {
        ArrangePreset preset = ArrangePreset::SideBySide;
        std::vector<int> order;  // position -> screen slot
        bool snap = true;        // edge on edge (no gap)
        bool flat = false;       // one flat wall instead of curving around the viewer
        float yaw = 0.0f;        // view the layout is built around
        float pitch = 0.0f;
        float dist = 3.0f;
        float scale = 1.0f;      // group size (snapped screens resize together)
        float curvature = 0.0f;  // shared by all screens of the layout
    };
    void computeArrangement(const Arrangement& a,
                            VulkanRenderer::ScreenPlacement out[VulkanRenderer::kMaxVideoSlots]) const;
    void applyArrangement(const Arrangement& a);
    bool inArrangement(int slot) const;
    bool groupedScreen(int slot) const { return arrangementActive_ && arrangement_.snap && inArrangement(slot); }
    // A new / closed screen joins / leaves the arrangement in use.
    void joinArrangement(int slot);
    void leaveArrangement(int slot);
    // Re-anchors the whole (snapped) arrangement so `slot` lands on `target`.
    void moveArrangementTo(int slot, const float target[3]);
    bool arrangementActive_ = false;  // the last applied layout is still in place
    // Screens start as an edge-to-edge side-by-side wall (or the last layout
    // chosen) until the user drags one out of an unsnapped layout.
    bool arrangementBroken_ = false;
    Arrangement arrangement_;
    // Arrange menu (action menu page 8): presets on the left, live preview of
    // the draft on the right (swap arrows between neighbours), snap / style, Apply.
    void openArrangeMenu();
    void selectArrangePreset(ArrangePreset preset);
    void refreshArrangeMenu();
    Arrangement arrangeDraft_;
    std::vector<ArrangePreset> arrangePresets_;  // presets listed in the menu
    float screenEyeAspect(int slot) const;  // per-eye width / height of a screen's video
    // Browser 'play' entry point: asks where to play when screens are in use.
    void playFromBrowser(const std::string& path);
    int movingScreen_ = -1;  // screen being placed with the laser, or -1
    // Web browser screen (proof of concept): the pvr-browser helper's page on a
    // floating screen; the laser acts as the mouse.
    // Browser screens live on screen slots like videos (placement, arrangement,
    // close); each has its own Chromium window. "web://" is the pseudo path the
    // screen picker uses for "open a browser here".
    static constexpr const char* kWebPath = "web://";
    static constexpr const char* kYouTubeUrl = "https://www.youtube.com";
    static constexpr const char* kBrowserHomeUrl = "https://www.google.com";
    BrowserHost webHosts_[VulkanRenderer::kMaxVideoSlots];
    int webMouseSlot_ = -1;      // browser screen the press / hover belongs to
    bool webMouseDown_ = false;
    bool webDragging_ = false;   // moved past the click slop while pressed
    float webMouseX_ = 0.0f;     // smoothed laser position, page pixels
    float webMouseY_ = 0.0f;
    float webDownX_ = 0.0f;      // where the press started
    float webDownY_ = 0.0f;
    bool webHoverValid_ = false;
    float webWheelAccum_ = 0.0f;
    // YouTube / Browser tabs: pick a screen (or use the main one) for `url`;
    // `kiosk` = just the page (YouTube), else a full browser with toolbar.
    void openWebScreen(const std::string& url, bool kiosk);
    // Opens `url` on `slot`; a browser of the same kind already there opens it
    // in a new tab (kiosk: navigates).
    void loadWeb(int slot, const std::string& url, bool kiosk);
    std::string pendingWebUrl_;  // page waiting for the "play where?" choice
    bool pendingWebKiosk_ = false;
    // In-app keyboard typing straight into a browser screen's focused field
    // (-1 = the keyboard edits its own text as usual).
    int kbWebSlot_ = -1;
    void openWebKeyboard(int slot, const BrowserHost::FocusEvent* field, const XrInputState& in);
    void webCaretTo(std::size_t byteOffset);  // move the page caret along with ours
    // Dock transport for the selected screen: its video, or on a browser
    // screen the page's main video (media bridge).
    bool curPaused();
    double curPosition();
    // Timeline [curTimelineStart, curDuration] in media time; 0 duration = no
    // timeline (nothing playing, a livestream without rewind).
    double curDuration();
    double curTimelineStart();  // livestreams: oldest point to rewind to
    int curLive();              // 0 = not live, 1 = live but behind, 2 = at the live edge
    void curTogglePause();
    void curSeek(double t);
    void curSeekRelative(double d);
    float movingDistance_ = 3.0f;
    std::string pendingPlayPath_;  // file waiting for the "play where?" choice

#ifdef PIXELVR_HAVE_MEDIA
    ScrubPreview scrubPreview_;  // timeline hover keyframe preview (selected screen)
    // `fromQueue`: the music queue picked this track (it starts from the top,
    // keeps the queue, and leaves open menus alone).
    void loadMedia(const std::string& path, int slot = -1, bool fromQueue = false);
    void buildMusicQueue(int slot, const std::string& path);  // audio file opened from a folder
    bool lastLoadFailed_ = false;  // the hub stays open to show why
    void playQueueTrack(int slot, int index);
    void updateMusicView(int slot);
    float musicScrollAccum_ = 0.0f;
    void switchTracks(bool byLanguage);  // reload current media at its position
#endif

    std::atomic<bool> watchdogRunning_{false};
    std::thread watchdogThread_;
    std::atomic<int64_t> lastFrameHeartbeatMs_{0};
    std::atomic<bool> pendingAutoLock_{false};
    std::atomic<bool> pauseOnRemoval_{true};  // settings mirrors for the watchdog thread
    std::atomic<bool> pauseLivestreams_{false};
    std::chrono::steady_clock::time_point lastGripFullRelease_{};
    void watchdogLoop();
};

} // namespace pixelvr

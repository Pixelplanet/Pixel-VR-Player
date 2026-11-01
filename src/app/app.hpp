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
    // Default placement for a new extra screen beside the main one.
    void placeNewScreen(int slot);
    // Recenter the view; in multi-screen mode the whole arrangement moves along.
    void recenterView(const XrInputState& in);
    // Rough check that another stream fits the hardware decoder budget.
    bool decoderBudgetAllows(const std::string& path) const;
    void openScreenMenu(int slot);
    void openPlayTargetMenu(const std::string& path);
    // Browser 'play' entry point: asks where to play when screens are in use.
    void playFromBrowser(const std::string& path);
    int movingScreen_ = -1;  // screen being placed with the laser, or -1
    float movingDistance_ = 3.0f;
    std::string pendingPlayPath_;  // file waiting for the "play where?" choice

#ifdef PIXELVR_HAVE_MEDIA
    ScrubPreview scrubPreview_;  // timeline hover keyframe preview (selected screen)
    void loadMedia(const std::string& path, int slot = -1);
    void switchTracks(bool byLanguage);  // reload current media at its position
#endif

    std::atomic<bool> watchdogRunning_{false};
    std::thread watchdogThread_;
    std::atomic<int64_t> lastFrameHeartbeatMs_{0};
    std::atomic<bool> pendingAutoLock_{false};
    std::chrono::steady_clock::time_point lastGripFullRelease_{};
    void watchdogLoop();
};

} // namespace pixelvr

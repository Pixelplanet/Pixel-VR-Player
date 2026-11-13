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
#include "app/chromium_installer.hpp"
#include "app/music_queue.hpp"
#include "plugins/plugin_host.hpp"
#include "plugins/plugin_repo.hpp"
#include "media/letterbox.hpp"
#include "util/one_euro.hpp"
#include "xr/vr_app_manifest.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <functional>
#include <string>
#include <thread>
#include <unordered_map>

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
    // Web screens only: after a scrub commits, the bar keeps showing the target
    // until the page reports a time near it (its time trails the seek). Any other
    // seek clears it (curSeek / curSeekRelative). -1 = no hold.
    double scrubHoldPos_ = -1.0;
    int scrubHoldScreen_ = -1;
    std::chrono::steady_clock::time_point scrubHoldUntil_{};
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
    // Folder download waiting for its answer in the shared confirm dialog (action menu page 2).
    bool confirmFolderDl_ = false;
    std::string folderDlPath_;
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
    // Menu nav stick auto-repeat: -1/+1 to step (initial press + hold), else 0.
    int navStep(float axisY, std::chrono::steady_clock::time_point now);
    int navStepX(float axisX, std::chrono::steady_clock::time_point now);
    void performVaultLock();
    // Closes the current clip and returns the renderer to flat/mono so the hub
    // panel stops behaving as if 180/360 content were still playing.
    void closeMedia(int slot = -1);
    // Plex Web hand-off: the page's play request plays the original file on
    // the same slot; stopping, closing or finishing it brings the page back.
    void startPlexPlay(int slot, const BrowserHost::PlexPlay& play);
    // Plex / Jellyfin hand-off: first wait (off the frame loop) until the server sends bytes.
    void beginWebPlay(int slot, const BrowserHost::PlexPlay& play);
    struct PendingWebPlay {
        int slot = 0;
        BrowserHost::PlexPlay play;
        std::shared_ptr<std::atomic<int>> state;  // 0 waiting, 1 the server answered, 2 it failed
        std::chrono::steady_clock::time_point since;
    };
    std::vector<PendingWebPlay> pendingWebPlays_;
    bool pluginScrubbing_ = false;  // scrubbing right now (told to plugins)
    // Per screen: the name reported to plugins (an encrypted vault file's real name, read once per
    // load from inside the file). Cleared on every load and close, so a renamed file is read afresh.
    std::string pluginNamePath_[VulkanRenderer::kMaxVideoSlots];
    std::string pluginNameReal_[VulkanRenderer::kMaxVideoSlots];
    int plexAudioStream_ = -1;  // the page's audio track for the hand-off being loaded
    int shortcutDragFrom_ = -1;  // settings row of the shortcut being dragged (list page)
    int shortcutDragTo_ = -1;    // the row it would land on
    bool returnFromPlex(int slot);  // false when the slot is not playing a Plex hand-off
    bool updatePlexPlayback(int slot);  // true once it ended and the page is back
    bool plexLoading_ = false;
    // Stores the current clip's position in playbackMemory_ (never for vault files).
    void rememberPlaybackPosition(int slot = -1);
    VulkanRenderer::ActionMenuData actionMenu_{};
    void openActionMenu();
    // Move / Copy: the user opens the target folder in the normal browser and confirms there ("Move
    // here"). Also used to choose the download folder from Settings ("Other...").
    struct PickState {
        enum class Kind { None, Move, Copy, DownloadDir };
        Kind kind = Kind::None;
        std::string srcPath, srcLabel;
        bool srcIsDir = false;
        std::vector<std::string> sidecars;  // matching files that travel with a video
    } pick_;
    bool picking() const { return pick_.kind != PickState::Kind::None; }
    void startPick(PickState::Kind kind, const Browser::Entry& e);
    void startDownloadDirPick();
    void startVaultMove(bool lockWhenDone = false);  // Settings > Vault location: to / from the SD card (FileOps relocation)
    void cancelPick(bool backToSettings = true);
    void confirmPick();
    std::string pickReason() const;  // why the open folder can't take it ("" = it can)
    std::vector<std::string> sidecarPatterns() const;
    // Footer notices in the browser (results, warnings): they outlive the folder rescans that follow.
    std::string hubNotice_;
    bool hubNoticeWarn_ = false;
    bool hubNoticePrivate_ = false;  // names a vault item: dropped when the vault locks
    std::chrono::steady_clock::time_point hubNoticeUntil_{};
    void showNotice(const std::string& text, bool warn = false, bool priv = false, double seconds = 6.0);
    void updateBrowserFooter(std::chrono::steady_clock::time_point now);
    void drainFileOps();
    std::string pendingActionPath_;  // the row Rename / Delete was opened for (the list may change meanwhile)
    // The format menu opened from a file's menu in the browser applies to THAT file (stored for its
    // next play), not to whatever is playing. Empty = opened from the dock: the playing screen.
    std::string formatMenuPath_;
    bool formatStoredOnly_ = false;
    void chooseFormat(int projection, int stereo);
    // Real names of encrypted files shown as a folder's preview (read once, dropped on lock): the
    // random file name must never be searched for format hints ("3d" is two hex digits).
    std::unordered_map<std::string, std::string> badgeNameCache_;
    bool confirmEditorDelete_ = false;  // the same confirmation, opened from the share editor's Delete
    bool confirmShareDelete_ = false;  // the delete confirmation (page 2) is for a saved network share
    void deleteShareEntry(const std::string& path);  // removes a saved share and its login
    std::chrono::steady_clock::time_point smbDeleteArmedUntil_{};  // share editor: Delete asks once more
    bool vaultCardSeen_ = false;     // the SD card with the vault was in while unlocked

    struct SmbModalState {
        bool active = false;
        bool isNew = true;
        std::string origHost;
        std::string origShare;
        std::string origProtocol = "smb";
        int origPort = 0;
        std::string protocol = "smb";  // smb | sftp | dav | davs
        std::string port;              // digits; empty = the protocol's default
        std::string host;
        std::string share;             // the start folder for sftp / dav
        bool anon = false;
        std::string user;
        std::string pass;
        std::string workgroup = "WORKGROUP";
        std::string label;
        enum class Field { None, Host, Share, User, Pass, Workgroup, Port, Label } editingField = Field::None;
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
        bool reveal = false;  // password shown in clear ("Show" on the keyboard)
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

    // Controller actions (buttons are assigned in SteamVR, see XrContext).
    void closeHub();                // hub, its menus, dialogs and the keyboard
    void cycleHubTab(int dir);      // Previous / Next: the rail's main tabs, wrapping
    void selectNextScreen(int dir); // Next / Previous Screen (multi-screen)
    void musicPrevious(int slot);   // the music view's prev / next buttons
    void musicNext(int slot);
    void cycleAudioTrack();         // the dock's Audio button
    void openFormatMenu();          // the dock's Format button (page 5)
    // The dock's Audio button: a list of the file's audio tracks (and video tracks when it has
    // several) to pick from directly (page 11). The bound "Audio Track" action still cycles.
    void openTracksMenu();
    struct TrackRow {
        bool audio = true;
        int index = -1;  // stream index within the container
    };
    std::vector<TrackRow> trackRows_;
    void downloadFocusedEntry();    // network entry: download (a folder asks first)
    void refreshControlsInfo();     // Settings > Controls > Buttons: live bindings
    bool buttonsPageShown_ = false;
    uint32_t bindingsGenSeen_ = 0;
    std::chrono::steady_clock::time_point lastControlsReadT_{};  // Buttons pages: ~1 Hz re-read
    bool controlsWarn_ = false;  // an essential action is unbound (warning hint)
    bool controlsNoticePending_ = false;  // old in-app button choices were reset
    // Floating button labels (Show Controls / Settings > Button Labels): each
    // action's bound source paths, re-read when the bindings change (and every
    // few seconds while shown); the cards follow the context every frame.
    std::vector<std::vector<std::string>> labelSources_;
    bool labelSourcesRead_ = false;
    uint32_t labelSourcesGen_ = 0;
    std::chrono::steady_clock::time_point labelSourcesReadT_{};
    std::string labelMappingLogged_;  // last logged button -> action mapping
    // laserOnWeb: the laser is on a web page this frame (the page takes B and the stick).
    void updateButtonLabels(std::chrono::steady_clock::time_point nowT, bool laserOnWeb);
    // Playback D-pad hold-to-repeat: 1 up, 2 down, 3 left, 4 right, 0 none.
    int dpadRepeatDir_ = 0;
    std::chrono::steady_clock::time_point dpadNextRepeat_{};

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
        bool opening = false;       // MediaEngine::open() is running on openThread
        std::thread openThread;
        std::atomic<int> openState{0};  // 0 = idle/running, 1 = succeeded, 2 = failed
        // A cancelled open whose thread is still inside the network library (an SMB connect or
        // read can't be interrupted): it is left to finish on its own, never joined on the frame
        // thread. Until then the engine belongs to that thread; a new load for this screen waits.
        bool openAbandoned = false;
        std::atomic<bool> openThreadDone{true};
        bool hasDeferredLoad = false;
        std::string deferredPath;
        bool deferredFromQueue = false;
        bool hasDeferredPlex = false;          // a Plex / Jellyfin hand-off waiting for the engine
        BrowserHost::PlexPlay deferredPlex{};
        std::chrono::steady_clock::time_point openStartedAt{};
        bool openFromQueue = false;
        bool openSameFile = false;
        bool openPlex = false;
        BrowserHost::PlexPlay openPlexPlay{};
        double openSeekPos = -1.0;  // >= 0: explicit seek after open (switchTracks / applyFormat)
        bool openStartPaused = false;
        int openQueueTries = 0;
        std::string path;    // for resume and reloading on a track change
        bool muted = false;  // user mute (the audio mode may mute it as well)
        int projection = 0;  // 0 flat, 1 360, 2 180 (extra screens: always 0)
        int stereo = 0;      // 0 mono, 1 SBS, 2 TB
        bool web = false;    // shows the web browser instead of a video
        bool webSuspended = false;  // its BrowserHost is alive, paused behind a natively playing (Plex) video
        std::chrono::steady_clock::time_point plexReportAt{};
        std::string plexService;        // "plex" or "jellyfin" (told to plugins)
        bool plexEpisode = false;       // a TV episode: the next one is asked for when it ends
        bool plexAwaitingNext = false;  // ended, waiting for the next episode's hand-off
        bool plexNavManual = false;     // ... asked for with the screen's arrow (keeps playing if there is none)
        std::chrono::steady_clock::time_point plexNextSince{};
        float volume = 1.0f; // this screen's own volume (on top of the system volume)
#ifdef PIXELVR_HAVE_MEDIA
        MusicQueue queue;    // audio files: the folder they were opened from
#endif
        int musicFirst = 0;  // first queue row shown in the music view
        std::string fingerprint;     // file identity for the playback memory
        bool formatPending = false;  // 3D layout still to come from the picture check
        float letterbox = 0.0f;      // black bars left out (share of the height, top and bottom each)
        bool letterboxPending = false;  // the measurement is still running
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
    // Adds `slot` to `a` the way a new screen joins: a third screen makes the standard
    // "two below, one above" (the new one on top) unless the user chose three in a row.
    void addToArrangement(Arrangement& a, int slot) const;
    bool rowOfThreeChosen_ = false;  // Side by side was applied with three screens
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
    bool looksImmersive(const std::string& path);  // 180/360, judged before opening it
    int movingScreen_ = -1;  // screen being placed with the laser, or -1
    // Web browser screen (proof of concept): the pvr-browser helper's page on a
    // floating screen; the laser acts as the mouse.
    // Browser screens live on screen slots like videos (placement, arrangement,
    // close); each has its own Chromium window. "web://" is the pseudo path the
    // screen picker uses for "open a browser here".
    static constexpr const char* kWebPath = "web://";
    BrowserHost webHosts_[VulkanRenderer::kMaxVideoSlots];
    // Plugins (docs/concepts/PLUGIN_SYSTEM_SPEC.md): fed one snapshot of the
    // screens per frame; the Plex title stands in for the tokenised URL.
    PluginHost plugins_;
    PluginRepo pluginRepo_;
    LetterboxDetector letterboxDetector_;
    void applyLetterbox(int slot);
    std::string plexTitles_[VulkanRenderer::kMaxVideoSlots];
    std::string plexFiles_[VulkanRenderer::kMaxVideoSlots];  // server file names (plugins match scripts by them)
    void updatePlugins();
    void installPlugin(const std::string& file);  // a .pvrplugin chosen in the file browser
    int webMouseSlot_ = -1;      // browser screen the press / hover belongs to
    bool webMouseDown_ = false;
    bool webDragging_ = false;   // moved past the click slop while pressed
    float webMouseX_ = 0.0f;     // smoothed laser position, page pixels
    float webMouseY_ = 0.0f;
    float webDownX_ = 0.0f;      // where the press started
    float webDownY_ = 0.0f;
    bool webHoverValid_ = false;
    float webWheelAccum_ = 0.0f;
    // The stick belongs to the page: latched while held, and shortly after the
    // laser leaves a web screen (no screen push from a slipped hit test).
    OneEuroPose aimFilter_[2];  // laser smoothing per hand (L1)
    bool volumeDragging_ = false;  // trigger held after a click on the volume bar
    bool webStickLatched_ = false;
    std::chrono::steady_clock::time_point webStickUntil_{};
    // YouTube / Browser tabs: pick a screen (or use the main one) for `url`;
    // `kiosk` = just the page (YouTube), else a full browser with toolbar.
    void openWebScreen(const std::string& url, bool kiosk, bool forceIncognito = false);
    // Opens `url` on `slot`; a browser of the same kind already there opens it
    // in a new tab (kiosk: navigates).
    void loadWeb(int slot, const std::string& url, bool kiosk, bool forceIncognito = false);
    std::string pendingWebUrl_;  // page waiting for the "play where?" choice
    bool pendingWebKiosk_ = false;
    bool pendingWebIncognito_ = false;
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
    bool webSeekBlocked() const;  // ad playing on a streaming page
    void curSeekRelative(double d);
    float movingDistance_ = 3.0f;
    std::string pendingPlayPath_;  // file waiting for the "play where?" choice

#ifdef PIXELVR_HAVE_MEDIA
    ScrubPreview scrubPreview_;  // timeline hover keyframe preview (selected screen)
    // `fromQueue`: the music queue picked this track (it starts from the top,
    // keeps the queue, and leaves open menus alone).
    void loadMedia(const std::string& path, int slot = -1, bool fromQueue = false);
    void cancelPendingOpen(int slot, bool wait = false);  // wait: join even a stuck open (exit only)
    bool startingDeferredLoad_ = false;  // the UI part of loadMedia already ran when it was requested
    bool panShiftLatched_ = false;       // 360/180: bumper + stick pan lasts until the sticks are centred
    void finishLoadMedia(int slot);
    void pollMediaOpens(std::chrono::steady_clock::time_point now);
    void buildMusicQueue(int slot, const std::string& path);  // audio file opened from a folder
    // Empties `slot` for new content, keeping its place in the screen layout.
    void clearSlotKeepLayout(int slot);
    void applyAudioFollow();  // audio-follow on: only the selected screen is heard
    bool lastLoadFailed_ = false;  // the hub stays open to show why
    // Short head-locked notice (e.g. software decoding because another process
    // holds the hardware decoder), shown until noticeUntil_.
    std::string notice_;
    std::chrono::steady_clock::time_point noticeUntil_{};
    std::string chapterKey_;       // whose chapters the timeline shows
    // Chromium missing: the install dialog (action menu page 9), and the
    // browser screen to open once it is installed.
    ChromiumInstaller chromiumInstaller_;
    int installWebSlot_ = -1;
    std::string installWebUrl_;
    bool installWebKiosk_ = false;
    void showChromiumDialog();  // (re)builds page 9 for the installer's state
    // Perf log accumulators (milliseconds since the last PERF line).
    double perfFeedMs_ = 0.0;
    double perfFrameMs_ = 0.0;
    double perfViewCpuMs_ = 0.0;
    uint64_t perfLastLate_ = 0;
    uint64_t perfLastWaits_ = 0;
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
    // What each screen shows, for the watchdog thread: 0 none, 1 media, 2 web.
    std::atomic<int> screenKind_[VulkanRenderer::kMaxVideoSlots] = {};
    std::atomic<bool> pauseLivestreams_{false};
    std::chrono::steady_clock::time_point lastGripFullRelease_{};
    void watchdogLoop();
};

} // namespace pixelvr

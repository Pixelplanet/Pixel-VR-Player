#pragma once

#include "gfx/vulkan_renderer.hpp"
#include "xr/xr_context.hpp"
#include "xr/render_model.hpp"

#ifdef PIXELVR_HAVE_MEDIA
#include "media/media_engine.hpp"
#include "media/scrub_preview.hpp"
#include "media/subtitle_sources.hpp"
#endif

#ifdef PIXELVR_HAVE_FREETYPE
#include "ui/browser.hpp"
#include "ui/thumbnail_cache.hpp"
#endif

#include "ui/settings_menu.hpp"
#include "ui/laser_focus.hpp"
#include "app/playback_memory.hpp"
#include "app/focus_log.hpp"
#include "app/gaze_filter.hpp"
#include "app/stereo_focus.hpp"
#include "app/browser_host.hpp"
#include "app/chromium_installer.hpp"
#include "app/music_queue.hpp"
#include "plugins/plugin_host.hpp"
#include "plugins/plugin_repo.hpp"
#include "media/letterbox.hpp"
#include "net/smb_credentials.hpp"
#include "util/one_euro.hpp"
#include "xr/vr_app_manifest.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
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
    std::string pendingDlName_;
    // Folder download waiting for its answer in the shared confirm dialog (action menu page 2).
    bool confirmFolderDl_ = false;
    // The shared two-button dialog with any question: `onYes` runs when the first button is chosen.
    // Asked from another dialog, its Cancel (and B) goes back to that dialog.
    void askConfirm(const std::string& title, const std::string& text, const std::string& yesLabel,
                    std::function<void()> onYes, const std::string& noLabel = "Cancel");
    std::function<void()> confirmYes_;
    void askShortcutDelete(int idx);  // Settings > Shortcuts: asks, then deletes
    void askPluginDelete(int idx);    // Settings > Plugins: asks, then removes the plugin
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

    // 3D Autofocus (docs/concepts/AUTOFOCUS_PLAN_2026-11-19.md). Everything here is in memory
    // only: gaze values and offsets are never logged, stored or given to plugins.
    StereoFocus focus_;
    GazeFilter focusGaze_;           // the steadied gaze the renderer measures at
    FocusLog focusLog_;              // Record Autofocus Log (debug, the user switches it on)
    std::string focusLogPath_;       // the video the open log belongs to
    std::string focusLogStoppedPath_; // full / not creatable for this video: not again
    bool focusLogVaultNoticed_ = false;

    // "Free Decoder": Steam's web helper held the hardware decoder, the user agreed to end it.
    // The helper is ended on a background thread (it can take a second); then the video reopens
    // where it was, now with the hardware decoder.
    struct DecoderFree {
        std::shared_ptr<std::atomic<int>> state;  // 0 running, 1 freed, 2 failed
        int slot = 0;
        std::string path;
    };
    std::optional<DecoderFree> decoderFree_;
    std::string decoderFreeAskedPath_;   // asked / freed for this file: not again right after
    std::chrono::steady_clock::time_point decoderFreeAskedAt_{};
    void askFreeDecoder(int slot);
    void pollDecoderFree();
    bool focusSettingPrev_ = false;  // the setting last frame (notice and log on a change)
    bool focusActivePrev_ = false;   // eye gaze was in use last frame (log on a change)
    bool focusOnPicture_ = false;    // the latest measured gaze point landed on the picture
    bool focusUnavailableNoticed_ = false;  // "needs eye tracking, which isn't available" shown
    bool focusSeekWasPending_ = false;      // a seek of slot 0 had not landed last frame
    // What the focus was for last frame: a change (clip, format, swap eyes, a seek) resets it.
    bool focusOpen_ = false;
    std::string focusPath_;
    int focusProjection_ = 0;
    int focusStereo_ = 0;
    bool focusSwap_ = false;
    double focusLastPos_ = -1.0;
    stereo::DisparityResult focusLastResult_;  // the latest measurement (info line only)
    bool focusHaveResult_ = false;
    float focusLastValidDisp_ = 0.0f;
    bool focusHaveValid_ = false;
    std::vector<std::string> focusInfoLines_;  // shown with "Autofocus Info" on, never logged
    std::chrono::steady_clock::time_point focusInfoAt_{};
    void updateStereoFocus(float dtS);
    void resetStereoFocus();
    int64_t colorFormat_ = 0;
    // Menu nav stick auto-repeat: -1/+1 to step (initial press + hold), else 0.
    int navStep(float axisY, std::chrono::steady_clock::time_point now);
    int navStepX(float axisX, std::chrono::steady_clock::time_point now);
    // Up / down hold: when the current hold began, and whether the last step was its first one
    // (the file list leaves for the rail / header only on a fresh press, and pages through after 1.5 s).
    std::chrono::steady_clock::time_point navHoldStart_{};
    bool navLastStepFirst_ = false;
    bool navHeldLong(std::chrono::steady_clock::time_point now) const {
        return !navArmed_ && now - navHoldStart_ >= std::chrono::milliseconds(1500);
    }
    // Laser or buttons: who moves the highlight in lists right now ("last input wins").
    LaserFocus laserFocus_;
    uint64_t laserFocusPageKey_ = 0;  // which page / dialog / folder was up last frame (a new one holds the focus)
    std::string laserFocusLocation_;
    void updateLaserFocus(const XrInputState& in);

    // Button navigation feedback (app/hub_nav.cpp), the same on every menu screen without each
    // screen doing anything: a light tick when a D-pad / stick step moved the focus, a short double
    // tick when it hit the end, and for A the click ring on the focused element plus one standard
    // tick. Screens only have to step with navStep / navStepX and keep their selection in the
    // renderer (see VulkanRenderer::focusedElementCentre).
    bool navStepFired_ = false;  // navStep / navStepX stepped this frame
    std::vector<int> navBefore_;  // the focus before this frame's menu input
    bool navEndTickPending_ = false;
    std::chrono::steady_clock::time_point navEndTickAt_{};
    std::vector<int> navFocusSnapshot() const;
    int navHand() const;          // the hand the ticks go to: the laser hand (one controller: that one)
    void navFocusTick();          // a focus move (hover strength, only with Hover / Focus Rumble on)
    void navEndTick();            // a step at the end of a list (two short ticks)
    void navActivateTick();       // A / trigger acted on something
    // At the start of the menu input: the press ring (laser target or focused element) and tick.
    void navFeedbackBegin(const XrInputState& in, std::chrono::steady_clock::time_point now);
    // After the menu input: the focus tick / end tick for this frame's steps.
    void navFeedbackEnd();

    // Hub focus zones: the rail and (Files) the header row, reached with the D-pad / stick at the
    // content's edge (Left in the first column = rail, Up in the top row = header), Up / Down /
    // Left / Right inside them, A presses, Right / Down / B go back into the content. The page
    // decides where its edge is (moveBrowserFocus, SettingsMenu's section list) and then calls
    // hubNavLeaveContent. Settings' section list is that page's part of the rail: the hub's rail
    // focus hands over to it when it reaches a section entry.
    static constexpr int kNavEdgeNone = 0, kNavEdgeLeft = 1, kNavEdgeUp = 2;
    void hubNavLeaveContent(int edge);
    void takeSettingsRailExit();  // a step off Settings' section list: onto the hub's tabs / Lock Vault
    int hubNavZone_ = 0;     // VulkanRenderer::kNavContent / kNavRail / kNavHeader / kNavCrumb
    int hubNavId_ = -1;      // rail hover id, header button id or crumb index
    int hubNavHoverSeen_ = -1;   // the laser's rail item last frame (a new one moves the focus)
    int hubNavPage_ = -1;        // the tab shown last frame (0 Files, 1 Downloads, 2 Settings)
    // The rail / header used this frame's stick: navStep / navStepX give the content nothing (and
    // keep their repeat state, so a held push does not step again at once). Cleared each frame.
    bool navStickOwned_ = false;
    int headerPress_ = -1;   // A on a header button this frame (read where the laser's clicks are)
    int crumbPress_ = -1;    // ... on a path part
    std::vector<int> railNavItems() const;    // rail hover ids top to bottom
    std::vector<int> headerNavItems() const;  // Files header button ids left to right
    int railEntryId() const;                  // where the focus lands on the rail
    // Moves the focus between zones and inside the rail / header; returns the rail item A pressed
    // (-1 = none). Consumes the stick while the focus is outside the content.
    int updateHubNav(XrInputState& in, std::chrono::steady_clock::time_point now, int sbHover, bool hubActive);
    bool hubNavBack(XrInputState& in);  // B with the focus on the rail / header: back into the content

    // Downloads tab with buttons: a focused card (D-pad / stick up / down, the laser moving onto a
    // card), A = its main action, Y = its Options menu. Stopping a transfer asks first.
    int dlFocus_ = -1;            // index into the jobs
    std::string dlFocusUri_;      // ... and which job that is (indexes move when jobs go)
    int dlFocusPos_ = 0;          // its place in the shown list (kept when the job goes)
    int dlHoverSeen_ = -1;
    std::string dlMenuUri_;       // the job the Options menu was opened for
    void updateDownloadsNav(XrInputState& in, std::chrono::steady_clock::time_point now, int sbHover);
    void downloadPrimaryAction(int job, XrInputState& in);  // Play / Pause / Retry
    void openFinishedDownload(int job, XrInputState& in);
    // `path` is still being downloaded (or is a plain copy in the vault's download folder): not
    // played or opened; a short notice says why (naming no file). True when refused.
    bool refuseUnfinishedDownload(const std::string& path);
    void openDownloadMenu();
    void chooseDownloadMenu(const std::string& opt, XrInputState& in);
    void askStopDownload();       // the running transfer: asks, then stops it (its part is deleted)
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
    int colorDragZone_ = -1;     // colour picker: the square / hue strip being dragged (-1 = none)
    void updateColorPicker(XrInputState& in);  // the laser on the colour picker page
    // Settings with the buttons alone (stick / D-pad, A): returns true when Left / Right changed a
    // value; `act` is that row's action, which the caller applies like a click.
    bool settingsButtons(XrInputState& in, std::chrono::steady_clock::time_point nowT, SettingsMenu::Action& act);
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
    // Options (Y) while choosing a folder: Move here / New folder / Stop moving, the confirm row first.
    void openPickMenu();
    // The file grid, by buttons: A opens / plays the focused entry; the stick / D-pad moves the focus
    // (rows keep their column and stop at its ends, List view pages with left / right, a long hold
    // pages through). Returns kNavEdgeLeft / kNavEdgeUp when a fresh press went past the first
    // column (List view: Left with the list's top in view) or the top row: the rail / header next.
    void activateBrowserFocus();
    int moveBrowserFocus(int dx, int dy, std::chrono::steady_clock::time_point now, bool fresh);
    void openNewFolderKeyboard(const XrInputState& in);
    bool browserScrollDrag_ = false;  // trigger held after grabbing the file list's scrollbar
    // The controller button an action is bound to right now ("A", "Trigger", "Bumpers"), from the
    // live SteamVR bindings; the action's own name when it is not bound to a button we can name.
    std::string buttonName(InputAction a) const;
    // Footer hint (button + what it does), the empty / loading message and the position readout.
    void publishBrowserGuide();
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
        bool savePass = true;          // off: the password is not written to the file (asked when opened)
        // A vault share (the switch is shown only while the vault is unlocked); origVault: it was one
        // when the form opened.
        bool vault = false;
        bool origVault = false;
        enum class Field { None, Host, Share, User, Pass, Workgroup, Port, Label } editingField = Field::None;
    } smbModal_{};

    // Sign-in dialog (page ShareLogin): a share asked for a login when a folder in it was opened.
    // Sign In reads the folder again with the typed login (in memory); only once that worked is it
    // kept as the switch says. The password is wiped when the dialog closes.
    struct ShareLoginState {
        bool active = false;
        std::string uri;  // the folder that asked
        SmbCredentialsManager::LoginTarget target;
        std::string user;
        std::string pass;
        bool savePass = true;
        bool busy = false;          // Sign In pressed: waiting for the folder to be read again
        unsigned waitSerial = 0;    // the browser's scan count when it was pressed
        std::string message;        // the header line (which share, and why it asks)
        std::string playAfter;      // asked by a video that could not open: played once signed in
        enum class Field { None, User, Pass } editingField = Field::None;
    } shareLogin_{};
    void openShareLogin(const std::string& uri, bool refused);
    void updateShareLoginMenu();
    void openShareLoginField(int id, bool chain);
    void navigateShareLogin(int dx, int dy, bool fresh);
    void submitShareLogin();
    void closeShareLogin(bool cancelled);
    void pollShareLogin();  // each frame: a folder that asks for a login, the result of Sign In
    void openSmbModal(bool isNew, const std::string& smbPath = "");
    void updateSmbModalMenu();
    void saveSmbModal(bool confirmed = false);  // confirmed: a saved password may go into the normal list
    // A share became a vault share: what was remembered about it in the open goes (resume points,
    // letterbox results, bookmarks on disk, an SFTP host key).
    void scrubShareTraces(const SmbShare& s);
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
        bool capsLock = false;            // Shift pressed twice: stays on until pressed again
        std::string enterLabel = "Done";  // the key that finishes: "Done", "Rename", "Next", "Enter" (web)
        int peekStart = -1;               // password: the character just typed, shown in clear
        int peekLen = 0;
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

    // --- Dialogs with buttons alone: one step back, the share form, the keyboard's key focus ---
    // A dialog page opened from another one (Format / Delete from File Actions, a question from the
    // share form): B, Cancel, the X and a click beside it go back to that page.
    struct MenuParent {
        VulkanRenderer::ActionMenuData data;
        int childPage = -1;      // the page it was left for
        std::string actionPath;  // pendingActionPath_ of that page
    };
    std::vector<MenuParent> menuStack_;
    void pushMenuParent(int childPage);   // before actionMenu_ turns into childPage
    bool restoreMenuParent(int childPage);  // back to the page childPage was opened from; false = none
    bool menuBack();      // B / Y / the X / a click outside: one step back (a root page closes)
    void cancelConfirm(); // the two-button question's Cancel, wherever it was asked
    // Share form: Cancel / B / X; with anything changed it asks first ("Discard Changes?").
    void closeShareEditor(bool ask);
    bool smbModalChanged() const;
    SmbModalState smbModalOpened_{};  // the form as it opened
    void openSmbField(int id, bool chain);  // the keyboard for field `id`; chain: Enter goes on to the next
    int smbChainNext(int id) const;         // the next field of the chain after `id`, -1 = none
    bool smbChain_ = false;                 // the open keyboard's Enter means "Next"
    float smbNavX_ = 0.0f;                  // share form: the column the stick keeps while going up / down
    void navigateShareEditor(int dx, int dy, bool fresh);
    void navigatePicker(int dx, int dy, bool fresh);
    void navigateArrange(int dx, int dy);
    void stepListSelection(int dy, bool fresh);  // list pages: wraps on a fresh press, stops while held
    int pickerLastTile_ = -1;   // "Play on which screen?": the tile Up returns to from Cancel
    int arrangeColumnSel_ = 0;  // Arrange: the preset / toggle Left returns to
    bool navLastFresh_ = false; // the last navStep / navStepX step was a fresh press (not the repeat)
    // In-app keyboard with buttons: the stick / D-pad move a key focus, A types it, X deletes,
    // Y finishes. The laser takes the focus over once it moves onto another key.
    int kbFocus_ = -1;
    bool kbNavOwns_ = false;
    int kbNavHoverKey_ = -1;    // the key under the laser when the stick last moved the focus
    bool kbRevealSession_ = false;  // "Show" on a password stays chosen for this session (not web fields)
    std::chrono::steady_clock::time_point kbPeekUntil_{};
    static int keyboardKeyWithCode(int code);
    // Notices as a pill on the panel, whatever page shows (fades in and out).
    std::chrono::steady_clock::time_point hubNoticeSince_{};
    void updateNoticeToast(std::chrono::steady_clock::time_point now);

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
        bool subtitle = false;  // a subtitle row (index -1 = off): no reload
        bool unusable = false;  // a picture subtitle track: listed so people see why it can't be picked
        std::string help;       // what this row is, in a sentence (shown under the title while it is selected)
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
        std::vector<BrowserHost::PlexPlay::Marker> plexMarkers;  // intro / credits of what plays (Skip pill)
        int subtitleChoice = -1;        // the subtitle track picked for this file (kept across a track reload)
        bool subtitleLogged = false;    // the first shown line of this choice was logged
        bool subtitleAuto = false;      // nothing chosen for this file: the preferred-subtitles setting decides
#ifdef PIXELVR_HAVE_MEDIA
        // Subtitle files found for this file outside of it (network share, vault, media server): kept
        // to be added again when the file is reopened for a track switch. Memory only.
        std::vector<SubtitleSource> extraSubs;
#endif
        uint64_t subLookupSerial = 0;   // which lookup's answer this screen waits for
        int subWantedServerId = -1;     // the server's external subtitle chosen on the page, until it has arrived
        int subWantedIndex = -1;        // a remembered by-hand external track, until the lookup has added it
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
        // The videos of the folder this file was opened from, in the file browser's order: a bumper tap
        // steps to the previous / next one. Memory only; emptied when the screen closes.
        std::vector<std::string> folderVideos;
        // Stalled stream: neither a new frame nor a moving clock for a while although it plays.
        uint64_t stallFrames = 0;
        double stallPos = -1.0;
        std::chrono::steady_clock::time_point stallSince{};
        bool stalled = false;
        // MediaEngine::decoderChanges() last seen (a retry, a switch to software or giving up),
        // and whether an early end ("the file ends early") was already reported.
        uint32_t seenDecoderChanges = 0;
        bool earlyEndShown = false;
        // Music queue: tracks skipped because they can't be played, said once the next one plays.
        int skippedTracks = 0;
        std::string skippedNote;
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
    // Numbers on the screens while they are unlocked: on when unlocking (which screen is which while
    // moving them), and the dock's numbers button switches them for that time. Locked: the saved setting.
    bool numbersWhileUnlocked_ = true;
    // Single-screen mode: the screen was dragged somewhere by hand (it is always movable there, no
    // lock button). Recenter or closing the video puts it back in front.
    bool singleMoved_ = false;
    float grabOffset_[3] = {0.0f, 0.0f, 0.0f};  // screen centre minus grab point
    void startGrabbingScreen(int slot, const XrInputState& in);
    float grabStart_[3] = {0.0f, 0.0f, 0.0f};  // grabbed screen centre at grab start
    float grabStartScale_ = 1.0f;
    // Single-screen drag: what to go back to when the trigger is let go without a real move (a click).
    VulkanRenderer::ScreenPlacement grabStartPlacement_{};
    bool grabStartSingleMoved_ = false;
    // What Settings had for the screen when it was dragged: a change there puts it back in front.
    float singleSnap_[3] = {0.0f, 0.0f, 0.0f};
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
    // Settings > Screen Size for the single main screen (`resetDistance`: also its distance).
    void applyScreenSize(bool resetDistance);
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
    // The dock stays up while the laser rests on it (it fades 3 s after the laser leaves); a laser
    // resting there untouched lets it go after a while all the same.
    int dockHoverKey_ = -2;
    std::chrono::steady_clock::time_point dockHoverSince_{};
    // Bumper taps on a flat video (no skip pill): previous (-1) / next (+1) episode of a Plex / Jellyfin
    // show, or the previous / next video of the folder. False when a tap means nothing there.
    bool stepVideo(int slot, int dir);
    // What that tap does right now, for the button label ("" = nothing).
    std::string bumperTapText(int dir);
    // The skip pill's text for a screen ("Skip Intro" / "Skip Credits"), or "".
    const char* skipPillLabel(int slot);
    void noteFolderVideos(int slot, const std::string& path);  // fills ScreenState::folderVideos
    void updateStalls(std::chrono::steady_clock::time_point now);  // "Buffering..." on a stalled stream
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
    // Skip Intro / Credits with a TAP on the right bumper: counted on release, and only when the
    // bumper was not used as a shift meanwhile (held long, a stick moved, the other bumper held).
    bool skipTapDown_ = false;
    bool skipTapSpoiled_ = false;
    // The left bumper's tap, by the same rules (previous episode / previous video of the folder).
    bool prevTapDown_ = false;
    bool prevTapSpoiled_ = false;
    // The right stick's gesture: 0 none, 1 up / down (distance, size, zoom), 2 left / right (scrub).
    // The other direction needs a firm push until the stick is back in the middle.
    int stickAxis_ = 0;
    bool panShiftLatched_ = false;       // 360/180: bumper + stick pan lasts until the sticks are centred
    void finishLoadMedia(int slot);
    // Subtitles from outside the file (media/subtitle_sources.hpp): asked for when a file has opened,
    // added to the screen's track list when the worker has them.
    SubtitleFinder subtitleFinder_;
    void startSubtitleLookup(int slot, const BrowserHost::PlexPlay* play);
    void pollSubtitleLookups();
    void addFoundSubtitles(int slot);
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
    // Preferred subtitles (Settings > Audio & Video Tracks): the track the setting picks for what is open
    // on `slot` (-1: none), and the note shown on the screen when it switched one on.
    int preferredSubtitleTrack(int slot);
    void noteAutoSubtitle(int slot, int track);
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

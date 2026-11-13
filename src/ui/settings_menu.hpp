#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace pixelvr {

class PluginHost;
class PluginRepo;

class SettingsMenu {
public:
    enum class Action {
        None,
        Back,
        CycleDownloadDir,
        PickDownloadFolder,   // "Other...": the App lets the user pick a folder in the browser
        EnterDownloadFolder,  // opens the Download Folder page (one option per line, like the track lists)
        PickDownloadPreset,   // a preset on that page (data = index)
        MoveVault,            // Vault location: move the vault to / from the SD card (App, after a 2nd click)
        ToggleRailShortcut0,
        ToggleRailShortcut1,
        ToggleRailShortcut2,
        ToggleRailShortcut3,
        ToggleRailShortcut4,
        ToggleRailShortcut5,
        ToggleRailShortcut6,
        ToggleRailShortcut7,
        // Shortcuts (UI & Appearance > Shortcuts): a list of name + address entries, each
        // editable; row.data carries the shortcut index.
        OpenShortcut,
        AddShortcut,
        EditShortcutName,     // the app opens the keyboard, then calls setShortcutName()
        EditShortcutUrl,      // the app opens the keyboard, then calls setShortcutUrl()
        ToggleShortcutRail,
        CycleShortcutKiosk,
        RemoveShortcut,
        MoveShortcutUp,       // the list order is the rail order
        MoveShortcutDown,
        BackToShortcuts,
        ToggleSwapEyes,
        ToggleModels,
        ToggleStats,
        CycleDistance,
        CycleCurvature,
        CycleUiTextScale,
        CycleUiTextColor,
        CycleUiAccentColor,
        CycleUiDistance,
        CycleUiFollowMode,
        EnterVideoList,
        EnterAudioList,
        EnterLangList,
        PickVideoStream,
        PickAudioStream,
        PickLanguage,
        CycleVaultAutoLockHeadset,
        CycleVaultTimeout,
        RecordVaultCombo,
        EnterListColsConfig,
        EnterRailShortcuts,
        ToggleListColFormat,
        ToggleListColRes,
        ToggleListColSize,
        ToggleListColDuration,
        ToggleHoverHaptics,
        CycleHoverHapticsStrength,
        CycleAmbilight,
        CyclePanelCurve,
        CycleUiOpacity,
        ToggleMultiScreen,
        TogglePauseOnRemoval,
        ToggleSilentScrub,     // stick scrub / skip keeps hidden controls hidden
        ToggleSilentZoom,      // stick distance / zoom / tilt / pan keeps hidden controls hidden
        ToggleSilentRecenter,  // Recenter keeps hidden controls hidden
        CycleKeyboardLayout,
        TogglePauseLivestreams,
        TogglePrivateBrowsing,
        ToggleVaultForceIncognito,
        StartVaultMoveLock,
        StartVaultMoveUnlock,
        CancelVaultMove,
        BackToVault,
        CycleBrowserSort,
        BackToUI,
        CycleAmbilightScope,
        CycleGlowReach,
        CycleLaserSmoothing,
        CycleRoom,
        EnterButtons,       // Controls & Input > Buttons (read-only binding pages)
        ToggleButtonLabels, // Controls & Input > Button Labels (floating labels at the controllers)
        // Plugins (Controls & Input > Plugins); row.data carries the setting index.
        EnterPlugins,
        OpenPlugin,
        TogglePluginEnabled,
        PluginSetting,
        EditPluginText,  // the app opens the keyboard, then calls submitPluginText()
        PluginVaultGrant,
        PluginRestart,
        BackToPlugins,
        BackToControls,
        InstallPluginPackage,
        ToggleRemoveBlackBars,
        InstallRepoPlugin,  // from the plugin repository; the app downloads repoEntryToInstall()
        OpenStore,          // the Install plugins page
        OpenStorePlugin,    // one repository plugin's details
        CycleHeatStyle,  // plugins' timeline graph: bars / line  // a .pvrplugin found in Downloads; the app installs pluginPackageToInstall()
    };

    // How a row's value is presented: the renderer draws a stepped slider for
    // ordered choices, a switch for toggles, swatches for colours, a key chip
    // for bindings, a chevron for rows that open a sub-page, and one pill per
    // option for small unordered choices (Segmented).
    // Shortcut: a line of the shortcut list (stepIdx = on the rail; steps = 1 while Delete waits
    // for its confirming second press; data = the shortcut index).
    enum class Kind { Choice, Toggle, Action, Binding, Color, Option, Segmented, Shortcut, PluginLine, Paragraph, Button };

    struct Row {
        std::string label;
        std::string value;
        Action action = Action::None;
        bool enabled = true;  // disabled rows render greyed and do nothing
        int data = -1;        // payload: stream position or language index
        Kind kind = Kind::Action;
        int steps = 0;        // Choice/Color/Segmented: number of options
        int stepIdx = 0;      // Choice/Color/Segmented: active option; Toggle/Option: 1 = on
        std::vector<std::string> options;  // Segmented: short label per option
    };

    SettingsMenu();

    void open();
    void close();  // also stops recording a new unlock combo
    bool isOpen() const { return open_; }
    void setOpen(bool o) { if (o) { open_ = true; refresh(); } else { close(); } }
    void refresh();

    const std::string& title() const { return title_; }
    // The footer line; on a sub-page the default hint says what B does there.
    const std::string& status() const;
    // One level up from a sub-page (track lists, Shortcuts, a plugin, Install plugins, Move Vault,
    // Controller Buttons). False on a top-level page: B then leaves Settings.
    bool navigateBack();
    int parentSection() const;  // -1 = top level
    int parentOf(int section) const;
    // One target section per " > " part of the title (-1 = not clickable: "Settings" and the page itself).
    std::vector<int> crumbTargets() const;
    // Up the page tree to `section` (a breadcrumb click). False if it is not an ancestor.
    bool navigateUpTo(int section);
    const std::vector<Row>& rows() const { return rows_; }
    int selected() const { return selected_; }
    void setSelected(int s);
    void move(int delta);

    // Activates the currently selected row and returns the requested action.
    Action activate();
    // For a Segmented row: activates it until option `segment` is chosen (so
    // the app sees the same action as a normal click) and returns that action.
    Action activateSegment(int segment);

    // Settings values
    int projectionMode() const { return projectionMode_; } // 0=Flat, 1=360, 2=180
    void setProjectionMode(int m) { projectionMode_ = m; refresh(); }
    int stereoMode() const { return stereoMode_; }         // 0=Mono, 1=SBS, 2=TB, 3=FS3D
    bool removeBlackBars() const { return removeBlackBars_; }  // leave letterbox bars out of screens
    void setStereoMode(int s) { stereoMode_ = s; refresh(); }
    bool swapEyes() const { return swapEyes_; }
    void setSwapEyes(bool s) { swapEyes_ = s; refresh(); }
    bool drawModels() const { return drawModels_; }
    // Floating labels next to the controller buttons (Show Controls toggles them too; saved).
    bool buttonLabels() const { return buttonLabels_; }
    void setButtonLabels(bool on);
    bool showStats() const { return showStats_; }
    // Buttons are assigned in SteamVR only. The app hands over each action's
    // label and its live bound buttons; the Buttons pages show them read-only.
    // `notice` (e.g. essential actions unbound) replaces the pages' hint line.
    void setControlsInfo(std::vector<std::pair<std::string, std::string>> info,
                         std::string notice = std::string());
    void openButtons();               // first Buttons page (Show Controls)
    bool showingButtons() const { return section_ >= kButtonsSection && section_ < kButtonsSection + buttonsPages(); }
    // Buttons pages: the shown page (0-based, -1 elsewhere) and the page count.
    int buttonsPage() const { return showingButtons() ? section_ - kButtonsSection : -1; }
    int buttonsPages() const {
        return std::max(1, (static_cast<int>(controlsInfo_.size()) + kButtonsPerPage - 1) / kButtonsPerPage);
    }
    // Turns the Buttons page by `delta`; wrap = past the last page to the first.
    // Returns false (nothing changed) at an end without wrap, or off the pages.
    bool turnButtonsPage(int delta, bool wrap);
    // settings.conf still had the old press-to-bind keys (no longer written);
    // custom = one of them was not its default.
    bool legacyBindingsSeen() const { return legacyBindingsSeen_; }
    bool legacyBindingsCustom() const { return legacyBindingsCustom_; }
    std::string downloadDir() const;
    // A folder chosen with "Other..." (overrides the presets; "" = a preset).
    void setCustomDownloadDir(const std::string& dir);
    const std::string& customDownloadDir() const { return downloadCustom_; }
    // Vault page status while the vault moves (App; "" = none).
    // A result to show on the Vault page for a while (e.g. why moving the vault failed).
    void setVaultMoveNotice(const std::string& text) {
        vaultMoveNotice_ = text;
        vaultMoveNoticeUntil_ = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        if (section_ == 4 || section_ == kVaultMoveSection) refresh();
    }
    void setVaultMoveStatus(const std::string& text) {
        if (text == vaultMoveStatus_) return;
        vaultMoveStatus_ = text;
        if (section_ == 4 || section_ == kVaultMoveSection) refresh();
    }
    void measureVault();      // starts the walk on a worker (a big vault on an SD card takes a while)
    void pollVaultMeasure();  // frame thread: refreshes the Move Vault page when the walk is done
    ~SettingsMenu();
    // Web shortcuts shown as tabs on the hub's rail (in table order).
    // A web shortcut: name + address; on the hub's rail or not; a page only (kiosk) or the
    // full browser. An empty address is asked for on first use (e.g. a Jellyfin server).
    struct Shortcut {
        std::string name;
        std::string url;
        bool kiosk = true;
        bool onRail = false;
    };
    static constexpr int kMaxRailShortcuts = 5;  // tabs the rail has room for
    struct RailShortcut {
        int index = -1;  // into shortcuts()
        std::string name;
        std::string url;
        bool kiosk = true;
    };
    std::vector<RailShortcut> railShortcuts() const;  // the first kMaxRailShortcuts on the rail
    const std::vector<Shortcut>& shortcuts() const { return shortcuts_; }
    int shortcutIdx() const { return shortcutIdx_; }  // the one on the edit page
    void setShortcutName(int idx, const std::string& name);
    void setShortcutUrl(int idx, const std::string& url);  // normalised (scheme added, Jellyfin /web trimmed)
    void openShortcutsPage(const std::string& hint);  // UI & Appearance > Shortcuts
    // The shortcut list's line controls (the app routes clicks on them here).
    static constexpr int kMaxShortcuts = 10;  // what the list page has room for
    void shortcutToggle(int idx);
    void shortcutMove(int from, int to);  // drag and drop; the list order is the rail order
    bool shortcutDelete(int idx);         // first press asks ("Sure?"), a second one within 4 s deletes
    void shortcutEdit(int idx);           // the shortcut's own page (name, address, ...)
    void tickShortcuts();                 // per frame: an unconfirmed Delete times out
    // Dense line lists: the shortcut list and the plugin list.
    bool compactPage() const {
        return section_ == 13 || section_ == 22 || section_ == kPluginsSection || section_ == kPluginSection || section_ == kStoreSection ||
               section_ == kStoreItemSection || section_ == kVaultMoveSection;
    }
    // A plugin's page turns pages with the header pager when its lines do not fit.
    int pluginPage() const { return section_ == kPluginSection ? pluginPage_ : -1; }
    int pluginPages() const { return section_ == kPluginSection ? pluginPages_ : 0; }
    // A plugin's message (e.g. a device scan): on the plugin pages' status line for a few seconds.
    void showPluginNotice(const std::string& s);
    // Plugin list lines (index into the installed plugins): switch, Edit, Remove (asks once more).
    void pluginToggle(int i);
    void pluginEdit(int i);
    bool pluginRemoveArmed(int i);  // true: confirmed, the app removes it now
    void tickPlugins();
    float screenDistance() const { return screenDistance_; }
    void setScreenDistance(float d) { screenDistance_ = d; refresh(); }
    float screenCurvature() const;
    void setCurvatureIdx(int idx);
    int curvatureIdx() const { return curvatureIdx_; }
    float uiTextScale() const;
    int uiTextColorIdx() const { return uiTextColorIdx_; }
    int uiAccentColorIdx() const { return uiAccentColorIdx_; }
    float uiDistance() const;
    int uiFollowMode() const { return uiFollowMode_; }
    void setUiFollowMode(int m) { uiFollowMode_ = m; saveConfig(); refresh(); }
    // Main panel shape: 0 = flat, 1 = gentle curve, 2 = full wrap.
    int panelCurveIdx() const { return panelCurveIdx_; }
    float panelCurve() const;  // renderer curve amount (0..1)
    // Multi-screen playback (up to 3 flat screens) and whose audio is heard:
    // 0 = only the selected screen, 1 = all screens mixed.
    bool multiScreen() const { return multiScreen_; }
    // Pause all screens (videos and browser pages) when the headset comes off.
    bool pauseOnRemoval() const { return pauseOnRemoval_; }
    // With the player controls hidden: these inputs act without bringing them back.
    bool silentScrub() const { return silentScrub_; }
    bool silentZoom() const { return silentZoom_; }
    bool silentRecenter() const { return silentRecenter_; }
    // In-app keyboard: 0 = English (QWERTY), 1 = German (QWERTZ).
    int keyboardLayout() const { return keyboardLayout_; }
    int laserSmoothing() const { return laserSmoothing_; }  // 0 Off, 1 Low, 2 Medium, 3 High
    // ...including livestreams on browser screens (off: they keep running).
    bool pauseLivestreams() const { return pauseLivestreams_; }
    // New browser screens open in a private (incognito) browser context.
    bool privateBrowsing() const { return privateBrowsing_; }
    // Force non-kiosk browser windows to open in incognito while the vault is unlocked.
    bool vaultForceIncognito() const { return vaultForceIncognito_; }
    // Borders around the screens in multi-screen mode (dock toggle).
    // Multi-screen: a big number on each screen (dock toggle).
    bool screenNumbers() const { return screenNumbers_; }
    void setScreenNumbers(bool on) { screenNumbers_ = on; saveConfig(); }
    // Multi-screen: the selected screen is heard, the others are muted (dock
    // button). Off: every screen plays its own audio (per-screen mute / volume).
    bool audioFollow() const { return audioFollow_; }
    void setAudioFollow(bool on) { audioFollow_ = on; saveConfig(); }
    // Panel style: 0 = Solid, 1 = Frosted, 2 = Glass; uiOpacity() is the glass slab's opacity.
    int uiOpacityIdx() const { return uiOpacityIdx_; }
    int panelStyle() const { return uiOpacityIdx_; }
    float uiOpacity() const;
    // File browser sort order: 0 = name, 1 = newest first, 2 = largest first.
    int browserSort() const { return browserSort_; }
    void setBrowserSort(int s) { browserSort_ = ((s % 3) + 3) % 3; saveConfig(); refresh(); }
    // Browser view mode is changed from the file browser header (not a settings row).
    int browserViewMode() const { return browserViewMode_; }
    void setBrowserViewMode(int m) { browserViewMode_ = m; saveConfig(); refresh(); }
    const std::string& quickPath() const { return quickPath_; }
    void setQuickPath(const std::string& p) { quickPath_ = p; saveConfig(); }

    int section() const { return section_; }
    void setSection(int s) {
        section_ = s;
        selected_ = 0;
        refresh();
    }

    // Track lists (container index + label) for the current media, pushed by the
    // app on load. selected*Stream() return the chosen container index (-1 none).
    void setStreams(const std::vector<std::pair<int, std::string>>& video,
                    const std::vector<std::pair<int, std::string>>& audio,
                    int currentVideo, int currentAudio);
    int selectedVideoStream() const;
    int selectedAudioStream() const;
    void setSelectedAudioStream(int containerIndex);
    void setSelectedVideoStream(int containerIndex);
    std::string audioLanguage() const;  // ISO code of the preferred language, ""

    void loadConfig();
    void saveConfig() const;

    bool listShowFormat() const { return listShowFormat_; }
    bool listShowRes() const { return listShowRes_; }
    bool listShowSize() const { return listShowSize_; }
    bool listShowDuration() const { return listShowDuration_; }

    bool hoverHapticsEnabled() const { return hoverHapticsEnabled_; }
    void setHoverHapticsEnabled(bool e) { hoverHapticsEnabled_ = e; saveConfig(); refresh(); }
    int hoverHapticsStrengthIdx() const { return hoverHapticsStrengthIdx_; }
    float hoverHapticsStrength() const;
    const char* hoverHapticsStrengthStr() const;

    // Ambient screen glow ("ambilight") behind the flat cinema screen.
    int ambilightIdx() const { return ambilightIdx_; }
    float ambilightIntensity() const;  // 0 = off
    float glowReach() const;           // how far the glow reaches, in screen heights
    // Room around the screens (the projection layer's clear colour), RGB.
    const float* roomColor() const;
    int roomIdx() const { return roomIdx_; }  // 0 Midnight, 1 Black, 2 Slate, 3 Warm
    bool ambilightAllScreens() const { return ambilightAllScreens_; }  // else only the selected screen

    // OpenXR swapchain supersampling / render resolution scale.

    void onKeyboardSubmit(const std::string& text) { (void)text; }
    void onKeyboardCancel() {}

    // Plugins page. The host outlives the menu (both belong to App).
    void setPluginHost(PluginHost* host) { plugins_ = host; }
    void setPluginRepo(PluginRepo* repo) { repo_ = repo; }
    // The repository entry the last InstallRepoPlugin row points at (index into repo entries).
    int repoEntryToInstall() const { return repoEntryToInstall_; }
    bool onPluginPage() const {
        return section_ == kPluginsSection || section_ == kPluginSection || section_ == kStoreSection || section_ == kStoreItemSection;
    }  // live status: refresh now and then
    // The package the last InstallPluginPackage row points at; a message for the page's status line.
    const std::string& pluginPackageToInstall() const { return pluginPackageToInstall_; }
    int heatStyle() const { return 1; }  // always the line graph
    // The Plugins page header button ("Install plugins"); "" = none.
    std::string headerButton() const { return section_ == kPluginsSection && repo_ ? "Install plugins" : std::string(); }
    void openPluginStore();               // the header button
    void openPluginUpdate(int pluginIdx);  // the green update icon on a plugin line
    void showStatus(const std::string& s) { status_ = s; }
    void openPluginsPage() {  // after installing a plugin
        section_ = kPluginsSection;
        selected_ = 0;
        open_ = true;
        refresh();
    }
    // After Action::EditPluginText: the text setting being edited and its value.
    int editingPluginSetting() const { return editingPluginSetting_; }
    std::string editingPluginText() const;
    void submitPluginText(const std::string& text);

private:
    PluginHost* plugins_ = nullptr;
    PluginRepo* repo_ = nullptr;
    int repoEntryToInstall_ = -1;
    std::vector<int> repoRows_;  // row data -> repo entry
    int pluginIdx_ = 0;
    std::vector<std::string> pluginPackages_;  // .pvrplugin files found in the download folders
    std::string pluginPackageToInstall_;
    int heatStyle_ = 0;
    int pluginRemoveArmed_ = -1;
    int pluginPage_ = 0, pluginPages_ = 1;
    std::string storeId_;      // the repository plugin shown on the details page
    bool storeUpdate_ = false;  // details page in update mode (changelog)
    bool pluginStatusShown_ = false;  // the plugin pages' status text is up (cleared on other pages)
    std::string pluginNotice_;
    std::chrono::steady_clock::time_point pluginNoticeUntil_{};
    std::chrono::steady_clock::time_point pluginRemoveUntil_{};              // plugin shown on the detail page (kPluginSection)
    int editingPluginSetting_ = -1;
    bool open_ = false;
    std::string title_ = "Settings > Display & Cinema";
    std::string status_ = "Point + Trigger: Change Setting  |  B: Close";
    std::vector<Row> rows_;
    int selected_ = 0;

    int projectionMode_ = 0;
    int stereoMode_ = 0;
    bool removeBlackBars_ = true;
    bool swapEyes_ = false;
    bool drawModels_ = true;   // our models (off: SteamVR draws its own controllers); laser stays
    bool buttonLabels_ = true; // floating button labels (on for new installs: shows what the buttons do)
    bool showStats_ = false;
    float screenDistance_ = 4.0f;  // room for several screens without crowding

    static constexpr int kPluginsSection = 15;  // Controls & Input > Plugins (list)
    static constexpr int kPluginSection = 16;   // one plugin's page
    static constexpr int kStoreSection = 18;    // Install plugins (from the repository)
    static constexpr int kStoreItemSection = 19;  // one repository plugin: about, how to use, install / update
    static constexpr int kVaultMoveSection = 20;  // Vault & Privacy > Move Vault
    static constexpr int kButtonsSection = 23;  // 23.. one section per Buttons page
    static constexpr int kButtonsPerPage = 10;  // all bindings; the header pager turns pages
    std::vector<std::pair<std::string, std::string>> controlsInfo_;  // (action label, bound buttons)
    std::string controlsNotice_;  // Buttons pages: replaces the hint (unbound essentials)
    std::string pageHint_;        // the page's own hint in status_ (reset on page change)
    bool legacyBindingsSeen_ = false;
    bool legacyBindingsCustom_ = false;
    int downloadDirIdx_ = 1;   // index into the download-destination presets (default ~/Downloads)
    std::string downloadCustom_;  // download_dir_custom: a folder picked with "Other..."
    std::chrono::steady_clock::time_point vaultMoveArmedUntil_{};
    bool vaultMoveArmed_ = false;  // the row says "Click again to move it"
    std::string vaultMoveStatus_;
    std::string vaultMoveNotice_;
    std::chrono::steady_clock::time_point vaultMoveNoticeUntil_{};
    std::atomic<uint64_t> vaultSizeBytes_{0};
    std::atomic<int> vaultFileCount_{0};
    std::atomic<bool> vaultMeasuring_{false};  // the folder walk runs on measureThread_
    std::atomic<bool> vaultMeasureDone_{false};
    mutable bool holdSaves_ = false;  // several steps in a row (a direct click on a slider step): saved once
    std::atomic<bool> vaultMeasureStop_{false};
    std::thread measureThread_;
    int railShortcutMask_ = (1 << 0) | (1 << 5);  // kRailShortcuts bits: YouTube + Browser
    int curvatureIdx_ = 0;     // 0=Flat (0%), 1=Gentle (25%), 2=Cinema (50%), 3=Deep (75%), 4=IMAX (100%)
    int uiTextScaleIdx_ = 2;   // 0=90%, 1=105%, 2=120% (default), 3=140%, 4=160%
    int uiTextColorIdx_ = 0;   // 0=Pure White, 1=Ice Cyan, 2=Warm Cream, 3=Soft Gold, 4=Mint Green
    int uiAccentColorIdx_ = 0; // 0=Cyber Cyan, 1=Emerald, 2=Amber Gold, 3=Royal Purple, 4=Crimson
    int uiDistanceIdx_ = 1;    // 0=Near (1.5m), 1=Comfort (1.9m), 2=Mid (2.3m), 3=Far (2.8m)
    int uiFollowMode_ = 0;     // 0=World Lock (default, zero drift/tilt), 1=Smooth Gaze Follow
    int browserViewMode_ = 0;  // 0=Large Cards (3x2), 1=Smaller Cards (4x3), 2=Compact List (1-Col)
    int panelCurveIdx_ = 1;    // 0=Flat, 1=Gentle (default), 2=Wrap
    int uiOpacityIdx_ = 1;     // panel style: 0=Solid, 1=Frosted (default), 2=Glass
    bool multiScreen_ = true; // off = classic single-screen player
    bool pauseOnRemoval_ = true;
    bool silentScrub_ = false;
    bool silentZoom_ = false;
    bool silentRecenter_ = false;
    int keyboardLayout_ = 0;
    int laserSmoothing_ = 2;
    int roomIdx_ = 0;
    bool pauseLivestreams_ = false;
    bool privateBrowsing_ = false;
    bool vaultForceIncognito_ = true;
    bool screenNumbers_ = false;
    std::vector<Shortcut> shortcuts_;  // `shortcut=` lines in settings.conf; defaults when absent
    int shortcutIdx_ = 0;
    int deleteArmedIdx_ = -1;
    std::chrono::steady_clock::time_point deleteArmedUntil_{};
    void ensureShortcuts();  // defaults / migration from rail_shortcuts + jellyfin_url
    std::string legacyJellyfinUrl_;
    bool audioFollow_ = false;
    int browserSort_ = 0;      // 0=Name, 1=Newest, 2=Largest
    bool listShowFormat_ = true;
    bool listShowRes_ = true;
    bool listShowSize_ = true;
    bool listShowDuration_ = true;
    std::string quickPath_;
    bool hoverHapticsEnabled_ = true;
    int hoverHapticsStrengthIdx_ = 1;
    int ambilightIdx_ = 2;     // 0=Off, 1=Subtle, 2=Medium (default), 3=Strong
    bool ambilightAllScreens_ = true;  // multi-screen: glow around every screen
    int glowReachIdx_ = 3;     // 0=Close, 1=Short, 2=Medium, 3=Wide (default, the look before the setting)

    int section_ = 0;   // 0=Display, 1=UI, 2=Audio, 3=Controls, 4=Vault; 10..12, 21, 22, 23..=sub-pages; 15/16=plugins
    std::vector<std::pair<int, std::string>> videoStreams_;  // (index, label)
    std::vector<std::pair<int, std::string>> audioStreams_;
    int videoSel_ = 0;      // position within videoStreams_
    int audioSel_ = 0;      // position within audioStreams_
    int audioLangIdx_ = 0;  // index into the preferred-language table
};

} // namespace pixelvr

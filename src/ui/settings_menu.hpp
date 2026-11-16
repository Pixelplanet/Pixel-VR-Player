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
        // Shortcuts (Files & Web > Shortcuts): a list of name + address entries, each
        // editable; row.data carries the shortcut index.
        OpenShortcut,
        AddShortcut,
        EditShortcutName,     // the app opens the keyboard, then calls setShortcutName()
        EditShortcutUrl,      // the app opens the keyboard, then calls setShortcutUrl()
        ToggleShortcutRail,
        CycleShortcutKiosk,
        RemoveShortcut,
        BackToShortcuts,
        ToggleSwapEyes,
        ToggleModels,
        ToggleStats,
        CycleDistance,
        CycleScreenSize,
        RemovePlugin,         // "Delete Plugin" on a plugin's page: the app asks first
        CycleCurvature,
        CycleUiTextScale,
        OpenUiTextColor,      // the colour picker page for the UI text colour
        OpenUiAccentColor,    // ... and for the accent colour
        CycleUiDistance,
        CycleUiFollowMode,
        EnterVideoList,
        EnterAudioList,
        EnterLangList,
        PickVideoStream,
        PickAudioStream,
        PickLanguage,
        CycleSubtitleMode,     // Subtitles: Off / Foreign parts only / Always
        EnterSubLangList,
        PickSubtitleLanguage,
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
        BackToFilesWeb,     // from Download Folder / List View Columns / Shortcuts
        ToggleMoveShowsUi,  // moving a controller brings faded player controls back
    };

    // How a row's value is presented: the renderer draws a stepped slider for
    // ordered choices, a switch for toggles, swatches for colours, a key chip
    // for bindings, a chevron for rows that open a sub-page, and one pill per
    // option for small unordered choices (Segmented).
    // Shortcut: a line of the shortcut list (stepIdx = on the rail; steps = 1 while Delete waits
    // for its confirming second press; data = the shortcut index).
    // ReadOnly: a line of a table (the Buttons pages): name and value, no control, nothing to press.
    enum class Kind { Choice, Toggle, Action, Binding, Color, Option, Segmented, Shortcut, PluginLine, Paragraph, Button, ReadOnly };

    struct Row {
        std::string label;
        std::string value;
        Action action = Action::None;
        bool enabled = true;  // disabled rows render greyed and do nothing
        int data = -1;        // payload: stream position or language index
        Kind kind = Kind::Action;
        int steps = 0;        // Choice/Color/Segmented: number of options
        int stepIdx = 0;      // Choice/Color/Segmented: active option; Toggle/Option: 1 = on
        std::vector<std::string> options;  // Segmented: short label per option; Choice: each step's name
        std::string reason;  // a disabled row: why it is off (shown instead of its value)
        std::string note;    // shown on the status line while the row is selected
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

    // Settings with the controller's buttons alone (stick / D-pad, A, B). The code is at the end of
    // settings_menu.cpp.
    // Files & Web: download folder, list view columns, shortcuts, private browsing.
    static constexpr int kFilesWebSection = 5;
    // The section list on the rail: Display, UI, Files & Web, Playback & Tracks, Controls, Plugins,
    // Vault (only while it is unlocked).
    int railSectionCount() const;
    int railPosOf(int section) const;  // the rail entry a page belongs to (a sub-page: its section's)
    int railPos() const { return railPosOf(section_); }
    void openRailSection(int pos);     // a click on a rail entry, or A on the marked one
    // The focus is on the section list (Up from the first row): Up / Down mark a section, A or Right
    // opens it. -1 while the focus is on the page.
    bool railFocus() const { return railFocus_; }
    int railFocusPos() const { return railFocus_ ? railFocusPos_ : -1; }
    // The section list is Settings' part of the hub rail. A step that would leave it (Left, Up from
    // the first entry, Down from the last) is reported here once, for the hub to take the focus to
    // the tabs above or Lock Vault below; the focus stays on the list until leaveRail().
    static constexpr int kRailExitNone = 0, kRailExitLeft = 1, kRailExitUp = 2, kRailExitDown = 3;
    int takeRailExit() {
        const int e = railExit_;
        railExit_ = kRailExitNone;
        return e;
    }
    void leaveRail() { railFocus_ = false; }
    void focusRail(int pos);  // the hub hands the focus back onto the list, on entry `pos`
    // Up / Down: the rows in reading order (list pages: the lines that can act); up from the first
    // one onto the section list.
    void navUpDown(int d);
    // Left / Right: one step down / up on a slider, pills or a switch (no wrap: `changed` stays false
    // at an end), the next button of a dialog, a page of a paged list; Left elsewhere moves onto the
    // section list. Returns the row's action when its value changed (the app applies it like a click).
    Action navLeftRight(int d, bool& changed);
    bool pressRail();  // A while the focus is on the section list: opens the marked section
    // A (or a click on a card outside its slider's steps): like activate(), but a slider steps up and
    // stops at the top instead of jumping back to the first step.
    Action press();
    // The laser moved onto a row (another one than before): the focus leaves the section list.
    void notePointer(int hovered);
    // Colour picker with buttons: a focus over its controls (the zones of the renderer's picker:
    // 0..4 suggested colours, 5 square, 6 hue strip, 7 Reset, 8 Back). A on the square or the strip
    // grabs its marker: the stick then moves it until A or B lets go.
    int pickerFocus() const { return colorPickerTarget() >= 0 && !railFocus_ ? pickFocus_ : -1; }
    int pickerGrab() const { return pickGrab_; }
    void pickerNav(int dx, int dy);
    bool pickerPress(int zone);       // true: the page was left (Back)
    void pickerNudge(float x, float y);  // stick deflection (-1..1) while a marker is grabbed
    bool pickerRelease();             // lets go of a grabbed marker (saved); false if none was

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
    void openShortcutsPage(const std::string& hint);  // Files & Web > Shortcuts
    // The shortcut list's line controls (the app routes clicks on them here).
    static constexpr int kMaxShortcuts = 10;  // what the list page has room for
    void shortcutToggle(int idx);
    void shortcutMove(int from, int to);  // drag and drop; the list order is the rail order
    bool shortcutDelete(int idx);         // deletes it (the app asks in the confirm dialog first)
    void shortcutEdit(int idx);           // the shortcut's own page (name, address, ...)
    void tickShortcuts();                 // per frame: an unconfirmed Delete times out
    // Dense line lists: the shortcut list and the plugin list.
    bool compactPage() const {
        return section_ == 13 || section_ == 22 || section_ == kPluginsSection || section_ == kPluginSection || section_ == kStoreSection ||
               section_ == kStoreItemSection || section_ == kVaultMoveSection;
    }
    // A plugin's page turns pages with the header pager when its lines do not fit.
    // (Also the track / language lists, which page the same way when they are long.)
    int pluginIndex() const { return pluginIdx_; }  // the plugin whose page is open
    int pluginPage() const { return section_ == kPluginSection ? pluginPage_ : (listPages_ > 1 ? listPage_ : -1); }
    int pluginPages() const { return section_ == kPluginSection ? pluginPages_ : (listPages_ > 1 ? listPages_ : 0); }
    // A plugin's message (e.g. a device scan): on the plugin pages' status line for a few seconds.
    void showPluginNotice(const std::string& s);
    // Plugin list lines (index into the installed plugins): switch, Edit, Remove (asks once more).
    void pluginToggle(int i);
    void pluginEdit(int i);
    bool pluginRemoveArmed(int i);  // true: confirmed, the app removes it now
    void tickPlugins();
    float screenDistance() const { return screenDistance_; }
    int screenSizeIdx() const { return screenSizeIdx_; }  // 0 Standard, 1 Large, 2 Cinema, 3 Original Pixels
    void setScreenDistance(float d) { screenDistance_ = d; refresh(); }
    float screenCurvature() const;
    void setCurvatureIdx(int idx);
    int curvatureIdx() const { return curvatureIdx_; }
    float uiTextScale() const;
    // The UI's text and accent colour as 0xRRGGBB (never too dark for the panels: ui/ui_colors.hpp).
    uint32_t uiTextRgb() const { return uiTextRgb_; }
    uint32_t uiAccentRgb() const { return uiAccentRgb_; }
    // The colour picker pages (UI & Appearance > Text Color / Accent Color). They have no rows:
    // the renderer draws the picker, the app routes the laser to the calls below.
    static constexpr int kTextColorSection = 8;
    static constexpr int kAccentColorSection = 9;
    int colorPickerTarget() const {  // 0 = text, 1 = accent, -1 = not on a picker page
        return section_ == kTextColorSection ? 0 : (section_ == kAccentColorSection ? 1 : -1);
    }
    // Where the picker's markers are: hue, saturation, brightness (0..1), as the user set them.
    float pickerHue() const { return pickH_; }
    float pickerSat() const { return pickS_; }
    float pickerVal() const { return pickV_; }
    // The spot the markers are on is too dark for the panels: the UI uses a brightened colour.
    bool pickerAdjusted() const { return pickAdjusted_; }
    // While dragging: the colour changes at once (live preview), the file is written by pickerCommit().
    void pickerSetHue(float h);
    void pickerSetSatVal(float s, float v);
    void pickerCommit();           // the drag ended: save
    void pickerPickSwatch(int i);  // one of the suggested colours (saved)
    void pickerReset();            // the default colour (saved)
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
    // Moving a controller brings faded player controls back (off: only buttons, stick and trigger do).
    bool moveShowsUi() const { return moveShowsUi_; }
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
    // Preferred subtitles for a file nothing was chosen for: 0 = off, 1 = foreign-language parts only
    // (a "forced" track), 2 = always (a full track); and their language (ISO code, "" = same as the audio).
    int subtitleMode() const { return subtitleMode_; }
    std::string subtitleLanguage() const;

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
    std::string headerButton() const { return section_ == kPluginsSection && repo_ ? "Install Plugins" : std::string(); }
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
    int pluginRemoveArmed_ = -1;
    int pluginPage_ = 0, pluginPages_ = 1;
    // Long lists (video / audio tracks, languages, the plugin list, the plugin store): pages of 12
    // lines, a Back row / button staying on every page.
    void paginateList();
    int listPage_ = 0, listPages_ = 1;
    int pluginsListPage_ = 0, storeListPage_ = 0;  // these two lists reopen on the page they were on
    int listPageSection_ = -1;  // the list the page belongs to (a new list opens at its active option)
    std::string storeId_;      // the repository plugin shown on the details page
    bool storeUpdate_ = false;  // details page in update mode (changelog)
    bool pluginStatusShown_ = false;  // the plugin pages' status text is up (cleared on other pages)
    std::string pluginNotice_;
    std::chrono::steady_clock::time_point pluginNoticeUntil_{};
    std::chrono::steady_clock::time_point pluginRemoveUntil_{};              // plugin shown on the detail page (kPluginSection)
    int editingPluginSetting_ = -1;
    bool open_ = false;
    std::string title_ = "Settings > Display & Cinema";
    std::string status_ = "Up / Down: Select  |  Left / Right or A: Change  |  B: Close";  // = kDefaultStatus
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
    int shortcutDefaultsSeen_ = 8;  // how many built-in shortcuts this list was offered (8 = before the key existed)
    int curvatureIdx_ = 0;     // 0=Flat (0%), 1=Gentle (25%), 2=Cinema (50%), 3=Deep (75%), 4=IMAX (100%)
    int uiTextScaleIdx_ = 2;   // 0=90%, 1=105%, 2=120% (default), 3=140%, 4=160%
    uint32_t uiTextRgb_ = 0xF7FAFF;    // ui_text_rgb (default: Pure White)
    uint32_t uiAccentRgb_ = 0x40C7FF;  // ui_accent_rgb (default: Cyber Cyan)
    float pickH_ = 0.0f, pickS_ = 0.0f, pickV_ = 1.0f;  // the picker's markers
    bool pickAdjusted_ = false;
    bool pickDirty_ = false;  // changed by a drag, not saved yet
    int pickFocus_ = 0;       // the picker control with the button focus
    int pickGrab_ = -1;       // the square / hue strip the stick moves (-1 none)
    std::chrono::steady_clock::time_point pickNudgeAt_{};
    bool railFocus_ = false;  // the focus is on the section list
    int railFocusPos_ = 0;
    int railExit_ = kRailExitNone;  // takeRailExit()
    int lastPointerRow_ = -1;
    void enterRail();
    void fixListSelection();  // list pages: the selection on a line that can act
    void pickerLoad();        // markers from the current colour (on opening the page)
    void pickerApply();       // the colour from the markers
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
    bool moveShowsUi_ = true;
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
    int screenSizeIdx_ = 0;
    int glowReachIdx_ = 3;     // 0=Close, 1=Short, 2=Medium, 3=Wide (default, the look before the setting)

    int section_ = 0;   // 0=Display, 1=UI, 2=Playback & Tracks, 3=Controls, 4=Vault, 5=Files & Web; 8, 9, 10..12, 13, 17, 21, 22, 23..=sub-pages; 15/16=plugins
    std::vector<std::pair<int, std::string>> videoStreams_;  // (index, label)
    std::vector<std::pair<int, std::string>> audioStreams_;
    int videoSel_ = 0;      // position within videoStreams_
    int audioSel_ = 0;      // position within audioStreams_
    int audioLangIdx_ = 0;  // index into the preferred-language table
    int subtitleMode_ = 0;  // 0 = Off, 1 = Foreign parts only, 2 = Always
    int subLangIdx_ = 0;    // index into kSubtitleLanguages (0 = same as audio)
    bool subLangList_ = false;  // the language list page (12) shows the subtitle languages
};

} // namespace pixelvr

#pragma once

#include <string>
#include <utility>
#include <vector>

namespace pixelvr {

class SettingsMenu {
public:
    enum class Action {
        None,
        Back,
        ListenPlay,
        ListenMenu,
        ListenRecenter,
        CycleDownloadDir,
        ToggleSwapEyes,
        ToggleUiLayer,
        ToggleModels,
        ToggleStats,
        CycleDistance,
        CycleCurvature,
        CycleUiTextScale,
        CycleUiTextColor,
        CycleUiAccentColor,
        CycleUiDistance,
        CycleUiFollowMode,
        ListenQuickPath,
        EnterVideoList,
        EnterAudioList,
        EnterLangList,
        PickVideoStream,
        PickAudioStream,
        PickLanguage,
        CycleVaultAutoLockHeadset,
        CycleVaultTimeout,
        ListenVaultMoveBtn,
        RecordVaultCombo,
        EnterListColsConfig,
        ToggleListColFormat,
        ToggleListColRes,
        ToggleListColSize,
        ToggleListColDuration,
        ToggleHoverHaptics,
        CycleHoverHapticsStrength,
        CycleAmbilight,
        CycleResScale,
        CyclePanelCurve,
        CycleUiOpacity,
        ToggleMultiScreen,
        TogglePauseOnRemoval,
        CycleKeyboardLayout,
        TogglePauseLivestreams,
        TogglePrivateBrowsing,
        CycleBrowserSort,
        BackToUI,
    };

    // How a row's value is presented: the renderer draws a stepped slider for
    // ordered choices, a switch for toggles, swatches for colours, a key chip
    // for bindings, a chevron for rows that open a sub-page, and one pill per
    // option for small unordered choices (Segmented).
    enum class Kind { Choice, Toggle, Action, Binding, Color, Option, Segmented };

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
    void close() { open_ = false; }
    bool isOpen() const { return open_; }
    void setOpen(bool o) { open_ = o; if (o) refresh(); }
    void refresh();

    const std::string& title() const { return title_; }
    const std::string& status() const { return status_; }
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
    int stereoMode() const { return stereoMode_; }         // 0=Mono, 1=SBS, 2=TB
    void setStereoMode(int s) { stereoMode_ = s; refresh(); }
    bool swapEyes() const { return swapEyes_; }
    bool uiLayer() const { return uiLayer_; }
    void setSwapEyes(bool s) { swapEyes_ = s; refresh(); }
    bool drawModels() const { return drawModels_; }
    bool showStats() const { return showStats_; }
    int bindPlay() const { return bindPlayBtn_; }
    int bindMenu() const { return bindMenuBtn_; }
    int bindRecenter() const { return bindRecenterBtn_; }
    bool isListening() const { return listeningFunc_ >= 0; }
    void assignListening(int button);
    std::string downloadDir() const;
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
    // In-app keyboard: 0 = English (QWERTY), 1 = German (QWERTZ).
    int keyboardLayout() const { return keyboardLayout_; }
    // ...including livestreams on browser screens (off: they keep running).
    bool pauseLivestreams() const { return pauseLivestreams_; }
    // New browser screens open in a private (incognito) browser context.
    bool privateBrowsing() const { return privateBrowsing_; }
    // Borders around the screens in multi-screen mode (dock toggle).
    bool screenFrames() const { return screenFrames_; }
    void setScreenFrames(bool on) { screenFrames_ = on; saveConfig(); }
    // Multi-screen: the selected screen is heard, the others are muted (dock
    // button). Off: every screen plays its own audio (per-screen mute / volume).
    bool audioFollow() const { return audioFollow_; }
    void setAudioFollow(bool on) { audioFollow_ = on; saveConfig(); }
    // Panel transparency: 0 = solid .. 3 = clear; uiOpacity() is the renderer factor.
    int uiOpacityIdx() const { return uiOpacityIdx_; }
    float uiOpacity() const;
    // File browser sort order: 0 = name, 1 = newest first, 2 = largest first.
    int browserSort() const { return browserSort_; }
    void setBrowserSort(int s) { browserSort_ = ((s % 3) + 3) % 3; saveConfig(); refresh(); }
    // Browser view mode is changed from the file browser header (not a settings row).
    int browserViewMode() const { return browserViewMode_; }
    void setBrowserViewMode(int m) { browserViewMode_ = m; saveConfig(); refresh(); }
    int bindQuickPath() const { return bindQuickPathBtn_; }
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

    // OpenXR swapchain supersampling / render resolution scale.
    int resScaleIdx() const { return resScaleIdx_; }
    float resScale() const;
    const char* resScaleStr() const;
    void setResScaleIdx(int idx);

    void onKeyboardSubmit(const std::string& text) { (void)text; }
    void onKeyboardCancel() {}

private:
    bool open_ = false;
    std::string title_ = "Settings > Display & Cinema";
    std::string status_ = "Point + Trigger: Change Setting  |  B: Close";
    std::vector<Row> rows_;
    int selected_ = 0;

    int projectionMode_ = 0;
    int stereoMode_ = 0;
    bool swapEyes_ = false;
    bool uiLayer_ = true;      // UI drawn as a compositor layer (sharp) vs. into the eye buffers
    bool drawModels_ = false;  // off: SteamVR draws the real controllers; laser stays
    bool showStats_ = false;
    float screenDistance_ = 3.0f;

    // Function -> physical button (XrInputState::Button index); press-to-bind.
    int bindPlayBtn_ = 0;      // default A
    int bindMenuBtn_ = 1;      // default B
    int bindRecenterBtn_ = 6;  // default Bumper (grip toggles the laser)
    int bindQuickPathBtn_ = 7; // default Right Stick Click (BtnThumbR)
    int listeningFunc_ = -1;   // 0=play 1=menu 2=recenter 3=move 4=quick path
    int downloadDirIdx_ = 0;   // index into the download-destination presets
    int curvatureIdx_ = 0;     // 0=Flat (0%), 1=Gentle (25%), 2=Cinema (50%), 3=Deep (75%), 4=IMAX (100%)
    int uiTextScaleIdx_ = 2;   // 0=90%, 1=105%, 2=120% (default), 3=140%, 4=160%
    int uiTextColorIdx_ = 0;   // 0=Pure White, 1=Ice Cyan, 2=Warm Cream, 3=Soft Gold, 4=Mint Green
    int uiAccentColorIdx_ = 0; // 0=Cyber Cyan, 1=Emerald, 2=Amber Gold, 3=Royal Purple, 4=Crimson
    int uiDistanceIdx_ = 1;    // 0=Near (1.5m), 1=Comfort (1.9m), 2=Mid (2.3m), 3=Far (2.8m)
    int uiFollowMode_ = 0;     // 0=World Lock (default, zero drift/tilt), 1=Smooth Gaze Follow
    int browserViewMode_ = 0;  // 0=Large Cards (3x2), 1=Smaller Cards (4x3), 2=Compact List (1-Col)
    int panelCurveIdx_ = 1;    // 0=Flat, 1=Gentle (default), 2=Wrap
    int uiOpacityIdx_ = 2;     // 0=Solid, 1=Frosted, 2=Glass (default), 3=Clear
    bool multiScreen_ = false; // off = classic single-screen player
    bool pauseOnRemoval_ = true;
    int keyboardLayout_ = 0;
    bool pauseLivestreams_ = false;
    bool privateBrowsing_ = false;
    bool screenFrames_ = true;
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
    int resScaleIdx_ = 4;      // 0=1.00x Panel 2160p, 1=1.15x Enhanced, 2=1.25x Balanced, 3=1.40x Ultra 3024p, 4=1.50x Extreme 3240p (default)

    int section_ = 0;   // 0=Display, 1=UI, 2=Audio, 3=Controls, 4=Vault; 10..12=sub-lists
    std::vector<std::pair<int, std::string>> videoStreams_;  // (index, label)
    std::vector<std::pair<int, std::string>> audioStreams_;
    int videoSel_ = 0;      // position within videoStreams_
    int audioSel_ = 0;      // position within audioStreams_
    int audioLangIdx_ = 0;  // index into the preferred-language table
};

} // namespace pixelvr

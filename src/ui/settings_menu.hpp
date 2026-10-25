#pragma once

#include <string>
#include <utility>
#include <vector>

namespace pixelvr {

class SettingsMenu {
public:
    enum class Action {
        None,
        EnterDisplay,
        EnterControls,
        EnterStreams,
        EnterSystem,
        Back,
        ListenPlay,
        ListenMenu,
        ListenRecenter,
        CycleDownloadDir,
        CycleProjection,
        CycleStereo,
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
        CycleBrowserViewMode,
        ListenQuickPath,
        EnterVideoList,
        EnterAudioList,
        EnterLangList,
        PickVideoStream,
        PickAudioStream,
        PickLanguage,
        RecenterScreen,
        CycleVaultAutoLockHeadset,
        CycleVaultTimeout,
        ListenVaultMoveBtn,
        RecordVaultCombo,
        LockVaultAction,
        EnterListColsConfig,
        ToggleListColFormat,
        ToggleListColRes,
        ToggleListColSize,
        ToggleListColDuration,
        ToggleHoverHaptics,
        CycleHoverHapticsStrength,
        CycleAmbilight,
        BackToUI,
        Close
    };

    struct Row {
        std::string label;
        std::string value;
        Action action = Action::None;
        bool enabled = true;  // disabled rows render greyed and do nothing
        int data = -1;        // payload: stream position or language index
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

    // Settings values
    int projectionMode() const { return projectionMode_; } // 0=Flat, 1=360, 2=180
    void setProjectionMode(int m) { projectionMode_ = m; refresh(); }
    int stereoMode() const { return stereoMode_; }         // 0=Mono, 1=SBS, 2=TB
    void setStereoMode(int s) { stereoMode_ = s; refresh(); }
    bool swapEyes() const { return swapEyes_; }
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

    void onKeyboardSubmit(const std::string& text) { (void)text; }
    void onKeyboardCancel() {}

private:
    bool open_ = false;
    std::string title_ = "Settings & Controller Remap";
    std::string status_ = "Point + Trigger: Change Setting | Grip: Recenter | B/Y: Close";
    std::vector<Row> rows_;
    int selected_ = 0;

    int projectionMode_ = 0;
    int stereoMode_ = 0;
    bool swapEyes_ = false;
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
    bool listShowFormat_ = true;
    bool listShowRes_ = true;
    bool listShowSize_ = true;
    bool listShowDuration_ = true;
    std::string quickPath_;
    bool hoverHapticsEnabled_ = true;
    int hoverHapticsStrengthIdx_ = 1;
    int ambilightIdx_ = 2;     // 0=Off, 1=Subtle, 2=Medium (default), 3=Strong

    int section_ = 0;   // 0=Display, 1=UI, 2=Audio, 3=Controls, 4=Vault; 10..12=sub-lists
    std::vector<std::pair<int, std::string>> videoStreams_;  // (index, label)
    std::vector<std::pair<int, std::string>> audioStreams_;
    int videoSel_ = 0;      // position within videoStreams_
    int audioSel_ = 0;      // position within audioStreams_
    int audioLangIdx_ = 0;  // index into the preferred-language table
};

} // namespace pixelvr

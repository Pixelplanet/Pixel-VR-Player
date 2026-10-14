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
        CyclePlayBinding,
        CycleMenuBinding,
        CycleSelectBinding,
        CycleRecenterBinding,
        CycleProjection,
        CycleStereo,
        ToggleSwapEyes,
        ToggleModels,
        ToggleStats,
        CycleDistance,
        CycleVideoStream,
        CycleAudioStream,
        CycleLanguage,
        RecenterScreen,
        Close
    };

    struct Row {
        std::string label;
        std::string value;
        Action action = Action::None;
    };

    SettingsMenu();

    void open();
    void close() { open_ = false; }
    bool isOpen() const { return open_; }
    void setOpen(bool o) { open_ = o; if (o) refresh(); }

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
    float screenDistance() const { return screenDistance_; }
    void setScreenDistance(float d) { screenDistance_ = d; refresh(); }

    // Track lists (container index + label) for the current media, pushed by the
    // app on load. selected*Stream() return the chosen container index (-1 none).
    void setStreams(const std::vector<std::pair<int, std::string>>& video,
                    const std::vector<std::pair<int, std::string>>& audio,
                    int currentVideo, int currentAudio);
    int selectedVideoStream() const;
    int selectedAudioStream() const;
    std::string audioLanguage() const;  // ISO code of the preferred language, ""

    void loadConfig();
    void saveConfig() const;

private:
    void refresh();

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

    // Configurable binding labels
    int bindPlayIdx_ = 0;      // 0: A/X, 1: Trigger, 2: Grip, 3: ThumbClick
    int bindMenuIdx_ = 0;      // 0: B/Y, 1: Menu Button, 2: ThumbClick
    int bindSelectIdx_ = 0;    // 0: Trigger, 1: A/X
    int bindRecenterIdx_ = 0;  // 0: Grip, 1: ThumbClick, 2: B/Y

    int section_ = -1;  // -1 = top-level section list; 0..3 = a section's rows
    std::vector<std::pair<int, std::string>> videoStreams_;  // (index, label)
    std::vector<std::pair<int, std::string>> audioStreams_;
    int videoSel_ = 0;      // position within videoStreams_
    int audioSel_ = 0;      // position within audioStreams_
    int audioLangIdx_ = 0;  // index into the preferred-language table
};

} // namespace pixelvr

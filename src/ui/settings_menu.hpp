#pragma once

#include <string>
#include <vector>

namespace pixelvr {

class SettingsMenu {
public:
    enum class Action {
        None,
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
    bool drawModels_ = true;  // app-drawn controller models (capsule at aim pose)
    bool showStats_ = false;
    float screenDistance_ = 3.0f;

    // Configurable binding labels
    int bindPlayIdx_ = 0;      // 0: A/X, 1: Trigger, 2: Grip, 3: ThumbClick
    int bindMenuIdx_ = 0;      // 0: B/Y, 1: Menu Button, 2: ThumbClick
    int bindSelectIdx_ = 0;    // 0: Trigger, 1: A/X
    int bindRecenterIdx_ = 0;  // 0: Grip, 1: ThumbClick, 2: B/Y
};

} // namespace pixelvr

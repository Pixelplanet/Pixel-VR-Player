#include "ui/settings_menu.hpp"

#include "util/logging.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace pixelvr {

namespace {

const char* kPlayOptions[] = {"A / X Button", "Trigger", "Grip", "Thumbstick Click"};
const char* kMenuOptions[] = {"B / Y Button", "Menu Button", "Thumbstick Click"};
const char* kSelectOptions[] = {"Trigger", "A / X Button"};
const char* kRecenterOptions[] = {"Grip", "Thumbstick Click", "B / Y Button"};

std::string getConfigPath() {
#ifdef _WIN32
    const char* appData = std::getenv("APPDATA");
    if (appData) {
        return std::string(appData) + "/PixelVR/settings.conf";
    }
#else
    const char* home = std::getenv("HOME");
    if (home) {
        return std::string(home) + "/.config/pixelvr/settings.conf";
    }
#endif
    return "pixelvr_settings.conf";
}

} // namespace

SettingsMenu::SettingsMenu() {
    loadConfig();
    refresh();
}

void SettingsMenu::open() {
    open_ = true;
    refresh();
}

void SettingsMenu::setSelected(int s) {
    const int n = static_cast<int>(rows_.size());
    selected_ = (n <= 0) ? 0 : (s < 0 ? 0 : (s >= n ? n - 1 : s));
}

void SettingsMenu::move(int delta) {
    setSelected(selected_ + delta);
}

void SettingsMenu::refresh() {
    rows_.clear();

    const char* projNames[] = {"Flat Screen", "360 Equirectangular", "180 Equirectangular"};
    const char* stereoNames[] = {"2D Mono", "Side-by-Side (SBS)", "Top-Bottom (OU)"};

    std::ostringstream distStream;
    distStream << std::fixed;
    distStream.precision(1);
    distStream << screenDistance_ << " m";

    rows_.push_back({"Play / Pause Binding", kPlayOptions[bindPlayIdx_ % 4], Action::CyclePlayBinding});
    rows_.push_back({"Menu / Browser Binding", kMenuOptions[bindMenuIdx_ % 3], Action::CycleMenuBinding});
    rows_.push_back({"Select / Click Binding", kSelectOptions[bindSelectIdx_ % 2], Action::CycleSelectBinding});
    rows_.push_back({"Recenter View Binding", kRecenterOptions[bindRecenterIdx_ % 3], Action::CycleRecenterBinding});
    rows_.push_back({"Projection Mode", projNames[projectionMode_ % 3], Action::CycleProjection});
    rows_.push_back({"Stereo 3D Format", stereoNames[stereoMode_ % 3], Action::CycleStereo});
    rows_.push_back({"Swap Left/Right Eye", swapEyes_ ? "On" : "Off", Action::ToggleSwapEyes});
    rows_.push_back({"Controller Models", drawModels_ ? "On" : "Off", Action::ToggleModels});
    rows_.push_back({"Debug Statistics", showStats_ ? "On" : "Off", Action::ToggleStats});
    rows_.push_back({"Screen Distance", distStream.str(), Action::CycleDistance});
    rows_.push_back({"Recenter Screen View", "[ Trigger to Recenter ]", Action::RecenterScreen});
    rows_.push_back({"Close Settings", "[ Back to Player ]", Action::Close});
}

SettingsMenu::Action SettingsMenu::activate() {
    if (selected_ < 0 || selected_ >= static_cast<int>(rows_.size())) {
        return Action::None;
    }
    const Action act = rows_[selected_].action;
    switch (act) {
        case Action::CyclePlayBinding:
            bindPlayIdx_ = (bindPlayIdx_ + 1) % 4;
            saveConfig();
            refresh();
            break;
        case Action::CycleMenuBinding:
            bindMenuIdx_ = (bindMenuIdx_ + 1) % 3;
            saveConfig();
            refresh();
            break;
        case Action::CycleSelectBinding:
            bindSelectIdx_ = (bindSelectIdx_ + 1) % 2;
            saveConfig();
            refresh();
            break;
        case Action::CycleRecenterBinding:
            bindRecenterIdx_ = (bindRecenterIdx_ + 1) % 3;
            saveConfig();
            refresh();
            break;
        case Action::CycleProjection:
            projectionMode_ = (projectionMode_ + 1) % 3;
            saveConfig();
            refresh();
            break;
        case Action::CycleStereo:
            stereoMode_ = (stereoMode_ + 1) % 3;
            saveConfig();
            refresh();
            break;
        case Action::ToggleSwapEyes:
            swapEyes_ = !swapEyes_;
            saveConfig();
            refresh();
            break;
        case Action::ToggleModels:
            drawModels_ = !drawModels_;
            saveConfig();
            refresh();
            break;
        case Action::ToggleStats:
            showStats_ = !showStats_;
            saveConfig();
            refresh();
            break;
        case Action::CycleDistance:
            screenDistance_ += 0.5f;
            if (screenDistance_ > 5.0f) screenDistance_ = 1.5f;
            saveConfig();
            refresh();
            break;
        case Action::RecenterScreen:
            break;
        case Action::Close:
            close();
            break;
        default:
            break;
    }
    return act;
}

void SettingsMenu::loadConfig() {
    const std::string path = getConfigPath();
    std::ifstream file(path);
    if (!file.is_open()) {
        return;
    }
    std::string line;
    while (std::getline(file, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        if (key == "projection") projectionMode_ = std::clamp(std::stoi(val), 0, 2);
        else if (key == "stereo") stereoMode_ = std::clamp(std::stoi(val), 0, 2);
        else if (key == "swap_eyes") swapEyes_ = (val == "1");
        else if (key == "draw_models") drawModels_ = (val == "1");
        else if (key == "show_stats") showStats_ = (val == "1");
        else if (key == "distance") screenDistance_ = std::clamp(std::stof(val), 1.0f, 10.0f);
        else if (key == "bind_play") bindPlayIdx_ = std::clamp(std::stoi(val), 0, 3);
        else if (key == "bind_menu") bindMenuIdx_ = std::clamp(std::stoi(val), 0, 2);
        else if (key == "bind_select") bindSelectIdx_ = std::clamp(std::stoi(val), 0, 1);
        else if (key == "bind_recenter") bindRecenterIdx_ = std::clamp(std::stoi(val), 0, 2);
    }
    PIXELVR_LOG_INFO("Loaded settings from %s", path.c_str());
}

void SettingsMenu::saveConfig() const {
    const std::string path = getConfigPath();
    try {
        std::filesystem::path p(path);
        if (p.has_parent_path()) {
            std::filesystem::create_directories(p.parent_path());
        }
        std::ofstream file(path);
        if (file.is_open()) {
            file << "projection=" << projectionMode_ << "\n";
            file << "stereo=" << stereoMode_ << "\n";
            file << "swap_eyes=" << (swapEyes_ ? 1 : 0) << "\n";
            file << "draw_models=" << (drawModels_ ? 1 : 0) << "\n";
            file << "show_stats=" << (showStats_ ? 1 : 0) << "\n";
            file << "distance=" << screenDistance_ << "\n";
            file << "bind_play=" << bindPlayIdx_ << "\n";
            file << "bind_menu=" << bindMenuIdx_ << "\n";
            file << "bind_select=" << bindSelectIdx_ << "\n";
            file << "bind_recenter=" << bindRecenterIdx_ << "\n";
        }
    } catch (const std::exception& e) {
        PIXELVR_LOG_WARN("Could not save settings: %s", e.what());
    }
}

} // namespace pixelvr

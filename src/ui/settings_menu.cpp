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

const char* kLangNames[] = {"Auto",    "English",  "German",  "French",
                           "Spanish", "Japanese", "Italian", "Russian",
                           "Chinese", "Korean"};
const char* kLangCodes[] = {"",    "eng", "deu", "fra", "spa",
                           "jpn", "ita", "rus", "chi", "kor"};
constexpr int kLangCount = 10;

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

    if (section_ < 0) {  // top-level section list
        title_ = "Settings";
        rows_.push_back({"Display & Projection", ">", Action::EnterDisplay});
        rows_.push_back({"Controls & Bindings", ">", Action::EnterControls});
        rows_.push_back({"Audio / Video Tracks", ">", Action::EnterStreams});
        rows_.push_back({"System & Debug", ">", Action::EnterSystem});
        rows_.push_back({"Recenter Screen View", "[ Trigger to Recenter ]",
                         Action::RecenterScreen});
        rows_.push_back({"Close Settings", "[ Back to Player ]", Action::Close});
        return;
    }

    if (section_ == 0) {
        title_ = "Display & Projection";
        const char* projNames[] = {"Flat Screen", "360 Equirectangular",
                                   "180 Equirectangular"};
        const char* stereoNames[] = {"2D Mono", "Side-by-Side (SBS)",
                                     "Top-Bottom (OU)"};
        std::ostringstream distStream;
        distStream << std::fixed;
        distStream.precision(1);
        distStream << screenDistance_ << " m";
        rows_.push_back({"Projection Mode", projNames[projectionMode_ % 3],
                         Action::CycleProjection});
        rows_.push_back({"Stereo 3D Format", stereoNames[stereoMode_ % 3],
                         Action::CycleStereo});
        rows_.push_back({"Swap Left/Right Eye", swapEyes_ ? "On" : "Off",
                         Action::ToggleSwapEyes});
        rows_.push_back({"Screen Distance", distStream.str(),
                         Action::CycleDistance});
    } else if (section_ == 1) {
        title_ = "Controls & Bindings";
        rows_.push_back({"Play / Pause Binding", kPlayOptions[bindPlayIdx_ % 4],
                         Action::CyclePlayBinding});
        rows_.push_back({"Menu / Browser Binding", kMenuOptions[bindMenuIdx_ % 3],
                         Action::CycleMenuBinding});
        rows_.push_back({"Select / Click Binding",
                         kSelectOptions[bindSelectIdx_ % 2],
                         Action::CycleSelectBinding});
        rows_.push_back({"Recenter View Binding",
                         kRecenterOptions[bindRecenterIdx_ % 3],
                         Action::CycleRecenterBinding});
        rows_.push_back({"Controller Models", drawModels_ ? "On" : "Off",
                         Action::ToggleModels});
    } else if (section_ == 2) {
        title_ = "Audio / Video Tracks";
        Row v{"Video Track",
              videoStreams_.empty()
                  ? "(none)"
                  : videoStreams_[std::min<std::size_t>(
                                      videoSel_, videoStreams_.size() - 1)].second,
              Action::EnterVideoList};
        v.enabled = videoStreams_.size() > 1;  // nothing to pick with 0/1 track
        if (!v.enabled) v.action = Action::None;
        rows_.push_back(v);
        Row a{"Audio Track",
              audioStreams_.empty()
                  ? "(none)"
                  : audioStreams_[std::min<std::size_t>(
                                      audioSel_, audioStreams_.size() - 1)].second,
              Action::EnterAudioList};
        a.enabled = audioStreams_.size() > 1;
        if (!a.enabled) a.action = Action::None;
        rows_.push_back(a);
        rows_.push_back({"Preferred Audio Language",
                         kLangNames[audioLangIdx_ % kLangCount],
                         Action::EnterLangList});
    } else if (section_ == 3) {
        title_ = "System & Debug";
        rows_.push_back({"Debug Statistics", showStats_ ? "On" : "Off",
                         Action::ToggleStats});
    } else if (section_ == 10) {
        title_ = "Select Video Track";
        for (std::size_t i = 0; i < videoStreams_.size(); ++i) {
            Row r{videoStreams_[i].second,
                  static_cast<int>(i) == videoSel_ ? "<" : "",
                  Action::PickVideoStream};
            r.data = static_cast<int>(i);
            rows_.push_back(r);
        }
    } else if (section_ == 11) {
        title_ = "Select Audio Track";
        for (std::size_t i = 0; i < audioStreams_.size(); ++i) {
            Row r{audioStreams_[i].second,
                  static_cast<int>(i) == audioSel_ ? "<" : "",
                  Action::PickAudioStream};
            r.data = static_cast<int>(i);
            rows_.push_back(r);
        }
    } else {  // section_ == 12: preferred-language list
        title_ = "Preferred Audio Language";
        for (int i = 0; i < kLangCount; ++i) {
            Row r{kLangNames[i], i == audioLangIdx_ ? "<" : "",
                  Action::PickLanguage};
            r.data = i;
            rows_.push_back(r);
        }
    }
    rows_.push_back({"< Back", "", Action::Back});
}

SettingsMenu::Action SettingsMenu::activate() {
    if (selected_ < 0 || selected_ >= static_cast<int>(rows_.size())) {
        return Action::None;
    }
    const Action act = rows_[selected_].action;
    switch (act) {
        case Action::EnterDisplay:  section_ = 0; selected_ = 0; refresh(); break;
        case Action::EnterControls: section_ = 1; selected_ = 0; refresh(); break;
        case Action::EnterStreams:  section_ = 2; selected_ = 0; refresh(); break;
        case Action::EnterSystem:   section_ = 3; selected_ = 0; refresh(); break;
        case Action::EnterVideoList: section_ = 10; selected_ = 0; refresh(); break;
        case Action::EnterAudioList: section_ = 11; selected_ = 0; refresh(); break;
        case Action::EnterLangList:  section_ = 12; selected_ = 0; refresh(); break;
        case Action::Back:
            section_ = (section_ >= 10) ? 2 : -1;  // list -> Tracks; else -> root
            selected_ = 0;
            refresh();
            break;
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
        case Action::PickVideoStream:
            videoSel_ = rows_[selected_].data;
            section_ = 2;
            selected_ = 0;
            refresh();
            break;
        case Action::PickAudioStream:
            audioSel_ = rows_[selected_].data;
            section_ = 2;
            selected_ = 0;
            refresh();
            break;
        case Action::PickLanguage:
            audioLangIdx_ = rows_[selected_].data;
            saveConfig();
            section_ = 2;
            selected_ = 0;
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

void SettingsMenu::setStreams(
    const std::vector<std::pair<int, std::string>>& video,
    const std::vector<std::pair<int, std::string>>& audio, int currentVideo,
    int currentAudio) {
    videoStreams_ = video;
    audioStreams_ = audio;
    videoSel_ = 0;
    for (std::size_t i = 0; i < video.size(); ++i)
        if (video[i].first == currentVideo) videoSel_ = static_cast<int>(i);
    audioSel_ = 0;
    for (std::size_t i = 0; i < audio.size(); ++i)
        if (audio[i].first == currentAudio) audioSel_ = static_cast<int>(i);
    if (open_) refresh();
}

int SettingsMenu::selectedVideoStream() const {
    return (videoSel_ >= 0 && videoSel_ < static_cast<int>(videoStreams_.size()))
               ? videoStreams_[videoSel_].first
               : -1;
}

int SettingsMenu::selectedAudioStream() const {
    return (audioSel_ >= 0 && audioSel_ < static_cast<int>(audioStreams_.size()))
               ? audioStreams_[audioSel_].first
               : -1;
}

std::string SettingsMenu::audioLanguage() const {
    return kLangCodes[audioLangIdx_ % kLangCount];
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
        else if (key == "audio_lang") audioLangIdx_ = std::clamp(std::stoi(val), 0, kLangCount - 1);
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
            file << "audio_lang=" << audioLangIdx_ << "\n";
        }
    } catch (const std::exception& e) {
        PIXELVR_LOG_WARN("Could not save settings: %s", e.what());
    }
}

} // namespace pixelvr

#include "ui/settings_menu.hpp"
#include "app/vault_manager.hpp"

#include "util/logging.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace pixelvr {

namespace {

// Must match XrInputState::Button ordering.
const char* kButtonNames[] = {"A",          "B",          "X",
                              "Y",          "Menu",       "View",
                              "Bumper",     "Thumbstick", "Grip",
                              "D-Pad Up",   "D-Pad Down", "D-Pad Left",
                              "D-Pad Right"};
constexpr int kButtonCount = 13;
const char* kDownloadDirs[] = {"~/media", "~/Downloads", "~/Videos"};
constexpr int kDownloadDirCount = 3;

const char* kLangNames[] = {"Auto",    "English",  "German",  "French",
                           "Spanish", "Japanese", "Italian", "Russian",
                           "Chinese", "Korean"};
const char* kLangCodes[] = {"",    "eng", "deu", "fra", "spa",
                           "jpn", "ita", "rus", "chi", "kor"};
constexpr int kLangCount = 10;

const char* kCurvatureNames[] = {"Flat (0%)", "Gentle (25%)", "Cinema (50%)",
                                 "Deep Curve (75%)", "IMAX Wrap (100%)"};
const float kCurvatureValues[] = {0.0f, 0.25f, 0.50f, 0.75f, 1.00f};
constexpr int kCurvatureCount = 5;

const char* kUiTextScaleNames[] = {"Compact (90%)", "Standard (105%)",
                                   "Large (120%)", "Extra Large (140%)",
                                   "Huge (160%)"};
const float kUiTextScaleValues[] = {0.90f, 1.05f, 1.20f, 1.40f, 1.60f};
constexpr int kUiTextScaleCount = 5;

const char* kUiTextColorNames[] = {"Pure White", "Ice Cyan", "Warm Cream",
                                   "Soft Gold", "Mint Green"};
constexpr int kUiTextColorCount = 5;

const char* kUiAccentColorNames[] = {"Cyber Cyan", "Emerald Green", "Amber Gold",
                                     "Royal Purple", "Crimson Red"};
constexpr int kUiAccentColorCount = 5;

const char* kUiDistanceNames[] = {"Near (1.5 m)", "Comfort (1.9 m)",
                                  "Medium (2.3 m)", "Far (2.8 m)"};
const float kUiDistanceValues[] = {1.5f, 1.9f, 2.3f, 2.8f};
constexpr int kUiDistanceCount = 4;

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
    if (section_ < 0 || section_ > 3) section_ = 0;
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
        title_ = "Settings > Display & Cinema";
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
        rows_.push_back({"Cinema Screen Curve", kCurvatureNames[curvatureIdx_ % kCurvatureCount],
                         Action::CycleCurvature});
        rows_.push_back({"Screen Distance", distStream.str(),
                         Action::CycleDistance});
        rows_.push_back({"Swap Left/Right Eye", swapEyes_ ? "On" : "Off",
                         Action::ToggleSwapEyes});
        rows_.push_back({"Recenter Screen View", "Recenter Now",
                         Action::RecenterScreen});
    } else if (section_ == 1) {
        title_ = "Settings > UI & Appearance";
        rows_.push_back({"UI Text Size",
                         kUiTextScaleNames[uiTextScaleIdx_ % kUiTextScaleCount],
                         Action::CycleUiTextScale});
        rows_.push_back({"UI Text Color",
                         kUiTextColorNames[uiTextColorIdx_ % kUiTextColorCount],
                         Action::CycleUiTextColor});
        rows_.push_back({"UI Accent Theme",
                         kUiAccentColorNames[uiAccentColorIdx_ % kUiAccentColorCount],
                         Action::CycleUiAccentColor});
        rows_.push_back({"UI Panel Distance",
                         kUiDistanceNames[uiDistanceIdx_ % kUiDistanceCount],
                         Action::CycleUiDistance});
        rows_.push_back({"Download Folder",
                         kDownloadDirs[downloadDirIdx_ % kDownloadDirCount],
                         Action::CycleDownloadDir});
        rows_.push_back({"Debug Statistics", showStats_ ? "On" : "Off",
                         Action::ToggleStats});
    } else if (section_ == 2) {
        title_ = "Settings > Audio & Video Tracks";
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
        title_ = "Settings > Controls & Bindings";
        auto btnLabel = [&](int func, int btn) -> std::string {
            return listeningFunc_ == func
                       ? std::string("[ press a button ]")
                       : std::string(kButtonNames[btn % kButtonCount]);
        };
        rows_.push_back({"Play / Pause", btnLabel(0, bindPlayBtn_),
                         Action::ListenPlay});
        rows_.push_back({"Open Menu / Browser", btnLabel(1, bindMenuBtn_),
                         Action::ListenMenu});
        rows_.push_back({"Recenter View", btnLabel(2, bindRecenterBtn_),
                         Action::ListenRecenter});
        rows_.push_back({"Select / Click", "Trigger", Action::None});
        rows_.push_back({"Controller Models", drawModels_ ? "On" : "Off",
                         Action::ToggleModels});
    } else if (section_ == 4) {
        title_ = "Settings > Vault & Privacy";
        rows_.push_back({"Auto-Lock on Headset Removal",
                         VaultManager::instance().autoLockHeadsetOff() ? "Enabled" : "Disabled",
                         Action::CycleVaultAutoLockHeadset});

        int toSec = VaultManager::instance().autoLockTimeoutSec();
        std::string toStr = "Disabled";
        if (toSec == 120) toStr = "2 Minutes";
        else if (toSec == 300) toStr = "5 Minutes";
        else if (toSec == 600) toStr = "10 Minutes";
        rows_.push_back({"Inactivity Auto-Lock Timeout", toStr,
                         Action::CycleVaultTimeout});

        auto moveBtnLabel = [&]() -> std::string {
            if (listeningFunc_ == 3) return "[ press a button ]";
            int mb = VaultManager::instance().moveButton();
            if (mb < 0 || mb >= kButtonCount) return "Disabled";
            return kButtonNames[mb % kButtonCount];
        };
        rows_.push_back({"File Actions Button", moveBtnLabel(),
                         Action::ListenVaultMoveBtn});

        std::string comboLabel;
        if (VaultManager::instance().isRecordingCombo()) {
            comboLabel = "[ Recording: " + std::to_string(VaultManager::instance().recordedCount()) + "/6 ]";
        } else {
            comboLabel = VaultManager::instance().comboString();
        }
        rows_.push_back({"Unlock Combo Sequence", comboLabel,
                         Action::RecordVaultCombo});

        rows_.push_back({"Lock Vault Now", "[ Lock & Return Home ]",
                         Action::LockVaultAction});
    } else if (section_ == 10) {
        title_ = "Settings > Select Video Track";
        for (std::size_t i = 0; i < videoStreams_.size(); ++i) {
            Row r{videoStreams_[i].second,
                  static_cast<int>(i) == videoSel_ ? "ACTIVE" : "Select",
                  Action::PickVideoStream};
            r.data = static_cast<int>(i);
            rows_.push_back(r);
        }
        rows_.push_back({"< Back to Tracks", "", Action::Back});
    } else if (section_ == 11) {
        title_ = "Settings > Select Audio Track";
        for (std::size_t i = 0; i < audioStreams_.size(); ++i) {
            Row r{audioStreams_[i].second,
                  static_cast<int>(i) == audioSel_ ? "ACTIVE" : "Select",
                  Action::PickAudioStream};
            r.data = static_cast<int>(i);
            rows_.push_back(r);
        }
        rows_.push_back({"< Back to Tracks", "", Action::Back});
    } else {  // section_ == 12: preferred-language list
        title_ = "Settings > Preferred Audio Language";
        for (int i = 0; i < kLangCount; ++i) {
            Row r{kLangNames[i], i == audioLangIdx_ ? "ACTIVE" : "Select",
                  Action::PickLanguage};
            r.data = i;
            rows_.push_back(r);
        }
        rows_.push_back({"< Back to Tracks", "", Action::Back});
    }
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
            section_ = (section_ >= 10) ? 2 : 0;  // list -> Tracks; else -> Display
            selected_ = 0;
            refresh();
            break;
        case Action::ListenPlay:
            listeningFunc_ = 0;
            status_ = "Press any controller button for Play / Pause";
            refresh();
            break;
        case Action::ListenMenu:
            listeningFunc_ = 1;
            status_ = "Press any controller button for Open Menu";
            refresh();
            break;
        case Action::ListenRecenter:
            listeningFunc_ = 2;
            status_ = "Press any controller button for Recenter";
            refresh();
            break;
        case Action::CycleDownloadDir:
            downloadDirIdx_ = (downloadDirIdx_ + 1) % kDownloadDirCount;
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
        case Action::CycleCurvature:
            curvatureIdx_ = (curvatureIdx_ + 1) % kCurvatureCount;
            saveConfig();
            refresh();
            break;
        case Action::CycleUiTextScale:
            uiTextScaleIdx_ = (uiTextScaleIdx_ + 1) % kUiTextScaleCount;
            saveConfig();
            refresh();
            break;
        case Action::CycleUiTextColor:
            uiTextColorIdx_ = (uiTextColorIdx_ + 1) % kUiTextColorCount;
            saveConfig();
            refresh();
            break;
        case Action::CycleUiAccentColor:
            uiAccentColorIdx_ = (uiAccentColorIdx_ + 1) % kUiAccentColorCount;
            saveConfig();
            refresh();
            break;
        case Action::CycleUiDistance:
            uiDistanceIdx_ = (uiDistanceIdx_ + 1) % kUiDistanceCount;
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
        case Action::CycleVaultAutoLockHeadset:
            VaultManager::instance().setAutoLockHeadsetOff(!VaultManager::instance().autoLockHeadsetOff());
            refresh();
            break;
        case Action::CycleVaultTimeout: {
            int to = VaultManager::instance().autoLockTimeoutSec();
            if (to == 0) to = 120;
            else if (to == 120) to = 300;
            else if (to == 300) to = 600;
            else to = 0;
            VaultManager::instance().setAutoLockTimeoutSec(to);
            refresh();
            break;
        }
        case Action::ListenVaultMoveBtn:
            listeningFunc_ = 3;
            status_ = "Press a controller button for Move-to-Vault";
            refresh();
            break;
        case Action::RecordVaultCombo:
            if (VaultManager::instance().isRecordingCombo()) {
                VaultManager::instance().cancelRecordingCombo();
                status_ = "Combo recording cancelled";
            } else {
                VaultManager::instance().startRecordingCombo();
                status_ = "Press 6 face buttons (A, B, X, Y) to record new combo";
            }
            refresh();
            break;
        case Action::LockVaultAction:
            VaultManager::instance().lock();
            section_ = 0;
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

void SettingsMenu::assignListening(int button) {
    if (listeningFunc_ < 0) return;
    const int b = (button < 0 || button >= kButtonCount) ? 0 : button;
    if (listeningFunc_ == 0) bindPlayBtn_ = b;
    else if (listeningFunc_ == 1) bindMenuBtn_ = b;
    else if (listeningFunc_ == 2) bindRecenterBtn_ = b;
    else if (listeningFunc_ == 3) VaultManager::instance().setMoveButton(b);
    listeningFunc_ = -1;
    status_ = "Bound to " + std::string(kButtonNames[b]);
    saveConfig();
    refresh();
}

std::string SettingsMenu::downloadDir() const {
    const char* home = std::getenv("HOME");
    const std::string base = home ? home : ".";
    const char* d = kDownloadDirs[downloadDirIdx_ % kDownloadDirCount];
    return d[0] == '~' ? base + (d + 1) : std::string(d);
}

float SettingsMenu::screenCurvature() const {
    return kCurvatureValues[curvatureIdx_ % kCurvatureCount];
}

void SettingsMenu::setCurvatureIdx(int idx) {
    curvatureIdx_ = ((idx % kCurvatureCount) + kCurvatureCount) % kCurvatureCount;
    saveConfig();
    refresh();
}

float SettingsMenu::uiTextScale() const {
    return kUiTextScaleValues[uiTextScaleIdx_ % kUiTextScaleCount];
}

float SettingsMenu::uiDistance() const {
    return kUiDistanceValues[uiDistanceIdx_ % kUiDistanceCount];
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
        else if (key == "bind_play_btn") bindPlayBtn_ = std::clamp(std::stoi(val), 0, kButtonCount - 1);
        else if (key == "bind_menu_btn") bindMenuBtn_ = std::clamp(std::stoi(val), 0, kButtonCount - 1);
        else if (key == "bind_recenter_btn") {
            const int b = std::clamp(std::stoi(val), 0, kButtonCount - 1);
            // Grip used to default to recenter; it now toggles the laser pointer,
            // so a saved grip binding is migrated to the new default (Bumper).
            bindRecenterBtn_ = (b == 8) ? 6 : b;
        }
        else if (key == "download_dir") downloadDirIdx_ = std::clamp(std::stoi(val), 0, kDownloadDirCount - 1);
        else if (key == "audio_lang") audioLangIdx_ = std::clamp(std::stoi(val), 0, kLangCount - 1);
        else if (key == "curvature") curvatureIdx_ = std::clamp(std::stoi(val), 0, kCurvatureCount - 1);
        else if (key == "ui_text_scale") uiTextScaleIdx_ = std::clamp(std::stoi(val), 0, kUiTextScaleCount - 1);
        else if (key == "ui_text_color") uiTextColorIdx_ = std::clamp(std::stoi(val), 0, kUiTextColorCount - 1);
        else if (key == "ui_accent_color") uiAccentColorIdx_ = std::clamp(std::stoi(val), 0, kUiAccentColorCount - 1);
        else if (key == "ui_distance") uiDistanceIdx_ = std::clamp(std::stoi(val), 0, kUiDistanceCount - 1);
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
            file << "bind_play_btn=" << bindPlayBtn_ << "\n";
            file << "bind_menu_btn=" << bindMenuBtn_ << "\n";
            file << "bind_recenter_btn=" << bindRecenterBtn_ << "\n";
            file << "download_dir=" << downloadDirIdx_ << "\n";
            file << "audio_lang=" << audioLangIdx_ << "\n";
            file << "curvature=" << curvatureIdx_ << "\n";
            file << "ui_text_scale=" << uiTextScaleIdx_ << "\n";
            file << "ui_text_color=" << uiTextColorIdx_ << "\n";
            file << "ui_accent_color=" << uiAccentColorIdx_ << "\n";
            file << "ui_distance=" << uiDistanceIdx_ << "\n";
        }
    } catch (const std::exception& e) {
        PIXELVR_LOG_WARN("Could not save settings: %s", e.what());
    }
}

} // namespace pixelvr

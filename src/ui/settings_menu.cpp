#include "ui/settings_menu.hpp"
#include "app/vault_manager.hpp"
#include "net/smb_credentials.hpp"
#include "xr/vr_keyboard.hpp"

#include "util/logging.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace pixelvr {

namespace {

const char* const kDefaultStatus = "Point + Trigger: Change Setting  |  B: Close";
const char* const kShortcutsStatus = "Shortcuts appear as tabs on the hub's rail, in this order.";
// Buttons are assigned in SteamVR only (OpenXR can't open that page for us).
const char* const kButtonsStatus =
    "Change buttons in SteamOS: SteamVR Settings > Controllers > Manage Controller Bindings";
const char* const kButtonsPagesStatus =
    "< > or Bumpers: Page  |  Change buttons: SteamVR > Controllers > Manage Controller Bindings";
const char* kDownloadDirs[] = {"~/media", "~/Downloads", "~/Videos"};
constexpr int kDownloadDirCount = 3;

// Web destinations the two lower rail tabs can open. Streaming services open
// as a kiosk screen (just the page); "Browser" is the full browser.
// Built-in defaults for the shortcut list (and the meaning of the old rail_shortcuts bits).
struct DefaultShortcut {
    const char* name;
    const char* url;
    bool kiosk;
};
const DefaultShortcut kRailShortcuts[] = {
    {"YouTube", "https://www.youtube.com", true},
    {"Netflix", "https://www.netflix.com", true},
    {"Disney+", "https://www.disneyplus.com", true},
    {"Prime Video", "https://www.primevideo.com", true},
    {"Twitch", "https://www.twitch.tv", true},
    {"Browser", "https://www.google.com", false},
    // Appended (not sorted) so saved rail_shortcuts bitmasks keep their meaning.
    {"Plex", "https://app.plex.tv", true},
    {"Jellyfin", "", true},  // no fixed URL: the user's server, asked for on first use
};
constexpr int kRailShortcutCount = 8;
static_assert(static_cast<int>(SettingsMenu::Action::ToggleRailShortcut0) + kRailShortcutCount - 1 ==
                  static_cast<int>(SettingsMenu::Action::ToggleRailShortcut7),
              "one ToggleRailShortcutN action per shortcut");

const char* kLangNames[] = {"Auto",    "English",  "German",  "French",
                           "Spanish", "Japanese", "Italian", "Russian",
                           "Chinese"};
const char* kLangCodes[] = {"",    "eng", "deu", "fra", "spa",
                           "jpn", "ita", "rus", "chi"};
constexpr int kLangCount = 9;

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

const char* kHoverHapticsStrengthNames[] = {
    "Subtle (25%)", "Medium (50%)", "Strong (75%)", "Maximum (100%)"};
const float kHoverHapticsStrengthValues[] = {0.08f, 0.15f, 0.28f, 0.50f};
constexpr int kHoverHapticsStrengthCount = 4;

const char* kAmbilightNames[] = {"Off", "Subtle", "Medium", "Strong"};
const float kAmbilightValues[] = {0.0f, 0.30f, 0.55f, 0.85f};
constexpr int kAmbilightCount = 4;

// Panel style: Solid = opaque; Frosted = the screens behind, blurred, under a
// tinted slab; Glass = see-through (the compositor blends it over everything).
// The value is the glass slab's opacity factor.
const char* kUiOpacityNames[] = {"Solid", "Frosted", "Glass"};
const float kUiOpacityValues[] = {1.0f, 0.60f, 0.45f};
constexpr int kUiOpacityCount = 3;

const char* kPanelCurveNames[] = {"Flat", "Gentle", "Wrap"};
const float kPanelCurveValues[] = {0.0f, 0.45f, 1.0f};
constexpr int kPanelCurveCount = 3;

// Default flat-screen distance presets: 1.5 m .. 5.0 m in 0.5 m steps.
constexpr float kDistanceMin = 2.0f;  // closer and the screen sits in front of the menus
constexpr float kDistanceStep = 0.5f;
constexpr int kDistanceSteps = 8;

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
    ensureShortcuts();
    refresh();
}

void SettingsMenu::open() {
    open_ = true;
    // Sub-pages (track lists, list columns) reopen at their parent section; the
    // Vault section is only reachable while the vault is unlocked.
    if (section_ < 0 || section_ > 4 ||
        (section_ == 4 && !VaultManager::instance().isUnlocked())) {
        section_ = 0;
    }
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
    if (section_ >= kButtonsSection && !showingButtons()) section_ = kButtonsSection;
    // Page hints belong to their page; transient messages stay.
    if (!pageHint_.empty() && status_ == pageHint_) status_ = kDefaultStatus;
    pageHint_.clear();
    auto hint = [&](const std::string& s) {
        status_ = s;
        pageHint_ = s;
    };

    auto choice = [&](const std::string& label, const std::string& value, int steps,
                      int idx, Action act, bool enabled = true) {
        Row r{label, value, act, enabled};
        r.kind = Kind::Choice;
        r.steps = steps;
        r.stepIdx = idx;
        rows_.push_back(r);
    };
    auto toggle = [&](const std::string& label, bool on, Action act,
                      bool enabled = true) {
        Row r{label, on ? "On" : "Off", act, enabled};
        r.kind = Kind::Toggle;
        r.stepIdx = on ? 1 : 0;
        rows_.push_back(r);
    };
    auto binding = [&](const std::string& label, const std::string& value, Action act) {
        Row r{label, value, act};
        r.kind = Kind::Binding;
        rows_.push_back(r);
    };
    auto color = [&](const std::string& label, const std::string& value, int steps,
                     int idx, Action act) {
        Row r{label, value, act};
        r.kind = Kind::Color;
        r.steps = steps;
        r.stepIdx = idx;
        rows_.push_back(r);
    };
    auto segmented = [&](const std::string& label, std::vector<std::string> opts, int idx,
                         Action act) {
        Row r{label, opts.empty() ? std::string() : opts[static_cast<std::size_t>(idx) % opts.size()], act};
        r.kind = Kind::Segmented;
        r.steps = static_cast<int>(opts.size());
        r.stepIdx = idx;
        r.options = std::move(opts);
        rows_.push_back(r);
    };
    auto option = [&](const std::string& label, bool active, Action act, int data) {
        Row r{label, active ? "Active" : "", act};
        r.kind = Kind::Option;
        r.stepIdx = active ? 1 : 0;
        r.data = data;
        rows_.push_back(r);
    };

    if (section_ == 0) {
        title_ = "Settings > Display & Cinema";
        choice("Cinema Screen Curve", kCurvatureNames[curvatureIdx_ % kCurvatureCount],
               kCurvatureCount, curvatureIdx_ % kCurvatureCount, Action::CycleCurvature);
        segmented("Room", {"Midnight", "Black", "Slate", "Warm"}, roomIdx_ % 4, Action::CycleRoom);
        choice("Ambient Screen Glow", kAmbilightNames[ambilightIdx_ % kAmbilightCount],
               kAmbilightCount, ambilightIdx_ % kAmbilightCount, Action::CycleAmbilight,
               projectionMode_ == 0);
        if (multiScreen_ && ambilightIdx_ % kAmbilightCount > 0) {
            segmented("Glow Around", {"Selected Screen", "All Screens"}, ambilightAllScreens_ ? 1 : 0,
                      Action::CycleAmbilightScope);
        }
        std::ostringstream distStream;
        distStream << std::fixed;
        distStream.precision(1);
        distStream << screenDistance_ << " m";
        const int distIdx = std::clamp(
            static_cast<int>(std::lround((screenDistance_ - kDistanceMin) / kDistanceStep)),
            0, kDistanceSteps - 1);
        choice("Default Screen Distance", distStream.str(), kDistanceSteps, distIdx,
               Action::CycleDistance);
        toggle("Swap Left/Right Eye", swapEyes_, Action::ToggleSwapEyes);
        toggle("Multiple Screens", multiScreen_, Action::ToggleMultiScreen);
        toggle("Pause When Headset Is Removed", pauseOnRemoval_, Action::TogglePauseOnRemoval);
        toggle("Pause Livestreams Too", pauseLivestreams_, Action::TogglePauseLivestreams, pauseOnRemoval_);
        toggle("Debug Statistics", showStats_, Action::ToggleStats);
    } else if (section_ == 1) {
        title_ = "Settings > UI & Appearance";
        choice("UI Text Size", kUiTextScaleNames[uiTextScaleIdx_ % kUiTextScaleCount],
               kUiTextScaleCount, uiTextScaleIdx_ % kUiTextScaleCount, Action::CycleUiTextScale);
        color("UI Text Color", kUiTextColorNames[uiTextColorIdx_ % kUiTextColorCount],
              kUiTextColorCount, uiTextColorIdx_ % kUiTextColorCount, Action::CycleUiTextColor);
        color("UI Accent Theme", kUiAccentColorNames[uiAccentColorIdx_ % kUiAccentColorCount],
              kUiAccentColorCount, uiAccentColorIdx_ % kUiAccentColorCount,
              Action::CycleUiAccentColor);
        choice("UI Panel Distance", kUiDistanceNames[uiDistanceIdx_ % kUiDistanceCount],
               kUiDistanceCount, uiDistanceIdx_ % kUiDistanceCount, Action::CycleUiDistance);
        segmented("Main Panel Shape",
                  {kPanelCurveNames[0], kPanelCurveNames[1], kPanelCurveNames[2]},
                  panelCurveIdx_ % kPanelCurveCount, Action::CyclePanelCurve);
        segmented("Panel Style", {kUiOpacityNames[0], kUiOpacityNames[1], kUiOpacityNames[2]},
                  uiOpacityIdx_ % kUiOpacityCount, Action::CycleUiOpacity);
        segmented("UI in 360/180 Videos", {"Fixed", "Follow Gaze"}, uiFollowMode_ % 2,
                  Action::CycleUiFollowMode);
        segmented("Download Folder", {kDownloadDirs[0], kDownloadDirs[1], kDownloadDirs[2]},
                  downloadDirIdx_ % kDownloadDirCount, Action::CycleDownloadDir);
        {
            std::string on;
            for (int i = 0; i < kRailShortcutCount; ++i) {
                if ((railShortcutMask_ >> i) & 1) on += (on.empty() ? "" : ", ") + std::string(kRailShortcuts[i].name);
            }
            rows_.push_back({"Shortcuts", on.empty() ? "None" : on, Action::EnterRailShortcuts});
        }
        rows_.push_back({"List View Columns", "Customize", Action::EnterListColsConfig});
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
        title_ = "Settings > Controls & Input";
        hint(kButtonsStatus);
        rows_.push_back({"Controller Buttons", "Assigned in SteamOS", Action::EnterButtons});
        toggle("Button Labels", buttonLabels_, Action::ToggleButtonLabels);
        segmented("Keyboard Layout", {"US", "Deutsch", "UK", "Français", "Nordic"}, keyboardLayout_ % 5,
                  Action::CycleKeyboardLayout);
        segmented("Laser Smoothing", {"Off", "Low", "Medium", "High"}, laserSmoothing_ % 4,
                  Action::CycleLaserSmoothing);
        toggle("Controller Models", drawModels_, Action::ToggleModels);
        toggle("Laser Hover Rumble", hoverHapticsEnabled_, Action::ToggleHoverHaptics);
        choice("Rumble Strength", hoverHapticsStrengthStr(), kHoverHapticsStrengthCount,
               hoverHapticsStrengthIdx_ % kHoverHapticsStrengthCount,
               Action::CycleHoverHapticsStrength, hoverHapticsEnabled_);
    } else if (section_ == 4) {
        title_ = "Settings > Vault & Privacy";
        toggle("Auto-Lock on Headset Removal", VaultManager::instance().autoLockHeadsetOff(),
               Action::CycleVaultAutoLockHeadset);
        toggle("Private Browsing (Incognito)", privateBrowsing_, Action::TogglePrivateBrowsing);

        const int toSec = VaultManager::instance().autoLockTimeoutSec();
        const int toIdx = (toSec == 120) ? 1 : (toSec == 300) ? 2 : (toSec == 600) ? 3 : 0;
        const char* toNames[4] = {"Disabled", "2 Minutes", "5 Minutes", "10 Minutes"};
        choice("Inactivity Auto-Lock", toNames[toIdx], 4, toIdx, Action::CycleVaultTimeout);

        std::string comboLabel;
        if (VaultManager::instance().isRecordingCombo()) {
            comboLabel = "Recording " + std::to_string(VaultManager::instance().recordedCount()) + " / 6";
        } else {
            comboLabel = VaultManager::instance().comboString();
        }
        binding("Unlock Combo Sequence", comboLabel, Action::RecordVaultCombo);
    } else if (showingButtons()) {
        // Read-only: one row per action with the buttons SteamVR binds to it.
        // Every card is a binding; the header pager (< n / m >) turns pages.
        const int page = section_ - kButtonsSection;
        title_ = "Settings > Controls & Input > Buttons";
        hint(controlsNotice_.empty() ? std::string(kButtonsPagesStatus) : controlsNotice_);
        const int n = static_cast<int>(controlsInfo_.size());
        for (int i = page * kButtonsPerPage; i < std::min(n, (page + 1) * kButtonsPerPage); ++i) {
            Row r{controlsInfo_[static_cast<std::size_t>(i)].first,
                  controlsInfo_[static_cast<std::size_t>(i)].second, Action::None};
            r.kind = Kind::Choice;  // plain value text, no control
            rows_.push_back(r);
        }
    } else if (section_ == 22) {
        title_ = "Settings > UI & Appearance > Shortcuts";
        hint(kShortcutsStatus);
        for (int i = 0; i < kRailShortcutCount; ++i) {
            const bool on = (railShortcutMask_ >> i) & 1;
            toggle(kRailShortcuts[i].name, on,
                   static_cast<Action>(static_cast<int>(Action::ToggleRailShortcut0) + i));
        }
        rows_.push_back({"Jellyfin address", jellyfinUrl_.empty() ? std::string("Not set") : jellyfinUrl_,
                         Action::EditJellyfinAddress});
        rows_.push_back({"Back to UI & Appearance", "", Action::BackToUI});
    } else if (section_ == 21) {
        title_ = "Settings > UI & Appearance > List Columns";
        toggle("Format / VR Badge", listShowFormat_, Action::ToggleListColFormat);
        toggle("Resolution & FPS", listShowRes_, Action::ToggleListColRes);
        toggle("File Size", listShowSize_, Action::ToggleListColSize);
        toggle("Video Duration", listShowDuration_, Action::ToggleListColDuration);
        rows_.push_back({"Back to UI & Appearance", "", Action::BackToUI});
    } else if (section_ == 10) {
        title_ = "Settings > Tracks > Video Track";
        for (std::size_t i = 0; i < videoStreams_.size(); ++i) {
            option(videoStreams_[i].second, static_cast<int>(i) == videoSel_,
                   Action::PickVideoStream, static_cast<int>(i));
        }
        rows_.push_back({"Back to Tracks", "", Action::Back});
    } else if (section_ == 11) {
        title_ = "Settings > Tracks > Audio Track";
        for (std::size_t i = 0; i < audioStreams_.size(); ++i) {
            option(audioStreams_[i].second, static_cast<int>(i) == audioSel_,
                   Action::PickAudioStream, static_cast<int>(i));
        }
        rows_.push_back({"Back to Tracks", "", Action::Back});
    } else {  // section_ == 12: preferred-language list
        title_ = "Settings > Tracks > Preferred Language";
        for (int i = 0; i < kLangCount; ++i) {
            option(kLangNames[i], i == audioLangIdx_, Action::PickLanguage, i);
        }
        rows_.push_back({"Back to Tracks", "", Action::Back});
    }
}

SettingsMenu::Action SettingsMenu::activate() {
    if (selected_ < 0 || selected_ >= static_cast<int>(rows_.size())) {
        return Action::None;
    }
    const Action act = rows_[selected_].action;
    switch (act) {
        case Action::EnterVideoList: section_ = 10; selected_ = 0; refresh(); break;
        case Action::EnterAudioList: section_ = 11; selected_ = 0; refresh(); break;
        case Action::EnterLangList:  section_ = 12; selected_ = 0; refresh(); break;
        case Action::Back:
            section_ = (section_ >= 10) ? 2 : 0;  // list -> Tracks; else -> Display
            selected_ = 0;
            refresh();
            break;
        case Action::EnterButtons:
            section_ = kButtonsSection;
            selected_ = 0;
            refresh();
            break;
        case Action::ToggleRailShortcut0:
        case Action::ToggleRailShortcut1:
        case Action::ToggleRailShortcut2:
        case Action::ToggleRailShortcut3:
        case Action::ToggleRailShortcut4:
        case Action::ToggleRailShortcut5:
        case Action::ToggleRailShortcut6:
        case Action::ToggleRailShortcut7: {
            const int i = static_cast<int>(act) - static_cast<int>(Action::ToggleRailShortcut0);
            railShortcutMask_ ^= (1 << i);
            saveConfig();
            refresh();
            break;
        }
        case Action::CycleDownloadDir:
            downloadDirIdx_ = (downloadDirIdx_ + 1) % kDownloadDirCount;
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
        case Action::ToggleButtonLabels:
            buttonLabels_ = !buttonLabels_;
            saveConfig();
            refresh();
            break;
        case Action::ToggleStats:
            showStats_ = !showStats_;
            saveConfig();
            refresh();
            break;
        case Action::CycleDistance: {
            const int idx = static_cast<int>(
                std::lround((screenDistance_ - kDistanceMin) / kDistanceStep));
            const int next = (std::clamp(idx, 0, kDistanceSteps - 1) + 1) % kDistanceSteps;
            screenDistance_ = kDistanceMin + kDistanceStep * static_cast<float>(next);
            saveConfig();
            refresh();
            break;
        }
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
        case Action::CycleUiFollowMode:
            uiFollowMode_ = (uiFollowMode_ + 1) % 2;
            saveConfig();
            refresh();
            break;
        case Action::ToggleHoverHaptics:
            hoverHapticsEnabled_ = !hoverHapticsEnabled_;
            saveConfig();
            refresh();
            break;
        case Action::CycleHoverHapticsStrength:
            hoverHapticsStrengthIdx_ = (hoverHapticsStrengthIdx_ + 1) % kHoverHapticsStrengthCount;
            saveConfig();
            refresh();
            break;
        case Action::CycleAmbilight:
            ambilightIdx_ = (ambilightIdx_ + 1) % kAmbilightCount;
            saveConfig();
            refresh();
            break;
        case Action::CycleAmbilightScope:
            ambilightAllScreens_ = !ambilightAllScreens_;
            saveConfig();
            refresh();
            break;
        case Action::CycleLaserSmoothing:
            laserSmoothing_ = (laserSmoothing_ + 1) % 4;
            saveConfig();
            refresh();
            break;
        case Action::CycleRoom:
            roomIdx_ = (roomIdx_ + 1) % 4;
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
        case Action::EnterListColsConfig:
            section_ = 21;
            selected_ = 0;
            refresh();
            break;
        case Action::EnterRailShortcuts:
            section_ = 22;
            selected_ = 0;
            refresh();
            break;
        case Action::ToggleListColFormat:
            listShowFormat_ = !listShowFormat_;
            refresh();
            saveConfig();
            break;
        case Action::ToggleListColRes:
            listShowRes_ = !listShowRes_;
            refresh();
            saveConfig();
            break;
        case Action::ToggleListColSize:
            listShowSize_ = !listShowSize_;
            refresh();
            saveConfig();
            break;
        case Action::ToggleListColDuration:
            listShowDuration_ = !listShowDuration_;
            refresh();
            saveConfig();
            break;
        case Action::ToggleMultiScreen:
            multiScreen_ = !multiScreen_;
            saveConfig();
            refresh();
            break;
        case Action::CycleKeyboardLayout:
            keyboardLayout_ = (keyboardLayout_ + 1) % 5;
            saveConfig();
            refresh();
            break;
        case Action::TogglePauseOnRemoval:
            pauseOnRemoval_ = !pauseOnRemoval_;
            saveConfig();
            refresh();
            break;
        case Action::TogglePauseLivestreams:
            pauseLivestreams_ = !pauseLivestreams_;
            saveConfig();
            refresh();
            break;
        case Action::TogglePrivateBrowsing:
            privateBrowsing_ = !privateBrowsing_;
            saveConfig();
            refresh();
            break;
        case Action::CycleUiOpacity:
            uiOpacityIdx_ = (uiOpacityIdx_ + 1) % kUiOpacityCount;
            saveConfig();
            refresh();
            break;
        case Action::CyclePanelCurve:
            panelCurveIdx_ = (panelCurveIdx_ + 1) % kPanelCurveCount;
            saveConfig();
            refresh();
            break;
        case Action::BackToUI:
            section_ = 1;
            selected_ = 0;
            refresh();
            break;
        default:
            break;
    }
    return act;
}

SettingsMenu::Action SettingsMenu::activateSegment(int segment) {
    if (selected_ < 0 || selected_ >= static_cast<int>(rows_.size())) return Action::None;
    const Row& row = rows_[static_cast<std::size_t>(selected_)];
    if (row.kind != Kind::Segmented || segment < 0 || segment >= row.steps) return activate();
    Action act = Action::None;
    // Each activation advances by one option; stop on the requested one.
    const int steps = row.steps;
    for (int guard = 0; guard < steps + 1; ++guard) {
        if (rows_[static_cast<std::size_t>(selected_)].stepIdx == segment) break;
        act = activate();
    }
    return act;
}

float SettingsMenu::uiOpacity() const {
    return kUiOpacityValues[uiOpacityIdx_ % kUiOpacityCount];
}

float SettingsMenu::panelCurve() const {
    return kPanelCurveValues[panelCurveIdx_ % kPanelCurveCount];
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

void SettingsMenu::setSelectedAudioStream(int containerIndex) {
    for (std::size_t i = 0; i < audioStreams_.size(); ++i) {
        if (audioStreams_[i].first == containerIndex) {
            audioSel_ = static_cast<int>(i);
            break;
        }
    }
    if (open_) refresh();
}

std::string SettingsMenu::audioLanguage() const {
    return kLangCodes[audioLangIdx_ % kLangCount];
}

void SettingsMenu::setControlsInfo(std::vector<std::pair<std::string, std::string>> info,
                                   std::string notice) {
    // Re-read about once a second: rebuild the rows only when something changed.
    if (info == controlsInfo_ && notice == controlsNotice_) return;
    controlsInfo_ = std::move(info);
    controlsNotice_ = std::move(notice);
    if (open_ && showingButtons()) {
        const int sel = selected_;
        refresh();
        setSelected(sel);
    }
}

bool SettingsMenu::turnButtonsPage(int delta, bool wrap) {
    if (!showingButtons() || delta == 0) return false;
    const int n = buttonsPages();
    int page = section_ - kButtonsSection + delta;
    if (wrap) {
        page = ((page % n) + n) % n;
    } else if (page < 0 || page >= n) {
        return false;
    }
    if (page == section_ - kButtonsSection) return false;
    section_ = kButtonsSection + page;
    selected_ = 0;
    refresh();
    return true;
}

void SettingsMenu::close() {
    open_ = false;
    // A combo recording only makes sense on the Vault page: leaving Settings by
    // any route (Main Menu, a rail tab, a web shortcut) cancels it, so later face
    // presses during playback are never saved as a new unlock combo.
    if (VaultManager::instance().isRecordingCombo()) VaultManager::instance().cancelRecordingCombo();
}

void SettingsMenu::setButtonLabels(bool on) {
    if (buttonLabels_ == on) return;
    buttonLabels_ = on;
    saveConfig();
    refresh();
}

void SettingsMenu::openButtons() {
    open_ = true;
    section_ = kButtonsSection;
    selected_ = 0;
    refresh();
}

void SettingsMenu::ensureShortcuts() {
    if (!shortcuts_.empty()) return;
    // First start with the shortcut list: the built-in services, with the old rail choice
    // (rail_shortcuts bits) and the old Jellyfin address carried over.
    for (int i = 0; i < kRailShortcutCount; ++i) {
        Shortcut sc{kRailShortcuts[i].name, kRailShortcuts[i].url, kRailShortcuts[i].kiosk,
                    ((railShortcutMask_ >> i) & 1) != 0};
        if (sc.url.empty() && sc.name == "Jellyfin" && !legacyJellyfinUrl_.empty()) sc.url = legacyJellyfinUrl_ + "/web/";
        shortcuts_.push_back(sc);
    }
}

void SettingsMenu::setShortcutName(int idx, const std::string& in) {
    if (idx < 0 || idx >= static_cast<int>(shortcuts_.size())) return;
    std::string name;
    for (const char c : in) {
        if (c != '|' && static_cast<unsigned char>(c) >= 0x20) name += c;  // one line, no field separator
    }
    while (!name.empty() && name.back() == ' ') name.pop_back();
    while (!name.empty() && name.front() == ' ') name.erase(0, 1);
    if (name.empty()) return;
    shortcuts_[static_cast<size_t>(idx)].name = name.substr(0, 40);
    saveConfig();
    refresh();
}

void SettingsMenu::setShortcutUrl(int idx, const std::string& in) {
    if (idx < 0 || idx >= static_cast<int>(shortcuts_.size())) return;
    // "192.168.1.10:8096" -> "http://192.168.1.10:8096"; a Jellyfin server opens at /web/.
    std::string url;
    for (const char c : in) {
        if (c != '|' && c != ' ' && static_cast<unsigned char>(c) >= 0x20) url += c;
    }
    if (!url.empty() && url.find("://") == std::string::npos) url = "http://" + url;
    Shortcut& sc = shortcuts_[static_cast<size_t>(idx)];
    if (sc.name == "Jellyfin" && !url.empty() && url.find("/web") == std::string::npos) {
        while (!url.empty() && url.back() == '/') url.pop_back();
        url += "/web/";
    }
    sc.url = url;
    saveConfig();
    refresh();
}

void SettingsMenu::openShortcutsPage(const std::string& hintText) {
    section_ = 22;
    selected_ = 0;
    open_ = true;
    refresh();
    if (!hintText.empty()) status_ = hintText;
}

std::vector<SettingsMenu::RailShortcut> SettingsMenu::railShortcuts() const {
    std::vector<RailShortcut> out;
    for (size_t i = 0; i < shortcuts_.size() && out.size() < static_cast<size_t>(kMaxRailShortcuts); ++i) {
        const Shortcut& sc = shortcuts_[i];
        if (sc.onRail) out.push_back({static_cast<int>(i), sc.name, sc.url, sc.kiosk});
    }
    return out;
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

float SettingsMenu::hoverHapticsStrength() const {
    return kHoverHapticsStrengthValues[hoverHapticsStrengthIdx_ % kHoverHapticsStrengthCount];
}

const char* SettingsMenu::hoverHapticsStrengthStr() const {
    return kHoverHapticsStrengthNames[hoverHapticsStrengthIdx_ % kHoverHapticsStrengthCount];
}

float SettingsMenu::ambilightIntensity() const {
    return kAmbilightValues[ambilightIdx_ % kAmbilightCount];
}

const float* SettingsMenu::roomColor() const {
    static const float kRooms[4][3] = {
        {0.020f, 0.020f, 0.050f},  // Midnight (the original navy)
        {0.000f, 0.000f, 0.000f},  // Black: cinema, nothing else
        {0.036f, 0.040f, 0.046f},  // Slate: neutral grey
        {0.052f, 0.033f, 0.022f},  // Warm: dim amber
    };
    return kRooms[std::clamp(roomIdx_, 0, 3)];
}


void SettingsMenu::loadConfig() {
    const std::string path = getConfigPath();
    std::ifstream file(path);
    if (!file.is_open()) {
        return;
    }
    std::string line;
    bool panelStyleSeen = false;
    int oldOpacityIdx = -1;
    auto noteLegacyBinding = [this](bool custom) {
        legacyBindingsSeen_ = true;
        legacyBindingsCustom_ = legacyBindingsCustom_ || custom;
    };
    while (std::getline(file, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        // A malformed value (hand edit, truncated write) must not abort startup:
        // skip that key and keep its default.
        try {
            if (key == "projection") projectionMode_ = std::clamp(std::stoi(val), 0, 2);
            else if (key == "stereo") stereoMode_ = std::clamp(std::stoi(val), 0, 2);
            else if (key == "swap_eyes") swapEyes_ = (val == "1");
            else if (key == "draw_models") drawModels_ = (val == "1");
            else if (key == "button_labels") buttonLabels_ = (val == "1");
            else if (key == "show_stats") showStats_ = (val == "1");
            else if (key == "distance") screenDistance_ = std::clamp(std::stof(val), kDistanceMin, 10.0f);
            // Old press-to-bind keys: buttons are assigned in SteamVR now, so
            // these are only read to tell the user custom choices were reset.
            // Defaults: play A (0), menu B (1), recenter Bumper (6; a saved
            // Grip 8 already meant the default), bookmarks stick click (7).
            else if (key == "bind_play_btn") noteLegacyBinding(std::stoi(val) != 0);
            else if (key == "bind_menu_btn") noteLegacyBinding(std::stoi(val) != 1);
            else if (key == "bind_recenter_btn") {
                const int b = std::stoi(val);
                noteLegacyBinding(b != 6 && b != 8);
            }
            else if (key == "bind_quick_path_btn") noteLegacyBinding(std::stoi(val) != 7);
            else if (key == "download_dir") downloadDirIdx_ = std::clamp(std::stoi(val), 0, kDownloadDirCount - 1);
            else if (key == "rail_shortcuts") railShortcutMask_ = std::stoi(val) & ((1 << kRailShortcutCount) - 1);
            else if (key == "jellyfin_url") legacyJellyfinUrl_ = val;
            else if (key == "shortcut") {  // onRail|kiosk|name|url
                const size_t a = val.find('|'), b = val.find('|', a + 1), c = val.find('|', b + 1);
                if (a != std::string::npos && b != std::string::npos && c != std::string::npos) {
                    shortcuts_.push_back({val.substr(b + 1, c - b - 1), val.substr(c + 1), val.substr(a + 1, b - a - 1) == "1",
                                          val.substr(0, a) == "1"});
                }
            }
            else if (key == "audio_lang") audioLangIdx_ = std::clamp(std::stoi(val), 0, kLangCount - 1);
            else if (key == "curvature") curvatureIdx_ = std::clamp(std::stoi(val), 0, kCurvatureCount - 1);
            else if (key == "ui_text_scale") uiTextScaleIdx_ = std::clamp(std::stoi(val), 0, kUiTextScaleCount - 1);
            else if (key == "ui_text_color") uiTextColorIdx_ = std::clamp(std::stoi(val), 0, kUiTextColorCount - 1);
            else if (key == "ui_accent_color") uiAccentColorIdx_ = std::clamp(std::stoi(val), 0, kUiAccentColorCount - 1);
            else if (key == "ui_distance") uiDistanceIdx_ = std::clamp(std::stoi(val), 0, kUiDistanceCount - 1);
            else if (key == "ui_follow_mode") uiFollowMode_ = std::clamp(std::stoi(val), 0, 1);
            else if (key == "browser_view_mode") browserViewMode_ = std::clamp(std::stoi(val), 0, 2);
            else if (key == "list_show_format") listShowFormat_ = (val == "1" || val == "true");
            else if (key == "list_show_res") listShowRes_ = (val == "1" || val == "true");
            else if (key == "list_show_size") listShowSize_ = (val == "1" || val == "true");
            else if (key == "list_show_dur") listShowDuration_ = (val == "1" || val == "true");
            else if (key == "quick_path") quickPath_ = val;
            else if (key == "hover_haptics") hoverHapticsEnabled_ = (val == "1" || val == "true");
            else if (key == "hover_haptics_strength") hoverHapticsStrengthIdx_ = std::clamp(std::stoi(val), 0, kHoverHapticsStrengthCount - 1);
            else if (key == "ambilight") ambilightIdx_ = std::clamp(std::stoi(val), 0, kAmbilightCount - 1);
            else if (key == "ambilight_all") ambilightAllScreens_ = (val == "1");
            else if (key == "laser_smoothing") laserSmoothing_ = std::clamp(std::stoi(val), 0, 3);
            else if (key == "room") roomIdx_ = std::clamp(std::stoi(val), 0, 3);
            else if (key == "multi_screen") multiScreen_ = (val == "1" || val == "true");
            else if (key == "pause_on_removal") pauseOnRemoval_ = (val == "1" || val == "true");
            else if (key == "keyboard_layout") keyboardLayout_ = std::clamp(std::stoi(val), 0, 4);
            else if (key == "pause_livestreams") pauseLivestreams_ = (val == "1" || val == "true");
            else if (key == "private_browsing") privateBrowsing_ = (val == "1" || val == "true");
            else if (key == "screen_numbers") screenNumbers_ = (val == "1");
            else if (key == "audio_follow") audioFollow_ = (val == "1" || val == "true");
            else if (key == "panel_style") {
                uiOpacityIdx_ = std::clamp(std::stoi(val), 0, kUiOpacityCount - 1);
                panelStyleSeen = true;
            } else if (key == "ui_opacity") oldOpacityIdx = std::stoi(val);  // before panel_style
            else if (key == "panel_curve") panelCurveIdx_ = std::clamp(std::stoi(val), 0, kPanelCurveCount - 1);
            else if (key == "browser_sort") browserSort_ = std::clamp(std::stoi(val), 0, 2);
        } catch (const std::exception&) {
            PIXELVR_LOG_WARN("Ignoring malformed setting '%s' in %s", key.c_str(), path.c_str());
        }
    }
    // Older files had Panel Transparency (Solid / Frosted / Glass / Clear, the
    // translucent ones faked): keep an explicit Solid, everyone else gets
    // the new default Frosted.
    if (!panelStyleSeen) uiOpacityIdx_ = (oldOpacityIdx == 0) ? 0 : 1;
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
            file << "button_labels=" << (buttonLabels_ ? 1 : 0) << "\n";
            file << "show_stats=" << (showStats_ ? 1 : 0) << "\n";
            file << "distance=" << screenDistance_ << "\n";
            file << "download_dir=" << downloadDirIdx_ << "\n";
            file << "rail_shortcuts=" << railShortcutMask_ << "\n";
            for (const Shortcut& sc : shortcuts_) {
                file << "shortcut=" << (sc.onRail ? 1 : 0) << "|" << (sc.kiosk ? 1 : 0) << "|" << sc.name << "|" << sc.url << "\n";
            }
            file << "audio_lang=" << audioLangIdx_ << "\n";
            file << "curvature=" << curvatureIdx_ << "\n";
            file << "ui_text_scale=" << uiTextScaleIdx_ << "\n";
            file << "ui_text_color=" << uiTextColorIdx_ << "\n";
            file << "ui_accent_color=" << uiAccentColorIdx_ << "\n";
            file << "ui_distance=" << uiDistanceIdx_ << "\n";
            file << "ui_follow_mode=" << uiFollowMode_ << "\n";
            file << "browser_view_mode=" << browserViewMode_ << "\n";
            file << "list_show_format=" << (listShowFormat_ ? 1 : 0) << "\n";
            file << "list_show_res=" << (listShowRes_ ? 1 : 0) << "\n";
            file << "list_show_size=" << (listShowSize_ ? 1 : 0) << "\n";
            file << "list_show_dur=" << (listShowDuration_ ? 1 : 0) << "\n";
            file << "hover_haptics=" << (hoverHapticsEnabled_ ? 1 : 0) << "\n";
            file << "hover_haptics_strength=" << hoverHapticsStrengthIdx_ << "\n";
            file << "ambilight=" << ambilightIdx_ << "\n";
            file << "ambilight_all=" << (ambilightAllScreens_ ? 1 : 0) << "\n";
            file << "laser_smoothing=" << laserSmoothing_ << "\n";
            file << "room=" << roomIdx_ << "\n";
            file << "panel_curve=" << panelCurveIdx_ << "\n";
            file << "panel_style=" << uiOpacityIdx_ << "\n";
            file << "multi_screen=" << (multiScreen_ ? 1 : 0) << "\n";
            file << "pause_on_removal=" << (pauseOnRemoval_ ? 1 : 0) << "\n";
            file << "keyboard_layout=" << keyboardLayout_ << "\n";
            file << "pause_livestreams=" << (pauseLivestreams_ ? 1 : 0) << "\n";
            file << "private_browsing=" << (privateBrowsing_ ? 1 : 0) << "\n";
            file << "screen_numbers=" << (screenNumbers_ ? 1 : 0) << "\n";
            file << "audio_follow=" << (audioFollow_ ? 1 : 0) << "\n";
            file << "browser_sort=" << browserSort_ << "\n";
            if (!quickPath_.empty()) {
                file << "quick_path=" << quickPath_ << "\n";
            }
        }
    } catch (const std::exception& e) {
        PIXELVR_LOG_WARN("Could not save settings: %s", e.what());
    }
}

} // namespace pixelvr

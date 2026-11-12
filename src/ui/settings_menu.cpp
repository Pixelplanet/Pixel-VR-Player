#include "ui/settings_menu.hpp"
#include "util/sdcard.hpp"
#include "app/vault_manager.hpp"
#include "net/smb_credentials.hpp"
#include "plugins/plugin_host.hpp"
#include "plugins/plugin_repo.hpp"
#include "xr/vr_keyboard.hpp"

#include "util/logging.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
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
const char* kDownloadDirs[] = {"~/media", "~/Downloads", "~/Videos", "SD card"};  // SD card: <card>/Downloads
constexpr int kDownloadDirCount = 4;

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

const std::vector<PluginHost::Plugin>& pluginList(const PluginHost* host) {
    static const std::vector<PluginHost::Plugin> none;
    return host ? host->plugins() : none;
}

} // namespace

SettingsMenu::SettingsMenu() {
    loadConfig();
    ensureShortcuts();
    refresh();
}

void SettingsMenu::measureVault() {
    vaultSizeBytes_ = 0;
    vaultFileCount_ = 0;
    const std::string dir = VaultManager::instance().vaultDir();
    if (dir.empty()) return;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(
             dir, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        std::error_code ec2;
        if (!it->is_regular_file(ec2)) continue;
        const std::string name = it->path().filename().string();
        if (!name.empty() && name[0] == '.') continue;
        vaultSizeBytes_ += it->file_size(ec2);
        ++vaultFileCount_;
    }
}

void SettingsMenu::open() {
    open_ = true;
    // Sub-pages (track lists, list columns) reopen at their parent section; the
    // Vault section is only reachable while the vault is unlocked.
    if (section_ == kPluginSection) section_ = kPluginsSection;  // a plugin's page reopens at the plugin list
    if (section_ == kVaultMoveSection) section_ = 4;
    if (section_ == kPluginsSection && plugins_) plugins_->rescan();
    if (section_ < 0 || (section_ > 4 && section_ != kPluginsSection) ||
        (section_ == 4 && !VaultManager::instance().isUnlocked())) {
        section_ = 0;
    }
    refresh();
}

std::string SettingsMenu::editingPluginText() const {
    if (!plugins_ || editingPluginSetting_ < 0 || pluginIdx_ >= static_cast<int>(plugins_->plugins().size())) return {};
    const auto& p = plugins_->plugins()[static_cast<size_t>(pluginIdx_)];
    if (editingPluginSetting_ >= static_cast<int>(p.settings.size())) return {};
    const auto& s = p.settings[static_cast<size_t>(editingPluginSetting_)];
    const auto it = p.values.find(s.key);
    return it == p.values.end() ? s.def : it->second;
}

void SettingsMenu::submitPluginText(const std::string& text) {
    if (plugins_ && editingPluginSetting_ >= 0) plugins_->setTextSetting(pluginIdx_, editingPluginSetting_, text);
    editingPluginSetting_ = -1;
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
    // The plugin pages' texts belong to them.
    if (pluginStatusShown_ && !onPluginPage()) status_ = kDefaultStatus;
    pluginStatusShown_ = onPluginPage();
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
    auto binding = [&](const std::string& label, const std::string& value, Action act, bool enabled = true) {
        Row r{label, value, act, enabled};
        r.kind = Kind::Binding;
        rows_.push_back(r);
    };
    auto color = [&](const std::string& label, const std::string& value, int steps,
                     int idx, Action act, bool enabled = true) {
        Row r{label, value, act, enabled};
        r.kind = Kind::Color;
        r.steps = steps;
        r.stepIdx = idx;
        rows_.push_back(r);
    };
    auto segmented = [&](const std::string& label, std::vector<std::string> opts, int idx,
                         Action act, bool enabled = true) {
        Row r{label, opts.empty() ? std::string() : opts[static_cast<std::size_t>(idx) % opts.size()], act, enabled};
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
        segmented("Glow Around", {"Selected Screen", "All Screens"}, ambilightAllScreens_ ? 1 : 0,
                  Action::CycleAmbilightScope,
                  projectionMode_ == 0 && multiScreen_ && (ambilightIdx_ % kAmbilightCount > 0));
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
        toggle("Remove Black Bars", removeBlackBars_, Action::ToggleRemoveBlackBars);
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
        segmented("Web Browser Mode", {"Regular", "Incognito"}, privateBrowsing_ ? 1 : 0,
                  Action::TogglePrivateBrowsing);
        {
            // The SD card is offered while one is inserted; the label shows the free space where files go.
            const bool card = !sdCardMount().empty();
            const bool custom = !downloadCustom_.empty();
            const bool sdChosen = !custom && downloadDirIdx_ % kDownloadDirCount == 3;
            std::vector<std::string> opts{kDownloadDirs[0], kDownloadDirs[1], kDownloadDirs[2]};
            if (card || sdChosen) opts.push_back(kDownloadDirs[3]);
            opts.push_back("Other...");
            const int idx = custom ? static_cast<int>(opts.size()) - 1 : downloadDirIdx_ % kDownloadDirCount;
            const std::string dir = downloadDir();
            const uint64_t freeB = freeBytes(std::filesystem::exists(dir) ? dir : std::filesystem::path(dir).parent_path().string());
            const std::string freeTxt = freeB ? " (" + gigabytes(freeB) + " free)" : std::string();
            std::string label;
            if (sdChosen && !card) {
                label = "Download Folder (no SD card: saving to ~/Downloads)";
            } else if (custom) {
                std::string leaf = std::filesystem::path(downloadCustom_).filename().string();
                if (leaf.empty()) leaf = downloadCustom_;
                label = removableMissing(downloadCustom_) ? "Download Folder: " + leaf + " (drive removed)"
                                                          : "Download Folder: " + leaf + freeTxt;
            } else {
                label = "Download Folder" + freeTxt;
            }
            segmented(label, opts, idx, Action::CycleDownloadDir);
        }
        {
            std::string on;
            for (const RailShortcut& r : railShortcuts()) on += (on.empty() ? "" : ", ") + r.name;
            rows_.push_back({"Shortcuts", on.empty() ? "None on the rail" : on, Action::EnterRailShortcuts});
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
    } else if (section_ == kPluginsSection) {
        title_ = "Settings > Plugins";
        const auto& list = pluginList(plugins_);
        for (size_t i = 0; i < list.size(); ++i) {
            Row r{list[i].name + (list[i].version.empty() ? "" : " " + list[i].version),
                  list[i].enabled || !list[i].manifestError.empty() ? list[i].status : std::string("Off"), Action::OpenPlugin};
            r.kind = Kind::PluginLine;
            r.data = static_cast<int>(i);
            if (repo_) {
                for (const auto& e : repo_->entries()) {
                    if (e.id == list[i].id && PluginHost::compareVersions(e.version, list[i].version) > 0) r.options = {"update"};
                }
            }
            r.stepIdx = list[i].enabled ? 1 : 0;
            r.steps = pluginRemoveArmed_ == static_cast<int>(i) ? 1 : 0;
            rows_.push_back(r);
        }
        std::vector<std::string> offeredIds;  // id@version offered from Downloads
        // Plugin packages waiting in the download folders (and their subfolders): one press installs.
        pluginPackages_.clear();
        {
            std::vector<std::string> roots{downloadDir()};
            if (const char* home = std::getenv("HOME")) roots.push_back(std::string(home) + "/Downloads");
            auto isPkg = [](const std::filesystem::path& f) {
                std::string e = f.extension().string();
                for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                return e == ".pvrplugin";
            };
            for (size_t r = 0; r < roots.size(); ++r) {
                if (std::find(roots.begin(), roots.begin() + static_cast<long>(r), roots[r]) != roots.begin() + static_cast<long>(r)) continue;
                std::error_code ec;
                for (const auto& e : std::filesystem::directory_iterator(roots[r], ec)) {
                    std::error_code ec2;
                    if (e.is_regular_file(ec2) && isPkg(e.path())) pluginPackages_.push_back(e.path().string());
                    if (e.is_directory(ec2)) {
                        for (const auto& f : std::filesystem::directory_iterator(e.path(), ec2)) {
                            std::error_code ec3;
                            if (f.is_regular_file(ec3) && isPkg(f.path())) pluginPackages_.push_back(f.path().string());
                        }
                    }
                }
            }
            std::sort(pluginPackages_.begin(), pluginPackages_.end());
            if (pluginPackages_.size() > 8) pluginPackages_.resize(8);
        }
        {
            std::vector<std::string> offered;
            offeredIds.clear();
            for (const std::string& file : pluginPackages_) {
                PluginHost::PackageInfo info;
                if (!plugins_ || !plugins_->probePackage(file, info)) continue;  // not a readable package
                const PluginHost::Plugin* have = nullptr;
                for (const auto& pl : list) {
                    if (pl.id == info.id) have = &pl;
                }
                if (have && PluginHost::compareVersions(info.version, have->version) <= 0) continue;  // installed
                const std::filesystem::path f(file);
                Row r{have ? "Update " + info.name + " " + have->version + " to " + info.version
                           : "Install " + info.name + " " + info.version,
                      "From " + f.parent_path().filename().string(), Action::InstallPluginPackage};
                r.data = static_cast<int>(offered.size());
                offered.push_back(file);
                offeredIds.push_back(info.id + "@" + info.version);
                rows_.push_back(r);
            }
            pluginPackages_ = std::move(offered);
        }
        if (repo_) repo_->refresh();  // for the update icons and the Install plugins page
        status_ = list.empty() && pluginPackages_.empty()
                      ? "No plugins yet: press Install plugins (top right) to add some."
                  : list.empty() ? "Plugin files found in Downloads: install one below, or press Install plugins."
                                 : "Switch plugins on and off here. Install plugins (top right) adds more.";
    } else if (section_ == kStoreSection || section_ == kStoreItemSection || section_ == kVaultMoveSection) {
        // Install plugins, a plugin's details and its update share one dialog layout: a text panel
        // (paragraph rows) and buttons below it (the main one in the accent colour).
        auto para = [&](const std::string& t, bool heading = false) {
            Row r{t, "", Action::None, false};
            r.kind = Kind::Paragraph;
            r.stepIdx = heading ? 1 : 0;
            rows_.push_back(r);
        };
        auto button = [&](const std::string& t, Action a, bool primary, int data = 0, bool enabled = true) {
            Row r{t, "", a, enabled};
            r.kind = Kind::Button;
            r.stepIdx = primary ? 1 : 0;
            r.data = data;
            rows_.push_back(r);
        };
        // Wraps at ~84 characters (** markers don't count). Lines after the first of a "- " or "1. " item
        // start with two spaces (drawn indented); a bold run split by the wrap is closed and reopened.
        auto wrapped = [&](const std::string& text, std::vector<std::string>& out) {
            const bool item = text.rfind("- ", 0) == 0 ||
                              (text.size() > 2 && std::isdigit(static_cast<unsigned char>(text[0])) && text.find(". ") < 3);
            std::string line, word;
            size_t visible = 0;
            bool bold = false;
            auto vis = [](const std::string& w) {
                size_t n = w.size();
                for (size_t at = w.find("**"); at != std::string::npos; at = w.find("**", at + 2)) n -= 2;
                return n;
            };
            auto flush = [&] {
                if (line.empty()) return;
                if (bold) line += "**";
                out.push_back(line);
                line = (item ? "  " : "") + std::string(bold ? "**" : "");
                visible = item ? 2 : 0;
            };
            for (size_t i = 0; i <= text.size(); ++i) {
                const char ch = i < text.size() ? text[i] : ' ';
                if (ch != ' ') {
                    word += ch;
                    continue;
                }
                if (word.empty()) continue;
                const size_t wv = vis(word);
                if (visible + wv + 1 > 84 && visible > (item ? 2u : 0u)) flush();
                const bool lead = line.empty() || line == "  " || line == "**" || line == "  **";
                line += (lead ? "" : " ") + word;
                visible += wv + (lead ? 0 : 1);
                for (size_t at = word.find("**"); at != std::string::npos; at = word.find("**", at + 2)) bold = !bold;
                word.clear();
            }
            if (!line.empty() && line != "  " && line != "**" && line != "  **") {
                out.push_back(line);
            }
        };
        const auto& list = pluginList(plugins_);
        const std::vector<PluginRepo::Entry> entries = repo_ ? repo_->entries() : std::vector<PluginRepo::Entry>{};
        if (section_ == kStoreSection) {
            title_ = "Settings > Plugins > Install plugins";
            int shown = 0;
            if (repo_) repo_->refresh();
            for (size_t ei = 0; ei < entries.size(); ++ei) {
                const PluginRepo::Entry& e = entries[ei];
                bool have = false;
                for (const auto& pl : list) have = have || pl.id == e.id;
                if (have) continue;  // installed ones are on the Plugins page (updates: the green arrow)
                Row r{e.name, repo_->downloading(e.id) ? std::string("Downloading...") : (e.summary.empty() ? e.description : e.summary),
                      Action::OpenStorePlugin};
                r.data = static_cast<int>(ei);
                rows_.push_back(r);
                ++shown;
            }
            if (shown == 0) {
                const std::string st = repo_ ? repo_->state() : std::string("The plugin repository is not available in this build.");
                if (!st.empty() && entries.empty()) {
                    para(st, true);
                    para("Check that the headset is online. The list is loaded again in a minute.");
                } else {
                    para("You have every available plugin installed", true);
                    para("New plugins show up here as soon as they are published. Updates for your plugins");
                    para("appear as a **green arrow** on the Plugins page.");
                    para("");
                    para("Missing a plugin? Request it at **github.com/Pixelplanet/Pixel-VR-Player-Plugins**");
                }
            }
            status_ = shown ? "Pick a plugin to see what it does. New plugins start switched off." : "";
            button("Back to Plugins", Action::BackToPlugins, false);
        } else if (section_ == kStoreItemSection) {
            const PluginRepo::Entry* e = nullptr;
            int ei = -1;
            for (size_t i = 0; i < entries.size(); ++i) {
                if (entries[i].id == storeId_) e = &entries[i], ei = static_cast<int>(i);
            }
            if (!e) {
                section_ = kPluginsSection;
                refresh();
                return;
            }
            const PluginHost::Plugin* have = nullptr;
            for (const auto& pl : list) {
                if (pl.id == e->id) have = &pl;
            }
            const bool update = storeUpdate_ && have;
            std::vector<std::pair<std::string, bool>> text;  // line, heading
            auto add = [&](const std::string& t, bool heading) {
                std::vector<std::string> ls;
                wrapped(t, ls);
                for (const auto& l : ls) text.push_back({l, heading});
            };
            if (update) {
                title_ = "Settings > Plugins > Update " + e->name;
                status_ = "Installed: " + have->version + "   |   Available: " + e->version;
                for (const auto& ch : e->changelog) {
                    if (PluginHost::compareVersions(ch.first, have->version) <= 0) continue;
                    if (!text.empty()) text.push_back({"", false});
                    add("What's new in " + ch.first, true);
                    for (const std::string& l : ch.second) add("- " + l, false);
                }
                if (text.empty()) add("No change notes were published for this update.", false);
            } else {
                title_ = "Settings > Plugins > Install plugins > " + e->name;
                status_ = e->name + " " + e->version;
                add("What it does", true);
                for (const std::string& l : e->about) add(l, false);
                if (e->about.empty()) add(e->description, false);
                if (!e->howto.empty()) {
                    text.push_back({"", false});
                    add("How to use it", true);
                    int n = 1;
                    for (const std::string& l : e->howto) add(std::to_string(n++) + ". " + l, false);
                }
            }
            for (size_t i = 0; i < text.size() && i < 15; ++i) para(text[i].first, text[i].second);
            const bool busy = repo_->downloading(e->id);
            button(busy ? "Downloading..." : (update ? "Update to " + e->version : "Install"), Action::InstallRepoPlugin, true, ei, !busy);
            button("Back", update ? Action::BackToPlugins : Action::OpenStore, false);
        } else if (section_ == kVaultMoveSection) {
            VaultManager& vm = VaultManager::instance();
            const bool onCard = vm.location() == VaultManager::Location::SdCard;
            const std::string card = sdCardMount();
            const bool moving = !vaultMoveStatus_.empty();
            auto fmtBytes = [](uint64_t b) -> std::string {
                char buf[48];
                if (b == 0) return "0 MB";
                if (b < (1ull << 20)) std::snprintf(buf, sizeof(buf), "%.0f KB", static_cast<double>(b) / 1024.0);
                else if (b < (1ull << 30)) std::snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(b) / static_cast<double>(1ull << 20));
                else std::snprintf(buf, sizeof(buf), "%.2f GB", static_cast<double>(b) / static_cast<double>(1ull << 30));
                return buf;
            };
            const uint64_t internalFree = freeBytes(vm.internalVaultDir());
            const uint64_t cardFree = card.empty() ? 0 : freeBytes(card);
            const uint64_t destFree = onCard ? internalFree : cardFree;
            const uint64_t neededBytes = vaultSizeBytes_ + (64ull << 20);
            const bool enoughSpace = destFree == 0 || destFree >= neededBytes;
            const bool canMove = !moving && vm.available() && (onCard || !card.empty()) && !vm.migrating() && enoughSpace;
            title_ = "Settings > Vault & Privacy > Move Vault";
            std::vector<std::pair<std::string, bool>> text;
            auto add = [&](const std::string& t, bool heading) {
                std::vector<std::string> ls;
                wrapped(t, ls);
                for (const auto& l : ls) text.push_back({l, heading});
            };
            add(moving ? "Moving Encrypted Vault..."
                       : (onCard ? "Move Vault to Headset (Internal Storage)" : "Move Vault to SD Card"), true);
            add("- **Current Location:** " + std::string(onCard ? "SD Card" : "Headset (Internal Storage)"), false);
            add("- **Destination:** " + std::string(onCard ? "Headset (Internal Storage)"
                                                           : (card.empty() ? "SD Card (not inserted)" : "SD Card (" + card + ")")), false);
            add("- **Vault Size:** " + fmtBytes(vaultSizeBytes_) + " (" + std::to_string(vaultFileCount_) +
                    (vaultFileCount_ == 1 ? " encrypted file)" : " encrypted files)"), false);
            add("- **Headset Free Space:** " + (internalFree ? fmtBytes(internalFree) + " available" : std::string("Unknown")), false);
            add("- **SD Card Free Space:** " + (card.empty() ? std::string("No SD card inserted")
                                                             : (cardFree ? fmtBytes(cardFree) + " available" : std::string("Unknown"))), false);
            text.push_back({"", false});
            add("- **Headset Auto-Lock:** Automatically paused while the vault moves, so you can take the headset off and set it down without interrupting the transfer.", false);
            add("- **Security:** All files remain AES-256 encrypted on the destination drive; your unlock key stays on the headset.", false);
            if (moving) {
                text.push_back({"", false});
                add("- **Progress:** " + vaultMoveStatus_, false);
                hint(vaultMoveStatus_);
            } else if (!enoughSpace) {
                text.push_back({"", false});
                add("- **Warning:** Not enough free space on the destination drive (needs at least " + fmtBytes(neededBytes) + ").", false);
                hint("Not enough free space on destination drive.");
            } else if (!vaultMoveNotice_.empty() && std::chrono::steady_clock::now() < vaultMoveNoticeUntil_) {
                text.push_back({"", false});
                add("- **Status:** " + vaultMoveNotice_, false);
                hint(vaultMoveNotice_);
            } else {
                hint("Choose whether the vault should lock automatically once the transfer finishes.");
            }
            for (size_t i = 0; i < text.size() && i < 15; ++i) para(text[i].first, text[i].second);
            if (moving) {
                button("Cancel Move", Action::CancelVaultMove, true, 0, true);
                button("Back", Action::BackToVault, false, 0, true);
            } else {
                button("Move & Lock When Done", Action::StartVaultMoveLock, true, 0, canMove);
                button("Move & Stay Unlocked", Action::StartVaultMoveUnlock, false, 0, canMove);
                button("Back", Action::BackToVault, false, 0, true);
            }
        }
    } else if (section_ == kPluginSection) {
        const auto& list = pluginList(plugins_);
        if (pluginIdx_ < 0 || pluginIdx_ >= static_cast<int>(list.size())) {
            section_ = kPluginsSection;
            refresh();
            return;
        }
        const PluginHost::Plugin& p = list[static_cast<size_t>(pluginIdx_)];
        title_ = "Settings > Plugins > " + p.name + (p.version.empty() ? "" : " " + p.version);
        status_ = p.status + (p.description.empty() ? "" : "  |  " + p.description);
        std::vector<Row> lines;
        if (!p.manifestError.empty()) {
            lines.push_back({"Cannot run: " + p.manifestError, "", Action::None, false});
        } else {
            Row en{"Enabled", p.enabled ? "On" : "Off", Action::TogglePluginEnabled};
            lines.push_back(en);
            for (size_t si = 0; si < p.settings.size(); ++si) {
                const PluginHost::Setting& st = p.settings[si];
                std::string value = plugins_->settingValueLabel(pluginIdx_, static_cast<int>(si));
                if (st.type == "action") value = p.enabled ? "Press to run" : "Turn the plugin on first";
                Row r{st.label, value, st.type == "text" ? Action::EditPluginText : Action::PluginSetting,
                      p.enabled};
                r.data = static_cast<int>(si);
                lines.push_back(r);  // plain lines: a press cycles the value / runs the action / edits the text
            }
            if (VaultManager::instance().isUnlocked() && p.hasPermission("playback")) {
                lines.push_back({"Use with vault videos", p.vaultGrant ? "On" : "Off", Action::PluginVaultGrant, p.enabled});
            }
            lines.push_back({"Restart plugin", p.pid > 0 ? "" : "not running", Action::PluginRestart, p.pid > 0});
        }
        std::string perms;
        for (const std::string& perm : p.permissions) perms += (perms.empty() ? "" : ", ") + perm;
        lines.push_back({"Permissions", perms.empty() ? "none" : perms, Action::None, false});
        constexpr int kPerPage = 11;  // + "Back to Plugins" = the 12 compact lines
        pluginPages_ = std::max(1, (static_cast<int>(lines.size()) + kPerPage - 1) / kPerPage);
        pluginPage_ = std::clamp(pluginPage_, 0, pluginPages_ - 1);
        for (int i = pluginPage_ * kPerPage; i < std::min(static_cast<int>(lines.size()), (pluginPage_ + 1) * kPerPage); ++i) {
            rows_.push_back(lines[static_cast<size_t>(i)]);
        }
        rows_.push_back({"Back to Plugins", "", Action::BackToPlugins});
        if (!pluginNotice_.empty() && std::chrono::steady_clock::now() < pluginNoticeUntil_) status_ = pluginNotice_;
        else if (pluginPages_ > 1) status_ = "Page " + std::to_string(pluginPage_ + 1) + " of " + std::to_string(pluginPages_) + ": " + status_;
    } else if (section_ == 4) {
        title_ = "Settings > Vault & Privacy";
        toggle("Auto-Lock on Headset Removal", VaultManager::instance().autoLockHeadsetOff(),
               Action::CycleVaultAutoLockHeadset);
        toggle("Incognito Browser While Vault Open", vaultForceIncognito_, Action::ToggleVaultForceIncognito);

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

        VaultManager& vm = VaultManager::instance();
        {
            // SD2: the vault on the SD card (key, combination and thumbnails stay on the headset).
            const bool onCard = vm.location() == VaultManager::Location::SdCard;
            const std::string card = sdCardMount();
            const bool moving = !vaultMoveStatus_.empty();
            const bool canOpenMove = moving || (onCard ? vm.available() : !card.empty());
            const std::string label = onCard ? "Vault Location: SD Card" : "Vault Location: Headset";
            const std::string value = moving ? "Moving... (Options / Cancel)"
                : (onCard ? (vm.available() ? "Move to Headset..." : "SD card not inserted")
                          : (!card.empty() ? "Move to SD Card..." : "No SD card inserted"));
            rows_.push_back({label, value, Action::MoveVault, canOpenMove});
        }
        // One status line: the most important thing wins.
        std::string rec = vm.recoveryFile();
        if (const char* home = std::getenv("HOME"); home != nullptr && *home && rec.rfind(home, 0) == 0) {
            rec = "~" + rec.substr(std::strlen(home));
        }
        if (!vaultMoveStatus_.empty()) {
            hint(vaultMoveStatus_);
        } else if (!vaultMoveNotice_.empty() && std::chrono::steady_clock::now() < vaultMoveNoticeUntil_) {
            hint(vaultMoveNotice_);
        } else if (vm.location() == VaultManager::Location::SdCard && !vm.available()) {
            hint("The vault is on the SD card: insert it to see your files.");
        } else if (vm.migrating() && vm.migrationTotal() > 0) {
            hint("Encrypting vault files in the background: " + std::to_string(vm.migrationDone()) + " / " +
                 std::to_string(vm.migrationTotal()) + ". The vault stays usable meanwhile.");
        } else {
            hint("Files are encrypted (AES-256). Forgot the combination? It is written down in " + rec + ".");
        }
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
        // One line per shortcut, in rail order: grab handle, on / off, name, address, Edit, Delete.
        hint("Hold a shortcut's handle and drag it to reorder. The rail shows the first " +
             std::to_string(kMaxRailShortcuts) + " that are on.");
        const bool armed = deleteArmedIdx_ >= 0 && std::chrono::steady_clock::now() < deleteArmedUntil_;
        for (size_t i = 0; i < shortcuts_.size(); ++i) {
            const Shortcut& sc = shortcuts_[i];
            std::string where = sc.url;
            if (const size_t p = where.find("://"); p != std::string::npos) where = where.substr(p + 3);
            while (!where.empty() && where.back() == '/') where.pop_back();
            Row r{sc.name, where, Action::OpenShortcut};
            r.kind = Kind::Shortcut;
            r.data = static_cast<int>(i);
            r.stepIdx = sc.onRail ? 1 : 0;
            r.steps = (armed && deleteArmedIdx_ == static_cast<int>(i)) ? 1 : 0;
            rows_.push_back(r);
        }
        {
            const bool full = shortcuts_.size() >= static_cast<size_t>(kMaxShortcuts);
            Row add{"Add shortcut", full ? "The list is full" : "Name and address", Action::AddShortcut};
            add.enabled = !full;
            rows_.push_back(add);
        }
        rows_.push_back({"Back to UI & Appearance", "", Action::BackToUI});
    } else if (section_ == 17) {
        if (shortcutIdx_ < 0 || shortcutIdx_ >= static_cast<int>(shortcuts_.size())) shortcutIdx_ = 0;
        const Shortcut& sc = shortcuts_.empty() ? Shortcut{} : shortcuts_[static_cast<size_t>(shortcutIdx_)];
        title_ = "Settings > UI & Appearance > Shortcuts > " + sc.name;
        hint("Name and address open the keyboard.");
        rows_.push_back({"Name", sc.name, Action::EditShortcutName});
        rows_.push_back({"Address", sc.url.empty() ? std::string("Asked on first use") : sc.url, Action::EditShortcutUrl});
        toggle("Show on the Rail", sc.onRail, Action::ToggleShortcutRail);
        segmented("Opens as", {"Page only", "Full browser"}, sc.kiosk ? 0 : 1, Action::CycleShortcutKiosk);
        {
            const bool armed = deleteArmedIdx_ == shortcutIdx_ && std::chrono::steady_clock::now() < deleteArmedUntil_;
            rows_.push_back({"Delete Shortcut", armed ? "Press again to delete" : "Asks once more first", Action::RemoveShortcut});
        }
        rows_.push_back({"Back to Shortcuts", "", Action::BackToShortcuts});
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
    if (!rows_[static_cast<size_t>(selected_)].enabled) {
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
        case Action::CycleHeatStyle:
            heatStyle_ = (heatStyle_ + 1) % 2;
            saveConfig();
            refresh();
            break;
        case Action::InstallRepoPlugin:
            repoEntryToInstall_ = rows_[static_cast<size_t>(selected_)].data;  // index into the repository entries
            break;  // the app downloads and installs it
        case Action::OpenStore:
            openPluginStore();
            break;
        case Action::OpenStorePlugin: {
            const int i = rows_[static_cast<size_t>(selected_)].data;
            const auto entries = repo_ ? repo_->entries() : std::vector<PluginRepo::Entry>{};
            if (i >= 0 && i < static_cast<int>(entries.size())) {
                storeId_ = entries[static_cast<size_t>(i)].id;
                storeUpdate_ = false;
                section_ = kStoreItemSection;
                selected_ = 0;
                refresh();
            }
            break;
        }
        case Action::ToggleRemoveBlackBars:
            removeBlackBars_ = !removeBlackBars_;
            saveConfig();
            refresh();
            break;
        case Action::InstallPluginPackage: {
            const int i = rows_[static_cast<size_t>(selected_)].data;
            pluginPackageToInstall_ = (i >= 0 && i < static_cast<int>(pluginPackages_.size()))
                                          ? pluginPackages_[static_cast<size_t>(i)]
                                          : std::string();
            break;  // the app installs it, then reopens this page with the result
        }
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
            if (i < static_cast<int>(shortcuts_.size())) shortcuts_[static_cast<size_t>(i)].onRail ^= true;
            saveConfig();
            refresh();
            break;
        }
        case Action::OpenShortcut:
            shortcutIdx_ = rows_[static_cast<size_t>(selected_)].data;
            section_ = 17;
            selected_ = 0;
            refresh();
            break;
        case Action::AddShortcut:
            if (shortcuts_.size() >= static_cast<size_t>(kMaxShortcuts)) break;
            shortcuts_.push_back({"New shortcut", "", false, true});
            shortcutIdx_ = static_cast<int>(shortcuts_.size()) - 1;
            saveConfig();
            section_ = 17;
            selected_ = 0;  // Name: the app opens the keyboard for it at once
            refresh();
            break;
        case Action::ToggleShortcutRail:
        case Action::CycleShortcutKiosk:
        case Action::RemoveShortcut:
        case Action::MoveShortcutUp:
        case Action::MoveShortcutDown: {
            if (shortcutIdx_ < 0 || shortcutIdx_ >= static_cast<int>(shortcuts_.size())) break;
            const size_t i = static_cast<size_t>(shortcutIdx_);
            if (act == Action::ToggleShortcutRail) shortcuts_[i].onRail ^= true;
            if (act == Action::CycleShortcutKiosk) shortcuts_[i].kiosk ^= true;
            if (act == Action::MoveShortcutUp && i > 0) {
                std::swap(shortcuts_[i], shortcuts_[i - 1]);
                --shortcutIdx_;
            }
            if (act == Action::MoveShortcutDown && i + 1 < shortcuts_.size()) {
                std::swap(shortcuts_[i], shortcuts_[i + 1]);
                ++shortcutIdx_;
            }
            if (act == Action::RemoveShortcut) {
                if (shortcutDelete(shortcutIdx_)) {
                    section_ = 22;
                    selected_ = std::min(static_cast<int>(i), static_cast<int>(shortcuts_.size()));
                }
                refresh();
                break;  // shortcutDelete() saved already
            }
            saveConfig();
            refresh();
            break;
        }
        case Action::BackToShortcuts:
            section_ = 22;
            selected_ = shortcutIdx_;
            refresh();
            break;
        case Action::CycleDownloadDir:
            if (!downloadCustom_.empty()) {
                downloadCustom_.clear();  // from "Other..." back to the first preset
                downloadDirIdx_ = 0;
            } else {
                downloadDirIdx_ = (downloadDirIdx_ + 1) % kDownloadDirCount;
                if (downloadDirIdx_ == 3 && sdCardMount().empty()) downloadDirIdx_ = 0;  // no card: skip it
            }
            saveConfig();
            refresh();
            break;
        case Action::MoveVault:
            vaultMoveArmed_ = false;
            measureVault();
            section_ = kVaultMoveSection;
            selected_ = 0;
            refresh();
            break;
        case Action::BackToVault:
            section_ = 4;
            selected_ = 4;
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
        case Action::ToggleVaultForceIncognito:
            vaultForceIncognito_ = !vaultForceIncognito_;
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
        case Action::EnterPlugins:
        case Action::BackToPlugins:
            if (plugins_) plugins_->rescan();
            section_ = kPluginsSection;
            selected_ = 0;
            refresh();
            break;
        case Action::BackToControls:
            section_ = 3;
            selected_ = 0;
            refresh();
            break;
        case Action::OpenPlugin:
            pluginIdx_ = rows_[static_cast<size_t>(selected_)].data;
            pluginPage_ = 0;
            section_ = kPluginSection;
            selected_ = 0;
            refresh();
            break;
        case Action::TogglePluginEnabled:
            if (plugins_ && pluginIdx_ < static_cast<int>(plugins_->plugins().size())) {
                plugins_->setEnabled(pluginIdx_, !plugins_->plugins()[static_cast<size_t>(pluginIdx_)].enabled);
            }
            refresh();
            break;
        case Action::PluginSetting:
            if (plugins_) plugins_->activateSetting(pluginIdx_, rows_[static_cast<size_t>(selected_)].data);
            refresh();
            break;
        case Action::EditPluginText:
            editingPluginSetting_ = rows_[static_cast<size_t>(selected_)].data;
            break;
        case Action::PluginVaultGrant:
            if (plugins_ && pluginIdx_ < static_cast<int>(plugins_->plugins().size())) {
                plugins_->setVaultGrant(pluginIdx_, !plugins_->plugins()[static_cast<size_t>(pluginIdx_)].vaultGrant);
            }
            refresh();
            break;
        case Action::PluginRestart:
            if (plugins_) plugins_->restart(pluginIdx_);
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
    if (!row.enabled) return Action::None;
    if (row.kind != Kind::Segmented || segment < 0 || segment >= row.steps) return activate();
    if (row.action == Action::CycleDownloadDir) {
        // The options depend on the card being in: pick by name, not by stepping.
        const std::string opt = row.options[static_cast<std::size_t>(segment)];
        if (opt == "Other...") return Action::PickDownloadFolder;
        for (int i = 0; i < kDownloadDirCount; ++i) {
            if (opt == kDownloadDirs[i]) {
                downloadDirIdx_ = i;
                downloadCustom_.clear();
                saveConfig();
                refresh();
                return Action::CycleDownloadDir;
            }
        }
        return Action::None;
    }
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
    if (section_ == kPluginSection && pluginPages_ > 1 && delta != 0) {
        int page = pluginPage_ + delta;
        if (wrap) page = ((page % pluginPages_) + pluginPages_) % pluginPages_;
        else if (page < 0 || page >= pluginPages_) return false;
        pluginPage_ = page;
        selected_ = 0;
        refresh();
        return true;
    }
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

void SettingsMenu::shortcutToggle(int idx) {
    if (idx < 0 || idx >= static_cast<int>(shortcuts_.size())) return;
    shortcuts_[static_cast<size_t>(idx)].onRail ^= true;
    saveConfig();
    refresh();
}

void SettingsMenu::shortcutMove(int from, int to) {
    const int n = static_cast<int>(shortcuts_.size());
    if (from < 0 || from >= n || to < 0 || to >= n || from == to) return;
    const Shortcut moved = shortcuts_[static_cast<size_t>(from)];
    shortcuts_.erase(shortcuts_.begin() + from);
    shortcuts_.insert(shortcuts_.begin() + to, moved);
    deleteArmedIdx_ = -1;
    selected_ = to;
    saveConfig();
    refresh();
}

bool SettingsMenu::shortcutDelete(int idx) {
    if (idx < 0 || idx >= static_cast<int>(shortcuts_.size())) return false;
    const auto now = std::chrono::steady_clock::now();
    if (deleteArmedIdx_ != idx || now >= deleteArmedUntil_) {  // first press: ask
        deleteArmedIdx_ = idx;
        deleteArmedUntil_ = now + std::chrono::seconds(4);
        refresh();  // (refresh() resets the status line to the page hint)
        status_ = "Press Delete again to remove \"" + shortcuts_[static_cast<size_t>(idx)].name + "\".";
        return false;
    }
    status_ = "Removed \"" + shortcuts_[static_cast<size_t>(idx)].name + "\".";
    shortcuts_.erase(shortcuts_.begin() + idx);
    deleteArmedIdx_ = -1;
    saveConfig();
    refresh();
    return true;
}

void SettingsMenu::shortcutEdit(int idx) {
    if (idx < 0 || idx >= static_cast<int>(shortcuts_.size())) return;
    shortcutIdx_ = idx;
    deleteArmedIdx_ = -1;
    section_ = 17;
    selected_ = 0;
    refresh();
}

void SettingsMenu::showPluginNotice(const std::string& n) {
    pluginNotice_ = n;
    pluginNoticeUntil_ = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    if (open_ && (section_ == kPluginsSection || section_ == kPluginSection)) refresh();
}

void SettingsMenu::openPluginStore() {
    section_ = kStoreSection;
    selected_ = 0;
    if (repo_) repo_->refresh(true);
    refresh();
}

void SettingsMenu::openPluginUpdate(int i) {
    if (!plugins_ || i < 0 || i >= static_cast<int>(plugins_->plugins().size())) return;
    storeId_ = plugins_->plugins()[static_cast<size_t>(i)].id;
    storeUpdate_ = true;
    section_ = kStoreItemSection;
    selected_ = 0;
    refresh();
}

void SettingsMenu::pluginToggle(int i) {
    if (!plugins_ || i < 0 || i >= static_cast<int>(plugins_->plugins().size())) return;
    plugins_->setEnabled(i, !plugins_->plugins()[static_cast<size_t>(i)].enabled);
    refresh();
}

void SettingsMenu::pluginEdit(int i) {
    if (!plugins_ || i < 0 || i >= static_cast<int>(plugins_->plugins().size())) return;
    pluginIdx_ = i;
    pluginPage_ = 0;
    pluginRemoveArmed_ = -1;
    section_ = kPluginSection;
    selected_ = 0;
    refresh();
}

bool SettingsMenu::pluginRemoveArmed(int i) {
    if (pluginRemoveArmed_ == i && std::chrono::steady_clock::now() < pluginRemoveUntil_) {
        pluginRemoveArmed_ = -1;
        return true;
    }
    pluginRemoveArmed_ = i;
    pluginRemoveUntil_ = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    refresh();
    return false;
}

void SettingsMenu::tickPlugins() {
    if (pluginRemoveArmed_ >= 0 && std::chrono::steady_clock::now() >= pluginRemoveUntil_) {
        pluginRemoveArmed_ = -1;
        if (section_ == kPluginsSection) refresh();
    }
}

void SettingsMenu::tickShortcuts() {
    if (deleteArmedIdx_ >= 0 && std::chrono::steady_clock::now() >= deleteArmedUntil_) {
        deleteArmedIdx_ = -1;
        if (section_ == 22 || section_ == 17) refresh();
    }
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

void SettingsMenu::setCustomDownloadDir(const std::string& dir) {
    downloadCustom_ = dir;
    saveConfig();
    refresh();
}

std::string SettingsMenu::downloadDir() const {
    if (!downloadCustom_.empty()) return downloadCustom_;
    const char* home = std::getenv("HOME");
    const std::string base = home ? home : ".";
    if (downloadDirIdx_ % kDownloadDirCount == 3) {
        const std::string card = sdCardMount();
        return card.empty() ? base + "/Downloads" : card + "/Downloads";  // card removed: the home Downloads
    }
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
            else if (key == "stereo") stereoMode_ = std::clamp(std::stoi(val), 0, 3);
            else if (key == "remove_bars") removeBlackBars_ = val != "0";
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
            else if (key == "download_dir_custom") downloadCustom_ = val;
            else if (key == "rail_shortcuts") railShortcutMask_ = std::stoi(val) & ((1 << kRailShortcutCount) - 1);
            else if (key == "jellyfin_url") legacyJellyfinUrl_ = val;
            else if (key == "heat_style") heatStyle_ = std::stoi(val) % 2;
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
            else if (key == "vault_force_incognito") vaultForceIncognito_ = (val == "1" || val == "true");
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
            file << "remove_bars=" << (removeBlackBars_ ? 1 : 0) << "\n";
            file << "swap_eyes=" << (swapEyes_ ? 1 : 0) << "\n";
            file << "draw_models=" << (drawModels_ ? 1 : 0) << "\n";
            file << "button_labels=" << (buttonLabels_ ? 1 : 0) << "\n";
            file << "show_stats=" << (showStats_ ? 1 : 0) << "\n";
            file << "distance=" << screenDistance_ << "\n";
            file << "download_dir=" << downloadDirIdx_ << "\n";
            if (!downloadCustom_.empty()) file << "download_dir_custom=" << downloadCustom_ << "\n";
            file << "rail_shortcuts=" << railShortcutMask_ << "\n";
            file << "heat_style=" << heatStyle_ << "\n";
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
            file << "vault_force_incognito=" << (vaultForceIncognito_ ? 1 : 0) << "\n";
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

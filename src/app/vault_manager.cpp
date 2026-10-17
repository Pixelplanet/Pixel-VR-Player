#include "app/vault_manager.hpp"
#include "util/logging.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace pixelvr {
namespace fs = std::filesystem;

namespace {

std::string getVaultConfigPath() {
#ifdef _WIN32
    const char* appData = std::getenv("APPDATA");
    if (appData) return std::string(appData) + "/PixelVR/vault.conf";
#else
    const char* home = std::getenv("HOME");
    if (home) return std::string(home) + "/.config/pixelvr/vault.conf";
#endif
    return "pixelvr_vault.conf";
}

const char* btnName(int b) {
    switch (b) {
        case XrInputState::BtnA: return "A";
        case XrInputState::BtnB: return "B";
        case XrInputState::BtnX: return "X";
        case XrInputState::BtnY: return "Y";
        case XrInputState::BtnMenu: return "Menu";
        case XrInputState::BtnView: return "View";
        case XrInputState::BtnBumper: return "Bumper";
        case XrInputState::BtnThumb: return "Thumb";
        case XrInputState::BtnGrip: return "Grip";
        default: return "?";
    }
}

static uint64_t fnv1a64(const std::string& str) {
    uint64_t hash = 14695981039346656037ULL;
    for (char c : str) {
        hash ^= static_cast<uint8_t>(c);
        hash *= 1099511628211ULL;
    }
    return hash;
}

} // namespace

VaultManager& VaultManager::instance() {
    static VaultManager s_instance;
    return s_instance;
}

VaultManager::VaultManager() {
    const char* home = std::getenv("HOME");
    if (home != nullptr) {
        vaultDir_ = std::string(home) + "/.pixelvr_vault";
    } else {
        vaultDir_ = "/home/steamos/.pixelvr_vault";
    }
    thumbsDir_ = vaultDir_ + "/.thumbs";

    // Default sequence: A -> B -> B -> A -> X -> Y
    comboSequence_ = {
        XrInputState::BtnA,
        XrInputState::BtnB,
        XrInputState::BtnB,
        XrInputState::BtnA,
        XrInputState::BtnX,
        XrInputState::BtnY
    };

    loadConfig();
}

void VaultManager::ensureDirectories() {
    std::error_code ec;
    fs::create_directories(vaultDir_, ec);
    if (!ec) {
        fs::permissions(vaultDir_, fs::perms::owner_all, fs::perm_options::replace, ec);
    }
    fs::create_directories(thumbsDir_, ec);
    if (!ec) {
        fs::permissions(thumbsDir_, fs::perms::owner_all, fs::perm_options::replace, ec);
    }
}

bool VaultManager::isVaultPath(const std::string& path) const {
    if (path.empty() || vaultDir_.empty()) return false;
    return path.rfind(vaultDir_, 0) == 0;
}

void VaultManager::lock() {
    if (!unlocked_) return;
    unlocked_ = false;
    comboIndex_ = 0;
    headsetLostTime_ = {};
    isRecordingCombo_ = false;
    recordingBuffer_.clear();
    PIXELVR_LOG_INFO("Vault locked");
}

void VaultManager::unlock() {
    ensureDirectories();
    unlocked_ = true;
    comboIndex_ = 0;
    headsetLostTime_ = {};
    lastActivity_ = std::chrono::steady_clock::now();
    PIXELVR_LOG_INFO("Vault unlocked at %s", vaultDir_.c_str());
}

VaultManager::ComboResult VaultManager::feedInput(int button, bool gripFullPressHeld,
                                                  std::chrono::steady_clock::time_point now) {
    if (!gripFullPressHeld) {
        comboIndex_ = 0;
        return ComboResult::None;
    }

    // Only face buttons participate in the cheat code
    if (button != XrInputState::BtnA && button != XrInputState::BtnB &&
        button != XrInputState::BtnX && button != XrInputState::BtnY) {
        return ComboResult::None;
    }

    using namespace std::chrono_literals;
    if (comboIndex_ > 0 && (now - lastComboInput_) > 2500ms) {
        comboIndex_ = 0;
    }
    lastComboInput_ = now;

    if (comboIndex_ < comboSequence_.size() && button == comboSequence_[comboIndex_]) {
        comboIndex_++;
        if (comboIndex_ >= comboSequence_.size()) {
            comboIndex_ = 0;
            if (unlocked_) {
                lock();
                return ComboResult::Locked;
            } else {
                unlock();
                return ComboResult::Unlocked;
            }
        }
        return ComboResult::StepMatched;
    } else {
        // Wrong button pressed while grip was held
        comboIndex_ = 0;
        return ComboResult::Failed;
    }
}

void VaultManager::recordActivity(std::chrono::steady_clock::time_point now) {
    lastActivity_ = now;
}

bool VaultManager::tick(std::chrono::steady_clock::time_point now, bool headsetFocused) {
    if (!unlocked_) return false;

    // Guard 1: Headset removed / focus lost (debounced with 3.0s threshold to avoid momentary tracking glitches)
    if (autoLockHeadsetOff_) {
        if (!headsetFocused) {
            if (headsetLostTime_.time_since_epoch().count() == 0) {
                headsetLostTime_ = now;
            } else {
                auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - headsetLostTime_).count();
                if (elapsedMs >= 3000) {
                    PIXELVR_LOG_INFO("Auto-locking vault: headset removed or session lost focus for %lld ms",
                                     static_cast<long long>(elapsedMs));
                    headsetLostTime_ = {};
                    lock();
                    return true;
                }
            }
        } else {
            headsetLostTime_ = {};
        }
    } else {
        headsetLostTime_ = {};
    }

    // Guard 2: Inactivity timeout (bypassed if autoLockTimeoutSec_ <= 0)
    if (autoLockTimeoutSec_ > 0 && lastActivity_.time_since_epoch().count() > 0) {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastActivity_).count();
        if (elapsed >= autoLockTimeoutSec_) {
            PIXELVR_LOG_INFO("Auto-locking vault: inactivity timeout (%llds >= %ds)",
                             static_cast<long long>(elapsed), autoLockTimeoutSec_);
            lock();
            return true;
        }
    }

    return false;
}

void VaultManager::cleanPublicThumbs(const std::string& path) {
    const char* home = std::getenv("HOME");
    if (!home) return;
    std::error_code ec;
    std::string pubThumbDir = std::string(home) + "/.cache/pixelvr/thumbs";
    if (!fs::exists(pubThumbDir, ec)) return;

    auto removeThumbFor = [&](const std::string& fpath) {
        const std::string baseHash = std::to_string(fnv1a64(fpath));
        std::error_code e;
        fs::remove(pubThumbDir + "/" + baseHash + ".rgba", e);
        e.clear();
        fs::remove(pubThumbDir + "/" + baseHash + ".meta", e);
    };

    if (fs::is_directory(path, ec)) {
        for (const auto& it : fs::recursive_directory_iterator(path, ec)) {
            if (it.is_regular_file()) {
                removeThumbFor(it.path().string());
            }
        }
    } else {
        removeThumbFor(path);
    }
}

std::string VaultManager::moveFileToVault(const std::string& srcPath, std::string& outNewPath) {
    if (!unlocked_) {
        return "Vault is locked";
    }
    if (srcPath.empty()) {
        return "Source path is empty";
    }
    std::error_code ec;
    if (!fs::exists(srcPath, ec)) {
        return "Path does not exist";
    }

    ensureDirectories();

    fs::path src(srcPath);
    const bool isDir = fs::is_directory(src, ec);
    std::string stem = src.stem().string();
    std::string ext = isDir ? "" : src.extension().string();
    std::string targetFilename = src.filename().string();
    fs::path dest = fs::path(vaultDir_) / targetFilename;

    // Prevent collision by appending _1, _2, etc.
    int counter = 1;
    while (fs::exists(dest, ec)) {
        targetFilename = isDir ? (stem + "_" + std::to_string(counter++))
                               : (stem + "_" + std::to_string(counter++) + ext);
        dest = fs::path(vaultDir_) / targetFilename;
    }

    // Try rename first
    fs::rename(src, dest, ec);
    if (ec) {
        ec.clear();
        if (isDir) {
            fs::copy(src, dest, fs::copy_options::recursive, ec);
            if (ec) {
                return "Failed to copy directory: " + ec.message();
            }
            fs::remove_all(src, ec);
        } else {
            fs::copy_file(src, dest, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                return "Failed to copy file: " + ec.message();
            }
            fs::remove(src, ec);
        }
    }

    // Ensure 0700 permissions
    ec.clear();
    fs::permissions(dest, fs::perms::owner_all, fs::perm_options::replace, ec);
    if (isDir) {
        for (const auto& it : fs::recursive_directory_iterator(dest, ec)) {
            std::error_code pe;
            fs::permissions(it.path(), fs::perms::owner_all, fs::perm_options::replace, pe);
        }
    }

    outNewPath = dest.string();
    PIXELVR_LOG_INFO("Moved into vault: %s -> %s", srcPath.c_str(), outNewPath.c_str());

    // Clean up any public thumbnails that existed for srcPath
    cleanPublicThumbs(srcPath);

    return "";
}

void VaultManager::setAutoLockHeadsetOff(bool enable) {
    autoLockHeadsetOff_ = enable;
    saveConfig();
}

void VaultManager::setAutoLockTimeoutSec(int sec) {
    autoLockTimeoutSec_ = sec;
    saveConfig();
}

void VaultManager::setMoveButton(int btn) {
    moveButton_ = btn;
    saveConfig();
}

std::string VaultManager::comboString() const {
    if (comboSequence_.empty()) return "None";
    std::string s;
    for (std::size_t i = 0; i < comboSequence_.size(); ++i) {
        if (i > 0) s += " ";
        s += btnName(comboSequence_[i]);
    }
    return s;
}

void VaultManager::startRecordingCombo() {
    isRecordingCombo_ = true;
    recordingBuffer_.clear();
}

void VaultManager::cancelRecordingCombo() {
    isRecordingCombo_ = false;
    recordingBuffer_.clear();
}

bool VaultManager::feedRecordButton(int button) {
    if (!isRecordingCombo_) return false;
    if (button != XrInputState::BtnA && button != XrInputState::BtnB &&
        button != XrInputState::BtnX && button != XrInputState::BtnY) {
        return false;
    }
    recordingBuffer_.push_back(button);
    if (recordingBuffer_.size() >= 6) {
        comboSequence_ = recordingBuffer_;
        isRecordingCombo_ = false;
        recordingBuffer_.clear();
        saveConfig();
        return true; // Finished recording 6-step combo
    }
    return false;
}

void VaultManager::loadConfig() {
    const std::string path = getVaultConfigPath();
    std::ifstream file(path);
    if (!file.is_open()) return;

    std::string line;
    while (std::getline(file, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);

        if (key == "auto_lock_headset_off") {
            autoLockHeadsetOff_ = (val == "1");
        } else if (key == "auto_lock_timeout_sec") {
            try { autoLockTimeoutSec_ = std::stoi(val); } catch (...) {}
        } else if (key == "move_button") {
            try { moveButton_ = std::stoi(val); } catch (...) {}
        } else if (key == "combo") {
            std::vector<int> loaded;
            std::stringstream ss(val);
            std::string item;
            while (std::getline(ss, item, ',')) {
                try { loaded.push_back(std::stoi(item)); } catch (...) {}
            }
            if (loaded.size() >= 4 && loaded.size() <= 8) {
                comboSequence_ = loaded;
            }
        }
    }
}

void VaultManager::saveConfig() const {
    const std::string path = getVaultConfigPath();
    try {
        fs::path p(path);
        if (p.has_parent_path()) {
            fs::create_directories(p.parent_path());
        }
        std::ofstream file(path);
        if (file.is_open()) {
            file << "auto_lock_headset_off=" << (autoLockHeadsetOff_ ? 1 : 0) << "\n";
            file << "auto_lock_timeout_sec=" << autoLockTimeoutSec_ << "\n";
            file << "move_button=" << moveButton_ << "\n";
            file << "combo=";
            for (std::size_t i = 0; i < comboSequence_.size(); ++i) {
                if (i > 0) file << ",";
                file << comboSequence_[i];
            }
            file << "\n";
        }
    } catch (const std::exception& e) {
        PIXELVR_LOG_WARN("Could not save vault settings: %s", e.what());
    }
}

} // namespace pixelvr

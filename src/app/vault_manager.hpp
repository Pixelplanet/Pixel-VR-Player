#pragma once

#include "xr/xr_context.hpp"

#include <chrono>
#include <string>
#include <vector>

namespace pixelvr {

class VaultManager {
public:
    enum class ComboResult {
        None,
        StepMatched,
        Unlocked,
        Locked,
        Failed
    };

    static VaultManager& instance();

    bool isUnlocked() const { return unlocked_; }
    void lock();
    void unlock();

    // Process button input during frame loop.
    // If gripFullPressHeld is true, face buttons are consumed for the combo and
    // caller should suppress them from normal UI.
    ComboResult feedInput(int button, bool gripFullPressHeld,
                          std::chrono::steady_clock::time_point now);

    // Call when any user activity occurs (sticks, triggers, buttons)
    void recordActivity(std::chrono::steady_clock::time_point now);

    // Frame tick: checks inactivity timeout and headset focus loss.
    // Returns true if the vault was locked as a result of the tick.
    bool tick(std::chrono::steady_clock::time_point now, bool headsetFocused);

    // Storage paths
    const std::string& vaultDir() const { return vaultDir_; }
    const std::string& thumbsDir() const { return thumbsDir_; }
    bool isVaultPath(const std::string& path) const;
    void ensureDirectories();

    // Move file or directory into vault (local paths only).
    // Returns empty string on success, or error message on failure.
    std::string moveFileToVault(const std::string& srcPath, std::string& outNewPath);

    // Clean up cached public thumbnails for a file or directory
    void cleanPublicThumbs(const std::string& path);

    // Settings
    bool autoLockHeadsetOff() const { return autoLockHeadsetOff_; }
    void setAutoLockHeadsetOff(bool enable);

    int autoLockTimeoutSec() const { return autoLockTimeoutSec_; }
    void setAutoLockTimeoutSec(int sec);

    int moveButton() const { return moveButton_; }
    void setMoveButton(int btn);

    const std::vector<int>& comboSequence() const { return comboSequence_; }
    std::string comboString() const;

    // Custom combo recording
    bool isRecordingCombo() const { return isRecordingCombo_; }
    void startRecordingCombo();
    void cancelRecordingCombo();
    bool feedRecordButton(int button);
    int recordedCount() const { return static_cast<int>(recordingBuffer_.size()); }

    void loadConfig();
    void saveConfig() const;

private:
    VaultManager();
    ~VaultManager() = default;

    bool unlocked_ = false;
    std::string vaultDir_;
    std::string thumbsDir_;

    // Combo state machine
    std::vector<int> comboSequence_;
    std::size_t comboIndex_ = 0;
    std::chrono::steady_clock::time_point lastComboInput_{};

    // Recording custom combo
    bool isRecordingCombo_ = false;
    std::vector<int> recordingBuffer_;

    // Settings
    bool autoLockHeadsetOff_ = true;
    int autoLockTimeoutSec_ = 0; // Disabled by default (headset face detection active)
    int moveButton_ = XrInputState::BtnY;

    std::chrono::steady_clock::time_point lastActivity_{};
    std::chrono::steady_clock::time_point headsetLostTime_{};
};

} // namespace pixelvr

#pragma once

#include "xr/xr_context.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <chrono>
#include <thread>
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

    bool isUnlocked() const { return unlocked_.load(); }
    void lock();
    void unlock();

    bool isComboEntryActive(std::chrono::steady_clock::time_point now) const {
        using namespace std::chrono_literals;
        return comboIndex_ > 0 && (now - lastComboInput_) < 2500ms;
    }

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

    // Where the vault lives: the internal ~/.pixelvr_vault, or <SD card>/.pixelvr_vault (SD2). The key
    // (vault.key), the written-down combination and the thumbnails always stay internal.
    enum class Location { Internal, SdCard };
    Location location() const;
    // The vault folder (thread-safe copy). On the SD card: the card's folder, or where it was last seen
    // while the card is out, so paths there still count as vault paths.
    std::string vaultDir() const;
    std::string internalVaultDir() const;
    // The vault folder can be used: internal always; SD card only while the card with THIS vault is in
    // (recognised by the id in <vault>/.pixelvr_vault_id).
    bool available() const;
    // Folder for a vault on the inserted SD card ("" without a card).
    static std::string cardVaultDirFor(const std::string& cardMount);
    // Encrypted thumbnails of vault files (internal, 0700).
    std::string thumbsDir() const;
    // path is the vault folder or inside it ('/' boundary).
    bool isVaultPath(const std::string& path) const;
    // A vault item: inside the vault folder, or ANY encrypted (.pvrv) file wherever it is (e.g. in a
    // folder moved out of the vault). Nothing about such paths may be remembered or shown while locked.
    static bool isPrivatePath(const std::string& path);
    // Creates the vault folder (0700) when available(); false if it can't be used (card missing).
    bool ensureDirectories();
    std::string keyFilePath() const;
    // After a relocation job copied everything (FileOps): switch to the new place and save it.
    void switchLocation(Location loc, const std::string& newDir, const std::string& cardId);
    std::string cardId() const;

    // Clean up cached public thumbnails for a file or directory
    void cleanPublicThumbs(const std::string& path);

    // Command line (install.sh --uninstall): "--vault-info" prints the vault's place and size;
    // "--vault-export <dir>" decrypts every file into <dir> (keeping folders), deleting each encrypted
    // file once its plain copy is complete. Returns the process exit code.
    static int runCli(const std::string& mode, const std::string& dest);

    // Settings
    bool autoLockHeadsetOff() const { return autoLockHeadsetOff_; }
    void setAutoLockHeadsetOff(bool enable);

    int autoLockTimeoutSec() const { return autoLockTimeoutSec_; }
    void setAutoLockTimeoutSec(int sec);

    // vault.conf still had the old in-app "File Actions Button" key (no longer
    // written); custom = it was not Y.
    bool legacyMoveButtonSeen() const { return legacyMoveButtonSeen_; }
    bool legacyMoveButtonCustom() const { return legacyMoveButtonCustom_; }

    const std::vector<int>& comboSequence() const { return comboSequence_; }
    std::string comboString() const;

    // Custom combo recording
    bool isRecordingCombo() const { return isRecordingCombo_; }
    void startRecordingCombo();
    void cancelRecordingCombo();
    bool feedRecordButton(int button);
    int recordedCount() const { return static_cast<int>(recordingBuffer_.size()); }

    // Encryption (docs: src/app/vault_crypto.hpp). Plain files found in the vault are encrypted in the
    // background after unlocking; progress for the UI.
    bool migrating() const { return migrating_.load(); }
    int migrationDone() const { return migDone_.load(); }
    int migrationTotal() const { return migTotal_.load(); }
    std::string recoveryFile() const;  // where the unlock combination is written down (user request)
    // Moving files into / out of the vault: FileOps (src/app/file_ops.hpp), on its worker thread.
    void loadConfig();
    void saveConfig() const;

private:
    VaultManager();
    ~VaultManager();  // stops the background encryption after the current file

    std::atomic<bool> unlocked_{false};
    mutable std::mutex dirMu_;     // guards the location fields (read from the scan / thumbnail / file threads)
    Location location_ = Location::Internal;
    std::string internalDir_;      // ~/.pixelvr_vault
    std::string cardDir_;          // <card>/.pixelvr_vault, last seen
    std::string cardId_;           // id written into the card vault's marker file
    std::string thumbsDir_;        // ~/.cache/pixelvr/vault-thumbs
    void writeInternalMarker() const;

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
    bool legacyMoveButtonSeen_ = false;
    bool legacyMoveButtonCustom_ = false;

    std::chrono::steady_clock::time_point lastActivity_{};
    std::chrono::steady_clock::time_point headsetLostTime_{};
    void startEncryption();      // after unlock: key + migration
    void writeRecovery() const;
    std::atomic<bool> migrating_{false};
    std::atomic<int> migDone_{0}, migTotal_{0};
    std::thread migThread_;
    std::atomic<bool> migStop_{false};  // exit: the encryption pass stops inside the current file
    mutable std::mutex cfgMu_;          // vault.conf writes (frame thread and the FileOps worker)
};

} // namespace pixelvr

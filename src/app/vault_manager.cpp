#include "app/vault_manager.hpp"
#include "app/vault_crypto.hpp"
#include "net/smb_credentials.hpp"
#include "util/file_copy.hpp"
#include "util/logging.hpp"
#include "util/sdcard.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <unistd.h>
#include <fstream>
#include <sstream>
#include <openssl/rand.h>

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

// Combo symbols are the face actions; on the default bindings they are the
// A / B / X / Y buttons, so the stored letters stay.
const char* btnName(int b) {
    switch (b) {
        case XrInputState::ComboPlay: return "A";
        case XrInputState::ComboBack: return "B";
        case XrInputState::ComboRecenter: return "X";
        case XrInputState::ComboOptions: return "Y";
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
    const std::string h = home != nullptr ? std::string(home) : std::string("/home/steamos");
    internalDir_ = h + "/.pixelvr_vault";
    // Thumbnails of vault files: encrypted, internal (also for a vault on the SD card), keyed by the
    // file's random name so they survive moves inside the vault.
    thumbsDir_ = h + "/.cache/pixelvr/vault-thumbs";

    // Default sequence: A -> B -> B -> A -> X -> Y
    comboSequence_ = {
        XrInputState::ComboPlay,
        XrInputState::ComboBack,
        XrInputState::ComboBack,
        XrInputState::ComboPlay,
        XrInputState::ComboRecenter,
        XrInputState::ComboOptions
    };

    loadConfig();
    registerLogRoots();
}

void VaultManager::registerLogRoots() const {
    // Every log line hides paths under these folders (plain names in .incoming, plain-named vault
    // folders). Only added, never removed: an old location stays hidden too. Locked or not.
    std::string internal, card;
    {
        std::lock_guard<std::mutex> l(dirMu_);
        internal = internalDir_;
        card = cardDir_;
    }
    if (!internal.empty()) log::add_private_root(internal);
    if (!card.empty()) log::add_private_root(card);
    const std::string now = vaultDir();
    if (!now.empty() && now != internal && now != card) log::add_private_root(now);
}

VaultManager::Location VaultManager::location() const {
    std::lock_guard<std::mutex> l(dirMu_);
    return location_;
}

std::string VaultManager::cardVaultDirFor(const std::string& cardMount) {
    return cardMount.empty() ? std::string() : cardMount + "/.pixelvr_vault";
}

std::string VaultManager::internalVaultDir() const {
    std::lock_guard<std::mutex> l(dirMu_);
    return internalDir_;
}

std::string VaultManager::vaultDir() const {
    std::lock_guard<std::mutex> l(dirMu_);
    if (location_ == Location::Internal) return internalDir_;
    const std::string now = cardVaultDirFor(sdCardMount());
    return now.empty() ? cardDir_ : now;
}

std::string VaultManager::thumbsDir() const {
    return thumbsDir_;  // fixed after construction
}

std::string VaultManager::cardId() const {
    std::lock_guard<std::mutex> l(dirMu_);
    return cardId_;
}

bool VaultManager::available() const {
    std::string id;
    {
        std::lock_guard<std::mutex> l(dirMu_);
        if (location_ == Location::Internal) return true;
        id = cardId_;
    }
    const std::string dir = cardVaultDirFor(sdCardMount());
    if (dir.empty() || id.empty()) return false;
    std::ifstream m(dir + "/.pixelvr_vault_id");
    std::string got;
    m >> got;
    return got == id;  // a different card (or a reformatted one) is not this vault
}

bool VaultManager::ensureDirectories() {
    if (!available()) return false;
    const std::string dir = vaultDir();
    if (dir.empty()) return false;
    std::error_code ec;
    if (location() == Location::Internal) fs::create_directories(dir, ec);  // never create a card vault here
    if (!fs::is_directory(dir, ec)) return false;
    fs::permissions(dir, fs::perms::owner_all, fs::perm_options::replace, ec);
    fs::create_directories(thumbsDir_, ec);
    fs::permissions(thumbsDir_, fs::perms::owner_all, fs::perm_options::replace, ec);
    return true;
}

std::string VaultManager::keyFilePath() const {
    const char* home = std::getenv("HOME");
    return std::string(home ? home : ".") + "/.config/pixelvr/vault.key";
}

bool VaultManager::isVaultPath(const std::string& path) const {
    const std::string dir = vaultDir();
    if (path.empty() || dir.empty()) return false;
    return path == dir || (path.size() > dir.size() && path.compare(0, dir.size(), dir) == 0 && path[dir.size()] == '/');
}

bool VaultManager::isPrivatePath(const std::string& path) {
    return vaultcrypto::hasExtension(path) || instance().isVaultPath(path) || SmbCredentialsManager::isVaultShareUri(path);
}

std::string VaultManager::sharesFilePath() const {
    // Next to vault.key (always internal, like the key): readable only with the vault's key.
    return fs::path(keyFilePath()).parent_path().string() + "/vault.shares";
}

std::string VaultManager::incomingDir() const {
    const std::string dir = vaultDir();
    return dir.empty() ? std::string() : dir + "/.incoming";
}

std::string VaultManager::forLog(const std::string& path, const std::string& text) {
    return isPrivatePath(path) ? std::string("(private)") : (text.empty() ? path : text);
}

void VaultManager::writeInternalMarker() const {
    // Lives in the internal folder (which then holds nothing else): see loadConfig.
    std::string id, dir, internal;
    Location loc;
    {
        std::lock_guard<std::mutex> l(dirMu_);
        id = cardId_;
        dir = cardDir_;
        internal = internalDir_;
        loc = location_;
    }
    std::error_code ec;
    if (loc == Location::Internal) {
        fs::remove(internal + "/.vault-on-sdcard", ec);
        return;
    }
    fs::create_directories(internal, ec);
    std::ofstream m(internal + "/.vault-on-sdcard", std::ios::trunc);
    m << id << " " << dir << "\n";
}

void VaultManager::switchLocation(Location loc, const std::string& newDir, const std::string& cardId) {
    {
        std::lock_guard<std::mutex> l(dirMu_);
        location_ = loc;
        if (loc == Location::SdCard) {
            cardDir_ = newDir;
            cardId_ = cardId;
        }
    }
    registerLogRoots();
    saveConfig();
    writeInternalMarker();
    PIXELVR_LOG_INFO("Vault location: %s", loc == Location::SdCard ? "SD card" : "internal");
}

void VaultManager::lock() {
    if (!unlocked_) return;
    unlocked_ = false;
    comboIndex_ = 0;
    headsetLostTime_ = {};
    isRecordingCombo_ = false;
    recordingBuffer_.clear();
    {
        // Downloads waiting to be encrypted are not (no key): their plain copies go with .incoming.
        std::lock_guard<std::mutex> g(migMu_);
        settle_.clear();
    }
    SmbCredentialsManager::instance().closeVaultShares();  // vault shares and their logins: gone
    vaultcrypto::clearKey();  // encrypted files can't be read until the next unlock
    PIXELVR_LOG_INFO("Vault locked");
}

void VaultManager::unlock() {
    ensureDirectories();  // not for a vault on a card that is not inserted
    unlocked_ = true;
    comboIndex_ = 0;
    headsetLostTime_ = {};
    lastActivity_ = std::chrono::steady_clock::now();
    PIXELVR_LOG_INFO("Vault unlocked (%s)", location() == Location::SdCard ? "SD card" : "internal");
    startEncryption();
}

VaultManager::ComboResult VaultManager::feedInput(int button, bool gripFullPressHeld,
                                                  std::chrono::steady_clock::time_point now) {
    if (!gripFullPressHeld) {
        comboIndex_ = 0;
        return ComboResult::None;
    }

    // Only face buttons participate in the cheat code
    if (button != XrInputState::ComboPlay && button != XrInputState::ComboBack &&
        button != XrInputState::ComboRecenter && button != XrInputState::ComboOptions) {
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

VaultManager::~VaultManager() {
    migStop_ = true;
    vaultcrypto::clearKey();
    if (migThread_.joinable()) migThread_.join();
}

std::string VaultManager::recoveryFile() const {
    // Next to the player (PixelVRPlayer/logs), under an innocuous name.
    char exe[4096];
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    std::string dir = n > 0 ? std::string(exe, static_cast<size_t>(n)) : std::string();
    dir = dir.empty() ? std::string(std::getenv("HOME") ? std::getenv("HOME") : ".") + "/PixelVRPlayer"
                      : dir.substr(0, dir.find_last_of('/'));
    return dir + "/logs/session.dat";
}

void VaultManager::writeRecovery() const {
    std::error_code ec;
    fs::create_directories(fs::path(recoveryFile()).parent_path(), ec);
    std::ofstream o(recoveryFile(), std::ios::trunc);
    o << "session " << comboString() << "\n";
}

void VaultManager::startEncryption() {
    const std::string keyFile = keyFilePath();
    std::string err;
    std::error_code ec;
    if (!fs::exists(keyFile, ec) && available()) {
        // vault.key is gone but encrypted files are there: a NEW key would "decrypt" them into garbage.
        for (auto it = fs::recursive_directory_iterator(vaultDir(), fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (vaultcrypto::hasExtension(it->path().string())) {
                PIXELVR_LOG_ERROR("Vault key: %s is missing but the vault holds encrypted files; not creating a new key",
                                  keyFile.c_str());
                return;
            }
        }
        ec.clear();
    }
    if (!fs::exists(keyFile, ec) && fs::exists(sharesFilePath(), ec)) {
        // The network share list of the lost key can't be read with a new one, and it would stay
        // unreadable for good (the list is then never written again: no vault share could be
        // saved). It is set aside under another name (still encrypted with the old key, so it can
        // be put back together with a vault.key restored from a backup); the new list starts empty.
        const std::string aside = sharesFilePath() + ".unreadable-" + std::to_string(static_cast<long long>(std::time(nullptr)));
        fs::rename(sharesFilePath(), aside, ec);
        PIXELVR_LOG_WARN("Vault key: vault.key is missing; the vault's network share list was set aside (%s)",
                         ec ? ec.message().c_str() : "it needs the old key");
        if (ec) return;  // (not creating a new key over it)
    }
    ec.clear();
    const bool ok = fs::exists(keyFile, ec) ? vaultcrypto::unlockKeyFile(keyFile, comboString(), err)
                                            : vaultcrypto::createKeyFile(keyFile, comboString(), err);
    if (!ok) {
        PIXELVR_LOG_ERROR("Vault key: %s", err.c_str());
        return;
    }
    writeRecovery();
    // The vault's network shares (encrypted with the key that is open now).
    SmbCredentialsManager::instance().openVaultShares(sharesFilePath());
    // Downloads from vault shares that did not finish (the vault locked meanwhile, or the player
    // stopped): their partial files go. (Every such download was stopped when the vault locked.)
    if (available()) fs::remove_all(incomingDir(), ec);
    encryptLeftovers();
}

void VaultManager::encryptLeftovers() {
    // Plain files left in the vault (from before encryption, copied in by hand, or downloaded from a
    // vault share): encrypt them in the background; each original is deleted only after its encrypted
    // copy reads back at full size. The folder walk runs there too (slow on an SD card).
    if (!unlocked_.load() || !vaultcrypto::hasKey() || !available()) return;
    std::lock_guard<std::mutex> g(migMu_);
    if (migrating_.load()) {
        migAgain_ = true;  // a pass is running: it walks the vault once more when done
        return;
    }
    // (A pass that ended has left its last migMu_ section: it finishes without the lock.)
    if (migThread_.joinable()) migThread_.join();
    migTotal_ = 0;
    migDone_ = 0;
    migAgain_ = false;
    migrating_ = true;
    const std::string root = vaultDir();
    migThread_ = std::thread([this, root] {
        const std::function<bool()> stop = [this] { return migStop_.load() || !vaultcrypto::hasKey(); };
        int passFiles = 0;
        for (;;) {
            // Finished downloads from vault shares first (the user waits for them).
            std::vector<std::string> settle;
            {
                std::lock_guard<std::mutex> sg(migMu_);
                settle.swap(settle_);
            }
            migDone_ = 0;
            migTotal_ = static_cast<int>(settle.size());
            for (const std::string& src : settle) {
                if (stop()) break;  // locked meanwhile: the plain copies are removed with .incoming
                passFiles += settleOne(src, root, stop);
                ++migDone_;
            }
            std::vector<fs::path> plain;
            std::error_code ec;
            for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator();
                 it.increment(ec)) {
                const std::string n = it->path().filename().string();
                if (!n.empty() && n[0] == '.') {  // markers, an old .thumbs folder, downloads in progress, hidden files
                    if (it->is_directory(ec)) it.disable_recursion_pending();
                    continue;
                }
                if (!it->is_regular_file(ec)) continue;
                if (vaultcrypto::hasExtension(n) || (n.size() > 5 && n.compare(n.size() - 5, 5, ".part") == 0)) continue;
                if (vaultcrypto::isEncrypted(it->path().string())) continue;
                plain.push_back(it->path());
            }
            migTotal_ += static_cast<int>(plain.size());
            if (!plain.empty()) PIXELVR_LOG_INFO("Vault: encrypting %zu files", plain.size());
            for (const fs::path& p : plain) {
                if (stop()) break;  // locked meanwhile: continue next time
                std::string out, err2;
                std::error_code e;
                const uint64_t size = fs::file_size(p, e);
                if (vaultcrypto::encryptFile(p.string(), p.parent_path().string(), out, err2, {}, {}, stop)) {
                    vaultcrypto::Reader r;
                    if (r.open(out) && r.size() == size) {
                        fs::remove(p, e);
                        cleanPublicThumbs(p.string());
                    } else {
                        fs::remove(out, e);
                    }
                } else if (!stop()) {
                    // (Not the error text: it may name the plain file.)
                    PIXELVR_LOG_WARN("Vault: could not encrypt a file");
                }
                ++migDone_;
            }
            passFiles += static_cast<int>(plain.size());
            // Asked again meanwhile (or a download finished): once more. Checked and ended under the
            // same lock encryptLeftovers takes, so no request falls in between.
            std::lock_guard<std::mutex> eg(migMu_);
            if ((migAgain_ || !settle_.empty()) && !stop()) {
                migAgain_ = false;
                continue;
            }
            migAgain_ = false;
            migrating_ = false;
            break;
        }
        if (passFiles > 0) PIXELVR_LOG_INFO("Vault: encryption pass finished (%d files)", passFiles);
    });
}

void VaultManager::settleDownload(const std::string& path) {
    // Only an item of a download folder: nothing else is ever encrypted away and deleted here.
    const fs::path p(path);
    if (path.empty() || p.parent_path().filename() != ".incoming") return;
    {
        std::lock_guard<std::mutex> g(migMu_);
        settle_.push_back(path);
    }
    encryptLeftovers();  // (locked or the card is out: it waits; locking removes it with .incoming)
}

int VaultManager::settleOne(const std::string& src, const std::string& root, const std::function<bool()>& stop) {
    // One plain file encrypted into `dir` under its real name; the plain one is deleted once the
    // encrypted copy reads back at full size.
    auto one = [&](const fs::path& p, const std::string& dir) -> bool {
        std::error_code e;
        const uint64_t size = fs::file_size(p, e);
        if (e) return false;
        std::string out, err;
        if (!vaultcrypto::encryptFile(p.string(), dir, out, err, {}, p.filename().string(), stop)) {
            // (Not the error text: it may name the file.)
            if (!stop()) PIXELVR_LOG_WARN("Vault: a download could not be encrypted");
            return false;
        }
        vaultcrypto::Reader r;
        if (r.open(out) && r.size() == size) {
            fs::remove(p, e);
            return true;
        }
        if (!vaultcrypto::hasKey()) {
            // Locked right after it was written (complete and synced, but no key to read it back):
            // the encrypted copy stays, the plain one goes (it would be removed with .incoming).
            fs::remove(p, e);
            return true;
        }
        fs::remove(out, e);
        PIXELVR_LOG_WARN("Vault: a download's encrypted copy did not read back; the download stays unencrypted");
        return false;
    };
    std::error_code ec;
    const fs::path from(src);
    const fs::file_status st = fs::symlink_status(from, ec);
    if (ec || !fs::exists(st)) return 0;  // gone (removed with .incoming)
    if (fs::is_regular_file(st)) return one(from, root) ? 1 : 0;
    if (!fs::is_directory(st)) return 0;
    // A folder: the same folders in the vault (under a free name), every file encrypted into its own.
    const std::string top = root + "/" + filecopy::freeName(root, from.filename().string(), true);
    std::vector<std::pair<fs::path, std::string>> files;  // plain file, the vault folder it goes into
    fs::create_directories(top, ec);
    fs::permissions(top, fs::perms::owner_all, fs::perm_options::replace, ec);
    bool all = !ec;
    for (auto it = fs::recursive_directory_iterator(from, ec); all && !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        std::error_code e;
        const fs::file_status s = it->symlink_status(e);
        const fs::path rel = it->path().lexically_relative(from);
        if (fs::is_directory(s)) {
            const std::string d = top + "/" + rel.string();
            fs::create_directories(d, e);
            fs::permissions(d, fs::perms::owner_all, fs::perm_options::replace, e);
            if (e) all = false;
        } else if (fs::is_regular_file(s)) {
            files.emplace_back(it->path(), rel.parent_path().empty() ? top : top + "/" + rel.parent_path().string());
        }
    }
    if (ec) all = false;
    int done = 0;
    for (const auto& f : files) {
        if (stop()) {
            all = false;
            break;
        }
        if (one(f.first, f.second)) {
            ++done;
        } else {
            all = false;
        }
    }
    filecopy::syncDir(root);
    if (all) {
        fs::remove_all(from, ec);
    } else if (!stop()) {
        PIXELVR_LOG_WARN("Vault: %d of %zu files of a downloaded folder encrypted (the rest is removed when the vault locks)",
                         done, files.size());
    }
    return done;
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

void VaultManager::cleanVaultThumbs(const std::string& path) {
    if (thumbsDir_.empty()) return;
    auto removeThumbFor = [&](const fs::path& f) {
        // As ThumbnailCache::handlePrivate names them: encrypted files by their random name, plain
        // leftovers in the vault by path.
        const std::string key = vaultcrypto::hasExtension(f.string()) ? f.filename().string() : f.string();
        const std::string base = thumbsDir_ + "/" + std::to_string(fnv1a64(key));
        std::error_code e;
        fs::remove(base + ".rgba", e);
        e.clear();
        fs::remove(base + ".meta", e);
    };
    std::error_code ec;
    if (fs::is_directory(path, ec)) {
        for (const auto& it : fs::recursive_directory_iterator(path, ec)) {
            std::error_code e;
            if (it.is_regular_file(e)) removeThumbFor(it.path());
        }
    } else {
        removeThumbFor(fs::path(path));
    }
}

void VaultManager::setAutoLockHeadsetOff(bool enable) {
    autoLockHeadsetOff_ = enable;
    saveConfig();
}

void VaultManager::setAutoLockTimeoutSec(int sec) {
    autoLockTimeoutSec_ = sec;
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
    if (button != XrInputState::ComboPlay && button != XrInputState::ComboBack &&
        button != XrInputState::ComboRecenter && button != XrInputState::ComboOptions) {
        return false;
    }
    recordingBuffer_.push_back(button);
    if (recordingBuffer_.size() >= 6) {
        // The vault key is protected by the combination: re-protect it with the new one first.
        const std::string oldCombo = comboString();
        const std::vector<int> oldSeq = comboSequence_;
        {
            std::lock_guard<std::mutex> cfg(cfgMu_);
            comboSequence_ = recordingBuffer_;
        }
        const std::string keyFile = keyFilePath();
        std::error_code kec;
        std::string err;
        if (fs::exists(keyFile, kec) && !vaultcrypto::rewrapKeyFile(keyFile, oldCombo, comboString(), err)) {
            PIXELVR_LOG_ERROR("Vault key: %s - keeping the old combination", err.c_str());
            std::lock_guard<std::mutex> cfg(cfgMu_);
            comboSequence_ = oldSeq;
        }
        isRecordingCombo_ = false;
        recordingBuffer_.clear();
        saveConfig();
        writeRecovery();
        return true; // Finished recording 6-step combo
    }
    return false;
}

void VaultManager::loadConfig() {
    const std::string path = getVaultConfigPath();
    bool locationSeen = false;
    std::ifstream file(path);
    if (!file.is_open()) {
        std::ifstream m(internalDir_ + "/.vault-on-sdcard");
        std::string id, dir;
        if (m >> id && std::getline(m >> std::ws, dir) && !id.empty()) {
            location_ = Location::SdCard;
            cardId_ = id;
            cardDir_ = dir;
        }
        return;
    }

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
            // Old in-app "File Actions Button" (Y = 3 was the default). Buttons
            // are assigned in SteamVR now: only note a custom one for the notice.
            legacyMoveButtonSeen_ = true;
            try { legacyMoveButtonCustom_ = std::stoi(val) != XrInputState::ComboOptions; } catch (...) {}
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
        } else if (key == "vault_location") {
            location_ = val == "sdcard" ? Location::SdCard : Location::Internal;
            locationSeen = true;
        } else if (key == "vault_card_dir") {
            cardDir_ = val;
        } else if (key == "vault_card_id") {
            cardId_ = val;
        }
    }
    file.close();
    // An older player rewrites vault.conf without the location keys: the marker left in the internal
    // folder still says the vault moved to the SD card (otherwise an empty internal vault would appear).
    if (!locationSeen) {
        std::ifstream m(internalDir_ + "/.vault-on-sdcard");
        std::string id, dir;
        if (m >> id && std::getline(m >> std::ws, dir) && !id.empty()) {
            location_ = Location::SdCard;
            cardId_ = id;
            cardDir_ = dir;
        }
    }
}

void VaultManager::saveConfig() const {
    const std::string path = getVaultConfigPath();
    std::lock_guard<std::mutex> cfg(cfgMu_);
    try {
        fs::path p(path);
        if (p.has_parent_path()) {
            fs::create_directories(p.parent_path());
        }
        const std::string tmp = path + ".tmp";
        {
        std::ofstream file(tmp, std::ios::trunc);
        if (file.is_open()) {
            file << "auto_lock_headset_off=" << (autoLockHeadsetOff_ ? 1 : 0) << "\n";
            file << "auto_lock_timeout_sec=" << autoLockTimeoutSec_ << "\n";
            file << "combo=";
            for (std::size_t i = 0; i < comboSequence_.size(); ++i) {
                if (i > 0) file << ",";
                file << comboSequence_[i];
            }
            file << "\n";
            std::lock_guard<std::mutex> l(dirMu_);
            file << "vault_location=" << (location_ == Location::SdCard ? "sdcard" : "internal") << "\n";
            if (!cardDir_.empty()) file << "vault_card_dir=" << cardDir_ << "\n";
            if (!cardId_.empty()) file << "vault_card_id=" << cardId_ << "\n";
        }
        }
        std::error_code rec;
        fs::rename(tmp, path, rec);
    } catch (const std::exception& e) {
        PIXELVR_LOG_WARN("Could not save vault settings: %s", e.what());
    }
}


// ---------------------------------------------------------------------------
// Command line for install.sh (no XR, no threads besides this one).
// ---------------------------------------------------------------------------
namespace {

struct VaultFile {
    fs::path path;
    fs::path rel;  // folder relative to the vault root
    uint64_t size = 0;
    bool encrypted = false;
};

std::vector<VaultFile> listVaultFiles(const std::string& root, bool* incomplete = nullptr) {
    std::vector<VaultFile> out;
    std::error_code ec;
    auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
    if (ec && incomplete) *incomplete = true;
    for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        const std::string n = it->path().filename().string();
        if (!n.empty() && n[0] == '.') {  // markers, an old .thumbs folder
            if (it->is_directory(ec)) it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(ec) || (n.size() > 5 && n.compare(n.size() - 5, 5, ".part") == 0)) continue;
        VaultFile f;
        f.path = it->path();
        f.rel = fs::relative(it->path().parent_path(), root, ec);
        if (f.rel == ".") f.rel.clear();
        f.size = fs::file_size(it->path(), ec);
        f.encrypted = vaultcrypto::hasExtension(n);
        out.push_back(std::move(f));
    }
    if (ec && incomplete) *incomplete = true;
    return out;
}

// Encrypted files or other real files still in the vault (not markers).
int leftInVault(const std::string& root) {
    int n = 0;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (it->is_regular_file(ec) && name != ".pixelvr_vault_id" && name != ".vault-on-sdcard" &&
            it->path().parent_path().filename() != ".thumbs") {
            ++n;
        }
    }
    return n;
}

}  // namespace

int VaultManager::runCli(const std::string& mode, const std::string& dest) {
    VaultManager& vm = instance();
    const bool card = vm.location() == Location::SdCard;
    const std::string dir = vm.vaultDir();
    std::error_code ec;
    if (mode == "info") {
        const bool avail = vm.available() && fs::is_directory(dir, ec);
        uint64_t bytes = 0;
        int files = 0, enc = 0;
        if (avail) {
            for (const VaultFile& f : listVaultFiles(dir)) {
                ++files;
                bytes += f.size;
                enc += f.encrypted ? 1 : 0;
            }
        }
        std::printf("location=%s\ndir=%s\navailable=%d\nfiles=%d\nencrypted=%d\nbytes=%llu\nkey=%s\nrecovery=%s\n",
                    card ? "sdcard" : "internal", dir.c_str(), avail ? 1 : 0, files, enc,
                    static_cast<unsigned long long>(bytes), fs::exists(vm.keyFilePath(), ec) ? vm.keyFilePath().c_str() : "",
                    vm.recoveryFile().c_str());
        return 0;
    }
    if (mode != "export" || dest.empty()) {
        std::fprintf(stderr, "usage: pixelvr --vault-info | --vault-export <folder>\n");
        return 2;
    }
    if (!vm.available() || !fs::is_directory(dir, ec)) {
        std::fprintf(stderr, card ? "The vault is on an SD card that is not inserted.\n" : "There is no vault.\n");
        return 3;
    }
    bool incomplete = false;
    fs::remove_all(vm.incomingDir(), ec);  // unfinished downloads from vault shares (partial files)
    ec.clear();
    const std::vector<VaultFile> files = listVaultFiles(dir, &incomplete);
    const bool anyEncrypted = std::any_of(files.begin(), files.end(), [](const VaultFile& f) { return f.encrypted; });
    const bool shares = fs::exists(vm.sharesFilePath(), ec);
    if (anyEncrypted || shares) {
        std::string err;
        if (!vaultcrypto::unlockKeyFile(vm.keyFilePath(), vm.comboString(), err)) {
            std::fprintf(stderr, "Cannot open the vault key: %s\n", err.c_str());
            return 4;
        }
    }
    fs::create_directories(dest, ec);
    if (!fs::is_directory(dest, ec)) {
        std::fprintf(stderr, "Cannot create %s\n", dest.c_str());
        return 5;
    }
    // Same drive: each encrypted file is deleted once its copy is complete, so the largest file must fit.
    uint64_t total = 0, largest = 0;
    for (const VaultFile& f : files) {
        total += f.size;
        largest = std::max(largest, f.size);
    }
    const bool same = filecopy::sameDevice(dir, dest);
    const uint64_t need = same ? largest : total;
    if (freeBytes(dest) < need + (64ull << 20)) {
        std::fprintf(stderr, "Not enough free space in %s: %s needed.\n", dest.c_str(), gigabytes(need).c_str());
        return 6;
    }
    int done = 0, failed = 0;
    for (const VaultFile& f : files) {
        const fs::path outDir = fs::path(dest) / f.rel;
        fs::create_directories(outDir, ec);
        std::string name = f.encrypted ? vaultcrypto::storedName(f.path.string()) : f.path.filename().string();
        if (f.encrypted && name.empty()) {  // another key (or damaged): never delete it
            ++failed;
            std::fprintf(stderr, "  could not open %s with this key (kept in the vault)\n", f.path.filename().string().c_str());
            continue;
        }
        for (char& c : name) {
            if (c == '/') c = '_';
        }
        if (name.empty() || vaultcrypto::hasExtension(name)) name += ".bin";
        const std::string target = (outDir / filecopy::freeName(outDir.string(), name, false)).string();
        std::string err;
        bool ok = false;
        if (f.encrypted) {
            ok = vaultcrypto::decryptFile(f.path.string(), target, err) && fs::file_size(target, ec) == [&] {
                vaultcrypto::Reader r;
                return r.open(f.path.string()) ? r.size() : ~0ull;
            }();
        } else if (same) {
            fs::rename(f.path, target, ec);
            ok = !ec;
            if (!ok) err = ec.message();
        } else {
            ok = filecopy::copyFileSynced(f.path.string(), target, err);
        }
        if (ok) {
            if (fs::exists(f.path, ec)) fs::remove(f.path, ec);
            ++done;
            std::printf("  %d / %zu  %s\n", done + failed, files.size(), (f.rel / fs::path(target).filename()).string().c_str());
        } else {
            ++failed;
            std::fprintf(stderr, "  could not export %s: %s (kept in the vault)\n", name.c_str(), err.c_str());
        }
        std::fflush(stdout);
    }
    // The vault's network shares go along: they become normal shares (the player's share list).
    int sharesMoved = 0;
    if (shares) {
        sharesMoved = SmbCredentialsManager::instance().exportVaultShares(vm.sharesFilePath());
        if (sharesMoved < 0) {
            ++failed;
            std::fprintf(stderr, "  could not open the vault's network share list with this key (kept)\n");
        }
    }
    vaultcrypto::clearKey();
    // Remove the now empty folders (not the vault folder itself).
    std::vector<fs::path> dirs;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_directory(ec)) dirs.push_back(it->path());
    }
    std::sort(dirs.begin(), dirs.end(), [](const fs::path& a, const fs::path& b) { return a.string().size() > b.string().size(); });
    for (const fs::path& d : dirs) {
        if (fs::is_empty(d, ec)) fs::remove(d, ec);
    }
    const int left = leftInVault(dir);
    std::printf("exported=%d\nfailed=%d\nleft=%d\nshares=%d\n", done, failed, left, std::max(0, sharesMoved));
    if (incomplete) std::fprintf(stderr, "Some vault folders could not be read: they stay in the vault.\n");
    return (failed == 0 && left == 0 && !incomplete) ? 0 : 1;
}

} // namespace pixelvr

#include "app/file_ops.hpp"

#include "app/vault_crypto.hpp"
#include "app/vault_manager.hpp"
#include "plugins/plugin_files.hpp"
#include "util/file_copy.hpp"
#include "util/logging.hpp"
#include "util/media_files.hpp"
#include "util/sdcard.hpp"

#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <set>

namespace pixelvr {
namespace fs = std::filesystem;

namespace {

bool isUnder(const std::string& path, const std::string& dir) {
    if (dir.empty()) return false;
    return path == dir || (path.size() > dir.size() && path.compare(0, dir.size(), dir) == 0 && path[dir.size()] == '/');
}

std::string trimSlash(std::string p) {
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    return p;
}

bool endsWith(const std::string& s, const char* suffix) {
    const size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// The vault root's own entries that never travel (markers, an old .thumbs folder, unfinished writes).
bool skippedRootName(const std::string& n) {
    return n.empty() || n[0] == '.' || endsWith(n, ".part");
}

// A real (stored) name made safe as a file name: no '/', and never ending in .pvrv for a plain file.
std::string safeName(std::string n, const std::string& fallback) {
    for (char& c : n) {
        if (c == '/' || c == '\0') c = '_';
    }
    if (n.empty() || n == "." || n == "..") n = fallback;
    if (vaultcrypto::hasExtension(n)) n += ".bin";
    return n;
}

uint64_t treeBytes(const fs::path& p) {
    std::error_code ec;
    const fs::file_status st = fs::symlink_status(p, ec);
    if (fs::is_symlink(st)) return 0;
    if (!fs::is_directory(st)) {
        if (vaultcrypto::hasExtension(p.string())) {
            vaultcrypto::Reader r;
            if (r.open(p.string())) return r.size();
        }
        return fs::file_size(p, ec);
    }
    uint64_t sum = 0;
    for (auto it = fs::recursive_directory_iterator(p, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_symlink(ec)) continue;
        if (it->is_regular_file(ec)) sum += fs::file_size(it->path(), ec);
    }
    return sum;
}

// One request on the worker.
struct Job {
    FileOps::Mode mode;
    std::function<void(uint64_t)> addDone;    // bytes finished (for the progress bar)
    std::function<bool()> stopRequested;
    std::string err;
    bool keepDiskNames = false;                // relocation: thumbnails are keyed by the random name
    bool destIsVault = false;                  // relocation: the new folder is not the vault YET
    std::map<std::string, std::set<std::string>> storedNames;  // vault folder -> real names in it
    std::vector<FileOps::Moved> moved;         // Move: every file / folder whose source is gone now
    int filesOk = 0, filesFailed = 0, leftovers = 0;

    bool move() const { return mode == FileOps::Mode::Move; }

    std::set<std::string>& namesIn(const std::string& dir) {
        auto it = storedNames.find(dir);
        if (it != storedNames.end()) return it->second;
        std::set<std::string>& s = storedNames[dir];
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            const std::string p = e.path().string();
            if (vaultcrypto::hasExtension(p)) {
                const std::string n = vaultcrypto::storedName(p);
                if (!n.empty()) s.insert(n);
            }
        }
        return s;
    }

    // A real name that no file in the vault folder `dir` has yet.
    std::string freeStoredName(const std::string& dir, const std::string& name) {
        std::set<std::string>& taken = namesIn(dir);
        const std::string cand = filecopy::freeName(dir, name, false, [&](const std::string& c) { return taken.count(c) > 0; });
        taken.insert(cand);
        return cand;
    }

    vaultcrypto::Progress progressFn() {
        auto last = std::make_shared<uint64_t>(0);
        return [this, last](uint64_t done, uint64_t) {
            if (done > *last) addDone(done - *last);
            *last = done;
        };
    }

    bool vaultDest(const std::string& dir) const { return destIsVault || VaultManager::instance().isVaultPath(dir); }

    bool fatal() const { return err == "stopped" || err == "the vault was locked" || err == "the vault is locked"; }

    // Returns the new path ("" + err on failure).
    std::string file(const std::string& src, const std::string& destDir) {
        const bool needsKey = vaultcrypto::hasExtension(src) || vaultDest(destDir);
        std::string out = fileImpl(src, destDir);
        if (out.empty() && needsKey && !vaultcrypto::hasKey()) err = "the vault was locked";
        if (out.empty()) {
            ++filesFailed;
        } else {
            ++filesOk;
            if (move()) moved.push_back({src, out});
        }
        return out;
    }

    // Deletes the source of a Move; if that fails, the new copy goes again (nothing was moved).
    bool finishMove(const std::string& src, const std::string& out, bool dstVault) {
        std::error_code ec;
        if (!fs::remove(src, ec) || ec) {
            std::error_code e2;
            fs::remove(out, e2);
            err = "the original can't be deleted (" + (ec ? ec.message() : std::string("read-only?")) + ")";
            return false;
        }
        if (dstVault) VaultManager::instance().cleanPublicThumbs(src);
        else VaultManager::instance().cleanVaultThumbs(src);  // left the vault: its encrypted thumbnail goes
        return true;
    }

    std::string fileImpl(const std::string& src, const std::string& destDir) {
        std::error_code ec;
        const bool srcEnc = vaultcrypto::hasExtension(src) && vaultcrypto::isEncrypted(src);
        const bool dstVault = vaultDest(destDir);
        bool same = filecopy::sameDevice(src, destDir);
        auto cancelled = [this, srcEnc, dstVault] {
            return stopRequested() || ((srcEnc || dstVault) && !vaultcrypto::hasKey());
        };
        const std::string diskLeaf = fs::path(src).filename().string();
        std::string name = srcEnc ? vaultcrypto::storedName(src) : diskLeaf;
        if (name.empty()) {
            err = "the vault is locked";
            return {};
        }
        std::string out;
        if (!srcEnc && !dstVault) {
            out = (fs::path(destDir) / filecopy::freeName(destDir, name, false)).string();
            if (move() && same) {
                fs::rename(src, out, ec);
                if (!ec) {
                    addDone(fs::file_size(out, ec));
                    filecopy::syncDir(destDir);
                    return out;
                }
                if (ec != std::errc::cross_device_link) {
                    err = ec.message();
                    return {};
                }
                ec.clear();  // one drive mounted twice: copy instead
            }
            if (!filecopy::copyFileSynced(src, out, err, progressFn(), cancelled)) return {};
        } else if (!srcEnc && dstVault) {
            if (!vaultcrypto::hasKey()) {
                err = "the vault is locked";
                return {};
            }
            const uint64_t size = fs::file_size(src, ec);
            if (!vaultcrypto::encryptFile(src, destDir, out, err, progressFn(), freeStoredName(destDir, name), cancelled)) return {};
            vaultcrypto::Reader r;
            if (!r.open(out) || r.size() != size) {
                fs::remove(out, ec);
                err = "the encrypted copy did not verify";
                return {};
            }
        } else if (srcEnc && !dstVault) {
            const std::string plainName = safeName(name, diskLeaf + ".bin");
            out = (fs::path(destDir) / filecopy::freeName(destDir, plainName, false)).string();
            if (!vaultcrypto::decryptFile(src, out, err, progressFn(), cancelled)) return {};
            vaultcrypto::Reader r;
            if (!r.open(src) || fs::file_size(out, ec) != r.size()) {
                fs::remove(out, ec);
                err = "the decrypted copy did not verify";
                return {};
            }
        } else {  // vault -> vault
            const std::string real = freeStoredName(destDir, name);
            if (move() && same) {
                out = (fs::path(destDir) / filecopy::freeName(destDir, diskLeaf, false)).string();
                fs::rename(src, out, ec);
                if (!ec) {
                    if (real != name) vaultcrypto::setStoredName(out, real);
                    filecopy::syncDir(destDir);
                    vaultcrypto::Reader r;
                    if (r.open(out)) addDone(r.size());
                    return out;
                }
                if (ec != std::errc::cross_device_link) {
                    err = ec.message();
                    return {};
                }
                ec.clear();
                same = false;
            }
            const std::string disk = keepDiskNames ? diskLeaf : std::string();
            vaultcrypto::Reader r;
            const uint64_t size = r.open(src) ? r.size() : 0;
            r.close();
            if (!vaultcrypto::reencryptFile(src, destDir, out, err, progressFn(), real, cancelled, disk)) return {};
            vaultcrypto::Reader v;
            if (!v.open(out) || v.size() != size) {
                fs::remove(out, ec);
                err = "the copy did not verify";
                return {};
            }
        }
        if (move() && !finishMove(src, out, dstVault)) return {};
        return out;
    }

    // Returns the new folder when everything in it went; "" when something failed (the counters and
    // `moved` say what did go). A Move deletes only what was transferred: what stays behind (links,
    // files being written, anything that failed or appeared meanwhile) keeps its folder.
    std::string folder(const std::string& src, const std::string& destDir) {
        VaultManager& vm = VaultManager::instance();
        std::error_code ec;
        const bool srcVault = vm.isVaultPath(src);
        const bool dstVault = vaultDest(destDir);
        const std::string name = fs::path(src).filename().string();
        const std::string out = (fs::path(destDir) / filecopy::freeName(destDir, name, true)).string();
        bool whole = move() && srcVault == dstVault && !destIsVault && filecopy::sameDevice(src, destDir);
        if (whole && dstVault) {
            // Plain files would stay plain under a new path: only when none are left (the encryption
            // pass converts them) is the folder moved as it is.
            for (auto it = fs::recursive_directory_iterator(src, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
                const std::string n = it->path().filename().string();
                if (it->is_regular_file(ec) && !vaultcrypto::hasExtension(n)) {
                    whole = false;
                    break;
                }
            }
            ec.clear();
        }
        if (whole) {
            fs::rename(src, out, ec);
            if (!ec) {
                filecopy::syncDir(destDir);
                addDone(treeBytes(out));
                moved.push_back({src, out});
                return out;
            }
            ec.clear();  // e.g. one drive mounted twice: file by file
        }
        fs::create_directory(out, ec);
        if (ec) {
            err = "cannot create the folder " + name;
            return {};
        }
        if (dstVault) fs::permissions(out, fs::perms::owner_all, fs::perm_options::replace, ec);
        std::vector<fs::path> children;
        for (const auto& e : fs::directory_iterator(src, ec)) children.push_back(e.path());
        std::sort(children.begin(), children.end());
        bool allOk = !ec;
        if (ec) err = "cannot read the folder " + name;
        for (const fs::path& c : children) {
            if (stopRequested()) {
                err = "stopped";
                return {};
            }
            std::error_code e2;
            const fs::file_status st = fs::symlink_status(c, e2);
            const std::string leaf = c.filename().string();
            if (fs::is_symlink(st) || (!fs::is_directory(st) && !fs::is_regular_file(st)) ||
                (fs::is_regular_file(st) && endsWith(leaf, ".part"))) {
                ++leftovers;  // links (never followed), devices, files still being written: stay
                continue;
            }
            const std::string r = fs::is_directory(st) ? folder(c.string(), out) : file(c.string(), out);
            if (r.empty()) {
                allOk = false;
                if (fatal()) return {};  // the vault locked or the user stopped: not the rest
            }
        }
        if (move()) {
            // Only an empty folder goes (rmdir): anything left behind keeps it.
            if (fs::remove(src, ec) && !ec) {
                moved.push_back({src, out});
                if (dstVault) vm.cleanPublicThumbs(src);
            }
        }
        return allOk ? out : std::string();
    }
};

void relocateVault(const FileOps::Request& r, FileOps::Result& res, Job& job) {
    VaultManager& vm = VaultManager::instance();
    std::error_code ec;
    // Copy everything into the new folder (keeping the random names, fresh nonces), check the count,
    // switch, then delete the old copy. A failure removes only what this attempt made; the old vault
    // stays as it was.
    const std::string oldRoot = r.sources.empty() ? std::string() : r.sources[0];
    const std::string newRoot = r.destDir;
    job.keepDiskNames = true;
    job.destIsVault = true;
    res.relocation = true;
    res.privateText = true;
    const std::string stopped = "Moving the vault stopped. The vault stays where it was.";
    if (!vaultcrypto::hasKey()) {
        res.message = "Unlock the vault to move it";
        return;
    }
    if (vm.migrating()) {
        res.message = "Wait until the vault files are encrypted, then move the vault";
        return;
    }
    // A leftover of an earlier attempt that was cut off (e.g. the card was pulled) is cleared.
    const bool leftover = fs::exists(newRoot + "/.relocating", ec);
    for (const auto& e : fs::directory_iterator(newRoot, ec)) {
        if (skippedRootName(e.path().filename().string())) continue;
        if (!leftover) {
            res.message = "There is already a vault folder at the new place";
            return;
        }
    }
    ec.clear();
    if (leftover) {
        std::vector<fs::path> old;
        for (const auto& e : fs::directory_iterator(newRoot, ec)) {
            if (!skippedRootName(e.path().filename().string())) old.push_back(e.path());
        }
        for (const fs::path& p : old) fs::remove_all(p, ec);
    }
    if (freeBytes(fs::path(newRoot).parent_path().string()) < treeBytes(oldRoot) + (64ull << 20)) {
        res.message = "Not enough free space for the vault";
        return;
    }
    const bool madeRoot = !fs::exists(newRoot, ec);
    fs::create_directories(newRoot, ec);
    fs::permissions(newRoot, fs::perms::owner_all, fs::perm_options::replace, ec);
    { std::ofstream(newRoot + "/.relocating", std::ios::trunc) << "1\n"; }
    if (r.toSdCard) {
        std::ofstream m(newRoot + "/.pixelvr_vault_id", std::ios::trunc);
        m << r.cardId << "\n";
    }
    filecopy::syncDir(newRoot);
    bool ok = true;
    std::vector<fs::path> children;
    for (const auto& e : fs::directory_iterator(oldRoot, ec)) children.push_back(e.path());
    if (ec) ok = false;
    for (const fs::path& c : children) {
        if (!ok) break;
        if (skippedRootName(c.filename().string())) continue;
        std::error_code e2;
        const fs::file_status st = fs::symlink_status(c, e2);
        if (fs::is_symlink(st)) continue;
        // Encrypted files are re-encrypted into the new place; plain leftovers are encrypted there.
        const std::string out = fs::is_directory(st) ? job.folder(c.string(), newRoot) : job.file(c.string(), newRoot);
        if (out.empty()) ok = false;
    }
    if (ok && job.filesFailed > 0) ok = false;
    if (!ok) {
        // The new folder held nothing else when this started: everything in it now is this attempt's
        // (the internal folder keeps its .vault-on-sdcard marker, hidden names are not touched).
        std::vector<fs::path> made;
        for (const auto& e : fs::directory_iterator(newRoot, ec)) {
            if (!skippedRootName(e.path().filename().string())) made.push_back(e.path());
        }
        for (const fs::path& p : made) fs::remove_all(p, ec);
        if (r.toSdCard) fs::remove(newRoot + "/.pixelvr_vault_id", ec);
        fs::remove(newRoot + "/.relocating", ec);
        if (r.toSdCard && madeRoot) fs::remove(newRoot, ec);  // only if empty now
        PIXELVR_LOG_WARN("Vault move stopped (%d files copied, %d failed)", job.filesOk, job.filesFailed);
        res.message = stopped;
        return;
    }
    fs::remove(newRoot + "/.relocating", ec);
    filecopy::syncDir(newRoot);
    vm.switchLocation(r.toSdCard ? VaultManager::Location::SdCard : VaultManager::Location::Internal, newRoot, r.cardId);
    for (const fs::path& c : children) {
        if (c.filename().string() == ".vault-on-sdcard") continue;
        fs::remove_all(c, ec);
    }
    if (!r.toSdCard) fs::remove_all(oldRoot, ec);  // the card's folder, marker and all
    res.ok = true;
    res.relocated = true;
    res.moved.push_back({oldRoot, newRoot});
    res.message = r.toSdCard ? "The vault is on the SD card now" : "The vault is on the headset now";
}

}  // namespace

FileOps& FileOps::instance() {
    static FileOps s;
    return s;
}

FileOps::~FileOps() {
    {
        std::lock_guard<std::mutex> l(mu_);
        stop_ = true;
        jobs_.clear();  // only the waiting ones: the running one was taken off already
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void FileOps::queue(Request r) {
    VaultManager& vm = VaultManager::instance();
    r.destWasVault = vm.isVaultPath(r.destDir);
    r.srcWasVault = !r.sources.empty() && vm.isVaultPath(r.sources[0]);
    std::lock_guard<std::mutex> l(mu_);
    if (r.relocateVault) ++relocPending_;
    jobs_.push_back(std::move(r));
    busy_ = true;
    if (!worker_.joinable()) worker_ = std::thread([this] { loop(); });
    cv_.notify_all();
}

FileOps::Status FileOps::status() const {
    std::lock_guard<std::mutex> l(mu_);
    Status s = status_;
    s.waiting = static_cast<int>(jobs_.size());
    return s;
}

bool FileOps::takeResult(Result& out) {
    std::lock_guard<std::mutex> l(mu_);
    if (results_.empty()) return false;
    out = std::move(results_.front());
    results_.pop_front();
    return true;
}

void FileOps::loop() {
    for (;;) {
        Request r;
        {
            std::unique_lock<std::mutex> l(mu_);
            cv_.wait(l, [this] { return stop_ || !jobs_.empty(); });
            if (stop_) return;
            r = std::move(jobs_.front());
            jobs_.pop_front();  // the destructor's clear() then only drops the waiting ones
            status_ = Status{};
            if (r.relocateVault) cancelReloc_.store(false);
            status_.busy = true;
            status_.copy = r.mode == Mode::Copy;
            status_.relocating = r.relocateVault;
            status_.label = r.label;
            status_.privateLabel = r.relocateVault || r.srcWasVault || r.destWasVault ||
                                   (!r.sources.empty() && VaultManager::isPrivatePath(r.sources[0]));
        }
        uint64_t total = 0;
        for (const std::string& s : r.sources) total += treeBytes(s);
        {
            std::lock_guard<std::mutex> l(mu_);
            status_.total = total;
        }
        Result res;
        res.mode = r.mode;
        res.destDir = r.destDir;
        res.lockWhenDone = r.lockWhenDone;
        run(r, res);
        std::lock_guard<std::mutex> l(mu_);
        if (r.relocateVault) --relocPending_;
        results_.push_back(std::move(res));
        status_ = Status{};
        if (jobs_.empty()) busy_ = false;
        if (stop_) return;
    }
}

void FileOps::cancelRelocation() {
    cancelReloc_.store(true);
}

bool FileOps::relocationPending() const {
    std::lock_guard<std::mutex> l(mu_);
    return relocPending_ > 0;
}

std::string FileOps::placeName(const std::string& dir) {
    VaultManager& vm = VaultManager::instance();
    const std::string d = trimSlash(dir);
    if (d == trimSlash(vm.vaultDir())) return "the vault";
    const std::string card = sdCardMount();
    if (!card.empty() && d == card) return "the SD card";
    const char* home = std::getenv("HOME");
    if (home && d == trimSlash(home)) return "Home";
    const std::string leaf = fs::path(d).filename().string();
    return leaf.empty() ? d : leaf;
}

const std::vector<std::string>& FileOps::defaultSidecarPatterns() {
    static const std::vector<std::string> p = {"{base}.funscript", "{base}.*.funscript", "{base}.srt",
                                               "{base}.*.srt", "{base}.ass", "{base}.*.ass",
                                               "{base}.vtt", "{base}.*.vtt"};
    return p;
}

std::vector<std::string> FileOps::sidecarsOf(const std::string& path, const std::vector<std::string>& patterns) {
    std::vector<std::string> out;
    std::error_code ec;
    if (path.empty() || patterns.empty() || fs::is_directory(path, ec)) return out;
    const bool enc = vaultcrypto::hasExtension(path);
    const std::string real = enc ? vaultcrypto::storedName(path) : fs::path(path).filename().string();
    if (real.empty()) return out;
    const std::string base = pluginfiles::baseNameOf(real);
    if (base.empty()) return out;
    struct Entry {
        std::string path, name;
    };
    std::vector<Entry> files;
    std::vector<std::string> otherBases;  // other videos / tracks in the folder
    for (const auto& e : fs::directory_iterator(fs::path(path).parent_path(), ec)) {
        const std::string p = e.path().string();
        if (p == path || !e.is_regular_file(ec) || e.is_symlink(ec)) continue;
        const std::string disk = e.path().filename().string();
        if (disk.empty() || disk[0] == '.' || endsWith(disk, ".part")) continue;
        const std::string name = vaultcrypto::hasExtension(p) ? vaultcrypto::storedName(p) : disk;
        if (name.empty() || name == real) continue;
        if (isPlayableFile(name)) otherBases.push_back(pluginfiles::baseNameOf(name));
        files.push_back({p, name});
    }
    for (const Entry& f : files) {
        bool match = false;
        for (const std::string& pat : patterns) {
            if (pluginfiles::sidecarMatch(pat, base, f.name)) {
                match = true;
                break;
            }
        }
        if (!match) continue;
        // Another video with the same base shares it ("Movie.mkv" + "Movie.mp4"): it stays. One with a
        // longer base owns it ("Movie.Directors.Cut.srt" belongs to "Movie.Directors.Cut.mkv").
        bool other = false;
        for (const std::string& ob : otherBases) {
            if (ob == base) other = true;
            if (ob.size() > base.size() && f.name.size() > ob.size() && f.name.compare(0, ob.size(), ob) == 0 &&
                f.name[ob.size()] == '.') {
                other = true;
            }
        }
        if (!other) out.push_back(f.path);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string FileOps::checkTarget(Mode mode, const std::string& srcIn, bool srcIsDir, const std::string& destIn) {
    VaultManager& vm = VaultManager::instance();
    const std::string src = trimSlash(srcIn), dest = trimSlash(destIn);
    std::error_code ec;
    if (dest.empty() || dest[0] != '/' || dest.find("://") != std::string::npos) return "Choose a folder on this headset";
    if (removableMissing(dest)) return "The SD card was removed";
    if (!fs::is_directory(dest, ec)) return "This folder is not available";
    if (srcIsDir && isUnder(dest, src)) return "A folder can't go into itself";
    const std::string srcParent = trimSlash(fs::path(src).parent_path().string());
    if (mode == Mode::Move && srcParent == dest) return "It is already in this folder";
    if (vm.isVaultPath(dest)) {
        if (!vm.isUnlocked()) return "The vault is locked";
        if (!vm.available()) return "Insert the SD card with your vault";
    }
    if (::access(dest.c_str(), W_OK) != 0) return "This folder is read-only";
    if (mode == Mode::Move && ::access(srcParent.c_str(), W_OK) != 0) return "The original can't be deleted there (read-only)";
    return {};
}

void FileOps::run(const Request& r, Result& res) {
    VaultManager& vm = VaultManager::instance();
    Job job;
    job.mode = r.relocateVault ? Mode::Copy : r.mode;
    job.addDone = [this](uint64_t n) {
        std::lock_guard<std::mutex> l(mu_);
        status_.done += n;
    };
    job.stopRequested = [this, reloc = r.relocateVault] {
        return stop_.load() || (reloc && cancelReloc_.load());
    };
    res.intoPrivate = vm.isVaultPath(r.destDir);
    res.fromPrivate = !r.sources.empty() && VaultManager::isPrivatePath(r.sources[0]);
    res.privateText = res.intoPrivate || res.fromPrivate || r.destWasVault || r.srcWasVault;
    const std::string verb = r.mode == Mode::Copy ? "copy" : "move";
    std::error_code ec;

    if (r.relocateVault) {
        relocateVault(r, res, job);
        return;
    }

    if (r.sources.empty()) return;
    const std::string& main = r.sources[0];
    // The vault moved to / from the SD card since this was chosen: its old folder is no vault now.
    if (r.destWasVault != vm.isVaultPath(r.destDir) || r.srcWasVault != vm.isVaultPath(main)) {
        res.message = "Could not " + verb + " " + r.label + ": the vault was moved meanwhile";
        return;
    }
    if (!fs::exists(fs::symlink_status(main, ec))) {
        res.message = "Could not " + verb + " " + r.label + ": it is no longer there";
        return;
    }
    const bool mainIsDir = fs::is_directory(fs::symlink_status(main, ec));
    const std::string why = checkTarget(r.mode, main, mainIsDir, r.destDir);
    if (!why.empty()) {
        res.message = "Could not " + verb + " " + r.label + ": " + why;
        return;
    }
    const std::string place = placeName(r.destDir);
    const std::string done = r.mode == Mode::Copy ? "Copied " : "Moved ";
    const std::string out = mainIsDir ? job.folder(main, r.destDir) : job.file(main, r.destDir);
    if (out.empty()) {
        // The item itself failed: its sidecars stay with it.
        if (mainIsDir && job.filesOk > 0) {
            res.message = done + std::to_string(job.filesOk) + " files of " + r.label + " to " + place + "; " +
                          std::to_string(job.filesFailed + job.leftovers) + " stayed in " + r.label +
                          (job.err.empty() ? std::string() : ": " + job.err);
        } else {
            res.message = "Could not " + verb + " " + r.label + ": " + (job.err.empty() ? std::string("unknown error") : job.err);
        }
        res.moved = std::move(job.moved);
        return;
    }
    int sideOk = 0, sideFailed = 0;
    std::string sideErr;
    for (size_t i = 1; i < r.sources.size(); ++i) {
        const std::string& s = r.sources[i];
        if (!fs::exists(s, ec)) continue;
        job.err.clear();
        if (job.file(s, r.destDir).empty()) {
            ++sideFailed;
            if (sideErr.empty()) sideErr = job.err;
            if (job.fatal()) break;
        } else {
            ++sideOk;
        }
    }
    res.ok = true;
    res.moved = std::move(job.moved);
    res.message = done + r.label;
    if (sideOk > 0) res.message += " (and " + std::to_string(sideOk) + (sideOk == 1 ? " matching file)" : " matching files)");
    res.message += " to " + place;
    if (job.leftovers > 0 && r.mode == Mode::Move) {
        res.message += "; " + std::to_string(job.leftovers) + " item(s) stayed (links or files being written)";
    }
    if (sideFailed > 0) res.message += "; " + std::to_string(sideFailed) + " matching file(s) could not be: " + sideErr;
    PIXELVR_LOG_INFO("FileOps: %s done (%d files) to %s, %d matching ok, %d failed", verb.c_str(), job.filesOk,
                     res.intoPrivate ? "the vault" : "a folder", sideOk, sideFailed);
}

}  // namespace pixelvr

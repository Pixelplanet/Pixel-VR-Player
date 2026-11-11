#include "app/file_ops.hpp"

#include "app/vault_crypto.hpp"
#include "app/vault_manager.hpp"
#include "plugins/plugin_files.hpp"
#include "util/file_copy.hpp"
#include "util/logging.hpp"
#include "util/sdcard.hpp"

#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
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

bool skippedName(const std::string& n) {
    // Hidden files (vault markers, an old .thumbs folder) and unfinished writes never travel.
    return n.empty() || n[0] == '.' || (n.size() > 5 && n.compare(n.size() - 5, 5, ".part") == 0);
}

uint64_t treeBytes(const fs::path& p) {
    std::error_code ec;
    if (!fs::is_directory(p, ec)) {
        if (vaultcrypto::hasExtension(p.string())) {
            vaultcrypto::Reader r;
            if (r.open(p.string())) return r.size();
        }
        return fs::file_size(p, ec);
    }
    uint64_t sum = 0;
    for (auto it = fs::recursive_directory_iterator(p, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        const std::string n = it->path().filename().string();
        if (skippedName(n)) {
            if (it->is_directory(ec)) it.disable_recursion_pending();
            continue;
        }
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

    // Returns the new path ("" + err on failure).
    std::string file(const std::string& src, const std::string& destDir) {
        const bool needsKey = vaultcrypto::hasExtension(src) || vaultDest(destDir);
        std::string out = fileImpl(src, destDir);
        if (out.empty() && needsKey && !vaultcrypto::hasKey()) err = "the vault was locked";
        return out;
    }

    std::string fileImpl(const std::string& src, const std::string& destDir) {
        VaultManager& vm = VaultManager::instance();
        std::error_code ec;
        const bool srcEnc = vaultcrypto::hasExtension(src) && vaultcrypto::isEncrypted(src);
        const bool dstVault = vaultDest(destDir);
        const bool same = filecopy::sameDevice(src, destDir);
        auto cancelled = [this, srcEnc, dstVault] {
            return stopRequested() || ((srcEnc || dstVault) && !vaultcrypto::hasKey());
        };
        std::string name = srcEnc ? vaultcrypto::storedName(src) : fs::path(src).filename().string();
        if (name.empty()) {
            err = "the vault is locked";
            return {};
        }
        if (name.find('/') != std::string::npos) name = fs::path(src).filename().string();
        std::string out;
        if (!srcEnc && !dstVault) {
            out = (fs::path(destDir) / filecopy::freeName(destDir, name, false)).string();
            if (move() && same) {
                fs::rename(src, out, ec);
                if (ec) {
                    err = ec.message();
                    return {};
                }
                addDone(fs::file_size(out, ec));
                filecopy::syncDir(destDir);
                return out;
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
            out = (fs::path(destDir) / filecopy::freeName(destDir, name, false)).string();
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
                out = (fs::path(destDir) / filecopy::freeName(destDir, fs::path(src).filename().string(), false)).string();
                fs::rename(src, out, ec);
                if (ec) {
                    err = ec.message();
                    return {};
                }
                if (real != name) vaultcrypto::setStoredName(out, real);
                filecopy::syncDir(destDir);
                vaultcrypto::Reader r;
                if (r.open(out)) addDone(r.size());
                return out;
            }
            const std::string disk = keepDiskNames ? fs::path(src).filename().string() : std::string();
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
        if (move()) {
            fs::remove(src, ec);
            if (dstVault) vm.cleanPublicThumbs(src);
        }
        return out;
    }

    std::string folder(const std::string& src, const std::string& destDir) {
        VaultManager& vm = VaultManager::instance();
        std::error_code ec;
        const bool srcVault = vm.isVaultPath(src);
        const bool dstVault = vaultDest(destDir);
        const std::string name = fs::path(src).filename().string();
        const std::string out = (fs::path(destDir) / filecopy::freeName(destDir, name, true)).string();
        bool whole = move() && srcVault == dstVault && filecopy::sameDevice(src, destDir);
        if (whole && dstVault) {
            // Plain files would stay plain under a new path: only when none are left (the encryption
            // pass converts them) is the folder moved as it is.
            for (auto it = fs::recursive_directory_iterator(src, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
                const std::string n = it->path().filename().string();
                if (it->is_regular_file(ec) && !skippedName(n) && !vaultcrypto::hasExtension(n)) {
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
                return out;
            }
            ec.clear();
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
        bool allOk = true;
        for (const fs::path& c : children) {
            if (stopRequested()) {
                err = "stopped";
                return {};
            }
            if (skippedName(c.filename().string())) continue;
            std::error_code e2;
            const bool isDir = fs::is_directory(c, e2);
            if (!isDir && !fs::is_regular_file(c, e2)) continue;  // links, devices: left where they are
            const std::string r = isDir ? folder(c.string(), out) : file(c.string(), out);
            if (r.empty()) {
                allOk = false;
                // The vault locked or the user stopped: no point in trying the rest.
                if (err == "stopped" || !vaultcrypto::hasKey()) return {};
            }
        }
        if (!allOk) return {};
        if (move()) {
            fs::remove_all(src, ec);
            if (dstVault) vm.cleanPublicThumbs(src);
        }
        return out;
    }
};

}  // namespace

FileOps& FileOps::instance() {
    static FileOps s;
    return s;
}

FileOps::~FileOps() {
    {
        std::lock_guard<std::mutex> l(mu_);
        stop_ = true;
        jobs_.clear();
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void FileOps::queue(Request r) {
    std::lock_guard<std::mutex> l(mu_);
    jobs_.push_back(std::move(r));
    busy_ = true;
    status_.waiting = static_cast<int>(jobs_.size()) - (status_.busy ? 0 : 1);
    if (!worker_.joinable()) worker_ = std::thread([this] { loop(); });
    cv_.notify_all();
}

FileOps::Status FileOps::status() const {
    std::lock_guard<std::mutex> l(mu_);
    Status s = status_;
    s.waiting = std::max(0, static_cast<int>(jobs_.size()) - 1);
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
            r = jobs_.front();
            status_ = Status{};
            status_.busy = true;
            status_.copy = r.mode == Mode::Copy;
            status_.relocating = r.relocateVault;
            status_.label = r.label;
            status_.privateLabel = !r.sources.empty() &&
                (VaultManager::isPrivatePath(r.sources[0]) || VaultManager::instance().isVaultPath(r.destDir));
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
        run(r, res);
        std::lock_guard<std::mutex> l(mu_);
        jobs_.pop_front();
        results_.push_back(std::move(res));
        status_ = Status{};
        if (jobs_.empty()) busy_ = false;
    }
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
    for (const auto& e : fs::directory_iterator(fs::path(path).parent_path(), ec)) {
        const std::string p = e.path().string();
        if (p == path || !e.is_regular_file(ec)) continue;
        const std::string disk = e.path().filename().string();
        if (skippedName(disk)) continue;
        const std::string name = vaultcrypto::hasExtension(p) ? vaultcrypto::storedName(p) : disk;
        if (name.empty() || name == real) continue;
        for (const std::string& pat : patterns) {
            if (pluginfiles::sidecarMatch(pat, base, name)) {
                out.push_back(p);
                break;
            }
        }
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
    if (mode == Mode::Move && trimSlash(fs::path(src).parent_path().string()) == dest) return "It is already in this folder";
    if (vm.isVaultPath(dest)) {
        if (!vm.isUnlocked()) return "The vault is locked";
        if (!vm.available()) return "Insert the SD card with your vault";
    }
    if (::access(dest.c_str(), W_OK) != 0) return "This folder is read-only";
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
    job.stopRequested = [this] { return stop_.load(); };
    res.intoPrivate = vm.isVaultPath(r.destDir);
    res.fromPrivate = !r.sources.empty() && VaultManager::isPrivatePath(r.sources[0]);
    res.privateText = res.intoPrivate || res.fromPrivate;
    const std::string verb = r.mode == Mode::Copy ? "copy" : "move";
    std::error_code ec;

    if (r.relocateVault) {
        // Copy everything into the new folder (keeping the random names, fresh nonces), check the count,
        // switch, then delete the old copy. A failure leaves the old vault as it was.
        const std::string oldRoot = r.sources.empty() ? std::string() : r.sources[0];
        const std::string newRoot = r.destDir;
        job.keepDiskNames = true;
        job.destIsVault = true;
        res.relocation = true;
        res.privateText = false;
        if (!vaultcrypto::hasKey()) {
            res.message = "Unlock the vault to move it";
            return;
        }
        for (const auto& e : fs::directory_iterator(newRoot, ec)) {
            if (!skippedName(e.path().filename().string())) {  // the internal folder may hold just a marker
                res.message = "There is already a vault folder at the new place";
                return;
            }
        }
        ec.clear();
        if (freeBytes(fs::path(newRoot).parent_path().string()) < treeBytes(oldRoot) + (64ull << 20)) {
            res.message = "Not enough free space for the vault";
            return;
        }
        fs::create_directories(newRoot, ec);
        fs::permissions(newRoot, fs::perms::owner_all, fs::perm_options::replace, ec);
        if (r.toSdCard) {
            std::ofstream m(newRoot + "/.pixelvr_vault_id", std::ios::trunc);
            m << r.cardId << "\n";
            m.close();
            filecopy::syncDir(newRoot);
        }
        // The new folder is not the vault yet: tell `file()` to encrypt / re-encrypt into it anyway.
        bool ok = true;
        int count = 0, copied = 0;
        std::vector<fs::path> children;
        for (const auto& e : fs::directory_iterator(oldRoot, ec)) children.push_back(e.path());
        for (const fs::path& c : children) {
            if (skippedName(c.filename().string())) continue;
            ++count;
            std::error_code e2;
            const bool isDir = fs::is_directory(c, e2);
            // Inside the old vault every file is either encrypted (re-encrypted into the new place) or
            // a plain leftover (copied; the next unlock's pass encrypts it there).
            const std::string out = isDir ? job.folder(c.string(), newRoot) : job.file(c.string(), newRoot);
            if (out.empty()) {
                ok = false;
                break;
            }
            ++copied;
        }
        if (!ok || copied != count) {
            fs::remove_all(newRoot, ec);
            res.message = "Moving the vault stopped: " + (job.err.empty() ? std::string("copy failed") : job.err) +
                          ". The vault stays where it was.";
            return;
        }
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
        return;
    }

    if (r.sources.empty()) return;
    const std::string why = checkTarget(r.mode, r.sources[0], fs::is_directory(r.sources[0], ec), r.destDir);
    if (!why.empty()) {
        res.message = "Could not " + verb + " " + r.label + ": " + why;
        return;
    }
    int ok = 0, failed = 0;
    std::string firstErr;
    for (const std::string& src : r.sources) {
        if (!fs::exists(src, ec)) continue;
        job.err.clear();
        const bool isDir = fs::is_directory(src, ec);
        const std::string out = isDir ? job.folder(src, r.destDir) : job.file(src, r.destDir);
        if (out.empty()) {
            ++failed;
            if (firstErr.empty()) firstErr = job.err;
            if (job.err == "stopped" || job.err == "the vault was locked" || job.err == "the vault is locked") break;
            continue;
        }
        ++ok;
        if (r.mode == Mode::Move) res.moved.push_back({src, out});
    }
    const std::string place = placeName(r.destDir);
    const std::string more = r.sources.size() > 1 && ok > 1
                                 ? " (and " + std::to_string(ok - 1) + (ok - 1 == 1 ? " matching file)" : " matching files)")
                                 : std::string();
    if (failed == 0) {
        res.ok = true;
        res.message = std::string(r.mode == Mode::Copy ? "Copied " : "Moved ") + r.label + more + " to " + place;
    } else if (ok > 0) {
        res.ok = true;
        res.message = std::string(r.mode == Mode::Copy ? "Copied " : "Moved ") + r.label + more + " to " + place +
                      ", but " + std::to_string(failed) + " could not be: " + firstErr;
    } else {
        res.message = "Could not " + verb + " " + r.label + ": " + (firstErr.empty() ? "unknown error" : firstErr);
    }
    PIXELVR_LOG_INFO("FileOps: %s %d item(s) to %s, %d failed", verb.c_str(), ok,
                     res.intoPrivate ? "the vault" : r.destDir.c_str(), failed);
}

}  // namespace pixelvr

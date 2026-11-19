#include "ui/browser.hpp"

#include "util/file_copy.hpp"
#include "util/sdcard.hpp"
#include "app/file_ops.hpp"
#include "app/vault_manager.hpp"
#include "app/vault_crypto.hpp"
#include "net/remote_fs.hpp"
#include "net/smb_credentials.hpp"

#include "util/logging.hpp"
#include "util/media_files.hpp"
#include "util/smb_lock.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <set>

#ifdef PIXELVR_HAVE_SMB
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <libsmbclient.h>

#include "util/spawn.hpp"

extern char** environ;
#endif

namespace pixelvr {
namespace fs = std::filesystem;

namespace {

bool is_audio(const std::string& name) { return isAudioFile(name); }
bool is_video(const std::string& name) { return isPlayableFile(name); }  // anything playable

#ifdef PIXELVR_HAVE_SMB
void smb_auth(const char* srv, const char* shr, char* wg, int wgLen, char* user, int userLen,
              char* pass, int passLen) {
    std::string u, p, w;
    if (srv != nullptr && SmbCredentialsManager::instance().findCredentials(srv, shr ? shr : "", u, p, w)) {
        if (user && userLen > 0) {
            std::strncpy(user, u.c_str(), userLen - 1);
            user[userLen - 1] = '\0';
        }
        if (pass && passLen > 0) {
            std::strncpy(pass, p.c_str(), passLen - 1);
            pass[passLen - 1] = '\0';
        }
        if (wg && wgLen > 0 && !w.empty()) {
            std::strncpy(wg, w.c_str(), wgLen - 1);
            wg[wgLen - 1] = '\0';
        }
        return;
    }
    if (user != nullptr && userLen > 0) user[0] = '\0';  // anonymous / guest
    if (pass != nullptr && passLen > 0) pass[0] = '\0';
}

bool ensure_smb() {
    static bool inited = false;
    static bool ok = false;
    if (!inited) {
        inited = true;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
        ok = (smbc_init(smb_auth, 0) == 0);
#pragma GCC diagnostic pop
        if (!ok) {
            PIXELVR_LOG_WARN("libsmbclient init failed");
        }
    }
    return ok;
}
#endif

std::string smb_parent(const std::string& uri) {
    if (isRemoteFsPath(uri)) {
        const std::string p = remoteParent(uri);
        return p.empty() ? "smb://" : p;  // above a server: the network list
    }
    if (uri.size() <= 6) return "";  // "smb://" -> leave the network
    const std::string rest = uri.substr(6);
    const auto slash = rest.find_last_of('/');
    if (slash == std::string::npos) return "smb://";  // server -> discovery
    return "smb://" + rest.substr(0, slash);
}

// The scheme-less part of a network URI, for labels.
std::string without_scheme(const std::string& uri) {
    const std::size_t at = uri.find("://");
    return at == std::string::npos ? uri : uri.substr(at + 3);
}

// What a folder that needs a login says (names the share, never the login itself).
std::string login_message(const std::string& uri, bool refused) {
    SmbCredentialsManager::LoginTarget t;
    const std::string label =
        SmbCredentialsManager::instance().loginTargetFor(uri, t) ? t.label : without_scheme(uri);
    return refused ? "The user name or password for " + label + " was not accepted"
                   : label + " needs a user name and password";
}

// What a folder says that the login works for but may not be opened with it (a folder ACL).
std::string no_permission_message(const std::string& uri) {
    std::string name = without_scheme(uri);
    while (name.size() > 1 && name.back() == '/') name.pop_back();
    const std::size_t cut = name.find_last_of('/');
    if (cut != std::string::npos && cut + 1 < name.size()) name = name.substr(cut + 1);
    return "No permission to open " + name;
}

// Where a configured server starts: smb://host[/share], sftp://host[:port]/folder, ...
std::string share_uri(const SmbShare& s) {
    if (s.protocol == "smb") {
        std::string child = "smb://" + s.host;
        if (!s.share.empty()) child += "/" + s.share;
        return child;
    }
    std::string child = s.protocol + "://" + s.host;
    if (s.port > 0) child += ":" + std::to_string(s.port);
    std::string folder = s.share;
    while (!folder.empty() && folder.back() == '/') folder.pop_back();
    // SFTP: a folder without a leading '/' (or none) is in the user's home,
    // written "/~/..." in the URI; the server resolves relative paths there.
    if (s.protocol == "sftp" && (folder.empty() || folder.front() != '/')) {
        return child + "/~" + (folder.empty() ? "" : "/" + folder);
    }
    while (!folder.empty() && folder.front() == '/') folder.erase(folder.begin());
    return child + "/" + folder;
}

// Seconds since the filesystem clock's epoch; only used to order entries.
long long mtime_of(const fs::path& p) {
    std::error_code ec;
    const auto t = fs::last_write_time(p, ec);
    if (ec) return 0;
    return std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count();
}

void sort_and_append(std::vector<Browser::Entry>& out,
                     std::vector<Browser::Entry>& dirs,
                     std::vector<Browser::Entry>& files, int mode = 0) {
    auto order = [mode](const Browser::Entry& a, const Browser::Entry& b) {
        const bool aHasMedia = a.isVideo || !a.previewPath.empty();
        const bool bHasMedia = b.isVideo || !b.previewPath.empty();
        if (aHasMedia != bHasMedia) return aHasMedia > bHasMedia;
        if (mode == 1 && a.mtime != b.mtime) return a.mtime > b.mtime;
        if (mode == 2 && a.size != b.size) return a.size > b.size;
        return a.label < b.label;
    };
    std::stable_sort(dirs.begin(), dirs.end(), order);
    std::stable_sort(files.begin(), files.end(), order);
    out.insert(out.end(), dirs.begin(), dirs.end());
    out.insert(out.end(), files.begin(), files.end());
}

} // namespace

Browser::Browser() {
    const char* home = std::getenv("HOME");
    homeDir_ = (home != nullptr) ? home : "/home/steamos";
    downloadDir_ = homeDir_ + "/Downloads";
    quickPath_ = homeDir_ + "/Videos";
    loadBookmarks();
}

void Browser::open() {
    open_ = true;
    browseOpen_.store(true);
    status_.clear();
    saveCurrentFolderState();
    // Resume where the user last was this session (local folder or SMB share).
    if (lastInSmb_ && !lastLocation_.empty()) {
        beginScan(lastLocation_, true, true);
        return;
    }
    std::string start = homeDir_;
    if (!lastInSmb_ && !lastLocation_.empty()) {
        std::error_code ec;
        if (fs::is_directory(lastLocation_, ec)) {
            start = lastLocation_;
        }
    }
    beginScan(start, false, !lastLocation_.empty() && start == lastLocation_);
}

void Browser::move(int delta) {
    if (entries_.empty()) return;
    selected_ += delta;
    if (selected_ < 0) selected_ = 0;
    if (selected_ >= static_cast<int>(entries_.size())) {
        selected_ = static_cast<int>(entries_.size()) - 1;
    }
}

// Human-readable byte size, e.g. "1.4 GB".
std::string format_size(long long bytes) {
    if (bytes <= 0) return "";
    char buf[32];
    const double kb = 1024.0, mb = kb * 1024.0, gb = mb * 1024.0;
    if (bytes >= gb) std::snprintf(buf, sizeof(buf), "%.2f GB", bytes / gb);
    else if (bytes >= mb) std::snprintf(buf, sizeof(buf), "%.0f MB", bytes / mb);
    else if (bytes >= kb) std::snprintf(buf, sizeof(buf), "%.0f KB", bytes / kb);
    else std::snprintf(buf, sizeof(buf), "%lld B", bytes);
    return buf;
}

void Browser::setSortMode(int mode) {
    mode = ((mode % 3) + 3) % 3;
    if (sortMode_.exchange(mode) == mode) return;
    // Re-sort the current listing in place: navigation/action rows stay at the
    // top, then folders, then files.
    std::size_t firstItem = 0;
    while (firstItem < entries_.size() &&
           (entries_[firstItem].isBack || entries_[firstItem].isNetwork ||
            entries_[firstItem].path.empty())) {
        ++firstItem;
    }
    std::vector<Entry> head(entries_.begin(), entries_.begin() + static_cast<std::ptrdiff_t>(firstItem));
    std::vector<Entry> dirs, files;
    for (std::size_t i = firstItem; i < entries_.size(); ++i) {
        (entries_[i].isDir ? dirs : files).push_back(entries_[i]);
    }
    sort_and_append(head, dirs, files, mode);
    entries_ = std::move(head);
    selected_ = 0;
    first_ = 0;
}

void Browser::scanLocalInto(const std::string& dir, ScanResult& out) {
    out.inSmb = false;
    out.location = dir;
    if (VaultManager::instance().isVaultPath(dir)) {
        out.title = "Vault (Private)";
    } else if (!dir.empty() && dir == sdCardMount()) {
        out.title = "SD card";
    } else {
        out.title = dir;
    }
    out.entries.clear();

    if (dir == homeDir_) {
        out.entries.push_back({"[ Network ]", "", false, false, false, true});
        // The SD card, while one is inserted, right below Network.
        const std::string card = sdCardMount();
        if (!card.empty()) {
            const uint64_t freeB = freeBytes(card);
            out.entries.push_back({"SD card/", card, true, false, false, false,
                                   "SD card" + (freeB ? "  \xB7  " + gigabytes(freeB) + " free" : std::string())});
        }
    }
    if (dir != "/") {
        out.entries.push_back({".. (up)", "", false, false, true, false});
    }

    std::vector<Entry> dirs, files;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end;
         it.increment(ec)) {
        const fs::path p = it->path();
        const std::string name = p.filename().string();
        if (name.empty() || name[0] == '.') continue;  // skip hidden
        if (name.size() > 10 && name.compare(name.size() - 10, 10, ".pvrv.part") == 0) continue;  // being encrypted
        std::error_code dec;
        if (it->is_directory(dec)) {
            if (dir == homeDir_) {
                if (name == "devkit-game" || name == "devkit-utils" ||
                    name == "ffbuild" || name == "pixelvr-src" ||
                    name == "steamvr" || name == "work" ||
                    name == "Desktop" || name == "Templates" ||
                    name == "Public" || name == "Documents" || name == "Music") {
                    continue;
                }
            }
            std::string preview;
            long long bestPreviewSize = -1;
            int videoCount = 0;
            int audioCount = 0;
            std::error_code sec;
            for (fs::directory_iterator subIt(p, sec), subEnd; !sec && subIt != subEnd; subIt.increment(sec)) {
                const std::string subName = subIt->path().filename().string();
                if (subName.empty() || subName[0] == '.') continue;
                std::error_code sdec;
                if (!subIt->is_directory(sdec) && is_video(subName)) {
                    std::error_code fsec;
                    const long long vsz = static_cast<long long>(fs::file_size(subIt->path(), fsec));
                    const long long score = (!fsec ? vsz : 1);
                    if (score > bestPreviewSize) {
                        bestPreviewSize = score;
                        preview = subIt->path().string();
                    }
                    ++(is_audio(subName) ? audioCount : videoCount);
                } else if (subIt->is_directory(sdec)) {
                    // Check one level deeper (e.g. ~/Videos/MovieFolder/movie.mp4)
                    std::error_code ssec2;
                    for (fs::directory_iterator subIt2(subIt->path(), ssec2), subEnd2;
                         !ssec2 && subIt2 != subEnd2; subIt2.increment(ssec2)) {
                        const std::string subName2 = subIt2->path().filename().string();
                        if (subName2.empty() || subName2[0] == '.') continue;
                        if (!subIt2->is_directory(ssec2) && is_video(subName2)) {
                            std::error_code fsec2;
                            const long long vsz2 = static_cast<long long>(fs::file_size(subIt2->path(), fsec2));
                            const long long score2 = (!fsec2 ? vsz2 : 1);
                            if (score2 > bestPreviewSize) {
                                bestPreviewSize = score2;
                                preview = subIt2->path().string();
                            }
                            ++(is_audio(subName2) ? audioCount : videoCount);
                        }
                    }
                }
            }
            std::string counts;
            if (videoCount > 0) counts = std::to_string(videoCount) + (videoCount == 1 ? " video" : " videos");
            if (audioCount > 0) {
                if (!counts.empty()) counts += ", ";
                counts += std::to_string(audioCount) + (audioCount == 1 ? " track" : " tracks");
            }
            std::string detail = !counts.empty()
                ? (counts + (bestPreviewSize > 0 ? "  \xB7  " + format_size(bestPreviewSize) : ""))
                : "Folder";
            dirs.push_back({name + "/", p.string(), true, false, false, false, detail,
                            bestPreviewSize > 0 ? bestPreviewSize : 0, preview, mtime_of(p)});
        } else {
            // Encrypted vault file: shown under its real name (stored inside it) and plain size.
            std::string shown = name;
            long long plainSize = -1;
            if (name.size() > 5 && name.compare(name.size() - 5, 5, vaultcrypto::kExtension) == 0) {
                vaultcrypto::Reader vr;
                if (vr.open(p.string())) {
                    shown = vr.name();
                    plainSize = static_cast<long long>(vr.size());
                } else {
                    shown = "Encrypted file (vault locked)";
                }
            }
            const bool isVid = is_video(shown);
            if (dir == homeDir_ && !isVid) continue;  // hide log/tarball files in home root
            std::error_code sec;
            const long long sz = plainSize >= 0 ? plainSize : static_cast<long long>(fs::file_size(p, sec));
            files.push_back({shown, p.string(), false, isVid, false, false,
                             format_size(sec ? 0 : sz), sec ? 0 : sz, isVid ? p.string() : "",
                             mtime_of(p)});
        }
    }
    sort_and_append(out.entries, dirs, files, sortMode_.load());
}

void Browser::scanSmbInto(const std::string& uri, ScanResult& out) {
    out.inSmb = true;
    out.location = uri;
    out.title = uri;
    out.entries.clear();
    out.entries.push_back({".. (up)", "", false, false, true, false});

    if (isRemoteFsPath(uri)) {
        std::vector<RemoteEntry> list;
        std::string err;
        if (!remoteList(uri, list, err)) {
            if (isRemoteLoginError(err)) {  // (remoteList sends no login known to be missing or refused)
                const SmbCredentialsManager& creds = SmbCredentialsManager::instance();
                // "Not accepted" only when a password was sent; else it "needs" one.
                out.login = (creds.loginHasPassword(uri) && !creds.loginMissing(uri)) ? 2 : 1;
                out.status = login_message(uri, out.login == 2);
                return;
            }
            if (isRemotePermissionError(err)) {  // the login works on the server, not for this folder
                out.status = no_permission_message(uri);
                return;
            }
            out.status = "Cannot browse " + without_scheme(uri) + ": " + err;
            return;
        }
        std::vector<Entry> rdirs, rfiles;
        std::string base = uri;
        while (base.size() > 1 && base.back() == '/') base.pop_back();
        for (const RemoteEntry& e : list) {
            if (e.name.empty() || e.name[0] == '.') continue;
            const std::string child = base + "/" + e.name;
            if (e.dir) {
                rdirs.push_back({e.name + "/", child, true, false, false, false});
            } else {
                const long long sz = static_cast<long long>(e.size);
                rfiles.push_back({e.name, child, false, is_video(e.name), false, false, format_size(sz), sz, "",
                                  static_cast<long long>(e.mtime)});
            }
        }
        sort_and_append(out.entries, rdirs, rfiles, sortMode_.load());
        out.status.clear();  // (the header shows "1-6 of N"; the bottom row the focused name)
        return;
    }

#ifdef PIXELVR_HAVE_SMB
    std::vector<Entry> dirs, files;
    bool browsed = false;
    int openErr = 0;  // errno of a failed smbc_opendir
    SmbCredentialsManager& creds = SmbCredentialsManager::instance();
    if (uri != "smb://") {
        // A password that is not saved (asked at the first access), or a login refused here before:
        // ask for it without sending anything to the server.
        const bool missing = creds.loginMissing(uri);
        if (missing || creds.loginRefused(uri)) {
            out.login = missing ? 1 : 2;
            out.status = login_message(uri, !missing);
            return;
        }
    }
    {
        // libsmbclient is not thread-safe: hold the global SMB lock across the
        // whole enumeration so it can't race the media stream's prefetch thread
        // (concurrent use aborts inside Samba name resolution).
        std::lock_guard<std::mutex> smb(smbGlobalLock());
        if (!ensure_smb()) {
            out.status = "SMB unavailable";
            return;
        }
        errno = 0;
        const int dh = smbc_opendir(uri.c_str());
        openErr = dh < 0 ? errno : 0;
        if (dh >= 0) {
            browsed = true;
            if (uri != "smb://") creds.noteLoginWorked(uri);
            struct smbc_dirent* de = nullptr;
            while ((de = smbc_readdir(dh)) != nullptr) {
                const std::string name = de->name;
                if (name.empty() || name == "." || name == "..") continue;
                if (name.back() == '$') continue;  // hidden admin shares
                std::string child = uri;
                if (child.empty() || child.back() != '/') child += '/';
                child += name;
                switch (de->smbc_type) {
                    case SMBC_WORKGROUP:
                    case SMBC_SERVER:
                    case SMBC_FILE_SHARE:
                    case SMBC_DIR:
                        dirs.push_back({name + "/", child, true, false, false, false});
                        break;
                    case SMBC_FILE: {
                        struct stat st{};
                        long long sz = 0;
                        long long mt = 0;
                        if (smbc_stat(child.c_str(), &st) == 0) {
                            sz = static_cast<long long>(st.st_size);
                            mt = static_cast<long long>(st.st_mtime);
                        }
                        files.push_back({name, child, false, is_video(name), false,
                                         false, format_size(sz), sz, "", mt});
                        break;
                    }
                    default:
                        break;
                }
            }
            smbc_closedir(dh);
        }
    }

    // NetBIOS browsing often misses modern servers; augment the network root with
    // configured manual shares and mDNS-advertised SMB hosts (NAS, macOS, Samba+avahi).
    if (uri == "smb://") {
        std::set<std::string> seen;
        // The very first entry in the SMB list is the "+ Add SMB Share" button!
        dirs.push_back({"+ Add Network Share", "smb://__add_share__", true, false, false, false, "SMB, SFTP or WebDAV"});
        seen.insert("smb://__add_share__");

        // Prepend all user-configured manual shares (SMB, SFTP, WebDAV) so private/unadvertised servers always appear
        const auto configuredShares = SmbCredentialsManager::instance().shares();
        for (const auto& s : configuredShares) {
            if (s.host.empty()) continue;
            const std::string child = share_uri(s);
            if (!seen.insert(child).second) continue;
            std::string label = s.name.empty() ? s.host : s.name;
            std::string detail = s.anonymous ? "Configured (Guest)" : "Configured (Protected)";
            if (s.protocol != "smb") detail = std::string(remoteProtocolLabel(child)) + (s.anonymous ? " (Guest)" : "");
            // (Vault shares are only in the list while the vault is unlocked.) Marked as the vault's
            // own folder is: "Vault (Private)".
            if (s.vault) detail = std::string(remoteProtocolLabel(child)) + " \xB7 Vault (Private)";
            if (s.protocol != "smb") {
                dirs.push_back({label + "  (" + s.host + ")", child, true, false, false, false, detail});
            } else if (!s.share.empty()) {
                dirs.push_back({label + "  (" + s.host + "/" + s.share + ")", child, true, false, false, false, detail});
            } else {
                dirs.push_back({label + "  (" + s.host + ")", child, true, false, false, false, detail});
            }
        }
        for (const auto& d : dirs) seen.insert(d.path);
        // (Not popen: its shell would inherit every descriptor of ours, a playing video's decoder too.)
        std::string avahi;
        if (spawnutil::runCapture({"timeout", "4", "avahi-browse", "-trp", "_smb._tcp"}, avahi)) {
            std::size_t pos = 0;
            while (pos < avahi.size()) {
                std::size_t nl = avahi.find('\n', pos);
                if (nl == std::string::npos) nl = avahi.size();
                const std::string line = avahi.substr(pos, nl - pos);
                pos = nl + 1;
                if (line.empty() || line[0] != '=') continue;  // resolved records only
                std::vector<std::string> f;
                std::string cur;
                for (const char* c = line.c_str(); *c != '\0'; ++c) {
                    if (*c == ';') {
                        f.push_back(cur);
                        cur.clear();
                    } else if (*c != '\n') {
                        cur += *c;
                    }
                }
                f.push_back(cur);
                if (f.size() < 8 || f[7].empty()) continue;
                const std::string child = "smb://" + f[7];
                if (!seen.insert(child).second) continue;
                const std::string label = (f[3].empty() ? f[7] : f[3]);
                dirs.push_back({label + "  (" + f[7] + ")", child, true, false,
                                false, false});
            }
        }
        browsed = browsed || !dirs.empty();
    }

    if (!browsed) {
        if (uri != "smb://" && SmbCredentialsManager::isLoginErrno(openErr)) {
            // Logon failure / access denied: remembered, so this login is not sent to the share
            // again. Not when this login already listed a folder of the share: then only this
            // folder may not be opened with it.
            if (!creds.noteLoginRefused(uri)) {
                out.status = no_permission_message(uri);
                return;
            }
            out.login = creds.loginHasPassword(uri) ? 2 : 1;
            out.status = login_message(uri, out.login == 2);
            return;
        }
        out.status = "Cannot browse " + uri;
        return;
    }
    sort_and_append(out.entries, dirs, files, sortMode_.load());
    out.status.clear();  // (the header shows "1-6 of N"; the bottom row the focused name)
#else
    out.status = "SMB support not built";
#endif
}

void Browser::updateActiveFilterFor(const std::string& loc, bool isSmb) {
    if (loc == "bookmarks://") {
        activeFilter_ = 4;
    } else if (isSmb || isRemotePath(loc)) {
        activeFilter_ = 2;
    } else if (loc == kVaultMissing || VaultManager::instance().isVaultPath(loc)) {
        activeFilter_ = 5;
    } else if (!downloadDir_.empty() && loc.rfind(downloadDir_, 0) == 0) {
        activeFilter_ = 3;
    } else if (loc.rfind(homeDir_ + "/Downloads", 0) == 0) {
        activeFilter_ = 3;
    } else if (loc.rfind(homeDir_ + "/Videos", 0) == 0) {
        activeFilter_ = 1;
    } else {
        activeFilter_ = 0;
    }
}

void Browser::saveCurrentFolderState() {
    if (location_.empty() || scanning_.load()) return;
    if (entries_.size() == 1 && entries_[0].label == "Scanning..." && entries_[0].path.empty()) {
        return;
    }
    std::string selPath;
    if (selected_ >= 0 && selected_ < static_cast<int>(entries_.size())) {
        selPath = entries_[static_cast<std::size_t>(selected_)].path;
    }
    folderHistory_[location_] = {selected_, first_, selPath};
}

// Starts a background scan so SMB discovery (NetBIOS + mDNS, seconds) and large
// directories never freeze the render loop. poll() publishes the result.
void Browser::beginScan(const std::string& location, bool isSmb,
                        bool restoreView, const std::string& childPath) {
    // (A folder in the vault has a plain name: not logged.)
    PIXELVR_LOG_INFO("beginScan '%s' smb=%d scanning=%d restore=%d", VaultManager::forLog(location).c_str(),
                     isSmb ? 1 : 0, scanning_.load() ? 1 : 0, restoreView ? 1 : 0);
    updateActiveFilterFor(location, isSmb);
    if (location != location_ && !location_.empty()) {  // where a Cancel in the sign-in dialog goes back to
        backLoc_ = location_;
        backSmb_ = inSmb_;
    }
    location_ = location;
    inSmb_ = isSmb;
    if (location == kVaultMissing || VaultManager::instance().isVaultPath(location)) {
        title_ = "Vault (Private)";
    } else if (!location.empty() && location == sdCardMount()) {
        title_ = "SD card";
    } else if (location == "bookmarks://") {
        title_ = "Bookmarks";
    } else {
        title_ = location;
    }
    status_ = "Scanning...";
    entries_.clear();
    entries_.push_back({"Scanning...", "", false, false, false, false});
    selected_ = 0;

    if (scanning_.load() && scanThread_.joinable()) {
        PIXELVR_LOG_INFO("beginScan DEFERRED (scan in progress)");
        // A scan is already running (possibly waiting on the SMB lock held by a
        // download). Defer this request rather than joining on the main thread,
        // which would freeze the UI until the download's read returns.
        pendingScanLoc_ = location;
        pendingScanSmb_ = isSmb;
        pendingScanRestore_ = restoreView;
        pendingScanChildPath_ = childPath;
        hasPendingScan_ = true;
        return;
    }
    if (scanThread_.joinable()) scanThread_.join();  // previous scan has finished
    restoreViewOnScan_ = restoreView;
    restoreChildPath_ = childPath;
    scanReady_.store(false);
    scanning_.store(true);
    scanBookmarks_ = bookmarks_;  // the scan thread never reads bookmarks_ itself
    scanThread_ =
        std::thread([this, location, isSmb] { runScan(location, isSmb); });
}

void Browser::openPath(const std::string& localDir) {
    open_ = true;
    browseOpen_.store(true);
    status_.clear();
    beginScan(localDir, false);
}

void Browser::openNetwork() {
    open_ = true;
    browseOpen_.store(true);
    status_.clear();
    beginScan("smb://", true);
    activeFilter_ = 2;
}

void Browser::leaveLoginFolder() {
    const std::string here = location_;
    quietLogin_ = true;
    if (!backLoc_.empty() && backLoc_ != here) {
        beginScan(backLoc_, backSmb_, true, here);
    } else {
        beginScan("smb://", true, true, here);
    }
}

void Browser::openHome() {
    openPath(homeDir_);
    activeFilter_ = 0;
}

void Browser::openVideos() {
    const std::string v = homeDir_ + "/Videos";
    std::error_code ec;
    if (fs::is_directory(v, ec)) {
        openPath(v);
    } else {
        openPath(homeDir_);
    }
    activeFilter_ = 1;
}

void Browser::openDownloads() {
    std::error_code ec;
    if (!fs::is_directory(downloadDir_, ec)) {
        fs::create_directories(downloadDir_, ec);
    }
    openPath(downloadDir_);
    activeFilter_ = 3;
}

void Browser::openDrives() {
    const std::string d = "/run/media";
    std::error_code ec;
    if (fs::is_directory(d, ec)) {
        openPath(d);
    } else {
        openPath("/");
    }
    activeFilter_ = 0;
}

void Browser::openVault() {
    VaultManager& vm = VaultManager::instance();
    if (!vm.isUnlocked()) return;
    if (!vm.ensureDirectories()) {  // on an SD card that is not inserted: say so, never make a new vault
        open_ = true;
        browseOpen_.store(true);
        status_.clear();
        beginScan(kVaultMissing, false);
        activeFilter_ = 5;
        return;
    }
    openPath(vm.vaultDir());
    activeFilter_ = 5;
}

void Browser::openBookmarks() {
    open_ = true;
    browseOpen_.store(true);
    status_.clear();
    beginScan("bookmarks://", false);
    activeFilter_ = 4;
}

void Browser::addBookmark(const std::string& path, const std::string& label) {
    if (path.empty() || path == "bookmarks://" || path == "smb://__add_share__" || path == kVaultMissing) return;
    for (auto& b : bookmarks_) {
        if (b.path == path) return;
    }
    std::string l = label;
    if (l.empty()) {
        if (isRemotePath(path)) {
            std::string rest = without_scheme(path);
            while (!rest.empty() && rest.back() == '/') rest.pop_back();
            l = rest;
        } else {
            fs::path p(path);
            l = p.filename().string();
            if (l.empty()) l = path;
        }
    }
    bookmarks_.push_back({path, l});
    quickPath_ = path;
    saveBookmarks();
    if (location_ == "bookmarks://") {
        beginScan("bookmarks://", false);
    }
}

void Browser::removeBookmark(const std::string& path) {
    auto it = std::remove_if(bookmarks_.begin(), bookmarks_.end(), [&](const Bookmark& b) {
        return b.path == path;
    });
    if (it != bookmarks_.end()) {
        bookmarks_.erase(it, bookmarks_.end());
        saveBookmarks();
        if (location_ == "bookmarks://") {
            beginScan("bookmarks://", false);
        }
    }
}

bool Browser::isBookmarked(const std::string& path) const {
    if (path.empty() || path == "bookmarks://") return false;
    for (const auto& b : bookmarks_) {
        if (b.path == path) return true;
    }
    return false;
}

void Browser::loadBookmarks() {
    bookmarks_.clear();
    const char* home = std::getenv("HOME");
    std::string path = std::string(home ? home : ".") + "/.config/pixelvr/bookmarks.json";
    std::ifstream f(path);
    if (f.is_open()) {
        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        f.close();

        std::size_t pos = 0;
        while ((pos = content.find('{', pos)) != std::string::npos) {
            std::size_t endObj = content.find('}', pos);
            if (endObj == std::string::npos) break;
            std::string obj = content.substr(pos, endObj - pos + 1);
            pos = endObj + 1;

            auto extract = [&](const std::string& key) -> std::string {
                std::string pat = "\"" + key + "\"";
                auto kpos = obj.find(pat);
                if (kpos == std::string::npos) return "";
                auto col = obj.find(':', kpos + pat.length());
                if (col == std::string::npos) return "";
                auto q1 = obj.find('\"', col + 1);
                if (q1 == std::string::npos) return "";
                auto q2 = obj.find('\"', q1 + 1);
                if (q2 == std::string::npos) return "";
                return obj.substr(q1 + 1, q2 - q1 - 1);
            };
            std::string bPath = extract("path");
            std::string bLabel = extract("label");
            if (!bPath.empty()) {
                bookmarks_.push_back({bPath, bLabel});
            }
        }
    }
    // Written by an older version: bookmarks into vault folders were saved too. Kept for this run
    // (hidden while locked), and the file is written again without them.
    if (std::any_of(bookmarks_.begin(), bookmarks_.end(), [](const Bookmark& b) { return VaultManager::isPrivatePath(b.path); })) {
        saveBookmarks();
    }

    if (bookmarks_.empty() && !quickPath_.empty()) {
        addBookmark(quickPath_);
    }
}

void Browser::saveBookmarks() const {
    const char* home = std::getenv("HOME");
    std::string configDir = std::string(home ? home : ".") + "/.config/pixelvr";
    std::error_code ec;
    std::filesystem::create_directories(configDir, ec);

    std::string path = configDir + "/bookmarks.json";
    std::ofstream f(path, std::ios::trunc);
    if (!f.is_open()) return;

    // Bookmarks into vault shares or vault folders are never written (the file is outside the vault):
    // they last in memory until the vault locks (a share's) or the player quits (a folder's, hidden
    // while locked).
    std::vector<const Bookmark*> kept;
    for (const Bookmark& b : bookmarks_) {
        if (!VaultManager::isPrivatePath(b.path)) kept.push_back(&b);
    }
    f << "[\n";
    for (std::size_t i = 0; i < kept.size(); ++i) {
        f << "  {\n";
        f << "    \"path\": \"" << kept[i]->path << "\",\n";
        f << "    \"label\": \"" << kept[i]->label << "\"\n";
        f << "  }" << (i + 1 < kept.size() ? ",\n" : "\n");
    }
    f << "]\n";
    f.close();
}

void Browser::scanBookmarksInto(ScanResult& out) {
    out.inSmb = false;
    out.location = "bookmarks://";
    out.title = "Bookmarks";
    out.entries.clear();
    out.entries.push_back({".. (Home)", homeDir_, true, false, true, false});

    std::size_t visible = 0;  // vault bookmarks only count while the vault is open
    for (const auto& bm : scanBookmarks_) {
        if (!VaultManager::isPrivatePath(bm.path) || VaultManager::instance().isUnlocked()) ++visible;
    }
    if (visible == 0) return;  // the content area says what to do (the app's empty state)
    out.status.clear();

    for (const auto& bm : scanBookmarks_) {
        // Bookmarks into the vault stay invisible while it is locked.
        if (VaultManager::isPrivatePath(bm.path) && !VaultManager::instance().isUnlocked()) continue;
        if (bm.path.empty()) continue;
        const bool isSmb = isRemotePath(bm.path);
        std::error_code ec;
        const bool isVid = is_video(bm.path);
        const bool isDir = isSmb ? !isVid : fs::is_directory(bm.path, ec);
        std::string label = bm.label;
        if (label.empty()) {
            if (isSmb) {
                label = without_scheme(bm.path);
                while (!label.empty() && label.back() == '/') label.pop_back();
            } else {
                fs::path p(bm.path);
                label = p.filename().string();
                if (label.empty()) label = bm.path;
            }
        }
        std::string detail = isSmb ? std::string(remoteProtocolLabel(bm.path)) + (isVid ? " Video" : " Share")
                                   : (isDir ? "Folder" : "Video File");
        std::string previewPath = isDir ? "" : bm.path;
        out.entries.push_back({label, bm.path, isDir, isVid, false, false, detail, 0, previewPath});
    }
}

bool Browser::inVault() const {
    return location_ == kVaultMissing || VaultManager::instance().isVaultPath(location_);
}

bool Browser::inPrivateShare() const {
    return isRemotePath(location_) && SmbCredentialsManager::isVaultShareUri(location_);
}

void Browser::forgetPrivate() {
    auto priv = [](const std::string& p) { return isRemotePath(p) && SmbCredentialsManager::isVaultShareUri(p); };
    // Rows shown now (the network list, bookmarks): the vault shares' ones go at once.
    if (!priv(location_)) {
        const std::string selPath = focused() != nullptr ? focused()->path : std::string();
        entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [&](const Entry& e) { return priv(e.path); }),
                       entries_.end());
        if (priv(selPath) || selected_ >= static_cast<int>(entries_.size())) selected_ = 0;
        first_ = std::min(first_, std::max(0, static_cast<int>(entries_.size()) - 1));
    }
    // Bookmarks into vault shares (never saved: in memory only).
    bookmarks_.erase(std::remove_if(bookmarks_.begin(), bookmarks_.end(), [&](const Bookmark& b) { return priv(b.path); }),
                     bookmarks_.end());
    // Folder history: where the browser was, and the rows focused there.
    for (auto it = folderHistory_.begin(); it != folderHistory_.end();) {
        it = (priv(it->first) || priv(it->second.selectedPath)) ? folderHistory_.erase(it) : std::next(it);
    }
    if (priv(lastLocation_)) {
        lastLocation_.clear();
        lastInSmb_ = false;
    }
    if (priv(backLoc_)) {
        backLoc_.clear();
        backSmb_ = false;
    }
    if (priv(restoreChildPath_)) restoreChildPath_.clear();
    if (hasPendingScan_ && priv(pendingScanLoc_)) {
        pendingScanLoc_ = "smb://";
        pendingScanChildPath_.clear();
    }
    if (priv(pendingScanChildPath_)) pendingScanChildPath_.clear();
    if (priv(loginNeed_.uri)) loginNeed_ = {};
    status_.clear();  // (it may name a file of a vault share: "Downloaded ...", "Can't play ...")
    // Downloads from vault shares: the running one stops (its partial file is removed), queued ones
    // go, and none of them stays in the list. One that finished in this very frame (its thread is
    // done, poll() has not seen it yet) counts as running: it is not reported or settled either.
    // A job counts as private when it started as one (its share may have left the vault since), when
    // its share is a vault share now, or when it points into the vault.
    VaultManager& vm = VaultManager::instance();
    auto privJob = [&](const DownloadJob& j) { return j.intoVault || priv(j.uri) || vm.isVaultPath(j.localPath); };
    const bool privateRunning = (dlActive_.load() || dlDone_.load()) && dlActiveJob_ >= 0 &&
                                dlActiveJob_ < static_cast<int>(dlJobs_.size()) &&
                                privJob(dlJobs_[static_cast<std::size_t>(dlActiveJob_)]);
    if (privateRunning) {
        if (dlActive_.load()) stopDownload();
        dlPrivateDropped_ = true;
        dlVaultSwitched_ = false;  // (nothing is said about it now)
        dlDroppedPath_ = dlJobs_[static_cast<std::size_t>(dlActiveJob_)].localPath;
        dlName_.clear();
    } else {
        // Nothing writes into the vault's download folder: what is left there (a finished download
        // not encrypted yet) goes now.
        wipeVaultIncoming({});
    }
    dlQueue_.erase(std::remove_if(dlQueue_.begin(), dlQueue_.end(),
                                  [&](const DownloadQueueItem& q) { return q.intoVault || priv(q.uri) || vm.isVaultPath(q.destDir); }),
                   dlQueue_.end());
    dlJobs_.erase(std::remove_if(dlJobs_.begin(), dlJobs_.end(), privJob), dlJobs_.end());
    dlActiveJob_ = -1;  // (indexes moved)
    for (std::size_t i = 0; i < dlJobs_.size(); ++i) {
        if (dlJobs_[i].state == 0) dlActiveJob_ = static_cast<int>(i);
    }
}

void Browser::wipeVaultIncoming(const std::string& droppedPath) {
    VaultManager& vm = VaultManager::instance();
    std::error_code ec;
    if (!vm.isUnlocked()) {
        // Locked: nothing in it may stay (finished downloads not encrypted yet, partial files).
        const std::string incoming = vm.incomingDir();
        if (!incoming.empty()) fs::remove_all(incoming, ec);
    } else if (!droppedPath.empty() && fs::path(droppedPath).parent_path().filename() == ".incoming") {
        // Unlocked again before the stopped download's thread ended: only its own file goes.
        fs::remove_all(droppedPath, ec);
        fs::remove(droppedPath + ".part", ec);
    }
}

bool Browser::downloadingIntoVault() const {
    return std::any_of(dlJobs_.begin(), dlJobs_.end(), [](const DownloadJob& j) {
        return (j.state == 0 || j.state == 4) &&
               (j.intoVault || (isRemotePath(j.uri) && SmbCredentialsManager::isVaultShareUri(j.uri)));
    });
}

bool Browser::isFileActionTarget(const Entry& e) const {
    if (e.isBack || e.isNetwork || e.path.empty() || e.path[0] != '/' || inSmb_) return false;
    if (location_ == kVaultMissing || isRemotePath(e.path)) return false;
    const std::string card = sdCardMount();
    if (!card.empty() && e.path == card) return false;  // the card itself (Delete would wipe it)
    if (e.path.size() > 5 && e.path.compare(e.path.size() - 5, 5, ".part") == 0) return false;  // being written
    if (isDownloadTarget(e.path)) return false;  // a running or queued download's target
    if (!e.isDir && e.label == "Encrypted file (vault locked)") return false;
    return true;
}

namespace {
bool pathUnder(const std::string& p, const std::string& dir) {
    return p == dir || (p.size() > dir.size() && p.compare(0, dir.size(), dir) == 0 && p[dir.size()] == '/');
}

// Why a download can't go where it belongs (Browser::destinationFor): the status line and the
// Downloads list's reason. (Neither names the share or the file.) 2 = kDestVaultMoving.
std::string destStatus(int why) {
    return why == 2 ? "Download not started: wait until the vault has moved" : "Download failed: the vault can't be used now";
}
std::string destError(int why) {
    return why == 2 ? "Wait until the vault has moved" : "The vault can't be used now";
}
}  // namespace

bool Browser::isUnfinishedDownload(const std::string& path) const {
    if (path.empty() || isRemotePath(path)) return false;
    // Plain copies of vault-share downloads (being written, or waiting to be encrypted).
    const std::string incoming = VaultManager::instance().incomingDir();
    if (!incoming.empty() && pathUnder(path, incoming)) return true;
    for (const DownloadJob& j : dlJobs_) {
        if ((j.state == 0 || j.state == 4) && !j.localPath.empty() && pathUnder(path, j.localPath)) return true;
    }
    return false;
}

int Browser::destinationFor(const std::string& uri, const std::string& name, std::string& target, bool& intoVault) {
    intoVault = isRemotePath(uri) && SmbCredentialsManager::isVaultShareUri(uri);
    std::string destDir = downloadDir_.empty() ? homeDir_ + "/Downloads" : downloadDir_;
    if (intoVault) {
        // From a vault share: into the vault (never the download folder). Written into a hidden
        // folder inside it until complete, then encrypted from there into the vault.
        VaultManager& vm = VaultManager::instance();
        if (!vm.isUnlocked() || !vm.ensureDirectories() || vm.incomingDir().empty()) return kDestVaultUnusable;
        // The vault is moving (to / from the SD card): its download folder would be left behind
        // at the old place, outside the vault.
        if (FileOps::instance().relocationPending()) return kDestVaultMoving;
        destDir = vm.incomingDir();
        std::error_code ec;
        fs::create_directories(destDir, ec);
        fs::permissions(destDir, fs::perms::owner_all, fs::perm_options::replace, ec);
        if (!fs::is_directory(destDir, ec)) return kDestVaultUnusable;
    }
    target = destDir + "/" + name;
    return 0;
}

bool Browser::dlPrivate(const std::string& uri) const {
    return dlQuiet_.load() || VaultManager::isPrivatePath(uri);
}

int Browser::reconcileVaultDownloads() {
    // Whether a download from `uri` belongs into the vault now.
    auto vaultNow = [](const std::string& uri) { return isRemotePath(uri) && SmbCredentialsManager::isVaultShareUri(uri); };
    int n = 0;
    // The running one: stopped (it can't be moved while smbclient writes it). One that finished in
    // this very frame is left to poll(): a finished vault download is encrypted into the vault even
    // when its share has just left it, and a finished normal one is complete where it is.
    if (dlActive_.load() && dlActiveJob_ >= 0 && dlActiveJob_ < static_cast<int>(dlJobs_.size())) {
        const DownloadJob& a = dlJobs_[static_cast<std::size_t>(dlActiveJob_)];
        if (vaultNow(a.uri) != a.intoVault && !dlPrivateDropped_) {
            stopDownload();
            dlVaultSwitched_ = true;  // poll(): says why, removes what it wrote
            dlDroppedPath_ = a.localPath;
            ++n;
        }
    }
    // Queued ones: where their share belongs now; failed if that can't be used.
    for (DownloadJob& j : dlJobs_) {
        if (j.state != 4 || vaultNow(j.uri) == j.intoVault) continue;
        std::string target;
        bool iv = false;
        const int bad = destinationFor(j.uri, fs::path(j.localPath).filename().string(), target, iv);
        for (auto it = dlQueue_.begin(); it != dlQueue_.end(); ++it) {
            if (it->uri != j.uri) continue;
            if (bad != 0) {
                dlQueue_.erase(it);
            } else {
                it->destDir = target;
                it->intoVault = iv;
            }
            break;
        }
        if (bad != 0) {
            j.state = 2;
            j.error = destError(bad);
            ++n;
        } else {
            j.localPath = target;  // (nothing lost: not counted)
            j.intoVault = iv;
        }
    }
    return n;
}

void Browser::shutdownDownloads() {
    dlQueue_.clear();
    if (dlActive_.load()) {
        // A paused one too: SIGCONT first (a stopped child must never be left behind), then the
        // kill. The thread removes the partial file (dlStopped_); a folder starts no further file.
        dlPaused_.store(false);  // (a folder thread waiting in its pause loop goes on to the abort)
        dlAbort_.store(true);
        dlStopped_.store(true);
        const int pid = dlPid_.load();
        if (pid > 0) {
            kill(pid, SIGCONT);
            kill(pid, SIGKILL);
        }
        PIXELVR_LOG_INFO("Download stopped at shutdown");
    }
    if (dlThread_.joinable()) dlThread_.join();
    dlActive_.store(false);
}

void Browser::retargetBookmarks(const std::string& from, const std::string& to) {
    bool changed = false;
    for (Bookmark& b : bookmarks_) {
        if (!pathUnder(b.path, from)) continue;
        const bool exact = b.path == from;
        b.path = to + b.path.substr(from.size());
        if (exact && !to.empty()) {
            const std::string leaf = fs::path(to).filename().string();
            if (!leaf.empty() && !VaultManager::isPrivatePath(to)) b.label = leaf;
        }
        changed = true;
    }
    if (changed) {
        saveBookmarks();
        if (location_ == "bookmarks://") beginScan("bookmarks://", false);
    }
}

void Browser::dropBookmarksUnder(const std::string& path) {
    const auto it = std::remove_if(bookmarks_.begin(), bookmarks_.end(), [&](const Bookmark& b) { return pathUnder(b.path, path); });
    if (it == bookmarks_.end()) return;
    bookmarks_.erase(it, bookmarks_.end());
    saveBookmarks();
    if (location_ == "bookmarks://") beginScan("bookmarks://", false);
}

bool Browser::deleteFocused(std::string& deletedName) {
    const Entry* e = focused();
    if (e == nullptr || e->isBack || inSmb_) return false;
    std::error_code ec;
    deletedName = e->label;
    fs::path p(e->path);
    VaultManager::instance().cleanPublicThumbs(e->path);
    VaultManager::instance().cleanVaultThumbs(e->path);  // a deleted vault file takes its thumbnail along
    if (fs::is_directory(p, ec)) {
        fs::remove_all(p, ec);
    } else {
        fs::remove(p, ec);
    }
    if (ec) {
        status_ = "Delete failed: " + ec.message();
        return false;
    }
    status_ = "Deleted " + deletedName;
    saveCurrentFolderState();
    beginScan(location_, inSmb_, true);
    return true;
}

void Browser::rescan() {
    saveCurrentFolderState();
    beginScan(location_, inSmb_, true);
}

std::vector<Browser::Crumb> Browser::crumbs() const {
    std::vector<Crumb> out;
    if (location_.empty()) return out;
    if (location_ == "bookmarks://") return {{"Bookmarks", location_, false}};
    if (location_ == kVaultMissing) return {{"Vault (Private)", location_, false}};
    auto lastPart = [](const std::string& p) {
        std::string s = p;
        while (s.size() > 1 && s.back() == '/') s.pop_back();
        const std::size_t slash = s.find_last_of('/');
        return slash == std::string::npos ? s : s.substr(slash + 1);
    };
    if (inSmb_) {
        // Same steps as "..": folder -> share -> server -> network list.
        for (std::string p = location_; !p.empty() && out.size() < 64; p = smb_parent(p)) {
            out.push_back({p == "smb://" ? "Network" : lastPart(without_scheme(p)), p, true});
        }
    } else {
        const std::string vault = VaultManager::instance().vaultDir();
        const bool inVaultDir = VaultManager::instance().isVaultPath(location_);
        const std::string card = sdCardMount();
        for (fs::path p(location_); out.size() < 64; p = p.parent_path()) {
            const std::string ps = p.string();
            if (inVaultDir && ps == vault) {
                out.push_back({"Vault (Private)", ps, false});
                break;
            }
            if (!inVaultDir && !card.empty() && ps == card) {  // the card is a root, like Home
                out.push_back({"SD card", ps, false});
                break;
            }
            if (ps == homeDir_) {
                out.push_back({"Home", ps, false});
                break;
            }
            if (ps.empty() || ps == "/" || !p.has_parent_path() || p.parent_path() == p) {
                if (ps == "/") out.push_back({"/", ps, false});
                break;
            }
            out.push_back({lastPart(ps), ps, false});
        }
    }
    std::reverse(out.begin(), out.end());
    return out;
}

void Browser::openCrumb(int index) {
    const std::vector<Crumb> c = crumbs();
    if (index < 0 || index + 1 >= static_cast<int>(c.size())) return;  // the current folder: nothing to do
    const Crumb& to = c[static_cast<std::size_t>(index)];
    // Coming back up keeps the folder we came from selected, like "..".
    beginScan(to.path, to.smb, true, c[static_cast<std::size_t>(index) + 1].path);
}

bool Browser::canCreateFolder() const {
    return !inSmb_ && !location_.empty() && location_[0] == '/' && !isRemotePath(location_);
}

bool Browser::createFolder(const std::string& name) {
    if (!canCreateFolder() || name.empty()) return false;
    if (name == "." || name == ".." || name.find_first_of("/\\") != std::string::npos) {
        status_ = "Folder names can't contain / or \\";
        return false;
    }
    std::error_code ec;
    const fs::path dest = fs::path(location_) / name;
    if (fs::exists(dest, ec)) {
        status_ = "An item with that name already exists";
        return false;
    }
    if (!fs::create_directory(dest, ec) || ec) {
        status_ = "Could not create folder: " + (ec ? ec.message() : std::string("unknown error"));
        return false;
    }
    status_ = "Created folder " + name;
    saveCurrentFolderState();
    folderHistory_[location_].selectedPath = dest.string();
    beginScan(location_, inSmb_, true);
    return true;
}

bool Browser::renameFocused(const std::string& newName) {
    const Entry* e = focused();
    if (e == nullptr || e->isBack || inSmb_ || newName.empty() || !isFileActionTarget(*e)) return false;
    if (newName == "." || newName == ".." || newName.find_first_of("/\\") != std::string::npos) {
        status_ = "A name can't contain / or \\";
        return false;
    }
    std::error_code ec;
    fs::path src(e->path);
    if (src.extension() == vaultcrypto::kExtension) {
        // Encrypted vault file: the real name lives inside it.
        if (!vaultcrypto::setStoredName(src.string(), newName)) {
            status_ = "Rename failed (vault locked?)";
            return false;
        }
        status_ = "Renamed to " + newName;
        beginScan(location_, inSmb_, true);
        return true;
    }
    fs::path dest = src.parent_path() / newName;
    if (dest == src) return true; // unchanged
    if (fs::exists(dest, ec)) {
        status_ = "An item with that name already exists";
        return false;
    }
    fs::rename(src, dest, ec);
    if (ec) {
        status_ = "Rename failed: " + ec.message();
        return false;
    }
    status_ = "Renamed to " + newName;
    saveCurrentFolderState();
    folderHistory_[location_].selectedPath = dest.string();
    beginScan(location_, inSmb_, true);
    return true;
}

std::string Browser::parentDir() const {
    if (location_ == "bookmarks://") return homeDir_;
    if (location_.empty() || location_ == "/" || inSmb_) return "";
    fs::path p(location_);
    return p.parent_path().string();
}

std::string Browser::homeDir() const {
    const char* h = std::getenv("HOME");
    return h ? std::string(h) : "/";
}

std::string Browser::videosDir() const {
    std::string d = homeDir() + "/Videos";
    std::error_code ec;
    return fs::is_directory(d, ec) ? d : homeDir();
}

std::string Browser::downloadsDir() const {
    std::string d = homeDir() + "/Downloads";
    std::error_code ec;
    return fs::is_directory(d, ec) ? d : homeDir();
}

void Browser::runScan(std::string location, bool isSmb) {
    ScanResult r;
    if (location == kVaultMissing) {
        r.location = location;
        r.title = "Vault (Private)";
        r.status = "Insert the SD card with your vault";
        r.entries.push_back({".. (up)", "", false, false, true, false});
        r.entries.push_back({"Insert the SD card with your vault", "", false, false, false, false,
                             "The vault is on the SD card"});
    } else if (location == "bookmarks://") {
        scanBookmarksInto(r);
    } else if (isSmb) {
        scanSmbInto(location, r);
    } else {
        scanLocalInto(location, r);
    }
    // (A folder in the vault has a plain name, and the status of a vault share's folder may name the
    // share: neither is logged.)
    const bool privScan = VaultManager::isPrivatePath(location);
    PIXELVR_LOG_INFO("scan done '%s': %zu entries (status=%s)", privScan ? "(private)" : location.c_str(), r.entries.size(),
                     privScan ? "(private)" : r.status.c_str());
    {
        std::lock_guard<std::mutex> lk(scanMutex_);
        scanResult_ = std::move(r);
    }
    scanReady_.store(true);
    scanning_.store(false);  // last: keeps isBusy() true until results are ready
}

Browser::Result Browser::activate(std::string& playPath) {
    if (selected_ < 0 || selected_ >= static_cast<int>(entries_.size())) {
        return Result::None;
    }
    const Entry e = entries_[static_cast<std::size_t>(selected_)];

    if (e.isBack) {
        const std::string prevChild = location_;
        if (location_ == "bookmarks://" || location_ == kVaultMissing) {
            beginScan(homeDir_, false, true, location_ == kVaultMissing ? std::string() : prevChild);
            return Result::Navigated;
        }
        if (!inSmb_ && !location_.empty() && location_ == sdCardMount()) {  // up from the card: home
            beginScan(homeDir_, false, true, prevChild);
            return Result::Navigated;
        }
        if (inSmb_) {
            const std::string parent = smb_parent(location_);
            if (parent.empty()) {
                beginScan(homeDir_, false, true, prevChild);
            } else {
                beginScan(parent, true, true, prevChild);
            }
        } else {
            if (VaultManager::instance().isVaultPath(location_)) {
                if (location_ == VaultManager::instance().vaultDir()) {
                    beginScan(homeDir_, false, true, prevChild);
                } else {
                    const fs::path p(location_);
                    beginScan(p.has_parent_path() ? p.parent_path().string() : homeDir_, false, true, prevChild);
                }
            } else {
                const fs::path p(location_);
                beginScan(p.has_parent_path() ? p.parent_path().string() : "/", false, true, prevChild);
            }
        }
        return Result::Navigated;
    }
    if (e.path == "smb://__add_share__") {
        return Result::AddSmbShare;
    }
    if (e.isNetwork) {
        saveCurrentFolderState();
        beginScan("smb://", true, false);
        return Result::Navigated;
    }
    if (e.isDir) {
        saveCurrentFolderState();
        const bool isSmb = e.isNetwork || isRemotePath(e.path);
        beginScan(e.path, isSmb, false);
        return Result::Navigated;
    }
    if (e.isVideo) {
        saveCurrentFolderState();
        // Stream by default: local files and SMB shares both play in place. The
        // media engine opens smb:// URLs directly, so there is no need to copy
        // the file to the device first. Downloading is an explicit action
        // (downloadFocused), surfaced as the browser's context option.
        playPath = e.path;
        return Result::Play;
    }
    if (!isRemotePath(e.path) && hasExtension(e.path, {".pvrplugin"})) {
        playPath = e.path;
        return Result::InstallPlugin;
    }
    return Result::None;
}

bool Browser::downloadFocused() {
    if (selected_ < 0 ||
        selected_ >= static_cast<int>(entries_.size())) {
        return false;
    }
    const Entry e = entries_[static_cast<std::size_t>(selected_)];
    if (!inSmb_ || e.isBack || e.isNetwork || e.isDir || e.path.empty()) {
        status_ = inSmb_ ? "Highlight a file to download (folders ask to confirm)"
                         : "Downloads are for network files";
        PIXELVR_LOG_WARN("Download: nothing downloadable focused (dir=%d smb=%d)",
                         e.isDir ? 1 : 0, inSmb_ ? 1 : 0);
        return false;
    }
    startDownload(e.path, e.label, false);
    return true;
}

const Browser::Entry* Browser::focused() const {
    if (selected_ < 0 || selected_ >= static_cast<int>(entries_.size())) {
        return nullptr;
    }
    return &entries_[static_cast<std::size_t>(selected_)];
}

Browser::~Browser() {
    shutdownDownloads();  // (normally done already, before the XR teardown)
    if (scanThread_.joinable()) scanThread_.join();
    if (dlThread_.joinable()) dlThread_.join();
    if (measureThread_.joinable()) measureThread_.join();
}

void Browser::startDownload(const std::string& uri, const std::string& nameIn,
                            bool isDir) {
    std::string name = nameIn;
    while (!name.empty() && name.back() == '/') name.pop_back();
    const std::size_t slash = name.find_last_of('/');
    if (slash != std::string::npos) name = name.substr(slash + 1);
    if (name.empty()) return;

    for (const auto& j : dlJobs_) {
        if (j.uri == uri && (j.state == 0 || j.state == 4)) {
            status_ = "Already downloading or in queue: " + name;
            return;
        }
    }

    // Where it goes, and whether it is a vault download, is decided here once and stays with the job.
    std::string targetPath;
    bool intoVault = false;
    if (const int bad = destinationFor(uri, name, targetPath, intoVault); bad != 0) {
        status_ = destStatus(bad);
        return;
    }
    const std::string destDir = fs::path(targetPath).parent_path().string();

    if (removableMissing(destDir, true)) {  // the chosen folder is on a drive that is not inserted
        DownloadJob j{name, targetPath, uri, 2, 0, 0, isDir};
        j.error = "SD card removed";
        j.intoVault = intoVault;
        dlJobs_.push_back(j);
        status_ = "Download failed: SD card removed";
        return;
    }
    // A job that finished this frame is resolved in poll(): until then the slot is still taken.
    if (dlActive_.load() || dlDone_.load()) {
        dlQueue_.push_back({uri, name, targetPath, isDir, intoVault});
        DownloadJob j{name, targetPath, uri, 4, 0, 0, isDir};
        j.intoVault = intoVault;
        dlJobs_.push_back(j);
        status_ = "Added to download queue: " + name;
        PIXELVR_LOG_INFO("Added to download queue: %s",
                         intoVault ? "(private)" : VaultManager::forLog(uri, name + " (" + uri + ")").c_str());
        return;
    }

    dlName_ = name;
    if (isDir) {
        startDownloadFolder(uri, targetPath, intoVault);
    } else {
        startDownloadFile(uri, targetPath, intoVault);
    }
}

void Browser::startDownloadFile(const std::string& uri, const std::string& local, bool intoVault) {
#ifdef PIXELVR_HAVE_SMB
    if (dlThread_.joinable()) dlThread_.join();
    // (Its thread reads this: set while no download thread runs.)
    dlQuiet_.store(intoVault || VaultManager::isPrivatePath(uri) || VaultManager::isPrivatePath(local));
    dlDone_.store(false);
    dlOk_.store(false);
    dlBytes_.store(0);
    dlTotal_.store(0);
    dlLocalPath_.clear();
    dlSpeed_ = 0.0;
    speedBytes_ = 0;
    speedTime_ = {};
    dlStopped_.store(false);
    dlCardGone_.store(false);
    dlLoginFailed_.store(false);
    dlPid_.store(-1);
    dlBaseBytes_.store(0);
    pausedByBrowse_ = false;
    {
        std::lock_guard<std::mutex> lk(dlCurMutex_);
        dlCurFile_.clear();
    }
    dlPaused_.store(false);
    dlAbort_.store(false);
    dlActive_.store(true);
    status_ = "Downloading " + dlName_ + " ...";
    PIXELVR_LOG_INFO("Download file: %s", dlQuiet_.load() ? "(private)" : (uri + " -> " + local).c_str());

    int existingIdx = -1;
    for (std::size_t i = 0; i < dlJobs_.size(); ++i) {
        if (dlJobs_[i].uri == uri && (dlJobs_[i].state == 4 || dlJobs_[i].state == 0)) {
            existingIdx = static_cast<int>(i);
            dlJobs_[i].state = 0;
            dlJobs_[i].name = dlName_;
            dlJobs_[i].localPath = local;
            dlJobs_[i].intoVault = intoVault;
            break;
        }
    }
    if (existingIdx >= 0) {
        dlActiveJob_ = existingIdx;
    } else {
        DownloadJob nj{dlName_, local, uri, 0, 0, 0, false};
        nj.intoVault = intoVault;
        dlJobs_.push_back(nj);
        dlActiveJob_ = static_cast<int>(dlJobs_.size()) - 1;
        if (dlJobs_.size() > 16) {
            for (auto it = dlJobs_.begin(); it != dlJobs_.end(); ++it) {
                if (it->state != 0 && it->state != 4) {
                    dlJobs_.erase(it);
                    break;
                }
            }
            for (std::size_t i = 0; i < dlJobs_.size(); ++i) {
                if (dlJobs_[i].state == 0) dlActiveJob_ = static_cast<int>(i);
            }
        }
    }
    const std::string name = dlName_;
    // Every download writes <name>.part and renames it when complete: a file under its real name is
    // always a whole one (poll() measures progress from the .part file).
    dlLocalPath_ = local + ".part";
    if (dlActiveJob_ >= 0 && dlActiveJob_ < static_cast<int>(dlJobs_.size())) {
        dlJobs_[static_cast<std::size_t>(dlActiveJob_)].wrote = !(isRemoteFsPath(uri) || isHttpUrl(uri));
    }
    if (isRemoteFsPath(uri) || isHttpUrl(uri)) {
        dlThread_ = std::thread([this, uri, local, name]() { downloadRemoteFile(uri, local, name); });
        return;
    }
    // Copy with a curl child process: it opens its own SMB connection, so the
    // browser's libsmbclient is never blocked and browsing keeps working.
    dlThread_ = std::thread([this, uri, local, name]() {
        std::error_code ec;
        std::filesystem::create_directories(
            std::filesystem::path(local).parent_path(), ec);
        const std::string part = local + ".part";

        struct stat st{};
        long long total = 0;
        // The share wants a login (not saved, or refused before): stop before smbclient sends one.
        SmbCredentialsManager& creds = SmbCredentialsManager::instance();
        bool loginFailed = creds.loginBlocked(uri);
        if (!loginFailed) {
            std::lock_guard<std::mutex> smb(smbGlobalLock());
            errno = 0;
            if (smbc_stat(uri.c_str(), &st) == 0) {
                total = static_cast<long long>(st.st_size);
                creds.noteLoginWorked(uri);
            } else if (SmbCredentialsManager::isLoginErrno(errno)) {
                // (False: the login works in the share, this file may not be read; smbclient below
                // then fails on it like on any other unreadable file.)
                loginFailed = creds.noteLoginRefused(uri);
            }
        }
        if (loginFailed) {
            PIXELVR_LOG_WARN("Download: %s needs a login", dlPrivate(uri) ? "(private)" : name.c_str());
            dlLoginFailed_.store(true);
            dlDone_.store(true);
            dlActive_.store(false);
            return;
        }
        dlTotal_.store(total);
        PIXELVR_LOG_INFO("Download: %s size=%lld bytes", dlPrivate(uri) ? "(private)" : name.c_str(), total);

        // Reuse a previous complete download instead of fetching again.
        std::error_code lec;
        if (total > 0 && std::filesystem::exists(local, lec) &&
            static_cast<long long>(std::filesystem::file_size(local, lec)) == total) {
            dlBaseBytes_.store(total);
            dlBytes_.store(total);
            dlOk_.store(true);
            dlDone_.store(true);
            dlActive_.store(false);
            return;
        }
        if (dlAbort_.load()) {  // stopped (or the player quits) before smbclient started
            dlDone_.store(true);
            dlActive_.store(false);
            return;
        }

        const int pid = spawnCurl(uri, part, dlPrivate(uri));
        dlPid_.store(pid);
        if (pid <= 0) {
            PIXELVR_LOG_WARN("Download: smbclient could not be started");
            dlDone_.store(true);
            dlActive_.store(false);
            return;
        }
        // A stop that came while it was being started saw no pid yet: it is killed here instead.
        if (dlAbort_.load()) kill(pid, SIGKILL);
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        dlPid_.store(-1);
        bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0 && !dlStopped_.load();
        long long got = -1;
        if (ok) {
            // smbclient's exit status alone does not prove the file is whole: the size must match
            // the server's (asked again once: the file may have grown since the first look).
            std::error_code zec;
            const auto sz = std::filesystem::file_size(part, zec);
            got = zec ? -1 : static_cast<long long>(sz);
            if (got >= 0 && total > 0 && got != total) {
                struct stat again{};
                std::lock_guard<std::mutex> smb(smbGlobalLock());
                if (smbc_stat(uri.c_str(), &again) == 0 && static_cast<long long>(again.st_size) == got) {
                    total = got;
                    dlTotal_.store(total);
                }
            }
            if (got < 0 || (total > 0 && got != total)) {
                PIXELVR_LOG_WARN("Download %s: incomplete (%lld of %lld bytes)", dlPrivate(uri) ? "(private)" : name.c_str(),
                                 got, total);
                ok = false;
            }
        }
        if (ok) {  // on disk before "Downloaded" (an SD card pulled right after keeps the file)
            filecopy::syncFile(part);
            // (poll() reads the base once the .part is renamed away: the job keeps its full size.)
            dlBaseBytes_.store(got);
            dlBytes_.store(got);
            std::error_code rec;
            std::filesystem::rename(part, local, rec);
            ok = !rec;
            if (ok) filecopy::syncDir(std::filesystem::path(local).parent_path().string());
        }
        if (!ok) {
            std::error_code rec;
            std::filesystem::remove(part, rec);  // no partial file left behind
        }
        dlOk_.store(ok);
        dlDone_.store(true);
        dlActive_.store(false);
        PIXELVR_LOG_INFO("Download %s: ok=%d (smbclient exit %d)", dlPrivate(uri) ? "(private)" : (name + " -> " + local).c_str(),
                         ok ? 1 : 0, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    });
#else
    (void)uri;
    (void)local;
    (void)intoVault;
    status_ = "SMB support not built";
#endif
}

// Spawns a child that copies `uri` (smb://) to `local`; returns its pid. Uses
// Samba's own smbclient (same stack as our libsmbclient) so guest access matches.
// NOTE: posix_spawn (not fork) — forking a multithreaded process can deadlock.
// `quiet`: a private download (nothing about it in the log).
int Browser::spawnCurl(const std::string& uri, const std::string& local, bool quiet) {
    std::string rest = uri;
    if (rest.rfind("smb://", 0) == 0) rest = rest.substr(6);
    const std::size_t s1 = rest.find('/');
    const std::size_t s2 =
        (s1 == std::string::npos) ? std::string::npos : rest.find('/', s1 + 1);
    if (s2 == std::string::npos) return -1;
    const std::string host = rest.substr(0, s1);
    const std::string share = rest.substr(s1 + 1, s2 - s1 - 1);
    const std::string service = "//" + rest.substr(0, s2);
    const std::string remote = rest.substr(s2 + 1);
    std::string cmd = "get \"";
    cmd += remote;
    cmd += "\" \"";
    cmd += local;
    cmd += "\"";

    // The login goes to smbclient as an auth file read from a pipe (/dev/fd/3 in the child): the
    // password never lands on the disk (a password kept for this session only, too).
    std::string authText = SmbCredentialsManager::instance().authFileText(host, share);
    int authPipe[2] = {-1, -1};
    if (!authText.empty()) {
        bool sent = pipe2(authPipe, O_CLOEXEC) == 0;
        // (A few lines: they fit the pipe's buffer, so this write never waits for the reader.)
        if (sent) sent = ::write(authPipe[1], authText.data(), authText.size()) == static_cast<ssize_t>(authText.size());
        std::fill(authText.begin(), authText.end(), '\0');
        if (authPipe[1] >= 0) ::close(authPipe[1]);
        if (!sent) {
            if (authPipe[0] >= 0) ::close(authPipe[0]);
            PIXELVR_LOG_WARN("DL: could not hand the login to smbclient");
            return -1;
        }
    }
    const std::string authFile = authPipe[0] >= 0 ? "/dev/fd/3" : "";
    if (quiet || SmbCredentialsManager::isVaultShareUri(uri)) {
        PIXELVR_LOG_INFO("DL spawn: smbclient (private)");
    } else {
        PIXELVR_LOG_INFO("DL spawn: smbclient %s %s -c '%s'", service.c_str(),
                         authFile.empty() ? "-N" : ("-A " + authFile).c_str(), cmd.c_str());
    }

    char* argv[8] = {nullptr};
    argv[0] = const_cast<char*>("smbclient");
    argv[1] = const_cast<char*>(service.c_str());
    if (!authFile.empty()) {
        argv[2] = const_cast<char*>("-A");
        argv[3] = const_cast<char*>(authFile.c_str());
        argv[4] = const_cast<char*>("-c");
        argv[5] = const_cast<char*>(cmd.c_str());
        argv[6] = nullptr;
    } else {
        argv[2] = const_cast<char*>("-N");
        argv[3] = const_cast<char*>("-c");
        argv[4] = const_cast<char*>(cmd.c_str());
        argv[5] = nullptr;
    }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    // Take stdin from /dev/null so smbclient can never block on an auth prompt.
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    // Its own output names the file and the share ("getting file \...") and would go straight into
    // the player's log, past the log filter: never kept (nothing is read from it).
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    if (authPipe[0] >= 0) posix_spawn_file_actions_adddup2(&fa, authPipe[0], 3);  // (dup2 clears close-on-exec)
    // Nothing else of ours goes along: an inherited hardware decoder keeps its session alive in the
    // driver for the whole download (every new 8K video is then refused).
    spawnutil::closeFrom(&fa, authPipe[0] >= 0 ? 4 : 3);
    pid_t pid = -1;
    const int rc = posix_spawn(&pid, "/usr/bin/smbclient", &fa, nullptr, argv,
                               environ);
    posix_spawn_file_actions_destroy(&fa);
    if (authPipe[0] >= 0) ::close(authPipe[0]);  // (the child has its own copy)
    if (rc != 0) {
        PIXELVR_LOG_WARN("DL: posix_spawn failed rc=%d", rc);
        return -1;
    }
    return static_cast<int>(pid);
}

void Browser::startDownloadFolder(const std::string& uri,
                                  const std::string& destDir, bool intoVault) {
#ifdef PIXELVR_HAVE_SMB
    if (dlThread_.joinable()) dlThread_.join();
    // (Its thread reads this: set while no download thread runs.)
    dlQuiet_.store(intoVault || VaultManager::isPrivatePath(uri) || VaultManager::isPrivatePath(destDir));
    dlDone_.store(false);
    dlOk_.store(false);
    dlBytes_.store(0);
    dlBaseBytes_.store(0);  // the previous folder's finished files must not count
    dlTotal_.store(0);
    {
        std::lock_guard<std::mutex> lk(dlCurMutex_);
        dlCurFile_.clear();
    }
    dlLocalPath_ = destDir;
    dlSpeed_ = 0.0;
    speedBytes_ = 0;
    speedTime_ = {};
    dlActive_.store(true);
    status_ = "Downloading folder " + dlName_ + " ...";
    PIXELVR_LOG_INFO("Download folder: %s", dlQuiet_.load() ? "(private)" : (uri + " -> " + destDir).c_str());
    {
        std::error_code xec;
        dlDestExisted_.store(std::filesystem::exists(destDir, xec));
    }
    dlStopped_.store(false);
    dlCardGone_.store(false);
    dlLoginFailed_.store(false);
    dlPaused_.store(false);
    dlAbort_.store(false);
    int existingIdx = -1;
    for (std::size_t i = 0; i < dlJobs_.size(); ++i) {
        if (dlJobs_[i].uri == uri && (dlJobs_[i].state == 4 || dlJobs_[i].state == 0)) {
            existingIdx = static_cast<int>(i);
            dlJobs_[i].state = 0;
            dlJobs_[i].name = dlName_;
            dlJobs_[i].localPath = destDir;
            dlJobs_[i].intoVault = intoVault;
            break;
        }
    }
    if (existingIdx >= 0) {
        dlActiveJob_ = existingIdx;
    } else {
        DownloadJob nj{dlName_, destDir, uri, 0, 0, 0, true};
        nj.intoVault = intoVault;
        dlJobs_.push_back(nj);
        dlActiveJob_ = static_cast<int>(dlJobs_.size()) - 1;
        if (dlJobs_.size() > 16) {
            for (auto it = dlJobs_.begin(); it != dlJobs_.end(); ++it) {
                if (it->state != 0 && it->state != 4) {
                    dlJobs_.erase(it);
                    break;
                }
            }
            for (std::size_t i = 0; i < dlJobs_.size(); ++i) {
                if (dlJobs_[i].state == 0) dlActiveJob_ = static_cast<int>(i);
            }
        }
    }
    if (isRemoteFsPath(uri)) {
        dlThread_ = std::thread([this, uri, destDir]() { downloadRemoteFolder(uri, destDir); });
        return;
    }
    dlThread_ = std::thread([this, uri, destDir]() {
        std::error_code ec;
        long long got = 0;
        long long count = 0;
        bool ok = true;
        SmbCredentialsManager& creds = SmbCredentialsManager::instance();
        if (creds.loginBlocked(uri)) {  // stop before any login is sent (not saved, or refused before)
            PIXELVR_LOG_WARN("Download folder: the share needs a login");
            dlLoginFailed_.store(true);
            dlDone_.store(true);
            dlActive_.store(false);
            return;
        }
        // Pre-walk the tree to total the size, so % progress and ETA can be shown. Each file's size
        // is kept: a copy only counts when it is that long.
        long long total = 0;
        std::unordered_map<std::string, long long> sizes;
        int rootErr = 0;  // errno when the folder itself could not be listed (0 = it could)
        std::function<void(const std::string&)> sumSizes =
            [&](const std::string& src) {
                if (dlAbort_.load()) return;  // stopped (or the player quits) meanwhile
                int d = -1;
                {
                    std::lock_guard<std::mutex> smb(smbGlobalLock());
                    errno = 0;
                    d = smbc_opendir(src.c_str());
                    if (d < 0 && src == uri) rootErr = errno ? errno : EIO;
                }
                if (d < 0) return;
                if (src == uri) creds.noteLoginWorked(uri);
                for (;;) {
                    if (dlAbort_.load()) break;  // stopped / quitting: no more stats (closedir still runs)
                    std::string nm;
                    int typ = 0;
                    {
                        std::lock_guard<std::mutex> smb(smbGlobalLock());
                        struct smbc_dirent* de = smbc_readdir(d);
                        if (de == nullptr) break;
                        nm = de->name;
                        typ = de->smbc_type;
                    }
                    if (nm.empty() || nm == "." || nm == "..") continue;
                    std::string s = src;
                    if (!s.empty() && s.back() != '/') s += '/';
                    s += nm;
                    if (typ == SMBC_DIR) {
                        sumSizes(s);
                    } else {
                        struct stat st{};
                        bool known = false;
                        {
                            std::lock_guard<std::mutex> smb(smbGlobalLock());
                            known = smbc_stat(s.c_str(), &st) == 0;
                        }
                        total += static_cast<long long>(st.st_size);
                        if (known) sizes[s] = static_cast<long long>(st.st_size);
                    }
                }
                {
                    std::lock_guard<std::mutex> smb(smbGlobalLock());
                    smbc_closedir(d);
                }
            };
        sumSizes(uri);
        if (rootErr != 0 && SmbCredentialsManager::isLoginErrno(rootErr)) {
            // The folder refused access: no second try below. A refused login is remembered (and
            // shown as "needs a login"); else this login may not open the folder.
            const bool login = creds.noteLoginRefused(uri);
            PIXELVR_LOG_WARN("Download folder: %s", login ? "the share refused the login" : "no permission");
            dlLoginFailed_.store(login);
            dlOk_.store(false);
            dlDone_.store(true);
            dlActive_.store(false);
            return;
        }
        dlTotal_.store(total);
        // Depth-first copy of an SMB folder. Recomputes a running byte total so
        // the overlay can show progress as files complete.
        std::function<void(const std::string&, const std::string&)> copyDir;
        auto copyFile = [&](const std::string& src, const std::string& dst) {
            while (!dlAbort_.load() && dlPaused_.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (dlAbort_.load()) {
                ok = false;
                return;
            }
            // Written as <name>.part and renamed once it is whole (its size is the server's).
            const std::string part = dst + ".part";
            {
                std::lock_guard<std::mutex> lk(dlCurMutex_);
                dlCurFile_ = part;  // for live progress in poll()
            }
            // Each file is copied by its own smbclient child (own SMB connection).
            const int pid = spawnCurl(src, part, dlPrivate(uri));
            if (pid <= 0) {
                ok = false;
                return;
            }
            dlPid_.store(pid);
            if (dlAbort_.load()) kill(pid, SIGKILL);  // a stop that saw no pid yet
            int status = 0;
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
            }
            dlPid_.store(-1);
            {
                std::lock_guard<std::mutex> lk(dlCurMutex_);
                dlCurFile_.clear();
            }
            const bool exited = WIFEXITED(status) && WEXITSTATUS(status) == 0;
            std::error_code pec;
            const auto partSize = std::filesystem::file_size(part, pec);
            const long long have = pec ? -1 : static_cast<long long>(partSize);
            const auto want = sizes.find(src);
            bool whole = have >= 0 && (want == sizes.end() || want->second == have);
            if (exited && !whole && have >= 0) {  // asked again once: the file may have grown since the walk
                struct stat again{};
                std::lock_guard<std::mutex> smb(smbGlobalLock());
                whole = smbc_stat(src.c_str(), &again) == 0 && static_cast<long long>(again.st_size) == have;
            }
            if (!exited || !whole || dlAbort_.load()) {
                if (exited && !whole) PIXELVR_LOG_WARN("Download folder: a file came out incomplete (%lld bytes)", have);
                // Report partial bytes too, so progress is not lost on failure.
                if (have > 0) got += have;
                dlBaseBytes_.store(got);
                std::error_code rec;
                std::filesystem::remove(part, rec);  // no partial file left behind
                ok = false;
                return;
            }
            filecopy::syncFile(part);
            std::error_code mec;
            std::filesystem::rename(part, dst, mec);
            if (mec) {
                std::error_code rec;
                std::filesystem::remove(part, rec);
                ok = false;
                return;
            }
            filecopy::syncDir(std::filesystem::path(dst).parent_path().string());
            got += have;
            dlBaseBytes_.store(got);
            dlBytes_.store(got);
            ++count;
        };
        copyDir = [&](const std::string& src, const std::string& dst) {
            std::filesystem::create_directories(dst, ec);
            int d = -1;
            int openErr = 0;
            {
                std::lock_guard<std::mutex> smb(smbGlobalLock());
                errno = 0;
                d = smbc_opendir(src.c_str());
                openErr = d < 0 ? errno : 0;
            }
            if (d < 0) {
                // (A subfolder the login may not open is a plain failure: the login listed the
                // folder above, so noteLoginRefused records nothing then.)
                if (SmbCredentialsManager::isLoginErrno(openErr) && creds.noteLoginRefused(src)) {
                    dlLoginFailed_.store(true);
                }
                ok = false;
                return;
            }
            for (;;) {
                std::string nm;
                int typ = 0;
                {
                    std::lock_guard<std::mutex> smb(smbGlobalLock());
                    struct smbc_dirent* de = smbc_readdir(d);
                    if (de == nullptr) break;
                    nm = de->name;
                    typ = de->smbc_type;
                }
                if (nm.empty() || nm == "." || nm == "..") continue;
                std::string s = src;
                if (!s.empty() && s.back() != '/') s += '/';
                s += nm;
                const std::string t = dst + "/" + nm;
                if (typ == SMBC_DIR) {
                    copyDir(s, t);
                } else {
                    copyFile(s, t);
                }
                if (!ok) break;
            }
            {
                std::lock_guard<std::mutex> smb(smbGlobalLock());
                smbc_closedir(d);
            }
        };
        copyDir(uri, destDir);
        if (dlAbort_.load()) {
            std::error_code rec;
            if (!dlDestExisted_.load() && !dlCardGone_.load()) std::filesystem::remove_all(destDir, rec);  // no partial folder
            dlStopped_.store(true);
            dlOk_.store(false);
            dlDone_.store(true);
            dlActive_.store(false);
            PIXELVR_LOG_INFO("Download folder stopped: %s", dlPrivate(uri) ? "(private)" : destDir.c_str());
            return;
        }
        dlOk_.store(ok);
        dlDone_.store(true);
        dlActive_.store(false);
        PIXELVR_LOG_INFO("Download folder done: %lld files, %lld bytes (ok=%d)",
                         count, got, ok ? 1 : 0);
    });
#else
    (void)uri;
    (void)destDir;
    (void)intoVault;
    status_ = "SMB support not built";
#endif
}

// Copies `uri` to `local` (via local.part, renamed when complete). With
// `setTotal` the file's size becomes the download's total (single files).
bool Browser::copyRemoteFile(const std::string& uri, const std::string& localIn, bool setTotal, std::string* usedLocal) {
    std::string err;
    auto src = remoteOpen(uri, err);
    if (!src) {
        // (The reason may name the file: neither for a private download.)
        if (dlPrivate(uri)) {
            PIXELVR_LOG_WARN("Download: cannot open (private)");
        } else {
            PIXELVR_LOG_WARN("Download: cannot open %s: %s", uri.c_str(), err.c_str());
        }
        if (isRemoteLoginError(err)) dlLoginFailed_.store(true);
        return false;
    }
    std::string local = localIn;
    if (setTotal && isHttpUrl(uri)) {
        // The server's own name (Content-Disposition) beats whatever the page guessed;
        // switch before anything is written. poll() applies it to the job on the main thread.
        const std::string suggested = src->fileName();
        if (!suggested.empty() && suggested != fs::path(local).filename().string()) {
            local = (fs::path(local).parent_path() / suggested).string();
            std::lock_guard<std::mutex> lk(dlRenameMutex_);
            pendingRenameName_ = suggested;
            pendingRenameLocal_ = local;
            hasRename_ = true;
        }
    }
    if (usedLocal) *usedLocal = local;
    const int64_t size = src->size();
    if (setTotal) dlTotal_.store(size);
    std::error_code ec;
    fs::create_directories(fs::path(local).parent_path(), ec);
    if (size > 0 && fs::is_regular_file(local, ec) && static_cast<int64_t>(fs::file_size(local, ec)) == size) {
        return true;  // a previous complete download
    }
    const std::string part = local + ".part";
    FILE* out = std::fopen(part.c_str(), "wb");
    if (out == nullptr) {
        PIXELVR_LOG_WARN("Download: cannot write %s", dlPrivate(uri) ? "(private)" : VaultManager::forLog(part).c_str());
        return false;
    }
    std::vector<uint8_t> buf(1u << 20);
    int64_t got = 0;
    bool ok = true;
    for (;;) {
        while (!dlAbort_.load() && dlPaused_.load()) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (dlAbort_.load()) {
            ok = false;
            break;
        }
        const int n = src->read(buf.data(), static_cast<int>(buf.size()));
        if (n < 0) {
            PIXELVR_LOG_WARN("Download: read error at %lld of %s", static_cast<long long>(got),
                             dlPrivate(uri) ? "(private)" : uri.c_str());
            ok = false;
            break;
        }
        if (n == 0) break;
        if (std::fwrite(buf.data(), 1, static_cast<std::size_t>(n), out) != static_cast<std::size_t>(n)) {
            ok = false;
            break;
        }
        got += n;
    }
    // On disk before it counts as done (an SD card pulled right after "Downloaded" keeps the file).
    if (ok) ok = std::fflush(out) == 0 && ::fsync(fileno(out)) == 0;
    ok = (std::fclose(out) == 0) && ok && (size < 0 || got == size);
    if (ok) {
        fs::rename(part, local, ec);
        ok = !ec;
        if (ok) filecopy::syncDir(fs::path(local).parent_path().string());
    }
    if (!ok) fs::remove(part, ec);
    return ok;
}

void Browser::downloadRemoteFile(const std::string& uri, const std::string& local, const std::string& name) {
    std::string used = local;
    const bool ok = copyRemoteFile(uri, local, true, &used);
    if (dlStopped_.load()) {
        std::error_code ec;
        fs::remove(used + ".part", ec);
    }
    dlOk_.store(ok);
    dlDone_.store(true);
    dlActive_.store(false);
    PIXELVR_LOG_INFO("Download %s: ok=%d", dlPrivate(uri) ? "(private)" : (name + " -> " + used).c_str(), ok ? 1 : 0);
}

void Browser::downloadRemoteFolder(const std::string& uri, const std::string& destDir) {
    long long got = 0;
    bool ok = true;
    std::function<long long(const std::string&)> sumSizes = [&](const std::string& src) -> long long {
        std::vector<RemoteEntry> list;
        std::string err;
        if (!remoteList(src, list, err)) return 0;
        long long total = 0;
        for (const RemoteEntry& e : list) {
            if (e.name.empty() || dlAbort_.load()) continue;
            total += e.dir ? sumSizes(src + (src.back() == '/' ? "" : "/") + e.name) : static_cast<long long>(e.size);
        }
        return total;
    };
    dlTotal_.store(sumSizes(uri));
    std::function<void(const std::string&, const std::string&)> copyDir = [&](const std::string& src, const std::string& dst) {
        std::error_code ec;
        fs::create_directories(dst, ec);
        std::vector<RemoteEntry> list;
        std::string err;
        if (!remoteList(src, list, err)) {
            if (dlPrivate(uri)) {
                PIXELVR_LOG_WARN("Download: cannot list (private)");
            } else {
                PIXELVR_LOG_WARN("Download: cannot list %s: %s", src.c_str(), err.c_str());
            }
            if (isRemoteLoginError(err)) dlLoginFailed_.store(true);
            ok = false;
            return;
        }
        for (const RemoteEntry& e : list) {
            if (e.name.empty() || e.name == "." || e.name == "..") continue;
            const std::string s = src + (src.back() == '/' ? "" : "/") + e.name;
            const std::string t = dst + "/" + e.name;
            if (e.dir) {
                copyDir(s, t);
            } else {
                {
                    std::lock_guard<std::mutex> lk(dlCurMutex_);
                    dlCurFile_ = t + ".part";  // live progress in poll()
                }
                const bool fileOk = copyRemoteFile(s, t, false);
                {
                    std::lock_guard<std::mutex> lk(dlCurMutex_);
                    dlCurFile_.clear();
                }
                if (!fileOk) {
                    ok = false;
                } else {
                    got += static_cast<long long>(e.size);
                    dlBaseBytes_.store(got);
                    dlBytes_.store(got);
                }
            }
            if (!ok || dlAbort_.load()) return;
        }
    };
    copyDir(uri, destDir);
    if (dlAbort_.load()) {
        std::error_code rec;
        if (!dlDestExisted_.load() && !dlCardGone_.load()) fs::remove_all(destDir, rec);  // no partial folder
        dlStopped_.store(true);
        dlOk_.store(false);
    } else {
        dlOk_.store(ok);
    }
    dlDone_.store(true);
    dlActive_.store(false);
    PIXELVR_LOG_INFO("Download folder done: %lld bytes (ok=%d)", got, ok ? 1 : 0);
}

void Browser::togglePause() {
    const bool p = !dlPaused_.load();
    dlPaused_.store(p);
    const int pid = dlPid_.load();
    if (pid > 0) kill(pid, p ? SIGSTOP : SIGCONT);
}

void Browser::stopDownload() {
    dlAbort_.store(true);
    dlStopped_.store(true);
    const int pid = dlPid_.load();
    if (pid > 0) kill(pid, SIGKILL);
}

void Browser::settleIntoVault(DownloadJob& j) {
    VaultManager& vm = VaultManager::instance();
    // Only a download started from a vault share is written into a vault download folder
    // (".incoming"). It is encrypted straight from there into the vault (the background pass), so no
    // plain copy is ever in the vault itself, and that holds even when its share has left the vault
    // since (it started as a vault download). (Also from an older place of the download folder: into
    // the vault where it is now.)
    if (!j.intoVault || fs::path(j.localPath).parent_path().filename() != ".incoming") return;
    vm.settleDownload(j.localPath);
    // The list's "open" goes to the vault folder (the encrypted file is there under its real name).
    j.localPath = vm.vaultDir() + "/";
    j.isDir = true;
}

void Browser::clearFinished() {
    std::vector<DownloadJob> keep;
    keep.reserve(dlJobs_.size());
    for (const DownloadJob& j : dlJobs_) {
        if (j.state == 0 || j.state == 4) {
            keep.push_back(j);  // keep the in-flight and queued jobs
            continue;
        }
        // Remove partial files from stopped/failed single-file downloads that wrote there themselves
        // (only the .part: never an older complete file of the same name; HTTP/SFTP clean up theirs).
        if (j.state != 1 && !j.isDir && j.wrote && !j.localPath.empty()) {
            std::error_code ec;
            std::filesystem::remove(j.localPath + ".part", ec);
        }
    }
    dlJobs_.swap(keep);
    dlActiveJob_ = -1;
    for (std::size_t i = 0; i < dlJobs_.size(); ++i) {
        if (dlJobs_[i].state == 0) dlActiveJob_ = static_cast<int>(i);
    }
    status_ = "Cleared finished downloads";
}

void Browser::removeJob(int index) {
    if (index < 0 || index >= static_cast<int>(dlJobs_.size())) return;
    const DownloadJob j = dlJobs_[static_cast<std::size_t>(index)];
    if (j.state == 0) return;  // the active job is stopped, not removed
    if (j.state == 4) {
        cancelQueued(j.uri);
        return;
    }
    if (j.state != 1 && !j.isDir && j.wrote && !j.localPath.empty()) {
        std::error_code ec;
        std::filesystem::remove(j.localPath + ".part", ec);
    }
    dlJobs_.erase(dlJobs_.begin() + index);
    dlActiveJob_ = -1;
    for (std::size_t i = 0; i < dlJobs_.size(); ++i) {
        if (dlJobs_[i].state == 0) dlActiveJob_ = static_cast<int>(i);
    }
    status_ = "Removed " + j.name;
}

void Browser::cancelQueued(const std::string& uri) {
    for (auto it = dlQueue_.begin(); it != dlQueue_.end(); ++it) {
        if (it->uri == uri) {
            dlQueue_.erase(it);
            break;
        }
    }
    for (auto it = dlJobs_.begin(); it != dlJobs_.end(); ++it) {
        if (it->uri == uri && it->state == 4) {
            dlJobs_.erase(it);
            break;
        }
    }
    dlActiveJob_ = -1;  // indexes moved
    for (std::size_t i = 0; i < dlJobs_.size(); ++i) {
        if (dlJobs_[i].state == 0) dlActiveJob_ = static_cast<int>(i);
    }
    status_ = "Removed from download queue";
}

void Browser::startNextQueued() {
    if (dlActive_.load()) return;
    DownloadQueueItem item;
    for (;;) {
        if (dlQueue_.empty()) return;
        item = dlQueue_.front();
        dlQueue_.pop_front();
        auto failQueued = [&](const std::string& why) {
            for (DownloadJob& j : dlJobs_) {
                if (j.uri == item.uri && j.state == 4) {
                    j.state = 2;
                    j.error = why;
                }
            }
        };
        // Its share moved into / out of the vault since it was queued (reconcileVaultDownloads
        // normally re-targets it then): where it belongs now.
        if ((isRemotePath(item.uri) && SmbCredentialsManager::isVaultShareUri(item.uri)) != item.intoVault) {
            std::string target;
            bool iv = false;
            if (const int bad = destinationFor(item.uri, fs::path(item.destDir).filename().string(), target, iv); bad != 0) {
                failQueued(destError(bad));
                continue;
            }
            item.destDir = target;
            item.intoVault = iv;
            for (DownloadJob& j : dlJobs_) {
                if (j.uri == item.uri && j.state == 4) {
                    j.localPath = target;
                    j.intoVault = iv;
                }
            }
        }
        if (!removableMissing(item.destDir, true)) break;
        failQueued("SD card removed");  // its drive is gone: fail it, try the next one
    }

    dlName_ = item.name;
    PIXELVR_LOG_INFO("Starting next queued download: %s",
                     item.intoVault ? "(private)" : VaultManager::forLog(item.uri, item.name + " (" + item.uri + ")").c_str());
    if (item.isDir) {
        startDownloadFolder(item.uri, item.destDir, item.intoVault);
    } else {
        startDownloadFile(item.uri, item.destDir, item.intoVault);
    }
}

bool Browser::isDownloadTarget(const std::string& path) const {
    auto under = [](const std::string& p, const std::string& dir) {
        return !dir.empty() && (p == dir || (p.size() > dir.size() && p.compare(0, dir.size(), dir) == 0 && p[dir.size()] == '/') ||
                                (dir.size() > p.size() && dir.compare(0, p.size(), p) == 0 && dir[p.size()] == '/'));
    };
    for (const DownloadJob& j : dlJobs_) {
        if ((j.state == 0 || j.state == 4) && under(path, j.localPath)) return true;  // also a folder holding it
    }
    return false;
}

bool Browser::retryJob(int index) {
    if (index < 0 || index >= static_cast<int>(dlJobs_.size())) return false;
    DownloadJob& j = dlJobs_[static_cast<std::size_t>(index)];
    if (j.state != 2 && j.state != 3) return false;
    for (const DownloadJob& o : dlJobs_) {
        if (&o != &j && o.uri == j.uri && (o.state == 0 || o.state == 4)) {
            status_ = "Already downloading or in queue: " + j.name;
            return false;
        }
    }
    if (j.intoVault || (isRemotePath(j.uri) && SmbCredentialsManager::isVaultShareUri(j.uri))) {
        // From a vault share: into the vault's download folder where the vault is now (it may have
        // moved since), never while it is moving. One that started as a vault download but whose
        // share is a normal one now goes into the download folder instead.
        std::string target;
        bool iv = false;
        if (const int bad = destinationFor(j.uri, fs::path(j.localPath).filename().string(), target, iv); bad != 0) {
            status_ = destStatus(bad);
            return false;
        }
        j.localPath = target;
        j.intoVault = iv;
    }
    if (removableMissing(j.localPath, true)) {
        status_ = "Insert the SD card again to retry";
        return false;
    }
    j.state = 4;
    j.error.clear();
    j.bytes = 0;
    dlQueue_.push_back({j.uri, j.name, j.localPath, j.isDir, j.intoVault});
    if (!dlActive_.load() && !dlDone_.load()) startNextQueued();
    return true;
}

void Browser::measureFolder(const std::string& uri) {
#ifdef PIXELVR_HAVE_SMB
    if (measuring_.load()) return;
    if (measureThread_.joinable()) measureThread_.join();
    measuredSize_.store(0);
    // A login known to be missing (not saved) or refused: nothing is sent to measure the folder
    // (the download itself then says that it needs a login). Only SMB folders are measured here:
    // libsmbclient cannot read the others.
    if (uri.rfind("smb://", 0) != 0 || SmbCredentialsManager::instance().loginBlocked(uri)) {
        measuring_.store(false);
        return;
    }
    measuring_.store(true);
    measureThread_ = std::thread([this, uri]() {
        long long total = 0;
        SmbCredentialsManager& creds = SmbCredentialsManager::instance();
        bool stop = false;  // the folder refused the login: no further folders are tried
        std::function<void(const std::string&)> sum = [&](const std::string& src) {
            if (stop) return;
            int d = -1;
            int e = 0;
            {
                std::lock_guard<std::mutex> smb(smbGlobalLock());
                errno = 0;
                d = smbc_opendir(src.c_str());
                e = d < 0 ? errno : 0;
            }
            if (d < 0) {
                // Remembered as refused unless this login already listed a folder of the share
                // (then only this folder may not be opened, and the others are still summed).
                if (SmbCredentialsManager::isLoginErrno(e) && creds.noteLoginRefused(src)) stop = true;
                return;
            }
            creds.noteLoginWorked(src);
            for (;;) {
                std::string nm;
                int typ = 0;
                {
                    std::lock_guard<std::mutex> smb(smbGlobalLock());
                    struct smbc_dirent* de = smbc_readdir(d);
                    if (de == nullptr) break;
                    nm = de->name;
                    typ = de->smbc_type;
                }
                if (nm.empty() || nm == "." || nm == "..") continue;
                std::string s = src;
                if (!s.empty() && s.back() != '/') s += '/';
                s += nm;
                if (typ == SMBC_DIR) {
                    sum(s);
                } else {
                    struct stat st{};
                    {
                        std::lock_guard<std::mutex> smb(smbGlobalLock());
                        smbc_stat(s.c_str(), &st);
                    }
                    total += static_cast<long long>(st.st_size);
                    measuredSize_.store(total);  // live-updating estimate
                }
            }
            {
                std::lock_guard<std::mutex> smb(smbGlobalLock());
                smbc_closedir(d);
            }
        };
        sum(uri);
        measuredSize_.store(total);
        measuring_.store(false);
    });
#else
    (void)uri;
#endif
}

void Browser::applyPendingRename() {
    std::string name, local;
    {
        std::lock_guard<std::mutex> lk(dlRenameMutex_);
        if (!hasRename_) return;
        name = pendingRenameName_;
        local = pendingRenameLocal_;
        hasRename_ = false;
    }
    dlName_ = name;
    dlLocalPath_ = local + ".part";  // poll() measures progress from this file
    if (dlActiveJob_ >= 0 && dlActiveJob_ < static_cast<int>(dlJobs_.size())) {
        dlJobs_[static_cast<std::size_t>(dlActiveJob_)].name = name;
        dlJobs_[static_cast<std::size_t>(dlActiveJob_)].localPath = local;
    }
}

Browser::Result Browser::poll(std::string& playPath) {
    applyPendingRename();
    // Publish a finished async directory scan (SMB discovery / large dirs).
    if (scanReady_.exchange(false)) {
        if (scanThread_.joinable()) scanThread_.join();
        // If the user switched to another directory while this scan was busy,
        // discard the stale result and immediately start the pending scan.
        if (hasPendingScan_) {
            hasPendingScan_ = false;
            const int savedFilter = activeFilter_;
            beginScan(pendingScanLoc_, pendingScanSmb_,
                      pendingScanRestore_, pendingScanChildPath_);
            activeFilter_ = savedFilter;
        } else if (!VaultManager::instance().isUnlocked() && isRemotePath(scanResult_.location) &&
                   SmbCredentialsManager::isVaultShareUri(scanResult_.location)) {
            // A folder of a vault share that was still being read when the vault locked: never shown.
            {
                std::lock_guard<std::mutex> lk(scanMutex_);
                scanResult_ = ScanResult{};
            }
            beginScan("smb://", true);
            return Result::None;
        } else {
            std::lock_guard<std::mutex> lk(scanMutex_);
            if (!VaultManager::instance().isUnlocked()) {
                // (A list read while the vault was unlocked, e.g. the network list with its vault shares.)
                auto& es = scanResult_.entries;
                es.erase(std::remove_if(es.begin(), es.end(),
                                        [](const Entry& e) {
                                            return isRemotePath(e.path) && SmbCredentialsManager::isVaultShareUri(e.path);
                                        }),
                         es.end());
            }
            entries_ = std::move(scanResult_.entries);
            title_ = std::move(scanResult_.title);
            status_ = std::move(scanResult_.status);
            location_ = scanResult_.location;
            inSmb_ = scanResult_.inSmb;
            lastLocation_ = location_;  // remember for the next open()
            lastInSmb_ = inSmb_;
            ++scanSerial_;
            lastScanOk_ = status_.empty() && scanResult_.login == 0;
            // A folder that wants a login: the app asks for it (not after leaving such a folder, so
            // a Cancel never lands in a second question).
            loginNeed_ = {};
            if (scanResult_.login != 0 && !quietLogin_) loginNeed_ = {location_, scanResult_.login == 2};
            quietLogin_ = false;

            const int n = static_cast<int>(entries_.size());
            if (restoreViewOnScan_ && n > 0) {
                auto it = folderHistory_.find(location_);
                if (it != folderHistory_.end()) {
                    const FolderViewState& st = it->second;
                    int foundIdx = -1;
                    if (!st.selectedPath.empty()) {
                        for (int i = 0; i < n; ++i) {
                            if (entries_[static_cast<std::size_t>(i)].path == st.selectedPath) {
                                foundIdx = i;
                                break;
                            }
                        }
                    }
                    if (foundIdx < 0 && !restoreChildPath_.empty()) {
                        for (int i = 0; i < n; ++i) {
                            if (entries_[static_cast<std::size_t>(i)].path == restoreChildPath_) {
                                foundIdx = i;
                                break;
                            }
                        }
                    }
                    selected_ = (foundIdx >= 0) ? foundIdx : std::clamp(st.selected, 0, n - 1);
                    first_ = std::clamp(st.first, 0, n - 1);
                    pendingRestoreFirst_ = first_;
                    hasPendingRestoreFirst_ = true;
                } else {
                    int foundIdx = 0;
                    if (!restoreChildPath_.empty()) {
                        for (int i = 0; i < n; ++i) {
                            if (entries_[static_cast<std::size_t>(i)].path == restoreChildPath_) {
                                foundIdx = i;
                                break;
                            }
                        }
                    }
                    selected_ = foundIdx;
                    first_ = 0;
                    pendingRestoreFirst_ = 0;
                    hasPendingRestoreFirst_ = true;
                }
            } else {
                selected_ = 0;
                first_ = 0;
                pendingRestoreFirst_ = 0;
                hasPendingRestoreFirst_ = true;
            }
            restoreViewOnScan_ = false;
            restoreChildPath_.clear();
        }
    }
    if (dlActive_.load()) {
        // Progress: a single file from its own size; a folder from completed
        // files plus the in-flight file.
        if (!dlLocalPath_.empty()) {
            std::error_code sec;
            if (std::filesystem::is_regular_file(dlLocalPath_, sec)) {
                const auto sz = std::filesystem::file_size(dlLocalPath_, sec);
                if (!sec) dlBytes_.store(static_cast<long long>(sz));
            } else {
                long long cur = 0;
                {
                    std::lock_guard<std::mutex> lk(dlCurMutex_);
                    if (!dlCurFile_.empty()) {
                        std::error_code cec;
                        cur = static_cast<long long>(
                            std::filesystem::file_size(dlCurFile_, cec));
                    }
                }
                dlBytes_.store(dlBaseBytes_.load() + cur);
            }
        }
        const long long b = dlBytes_.load();
        const long long t = dlTotal_.load();
        const auto now = std::chrono::steady_clock::now();
        if (speedTime_.time_since_epoch().count() == 0) {
            speedTime_ = now;
            speedBytes_ = b;
        }
        const double dt = std::chrono::duration<double>(now - speedTime_).count();
        if (dt > 0.25) {
            const double inst = static_cast<double>(b - speedBytes_) / dt;
            dlSpeed_ = (dlSpeed_ <= 0.0) ? inst : dlSpeed_ * 0.6 + inst * 0.4;
            speedBytes_ = b;
            speedTime_ = now;
        }
        status_ = downloadText();
        if (dlActiveJob_ >= 0 && dlActiveJob_ < static_cast<int>(dlJobs_.size())) {
            dlJobs_[dlActiveJob_].bytes = b;
            dlJobs_[dlActiveJob_].total = t;
        }
        // The SD card (or USB drive) under the target pulled: stop, reported as "SD card removed".
        if (now >= dlNextDriveCheck_) {
            dlNextDriveCheck_ = now + std::chrono::milliseconds(500);
            if (!dlCardGone_.load() && !dlLocalPath_.empty() && removableMissing(dlLocalPath_)) {
                PIXELVR_LOG_WARN("Download: the drive under %s was removed",
                                 dlQuiet_.load() ? "(private)" : VaultManager::forLog(dlLocalPath_).c_str());
                dlCardGone_.store(true);
                dlAbort_.store(true);
                const int pid = dlPid_.load();
                if (pid > 0) kill(pid, SIGKILL);
            }
        }
        return Result::None;
    }
    applyPendingRename();  // a download that finished within this very frame
    if (dlDone_.exchange(false)) {
        if (dlThread_.joinable()) dlThread_.join();
        bool keptAfterSwitch = false;  // stopped by a share's vault switch, but it had just finished
        if (dlActiveJob_ >= 0 && dlActiveJob_ < static_cast<int>(dlJobs_.size())) {
            DownloadJob& j = dlJobs_[dlActiveJob_];
            j.bytes = dlBytes_.load();
            j.total = dlTotal_.load();
            const bool gone = dlCardGone_.load() || (!dlOk_.load() && !dlStopped_.load() && removableMissing(j.localPath, true));
            j.state = gone ? 2 : dlStopped_.load() ? 3 : (dlOk_.load() ? 1 : 2);
            j.error = gone ? "SD card removed" : std::string();
            if (!gone && !dlOk_.load() && !dlStopped_.load() && dlLoginFailed_.load()) {
                j.error = "Needs a login: open the share in Files to sign in";
            }
            if (j.state == 1) settleIntoVault(j);  // (only a download started from a vault share)
            std::error_code xec;
            if (!dlVaultSwitched_ || dlPrivateDropped_ || gone) {
                // (not stopped by a share's vault switch)
            } else if (j.state == 3 && dlOk_.load() && fs::exists(j.localPath, xec)) {
                // It had just finished when it was stopped: kept as done (a download started from a
                // vault share is still encrypted into the vault).
                j.state = 1;
                j.error.clear();
                settleIntoVault(j);
                keptAfterSwitch = true;
            } else if (j.state != 1) {
                // Stopped because its share moved into / out of the vault: what it wrote goes (its
                // thread removed the partial file or new folder; a .part it left goes here), and a
                // retry goes where the share belongs now. (Not wipeVaultIncoming: unlocked, that
                // removes the final name too, which may be an earlier download still being encrypted.)
                j.state = 3;
                j.error = "Stopped: the share's vault setting changed";
                if (!dlDroppedPath_.empty()) fs::remove(dlDroppedPath_ + ".part", xec);
                std::string target;
                bool iv = false;
                if (destinationFor(j.uri, fs::path(j.localPath).filename().string(), target, iv) == 0) {
                    j.localPath = target;
                    j.intoVault = iv;
                }
            }
        }
        dlActiveJob_ = -1;
        const bool vaultSwitched = dlVaultSwitched_;
        dlVaultSwitched_ = false;
        if (dlPrivateDropped_) {
            dlPrivateDropped_ = false;  // stopped by locking the vault: nothing is said about it
            // Its thread has ended: nothing writes into the vault's download folder any more.
            wipeVaultIncoming(dlDroppedPath_);
            dlDroppedPath_.clear();
        } else if (vaultSwitched && !dlCardGone_.load()) {
            status_ = keptAfterSwitch ? "Downloaded " + dlName_
                                      : std::string("Download stopped: the share's vault setting changed");  // (names no file)
            dlDroppedPath_.clear();
        } else if (dlCardGone_.load()) {
            status_ = "Download failed: SD card removed";
            // Queued downloads to that drive fail the same way (never sent elsewhere).
            for (DownloadJob& q : dlJobs_) {
                if (q.state == 4 && removableMissing(q.localPath)) {
                    q.state = 2;
                    q.error = "SD card removed";
                }
            }
            dlQueue_.erase(std::remove_if(dlQueue_.begin(), dlQueue_.end(),
                                          [](const DownloadQueueItem& it) { return removableMissing(it.destDir); }),
                           dlQueue_.end());
        } else if (dlStopped_.load()) {
            status_ = "Stopped " + dlName_;
        } else if (dlOk_.load()) {
            // Completing a download must NOT interrupt current playback (it used
            // to auto-play, and for a folder that fed a directory to the media
            // engine). Finished items are played from the downloads window.
            status_ = "Downloaded " + dlName_;
        } else if (dlLoginFailed_.load()) {
            status_ = "Download failed: the share needs a login (open it in Files to sign in)";
        } else {
            status_ = "Download failed: " + dlName_;
        }
        startNextQueued();
    }
    return Result::None;
}

std::string Browser::downloadText() const {
    const long long b = dlBytes_.load();
    const long long t = dlTotal_.load();
    std::string n = dlName_;
    if (n.size() > 24) n = n.substr(0, 23) + "~";
    std::string s = n + "  ";
    char buf[160];
    const double mb = static_cast<double>(b) / (1024.0 * 1024.0);
    if (t > 0) {
        std::snprintf(buf, sizeof(buf), "%lld%%  %.0f/%.0f MB", b * 100 / t, mb,
                      static_cast<double>(t) / (1024.0 * 1024.0));
        s += buf;
    } else {
        std::snprintf(buf, sizeof(buf), "%.0f MB", mb);
        s += buf;
    }
    if (dlSpeed_ > 1.0) {
        std::snprintf(buf, sizeof(buf), "  %.1f MB/s",
                      dlSpeed_ / (1024.0 * 1024.0));
        s += buf;
    }
    if (t > 0 && !dlPaused_.load() && dlSpeed_ >= 100 * 1024.0) {
        double eta = static_cast<double>(t - b) / dlSpeed_;
        if (eta < 0.0) eta = 0.0;
        const int secs = static_cast<int>(eta);
        std::snprintf(buf, sizeof(buf), "  ETA %d:%02d", secs / 60, secs % 60);
        s += buf;
    } else if (t > 0) {
        s += "  ETA --:--";
    }
    return s;
}

} // namespace pixelvr

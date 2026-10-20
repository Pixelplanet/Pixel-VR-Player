#include "ui/browser.hpp"
#include "app/vault_manager.hpp"
#include "net/smb_credentials.hpp"

#include "util/logging.hpp"
#include "util/smb_lock.hpp"

#include <algorithm>
#include <cctype>
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

extern char** environ;
#endif

namespace pixelvr {
namespace fs = std::filesystem;

namespace {

bool is_video(const std::string& name) {
    static const char* const kExts[] = {".mp4", ".mkv",  ".mov", ".webm",
                                        ".m4v", ".avi",  ".ts",  ".m2ts",
                                        ".wmv", ".flv",  ".mpg", ".m3u8"};
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const char* e : kExts) {
        const std::size_t n = std::char_traits<char>::length(e);
        if (lower.size() >= n && lower.compare(lower.size() - n, n, e) == 0) {
            return true;
        }
    }
    return false;
}

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
    if (uri.size() <= 6) return "";  // "smb://" -> leave the network
    const std::string rest = uri.substr(6);
    const auto slash = rest.find_last_of('/');
    if (slash == std::string::npos) return "smb://";  // server -> discovery
    return "smb://" + rest.substr(0, slash);
}

void sort_and_append(std::vector<Browser::Entry>& out,
                     std::vector<Browser::Entry>& dirs,
                     std::vector<Browser::Entry>& files) {
    auto by_media_then_label = [](const Browser::Entry& a, const Browser::Entry& b) {
        const bool aHasMedia = a.isVideo || !a.previewPath.empty();
        const bool bHasMedia = b.isVideo || !b.previewPath.empty();
        if (aHasMedia != bHasMedia) return aHasMedia > bHasMedia;
        return a.label < b.label;
    };
    std::sort(dirs.begin(), dirs.end(), by_media_then_label);
    std::sort(files.begin(), files.end(), by_media_then_label);
    out.insert(out.end(), dirs.begin(), dirs.end());
    out.insert(out.end(), files.begin(), files.end());
}

} // namespace

Browser::Browser() {
    const char* home = std::getenv("HOME");
    homeDir_ = (home != nullptr) ? home : "/home/steamos";
    downloadDir_ = homeDir_ + "/media";
    quickPath_ = homeDir_ + "/Videos";
    loadBookmarks();
}

void Browser::open() {
    open_ = true;
    browseOpen_.store(true);
    status_.clear();
    // Resume where the user last was this session (local folder or SMB share).
    if (lastInSmb_ && !lastLocation_.empty()) {
        beginScan(lastLocation_, true);
        return;
    }
    std::string start = homeDir_;
    if (!lastInSmb_ && !lastLocation_.empty()) {
        std::error_code ec;
        if (fs::is_directory(lastLocation_, ec)) {
            start = lastLocation_;
        }
    }
    beginScan(start, false);
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

void Browser::scanLocalInto(const std::string& dir, ScanResult& out) {
    out.inSmb = false;
    out.location = dir;
    if (VaultManager::instance().isVaultPath(dir)) {
        out.title = "Vault (Private)";
    } else {
        out.title = dir;
    }
    out.entries.clear();

    if (dir == homeDir_) {
        out.entries.push_back({"[ Network (SMB) ]", "", false, false, false, true});
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
                    ++videoCount;
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
                            ++videoCount;
                        }
                    }
                }
            }
            std::string detail = (videoCount > 0)
                ? (std::to_string(videoCount) + (videoCount == 1 ? " video" : " videos") +
                   (bestPreviewSize > 0 ? "  \xB7  " + format_size(bestPreviewSize) : ""))
                : "Folder";
            dirs.push_back({name + "/", p.string(), true, false, false, false, detail,
                            bestPreviewSize > 0 ? bestPreviewSize : 0, preview});
        } else {
            const bool isVid = is_video(name);
            if (dir == homeDir_ && !isVid) continue;  // hide log/tarball files in home root
            std::error_code sec;
            const long long sz = static_cast<long long>(fs::file_size(p, sec));
            files.push_back({name, p.string(), false, isVid, false, false,
                             format_size(sec ? 0 : sz), sec ? 0 : sz, isVid ? p.string() : ""});
        }
    }
    sort_and_append(out.entries, dirs, files);
}

void Browser::scanSmbInto(const std::string& uri, ScanResult& out) {
    out.inSmb = true;
    out.location = uri;
    out.title = uri;
    out.entries.clear();
    out.entries.push_back({".. (up)", "", false, false, true, false});

#ifdef PIXELVR_HAVE_SMB
    std::vector<Entry> dirs, files;
    bool browsed = false;
    {
        // libsmbclient is not thread-safe: hold the global SMB lock across the
        // whole enumeration so it can't race the media stream's prefetch thread
        // (concurrent use aborts inside Samba name resolution).
        std::lock_guard<std::mutex> smb(smbGlobalLock());
        if (!ensure_smb()) {
            out.status = "SMB unavailable";
            return;
        }
        const int dh = smbc_opendir(uri.c_str());
        if (dh >= 0) {
            browsed = true;
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
                        if (smbc_stat(child.c_str(), &st) == 0) {
                            sz = static_cast<long long>(st.st_size);
                        }
                        files.push_back({name, child, false, is_video(name), false,
                                         false, format_size(sz), sz});
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
        dirs.push_back({"+ Add SMB Share", "smb://__add_share__", true, false, false, false, "Configure New Network Share"});
        seen.insert("smb://__add_share__");

        // Prepend all user-configured manual SMB shares so private/unadvertised shares always appear
        const auto configuredShares = SmbCredentialsManager::instance().shares();
        for (const auto& s : configuredShares) {
            if (s.host.empty()) continue;
            std::string child = "smb://" + s.host;
            if (!s.share.empty()) {
                if (child.back() != '/') child += '/';
                child += s.share;
            }
            if (!seen.insert(child).second) continue;
            std::string label = s.name.empty() ? s.host : s.name;
            std::string detail = s.anonymous ? "Configured (Guest)" : "Configured (Protected)";
            if (!s.share.empty()) {
                dirs.push_back({label + "  (" + s.host + "/" + s.share + ")", child, true, false, false, false, detail});
            } else {
                dirs.push_back({label + "  (" + s.host + ")", child, true, false, false, false, detail});
            }
        }
        for (const auto& d : dirs) seen.insert(d.path);
        FILE* pipe = popen("timeout 4 avahi-browse -trp _smb._tcp 2>/dev/null", "r");
        if (pipe != nullptr) {
            char line[2048];
            while (std::fgets(line, sizeof(line), pipe) != nullptr) {
                if (line[0] != '=') continue;  // resolved records only
                std::vector<std::string> f;
                std::string cur;
                for (const char* c = line; *c != '\0'; ++c) {
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
            pclose(pipe);
        }
        browsed = browsed || !dirs.empty();
    }

    if (!browsed) {
        out.status = "Cannot browse " + uri;
        return;
    }
    sort_and_append(out.entries, dirs, files);
    out.status = std::to_string(out.entries.size() - 1) + " items";
#else
    out.status = "SMB support not built";
#endif
}

// Starts a background scan so SMB discovery (NetBIOS + mDNS, seconds) and large
// directories never freeze the render loop. poll() publishes the result.
void Browser::beginScan(const std::string& location, bool isSmb) {
    PIXELVR_LOG_INFO("beginScan '%s' smb=%d scanning=%d", location.c_str(),
                     isSmb ? 1 : 0, scanning_.load() ? 1 : 0);
    if (scanning_.load() && scanThread_.joinable()) {
        PIXELVR_LOG_INFO("beginScan DEFERRED (scan in progress)");
        // A scan is already running (possibly waiting on the SMB lock held by a
        // download). Defer this request rather than joining on the main thread,
        // which would freeze the UI until the download's read returns.
        pendingScanLoc_ = location;
        pendingScanSmb_ = isSmb;
        hasPendingScan_ = true;
        return;
    }
    if (scanThread_.joinable()) scanThread_.join();  // previous scan has finished
    scanReady_.store(false);
    scanning_.store(true);
    title_ = location;
    status_ = "Scanning...";
    entries_.clear();
    entries_.push_back({"Scanning...", "", false, false, false, false});
    selected_ = 0;
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
}

void Browser::openHome() {
    openPath(homeDir_);
}

void Browser::openVideos() {
    const std::string v = homeDir_ + "/Videos";
    std::error_code ec;
    if (fs::is_directory(v, ec)) {
        openPath(v);
    } else {
        openPath(homeDir_);
    }
}

void Browser::openDownloads() {
    std::error_code ec;
    if (!fs::is_directory(downloadDir_, ec)) {
        fs::create_directories(downloadDir_, ec);
    }
    openPath(downloadDir_);
}

void Browser::openDrives() {
    const std::string d = "/run/media";
    std::error_code ec;
    if (fs::is_directory(d, ec)) {
        openPath(d);
    } else {
        openPath("/");
    }
}

void Browser::openVault() {
    if (!VaultManager::instance().isUnlocked()) return;
    VaultManager::instance().ensureDirectories();
    openPath(VaultManager::instance().vaultDir());
}

void Browser::openBookmarks() {
    open_ = true;
    browseOpen_.store(true);
    status_.clear();
    beginScan("bookmarks://", false);
}

void Browser::addBookmark(const std::string& path, const std::string& label) {
    if (path.empty() || path == "bookmarks://" || path == "smb://__add_share__") return;
    for (auto& b : bookmarks_) {
        if (b.path == path) return;
    }
    std::string l = label;
    if (l.empty()) {
        if (path.rfind("smb://", 0) == 0) {
            std::string rest = path.substr(6);
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

    f << "[\n";
    for (std::size_t i = 0; i < bookmarks_.size(); ++i) {
        f << "  {\n";
        f << "    \"path\": \"" << bookmarks_[i].path << "\",\n";
        f << "    \"label\": \"" << bookmarks_[i].label << "\"\n";
        f << "  }" << (i + 1 < bookmarks_.size() ? ",\n" : "\n");
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

    if (bookmarks_.empty()) {
        out.status = "No bookmarks yet. Click [+ Bookmark] in any folder to add one.";
        return;
    }

    out.status = std::to_string(bookmarks_.size()) + (bookmarks_.size() == 1 ? " bookmark" : " bookmarks");

    for (const auto& bm : bookmarks_) {
        if (bm.path.empty()) continue;
        const bool isSmb = (bm.path.rfind("smb://", 0) == 0);
        std::error_code ec;
        const bool isDir = isSmb || fs::is_directory(bm.path, ec);
        std::string label = bm.label;
        if (label.empty()) {
            if (isSmb) {
                label = bm.path.substr(6);
                while (!label.empty() && label.back() == '/') label.pop_back();
            } else {
                fs::path p(bm.path);
                label = p.filename().string();
                if (label.empty()) label = bm.path;
            }
        }
        std::string detail = isSmb ? "SMB Share" : (isDir ? "Folder" : "Video File");
        std::string previewPath = isDir ? "" : bm.path;
        out.entries.push_back({label, bm.path, isDir, !isDir && is_video(bm.path), false, isSmb, detail, 0, previewPath});
    }
}

bool Browser::inVault() const {
    return VaultManager::instance().isVaultPath(location_);
}

bool Browser::moveFocusedToVault(std::string& outNewPath) {
    if (!VaultManager::instance().isUnlocked()) return false;
    const Entry* e = focused();
    if (e == nullptr || e->isBack || inSmb_) return false;
    std::string err = VaultManager::instance().moveFileToVault(e->path, outNewPath);
    if (!err.empty()) {
        status_ = "Move error: " + err;
        return false;
    }
    status_ = "Moved " + e->label + " to Vault";
    beginScan(location_, inSmb_);
    return true;
}

bool Browser::moveFocusedTo(const std::string& destDir, std::string& outNewPath) {
    const Entry* e = focused();
    if (e == nullptr || e->isBack || inSmb_) return false;
    std::error_code ec;
    if (!fs::exists(destDir, ec) || !fs::is_directory(destDir, ec)) {
        status_ = "Destination folder does not exist";
        return false;
    }
    fs::path src(e->path);
    const bool isDir = fs::is_directory(src, ec);
    std::string stem = src.stem().string();
    std::string ext = isDir ? "" : src.extension().string();
    std::string targetFilename = src.filename().string();
    fs::path dest = fs::path(destDir) / targetFilename;

    int counter = 1;
    while (fs::exists(dest, ec)) {
        targetFilename = isDir ? (stem + "_" + std::to_string(counter++))
                               : (stem + "_" + std::to_string(counter++) + ext);
        dest = fs::path(destDir) / targetFilename;
    }

    fs::rename(src, dest, ec);
    if (ec) {
        ec.clear();
        if (isDir) {
            fs::copy(src, dest, fs::copy_options::recursive, ec);
            if (ec) {
                status_ = "Move failed: " + ec.message();
                return false;
            }
            fs::remove_all(src, ec);
        } else {
            fs::copy_file(src, dest, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                status_ = "Move failed: " + ec.message();
                return false;
            }
            fs::remove(src, ec);
        }
    }
    outNewPath = dest.string();
    status_ = "Moved " + e->label + " to " + fs::path(destDir).filename().string();
    beginScan(location_, inSmb_);
    return true;
}

bool Browser::deleteFocused(std::string& deletedName) {
    const Entry* e = focused();
    if (e == nullptr || e->isBack || inSmb_) return false;
    std::error_code ec;
    deletedName = e->label;
    fs::path p(e->path);
    VaultManager::instance().cleanPublicThumbs(e->path);
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
    beginScan(location_, inSmb_);
    return true;
}

bool Browser::renameFocused(const std::string& newName) {
    const Entry* e = focused();
    if (e == nullptr || e->isBack || inSmb_ || newName.empty()) return false;
    std::error_code ec;
    fs::path src(e->path);
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
    beginScan(location_, inSmb_);
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
    if (location == "bookmarks://") {
        scanBookmarksInto(r);
    } else if (isSmb) {
        scanSmbInto(location, r);
    } else {
        scanLocalInto(location, r);
    }
    PIXELVR_LOG_INFO("scan done '%s': %zu entries (status=%s)", location.c_str(),
                     r.entries.size(), r.status.c_str());
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
        if (location_ == "bookmarks://") {
            beginScan(homeDir_, false);
            return Result::Navigated;
        }
        if (inSmb_) {
            const std::string parent = smb_parent(location_);
            if (parent.empty()) {
                beginScan(homeDir_, false);
            } else {
                beginScan(parent, true);
            }
        } else {
            if (VaultManager::instance().isVaultPath(location_)) {
                if (location_ == VaultManager::instance().vaultDir()) {
                    beginScan(homeDir_, false);
                } else {
                    const fs::path p(location_);
                    beginScan(p.has_parent_path() ? p.parent_path().string() : homeDir_, false);
                }
            } else {
                const fs::path p(location_);
                beginScan(p.has_parent_path() ? p.parent_path().string() : "/", false);
            }
        }
        return Result::Navigated;
    }
    if (e.path == "smb://__add_share__") {
        return Result::AddSmbShare;
    }
    if (e.isNetwork) {
        beginScan("smb://", true);
        return Result::Navigated;
    }
    if (e.isDir) {
        const bool isSmb = e.isNetwork || (e.path.rfind("smb://", 0) == 0);
        beginScan(e.path, isSmb);
        return Result::Navigated;
    }
    if (e.isVideo) {
        // Stream by default: local files and SMB shares both play in place. The
        // media engine opens smb:// URLs directly, so there is no need to copy
        // the file to the device first. Downloading is an explicit action
        // (downloadFocused), surfaced as the browser's context option.
        playPath = e.path;
        return Result::Play;
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
                         : "Downloads are for network (SMB) files";
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

    const std::string destDir =
        downloadDir_.empty() ? homeDir_ + "/media" : downloadDir_;
    const std::string targetPath = destDir + "/" + name;

    if (dlActive_.load()) {
        dlQueue_.push_back({uri, name, targetPath, isDir});
        dlJobs_.push_back({name, targetPath, uri, 4, 0, 0, isDir});
        status_ = "Added to download queue: " + name;
        PIXELVR_LOG_INFO("Added to download queue: %s (%s)", name.c_str(), uri.c_str());
        return;
    }

    dlName_ = name;
    if (isDir) {
        startDownloadFolder(uri, targetPath);
    } else {
        startDownloadFile(uri, targetPath);
    }
}

void Browser::startDownloadFile(const std::string& uri, const std::string& local) {
#ifdef PIXELVR_HAVE_SMB
    if (dlThread_.joinable()) dlThread_.join();
    dlDone_.store(false);
    dlOk_.store(false);
    dlBytes_.store(0);
    dlTotal_.store(0);
    dlLocalPath_.clear();
    dlSpeed_ = 0.0;
    speedBytes_ = 0;
    speedTime_ = {};
    dlStopped_.store(false);
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
    PIXELVR_LOG_INFO("Download file: %s -> %s", uri.c_str(), local.c_str());

    int existingIdx = -1;
    for (std::size_t i = 0; i < dlJobs_.size(); ++i) {
        if (dlJobs_[i].uri == uri && (dlJobs_[i].state == 4 || dlJobs_[i].state == 0)) {
            existingIdx = static_cast<int>(i);
            dlJobs_[i].state = 0;
            dlJobs_[i].name = dlName_;
            dlJobs_[i].localPath = local;
            break;
        }
    }
    if (existingIdx >= 0) {
        dlActiveJob_ = existingIdx;
    } else {
        dlJobs_.push_back({dlName_, local, uri, 0, 0, 0, false});
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
    dlLocalPath_ = local;
    // Copy with a curl child process: it opens its own SMB connection, so the
    // browser's libsmbclient is never blocked and browsing keeps working.
    dlThread_ = std::thread([this, uri, local, name]() {
        std::error_code ec;
        std::filesystem::create_directories(
            std::filesystem::path(local).parent_path(), ec);

        struct stat st{};
        long long total = 0;
        {
            std::lock_guard<std::mutex> smb(smbGlobalLock());
            if (smbc_stat(uri.c_str(), &st) == 0) {
                total = static_cast<long long>(st.st_size);
            }
        }
        dlTotal_.store(total);
        PIXELVR_LOG_INFO("Download: %s size=%lld bytes", name.c_str(), total);

        // Reuse a previous complete download instead of fetching again.
        std::error_code lec;
        if (total > 0 && std::filesystem::exists(local, lec) &&
            static_cast<long long>(std::filesystem::file_size(local, lec)) == total) {
            dlOk_.store(true);
            dlDone_.store(true);
            dlActive_.store(false);
            return;
        }

        const int pid = spawnCurl(uri, local);
        dlPid_.store(pid);
        if (pid <= 0) {
            PIXELVR_LOG_WARN("Download: spawn curl failed for %s", uri.c_str());
            dlDone_.store(true);
            dlActive_.store(false);
            return;
        }
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        dlPid_.store(-1);
        const bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
        if (dlStopped_.load()) {
            std::error_code rec;
            std::filesystem::remove(local, rec);  // no partial file left behind
        }
        dlOk_.store(ok);
        dlDone_.store(true);
        dlActive_.store(false);
        PIXELVR_LOG_INFO("Download %s: ok=%d -> %s", name.c_str(), ok ? 1 : 0,
                         local.c_str());
    });
#else
    (void)uri;
    (void)local;
    status_ = "SMB support not built";
#endif
}

// Spawns a child that copies `uri` (smb://) to `local`; returns its pid. Uses
// Samba's own smbclient (same stack as our libsmbclient) so guest access matches.
// NOTE: posix_spawn (not fork) — forking a multithreaded process can deadlock.
int Browser::spawnCurl(const std::string& uri, const std::string& local) {
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

    std::string authFile = SmbCredentialsManager::instance().getAuthFilePath(host, share);
    PIXELVR_LOG_INFO("DL spawn: smbclient %s %s -c '%s'", service.c_str(),
                     authFile.empty() ? "-N" : ("-A " + authFile).c_str(), cmd.c_str());

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
    pid_t pid = -1;
    const int rc = posix_spawn(&pid, "/usr/bin/smbclient", &fa, nullptr, argv,
                               environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) {
        PIXELVR_LOG_WARN("DL: posix_spawn failed rc=%d", rc);
        return -1;
    }
    return static_cast<int>(pid);
}

void Browser::startDownloadFolder(const std::string& uri,
                                  const std::string& destDir) {
#ifdef PIXELVR_HAVE_SMB
    if (dlThread_.joinable()) dlThread_.join();
    dlDone_.store(false);
    dlOk_.store(false);
    dlBytes_.store(0);
    dlTotal_.store(0);
    dlLocalPath_ = destDir;
    dlSpeed_ = 0.0;
    speedBytes_ = 0;
    speedTime_ = {};
    dlActive_.store(true);
    status_ = "Downloading folder " + dlName_ + " ...";
    PIXELVR_LOG_INFO("Download folder: %s -> %s", uri.c_str(), destDir.c_str());
    dlStopped_.store(false);
    dlPaused_.store(false);
    dlAbort_.store(false);
    int existingIdx = -1;
    for (std::size_t i = 0; i < dlJobs_.size(); ++i) {
        if (dlJobs_[i].uri == uri && (dlJobs_[i].state == 4 || dlJobs_[i].state == 0)) {
            existingIdx = static_cast<int>(i);
            dlJobs_[i].state = 0;
            dlJobs_[i].name = dlName_;
            dlJobs_[i].localPath = destDir;
            break;
        }
    }
    if (existingIdx >= 0) {
        dlActiveJob_ = existingIdx;
    } else {
        dlJobs_.push_back({dlName_, destDir, uri, 0, 0, 0, true});
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
    dlThread_ = std::thread([this, uri, destDir]() {
        std::error_code ec;
        long long got = 0;
        long long count = 0;
        bool ok = true;
        // Pre-walk the tree to total the size, so % progress and ETA can be shown.
        long long total = 0;
        std::function<void(const std::string&)> sumSizes =
            [&](const std::string& src) {
                int d = -1;
                {
                    std::lock_guard<std::mutex> smb(smbGlobalLock());
                    d = smbc_opendir(src.c_str());
                }
                if (d < 0) return;
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
                        sumSizes(s);
                    } else {
                        struct stat st{};
                        {
                            std::lock_guard<std::mutex> smb(smbGlobalLock());
                            smbc_stat(s.c_str(), &st);
                        }
                        total += static_cast<long long>(st.st_size);
                    }
                }
                {
                    std::lock_guard<std::mutex> smb(smbGlobalLock());
                    smbc_closedir(d);
                }
            };
        sumSizes(uri);
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
            {
                std::lock_guard<std::mutex> lk(dlCurMutex_);
                dlCurFile_ = dst;  // for live progress in poll()
            }
            // Each file is copied by its own smbclient child (own SMB connection).
            const int pid = spawnCurl(src, dst);
            if (pid <= 0) {
                ok = false;
                return;
            }
            dlPid_.store(pid);
            int status = 0;
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
            }
            dlPid_.store(-1);
            {
                std::lock_guard<std::mutex> lk(dlCurMutex_);
                dlCurFile_.clear();
            }
            if (!(WIFEXITED(status) && WEXITSTATUS(status) == 0)) {
                // Report partial bytes too, so progress is not lost on failure.
                std::error_code pec;
                got += static_cast<long long>(std::filesystem::file_size(dst, pec));
                dlBaseBytes_.store(got);
                ok = false;
                return;
            }
            std::error_code sec;
            got += static_cast<long long>(std::filesystem::file_size(dst, sec));
            dlBaseBytes_.store(got);
            dlBytes_.store(got);
            ++count;
        };
        copyDir = [&](const std::string& src, const std::string& dst) {
            std::filesystem::create_directories(dst, ec);
            int d = -1;
            {
                std::lock_guard<std::mutex> smb(smbGlobalLock());
                d = smbc_opendir(src.c_str());
            }
            if (d < 0) {
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
            std::filesystem::remove_all(destDir, rec);  // no partial folder
            dlStopped_.store(true);
            dlOk_.store(false);
            dlDone_.store(true);
            dlActive_.store(false);
            PIXELVR_LOG_INFO("Download folder stopped: %s", destDir.c_str());
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
    status_ = "SMB support not built";
#endif
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

void Browser::clearFinished() {
    std::vector<DownloadJob> keep;
    keep.reserve(dlJobs_.size());
    for (const DownloadJob& j : dlJobs_) {
        if (j.state == 0 || j.state == 4) {
            keep.push_back(j);  // keep the in-flight and queued jobs
            continue;
        }
        // Remove partial files from stopped/failed single-file downloads.
        if (j.state != 1 && !j.isDir && !j.localPath.empty()) {
            std::error_code ec;
            std::filesystem::remove(j.localPath, ec);
        }
    }
    dlJobs_.swap(keep);
    dlActiveJob_ = -1;
    for (std::size_t i = 0; i < dlJobs_.size(); ++i) {
        if (dlJobs_[i].state == 0) dlActiveJob_ = static_cast<int>(i);
    }
    status_ = "Cleared finished downloads";
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
    status_ = "Removed from download queue";
}

void Browser::startNextQueued() {
    if (dlActive_.load()) return;
    if (dlQueue_.empty()) return;

    DownloadQueueItem item = dlQueue_.front();
    dlQueue_.pop_front();

    dlName_ = item.name;
    PIXELVR_LOG_INFO("Starting next queued download: %s (%s)", item.name.c_str(), item.uri.c_str());
    if (item.isDir) {
        startDownloadFolder(item.uri, item.destDir);
    } else {
        startDownloadFile(item.uri, item.destDir);
    }
}

void Browser::measureFolder(const std::string& uri) {
#ifdef PIXELVR_HAVE_SMB
    if (measuring_.load()) return;
    if (measureThread_.joinable()) measureThread_.join();
    measuredSize_.store(0);
    measuring_.store(true);
    measureThread_ = std::thread([this, uri]() {
        long long total = 0;
        std::function<void(const std::string&)> sum = [&](const std::string& src) {
            int d = -1;
            {
                std::lock_guard<std::mutex> smb(smbGlobalLock());
                d = smbc_opendir(src.c_str());
            }
            if (d < 0) return;
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

Browser::Result Browser::poll(std::string& playPath) {
    // Publish a finished async directory scan (SMB discovery / large dirs).
    if (scanReady_.exchange(false)) {
        if (scanThread_.joinable()) scanThread_.join();
        {
            std::lock_guard<std::mutex> lk(scanMutex_);
            entries_ = std::move(scanResult_.entries);
            title_ = std::move(scanResult_.title);
            status_ = std::move(scanResult_.status);
            location_ = scanResult_.location;
            inSmb_ = scanResult_.inSmb;
            selected_ = 0;
            lastLocation_ = location_;  // remember for the next open()
            lastInSmb_ = inSmb_;
        }
        // Run a navigation the user requested while this scan was busy.
        if (hasPendingScan_) {
            hasPendingScan_ = false;
            beginScan(pendingScanLoc_, pendingScanSmb_);
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
        return Result::None;
    }
    if (dlDone_.exchange(false)) {
        if (dlThread_.joinable()) dlThread_.join();
        if (dlActiveJob_ >= 0 && dlActiveJob_ < static_cast<int>(dlJobs_.size())) {
            DownloadJob& j = dlJobs_[dlActiveJob_];
            j.bytes = dlBytes_.load();
            j.total = dlTotal_.load();
            j.state = dlStopped_.load() ? 3 : (dlOk_.load() ? 1 : 2);
        }
        dlActiveJob_ = -1;
        if (dlStopped_.load()) {
            status_ = "Stopped " + dlName_;
        } else if (dlOk_.load()) {
            // Completing a download must NOT interrupt current playback (it used
            // to auto-play, and for a folder that fed a directory to the media
            // engine). Finished items are played from the downloads window.
            status_ = "Downloaded " + dlName_;
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

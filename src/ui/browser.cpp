#include "ui/browser.hpp"

#include "util/logging.hpp"
#include "util/smb_lock.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
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
void smb_auth(const char*, const char*, char*, int, char* user, int userLen,
              char* pass, int passLen) {
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
    auto by_label = [](const Browser::Entry& a, const Browser::Entry& b) {
        return a.label < b.label;
    };
    std::sort(dirs.begin(), dirs.end(), by_label);
    std::sort(files.begin(), files.end(), by_label);
    out.insert(out.end(), dirs.begin(), dirs.end());
    out.insert(out.end(), files.begin(), files.end());
}

} // namespace

Browser::Browser() {
    const char* home = std::getenv("HOME");
    homeDir_ = (home != nullptr) ? home : "/home/steamos";
    downloadDir_ = homeDir_ + "/media";
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
    out.title = dir;
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
            dirs.push_back({name + "/", p.string(), true, false, false, false});
        } else {
            std::error_code sec;
            const long long sz = static_cast<long long>(fs::file_size(p, sec));
            files.push_back({name, p.string(), false, is_video(name), false, false,
                             format_size(sec ? 0 : sz), sec ? 0 : sz});
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
    // mDNS-advertised SMB hosts (NAS, macOS, Samba+avahi). The slow network probe
    // runs OUTSIDE the SMB lock (it is not a libsmbclient call).
    if (uri == "smb://") {
        std::set<std::string> seen;
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
    if (scanning_.load() && scanThread_.joinable()) {
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

void Browser::runScan(std::string location, bool isSmb) {
    ScanResult r;
    if (isSmb) {
        scanSmbInto(location, r);
    } else {
        scanLocalInto(location, r);
    }
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
        if (inSmb_) {
            const std::string parent = smb_parent(location_);
            if (parent.empty()) {
                beginScan(homeDir_, false);
            } else {
                beginScan(parent, true);
            }
        } else {
            const fs::path p(location_);
            beginScan(p.has_parent_path() ? p.parent_path().string() : "/", false);
        }
        return Result::Navigated;
    }
    if (e.isNetwork) {
        beginScan("smb://", true);
        return Result::Navigated;
    }
    if (e.isDir) {
        beginScan(e.path, inSmb_);
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
    if (dlActive_.load() || selected_ < 0 ||
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
    if (dlActive_.load()) return;
    std::string name = nameIn;
    while (!name.empty() && name.back() == '/') name.pop_back();
    const std::size_t slash = name.find_last_of('/');
    if (slash != std::string::npos) name = name.substr(slash + 1);
    if (name.empty()) return;
    dlName_ = name;
    const std::string destDir =
        downloadDir_.empty() ? homeDir_ + "/media" : downloadDir_;
    if (isDir) {
        startDownloadFolder(uri, destDir + "/" + name);
    } else {
        startDownloadFile(uri, destDir + "/" + name);
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
    dlJobs_.push_back({dlName_, local, uri, 0, 0, 0, false});
    dlActiveJob_ = static_cast<int>(dlJobs_.size()) - 1;
    if (dlJobs_.size() > 12) {
        dlJobs_.erase(dlJobs_.begin());
        dlActiveJob_ = static_cast<int>(dlJobs_.size()) - 1;
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
    const std::string service = "//" + rest.substr(0, s2);
    const std::string remote = rest.substr(s2 + 1);
    std::string cmd = "get \"";
    cmd += remote;
    cmd += "\" \"";
    cmd += local;
    cmd += "\"";
    PIXELVR_LOG_INFO("DL spawn: smbclient %s -N -c '%s'", service.c_str(),
                     cmd.c_str());
    char* argv[] = {const_cast<char*>("smbclient"),
                    const_cast<char*>(service.c_str()),
                    const_cast<char*>("-N"),
                    const_cast<char*>("-c"),
                    const_cast<char*>(cmd.c_str()),
                    nullptr};
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
    dlJobs_.push_back({dlName_, destDir, uri, 0, 0, 0, true});
    dlActiveJob_ = static_cast<int>(dlJobs_.size()) - 1;
    if (dlJobs_.size() > 12) {
        dlJobs_.erase(dlJobs_.begin());
        dlActiveJob_ = static_cast<int>(dlJobs_.size()) - 1;
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
        if (j.state == 0) {
            keep.push_back(j);  // keep the in-flight job
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
        // Auto-pause the child while the browse panel is open, so directory
        // listing always responds (resumes when the panel closes).
        const int cpid = dlPid_.load();
        if (cpid > 0) {
            if (browseOpen_.load() && !pausedByBrowse_) {
                kill(cpid, SIGSTOP);
                pausedByBrowse_ = true;
            } else if (!browseOpen_.load() && pausedByBrowse_) {
                kill(cpid, SIGCONT);
                pausedByBrowse_ = false;
            }
        }
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
            playPath = dlLocalPath_;
            status_ = "Playing " + dlName_;
            return Result::Play;
        } else {
            status_ = "Download failed: " + dlName_;
        }
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

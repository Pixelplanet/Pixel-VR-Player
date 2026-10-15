#include "ui/browser.hpp"

#include "util/logging.hpp"
#include "util/smb_lock.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <set>

#ifdef PIXELVR_HAVE_SMB
#include <fcntl.h>
#include <sys/stat.h>

#include <libsmbclient.h>
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
        } else if (is_video(name)) {
            files.push_back({name, p.string(), false, true, false, false});
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
                    case SMBC_FILE:
                        if (is_video(name)) {
                            files.push_back({name, child, false, true, false, false});
                        }
                        break;
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
    if (!e.isVideo || !inSmb_) {
        PIXELVR_LOG_WARN("Download: no SMB video focused (isVideo=%d inSmb=%d)",
                         e.isVideo ? 1 : 0, inSmb_ ? 1 : 0);
        return false;  // Only network videos can be downloaded to local storage.
    }
    startDownload(e.path, e.label);  // async copy to ~/media, then Play via poll()
    return true;
}

Browser::~Browser() {
    if (scanThread_.joinable()) scanThread_.join();
    if (dlThread_.joinable()) dlThread_.join();
}

void Browser::startDownload(const std::string& uri, const std::string& name) {
    if (dlActive_.load()) return;
#ifdef PIXELVR_HAVE_SMB
    if (dlThread_.joinable()) dlThread_.join();
    dlDone_.store(false);
    dlOk_.store(false);
    dlBytes_.store(0);
    dlTotal_.store(0);
    dlName_ = name;
    dlLocalPath_.clear();
    dlActive_.store(true);
    status_ = "Downloading " + name + " ...";
    const std::string home = homeDir_;
    const std::string destDir = downloadDir_.empty() ? home + "/media" : downloadDir_;
    PIXELVR_LOG_INFO("Download start: %s -> %s", uri.c_str(),
                     (destDir + "/" + name).c_str());
    dlThread_ = std::thread([this, uri, name, destDir]() {
        std::error_code ec;
        std::filesystem::create_directories(destDir, ec);
        const std::string local = destDir + "/" + name;

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
            dlLocalPath_ = local;
            dlOk_.store(true);
            dlDone_.store(true);
            dlActive_.store(false);
            return;
        }

        int fd = -1;
        {
            std::lock_guard<std::mutex> smb(smbGlobalLock());
            fd = smbc_open(uri.c_str(), O_RDONLY, 0);
        }
        if (fd < 0) {
            PIXELVR_LOG_WARN("Download: smbc_open failed: %s", uri.c_str());
            dlDone_.store(true);
            dlActive_.store(false);
            return;
        }
        FILE* out = std::fopen(local.c_str(), "wb");
        if (out == nullptr) {
            PIXELVR_LOG_WARN("Download: cannot write %s", local.c_str());
            smbc_close(fd);
            dlDone_.store(true);
            dlActive_.store(false);
            return;
        }
        std::vector<char> buf(1 << 20);
        ssize_t n = 0;
        long long got = 0;
        for (;;) {
            {
                std::lock_guard<std::mutex> smb(smbGlobalLock());
                n = smbc_read(fd, buf.data(), buf.size());
            }
            if (n <= 0) break;
            std::fwrite(buf.data(), 1, static_cast<std::size_t>(n), out);
            got += n;
            dlBytes_.store(got);
        }
        std::fclose(out);
        {
            std::lock_guard<std::mutex> smb(smbGlobalLock());
            smbc_close(fd);
        }

        dlLocalPath_ = local;
        dlOk_.store(n >= 0 && got > 0);
        dlDone_.store(true);
        dlActive_.store(false);
        PIXELVR_LOG_INFO("Download done: %s %lld bytes -> %s (ok=%d)", name.c_str(),
                         got, local.c_str(), (n >= 0 && got > 0) ? 1 : 0);
    });
#else
    (void)uri;
    (void)name;
    status_ = "SMB support not built";
#endif
}

Browser::Result Browser::poll(std::string& playPath) {
    // Publish a finished async directory scan (SMB discovery / large dirs).
    if (scanReady_.exchange(false)) {
        if (scanThread_.joinable()) scanThread_.join();
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
    if (dlActive_.load()) {
        status_ = downloadText();
        return Result::None;
    }
    if (dlDone_.exchange(false)) {
        if (dlThread_.joinable()) dlThread_.join();
        if (dlOk_.load()) {
            playPath = dlLocalPath_;
            status_ = "Playing " + dlName_;
            return Result::Play;
        }
        status_ = "Download failed: " + dlName_;
    }
    return Result::None;
}

std::string Browser::downloadText() const {
    const long long b = dlBytes_.load();
    const long long t = dlTotal_.load();
    std::string s = "Downloading " + dlName_ + "  ";
    if (t > 0) {
        s += std::to_string(b * 100 / t) + "%";
        const double mb = static_cast<double>(b) / (1024.0 * 1024.0);
        const double tot = static_cast<double>(t) / (1024.0 * 1024.0);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "  (%.0f / %.0f MB)", mb, tot);
        s += buf;
    } else {
        s += std::to_string(b / (1024 * 1024)) + " MB";
    }
    return s;
}

} // namespace pixelvr

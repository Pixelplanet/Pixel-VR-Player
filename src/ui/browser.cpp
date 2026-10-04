#include "ui/browser.hpp"

#include "util/logging.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
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
}

void Browser::open() {
    open_ = true;
    inSmb_ = false;
    status_.clear();
    // Resume in the last local folder visited this session, if it still exists.
    std::string start = homeDir_;
    if (!lastLocalDir_.empty()) {
        std::error_code ec;
        if (fs::is_directory(lastLocalDir_, ec)) {
            start = lastLocalDir_;
        }
    }
    scanLocal(start);
}

void Browser::move(int delta) {
    if (entries_.empty()) return;
    selected_ += delta;
    if (selected_ < 0) selected_ = 0;
    if (selected_ >= static_cast<int>(entries_.size())) {
        selected_ = static_cast<int>(entries_.size()) - 1;
    }
}

void Browser::scanLocal(const std::string& dir) {
    inSmb_ = false;
    location_ = dir;
    lastLocalDir_ = dir;  // remember for the next open()
    title_ = dir;
    entries_.clear();
    selected_ = 0;

    if (dir == homeDir_) {
        entries_.push_back({"[ Network (SMB) ]", "", false, false, false, true});
    }
    if (dir != "/") {
        entries_.push_back({".. (up)", "", false, false, true, false});
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
    sort_and_append(entries_, dirs, files);
}

void Browser::scanSmb(const std::string& uri) {
    inSmb_ = true;
    location_ = uri;
    title_ = uri;
    entries_.clear();
    selected_ = 0;
    entries_.push_back({".. (up)", "", false, false, true, false});

#ifdef PIXELVR_HAVE_SMB
    if (!ensure_smb()) {
        status_ = "SMB unavailable";
        return;
    }
    const int dh = smbc_opendir(uri.c_str());
    if (dh < 0) {
        status_ = "Cannot browse " + uri;
        return;
    }
    std::vector<Entry> dirs, files;
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

    // NetBIOS browsing often misses modern servers; augment the network root
    // with mDNS-advertised SMB hosts (NAS, macOS, Samba+avahi).
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
    }

    sort_and_append(entries_, dirs, files);
    status_ = std::to_string(entries_.size() - 1) + " items";
#else
    status_ = "SMB support not built";
#endif
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
                scanLocal(homeDir_);
            } else {
                scanSmb(parent);
            }
        } else {
            const fs::path p(location_);
            scanLocal(p.has_parent_path() ? p.parent_path().string() : "/");
        }
        return Result::Navigated;
    }
    if (e.isNetwork) {
        scanSmb("smb://");
        return Result::Navigated;
    }
    if (e.isDir) {
        if (inSmb_) {
            scanSmb(e.path);
        } else {
            scanLocal(e.path);
        }
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
        return false;  // Only network videos can be downloaded to local storage.
    }
    startDownload(e.path, e.label);  // async copy to ~/media, then Play via poll()
    return true;
}

Browser::~Browser() {
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
    dlThread_ = std::thread([this, uri, name, home]() {
        std::error_code ec;
        std::filesystem::create_directories(home + "/media", ec);
        const std::string local = home + "/media/" + name;

        struct stat st{};
        const long long total =
            (smbc_stat(uri.c_str(), &st) == 0) ? static_cast<long long>(st.st_size)
                                               : 0;
        dlTotal_.store(total);

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

        const int fd = smbc_open(uri.c_str(), O_RDONLY, 0);
        if (fd < 0) {
            dlDone_.store(true);
            dlActive_.store(false);
            return;
        }
        FILE* out = std::fopen(local.c_str(), "wb");
        if (out == nullptr) {
            smbc_close(fd);
            dlDone_.store(true);
            dlActive_.store(false);
            return;
        }
        std::vector<char> buf(1 << 20);
        ssize_t n = 0;
        long long got = 0;
        while ((n = smbc_read(fd, buf.data(), buf.size())) > 0) {
            std::fwrite(buf.data(), 1, static_cast<std::size_t>(n), out);
            got += n;
            dlBytes_.store(got);
        }
        std::fclose(out);
        smbc_close(fd);

        dlLocalPath_ = local;
        dlOk_.store(n >= 0 && got > 0);
        dlDone_.store(true);
        dlActive_.store(false);
    });
#else
    (void)uri;
    (void)name;
    status_ = "SMB support not built";
#endif
}

Browser::Result Browser::poll(std::string& playPath) {
    if (dlActive_.load()) {
        const long long b = dlBytes_.load();
        const long long t = dlTotal_.load();
        if (t > 0) {
            status_ = "Downloading " + dlName_ + "  " +
                      std::to_string(b * 100 / t) + "%";
        } else {
            status_ = "Downloading " + dlName_ + "  " +
                      std::to_string(b / (1024 * 1024)) + " MB";
        }
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

} // namespace pixelvr

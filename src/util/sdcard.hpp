// The SD card as SteamOS mounts it (/run/media/<user>/<label> from /dev/mmcblk*), removable drives in
// general, and free space. Detected from /proc/mounts (an empty mount folder doesn't count); cached for
// two seconds because the app asks every frame. Safe to call from any thread.
#pragma once

#include <sys/statvfs.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace pixelvr {

namespace sdcard_detail {

// /proc/mounts escapes spaces etc. as \040.
inline std::string unescapeMount(const std::string& dir) {
    std::string out;
    for (size_t i = 0; i < dir.size(); ++i) {
        if (dir[i] == '\\' && i + 3 < dir.size()) {
            try {
                out += static_cast<char>(std::stoi(dir.substr(i + 1, 3), nullptr, 8));
            } catch (...) {
                out += dir[i];
                continue;
            }
            i += 3;
        } else {
            out += dir[i];
        }
    }
    return out;
}

struct Mounts {
    std::vector<std::pair<std::string, std::string>> list;  // device, mount point
};

inline Mounts mounts() {
    static std::mutex mu;
    static Mounts cached;
    static std::chrono::steady_clock::time_point at{};
    std::lock_guard<std::mutex> lk(mu);
    const auto now = std::chrono::steady_clock::now();
    if (at != std::chrono::steady_clock::time_point{} && now - at < std::chrono::seconds(2)) return cached;
    at = now;
    cached.list.clear();
    std::ifstream in("/proc/mounts");
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string dev, dir;
        ls >> dev >> dir;
        if (!dir.empty()) cached.list.emplace_back(dev, unescapeMount(dir));
    }
    return cached;
}

inline bool under(const std::string& path, const std::string& dir) {
    if (dir == "/") return !path.empty() && path[0] == '/';
    return path == dir || (path.size() > dir.size() && path.compare(0, dir.size(), dir) == 0 && path[dir.size()] == '/');
}

}  // namespace sdcard_detail

inline std::string sdCardMount() {
    for (const auto& m : sdcard_detail::mounts().list) {
        if (m.first.rfind("/dev/mmcblk", 0) == 0 && m.second.rfind("/run/media/", 0) == 0) return m.second;
    }
    return {};
}

// The mount point holding `path` (longest match; "/" if nothing else).
inline std::string mountPointOf(const std::string& path) {
    std::string best = "/";
    for (const auto& m : sdcard_detail::mounts().list) {
        if (m.second.size() > best.size() && sdcard_detail::under(path, m.second)) best = m.second;
    }
    return best;
}

// `path` is on a removable drive (under /run/media) that is no longer mounted: an SD card or USB drive
// was pulled. Writing there would recreate the folders on the internal storage.
inline bool removableMissing(const std::string& path) {
    if (path.rfind("/run/media/", 0) != 0) return false;
    return mountPointOf(path).rfind("/run/media/", 0) != 0;
}

// Free bytes for the user at `path` (0 if unknown).
inline uint64_t freeBytes(const std::string& path) {
    struct statvfs s{};
    if (path.empty() || statvfs(path.c_str(), &s) != 0) return 0;
    return static_cast<uint64_t>(s.f_bavail) * s.f_frsize;
}

inline std::string gigabytes(uint64_t bytes) {
    char b[32];
    std::snprintf(b, sizeof b, "%.1f GB", static_cast<double>(bytes) / 1e9);
    return b;
}

}  // namespace pixelvr

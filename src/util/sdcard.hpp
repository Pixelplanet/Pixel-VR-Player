// The SD card as SteamOS mounts it (/run/media/<user>/<label> from /dev/mmcblk*), and free space.
// Detected from /proc/mounts (an empty mount folder doesn't count); cached for two seconds because the
// app asks every frame.
#pragma once

#include <sys/statvfs.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace pixelvr {

inline std::string sdCardMount() {
    static std::string cached;
    static std::chrono::steady_clock::time_point at{};
    const auto now = std::chrono::steady_clock::now();
    if (at != std::chrono::steady_clock::time_point{} && now - at < std::chrono::seconds(2)) return cached;
    at = now;
    cached.clear();
    std::ifstream in("/proc/mounts");
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string dev, dir;
        ls >> dev >> dir;
        if (dev.rfind("/dev/mmcblk", 0) != 0 || dir.rfind("/run/media/", 0) != 0) continue;
        std::string out;  // /proc/mounts escapes spaces etc. as \040
        for (size_t i = 0; i < dir.size(); ++i) {
            if (dir[i] == '\\' && i + 3 < dir.size()) {
                out += static_cast<char>(std::stoi(dir.substr(i + 1, 3), nullptr, 8));
                i += 3;
            } else {
                out += dir[i];
            }
        }
        cached = out;
        break;
    }
    return cached;
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

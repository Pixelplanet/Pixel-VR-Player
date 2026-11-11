#include "app/playback_memory.hpp"

#include "util/logging.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace pixelvr {

namespace {

constexpr std::size_t kMaxEntries = 500;     // oldest entries are pruned on save
constexpr double kResumeMinSec = 15.0;       // closer to the start: start over
constexpr double kResumeEndMarginSec = 30.0; // closer to the end: start over

std::string statePath() {
    const char* home = std::getenv("HOME");
    return std::string(home ? home : ".") + "/.config/pixelvr/playback_state.tsv";
}

long long nowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace

void PlaybackMemory::load() {
    entries_.clear();
    std::ifstream in(statePath());
    std::string line;
    while (std::getline(in, line)) {
        // path \t position \t projection \t stereo \t touched [\t fingerprint]
        std::vector<std::string> f;
        std::stringstream ss(line);
        std::string part;
        while (std::getline(ss, part, '\t')) f.push_back(part);
        if ((f.size() != 5 && f.size() != 6) || f[0].empty()) continue;
        try {
            Entry e;
            e.position = std::stod(f[1]);
            e.projection = std::clamp(std::stoi(f[2]), -1, 2);
            e.stereo = std::clamp(std::stoi(f[3]), -1, 3);  // 3 = frame-sequential
            e.touched = std::stoll(f[4]);
            if (f.size() == 6) e.fingerprint = f[5];
            entries_[f[0]] = e;
        } catch (const std::exception&) {
            continue;  // skip malformed lines
        }
    }
    dirty_ = false;
}

void PlaybackMemory::save() {
    if (!dirty_) return;
    std::vector<std::pair<std::string, Entry>> rows(entries_.begin(), entries_.end());
    std::sort(rows.begin(), rows.end(),
              [](const auto& a, const auto& b) { return a.second.touched > b.second.touched; });
    if (rows.size() > kMaxEntries) rows.resize(kMaxEntries);
    const std::string path = statePath();
    try {
        std::filesystem::create_directories(std::filesystem::path(path).parent_path());
        const std::string tmp = path + ".tmp";
        {
            std::ofstream out(tmp, std::ios::trunc);
            for (const auto& [file, e] : rows) {
                if (file.find('\t') != std::string::npos || file.find('\n') != std::string::npos) continue;
                out << file << '\t' << e.position << '\t' << e.projection << '\t' << e.stereo
                    << '\t' << e.touched << '\t' << e.fingerprint << '\n';
            }
        }
        std::filesystem::rename(tmp, path);  // atomic replace
        dirty_ = false;
    } catch (const std::exception& ex) {
        PIXELVR_LOG_WARN("Could not save playback state: %s", ex.what());
    }
}

std::string PlaybackMemory::fingerprintOf(const std::string& path) {
    if (path.empty() || path[0] != '/') return {};
    std::ifstream in(path, std::ios::binary);
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (!in || ec || size == 0) return {};
    constexpr std::size_t kChunk = 64 * 1024;
    uint64_t hash = 14695981039346656037ULL;  // FNV-1a over the first and last 64 KB
    std::vector<char> buf(kChunk);
    auto mix = [&](std::streamoff at) {
        in.clear();
        in.seekg(at);
        in.read(buf.data(), static_cast<std::streamsize>(kChunk));
        for (std::streamsize i = 0; i < in.gcount(); ++i) {
            hash ^= static_cast<uint8_t>(buf[static_cast<size_t>(i)]);
            hash *= 1099511628211ULL;
        }
    };
    mix(0);
    if (size > kChunk) mix(static_cast<std::streamoff>(size - std::min<uintmax_t>(size, kChunk)));
    char out[48];
    std::snprintf(out, sizeof(out), "%llx-%016llx", static_cast<unsigned long long>(size),
                  static_cast<unsigned long long>(hash));
    return out;
}

const PlaybackMemory::Entry* PlaybackMemory::find(const std::string& path, const std::string& fingerprint) {
    const auto it = entries_.find(path);
    if (it != entries_.end()) return &it->second;
    if (fingerprint.empty()) return nullptr;
    for (auto byId = entries_.begin(); byId != entries_.end(); ++byId) {
        if (byId->second.fingerprint != fingerprint) continue;
        // The same file under a new name or folder: remember it there now.
        Entry moved = byId->second;
        entries_.erase(byId);
        dirty_ = true;
        return &(entries_[path] = moved);
    }
    return nullptr;
}

PlaybackMemory::Entry& PlaybackMemory::touch(const std::string& path) {
    Entry& e = entries_[path];
    e.touched = nowSeconds();
    dirty_ = true;
    return e;
}

void PlaybackMemory::setPosition(const std::string& path, double position, double duration,
                                 const std::string& fingerprint) {
    if (path.empty()) return;
    const bool resumable = position >= kResumeMinSec &&
                           (duration <= 0.0 || position <= duration - kResumeEndMarginSec);
    const double stored = resumable ? position : 0.0;
    const Entry* existing = find(path, fingerprint);
    if (existing && std::abs(existing->position - stored) < 1.0 &&
        (fingerprint.empty() || existing->fingerprint == fingerprint)) {
        return;  // nothing new
    }
    Entry& e = touch(path);
    e.position = stored;
    if (!fingerprint.empty()) e.fingerprint = fingerprint;
}

void PlaybackMemory::forgetUnder(const std::string& path) {
    if (path.empty()) return;
    for (auto it = entries_.begin(); it != entries_.end();) {
        const std::string& p = it->first;
        const bool under = p == path || (p.size() > path.size() && p.compare(0, path.size(), path) == 0 && p[path.size()] == '/');
        if (under) {
            it = entries_.erase(it);
            dirty_ = true;
        } else {
            ++it;
        }
    }
}

void PlaybackMemory::setFormat(const std::string& path, int projection, int stereo, const std::string& fingerprint) {
    if (path.empty()) return;
    Entry& e = touch(path);
    e.projection = projection;
    e.stereo = stereo;
    if (!fingerprint.empty()) e.fingerprint = fingerprint;
}

}  // namespace pixelvr

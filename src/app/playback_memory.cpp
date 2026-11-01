#include "app/playback_memory.hpp"

#include "util/logging.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
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
        // path \t position \t projection \t stereo \t touched
        std::vector<std::string> f;
        std::stringstream ss(line);
        std::string part;
        while (std::getline(ss, part, '\t')) f.push_back(part);
        if (f.size() != 5 || f[0].empty()) continue;
        try {
            Entry e;
            e.position = std::stod(f[1]);
            e.projection = std::clamp(std::stoi(f[2]), -1, 2);
            e.stereo = std::clamp(std::stoi(f[3]), -1, 2);
            e.touched = std::stoll(f[4]);
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
                    << '\t' << e.touched << '\n';
            }
        }
        std::filesystem::rename(tmp, path);  // atomic replace
        dirty_ = false;
    } catch (const std::exception& ex) {
        PIXELVR_LOG_WARN("Could not save playback state: %s", ex.what());
    }
}

const PlaybackMemory::Entry* PlaybackMemory::find(const std::string& path) const {
    const auto it = entries_.find(path);
    return it == entries_.end() ? nullptr : &it->second;
}

PlaybackMemory::Entry& PlaybackMemory::touch(const std::string& path) {
    Entry& e = entries_[path];
    e.touched = nowSeconds();
    dirty_ = true;
    return e;
}

void PlaybackMemory::setPosition(const std::string& path, double position, double duration) {
    if (path.empty()) return;
    const bool resumable = position >= kResumeMinSec &&
                           (duration <= 0.0 || position <= duration - kResumeEndMarginSec);
    const double stored = resumable ? position : 0.0;
    const Entry* existing = find(path);
    if (existing && std::abs(existing->position - stored) < 1.0) return;  // nothing new
    touch(path).position = stored;
}

void PlaybackMemory::setFormat(const std::string& path, int projection, int stereo) {
    if (path.empty()) return;
    Entry& e = touch(path);
    e.projection = projection;
    e.stereo = stereo;
}

}  // namespace pixelvr

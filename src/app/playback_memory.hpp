#pragma once

#include <string>
#include <unordered_map>

namespace pixelvr {

// Remembers, per media file, where playback stopped and any projection/stereo
// format the viewer picked by hand, so reopening a file resumes there in the
// right format. Stored as a small tab-separated file next to the settings.
// Callers must not record hidden-vault paths (nothing about them is persisted).
class PlaybackMemory {
public:
    struct Entry {
        double position = 0.0;  // seconds; 0 = start from the beginning
        int projection = -1;    // -1 = auto-detect, else ProjectionMode index
        int stereo = -1;        // -1 = auto-detect, else StereoMode index
        long long touched = 0;  // unix seconds of the last update (for pruning)
    };

    void load();
    // Writes the file if anything changed since the last save.
    void save();

    const Entry* find(const std::string& path) const;
    // Records the playback position; positions near the start or the end are
    // cleared so a finished file starts over next time.
    void setPosition(const std::string& path, double position, double duration);
    void setFormat(const std::string& path, int projection, int stereo);

private:
    Entry& touch(const std::string& path);

    std::unordered_map<std::string, Entry> entries_;
    bool dirty_ = false;
};

}  // namespace pixelvr

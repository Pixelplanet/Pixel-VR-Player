#pragma once
// File-name checks for what the player opens.

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <string>

namespace pixelvr {

inline bool hasExtension(const std::string& name, std::initializer_list<const char*> exts) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const char* e : exts) {
        const std::size_t n = std::char_traits<char>::length(e);
        if (lower.size() >= n && lower.compare(lower.size() - n, n, e) == 0) return true;
    }
    return false;
}

inline bool isAudioFile(const std::string& name) {
    return hasExtension(name, {".mp3", ".flac", ".m4a", ".aac", ".ogg", ".oga", ".opus", ".wav", ".wma", ".alac",
                               ".aiff", ".aif", ".ape", ".mka"});
}

// Anything the player opens: videos and audio files (shown with cover art).
inline bool isPlayableFile(const std::string& name) {
    return isAudioFile(name) || hasExtension(name, {".mp4", ".mkv", ".mov", ".webm", ".m4v", ".avi", ".ts", ".m2ts",
                                                    ".wmv", ".flv", ".mpg", ".m3u8"});
}

}  // namespace pixelvr

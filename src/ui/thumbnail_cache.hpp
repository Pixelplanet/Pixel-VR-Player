#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace pixelvr {

class ThumbnailCache {
public:
    struct MediaMeta {
        int width = 0;
        int height = 0;
        double fps = 0.0;
        double durationSec = 0.0;
        bool ready = false;
    };

    ThumbnailCache();
    ~ThumbnailCache();

    // Requests thumbnail extraction/load and metadata probe for a path (non-blocking).
    // If highPriority is true, it is inserted at the front of the queue (e.g. visible page cards).
    void request(const std::string& videoPath, bool highPriority = false);

    // Returns true and copies out the 256x144 RGBA pixels if ready.
    bool get(const std::string& videoPath, std::vector<uint8_t>& outRgba);

    // Returns true and populates MediaMeta if probed.
    bool getMeta(const std::string& videoPath, MediaMeta& outMeta);

    // Clears cached thumbnails and metadata whose paths start with vaultDir.
    void clearVaultThumbs(const std::string& vaultDir);

private:
    void workerLoop();
    bool extractThumbnailAndMeta(const std::string& videoPath,
                                 std::vector<uint8_t>& outRgba,
                                 MediaMeta& outMeta);

    std::string cacheDir_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::string> queue_;
    std::unordered_map<std::string, bool> queuedPaths_;
    std::unordered_map<std::string, std::vector<uint8_t>> cache_;
    std::unordered_map<std::string, MediaMeta> metaCache_;
    std::atomic<bool> stop_{false};
    std::thread worker_;
};

} // namespace pixelvr

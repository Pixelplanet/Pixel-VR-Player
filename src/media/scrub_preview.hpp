#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pixelvr {

// Timeline hover preview: decodes the keyframe nearest a requested time from
// the current clip on a background thread, using a software decoder so the
// hardware decoder used for playback is never touched. Only the most recent
// request is served; older ones are dropped.
class ScrubPreview {
public:
    static constexpr int kWidth = 256;
    static constexpr int kHeight = 144;

    ScrubPreview();
    ~ScrubPreview();

    ScrubPreview(const ScrubPreview&) = delete;
    ScrubPreview& operator=(const ScrubPreview&) = delete;

    // Sets the clip to preview (empty path closes it). stereoMode: 0 mono,
    // 1 side-by-side, 2 top-bottom; stereo frames show the left/top eye only.
    void setSource(const std::string& path, int stereoMode);
    // Asks for a preview at `seconds`; cheap to call every frame.
    void request(double seconds);
    // Returns true once per new result and copies the kWidth x kHeight RGBA
    // pixels and the timestamp they were taken from.
    bool fetch(std::vector<uint8_t>& rgba, double& seconds);

private:
    void workerLoop();

    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_;
    bool stop_ = false;

    std::string path_;
    int stereo_ = 0;
    uint64_t sourceGen_ = 0;  // bumped by setSource so the worker reopens
    bool hasRequest_ = false;
    double requestSec_ = 0.0;

    std::vector<uint8_t> result_;
    double resultSec_ = 0.0;
    bool resultNew_ = false;
};

}  // namespace pixelvr

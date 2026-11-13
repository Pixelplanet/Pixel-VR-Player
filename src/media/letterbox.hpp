// Finds black bars encoded above and below the picture (letterboxing), so screens can leave them out.
// Runs in the background on the file being played: decodes a few frames spread across it (software
// decoder, never the hardware one) and measures the black rows at the top and bottom.
//
// Safe by design: a frame only counts when it is bright enough to judge; the crop is the smallest bar
// seen on ANY counted frame (minus a small margin), so a dark scene can never cut into the picture.
#pragma once

#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace pixelvr {

class LetterboxDetector {
public:
    LetterboxDetector();
    ~LetterboxDetector();
    // Starts a measurement (no-op when known or queued). Local files and http(s) streams.
    // `keepPrivate` (vault files): the result is kept in memory only, never written to the cache file.
    void request(const std::string& path, bool keepPrivate = false);
    // True once measured: `frac` = share of the picture height cut at the top and at the bottom each
    // (0 = no bars).
    bool result(const std::string& path, float& frac);
    // Drops results for `path` and everything under it (e.g. moved into the vault).
    void forgetUnder(const std::string& path);

    // The measurement itself (also used by tools/letterbox_test).
    static float measure(const std::string& path, int* framesJudged = nullptr);

private:
    void loop();
    void load();
    void save();
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::string> queue_;
    std::unordered_map<std::string, float> results_;  // key: path + size/mtime stamp
    std::unordered_map<std::string, float> privateResults_;  // vault files: this session only, never saved
    std::deque<std::string> privateQueued_;                  // paths in queue_ that are private
    std::string file_;
    bool stop_ = false;
    std::thread worker_;
};

}  // namespace pixelvr

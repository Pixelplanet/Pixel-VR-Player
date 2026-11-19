#pragma once

// 3D Autofocus recording (Settings > Display & Cinema > 3D Autofocus > Record Autofocus Log):
// what the eyes did and what the autofocus measured and applied, one row per frame, so a test
// session can be studied afterwards. Only while the user switched it on (not saved, off at every
// start), never for vault / private videos, into the player's logs folder:
//   autofocus-<date>-<time>.csv          one row per frame
//   autofocus-<date>-<time>-scores.csv   the 128 match scores of every measurement
// Directions are unit vectors in the app's reference space (x right, y up, -z forward at the
// recenter). Stops by itself at kMaxBytes.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>
#include <unistd.h>

namespace pixelvr {

class FocusLog {
public:
    static constexpr long kMaxBytes = 80L * 1024 * 1024;

    struct Row {
        float dt = 0.0f;
        double pos = -1.0;            // video position (s)
        bool paused = false, menu = false;
        int projection = 0, stereo = 0;
        bool swap = false;
        bool rawOk = false;
        float raw[3] = {0, 0, -1};     // the gaze as the tracker gave it
        bool filtOk = false;
        float filt[3] = {0, 0, -1};    // steadied (what is measured at)
        bool headOk = false;
        float head[3] = {0, 0, -1};    // head forward
        bool srcOk = false;
        float lonDeg = 0, latDeg = 0;  // gaze point on the source video
        bool measured = false;         // a search result arrived this frame
        bool mOk = false;
        float dispDeg = 0, score = 0, variance = 0;
        int best = -1;
        bool focused = false;
        float distM = 0, targetDeg = 0, offsetDeg = 0, testDeg = 0;
        int event = 0;                 // StereoFocus::Event: 1 engage, 2 retarget, 3 release far,
                                       // 4 release invalid, 5 release strength 0
        float velocity = 0, invalidS = 0, gazeLostS = 0;
        bool onPicture = false;
        float zoom = 1;
    };

    ~FocusLog() { close(); }

    bool isOpen() const { return frames_ != nullptr; }
    const std::string& name() const { return name_; }

    // Opens a new pair of files in the player's logs folder; false if that fails.
    bool open(const std::string& settingsLine) {
        close();
        std::error_code ec;
        const std::filesystem::path dir = exeDir() / "logs";
        std::filesystem::create_directories(dir, ec);
        const std::time_t now = std::time(nullptr);
        std::tm tm{};
        localtime_r(&now, &tm);
        char stamp[32];
        std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
        name_ = std::string("autofocus-") + stamp;
        frames_ = std::fopen((dir / (name_ + ".csv")).c_str(), "w");
        scores_ = std::fopen((dir / (name_ + "-scores.csv")).c_str(), "w");
        if (frames_ == nullptr || scores_ == nullptr) {
            close();
            return false;
        }
        bytes_ = 0;
        start_ = std::chrono::steady_clock::now();
        lastFlush_ = start_;
        bytes_ += std::fprintf(frames_, "# %s\n", settingsLine.c_str());
        bytes_ += std::fprintf(frames_,
            "t,dt,pos,paused,menu,proj,stereo,swap,raw_ok,rx,ry,rz,filt_ok,fx,fy,fz,head_ok,hx,hy,hz,"
            "src_ok,lon,lat,meas,m_ok,disp,score,var,best,focused,dist,target,offset,test,"
            "event,vel,invalid_s,gaze_lost_s,on_pic,zoom\n");
        bytes_ += std::fprintf(scores_, "t,var,best,scores (candidate 0..127; candidate 16 = no shift, "
                                        "step 0.1667 deg; nan = reads past the picture)\n");
        return true;
    }

    void close() {
        if (frames_ != nullptr) std::fclose(frames_);
        if (scores_ != nullptr) std::fclose(scores_);
        frames_ = scores_ = nullptr;
    }

    void frame(const Row& r) {
        if (!isOpen()) return;
        const double t = seconds();
        bytes_ += std::fprintf(frames_,
            "%.4f,%.4f,%.3f,%d,%d,%d,%d,%d,%d,%.5f,%.5f,%.5f,%d,%.5f,%.5f,%.5f,%d,%.5f,%.5f,%.5f,"
            "%d,%.3f,%.3f,%d,%d,%.3f,%.4f,%.6f,%d,%d,%.3f,%.3f,%.3f,%.2f,%d,%.3f,%.3f,%.3f,%d,%.3f\n",
            t, r.dt, r.pos, r.paused, r.menu, r.projection, r.stereo, r.swap, r.rawOk, r.raw[0], r.raw[1],
            r.raw[2], r.filtOk, r.filt[0], r.filt[1], r.filt[2], r.headOk, r.head[0], r.head[1], r.head[2],
            r.srcOk, r.lonDeg, r.latDeg, r.measured, r.mOk, r.dispDeg, r.score, r.variance, r.best,
            r.focused, r.distM, r.targetDeg, r.offsetDeg, r.testDeg, r.event, r.velocity, r.invalidS,
            r.gazeLostS, r.onPicture, r.zoom);
        maybeFlush();
    }

    void scores(const float* s, int n, float variance, int best) {
        if (!isOpen() || s == nullptr) return;
        bytes_ += std::fprintf(scores_, "%.4f,%.6f,%d", seconds(), variance, best);
        for (int i = 0; i < n; ++i) bytes_ += std::fprintf(scores_, ",%.3f", s[i]);
        bytes_ += std::fprintf(scores_, "\n");
        maybeFlush();
    }

    bool full() const { return bytes_ > kMaxBytes; }

private:
    static std::filesystem::path exeDir() {
        char buf[4096];
        const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n <= 0) return std::filesystem::path(".");
        buf[n] = '\0';
        return std::filesystem::path(buf).parent_path();
    }
    double seconds() const {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
    }
    void maybeFlush() {
        const auto now = std::chrono::steady_clock::now();
        if (now - lastFlush_ < std::chrono::seconds(1)) return;
        lastFlush_ = now;
        std::fflush(frames_);
        std::fflush(scores_);
    }

    std::FILE* frames_ = nullptr;
    std::FILE* scores_ = nullptr;
    std::string name_;
    long bytes_ = 0;
    std::chrono::steady_clock::time_point start_{};
    std::chrono::steady_clock::time_point lastFlush_{};
};

}  // namespace pixelvr

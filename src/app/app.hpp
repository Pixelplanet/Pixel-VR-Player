#pragma once

#include "gfx/vulkan_renderer.hpp"
#include "xr/xr_context.hpp"
#include "xr/render_model.hpp"

#ifdef PIXELVR_HAVE_MEDIA
#include "media/media_engine.hpp"
#endif

#ifdef PIXELVR_HAVE_FREETYPE
#include "ui/browser.hpp"
#endif

#include "ui/settings_menu.hpp"

#include <chrono>
#include <string>

namespace pixelvr {

// Owns the top-level objects and runs the frame loop. Later phases add the
// scene, UI, and media sources here.
class App {
public:
    bool init(const std::string& mediaPath = "");
    int run();
    void requestExit() { exitRequested_ = true; }

private:
    // Declaration order matters for teardown: the XR session (xr_) must be
    // destroyed before the Vulkan device (renderer_), so renderer_ is declared
    // first and therefore destroyed last.
    VulkanRenderer renderer_;
    XrContext xr_;
    RenderModelSystem renderModels_;
    bool exitRequested_ = false;

    // Transport HUD auto-hide timer.
    std::chrono::steady_clock::time_point uiUntil_{};
    // Frame timing for continuous seek (F4).
    std::chrono::steady_clock::time_point lastFrameTime_{};

    // Controller activity: auto-hide the laser, models and controls when the
    // user holds the controllers still for a few seconds.
    std::chrono::steady_clock::time_point lastControllerActivity_{};
    float activityAnchorPos_[2][3] = {{0, 0, 0}, {0, 0, 0}};
    float activityAnchorOrient_[2][4] = {{0, 0, 0, 1}, {0, 0, 0, 1}};
    bool activityAnchorValid_[2] = {false, false};
    bool seekStepArmed_ = true;  // right-stick +/-30s debounce
    bool navArmed_ = true;       // menu nav: armed for the next stick press
    std::chrono::steady_clock::time_point navNextRepeat_{};  // nav auto-repeat
    bool scrubbing_ = false;     // dragging the timeline / stick-scrubbing
    double scrubTarget_ = 0.0;   // pending scrub position (seconds)
    std::chrono::steady_clock::time_point perfLastLog_{};
    int perfFrames_ = 0;
    uint64_t perfLastDecoded_ = 0;

    // Place the screen in front of the user on the first valid head pose.
    bool didInitialRecenter_ = false;
    bool controllerModelLoaded_ = false;  // real glTF model uploaded once
    // Local grip->render-model-space correction so the model sits exactly where
    // the runtime renders it (derived from the fetched model's owner hand).
    bool modelCorrValid_ = false;
    float modelCorrPos_[3] = {0.0f, 0.0f, 0.0f};
    float modelCorrQuat_[4] = {0.0f, 0.0f, 0.0f, 1.0f};

    // Debug statistics (recomputed a few times per second).
    std::chrono::steady_clock::time_point lastStatsUpdate_{};
    int statsFrameAccum_ = 0;
    uint64_t statsLastDecoded_ = 0;
    double decFpsEma_ = 0.0;  // smoothed decode fps (the raw window is bursty)
    unsigned long long cpuPrevIdle_ = 0;
    unsigned long long cpuPrevTotal_ = 0;
    std::vector<std::string> statsLines_;
    void updateStats();
    // Menu nav stick auto-repeat: -1/+1 to step (initial press + hold), else 0.
    int navStep(float axisY, std::chrono::steady_clock::time_point now);

#ifdef PIXELVR_HAVE_FREETYPE
    Browser browser_;
#endif

    SettingsMenu settings_;

#ifdef PIXELVR_HAVE_MEDIA
    MediaEngine media_;
    VideoFrame frame_;
    DrmVideoFrame drmFrame_;
    bool mediaOpen_ = false;
    std::string currentMediaPath_;  // for reloading on a track/language change
    void loadMedia(const std::string& path);
    void switchTracks(bool byLanguage);  // reload current media at its position
#endif
};

} // namespace pixelvr

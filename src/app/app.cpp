#include "app/app.hpp"

#include "util/logging.hpp"

#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace pixelvr {

namespace {

std::string to_lower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

bool contains(const std::string& s, const char* token) {
    return s.find(token) != std::string::npos;
}

// Infers projection and stereo layout from common filename conventions, e.g.
// "clip_360_tb.mp4", "movie_180_sbs.mkv", "trailer_vr180.mp4". `stereoFromName`
// reports whether the stereo layout was stated explicitly (so the caller can
// refine it from the frame aspect ratio otherwise).
void detect_format(const std::string& path, VulkanRenderer::ProjectionMode& proj,
                   VulkanRenderer::StereoMode& stereo, bool& stereoFromName) {
    const std::string p = to_lower(path);

    if (contains(p, "360") || contains(p, "equirect")) {
        proj = VulkanRenderer::ProjectionMode::Equirect360;
    } else if (contains(p, "180") || contains(p, "vr180")) {
        proj = VulkanRenderer::ProjectionMode::Equirect180;
    } else {
        proj = VulkanRenderer::ProjectionMode::Flat;
    }

    stereoFromName = true;
    if (contains(p, "_tb") || contains(p, "-tb") || contains(p, "_ou") ||
        contains(p, "overunder") || contains(p, "over-under")) {
        stereo = VulkanRenderer::StereoMode::TopBottom;
    } else if (contains(p, "_sbs") || contains(p, "-sbs") || contains(p, "_lr") ||
               contains(p, "sidebyside") || contains(p, "side-by-side") ||
               contains(p, "half-sbs")) {
        stereo = VulkanRenderer::StereoMode::SideBySide;
    } else if (contains(p, "3d")) {
        stereo = (proj == VulkanRenderer::ProjectionMode::Equirect180)
                     ? VulkanRenderer::StereoMode::SideBySide
                     : VulkanRenderer::StereoMode::TopBottom;
    } else {
        stereo = VulkanRenderer::StereoMode::Mono;
        stereoFromName = false;  // no explicit layout; refine from aspect ratio
    }
}

} // namespace

bool App::init(const std::string& mediaPath) {
    if (!xr_.createInstanceAndSystem("Pixel VR Player")) {
        return false;
    }
    if (!renderer_.createDevice(xr_)) {
        return false;
    }
    if (!xr_.createSession(renderer_.graphicsBinding())) {
        return false;
    }
    if (!xr_.createReferenceSpace()) {
        return false;
    }
    if (!xr_.createActions()) {
        PIXELVR_LOG_WARN("Controller input unavailable; continuing without it");
    }
    renderModels_.init(xr_.instance(), xr_.session(), xr_.renderModelEnabled());

    const std::vector<int64_t> formats = xr_.enumerateSwapchainFormats();
    if (formats.empty()) {
        PIXELVR_LOG_ERROR("Runtime reported no swapchain formats");
        return false;
    }
    const int64_t colorFormat = renderer_.chooseColorFormat(formats);
    if (!xr_.createSwapchains(colorFormat)) {
        return false;
    }
    if (!renderer_.createRenderResources(xr_.swapchains())) {
        return false;
    }

#ifdef PIXELVR_HAVE_MEDIA
    if (!mediaPath.empty()) {
        loadMedia(mediaPath);
    }
#else
    (void)mediaPath;
#endif

    PIXELVR_LOG_INFO("Initialization complete");
    return true;
}

#ifdef PIXELVR_HAVE_MEDIA
void App::loadMedia(const std::string& path) {
    VulkanRenderer::ProjectionMode proj = VulkanRenderer::ProjectionMode::Flat;
    VulkanRenderer::StereoMode stereo = VulkanRenderer::StereoMode::Mono;
    bool stereoFromName = false;
    detect_format(path, proj, stereo, stereoFromName);

    mediaOpen_ = media_.open(path);
    if (!mediaOpen_) {
        // Clear stale video from the previous clip so the renderer shows black
        // instead of the last decoded frame of the old file.
        renderer_.clearVideo();
        PIXELVR_LOG_WARN("Failed to open %s", path.c_str());
        return;
    }

    // Refine the stereo layout from the frame aspect ratio when the filename did
    // not state it: once the projection is a VR type the aspect is unambiguous
    // (360 mono 2:1, SBS 4:1, OU 1:1; 180 mono 1:1, SBS 2:1, OU 1:2).
    const int w = media_.width();
    const int h = media_.height();
    if (!stereoFromName && h > 0 &&
        proj != VulkanRenderer::ProjectionMode::Flat) {
        const float ar = static_cast<float>(w) / static_cast<float>(h);
        if (proj == VulkanRenderer::ProjectionMode::Equirect360) {
            stereo = ar >= 3.0f   ? VulkanRenderer::StereoMode::SideBySide
                     : ar <= 1.3f ? VulkanRenderer::StereoMode::TopBottom
                                  : VulkanRenderer::StereoMode::Mono;
        } else {
            stereo = ar >= 1.6f   ? VulkanRenderer::StereoMode::SideBySide
                     : ar <= 0.7f ? VulkanRenderer::StereoMode::TopBottom
                                  : VulkanRenderer::StereoMode::Mono;
        }
    }

    renderer_.setProjectionMode(proj);
    renderer_.setStereoMode(stereo);
    settings_.setProjectionMode(static_cast<int>(proj));
    settings_.setStereoMode(static_cast<int>(stereo));
    static const char* const kProj[] = {"flat", "360", "180"};
    static const char* const kStereo[] = {"mono", "SBS", "TB"};
    PIXELVR_LOG_INFO("Loaded %s (%dx%d, %s/%s)", path.c_str(), w, h,
                     kProj[static_cast<int>(proj)],
                     kStereo[static_cast<int>(stereo)]);
}

void App::updateStats() {
    using clock = std::chrono::steady_clock;
    const auto now = clock::now();
    ++statsFrameAccum_;
    if (lastStatsUpdate_.time_since_epoch().count() == 0) {
        lastStatsUpdate_ = now;
        statsLastDecoded_ = media_.decodedFrames();
        return;
    }
    const double elapsed =
        std::chrono::duration<double>(now - lastStatsUpdate_).count();
    if (elapsed < 0.5) {
        return;  // refresh the readout ~twice a second
    }

    const double fps = statsFrameAccum_ / elapsed;
    const uint64_t dec = media_.decodedFrames();
    const double decFps = static_cast<double>(dec - statsLastDecoded_) / elapsed;
    statsFrameAccum_ = 0;
    statsLastDecoded_ = dec;
    lastStatsUpdate_ = now;

    // Aggregate CPU load from /proc/stat deltas.
    float cpuPct = -1.0f;
    {
        std::ifstream f("/proc/stat");
        std::string cpu;
        unsigned long long u = 0, n = 0, s = 0, idle = 0, io = 0, irq = 0,
                           sirq = 0, steal = 0;
        if (f >> cpu >> u >> n >> s >> idle >> io >> irq >> sirq >> steal) {
            const unsigned long long idleAll = idle + io;
            const unsigned long long total =
                u + n + s + idle + io + irq + sirq + steal;
            if (cpuPrevTotal_ != 0 && total > cpuPrevTotal_) {
                const unsigned long long dtot = total - cpuPrevTotal_;
                const unsigned long long didle = idleAll - cpuPrevIdle_;
                cpuPct = 100.0f * static_cast<float>(dtot - didle) /
                         static_cast<float>(dtot);
            }
            cpuPrevTotal_ = total;
            cpuPrevIdle_ = idleAll;
        }
    }

    // RAM usage from /proc/meminfo.
    long memTotalKb = 0, memAvailKb = 0;
    {
        std::ifstream f("/proc/meminfo");
        std::string key, unit;
        long val = 0;
        while (f >> key >> val >> unit) {
            if (key == "MemTotal:") memTotalKb = val;
            else if (key == "MemAvailable:") { memAvailKb = val; break; }
        }
    }

    // Adreno GPU: the Frame runs the Turnip/msm driver (no kgsl sysfs), so report
    // the GPU clock from devfreq (the governor scales it with load; pegged at max
    // means GPU-bound).
    long gpuCurMhz = 0, gpuMaxMhz = 0;
    {
        std::ifstream cf("/sys/class/devfreq/3d00000.gpu/cur_freq");
        std::ifstream mf("/sys/class/devfreq/3d00000.gpu/max_freq");
        long cur = 0, mx = 0;
        if (cf >> cur) gpuCurMhz = cur / 1000000;
        if (mf >> mx) gpuMaxMhz = mx / 1000000;
    }

    char buf[160];
    statsLines_.clear();
    std::snprintf(buf, sizeof(buf), "FPS %.1f   (%.1f ms)", fps,
                  fps > 0.0 ? 1000.0 / fps : 0.0);
    statsLines_.emplace_back(buf);
    if (mediaOpen_) {
        std::snprintf(buf, sizeof(buf), "Video %dx%d  %s", media_.width(),
                      media_.height(), media_.codecName().c_str());
        statsLines_.emplace_back(buf);
        std::snprintf(buf, sizeof(buf), "Decode %.1f fps", decFps);
        statsLines_.emplace_back(buf);
    }
    if (cpuPct >= 0.0f) {
        std::snprintf(buf, sizeof(buf), "CPU %.0f%%", cpuPct);
        statsLines_.emplace_back(buf);
    }
    if (gpuMaxMhz > 0) {
        std::snprintf(buf, sizeof(buf), "GPU %ld/%ld MHz", gpuCurMhz, gpuMaxMhz);
    } else {
        std::snprintf(buf, sizeof(buf), "GPU n/a");
    }
    statsLines_.emplace_back(buf);
    if (memTotalKb > 0) {
        std::snprintf(buf, sizeof(buf), "RAM %ld / %ld MB",
                      (memTotalKb - memAvailKb) / 1024, memTotalKb / 1024);
        statsLines_.emplace_back(buf);
    }
}
#endif

int App::run() {
    using namespace std::chrono_literals;

    while (!exitRequested_) {
        if (!xr_.pollEvents(exitRequested_) || exitRequested_) {
            break;
        }

        if (xr_.sessionRunning()) {
            // Compute frame delta-time for continuous seek (F4).
            const auto now = std::chrono::steady_clock::now();
            float dt = 0.0f;
            if (lastFrameTime_.time_since_epoch().count() > 0) {
                dt = std::chrono::duration<float>(now - lastFrameTime_).count();
                dt = std::clamp(dt, 0.0f, 0.1f);  // cap to avoid huge jumps
            }
            lastFrameTime_ = now;

#ifdef PIXELVR_HAVE_MEDIA
            if (mediaOpen_ && media_.drmActive()) {
                // Zero-copy dma-buf path: import the decoder's frame directly.
                if (media_.latestDrmFrame(drmFrame_)) {
                    renderer_.updateVideoDmabuf(drmFrame_);
                }
            } else if (mediaOpen_ && media_.latestFrame(frame_)) {
                renderer_.updateVideoTexture(frame_.width, frame_.height,
                                             frame_.y.data(), frame_.uv.data(),
                                             frame_.colorspace, frame_.fullRange);
            }
#endif
            xr_.renderFrame([this](uint32_t viewIndex, uint32_t imageIndex,
                                   const XrView& view, const ViewSwapchain& swapchain) {
                renderer_.renderView(viewIndex, imageIndex, view, swapchain);
            });

            // Apply controller input that renderFrame synced this frame.
            const XrInputState& in = xr_.input();
            bool interacted = false;
            bool browserOpen = false;
            bool settingsOpen = false;

            // Fetch the runtime-provided controller model once a controller is
            // tracked (one-shot; subsequent calls return immediately).
            if (in.controllers[0].active || in.controllers[1].active) {
                renderModels_.poll();
            }

            // Live settings + place the screen in front on first head tracking.
            renderer_.setSwapEyes(settings_.swapEyes());
            renderer_.setDrawModels(settings_.drawModels());
            if (!didInitialRecenter_ && in.headValid) {
                renderer_.recenterScreen(in.headPos, in.headForward);
                didInitialRecenter_ = true;
            }

            // Controller activity: moving a controller past a small threshold (or
            // any button/stick input) keeps the laser, models and controls shown;
            // after a few seconds of stillness they fade away.
            const auto nowT = std::chrono::steady_clock::now();
            bool activity = false;
            for (int h = 0; h < 2; ++h) {
                const auto& c = in.controllers[h];
                if (!c.active) {
                    activityAnchorValid_[h] = false;
                    continue;
                }
                if (!activityAnchorValid_[h]) {
                    activityAnchorValid_[h] = true;
                    for (int k = 0; k < 3; ++k) activityAnchorPos_[h][k] = c.position[k];
                    for (int k = 0; k < 4; ++k) activityAnchorOrient_[h][k] = c.orientation[k];
                    continue;
                }
                float dp = 0.0f;
                for (int k = 0; k < 3; ++k) {
                    const float d = c.position[k] - activityAnchorPos_[h][k];
                    dp += d * d;
                }
                float od = 0.0f;
                for (int k = 0; k < 4; ++k) od += c.orientation[k] * activityAnchorOrient_[h][k];
                if (std::sqrt(dp) > 0.02f || std::fabs(od) < 0.995f) {
                    activity = true;
                    for (int k = 0; k < 3; ++k) activityAnchorPos_[h][k] = c.position[k];
                    for (int k = 0; k < 4; ++k) activityAnchorOrient_[h][k] = c.orientation[k];
                }
            }
            if (in.togglePlay || in.browserToggle || in.select || in.selectHeld ||
                in.recenter || std::fabs(in.thumbstickX) > 0.15f ||
                std::fabs(in.thumbstickY) > 0.15f ||
                std::fabs(in.thumbstickRX) > 0.15f) {
                activity = true;
            }
            if (activity) lastControllerActivity_ = nowT;
            const bool controllersEngaged =
                (nowT - lastControllerActivity_) < std::chrono::milliseconds(3000);

#ifdef PIXELVR_HAVE_FREETYPE
            if (in.browserToggle) {
                if (settings_.isOpen()) {
                    settings_.close();
                } else if (browser_.isOpen()) {
                    browser_.setOpen(false);
                } else {
                    browser_.open();
                }
            }
            if (browser_.isOpen()) {
                // A finished SMB download auto-plays and closes the browser.
                std::string donePath;
                if (browser_.poll(donePath) == Browser::Result::Play &&
                    !donePath.empty()) {
#ifdef PIXELVR_HAVE_MEDIA
                    loadMedia(donePath);
#endif
                    browser_.setOpen(false);
                }
            }
            // Publish the browser panel first so the pointer can raycast it.
            if (browser_.isOpen()) {
                std::vector<std::string> rows;
                rows.reserve(browser_.entries().size());
                for (const auto& e : browser_.entries()) {
                    rows.push_back(e.label);
                }
                std::string status = browser_.status();
                if (browser_.onNetwork() && !browser_.isBusy()) {
                    status = "Point + Trigger: Stream      A: Download";
                }
                renderer_.setBrowser(true, browser_.title(), rows,
                                     browser_.selected(), status);
                browserOpen = true;
            } else {
                renderer_.setBrowser(false, "", {}, 0, "");
            }
#endif

            // Settings panel overlay
            if (settings_.isOpen()) {
                std::vector<std::string> sRows;
                sRows.reserve(settings_.rows().size());
                for (const auto& r : settings_.rows()) {
                    sRows.push_back(r.label + " : " + r.value);
                }
                renderer_.setSettings(true, settings_.title(), sRows,
                                      settings_.selected(), settings_.status());
                settingsOpen = true;
            } else {
                renderer_.setSettings(false, "", {}, 0, "");
            }

            // Feed controller poses to the renderer (models, pointer ray, cursor);
            // it raycasts the pointer against UI panels and reports hit rows/buttons.
            {
                auto vis = [](const XrInputState::ControllerPose& c) {
                    VulkanRenderer::ControllerVis v;
                    v.active = c.active;
                    for (int k = 0; k < 3; ++k) v.position[k] = c.position[k];
                    for (int k = 0; k < 4; ++k) v.orientation[k] = c.orientation[k];
                    return v;
                };
                const bool uiPanelActive = browserOpen || settingsOpen;
                // Controls (buttons + timeline) show only while a controller is
                // actively moving; they fade after a few seconds of stillness.
                renderer_.setControlBar(!uiPanelActive && controllersEngaged &&
                                        (in.controllers[0].active ||
                                         in.controllers[1].active));
                // Laser + models stay up while a panel is open (to point at it),
                // otherwise they follow the same activity timeout.
                const bool showLaser = uiPanelActive || controllersEngaged;
                auto gated = [&](XrInputState::ControllerPose c) {
                    if (!showLaser) c.active = false;
                    return vis(c);
                };
                renderer_.setControllers(gated(in.controllers[0]),
                                         gated(in.controllers[1]));
            }

#ifdef PIXELVR_HAVE_FREETYPE
            if (browser_.isOpen() && !browser_.isBusy()) {
                if (in.thumbstickY > 0.6f || in.thumbstickY < -0.6f) {
                    if (navArmed_) {
                        const int delta = (in.thumbstickY > 0.0f) ? -1 : 1;
                        browser_.move(delta);
                        renderer_.scrollBrowser(delta);
                        navArmed_ = false;
                    }
                } else if (in.thumbstickY > -0.3f && in.thumbstickY < 0.3f) {
                    navArmed_ = true;
                }

                const int hovered = renderer_.browserHovered();
                if (hovered >= 0) {
                    renderer_.setBrowserSelected(hovered);
                    if (in.select) {
                        browser_.setSelected(hovered);
                        std::string playPath;
                        if (browser_.activate(playPath) == Browser::Result::Play &&
                            !playPath.empty()) {
#ifdef PIXELVR_HAVE_MEDIA
                            loadMedia(playPath);
#endif
                            browser_.setOpen(false);
                        }
                    }
                }
                if (in.togglePlay) {
                    browser_.downloadFocused();
                }
            }
            if (browser_.isOpen() && in.recenter && in.headValid) {
                renderer_.recenterScreen(in.headPos, in.headForward);
            }
#endif

            if (settings_.isOpen()) {
                if (in.thumbstickY > 0.6f || in.thumbstickY < -0.6f) {
                    if (navArmed_) {
                        settings_.move(in.thumbstickY > 0.0f ? -1 : 1);
                        navArmed_ = false;
                    }
                } else if (in.thumbstickY > -0.3f && in.thumbstickY < 0.3f) {
                    navArmed_ = true;
                }

                const int hovered = renderer_.settingsHovered();
                if (hovered >= 0) {
                    renderer_.setSettingsSelected(hovered);
                    if (in.select) {
                        settings_.setSelected(hovered);
                        auto act = settings_.activate();
                        if (act == SettingsMenu::Action::RecenterScreen) {
                            if (in.headValid) {
                                renderer_.recenterScreen(in.headPos, in.headForward);
                            }
                        } else if (act == SettingsMenu::Action::CycleProjection) {
                            int pm = settings_.projectionMode();
                            renderer_.setProjectionMode(
                                pm == 0 ? VulkanRenderer::ProjectionMode::Flat :
                                pm == 1 ? VulkanRenderer::ProjectionMode::Equirect360 :
                                          VulkanRenderer::ProjectionMode::Equirect180);
                        } else if (act == SettingsMenu::Action::CycleStereo) {
                            int sm = settings_.stereoMode();
                            renderer_.setStereoMode(
                                sm == 0 ? VulkanRenderer::StereoMode::Mono :
                                sm == 1 ? VulkanRenderer::StereoMode::SideBySide :
                                          VulkanRenderer::StereoMode::TopBottom);
                        } else if (act == SettingsMenu::Action::CycleDistance) {
                            renderer_.adjustScreenDistance(settings_.screenDistance() - 3.0f);
                        }
                    }
                }
                if (in.recenter && in.headValid) {
                    renderer_.recenterScreen(in.headPos, in.headForward);
                }
            }

            if (!browserOpen && !settingsOpen) {
                // Point-and-click the media control bar with the trigger.
                const int btn = renderer_.pointerButton();
                if (in.select && btn >= 0) {
                    const auto cb = static_cast<VulkanRenderer::CtrlButton>(btn);
                    if (cb == VulkanRenderer::CtrlButton::Stop) {
#ifdef PIXELVR_HAVE_MEDIA
                        media_.close();
                        mediaOpen_ = false;
                        renderer_.clearVideo();
#endif
#ifdef PIXELVR_HAVE_FREETYPE
                        browser_.open();  // large file selection
#endif
                    } else if (cb == VulkanRenderer::CtrlButton::Settings) {
                        settings_.open();
                    } else if (cb == VulkanRenderer::CtrlButton::Recenter) {
                        if (in.headValid) {
                            renderer_.recenterScreen(in.headPos, in.headForward);
                        }
                    }
#ifdef PIXELVR_HAVE_FREETYPE
                    else if (cb == VulkanRenderer::CtrlButton::Files) {
                        browser_.open();
                    }
#endif
#ifdef PIXELVR_HAVE_MEDIA
                    else if (mediaOpen_ &&
                             cb == VulkanRenderer::CtrlButton::PlayPause) {
                        media_.togglePause();
                    } else if (mediaOpen_ &&
                               cb == VulkanRenderer::CtrlButton::SeekBack) {
                        media_.seekRelative(-10.0);
                    } else if (mediaOpen_ &&
                               cb == VulkanRenderer::CtrlButton::SeekFwd) {
                        media_.seekRelative(10.0);
                    }
#endif
                    interacted = true;
                }
                if (in.recenter && in.headValid) {
                    renderer_.recenterScreen(in.headPos, in.headForward);
                    interacted = true;
                }
                if (in.thumbstickY > 0.15f || in.thumbstickY < -0.15f) {
                    renderer_.adjustScreenDistance(in.thumbstickY * 0.04f);
                    interacted = true;
                }
#ifdef PIXELVR_HAVE_MEDIA
                if (in.togglePlay && mediaOpen_) {
                    media_.togglePause();
                    interacted = true;
                }
                if (mediaOpen_) {
                    // Right stick left/right steps the video -/+30s (debounced,
                    // acts as a d-pad jump).
                    constexpr double kStep = 30.0;
                    if (in.thumbstickRX > 0.6f && seekStepArmed_) {
                        media_.seekRelative(kStep);
                        seekStepArmed_ = false;
                        interacted = true;
                    } else if (in.thumbstickRX < -0.6f && seekStepArmed_) {
                        media_.seekRelative(-kStep);
                        seekStepArmed_ = false;
                        interacted = true;
                    } else if (std::fabs(in.thumbstickRX) < 0.3f) {
                        seekStepArmed_ = true;
                    }

                    // Left stick scrubs smoothly; speed grows with deflection as
                    // sign(x) * deadzone(|x|)^2 * maxRate.
                    constexpr float kDeadzone = 0.15f;
                    constexpr float kMaxSeekRate = 60.0f;  // seconds per second
                    const float ax = in.thumbstickLX;
                    const float absx = std::fabs(ax);
                    if (absx > kDeadzone && dt > 0.0f) {
                        const float t = (absx - kDeadzone) / (1.0f - kDeadzone);
                        const float rate = std::copysign(t * t * kMaxSeekRate, ax);
                        media_.seekRelative(static_cast<double>(rate * dt));
                        interacted = true;
                    }

                    // Grab the timeline dot with the trigger and drag to scrub.
                    if (renderer_.pointerOnTimeline() && in.selectHeld &&
                        media_.duration() > 0.0) {
                        media_.seek(renderer_.pointerScrub() * media_.duration());
                        interacted = true;
                    }
                }
                if (interacted) {
                    lastControllerActivity_ = nowT;
                }
                const bool paused = mediaOpen_ && media_.paused();
                const bool controlsUp =
                    mediaOpen_ &&
                    (paused || (controllersEngaged && (in.controllers[0].active ||
                                                       in.controllers[1].active)));
                float progress = 0.0f;
                if (mediaOpen_ && media_.duration() > 0.0) {
                    progress =
                        static_cast<float>(media_.position() / media_.duration());
                }
                renderer_.setTransport(controlsUp, progress, paused);
#else
                (void)interacted;
#endif
            }

#ifdef PIXELVR_HAVE_MEDIA
            if (settings_.showStats()) {
                updateStats();
            }
#endif
            renderer_.setStats(settings_.showStats(), statsLines_);
        } else {
            // Idle until the runtime tells us to start rendering.
            std::this_thread::sleep_for(50ms);
        }
    }

    if (renderer_.device() != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(renderer_.device());
    }
    renderModels_.shutdown();  // destroy XR model handles before the session
    PIXELVR_LOG_INFO("Shutting down");
    return 0;
}

} // namespace pixelvr

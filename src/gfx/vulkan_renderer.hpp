#pragma once

#include "gfx/text_atlas.hpp"
#include "xr/openxr_headers.hpp"

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace pixelvr {

class XrContext;
struct ViewSwapchain;
struct DrmVideoFrame;
struct GltfMesh;

// Owns the Vulkan device (created through OpenXR) and the per-eye render
// resources. For Phase 0/1 it draws a pose-anchored test triangle into each eye
// swapchain image; later phases render the video projection meshes here.
class VulkanRenderer {
public:
    VulkanRenderer() = default;
    ~VulkanRenderer();

    VulkanRenderer(const VulkanRenderer&) = delete;
    VulkanRenderer& operator=(const VulkanRenderer&) = delete;

    // Creates the Vulkan instance, physical device, logical device, and queue
    // via the OpenXR XR_KHR_vulkan_enable2 helpers on `xr`.
    bool createDevice(XrContext& xr);

    // The graphics binding to pass to XrContext::createSession.
    const XrGraphicsBindingVulkan2KHR& graphicsBinding() const { return binding_; }

    // Picks a preferred color format from the runtime-supported list.
    int64_t chooseColorFormat(const std::vector<int64_t>& runtimeFormats) const;

    // Builds the render pass, pipeline, image views, and framebuffers for the
    // provided per-eye swapchains.
    bool createRenderResources(const std::vector<ViewSwapchain>& swapchains);

    // Records and submits the draw for one eye. Invoked by XrContext::renderFrame.
    void renderView(uint32_t viewIndex, uint32_t imageIndex, const XrView& view,
                    const ViewSwapchain& swapchain);

    // Uploads a decoded NV12 frame (luma + interleaved chroma) into the video
    // textures, creating or resizing them as needed. Safe to call once per frame.
    void updateVideoTexture(int width, int height, const uint8_t* y,
                            const uint8_t* uv, int colorspace, bool fullRange);

    // Imports a decoded DMA-BUF frame (drm_prime) as a YCbCr image sampled
    // through a VkSamplerYcbcrConversion, replacing the CPU NV12 upload. Resolves
    // ownership of frame.fd (imports it into Vulkan or closes it) and clears it.
    void updateVideoDmabuf(DrmVideoFrame& frame);
    // Releases the imported dma-buf frame and the decoder's capture-buffer ref
    // while keeping the resolved image on screen; call before the decoder seeks
    // or a new clip opens so the hardware decoder can reclaim its buffers.
    void releaseVideoFrame();
    bool hasVideo() const { return hasVideo_; }
    void clearVideo() { hasVideo_ = false; }

    enum class ProjectionMode { Flat, Equirect360, Equirect180 };
    enum class StereoMode { Mono, SideBySide, TopBottom };
    void setProjectionMode(ProjectionMode mode) { projectionMode_ = mode; }
    void setStereoMode(StereoMode mode) { stereoMode_ = mode; }
    // 360/180 viewpoint: zoom (>1 = content farther) and vertical tilt (radians).
    void adjustSphereZoom(float delta) {
        sphereZoom_ += delta;
        if (sphereZoom_ < 0.5f) sphereZoom_ = 0.5f;
        if (sphereZoom_ > 2.5f) sphereZoom_ = 2.5f;
    }
    void adjustSpherePitch(float delta) {
        spherePitch_ += delta;
        if (spherePitch_ < -0.6f) spherePitch_ = -0.6f;
        if (spherePitch_ > 0.6f) spherePitch_ = 0.6f;
    }
    void setSphereZoom(float z) { sphereZoom_ = z; }
    void setSpherePitch(float p) { spherePitch_ = p; }
    float sphereZoom() const { return sphereZoom_; }
    float spherePitch() const { return spherePitch_; }
    void setSwapEyes(bool on) { swapEyes_ = on; }
    void setDrawModels(bool on) { drawModels_ = on; }

    // Debug statistics overlay (FPS, decode, system load). Drawn head-locked.
    void setStats(bool visible, const std::vector<std::string>& lines);

    // Background download status line, drawn head-locked near the bottom edge so
    // downloads are visible after the browser panel is closed.
    void setDownloadStatus(bool visible, const std::string& text) {
        downloadStatusVisible_ = visible;
        downloadStatusText_ = text;
    }

    // Repositions the flat screen in front of the given head pose (headForward is
    // a world-space direction); resets the viewing distance to the default.
    void recenterScreen(const float headPos[3], const float headForward[3]);
    const float* screenPivot() const { return screenPivot_; }
    const float* screenForward() const { return screenForward_; }
    // Moves the flat screen farther (positive) or nearer (negative), clamped.
    void adjustScreenDistance(float delta);
    // Parametric curvature for cinema screen: 0.0 = flat plane, 1.0 = full cylinder wrap.
    void setScreenCurvature(float c) { screenCurvature_ = c < 0.0f ? 0.0f : (c > 1.0f ? 1.0f : c); }
    float screenCurvature() const { return screenCurvature_; }
    void adjustScreenCurvature(float delta) {
        setScreenCurvature(screenCurvature_ + delta);
    }


    // Transport HUD: progress in [0,1]. Drawn head-locked when visible.
    void setTransport(bool visible, float progress, bool paused,
                      double elapsedSec = 0.0, double durationSec = 0.0) {
        transportVisible_ = visible;
        transportProgress_ = progress;
        transportPaused_ = paused;
        transportElapsed_ = elapsedSec;
        transportDuration_ = durationSec;
    }
    // True when the pointer is over the clickable total/remaining time readout.
    bool pointerOnTimeToggle() const { return pointerOnTimeToggle_; }
    void toggleTimeDisplay() { timeShowRemaining_ = !timeShowRemaining_; }

    struct BrowserItem {
        std::string name;
        std::string path;
        std::string detail;
        std::string tag;
        std::string durationTag;
        std::string sizeTag;
        std::string resolutionTag;
        std::string codecTag;
        std::string previewPath;
        bool isDir = false;
        bool isVideo = false;
        bool isBack = false;
    };

    struct DownloadItem {
        std::string label;
        int state = 0;          // 0=active, 1=done, 2=failed, 3=stopped
        float progress = 0.0f;  // 0.0 to 1.0
        std::string detail;
    };

    // File browser overlay: rich spatial card grid + sidebar navigation
    void setBrowserItems(bool visible, const std::string& title,
                         const std::vector<BrowserItem>& items, int selected,
                         const std::string& status);
    void setBrowser(bool visible, const std::string& title,
                    const std::vector<std::string>& rows, int selected,
                    const std::string& status);
    void updateCardThumbnail(int slot, const std::string& path, const uint8_t* rgba, int width, int height);
    void clearCardThumbnails();
    int browserFirst() const { return browserFirst_; }
    void setBrowserFirst(int first) { browserFirst_ = first < 0 ? 0 : first; }
    int browserSidebarHovered() const { return browserSidebarHovered_; }
    int browserPageBtnHovered() const { return browserPageBtnHovered_; }
    void setBrowserQuickFilter(int f) { browserQuickFilter_ = f; }
    int browserQuickFilter() const { return browserQuickFilter_; }
    void setDownloadsFilter(int f) { downloadsFilter_ = f; }
    int downloadsFilter() const { return downloadsFilter_; }
    void setActiveDownloadCount(int c) { activeDownloadCount_ = c; }
    void setSettingsSection(int s) { settingsSection_ = s; }
    void setVaultUnlocked(bool u) { vaultUnlocked_ = u; }
    bool vaultUnlocked() const { return vaultUnlocked_; }
    void setUiAppearance(float textScale, int textColorIdx, int accentColorIdx,
                         float uiDistance) {
        uiTextScale_ = textScale;
        uiTextColorIdx_ = textColorIdx;
        uiAccentColorIdx_ = accentColorIdx;
        uiDistance_ = uiDistance;
    }

    enum class BrowserViewMode : int {
        BigCards = 0,    // 3x2, 6 per page (Large cards with full 16:9 thumbnail)
        MiniCards = 1,   // 4x3, 12 per page (Compact cards with 16:9 thumbnail)
        CompactList = 2  // 1x8, 8 per page (Single-column detailed configurable list view)
    };
    void setBrowserViewMode(BrowserViewMode mode) { browserViewMode_ = mode; }
    BrowserViewMode browserViewMode() const { return browserViewMode_; }
    void setBrowserQuickPath(bool isCurrentQuickPath) { browserIsQuickPath_ = isCurrentQuickPath; }
    void setBrowserBookmarked(bool isCurrentBookmarked) { browserIsQuickPath_ = isCurrentBookmarked; }
    bool browserIsQuickPath() const { return browserIsQuickPath_; }
    bool browserIsBookmarked() const { return browserIsQuickPath_; }
    void setListColumnVisibility(bool showFormat, bool showRes, bool showSize, bool showDuration) {
        listShowFormat_ = showFormat;
        listShowRes_ = showRes;
        listShowSize_ = showSize;
        listShowDuration_ = showDuration;
    }

    struct KeyboardModalData {
        bool visible = false;
        std::string prompt;
        std::string text;
        bool isPassword = false;
        int cursorPos = 0;
        bool hasTransform = false;
        float transform[3][4]{};
    };
    void setKeyboardModal(bool visible, const std::string& prompt,
                          const std::string& text, bool isPassword = false,
                          const float* transform = nullptr,
                          int cursorPos = -1) {
        keyboardModal_.visible = visible;
        keyboardModal_.prompt = prompt;
        keyboardModal_.text = text;
        keyboardModal_.isPassword = isPassword;
        keyboardModal_.cursorPos = (cursorPos >= 0) ? cursorPos : static_cast<int>(text.size());
        if (transform && visible) {
            std::memcpy(keyboardModal_.transform, transform, sizeof(keyboardModal_.transform));
            keyboardModal_.hasTransform = true;
        } else {
            keyboardModal_.hasTransform = false;
        }
    }
    const KeyboardModalData& keyboardModal() const { return keyboardModal_; }

    void setUiFollowMode(int mode) { uiFollowMode_ = mode; }
    int uiFollowMode() const { return uiFollowMode_; }

    bool pointerActive() const { return pointerActive_; }

    // Downloads overlay: modern glassmorphic window with job cards
    void setDownloads(bool visible, const std::string& title,
                      const std::vector<std::string>& rows,
                      const std::string& status,
                      const std::vector<std::string>& buttons = {},
                      const std::vector<DownloadItem>& jobs = {}) {
        downloadsVisible_ = visible;
        downloadsTitle_ = title;
        downloadsRows_ = rows;
        downloadsStatus_ = status;
        downloadsButtons_ = buttons;  // content-sized buttons along the bottom
        downloadJobs_ = jobs;
    }
    // Draws a stronger modal backdrop behind the panel (for confirmations).
    void setDownloadsModal(bool modal) { downloadsModal_ = modal; }
    // The downloads-window list row / button under the pointer, or -1.
    int downloadsHovered() const { return downloadsHovered_; }
    int downloadsButtonHovered() const { return downloadsBtnHovered_; }

    // File Action Submenu (opened by button Y in file browser)
    struct ActionMenuData {
        bool visible = false;
        int page = 0; // 0: Main, 1: MoveDestinations, 2: DeleteConfirm, 3: Rename
        std::string title;
        std::string subtitle;
        std::vector<std::string> options;
        int selectedOption = -1;
        int editingRow = -1;
        int cursorPos = 0;
        std::string renameBuffer;
    };
    void setActionMenu(const ActionMenuData& data) { actionMenu_ = data; }
    const ActionMenuData& actionMenu() const { return actionMenu_; }
    int actionMenuBtnHovered() const { return actionMenuBtnHovered_; }
    int actionMenuKeyHovered() const { return actionMenuKeyHovered_; }
    void setBrowserNetwork(bool onNet) { browserOnNetwork_ = onNet; }

    // Tracked controllers: aim poses used to draw pointer rays, controller
    // models and the UI cursor. Index 0 = left, 1 = right. Call once per frame,
    // after setBrowser, so the pointer can be raycast against the current panel.
    struct ControllerVis {
        bool active = false;
        float position[3] = {0.0f, 0.0f, 0.0f};
        float orientation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        // Grip pose (controller body) for the render model; the aim pose above
        // drives the pointer ray and UI cursor.
        bool gripActive = false;
        float gripPosition[3] = {0.0f, 0.0f, 0.0f};
        float gripOrientation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    };
    void setControllers(const ControllerVis& left, const ControllerVis& right);

    // Replaces the procedural controller geometry with the runtime-provided glTF
    // render model (parsed from the GLB bytes) for hand (0 = left, 1 = right).
    bool loadControllerModel(int hand, const std::vector<uint8_t>& glb);
    bool loadControllerModel(const std::vector<uint8_t>& glb) {
        return loadControllerModel(1, glb);
    }

    // Per-node animated transforms from xrGetRenderModelStateEXT, matched to
    // submeshes by node name (drives button-press motion on the model).
    struct NodeStateVis {
        std::string name;
        float pos[3] = {0.0f, 0.0f, 0.0f};
        float quat[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        bool visible = true;
    };
    void setControllerNodeStates(int hand, const std::vector<NodeStateVis>& states);
    void setControllerNodeStates(const std::vector<NodeStateVis>& states) {
        setControllerNodeStates(1, states);
    }

    enum class UiShape : int {
        RoundedBox = 0,
        Play = 1,
        Pause = 2,
        SeekBack = 3,
        SeekFwd = 4,
        Files = 5,
        Settings = 6,
        Recenter = 7,
        Cursor = 8,
        Close = 9,
        Checkmark = 10,
        Stop = 11,
        Download = 12,
        List = 13,
        GlowRing = 14,
        Speaker = 15,
        Headset = 16,
        CurvedDisplay = 17,
        Chevron = 18,
        Home = 19,
    };

    // The browser row currently under the pointer, or -1 if none.
    int pointerRow() const { return pointerRow_; }
    int browserHovered() const { return browserHovered_; }
    void scrollBrowser(int deltaRows);

    // Updates just the highlighted browser row (used by the pointer each frame).
    void setBrowserSelected(int index) { browserSelected_ = index; }

    enum class CtrlButton {
        SeekBack = 0,
        PlayPause = 1,
        SeekFwd = 2,
        Stop = 3,
        Files = 4,
        Settings = 5,
        Downloads = 6,  // open the downloads window (list of jobs)
        Download = 7,   // download the focused file-browser entry
        FormatToggle = 8,
        AudioToggle = 9,
        ScreenToggle = 10,
        Recenter = 11,  // recenter screen/view from HUD
        HideHud = 12    // immediately hide player controls
    };
    void setControlBar(bool visible) { controlBarVisible_ = visible; }
    bool controlBarVisible() const { return controlBarVisible_; }
    void setHudLabels(const std::string& format, const std::string& audio,
                      const std::string& screen, bool formatEnabled = true,
                      bool audioEnabled = true) {
        hudFormat_ = format;
        hudAudio_ = audio;
        hudScreen_ = screen;
        hudFormatEnabled_ = formatEnabled;
        hudAudioEnabled_ = audioEnabled;
    }

    // Per-hand laser pointer toggle (grip full press). Only one controller has
    // an active laser pointer at any time. When a hand enables its laser, the
    // other hand's laser is turned off.
    void setLaserOn(int hand, bool on) {
        if (hand >= 0 && hand < 2) {
            laserOn_[hand] = on;
            if (on) laserOn_[1 - hand] = false;
        }
    }
    bool laserOn(int hand) const {
        return (hand >= 0 && hand < 2) ? laserOn_[hand] : false;
    }

    // The control-bar button currently under the pointer, or -1 if none.
    int pointerButton() const { return pointerButton_; }
    // True when the pointer is over the draggable timeline; pointerScrub() is
    // then the target position in [0,1].
    bool pointerOnTimeline() const { return pointerOnTimeline_; }
    float pointerScrub() const { return pointerScrub_; }

    // Badge on the control-bar Downloads button: a count while downloading, or a
    // green tick once all downloads are done (drives the on-button indicator).
    void setDownloadsBadge(bool active, bool allDone, bool failed = false) {
        downloadsActive_ = active;
        downloadsAllDone_ = allDone;
        downloadsFailed_ = failed;
    }

    // Settings overlay panel (re-uses panel & pointer interaction)
    void setSettings(bool visible, const std::string& title,
                     const std::vector<std::string>& rows, int selected,
                     const std::string& status,
                     const std::vector<uint8_t>& enabled = {});
    bool settingsVisible() const { return settingsVisible_; }
    int settingsRow() const { return settingsRow_; }
    int settingsHovered() const { return settingsHovered_; }
    void setSettingsSelected(int index) { settingsSelected_ = index; }

    VkDevice device() const { return device_; }

private:
    bool createRenderPass(VkFormat format);
    VkSampleCountFlagBits chooseSampleCount() const;
    bool createMsaaTarget(int32_t width, int32_t height);
    bool createDepthTarget(int32_t width, int32_t height);
    bool initText();
    bool createSamplerAndDescriptors();
    bool createPipelines();
    bool buildPipeline(const std::string& vertName, const std::string& fragName,
                       VkPipelineLayout layout, VkPipeline& out, bool blend = false,
                       VkRenderPass pass = VK_NULL_HANDLE,
                       VkSampleCountFlagBits samples =
                           VK_SAMPLE_COUNT_FLAG_BITS_MAX_ENUM);
    bool createVideoResources(int width, int height);
    void destroyVideoResources();
    bool createYcbcrPipeline(uint32_t fourcc, int colorspace, bool fullRange);
    void destroyYcbcr();
    bool createResolveTarget(int width, int height);
    void destroyResolveTarget();
    bool createModelPipeline();
    // Content-sized layout for the downloads-window buttons (centered, along the
    // bottom). Fills centre-x and half-width per button.
    void downloadsButtonLayout(std::vector<float>& cx,
                               std::vector<float>& halfW) const;
    bool importDmabufImage(const DrmVideoFrame& frame, VkImage& image,
                           VkDeviceMemory& memory, VkImageView& view,
                           bool& fdConsumed);
    void resolveFrame(VkImage importedImage);
    void updatePointer();
    uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const;
    VkShaderModule loadShaderModule(const std::string& filename);

    struct ViewTargets {
        int32_t width = 0;
        int32_t height = 0;
        std::vector<VkImageView> imageViews;
        std::vector<VkFramebuffer> framebuffers;
    };

    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    uint32_t queueFamilyIndex_ = 0;
    VkQueue queue_ = VK_NULL_HANDLE;

    XrGraphicsBindingVulkan2KHR binding_{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};

    VkFormat colorFormat_ = VK_FORMAT_UNDEFINED;
    VkSampleCountFlagBits sampleCount_ = VK_SAMPLE_COUNT_1_BIT;
    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout spherePipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline spherePipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout uiPipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline uiPipeline_ = VK_NULL_HANDLE;

    ProjectionMode projectionMode_ = ProjectionMode::Flat;
    StereoMode stereoMode_ = StereoMode::Mono;
    bool swapEyes_ = false;
    bool drawModels_ = false;
    float videoAspect_ = 16.0f / 9.0f;

    bool statsVisible_ = false;
    std::vector<std::string> statsLines_;
    bool downloadStatusVisible_ = false;
    std::string downloadStatusText_;

    // Downloads overlay (list of jobs).
    bool downloadsVisible_ = false;
    std::string downloadsTitle_;
    std::vector<std::string> downloadsRows_;
    std::string downloadsStatus_;
    bool downloadsActive_ = false;
    bool downloadsAllDone_ = false;
    bool downloadsFailed_ = false;
    bool downloadsModal_ = false;
    std::vector<std::string> downloadsButtons_;
    std::vector<DownloadItem> downloadJobs_;
    int downloadsBtnHovered_ = -1;
    int downloadsHovered_ = -1;

    // Action menu state
    ActionMenuData actionMenu_{};
    int actionMenuBtnHovered_ = -1;
    int actionMenuKeyHovered_ = -1;
    bool browserOnNetwork_ = false;

    // Flat-screen placement in the app reference space (updated by recenter and
    // thumbstick distance control).
    float screenPivot_[3] = {0.0f, 0.0f, 0.0f};
    float screenForward_[3] = {0.0f, 0.0f, -1.0f};
    float screenYaw_ = 0.0f;
    float recenterPitch_ = 0.0f;  // head pitch at recenter (screen/controls tilt)
    float sphereYaw_ = 0.0f;  // recenter longitude offset for 360/180 content
    float spherePitch_ = 0.0f;  // 360/180 viewpoint tilt (radians)
    float sphereZoom_ = 1.0f;   // 360/180 FOV scale (>1 pushes content farther)
    float screenDistance_ = 3.0f;
    float screenCurvature_ = 0.0f;  // 0.0 = flat plane (default), 1.0 = deep cinema curve

    // Transport HUD state.
    bool transportVisible_ = false;
    float transportProgress_ = 0.0f;
    bool transportPaused_ = false;
    double transportElapsed_ = 0.0;   // seconds played
    double transportDuration_ = 0.0;  // seconds total
    bool timeShowRemaining_ = false;  // right readout shows time-left vs. total
    bool pointerOnTimeToggle_ = false;
    std::string hudFormat_ = "2D FLAT";
    std::string hudAudio_ = "AUDIO";
    std::string hudScreen_ = "CURVED";
    bool hudFormatEnabled_ = true;
    bool hudAudioEnabled_ = true;
    float sphereFollowYaw_ = 0.0f;
    bool sphereFollowInit_ = false;

    struct CardThumbnail {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
        std::string path;
        bool ready = false;
    };
    CardThumbnail cardThumbs_[12];
    VkPipeline thumbPipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool thumbPool_ = VK_NULL_HANDLE;
    BrowserViewMode browserViewMode_ = BrowserViewMode::BigCards;
    bool browserIsQuickPath_ = false;
    KeyboardModalData keyboardModal_{};
    int uiFollowMode_ = 0; // 0 = World Lock (Default, zero drift/tilt), 1 = Smooth Gaze Follow
    bool listShowFormat_ = true;
    bool listShowRes_ = true;
    bool listShowSize_ = true;
    bool listShowDuration_ = true;

    // Text rendering (FreeType glyph atlas) + file browser overlay.
    FontAtlas fontAtlas_;
    VkImage fontImage_ = VK_NULL_HANDLE;
    VkDeviceMemory fontMemory_ = VK_NULL_HANDLE;
    VkImageView fontView_ = VK_NULL_HANDLE;
    VkSampler fontSampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout textSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool textPool_ = VK_NULL_HANDLE;
    VkDescriptorSet textSet_ = VK_NULL_HANDLE;
    VkPipelineLayout textPipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline textPipeline_ = VK_NULL_HANDLE;
    bool textReady_ = false;

    bool browserVisible_ = false;
    std::string browserTitle_;
    std::string browserStatus_;
    std::vector<std::string> browserRows_;
    int browserSelected_ = 0;
    int browserHovered_ = -1;  // pointer hover target (independent of scroll)
    int browserFirst_ = 0;     // scroll offset for visible window
    std::vector<BrowserItem> browserItems_;
    int browserSidebarHovered_ = -1;
    int browserPageBtnHovered_ = -1;
    int browserQuickFilter_ = 0;  // 0=Local, 1=Videos, 2=SMB, 3=Downloads, 4=Bookmarks, 5=Vault
    int downloadsFilter_ = 0;  // 0=All, 1=Active, 2=Completed
    int activeDownloadCount_ = 0;
    int settingsSection_ = 0;
    float uiTextScale_ = 1.20f;
    int uiTextColorIdx_ = 0;
    int uiAccentColorIdx_ = 0;
    float uiDistance_ = 1.9f;

    // Settings panel overlay
    bool settingsVisible_ = false;
    std::string settingsTitle_;
    std::string settingsStatus_;
    std::vector<std::string> settingsRows_;
    std::vector<uint8_t> settingsEnabled_;  // per-row: 0 = greyed/disabled
    int settingsSelected_ = 0;
    int settingsHovered_ = -1;
    int settingsRow_ = -1;

    // Tracked controllers + pointer (raycast against UI panels).
    ControllerVis controllers_[2];
    bool pointerActive_ = false;
    int pointerRow_ = -1;
    int pointerHand_ = -1;
    // Whether each hand's laser pointer is on. Only one controller has an active
    // laser pointer at any time. Defaults to right controller active, left off.
    bool laserOn_[2] = {false, true};
    float pointerHit_[3] = {0.0f, 0.0f, 0.0f};
    bool controlBarVisible_ = false;
    bool vaultUnlocked_ = false;
    int pointerButton_ = -1;
    bool pointerOnTimeline_ = false;
    float pointerScrub_ = 0.0f;

    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;

    // Video texture sampling: descriptor set + sampler shared by both eyes.
    VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;

    // NV12 video textures (luma R8 full-res + chroma RG8 half-res) and staging.
    VkImage lumaImage_ = VK_NULL_HANDLE;
    VkDeviceMemory lumaMemory_ = VK_NULL_HANDLE;
    VkImageView lumaView_ = VK_NULL_HANDLE;
    VkImage chromaImage_ = VK_NULL_HANDLE;
    VkDeviceMemory chromaMemory_ = VK_NULL_HANDLE;
    VkImageView chromaView_ = VK_NULL_HANDLE;
    int videoWidth_ = 0;
    int videoHeight_ = 0;
    uint32_t lumaMipLevels_ = 1;
    uint32_t chromaMipLevels_ = 1;
    bool videoLayoutInitialized_ = false;
    bool hasVideo_ = false;

    // YUV->RGB coefficients + luma range/scale, derived from the frame metadata.
    float conversion_[4] = {1.402f, -0.344f, -0.714f, 1.772f};
    float range_[4] = {16.0f / 255.0f, 255.0f / 219.0f, 1.0f, 0.0f};

    VkBuffer stagingBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory_ = VK_NULL_HANDLE;
    VkDeviceSize stagingSize_ = 0;
    void* stagingMapped_ = nullptr;

    VkCommandBuffer uploadCmd_ = VK_NULL_HANDLE;
    VkFence uploadFence_ = VK_NULL_HANDLE;

    std::vector<ViewTargets> viewTargets_;

    // Shared multisampled color target, resolved into each eye's swapchain image.
    VkImage msaaImage_ = VK_NULL_HANDLE;
    VkDeviceMemory msaaMemory_ = VK_NULL_HANDLE;
    VkImageView msaaView_ = VK_NULL_HANDLE;

    // Shared depth buffer, reused across eyes (they render sequentially). Gives
    // the 3D controller models correct self-occlusion instead of showing through.
    VkImage depthImage_ = VK_NULL_HANDLE;
    VkDeviceMemory depthMemory_ = VK_NULL_HANDLE;
    VkImageView depthView_ = VK_NULL_HANDLE;
    VkFormat depthFormat_ = VK_FORMAT_D32_SFLOAT;

    float clearColor_[4] = {0.02f, 0.02f, 0.05f, 1.0f};
    bool dmabufCapable_ = false;
    bool queueFamilyForeign_ = false;
    PFN_vkGetMemoryFdPropertiesKHR getMemoryFdProperties_ = nullptr;

    // Zero-copy dma-buf video: a YCbCr conversion + immutable sampler baked into
    // its own descriptor set layout and pipelines, rebuilt when the frame format
    // or colour metadata changes. The imported image is single-buffered and
    // retired on the next import (renderView has waited on its fence by then).
    VkSamplerYcbcrConversion ycbcrConversion_ = VK_NULL_HANDLE;
    VkSampler ycbcrSampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout ycbcrSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool ycbcrPool_ = VK_NULL_HANDLE;
    VkDescriptorSet ycbcrSet_ = VK_NULL_HANDLE;
    VkPipelineLayout ycbcrQuadLayout_ = VK_NULL_HANDLE;
    VkPipeline ycbcrQuadPipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout ycbcrSphereLayout_ = VK_NULL_HANDLE;
    VkPipeline ycbcrSpherePipeline_ = VK_NULL_HANDLE;
    VkFormat ycbcrFormat_ = VK_FORMAT_UNDEFINED;
    uint32_t ycbcrKeyFourcc_ = 0;
    int ycbcrKeyColorspace_ = -1;
    bool ycbcrKeyFullRange_ = false;
    VkImage drmImage_ = VK_NULL_HANDLE;
    VkDeviceMemory drmMemory_ = VK_NULL_HANDLE;
    VkImageView drmView_ = VK_NULL_HANDLE;
    std::shared_ptr<void> drmKeepAlive_;  // decoder frame ref for the live image
    bool usingDmabuf_ = false;
    float drmUvScaleX_ = 1.0f;  // visible/coded, applied to UVs to drop padding
    float drmUvScaleY_ = 1.0f;

    // Minification mip chain: the imported YCbCr frame is resolved into this RGBA
    // image (with mipmaps) each frame, then sampled trilinearly by the video
    // pipelines so 4K content doesn't shimmer when shown on a small screen.
    VkRenderPass resolvePass_ = VK_NULL_HANDLE;
    VkPipelineLayout resolveLayout_ = VK_NULL_HANDLE;
    VkPipeline resolvePipeline_ = VK_NULL_HANDLE;
    VkSampler resolveSampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout resolveSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool resolvePool_ = VK_NULL_HANDLE;
    VkDescriptorSet resolveSet_ = VK_NULL_HANDLE;
    VkImage resolveImage_ = VK_NULL_HANDLE;
    VkDeviceMemory resolveMemory_ = VK_NULL_HANDLE;
    VkImageView resolveView_ = VK_NULL_HANDLE;        // all mips (sampled)
    VkImageView resolveTargetView_ = VK_NULL_HANDLE;  // mip 0 (render target)
    VkFramebuffer resolveFbo_ = VK_NULL_HANDLE;
    int resolveW_ = 0;
    int resolveH_ = 0;
    uint32_t resolveMips_ = 1;

    // Lit, textured 3D controller render models. The glTF geometry is loaded
    // separately for left (0) and right (1) hands into handModels_[2].
    // Drawn with the depth buffer so near parts occlude far ones.
    VkPipelineLayout modelLayout_ = VK_NULL_HANDLE;
    VkPipeline modelPipeline_ = VK_NULL_HANDLE;

    // Procedural capsule fallback buffers (used before glTF is fetched)
    VkBuffer capsuleVertexBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory capsuleVertexMemory_ = VK_NULL_HANDLE;
    VkBuffer capsuleIndexBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory capsuleIndexMemory_ = VK_NULL_HANDLE;
    uint32_t capsuleIndexCount_ = 0;

    struct ModelTexture {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
    };
    struct ModelSubmesh {
        uint32_t firstIndex = 0;
        uint32_t indexCount = 0;
        VkDescriptorSet set = VK_NULL_HANDLE;  // base-color texture descriptor
        float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        std::string nodeName;  // matched to an animated render-model node state
    };
    struct HandModel {
        VkBuffer vertexBuffer = VK_NULL_HANDLE;
        VkDeviceMemory vertexMemory = VK_NULL_HANDLE;
        VkBuffer indexBuffer = VK_NULL_HANDLE;
        VkDeviceMemory indexMemory = VK_NULL_HANDLE;
        uint32_t indexCount = 0;
        VkIndexType indexType = VK_INDEX_TYPE_UINT32;
        std::vector<ModelTexture> textures;
        std::vector<ModelSubmesh> submeshes;
        std::vector<NodeStateVis> nodeStates;
        bool ready = false;
    };
    HandModel handModels_[2];  // 0 = left, 1 = right

    VkDescriptorSetLayout modelSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool modelDescPool_ = VK_NULL_HANDLE;
    VkSampler modelSampler_ = VK_NULL_HANDLE;
    ModelTexture modelWhite_;  // 1x1 white fallback for untextured submeshes

    void destroyHandModel(HandModel& hm);
    bool uploadHandMesh(int hand, const GltfMesh& mesh);
    bool createModelTexture(const uint8_t* rgba, int width, int height,
                            ModelTexture& out);
};

} // namespace pixelvr

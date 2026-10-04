#pragma once

#include "gfx/text_atlas.hpp"
#include "xr/openxr_headers.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pixelvr {

class XrContext;
struct ViewSwapchain;
struct DrmVideoFrame;

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
    bool hasVideo() const { return hasVideo_; }
    void clearVideo() { hasVideo_ = false; }

    enum class ProjectionMode { Flat, Equirect360, Equirect180 };
    enum class StereoMode { Mono, SideBySide, TopBottom };
    void setProjectionMode(ProjectionMode mode) { projectionMode_ = mode; }
    void setStereoMode(StereoMode mode) { stereoMode_ = mode; }
    void setSwapEyes(bool on) { swapEyes_ = on; }
    void setDrawModels(bool on) { drawModels_ = on; }

    // Debug statistics overlay (FPS, decode, system load). Drawn head-locked.
    void setStats(bool visible, const std::vector<std::string>& lines);

    // Repositions the flat screen in front of the given head pose (headForward is
    // a world-space direction); resets the viewing distance to the default.
    void recenterScreen(const float headPos[3], const float headForward[3]);
    // Moves the flat screen farther (positive) or nearer (negative), clamped.
    void adjustScreenDistance(float delta);

    // Transport HUD: progress in [0,1]. Drawn head-locked when visible.
    void setTransport(bool visible, float progress, bool paused) {
        transportVisible_ = visible;
        transportProgress_ = progress;
        transportPaused_ = paused;
    }

    // File browser overlay: a title, list rows, selected index and a status line.
    void setBrowser(bool visible, const std::string& title,
                    const std::vector<std::string>& rows, int selected,
                    const std::string& status);

    // Tracked controllers: aim poses used to draw pointer rays, controller
    // models and the UI cursor. Index 0 = left, 1 = right. Call once per frame,
    // after setBrowser, so the pointer can be raycast against the current panel.
    struct ControllerVis {
        bool active = false;
        float position[3] = {0.0f, 0.0f, 0.0f};
        float orientation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    };
    void setControllers(const ControllerVis& left, const ControllerVis& right);

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
    };

    // The browser row currently under the pointer, or -1 if none.
    int pointerRow() const { return pointerRow_; }
    int browserHovered() const { return browserHovered_; }
    int browserFirst() const { return browserFirst_; }
    void scrollBrowser(int deltaRows);

    // Updates just the highlighted browser row (used by the pointer each frame).
    void setBrowserSelected(int index) { browserSelected_ = index; }

    // Media control bar shown below the video (point-and-click transport). The
    // buttons are: seek back, play/pause, seek forward, open files, settings, recenter.
    enum class CtrlButton {
        SeekBack = 0,
        PlayPause = 1,
        SeekFwd = 2,
        Stop = 3,
        Files = 4,
        Settings = 5,
        Recenter = 6
    };
    void setControlBar(bool visible) { controlBarVisible_ = visible; }

    // The control-bar button currently under the pointer, or -1 if none.
    int pointerButton() const { return pointerButton_; }
    // True when the pointer is over the draggable timeline; pointerScrub() is
    // then the target position in [0,1].
    bool pointerOnTimeline() const { return pointerOnTimeline_; }
    float pointerScrub() const { return pointerScrub_; }

    // Settings overlay panel (re-uses panel & pointer interaction)
    void setSettings(bool visible, const std::string& title,
                     const std::vector<std::string>& rows, int selected,
                     const std::string& status);
    bool settingsVisible() const { return settingsVisible_; }
    int settingsRow() const { return settingsRow_; }
    int settingsHovered() const { return settingsHovered_; }
    void setSettingsSelected(int index) { settingsSelected_ = index; }

    VkDevice device() const { return device_; }

private:
    bool createRenderPass(VkFormat format);
    VkSampleCountFlagBits chooseSampleCount() const;
    bool createMsaaTarget(int32_t width, int32_t height);
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

    // Flat-screen placement in the app reference space (updated by recenter and
    // thumbstick distance control).
    float screenPivot_[3] = {0.0f, 0.0f, 0.0f};
    float screenForward_[3] = {0.0f, 0.0f, -1.0f};
    float screenYaw_ = 0.0f;
    float screenDistance_ = 3.0f;

    // Transport HUD state.
    bool transportVisible_ = false;
    float transportProgress_ = 0.0f;
    bool transportPaused_ = false;

    // Text rendering (FreeType glyph atlas) + file browser overlay.
    FontAtlas fontAtlas_;
    VkImage fontImage_ = VK_NULL_HANDLE;
    VkDeviceMemory fontMemory_ = VK_NULL_HANDLE;
    VkImageView fontView_ = VK_NULL_HANDLE;
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

    // Settings panel overlay
    bool settingsVisible_ = false;
    std::string settingsTitle_;
    std::string settingsStatus_;
    std::vector<std::string> settingsRows_;
    int settingsSelected_ = 0;
    int settingsHovered_ = -1;
    int settingsRow_ = -1;

    // Tracked controllers + pointer (raycast against UI panels).
    ControllerVis controllers_[2];
    bool pointerActive_ = false;
    int pointerRow_ = -1;
    int pointerHand_ = -1;
    float pointerHit_[3] = {0.0f, 0.0f, 0.0f};
    bool controlBarVisible_ = false;
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

    // Lit 3D controller models (procedural capsule geometry, back-face culled so
    // no depth buffer is required). Drawn at each tracked controller pose.
    VkPipelineLayout modelLayout_ = VK_NULL_HANDLE;
    VkPipeline modelPipeline_ = VK_NULL_HANDLE;
    VkBuffer modelVertexBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory modelVertexMemory_ = VK_NULL_HANDLE;
    VkBuffer modelIndexBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory modelIndexMemory_ = VK_NULL_HANDLE;
    uint32_t modelIndexCount_ = 0;
};

} // namespace pixelvr

#pragma once

#include "xr/openxr_headers.hpp"

#include <cstdint>
#include <functional>
#include <vector>

namespace pixelvr {

// A per-eye swapchain of color images provided by the OpenXR runtime.
struct ViewSwapchain {
    XrSwapchain handle = XR_NULL_HANDLE;
    int32_t width = 0;
    int32_t height = 0;
    int64_t format = 0;
    std::vector<XrSwapchainImageVulkan2KHR> images;
};

// Per-frame controller input. Boolean fields are rising-edge (true only on the
// frame the button was pressed). Head pose is in the app reference space.
struct XrInputState {
    bool togglePlay = false;   // A / X face button: play/pause
    bool browserToggle = false; // B / Y face button: open/close browser (menu)
    bool select = false;       // trigger: rising edge (pointer click)
    bool selectHeld = false;   // trigger: currently held (dragging the timeline)
    bool recenter = false;     // grip squeeze: recenter the screen
    float thumbstickX = 0.0f;  // combined (either hand)
    float thumbstickY = 0.0f;
    float thumbstickLX = 0.0f; // left controller stick
    float thumbstickLY = 0.0f;
    float thumbstickRX = 0.0f; // right controller stick
    float thumbstickRY = 0.0f;
    bool headValid = false;
    float headPos[3] = {0.0f, 0.0f, 0.0f};
    float headForward[3] = {0.0f, 0.0f, -1.0f};

    // Tracked controller aim poses (index 0 = left, 1 = right). The pointer ray
    // is the -Z axis of `orientation` from `position`, in the app reference space.
    struct ControllerPose {
        bool active = false;
        float position[3] = {0.0f, 0.0f, 0.0f};
        float orientation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    };
    ControllerPose controllers[2];
};

// Owns the OpenXR instance, system, session, reference space, and swapchains,
// and drives the frame loop. Vulkan objects are created through the OpenXR
// XR_KHR_vulkan_enable2 helpers exposed here, then handed back via createSession.
class XrContext {
public:
    XrContext() = default;
    ~XrContext();

    XrContext(const XrContext&) = delete;
    XrContext& operator=(const XrContext&) = delete;

    // --- Setup (call in order) -------------------------------------------
    bool createInstanceAndSystem(const char* appName);

    // Vulkan creation routed through OpenXR (required by the spec).
    bool vulkanGraphicsRequirements(XrGraphicsRequirementsVulkan2KHR& out);
    bool createVulkanInstance(const VkInstanceCreateInfo& createInfo,
                              VkInstance& outInstance, VkResult& outVkResult);
    bool vulkanPhysicalDevice(VkInstance instance, VkPhysicalDevice& out);
    bool createVulkanDevice(VkPhysicalDevice physicalDevice,
                            const VkDeviceCreateInfo& createInfo,
                            VkDevice& outDevice, VkResult& outVkResult);

    bool createSession(const XrGraphicsBindingVulkan2KHR& binding);
    bool createReferenceSpace();
    std::vector<int64_t> enumerateSwapchainFormats();
    bool createSwapchains(int64_t colorFormat);

    // Controller input (OpenXR actions). Call createActions after createSession.
    bool createActions();
    const XrInputState& input() const { return input_; }

    // --- Runtime ---------------------------------------------------------
    // Pumps the event queue and advances the session state machine.
    // Sets exitRequested when the runtime asks the app to quit.
    bool pollEvents(bool& exitRequested);
    bool sessionRunning() const { return sessionRunning_; }

    // Callback invoked once per view when the frame should be rendered.
    using RenderViewFn = std::function<void(uint32_t viewIndex, uint32_t imageIndex,
                                            const XrView& view,
                                            const ViewSwapchain& swapchain)>;

    // Runs xrWaitFrame / xrBeginFrame, locates the eye views, invokes
    // renderView per eye, and submits the stereo projection layer via xrEndFrame.
    bool renderFrame(const RenderViewFn& renderView);

    // --- Accessors -------------------------------------------------------
    XrInstance instance() const { return instance_; }
    XrSession session() const { return session_; }
    XrSystemId systemId() const { return systemId_; }
    bool renderModelEnabled() const { return renderModelEnabled_; }
    uint32_t viewCount() const { return static_cast<uint32_t>(configViews_.size()); }
    const std::vector<XrViewConfigurationView>& configViews() const { return configViews_; }
    const std::vector<ViewSwapchain>& swapchains() const { return swapchains_; }

private:
    bool loadVulkanEnable2Functions();
    bool locateViews(XrTime displayTime, std::vector<XrView>& views, bool& shouldRender);
    void syncActions();
    void handleSessionStateChange(const XrEventDataSessionStateChanged& ev,
                                  bool& exitRequested);

    static constexpr XrViewConfigurationType kViewConfigType =
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    static constexpr XrReferenceSpaceType kReferenceSpaceType =
        XR_REFERENCE_SPACE_TYPE_LOCAL;

    XrInstance instance_ = XR_NULL_HANDLE;
    XrSystemId systemId_ = XR_NULL_SYSTEM_ID;
    XrSession session_ = XR_NULL_HANDLE;
    XrSpace appSpace_ = XR_NULL_HANDLE;

    XrSessionState sessionState_ = XR_SESSION_STATE_UNKNOWN;
    bool sessionRunning_ = false;
    bool renderModelEnabled_ = false;

    std::vector<XrViewConfigurationView> configViews_;
    std::vector<ViewSwapchain> swapchains_;
    std::vector<XrView> views_;

    // Controller input.
    XrActionSet actionSet_ = XR_NULL_HANDLE;
    XrAction togglePlayAction_ = XR_NULL_HANDLE;
    XrAction recenterAction_ = XR_NULL_HANDLE;
    XrAction browserAction_ = XR_NULL_HANDLE;
    XrAction selectAction_ = XR_NULL_HANDLE;
    XrAction thumbstickAction_ = XR_NULL_HANDLE;
    XrAction thumbstickLeftAction_ = XR_NULL_HANDLE;
    XrAction thumbstickRightAction_ = XR_NULL_HANDLE;
    XrAction aimPoseAction_ = XR_NULL_HANDLE;
    XrPath handPaths_[2] = {XR_NULL_PATH, XR_NULL_PATH};
    XrSpace aimSpaces_[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
    bool actionsReady_ = false;
    XrInputState input_;

    // XR_KHR_vulkan_enable2 entry points (resolved at runtime).
    PFN_xrGetVulkanGraphicsRequirements2KHR pfnGetVulkanGraphicsRequirements2_ = nullptr;
    PFN_xrCreateVulkanInstanceKHR pfnCreateVulkanInstance_ = nullptr;
    PFN_xrGetVulkanGraphicsDevice2KHR pfnGetVulkanGraphicsDevice2_ = nullptr;
    PFN_xrCreateVulkanDeviceKHR pfnCreateVulkanDevice_ = nullptr;
};

} // namespace pixelvr

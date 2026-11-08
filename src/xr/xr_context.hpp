#pragma once

#include "xr/openxr_headers.hpp"

#include <cstdint>
#include <functional>
#include <string>
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

// One composition layer (UI panel, video / browser screen, pointer cursor) the
// renderer wants composited over the projection layer this frame. The pose is
// in the app reference space; a cylinder is used when `cylinder` is set and
// the runtime supports it, otherwise a flat quad.
struct CompositionLayer {
    XrSwapchain swapchain = XR_NULL_HANDLE;
    bool cylinder = false;
    bool blend = false;         // composite with the texture's alpha (else opaque)
    XrEyeVisibility eye = XR_EYE_VISIBILITY_BOTH;
    XrPosef pose{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
    float width = 1.0f;         // quad size (metres)
    float height = 1.0f;
    float radius = 1.0f;        // cylinder parameters
    float centralAngle = 1.0f;
    float aspectRatio = 1.0f;
    XrRect2Di imageRect{{0, 0}, {0, 0}};
    // A further stereo projection layer (the controller models over the
    // screens): `projSwapchain` holds one swapchain per eye, each showing
    // `imageRect`, rendered with this frame's located views. The pose / size
    // fields above are unused. Always premultiplied-alpha blended.
    bool projection = false;
    XrSwapchain projSwapchain[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
};

// The player's OpenXR actions that SteamVR shows and binds (one set, "Player
// Controls"). SteamVR is the only place buttons are assigned; the app only
// reads these. Order = the Controls page order.
enum class InputAction : int {
    Select, PlayOk, Back, MainMenu, Options, Recenter, ToggleUi, Previous, Next,
    DpadUp, DpadDown, DpadLeft, DpadRight, StickRight, StickLeft, ShowControls, LaserHand,
    Mute, Stop, Bookmarks, NextScreen, PreviousScreen, SkipBack, SkipForward,
    LockScreens, AudioTrack, Format3d,
    Count
};

// Per-frame controller input. Boolean fields are rising-edge (true only on the
// frame the action was pressed) unless named *Held. Head pose is in the app
// reference space. The meanings are the actions' (see InputAction), whatever
// buttons SteamVR binds them to.
struct XrInputState {
    bool select = false;       // trigger: rising edge (pointer click)
    bool selectHeld = false;   // trigger: currently held (dragging the timeline)
    bool togglePlay = false;   // "Play / OK"
    bool back = false;         // "Back": one step back
    bool mainMenu = false;     // "Main Menu": open / close the hub
    bool toggleUi = false;     // "Show / Hide UI"
    bool options = false;      // "Options": File Actions
    bool recenter = false;     // "Recenter"
    bool previous = false;     // "Previous": rail tab / track
    bool next = false;         // "Next"
    bool dpadUp = false, dpadDown = false, dpadLeft = false, dpadRight = false;
    bool dpadUpHeld = false, dpadDownHeld = false, dpadLeftHeld = false, dpadRightHeld = false;
    bool showControls = false; // "Show Controls": the Controls page
    // Actions with no default binding (SteamVR chords / long presses).
    bool mute = false, stop = false, bookmarks = false, nextScreen = false,
         previousScreen = false, skipBack = false, skipForward = false,
         lockScreens = false, audioTrack = false, format3d = false;
    // Combo symbol of the face action pressed this frame, else -1. Same ids the
    // stored unlock combo uses, so saved combos keep working on the defaults.
    // App code never clears it (other consumers clear the actions above).
    enum ComboSymbol { ComboPlay = 0, ComboBack = 1, ComboRecenter = 2, ComboOptions = 3 };
    int comboEdge = -1;
    float thumbstickX = 0.0f;  // combined: the longer of the two sticks
    float thumbstickY = 0.0f;
    float thumbstickLX = 0.0f; // left controller stick ("Skip / Distance")
    float thumbstickLY = 0.0f;
    float thumbstickRX = 0.0f; // right controller stick ("Scrub / Distance")
    float thumbstickRY = 0.0f;

    // Consumes every button action this frame (not select, sticks, grip or
    // comboEdge), e.g. while the keyboard or a grab owns the input.
    void clearButtons() {
        togglePlay = back = mainMenu = toggleUi = options = recenter = previous = next = false;
        dpadUp = dpadDown = dpadLeft = dpadRight = false;
        dpadUpHeld = dpadDownHeld = dpadLeftHeld = dpadRightHeld = false;
        showControls = mute = stop = bookmarks = nextScreen = previousScreen = false;
        skipBack = skipForward = lockScreens = audioTrack = format3d = false;
    }
    bool anyButton() const {
        return togglePlay || back || mainMenu || toggleUi || options || recenter || previous ||
               next || dpadUp || dpadDown || dpadLeft || dpadRight || showControls || mute ||
               stop || bookmarks || nextScreen || previousScreen || skipBack || skipForward ||
               lockScreens || audioTrack || format3d;
    }
    bool headValid = false;
    float headPos[3] = {0.0f, 0.0f, 0.0f};
    float headForward[3] = {0.0f, 0.0f, -1.0f};
    bool stageValid = false;
    XrPosef appToStagePose{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 1.65f, 0.0f}};
    float headPosStage[3] = {0.0f, 1.65f, 0.0f};
    float headForwardStage[3] = {0.0f, 0.0f, -1.0f};

    // Tracked controller aim poses (index 0 = left, 1 = right). The pointer ray
    // is the -Z axis of `orientation` from `position`, in the app reference space.
    struct ControllerPose {
        bool active = false;
        float position[3] = {0.0f, 0.0f, 0.0f};
        float orientation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        // Grip pose (the controller body); used to place the 3D render model.
        // The aim pose above drives the pointer ray.
        bool gripActive = false;
        float gripPosition[3] = {0.0f, 0.0f, 0.0f};
        float gripOrientation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    };
    ControllerPose controllers[2];

    // "Laser Hand": full grip press per hand (Frame squeeze click, else the
    // squeeze value past a threshold). Moves the laser to that hand.
    // Index 0 = left, 1 = right.
    bool gripFullPressHeld[2] = {false, false};
    bool gripFullPressDown[2] = {false, false};
    float gripValue[2] = {0.0f, 0.0f};  // analog soft squeeze value (0.0 to 1.0)
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
    bool createSwapchains(int64_t colorFormat, float scaleFactor = 1.25f);
    // Off-screen swapchains for composition layers (UI panel, video / browser
    // screens), so the compositor samples them once at display resolution
    // instead of their being rasterised into the eye buffers and resampled
    // again. Created swapchains are destroyed with the context (or explicitly).
    bool createLayerSwapchain(int64_t colorFormat, uint32_t width, uint32_t height,
                              ViewSwapchain& out);
    void destroyLayerSwapchain(ViewSwapchain& sc);
    // Acquire (and wait for) the next image of a layer swapchain; the renderer
    // draws into it and calls releaseLayerImage once the work is submitted.
    bool acquireLayerImage(const ViewSwapchain& sc, uint32_t& imageIndex);
    void releaseLayerImage(const ViewSwapchain& sc);
    bool cylinderLayerSupported() const { return cylinderLayerEnabled_; }
    // False once the runtime rejected a frame with a second projection layer
    // (CompositionLayer::projection); the renderer then keeps that content in
    // the main projection layer.
    bool extraProjectionLayersOk() const { return extraProjectionOk_; }

    // Controller input (OpenXR actions). Call createActions after createSession.
    bool createActions();
    const XrInputState& input() const { return input_; }
    // The label SteamVR shows for an action ("Play / OK").
    static const char* actionLabel(InputAction a);
    // The buttons SteamVR currently binds to an action, e.g. "Right A", joined
    // with ", "; "Not bound", "Left controller off" / "Right controller off" when
    // the hand(s) of its default are inactive (SteamVR lists active sources only),
    // or "See SteamVR bindings" when the runtime can't say. Logs each change.
    std::string describeBinding(InputAction a);
    // The source paths SteamVR binds to an action right now, e.g.
    // "/user/hand/right/input/a/click" (Laser Hand includes its analog
    // fallback); empty when unbound, off or unknown. Quiet (no logging).
    std::vector<std::string> boundSourcePaths(InputAction a);
    // The runtime has an interaction profile bound for this hand (0 left, 1 right).
    bool handActive(int hand) const;
    // Bumped after a focused sync once focus or the interaction profile changed
    // (bindings may have been edited in the dashboard meanwhile).
    uint32_t bindingsGeneration() const { return bindingsGeneration_; }
    void triggerHaptic(int hand, float durationSec = 0.015f, float frequencyHz = 0.0f, float amplitude = 0.5f);

    // --- Runtime ---------------------------------------------------------
    // Pumps the event queue and advances the session state machine.
    // Sets exitRequested when the runtime asks the app to quit.
    bool pollEvents(bool& exitRequested);
    bool sessionRunning() const { return sessionRunning_; }
    bool sessionFocused() const { return sessionState_ == XR_SESSION_STATE_FOCUSED; }

    // Callback invoked once per view when the frame should be rendered.
    using RenderViewFn = std::function<void(uint32_t viewIndex, uint32_t imageIndex,
                                            const XrView& view,
                                            const ViewSwapchain& swapchain)>;

    // Callback that records composition-layer content (acquiring / releasing
    // layer swapchain images itself) and appends the layers to submit, in
    // back-to-front order.
    using RenderLayersFn = std::function<void(XrContext& xr, const XrView* views, uint32_t viewCount,
                                              std::vector<CompositionLayer>& out)>;

    // Runs xrWaitFrame / xrBeginFrame, locates the eye views, invokes
    // renderView per eye, and submits the stereo projection layer via xrEndFrame.
    // `beforeViews` / `afterViews` add composition layers (UI panel, screens)
    // composited over the projection layer, in the order given.
    bool renderFrame(const RenderViewFn& renderView, const RenderLayersFn& beforeViews = nullptr,
                     const RenderLayersFn& afterViews = nullptr);

    // --- Accessors -------------------------------------------------------
    XrInstance instance() const { return instance_; }
    XrSession session() const { return session_; }
    XrSpace appSpace() const { return appSpace_; }
    XrSpace stageSpace() const { return stageSpace_; }
    XrTime displayTime() const { return lastDisplayTime_; }
    void transformToStage(const float localPos[3], const float localFwd[3],
                          float stagePos[3], float stageFwd[3]) const;
    XrSystemId systemId() const { return systemId_; }
    bool renderModelEnabled() const { return renderModelEnabled_; }
    uint32_t viewCount() const { return static_cast<uint32_t>(configViews_.size()); }
    const std::vector<XrViewConfigurationView>& configViews() const { return configViews_; }
    const std::vector<ViewSwapchain>& swapchains() const { return swapchains_; }

    struct ViewFallbackStats {
        uint64_t zeroLayerFrames = 0;
        uint64_t posePartialFrames = 0;
        uint64_t poseFullFallbackFrames = 0;
    };
    ViewFallbackStats viewFallbackStats() const { return viewFallbackStats_; }
    bool userPresent() const;

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
    XrSpace stageSpace_ = XR_NULL_HANDLE;
    XrTime lastDisplayTime_ = 0;

    XrSessionState sessionState_ = XR_SESSION_STATE_UNKNOWN;
    bool sessionRunning_ = false;
    bool renderModelEnabled_ = false;
    bool frameControllerProfile_ = false;  // native Steam Frame profile bound
    bool profileLogged_ = false;
    bool profileLoggedHand_[2] = {false, false};  // each hand logged once

    bool cylinderLayerEnabled_ = false;   // XR_KHR_composition_layer_cylinder
    bool extraProjectionOk_ = true;       // runtime takes a second projection layer
    uint32_t maxLayerCount_ = 0;          // XrSystemGraphicsProperties (0 = not queried yet)
    bool layerLimitLogged_ = false;

    std::vector<XrViewConfigurationView> configViews_;
    std::vector<ViewSwapchain> swapchains_;
    std::vector<XrSwapchain> layerSwapchains_;   // created via createLayerSwapchain
    std::vector<XrView> views_;
    std::vector<XrView> lastValidViews_;
    XrTime lastValidViewsTime_ = 0;
    ViewFallbackStats viewFallbackStats_;

    // Controller input.
    XrActionSet actionSet_ = XR_NULL_HANDLE;
    XrAction actions_[static_cast<int>(InputAction::Count)] = {};
    XrAction act(InputAction a) const { return actions_[static_cast<int>(a)]; }
    XrAction aimPoseAction_ = XR_NULL_HANDLE;
    XrAction gripPoseAction_ = XR_NULL_HANDLE;
    XrAction squeezeValueAction_ = XR_NULL_HANDLE;  // "Laser Hand (Analog)" fallback
    XrAction hapticAction_ = XR_NULL_HANDLE;
    XrPath handPaths_[2] = {XR_NULL_PATH, XR_NULL_PATH};
    XrSpace aimSpaces_[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
    XrSpace gripSpaces_[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
    bool actionsReady_ = false;
    bool prevFull_[2] = {false, false};  // squeeze-value full-press hysteresis
    bool bindingsDirty_ = false;
    uint32_t bindingsGeneration_ = 0;
    std::string lastBinding_[static_cast<int>(InputAction::Count)];   // last result, logged on change
    unsigned frameSuggestHands_[static_cast<int>(InputAction::Count)] = {};  // Steam Frame defaults: bit 0 left, 1 right
    bool lastHandActive_[2] = {false, false};
    int handCheckFrames_ = 0;
    XrInputState input_;

    // XR_KHR_vulkan_enable2 entry points (resolved at runtime).
    PFN_xrGetVulkanGraphicsRequirements2KHR pfnGetVulkanGraphicsRequirements2_ = nullptr;
    PFN_xrCreateVulkanInstanceKHR pfnCreateVulkanInstance_ = nullptr;
    PFN_xrGetVulkanGraphicsDevice2KHR pfnGetVulkanGraphicsDevice2_ = nullptr;
    PFN_xrCreateVulkanDeviceKHR pfnCreateVulkanDevice_ = nullptr;
};

} // namespace pixelvr

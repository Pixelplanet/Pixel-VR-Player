#include "xr/xr_context.hpp"

#include "xr/xr_result.hpp"
#include "util/logging.hpp"

#include <array>
#include <cstring>

namespace pixelvr {

namespace {

// Resolves an OpenXR extension function pointer by name.
template <typename Fn>
bool load_xr_function(XrInstance instance, const char* name, Fn& out) {
    auto result = xrGetInstanceProcAddr(instance, name,
                                        reinterpret_cast<PFN_xrVoidFunction*>(&out));
    if (XR_FAILED(result) || out == nullptr) {
        PIXELVR_LOG_ERROR("Failed to resolve OpenXR function: %s", name);
        return false;
    }
    return true;
}

} // namespace

XrContext::~XrContext() {
    for (auto& sc : swapchains_) {
        if (sc.handle != XR_NULL_HANDLE) {
            xrDestroySwapchain(sc.handle);
        }
    }
    if (appSpace_ != XR_NULL_HANDLE) {
        xrDestroySpace(appSpace_);
    }
    if (actionSet_ != XR_NULL_HANDLE) {
        xrDestroyActionSet(actionSet_);
    }
    if (session_ != XR_NULL_HANDLE) {
        xrDestroySession(session_);
    }
    if (instance_ != XR_NULL_HANDLE) {
        xrDestroyInstance(instance_);
    }
}

bool XrContext::createInstanceAndSystem(const char* appName) {
    // Discover available extensions.
    uint32_t extCount = 0;
    PIXELVR_XR_CHECK(XR_NULL_HANDLE,
                     xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr));
    std::vector<XrExtensionProperties> extProps(
        extCount, XrExtensionProperties{XR_TYPE_EXTENSION_PROPERTIES});
    PIXELVR_XR_CHECK(XR_NULL_HANDLE,
                     xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount,
                                                            extProps.data()));

    auto has_ext = [&](const char* name) {
        for (const auto& p : extProps) {
            if (std::strcmp(p.extensionName, name) == 0) {
                return true;
            }
        }
        return false;
    };

    if (!has_ext(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME)) {
        PIXELVR_LOG_ERROR("Runtime lacks required extension %s",
                          XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
        return false;
    }

    std::vector<const char*> enabledExts{XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};

    XrInstanceCreateInfo createInfo{XR_TYPE_INSTANCE_CREATE_INFO};
    std::strncpy(createInfo.applicationInfo.applicationName, appName,
                 XR_MAX_APPLICATION_NAME_SIZE - 1);
    createInfo.applicationInfo.applicationVersion = 1;
    std::strncpy(createInfo.applicationInfo.engineName, "PixelVR",
                 XR_MAX_ENGINE_NAME_SIZE - 1);
    createInfo.applicationInfo.engineVersion = 1;
    // SteamVR's runtime rejects a 1.1 apiVersion with INITIALIZATION_FAILED; request 1.0.
    createInfo.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    createInfo.enabledExtensionCount = static_cast<uint32_t>(enabledExts.size());
    createInfo.enabledExtensionNames = enabledExts.data();

    PIXELVR_XR_CHECK(XR_NULL_HANDLE, xrCreateInstance(&createInfo, &instance_));

    XrInstanceProperties instanceProps{XR_TYPE_INSTANCE_PROPERTIES};
    if (XR_SUCCEEDED(xrGetInstanceProperties(instance_, &instanceProps))) {
        PIXELVR_LOG_INFO("OpenXR runtime: %s (%u.%u.%u)", instanceProps.runtimeName,
                         XR_VERSION_MAJOR(instanceProps.runtimeVersion),
                         XR_VERSION_MINOR(instanceProps.runtimeVersion),
                         XR_VERSION_PATCH(instanceProps.runtimeVersion));
    }

    // Get the HMD system.
    XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    PIXELVR_XR_CHECK(instance_, xrGetSystem(instance_, &systemInfo, &systemId_));

    if (!loadVulkanEnable2Functions()) {
        return false;
    }

    // Enumerate the stereo view configuration.
    uint32_t viewCount = 0;
    PIXELVR_XR_CHECK(instance_,
                     xrEnumerateViewConfigurationViews(instance_, systemId_,
                                                       kViewConfigType, 0, &viewCount,
                                                       nullptr));
    configViews_.assign(viewCount,
                        XrViewConfigurationView{XR_TYPE_VIEW_CONFIGURATION_VIEW});
    PIXELVR_XR_CHECK(instance_,
                     xrEnumerateViewConfigurationViews(instance_, systemId_,
                                                       kViewConfigType, viewCount,
                                                       &viewCount, configViews_.data()));
    views_.assign(viewCount, XrView{XR_TYPE_VIEW});

    PIXELVR_LOG_INFO("System ready: %u views, %ux%u per eye", viewCount,
                     configViews_[0].recommendedImageRectWidth,
                     configViews_[0].recommendedImageRectHeight);
    return true;
}

bool XrContext::loadVulkanEnable2Functions() {
    return load_xr_function(instance_, "xrGetVulkanGraphicsRequirements2KHR",
                            pfnGetVulkanGraphicsRequirements2_) &&
           load_xr_function(instance_, "xrCreateVulkanInstanceKHR",
                            pfnCreateVulkanInstance_) &&
           load_xr_function(instance_, "xrGetVulkanGraphicsDevice2KHR",
                            pfnGetVulkanGraphicsDevice2_) &&
           load_xr_function(instance_, "xrCreateVulkanDeviceKHR",
                            pfnCreateVulkanDevice_);
}

bool XrContext::vulkanGraphicsRequirements(XrGraphicsRequirementsVulkan2KHR& out) {
    out = XrGraphicsRequirementsVulkan2KHR{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
    PIXELVR_XR_CHECK(instance_,
                     pfnGetVulkanGraphicsRequirements2_(instance_, systemId_, &out));
    return true;
}

bool XrContext::createVulkanInstance(const VkInstanceCreateInfo& createInfo,
                                     VkInstance& outInstance, VkResult& outVkResult) {
    XrVulkanInstanceCreateInfoKHR info{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
    info.systemId = systemId_;
    info.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
    info.vulkanCreateInfo = &createInfo;
    info.vulkanAllocator = nullptr;
    PIXELVR_XR_CHECK(instance_,
                     pfnCreateVulkanInstance_(instance_, &info, &outInstance, &outVkResult));
    return outVkResult == VK_SUCCESS;
}

bool XrContext::vulkanPhysicalDevice(VkInstance instance, VkPhysicalDevice& out) {
    XrVulkanGraphicsDeviceGetInfoKHR info{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
    info.systemId = systemId_;
    info.vulkanInstance = instance;
    PIXELVR_XR_CHECK(instance_, pfnGetVulkanGraphicsDevice2_(instance_, &info, &out));
    return true;
}

bool XrContext::createVulkanDevice(VkPhysicalDevice physicalDevice,
                                   const VkDeviceCreateInfo& createInfo,
                                   VkDevice& outDevice, VkResult& outVkResult) {
    XrVulkanDeviceCreateInfoKHR info{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
    info.systemId = systemId_;
    info.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
    info.vulkanPhysicalDevice = physicalDevice;
    info.vulkanCreateInfo = &createInfo;
    info.vulkanAllocator = nullptr;
    PIXELVR_XR_CHECK(instance_,
                     pfnCreateVulkanDevice_(instance_, &info, &outDevice, &outVkResult));
    return outVkResult == VK_SUCCESS;
}

bool XrContext::createSession(const XrGraphicsBindingVulkan2KHR& binding) {
    XrSessionCreateInfo createInfo{XR_TYPE_SESSION_CREATE_INFO};
    createInfo.next = &binding;
    createInfo.systemId = systemId_;
    PIXELVR_XR_CHECK(instance_, xrCreateSession(instance_, &createInfo, &session_));
    PIXELVR_LOG_INFO("OpenXR session created");
    return true;
}

bool XrContext::createReferenceSpace() {
    XrReferenceSpaceCreateInfo createInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    createInfo.referenceSpaceType = kReferenceSpaceType;
    createInfo.poseInReferenceSpace.orientation.w = 1.0f;
    PIXELVR_XR_CHECK(instance_,
                     xrCreateReferenceSpace(session_, &createInfo, &appSpace_));
    return true;
}

std::vector<int64_t> XrContext::enumerateSwapchainFormats() {
    uint32_t count = 0;
    if (XR_FAILED(xrEnumerateSwapchainFormats(session_, 0, &count, nullptr))) {
        return {};
    }
    std::vector<int64_t> formats(count);
    if (XR_FAILED(xrEnumerateSwapchainFormats(session_, count, &count, formats.data()))) {
        return {};
    }
    return formats;
}

bool XrContext::createSwapchains(int64_t colorFormat) {
    swapchains_.clear();
    swapchains_.reserve(configViews_.size());

    for (const auto& view : configViews_) {
        ViewSwapchain sc;
        sc.width = static_cast<int32_t>(view.recommendedImageRectWidth);
        sc.height = static_cast<int32_t>(view.recommendedImageRectHeight);
        sc.format = colorFormat;

        XrSwapchainCreateInfo createInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        createInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                                XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        createInfo.format = colorFormat;
        createInfo.sampleCount = 1;
        createInfo.width = view.recommendedImageRectWidth;
        createInfo.height = view.recommendedImageRectHeight;
        createInfo.faceCount = 1;
        createInfo.arraySize = 1;
        createInfo.mipCount = 1;

        PIXELVR_XR_CHECK(instance_, xrCreateSwapchain(session_, &createInfo, &sc.handle));

        uint32_t imageCount = 0;
        PIXELVR_XR_CHECK(instance_,
                         xrEnumerateSwapchainImages(sc.handle, 0, &imageCount, nullptr));
        sc.images.assign(imageCount,
                        XrSwapchainImageVulkan2KHR{XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
        PIXELVR_XR_CHECK(
            instance_,
            xrEnumerateSwapchainImages(
                sc.handle, imageCount, &imageCount,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(sc.images.data())));

        swapchains_.push_back(std::move(sc));
    }

    PIXELVR_LOG_INFO("Created %zu swapchains", swapchains_.size());
    return true;
}

bool XrContext::createActions() {
    XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strcpy(setInfo.actionSetName, "gameplay");
    std::strcpy(setInfo.localizedActionSetName, "Gameplay");
    PIXELVR_XR_CHECK(instance_, xrCreateActionSet(instance_, &setInfo, &actionSet_));

    auto make_action = [&](const char* name, const char* localized,
                           XrActionType type, XrAction& out) -> bool {
        XrActionCreateInfo ai{XR_TYPE_ACTION_CREATE_INFO};
        std::strcpy(ai.actionName, name);
        std::strcpy(ai.localizedActionName, localized);
        ai.actionType = type;
        return XR_SUCCEEDED(xrCreateAction(actionSet_, &ai, &out));
    };
    if (!make_action("toggle_play", "Play/Pause", XR_ACTION_TYPE_BOOLEAN_INPUT,
                     togglePlayAction_) ||
        !make_action("browser", "Open Menu", XR_ACTION_TYPE_BOOLEAN_INPUT,
                     browserAction_) ||
        !make_action("select", "Select", XR_ACTION_TYPE_BOOLEAN_INPUT,
                     selectAction_) ||
        !make_action("recenter", "Recenter Screen", XR_ACTION_TYPE_BOOLEAN_INPUT,
                     recenterAction_) ||
        !make_action("adjust", "Adjust Distance", XR_ACTION_TYPE_VECTOR2F_INPUT,
                     thumbstickAction_) ||
        !make_action("stick_left", "Left Stick", XR_ACTION_TYPE_VECTOR2F_INPUT,
                     thumbstickLeftAction_) ||
        !make_action("stick_right", "Right Stick", XR_ACTION_TYPE_VECTOR2F_INPUT,
                     thumbstickRightAction_)) {
        PIXELVR_LOG_ERROR("Failed to create XR actions");
        return false;
    }

    auto path = [&](const char* s) {
        XrPath p = XR_NULL_PATH;
        xrStringToPath(instance_, s, &p);
        return p;
    };

    // A single pose action, tracked per hand via subaction paths, drives the
    // controller pointer rays and models.
    handPaths_[0] = path("/user/hand/left");
    handPaths_[1] = path("/user/hand/right");
    {
        XrActionCreateInfo ai{XR_TYPE_ACTION_CREATE_INFO};
        std::strcpy(ai.actionName, "aim_pose");
        std::strcpy(ai.localizedActionName, "Pointer Pose");
        ai.actionType = XR_ACTION_TYPE_POSE_INPUT;
        ai.countSubactionPaths = 2;
        ai.subactionPaths = handPaths_;
        if (XR_FAILED(xrCreateAction(actionSet_, &ai, &aimPoseAction_))) {
            PIXELVR_LOG_WARN("Failed to create aim pose action");
            aimPoseAction_ = XR_NULL_HANDLE;
        }
    }

    // Suggest bindings per interaction profile. Controls favour the face buttons
    // and trigger over thumbstick clicks: A/X play/pause, B/Y open the menu,
    // trigger selects, grip recenters. Every profile also tracks the aim pose.
    // A profile the runtime doesn't recognize is simply skipped.
    auto suggest = [&](const char* profile,
                       std::vector<XrActionSuggestedBinding> binds) {
        XrInteractionProfileSuggestedBinding s{
            XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        s.interactionProfile = path(profile);
        s.suggestedBindings = binds.data();
        s.countSuggestedBindings = static_cast<uint32_t>(binds.size());
        if (XR_FAILED(xrSuggestInteractionProfileBindings(instance_, &s))) {
            PIXELVR_LOG_WARN("Interaction profile not accepted: %s", profile);
        }
    };

    suggest("/interaction_profiles/khr/simple_controller",
            {{selectAction_, path("/user/hand/left/input/select/click")},
             {selectAction_, path("/user/hand/right/input/select/click")},
             {browserAction_, path("/user/hand/left/input/menu/click")},
             {browserAction_, path("/user/hand/right/input/menu/click")},
             {aimPoseAction_, path("/user/hand/left/input/aim/pose")},
             {aimPoseAction_, path("/user/hand/right/input/aim/pose")}});

    suggest("/interaction_profiles/oculus/touch_controller",
            {{togglePlayAction_, path("/user/hand/right/input/a/click")},
             {togglePlayAction_, path("/user/hand/left/input/x/click")},
             {browserAction_, path("/user/hand/right/input/b/click")},
             {browserAction_, path("/user/hand/left/input/y/click")},
             {selectAction_, path("/user/hand/right/input/trigger/value")},
             {selectAction_, path("/user/hand/left/input/trigger/value")},
             {recenterAction_, path("/user/hand/right/input/squeeze/value")},
             {recenterAction_, path("/user/hand/left/input/squeeze/value")},
             {thumbstickAction_, path("/user/hand/right/input/thumbstick")},
             {thumbstickAction_, path("/user/hand/left/input/thumbstick")},
             {thumbstickLeftAction_, path("/user/hand/left/input/thumbstick")},
             {thumbstickRightAction_, path("/user/hand/right/input/thumbstick")},
             {aimPoseAction_, path("/user/hand/right/input/aim/pose")},
             {aimPoseAction_, path("/user/hand/left/input/aim/pose")}});

    suggest("/interaction_profiles/valve/index_controller",
            {{togglePlayAction_, path("/user/hand/right/input/a/click")},
             {togglePlayAction_, path("/user/hand/left/input/a/click")},
             {browserAction_, path("/user/hand/right/input/b/click")},
             {browserAction_, path("/user/hand/left/input/b/click")},
             {selectAction_, path("/user/hand/right/input/trigger/value")},
             {selectAction_, path("/user/hand/left/input/trigger/value")},
             {recenterAction_, path("/user/hand/right/input/squeeze/value")},
             {recenterAction_, path("/user/hand/left/input/squeeze/value")},
             {thumbstickAction_, path("/user/hand/right/input/thumbstick")},
             {thumbstickAction_, path("/user/hand/left/input/thumbstick")},
             {thumbstickLeftAction_, path("/user/hand/left/input/thumbstick")},
             {thumbstickRightAction_, path("/user/hand/right/input/thumbstick")},
             {aimPoseAction_, path("/user/hand/right/input/aim/pose")},
             {aimPoseAction_, path("/user/hand/left/input/aim/pose")}});

    XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets = 1;
    attach.actionSets = &actionSet_;
    PIXELVR_XR_CHECK(instance_, xrAttachSessionActionSets(session_, &attach));

    // Action spaces let us locate each controller's aim pose every frame.
    if (aimPoseAction_ != XR_NULL_HANDLE) {
        for (int h = 0; h < 2; ++h) {
            XrActionSpaceCreateInfo si{XR_TYPE_ACTION_SPACE_CREATE_INFO};
            si.action = aimPoseAction_;
            si.subactionPath = handPaths_[h];
            si.poseInActionSpace.orientation.w = 1.0f;
            if (XR_FAILED(xrCreateActionSpace(session_, &si, &aimSpaces_[h]))) {
                PIXELVR_LOG_WARN("Failed to create aim action space %d", h);
                aimSpaces_[h] = XR_NULL_HANDLE;
            }
        }
    }

    actionsReady_ = true;
    PIXELVR_LOG_INFO(
        "Controller input ready (A/X play, B/Y menu, trigger select, grip recenter)");
    return true;
}

void XrContext::syncActions() {
    input_.togglePlay = false;
    input_.browserToggle = false;
    input_.select = false;
    input_.selectHeld = false;
    input_.recenter = false;
    input_.thumbstickX = 0.0f;
    input_.thumbstickY = 0.0f;
    input_.thumbstickLX = 0.0f;
    input_.thumbstickLY = 0.0f;
    input_.thumbstickRX = 0.0f;
    input_.thumbstickRY = 0.0f;
    if (!actionsReady_) {
        return;
    }

    XrActiveActionSet active{actionSet_, XR_NULL_PATH};
    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    if (XR_FAILED(xrSyncActions(session_, &sync))) {
        return;  // Not focused yet: no input this frame.
    }

    auto rising_edge = [&](XrAction a) -> bool {
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = a;
        XrActionStateBoolean st{XR_TYPE_ACTION_STATE_BOOLEAN};
        if (XR_FAILED(xrGetActionStateBoolean(session_, &gi, &st))) {
            return false;
        }
        return st.isActive && st.currentState && st.changedSinceLastSync;
    };
    input_.togglePlay = rising_edge(togglePlayAction_);
    input_.browserToggle = rising_edge(browserAction_);
    input_.select = rising_edge(selectAction_);
    input_.recenter = rising_edge(recenterAction_);

    // Trigger held (not just the rising edge) so the timeline can be dragged.
    {
        XrActionStateGetInfo hi{XR_TYPE_ACTION_STATE_GET_INFO};
        hi.action = selectAction_;
        XrActionStateBoolean hs{XR_TYPE_ACTION_STATE_BOOLEAN};
        if (XR_SUCCEEDED(xrGetActionStateBoolean(session_, &hi, &hs)) &&
            hs.isActive) {
            input_.selectHeld = hs.currentState;
        }
    }

    auto read_stick = [&](XrAction a, float& x, float& y) {
        if (a == XR_NULL_HANDLE) return;
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = a;
        XrActionStateVector2f v{XR_TYPE_ACTION_STATE_VECTOR2F};
        if (XR_SUCCEEDED(xrGetActionStateVector2f(session_, &gi, &v)) && v.isActive) {
            x = v.currentState.x;
            y = v.currentState.y;
        }
    };
    read_stick(thumbstickAction_, input_.thumbstickX, input_.thumbstickY);
    read_stick(thumbstickLeftAction_, input_.thumbstickLX, input_.thumbstickLY);
    read_stick(thumbstickRightAction_, input_.thumbstickRX, input_.thumbstickRY);
}

void XrContext::handleSessionStateChange(const XrEventDataSessionStateChanged& ev,
                                         bool& exitRequested) {
    sessionState_ = ev.state;

    switch (sessionState_) {
        case XR_SESSION_STATE_READY: {
            XrSessionBeginInfo beginInfo{XR_TYPE_SESSION_BEGIN_INFO};
            beginInfo.primaryViewConfigurationType = kViewConfigType;
            if (XR_SUCCEEDED(xrBeginSession(session_, &beginInfo))) {
                sessionRunning_ = true;
                PIXELVR_LOG_INFO("Session running");
            }
            break;
        }
        case XR_SESSION_STATE_STOPPING: {
            sessionRunning_ = false;
            PIXELVR_XR_WARN(instance_, xrEndSession(session_));
            PIXELVR_LOG_INFO("Session stopped");
            break;
        }
        case XR_SESSION_STATE_EXITING:
        case XR_SESSION_STATE_LOSS_PENDING:
            exitRequested = true;
            break;
        default:
            break;
    }
}

bool XrContext::pollEvents(bool& exitRequested) {
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    auto next_event = [&]() {
        event = XrEventDataBuffer{XR_TYPE_EVENT_DATA_BUFFER};
        return xrPollEvent(instance_, &event) == XR_SUCCESS;
    };

    while (next_event()) {
        switch (event.type) {
            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                exitRequested = true;
                return true;
            case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
                handleSessionStateChange(
                    *reinterpret_cast<const XrEventDataSessionStateChanged*>(&event),
                    exitRequested);
                break;
            default:
                break;
        }
    }
    return true;
}

bool XrContext::locateViews(XrTime displayTime, std::vector<XrView>& views,
                            bool& shouldRender) {
    XrViewState viewState{XR_TYPE_VIEW_STATE};
    XrViewLocateInfo locateInfo{XR_TYPE_VIEW_LOCATE_INFO};
    locateInfo.viewConfigurationType = kViewConfigType;
    locateInfo.displayTime = displayTime;
    locateInfo.space = appSpace_;

    uint32_t count = static_cast<uint32_t>(views.size());
    XrResult res = xrLocateViews(session_, &locateInfo, &viewState, count, &count,
                                 views.data());
    if (XR_FAILED(res)) {
        shouldRender = false;
        return false;
    }

    shouldRender = (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) &&
                   (viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT);
    return true;
}

bool XrContext::renderFrame(const RenderViewFn& renderView) {
    XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState frameState{XR_TYPE_FRAME_STATE};
    PIXELVR_XR_CHECK(instance_, xrWaitFrame(session_, &waitInfo, &frameState));

    XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
    PIXELVR_XR_CHECK(instance_, xrBeginFrame(session_, &beginInfo));

    syncActions();

    std::vector<XrCompositionLayerProjectionView> projectionViews;
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};

    bool viewsValid = false;
    if (frameState.shouldRender == XR_TRUE) {
        locateViews(frameState.predictedDisplayTime, views_, viewsValid);
    }

    // Publish the head pose (eye midpoint + forward) for recenter logic.
    input_.headValid = viewsValid && views_.size() >= 2;
    if (input_.headValid) {
        const XrPosef& l = views_[0].pose;
        const XrPosef& r = views_[1].pose;
        input_.headPos[0] = 0.5f * (l.position.x + r.position.x);
        input_.headPos[1] = 0.5f * (l.position.y + r.position.y);
        input_.headPos[2] = 0.5f * (l.position.z + r.position.z);
        const XrQuaternionf& q = l.orientation;
        input_.headForward[0] = -2.0f * (q.x * q.z + q.w * q.y);
        input_.headForward[1] = -2.0f * (q.y * q.z - q.w * q.x);
        input_.headForward[2] = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
    }

    // Locate each controller's aim pose (for the pointer rays and models).
    for (int h = 0; h < 2; ++h) {
        input_.controllers[h].active = false;
        if (aimSpaces_[h] == XR_NULL_HANDLE) {
            continue;
        }
        XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
        if (XR_FAILED(xrLocateSpace(aimSpaces_[h], appSpace_,
                                    frameState.predictedDisplayTime, &loc))) {
            continue;
        }
        constexpr XrSpaceLocationFlags need =
            XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
            XR_SPACE_LOCATION_POSITION_VALID_BIT;
        if ((loc.locationFlags & need) == need) {
            auto& c = input_.controllers[h];
            c.active = true;
            c.position[0] = loc.pose.position.x;
            c.position[1] = loc.pose.position.y;
            c.position[2] = loc.pose.position.z;
            c.orientation[0] = loc.pose.orientation.x;
            c.orientation[1] = loc.pose.orientation.y;
            c.orientation[2] = loc.pose.orientation.z;
            c.orientation[3] = loc.pose.orientation.w;
        }
    }

    if (viewsValid) {
        projectionViews.resize(views_.size(),
                               {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});

        for (uint32_t i = 0; i < views_.size(); ++i) {
            ViewSwapchain& sc = swapchains_[i];

            uint32_t imageIndex = 0;
            XrSwapchainImageAcquireInfo acquireInfo{
                XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            PIXELVR_XR_CHECK(instance_,
                             xrAcquireSwapchainImage(sc.handle, &acquireInfo, &imageIndex));

            XrSwapchainImageWaitInfo waitImageInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            waitImageInfo.timeout = XR_INFINITE_DURATION;
            PIXELVR_XR_CHECK(instance_, xrWaitSwapchainImage(sc.handle, &waitImageInfo));

            renderView(i, imageIndex, views_[i], sc);

            XrSwapchainImageReleaseInfo releaseInfo{
                XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            PIXELVR_XR_CHECK(instance_, xrReleaseSwapchainImage(sc.handle, &releaseInfo));

            projectionViews[i].pose = views_[i].pose;
            projectionViews[i].fov = views_[i].fov;
            projectionViews[i].subImage.swapchain = sc.handle;
            projectionViews[i].subImage.imageRect.offset = {0, 0};
            projectionViews[i].subImage.imageRect.extent = {sc.width, sc.height};
            projectionViews[i].subImage.imageArrayIndex = 0;
        }

        layer.space = appSpace_;
        layer.viewCount = static_cast<uint32_t>(projectionViews.size());
        layer.views = projectionViews.data();
    }

    std::array<XrCompositionLayerBaseHeader*, 1> layers{
        reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer)};

    XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
    endInfo.displayTime = frameState.predictedDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount = viewsValid ? 1u : 0u;
    endInfo.layers = viewsValid ? layers.data() : nullptr;

    PIXELVR_XR_CHECK(instance_, xrEndFrame(session_, &endInfo));
    return true;
}

} // namespace pixelvr

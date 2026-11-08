#include "xr/xr_context.hpp"

#include "xr/xr_result.hpp"
#include "xr/vr_keyboard.hpp"
#include "util/logging.hpp"

#include <algorithm>
#include <array>
#include <cmath>
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
    for (XrSwapchain sc : layerSwapchains_) {
        if (sc != XR_NULL_HANDLE) xrDestroySwapchain(sc);
    }
    if (appSpace_ != XR_NULL_HANDLE) {
        xrDestroySpace(appSpace_);
    }
    if (stageSpace_ != XR_NULL_HANDLE) {
        xrDestroySpace(stageSpace_);
        stageSpace_ = XR_NULL_HANDLE;
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

    // Optional: curved UI composition layer (the flat quad layer is core).
    if (has_ext(XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME)) {
        enabledExts.push_back(XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME);
        cylinderLayerEnabled_ = true;
        PIXELVR_LOG_INFO("Cylinder composition layers available");
    } else {
        PIXELVR_LOG_WARN("%s unavailable; the UI layer falls back to a flat quad",
                         XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME);
    }

    // Optional: real controller models via XR_EXT_render_model (depends on uuid +
    // interaction_render_model). Enabled only if all three are present.
    if (has_ext(XR_EXT_RENDER_MODEL_EXTENSION_NAME) &&
        has_ext(XR_EXT_INTERACTION_RENDER_MODEL_EXTENSION_NAME) &&
        has_ext(XR_EXT_UUID_EXTENSION_NAME)) {
        enabledExts.push_back(XR_EXT_UUID_EXTENSION_NAME);
        enabledExts.push_back(XR_EXT_RENDER_MODEL_EXTENSION_NAME);
        enabledExts.push_back(XR_EXT_INTERACTION_RENDER_MODEL_EXTENSION_NAME);
        renderModelEnabled_ = true;
        PIXELVR_LOG_INFO("Render-model extensions enabled");
    }

    // Native Steam Frame controller profile (/interaction_profiles/valve/
    // frame_controller): exposes a/b/x/y, dpad, view, bumper, analog grip, etc.,
    // instead of SteamVR's lossy auto-remap onto the Index/Touch profiles.
    static constexpr char kFrameControllerExt[] =
        "XR_VALVE_frame_controller_interaction";
    if (has_ext(kFrameControllerExt)) {
        enabledExts.push_back(kFrameControllerExt);
        frameControllerProfile_ = true;
        PIXELVR_LOG_INFO("Steam Frame controller profile extension enabled");
    } else {
        PIXELVR_LOG_WARN("XR_VALVE_frame_controller_interaction unavailable");
    }

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

    // Create STAGE reference space (floor level tracking bounds, matching SteamVR TrackingUniverseStanding)
    XrReferenceSpaceCreateInfo stageInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    stageInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    stageInfo.poseInReferenceSpace.orientation.w = 1.0f;
    XrResult stageRes = xrCreateReferenceSpace(session_, &stageInfo, &stageSpace_);
    if (XR_SUCCEEDED(stageRes)) {
        PIXELVR_LOG_INFO("XrContext: Successfully created STAGE reference space");
    } else {
        PIXELVR_LOG_WARN("XrContext: STAGE reference space not available (%d), using calibrated standing fallback", static_cast<int>(stageRes));
        stageSpace_ = XR_NULL_HANDLE;
    }
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

bool XrContext::createSwapchains(int64_t colorFormat, float scaleFactor) {
    for (auto& sc : swapchains_) {
        if (sc.handle != XR_NULL_HANDLE) {
            xrDestroySwapchain(sc.handle);
        }
    }
    swapchains_.clear();
    swapchains_.reserve(configViews_.size());

    scaleFactor = std::clamp(scaleFactor, 0.5f, 2.5f);

    for (const auto& view : configViews_) {
        ViewSwapchain sc;
        // Configurable OpenXR Swapchain Supersampling:
        // Boost swapchain resolution from SteamVR's conservative default (e.g. 1728) to the headset's
        // native physical display resolution (2160x2160, 1.25x) or crisp supersampling (1.4x-1.75x).
        // Rendering at higher pixel density eliminates compositor magnification blur, improves text
        // clarity, and reduces moire/specular shimmering during head movement in VR.
        const uint32_t maxW = (view.maxImageRectWidth > 0) ? view.maxImageRectWidth : 4096;
        const uint32_t maxH = (view.maxImageRectHeight > 0) ? view.maxImageRectHeight : 4096;
        // Anchor baseline resolution to physical panel dimensions (2160x2160 per eye),
        // preventing SteamVR's conservative default (e.g. 1728) from degrading display sharpness.
        const uint32_t baseW = std::max(view.recommendedImageRectWidth, 2160u);
        const uint32_t baseH = std::max(view.recommendedImageRectHeight, 2160u);
        uint32_t targetW = std::clamp(static_cast<uint32_t>(std::round(baseW * scaleFactor)),
                                      256u, maxW);
        uint32_t targetH = std::clamp(static_cast<uint32_t>(std::round(baseH * scaleFactor)),
                                      256u, maxH);
        targetW = (targetW + 1) & ~1u;
        targetH = (targetH + 1) & ~1u;
        sc.width = static_cast<int32_t>(targetW);
        sc.height = static_cast<int32_t>(targetH);
        sc.format = colorFormat;

        XrSwapchainCreateInfo createInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        createInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                                XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        createInfo.format = colorFormat;
        createInfo.sampleCount = 1;
        createInfo.width = targetW;
        createInfo.height = targetH;
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

    PIXELVR_LOG_INFO("Created %zu swapchains (%dx%d per eye, scale %.2fx)", swapchains_.size(),
                     swapchains_.empty() ? 0 : swapchains_[0].width,
                     swapchains_.empty() ? 0 : swapchains_[0].height,
                     scaleFactor);
    return true;
}

bool XrContext::createLayerSwapchain(int64_t colorFormat, uint32_t width, uint32_t height,
                                     ViewSwapchain& out) {
    destroyLayerSwapchain(out);
    XrSwapchainCreateInfo createInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    createInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                            XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    createInfo.format = colorFormat;
    createInfo.sampleCount = 1;
    createInfo.width = width;
    createInfo.height = height;
    createInfo.faceCount = 1;
    createInfo.arraySize = 1;
    createInfo.mipCount = 1;
    PIXELVR_XR_CHECK(instance_, xrCreateSwapchain(session_, &createInfo, &out.handle));
    layerSwapchains_.push_back(out.handle);

    out.width = static_cast<int32_t>(width);
    out.height = static_cast<int32_t>(height);
    out.format = colorFormat;
    uint32_t imageCount = 0;
    PIXELVR_XR_CHECK(instance_, xrEnumerateSwapchainImages(out.handle, 0, &imageCount, nullptr));
    out.images.assign(imageCount, XrSwapchainImageVulkan2KHR{XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
    PIXELVR_XR_CHECK(
        instance_,
        xrEnumerateSwapchainImages(out.handle, imageCount, &imageCount,
                                   reinterpret_cast<XrSwapchainImageBaseHeader*>(out.images.data())));
    PIXELVR_LOG_INFO("Created layer swapchain (%ux%u, %u images)", width, height, imageCount);
    return true;
}

void XrContext::destroyLayerSwapchain(ViewSwapchain& sc) {
    if (sc.handle != XR_NULL_HANDLE) {
        xrDestroySwapchain(sc.handle);
        layerSwapchains_.erase(std::remove(layerSwapchains_.begin(), layerSwapchains_.end(), sc.handle),
                               layerSwapchains_.end());
    }
    sc = ViewSwapchain{};
}

bool XrContext::acquireLayerImage(const ViewSwapchain& sc, uint32_t& imageIndex) {
    XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    PIXELVR_XR_CHECK(instance_, xrAcquireSwapchainImage(sc.handle, &acquireInfo, &imageIndex));
    XrSwapchainImageWaitInfo waitImageInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    waitImageInfo.timeout = XR_INFINITE_DURATION;
    PIXELVR_XR_CHECK(instance_, xrWaitSwapchainImage(sc.handle, &waitImageInfo));
    return true;
}

void XrContext::releaseLayerImage(const ViewSwapchain& sc) {
    XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(sc.handle, &releaseInfo);
}

namespace {

struct ActionDef {
    // /actions/gameplay/in/<name>. SteamOS shows this raw name in its binding
    // callouts (SteamVR drops localizedActionName), so it must read well on its
    // own: lowercase a-z, 0-9, '-', '_', '.'. Renaming resets saved SteamVR bindings.
    const char* name;
    const char* label;  // our own Buttons page: true in every context, <= 3 words
    XrActionType type;
};

// Indexed by InputAction. Nothing here may hint at the vault: the grip is just
// "Laser Hand", and the unlock combo reads the face actions below.
constexpr XrActionType kBool = XR_ACTION_TYPE_BOOLEAN_INPUT;
const ActionDef kActionDefs[] = {
    {"select", "Select", kBool},
    {"play-ok", "Play / OK", kBool},
    {"back", "Back", kBool},
    {"main-menu", "Main Menu", kBool},
    {"options", "Options", kBool},
    {"recenter", "Recenter", kBool},
    {"show-hide-ui", "Show / Hide UI", kBool},
    {"previous", "Previous", kBool},
    {"next", "Next", kBool},
    {"up-volume-up", "Up / Vol +", kBool},
    {"down-volume-down", "Down / Vol \xE2\x80\x93", kBool},           // en dash: our font has it
    {"left-back-10s", "Left / \xE2\x80\x93" "10 s", kBool},
    {"right-forward-10s", "Right / +10 s", kBool},
    {"scrub-distance", "Scrub / Distance", XR_ACTION_TYPE_VECTOR2F_INPUT},  // right stick
    {"skip-distance", "Skip / Distance", XR_ACTION_TYPE_VECTOR2F_INPUT},    // left stick
    {"show-controls", "Show Controls", kBool},
    {"laser-hand", "Laser Hand", kBool},  // per hand (subaction paths)
    // No default binding: for SteamVR chords / long presses.
    {"mute", "Mute", kBool},
    {"stop", "Stop", kBool},
    {"bookmarks", "Bookmarks", kBool},
    {"next-screen", "Next Screen", kBool},
    {"previous-screen", "Previous Screen", kBool},
    {"back-30s", "Back 30 s", kBool},
    {"forward-30s", "Forward 30 s", kBool},
    {"lock-screens", "Lock Screens", kBool},
    {"audio-track", "Audio Track", kBool},
    {"format-3d", "3D Format", kBool},
};
static_assert(sizeof(kActionDefs) / sizeof(kActionDefs[0]) == static_cast<std::size_t>(InputAction::Count),
              "one ActionDef per InputAction");

// "Right Hand A Button" -> "Right A": the Controls page has narrow value wells.
std::string shortenSourceName(std::string s) {
    auto eraseAll = [&s](const std::string& what, const std::string& with) {
        for (std::size_t p = s.find(what); p != std::string::npos; p = s.find(what, p + with.size())) {
            s.replace(p, what.size(), with);
        }
    };
    eraseAll("Left Hand ", "Left ");
    eraseAll("Right Hand ", "Right ");
    eraseAll(" Button", "");
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

} // namespace

const char* XrContext::actionLabel(InputAction a) {
    const int i = static_cast<int>(a);
    return (i >= 0 && i < static_cast<int>(InputAction::Count)) ? kActionDefs[i].label : "";
}

bool XrContext::createActions() {
    XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strcpy(setInfo.actionSetName, "gameplay");  // kept: saved SteamVR bindings use it
    std::strcpy(setInfo.localizedActionSetName, "Player Controls");
    PIXELVR_XR_CHECK(instance_, xrCreateActionSet(instance_, &setInfo, &actionSet_));

    auto path = [&](const char* s) {
        XrPath p = XR_NULL_PATH;
        xrStringToPath(instance_, s, &p);
        return p;
    };
    handPaths_[0] = path("/user/hand/left");
    handPaths_[1] = path("/user/hand/right");

    auto make_action = [&](const char* name, const char* localized, XrActionType type,
                           bool perHand, XrAction& out) -> bool {
        XrActionCreateInfo ai{XR_TYPE_ACTION_CREATE_INFO};
        std::strcpy(ai.actionName, name);
        std::strcpy(ai.localizedActionName, localized);
        ai.actionType = type;
        if (perHand) {
            ai.countSubactionPaths = 2;
            ai.subactionPaths = handPaths_;
        }
        if (XR_FAILED(xrCreateAction(actionSet_, &ai, &out))) {
            PIXELVR_LOG_ERROR("Failed to create XR action %s (\"%s\")", name, localized);
            out = XR_NULL_HANDLE;
            return false;
        }
        return true;
    };
    for (int i = 0; i < static_cast<int>(InputAction::Count); ++i) {
        const ActionDef& d = kActionDefs[i];
        const bool perHand = (i == static_cast<int>(InputAction::LaserHand));
        if (!make_action(d.name, d.label, d.type, perHand, actions_[i])) {
            return false;
        }
    }
    // Pointer and model poses, haptics and the analog full-grip fallback: per
    // hand. Poses and outputs never show as SteamVR callouts.
    make_action("laser-hand-analog", "Laser Hand (Analog)", XR_ACTION_TYPE_FLOAT_INPUT, true,
                squeezeValueAction_);
    make_action("aim_pose", "Pointer Pose", XR_ACTION_TYPE_POSE_INPUT, true, aimPoseAction_);
    make_action("grip_pose", "Grip Pose", XR_ACTION_TYPE_POSE_INPUT, true, gripPoseAction_);
    make_action("haptic_out", "Haptic Output", XR_ACTION_TYPE_VIBRATION_OUTPUT, true, hapticAction_);

    using IA = InputAction;
    struct Bind {
        XrAction action;
        const char* path;
    };
    // Suggest bindings per interaction profile. One invalid path rejects the
    // whole profile, so every path must exist in that profile.
    auto suggest = [&](const char* profile, std::initializer_list<Bind> list) {
        std::vector<XrActionSuggestedBinding> binds;
        for (const Bind& b : list) {
            if (b.action != XR_NULL_HANDLE) binds.push_back({b.action, path(b.path)});
        }
        XrInteractionProfileSuggestedBinding s{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        s.interactionProfile = path(profile);
        s.suggestedBindings = binds.data();
        s.countSuggestedBindings = static_cast<uint32_t>(binds.size());
        const XrResult r = xrSuggestInteractionProfileBindings(instance_, &s);
        if (XR_FAILED(r)) {
            PIXELVR_LOG_ERROR("Interaction profile REJECTED (%d), none of its %zu bindings apply: %s",
                              static_cast<int>(r), binds.size(), profile);
        }
    };

    suggest("/interaction_profiles/khr/simple_controller",
            {{act(IA::Select), "/user/hand/left/input/select/click"},
             {act(IA::Select), "/user/hand/right/input/select/click"},
             {act(IA::MainMenu), "/user/hand/left/input/menu/click"},
             {act(IA::MainMenu), "/user/hand/right/input/menu/click"},
             {aimPoseAction_, "/user/hand/left/input/aim/pose"},
             {aimPoseAction_, "/user/hand/right/input/aim/pose"},
             {gripPoseAction_, "/user/hand/left/input/grip/pose"},
             {gripPoseAction_, "/user/hand/right/input/grip/pose"},
             {hapticAction_, "/user/hand/left/output/haptic"},
             {hapticAction_, "/user/hand/right/output/haptic"}});

    // Touch: A/B/X/Y keep the combo letters' defaults (A play, B back, X
    // recenter, Y options). No squeeze click: the analog value is the full press.
    suggest("/interaction_profiles/oculus/touch_controller",
            {{act(IA::Select), "/user/hand/left/input/trigger/value"},
             {act(IA::Select), "/user/hand/right/input/trigger/value"},
             {act(IA::PlayOk), "/user/hand/right/input/a/click"},
             {act(IA::Back), "/user/hand/right/input/b/click"},
             {act(IA::Recenter), "/user/hand/left/input/x/click"},
             {act(IA::Options), "/user/hand/left/input/y/click"},
             {act(IA::MainMenu), "/user/hand/left/input/menu/click"},
             {act(IA::ShowControls), "/user/hand/left/input/thumbstick/click"},
             {act(IA::ShowControls), "/user/hand/right/input/thumbstick/click"},
             {act(IA::StickLeft), "/user/hand/left/input/thumbstick"},
             {act(IA::StickRight), "/user/hand/right/input/thumbstick"},
             {squeezeValueAction_, "/user/hand/left/input/squeeze/value"},
             {squeezeValueAction_, "/user/hand/right/input/squeeze/value"},
             {aimPoseAction_, "/user/hand/left/input/aim/pose"},
             {aimPoseAction_, "/user/hand/right/input/aim/pose"},
             {gripPoseAction_, "/user/hand/left/input/grip/pose"},
             {gripPoseAction_, "/user/hand/right/input/grip/pose"},
             {hapticAction_, "/user/hand/left/output/haptic"},
             {hapticAction_, "/user/hand/right/output/haptic"}});

    // Index: A/B per hand; it has no squeeze/click (only value + force) and no
    // menu button, so the left stick click opens the menu.
    suggest("/interaction_profiles/valve/index_controller",
            {{act(IA::Select), "/user/hand/left/input/trigger/value"},
             {act(IA::Select), "/user/hand/right/input/trigger/value"},
             {act(IA::PlayOk), "/user/hand/right/input/a/click"},
             {act(IA::Back), "/user/hand/right/input/b/click"},
             {act(IA::Recenter), "/user/hand/left/input/a/click"},
             {act(IA::Options), "/user/hand/left/input/b/click"},
             {act(IA::MainMenu), "/user/hand/left/input/thumbstick/click"},
             {act(IA::ShowControls), "/user/hand/right/input/thumbstick/click"},
             {act(IA::StickLeft), "/user/hand/left/input/thumbstick"},
             {act(IA::StickRight), "/user/hand/right/input/thumbstick"},
             {squeezeValueAction_, "/user/hand/left/input/squeeze/value"},
             {squeezeValueAction_, "/user/hand/right/input/squeeze/value"},
             {aimPoseAction_, "/user/hand/left/input/aim/pose"},
             {aimPoseAction_, "/user/hand/right/input/aim/pose"},
             {gripPoseAction_, "/user/hand/left/input/grip/pose"},
             {gripPoseAction_, "/user/hand/right/input/grip/pose"},
             {hapticAction_, "/user/hand/left/output/haptic"},
             {hapticAction_, "/user/hand/right/output/haptic"}});

    // Native Steam Frame profile: the runtime rejects the whole set if any path
    // is invalid, so probe each candidate individually, keep the accepted ones,
    // then suggest the working subset. Layout: A, B, X, Y and Menu are on the
    // right controller, View and the D-pad on the left; both have trigger,
    // grip (squeeze), bumper and stick.
    if (frameControllerProfile_) {
        const XrPath frameProfile = path("/interaction_profiles/valve/frame_controller");
        std::vector<XrActionSuggestedBinding> frameBinds;
        int rejected = 0;
        auto probe = [&](XrAction a, const char* p) {
            if (a == XR_NULL_HANDLE) return;
            const XrActionSuggestedBinding one{a, path(p)};
            XrInteractionProfileSuggestedBinding s{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
            s.interactionProfile = frameProfile;
            s.suggestedBindings = &one;
            s.countSuggestedBindings = 1;
            if (XR_SUCCEEDED(xrSuggestInteractionProfileBindings(instance_, &s))) {
                frameBinds.push_back(one);
                // Remember the hand of each default, so the Buttons page can say
                // "Left controller off" instead of "Not bound".
                const int hand = (std::strncmp(p, "/user/hand/left/", 16) == 0) ? 0 : 1;
                for (int i = 0; i < static_cast<int>(InputAction::Count); ++i) {
                    if (actions_[i] == a) frameSuggestHands_[i] |= (1u << hand);
                }
            } else {
                ++rejected;
                PIXELVR_LOG_ERROR("Steam Frame binding REJECTED: %s", p);
            }
        };
        probe(aimPoseAction_, "/user/hand/left/input/aim/pose");
        probe(aimPoseAction_, "/user/hand/right/input/aim/pose");
        probe(gripPoseAction_, "/user/hand/left/input/grip/pose");
        probe(gripPoseAction_, "/user/hand/right/input/grip/pose");
        probe(hapticAction_, "/user/hand/left/output/haptic");
        probe(hapticAction_, "/user/hand/right/output/haptic");
        probe(act(IA::Select), "/user/hand/left/input/trigger/value");
        probe(act(IA::Select), "/user/hand/right/input/trigger/value");
        probe(act(IA::LaserHand), "/user/hand/left/input/squeeze/click");
        probe(act(IA::LaserHand), "/user/hand/right/input/squeeze/click");
        probe(act(IA::PlayOk), "/user/hand/right/input/a/click");
        probe(act(IA::Back), "/user/hand/right/input/b/click");
        probe(act(IA::Recenter), "/user/hand/right/input/x/click");
        probe(act(IA::Options), "/user/hand/right/input/y/click");
        probe(act(IA::MainMenu), "/user/hand/right/input/menu/click");
        probe(act(IA::ToggleUi), "/user/hand/left/input/view/click");
        probe(act(IA::Previous), "/user/hand/left/input/bumper/click");
        probe(act(IA::Next), "/user/hand/right/input/bumper/click");
        probe(act(IA::DpadUp), "/user/hand/left/input/dpad_up/click");
        probe(act(IA::DpadDown), "/user/hand/left/input/dpad_down/click");
        probe(act(IA::DpadLeft), "/user/hand/left/input/dpad_left/click");
        probe(act(IA::DpadRight), "/user/hand/left/input/dpad_right/click");
        probe(act(IA::StickRight), "/user/hand/right/input/thumbstick");
        probe(act(IA::StickLeft), "/user/hand/left/input/thumbstick");
        probe(act(IA::ShowControls), "/user/hand/left/input/thumbstick/click");
        probe(act(IA::ShowControls), "/user/hand/right/input/thumbstick/click");
        if (!frameBinds.empty()) {
            XrInteractionProfileSuggestedBinding s{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
            s.interactionProfile = frameProfile;
            s.suggestedBindings = frameBinds.data();
            s.countSuggestedBindings = static_cast<uint32_t>(frameBinds.size());
            if (XR_SUCCEEDED(xrSuggestInteractionProfileBindings(instance_, &s))) {
                PIXELVR_LOG_INFO("Steam Frame native profile: %zu bindings accepted, %d rejected",
                                 frameBinds.size(), rejected);
            } else {
                PIXELVR_LOG_ERROR("Steam Frame native profile final suggest failed");
            }
        }
    }

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
    if (gripPoseAction_ != XR_NULL_HANDLE) {
        for (int h = 0; h < 2; ++h) {
            XrActionSpaceCreateInfo si{XR_TYPE_ACTION_SPACE_CREATE_INFO};
            si.action = gripPoseAction_;
            si.subactionPath = handPaths_[h];
            si.poseInActionSpace.orientation.w = 1.0f;
            if (XR_FAILED(xrCreateActionSpace(session_, &si, &gripSpaces_[h]))) {
                PIXELVR_LOG_WARN("Failed to create grip action space %d", h);
                gripSpaces_[h] = XR_NULL_HANDLE;
            }
        }
    }

    actionsReady_ = true;
    PIXELVR_LOG_INFO("Controller input ready: action set \"Player Controls\", %d actions "
                     "(buttons are assigned in SteamVR)",
                     static_cast<int>(InputAction::Count));
    return true;
}

void XrContext::syncActions() {
    // Every edge is cleared each frame, the full grip press included.
    input_.select = false;
    input_.selectHeld = false;
    input_.clearButtons();
    input_.comboEdge = -1;
    input_.thumbstickX = 0.0f;
    input_.thumbstickY = 0.0f;
    input_.thumbstickLX = 0.0f;
    input_.thumbstickLY = 0.0f;
    input_.thumbstickRX = 0.0f;
    input_.thumbstickRY = 0.0f;
    input_.gripFullPressDown[0] = false;
    input_.gripFullPressDown[1] = false;
    if (!actionsReady_) {
        return;
    }

    if (!sessionFocused()) {
        // Yield controller input and clear all held inputs when the session does not have focus (e.g. Steam dashboard).
        for (int h = 0; h < 2; ++h) {
            input_.gripValue[h] = 0.0f;
            input_.gripFullPressHeld[h] = false;
            prevFull_[h] = false;
        }
        return;
    }

    XrActiveActionSet active{actionSet_, XR_NULL_PATH};
    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    if (XR_FAILED(xrSyncActions(session_, &sync))) {
        return;  // Not focused yet: no input this frame.
    }
    if (bindingsDirty_) {  // bindings can be queried now that a focused sync ran
        bindingsDirty_ = false;
        ++bindingsGeneration_;
    }
    // A controller waking up (or going to sleep) changes what can be listed;
    // the profile-changed event normally says so, this is the backstop (~1 Hz).
    if (++handCheckFrames_ >= 90) {
        handCheckFrames_ = 0;
        for (int h = 0; h < 2; ++h) {
            const bool on = handActive(h);
            if (on != lastHandActive_[h]) {
                lastHandActive_[h] = on;
                bindingsDirty_ = true;
            }
        }
    }

    // One-shot: log which interaction profile the runtime actually bound, so we
    // can confirm the native Steam Frame profile is active (vs a remap).
    if (!profileLogged_) {
        bool allBound = true;
        for (int h = 0; h < 2; ++h) {
            if (profileLoggedHand_[h]) continue;
            XrInteractionProfileState ips{XR_TYPE_INTERACTION_PROFILE_STATE};
            if (XR_SUCCEEDED(xrGetCurrentInteractionProfile(
                    session_, handPaths_[h], &ips)) &&
                ips.interactionProfile != XR_NULL_PATH) {
                char buf[XR_MAX_PATH_LENGTH];
                uint32_t len = 0;
                if (XR_SUCCEEDED(xrPathToString(instance_, ips.interactionProfile,
                                                sizeof(buf), &len, buf))) {
                    PIXELVR_LOG_INFO("Active interaction profile [%s]: %s",
                                     h == 0 ? "left" : "right", buf);
                    profileLoggedHand_[h] = true;  // a hand never bound (one controller) retries quietly
                }
            } else {
                allBound = false;  // retry next frame until the runtime binds it
            }
        }
        profileLogged_ = allBound;
    }

    using IA = InputAction;
    // held: current level; returns the rising edge.
    auto read_bool = [&](XrAction a, bool* held = nullptr) -> bool {
        if (a == XR_NULL_HANDLE) return false;
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = a;
        XrActionStateBoolean st{XR_TYPE_ACTION_STATE_BOOLEAN};
        if (XR_FAILED(xrGetActionStateBoolean(session_, &gi, &st)) || !st.isActive) {
            return false;
        }
        if (held) *held = st.currentState;
        return st.currentState && st.changedSinceLastSync;
    };
    input_.select = read_bool(act(IA::Select), &input_.selectHeld);
    input_.togglePlay = read_bool(act(IA::PlayOk));
    input_.back = read_bool(act(IA::Back));
    input_.mainMenu = read_bool(act(IA::MainMenu));
    input_.options = read_bool(act(IA::Options));
    input_.recenter = read_bool(act(IA::Recenter));
    input_.toggleUi = read_bool(act(IA::ToggleUi));
    input_.previous = read_bool(act(IA::Previous));
    input_.next = read_bool(act(IA::Next));
    input_.dpadUp = read_bool(act(IA::DpadUp), &input_.dpadUpHeld);
    input_.dpadDown = read_bool(act(IA::DpadDown), &input_.dpadDownHeld);
    input_.dpadLeft = read_bool(act(IA::DpadLeft), &input_.dpadLeftHeld);
    input_.dpadRight = read_bool(act(IA::DpadRight), &input_.dpadRightHeld);
    input_.showControls = read_bool(act(IA::ShowControls));
    input_.mute = read_bool(act(IA::Mute));
    input_.stop = read_bool(act(IA::Stop));
    input_.bookmarks = read_bool(act(IA::Bookmarks));
    input_.nextScreen = read_bool(act(IA::NextScreen));
    input_.previousScreen = read_bool(act(IA::PreviousScreen));
    input_.skipBack = read_bool(act(IA::SkipBack));
    input_.skipForward = read_bool(act(IA::SkipForward));
    input_.lockScreens = read_bool(act(IA::LockScreens));
    input_.audioTrack = read_bool(act(IA::AudioTrack));
    input_.format3d = read_bool(act(IA::Format3d));

    // The unlock combo's symbols are the four face actions (A B X Y on the
    // default bindings), taken before any context consumes them.
    if (input_.togglePlay) input_.comboEdge = XrInputState::ComboPlay;
    else if (input_.back) input_.comboEdge = XrInputState::ComboBack;
    else if (input_.recenter) input_.comboEdge = XrInputState::ComboRecenter;
    else if (input_.options) input_.comboEdge = XrInputState::ComboOptions;

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
    read_stick(act(IA::StickLeft), input_.thumbstickLX, input_.thumbstickLY);
    read_stick(act(IA::StickRight), input_.thumbstickRX, input_.thumbstickRY);
    // Combined stick (menus, distance, keyboard): the longer of the two, the
    // rule OpenXR itself uses for one vector2 action bound to two sticks.
    const float l2 = input_.thumbstickLX * input_.thumbstickLX + input_.thumbstickLY * input_.thumbstickLY;
    const float r2 = input_.thumbstickRX * input_.thumbstickRX + input_.thumbstickRY * input_.thumbstickRY;
    input_.thumbstickX = (r2 > l2) ? input_.thumbstickRX : input_.thumbstickLX;
    input_.thumbstickY = (r2 > l2) ? input_.thumbstickRY : input_.thumbstickLY;

    // "Laser Hand": the discrete squeeze click, else the analog squeeze value.
    for (int h = 0; h < 2; ++h) {
        float sqVal = 0.0f;
        if (squeezeValueAction_ != XR_NULL_HANDLE) {
            XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
            gi.action = squeezeValueAction_;
            gi.subactionPath = handPaths_[h];
            XrActionStateFloat st{XR_TYPE_ACTION_STATE_FLOAT};
            if (XR_SUCCEEDED(xrGetActionStateFloat(session_, &gi, &st)) && st.isActive) {
                sqVal = st.currentState;
            }
        }
        input_.gripValue[h] = sqVal;

        bool fullHeld = false;
        bool fullDown = false;
        if (act(IA::LaserHand) != XR_NULL_HANDLE) {
            XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
            gi.action = act(IA::LaserHand);
            gi.subactionPath = handPaths_[h];
            XrActionStateBoolean st{XR_TYPE_ACTION_STATE_BOOLEAN};
            if (XR_SUCCEEDED(xrGetActionStateBoolean(session_, &gi, &st)) && st.isActive) {
                fullHeld = st.currentState;
                fullDown = st.currentState && st.changedSinceLastSync;
            }
        }
        // Fallback for hardware profiles where squeeze/click is not exposed separately:
        // Use a strict bottom-out threshold (>= 0.90f to engage, < 0.80f to release)
        if (!fullHeld) {
            if (prevFull_[h]) {
                if (sqVal >= 0.80f) {
                    fullHeld = true;
                } else {
                    prevFull_[h] = false;
                }
            } else {
                if (sqVal >= 0.90f) {
                    fullHeld = true;
                    fullDown = true;
                    prevFull_[h] = true;
                }
            }
        }
        input_.gripFullPressHeld[h] = fullHeld;
        input_.gripFullPressDown[h] = fullDown;
    }
}

std::string XrContext::describeBinding(InputAction a) {
    const int i = static_cast<int>(a);
    if (i < 0 || i >= static_cast<int>(InputAction::Count)) return std::string();
    if (!actionsReady_ || session_ == XR_NULL_HANDLE) return "See SteamVR bindings";
    std::vector<XrAction> acts{actions_[i]};
    if (a == InputAction::LaserHand) acts.push_back(squeezeValueAction_);  // its analog fallback
    std::vector<std::string> names;
    std::vector<std::string> raw;  // "<path> -> <runtime name>", logged when the result changes
    bool failed = false;
    for (XrAction x : acts) {
        if (x == XR_NULL_HANDLE) continue;
        XrBoundSourcesForActionEnumerateInfo info{XR_TYPE_BOUND_SOURCES_FOR_ACTION_ENUMERATE_INFO};
        info.action = x;
        uint32_t n = 0;
        if (XR_FAILED(xrEnumerateBoundSourcesForAction(session_, &info, 0, &n, nullptr))) {
            failed = true;
            continue;
        }
        std::vector<XrPath> sources(n);
        if (n > 0 && XR_FAILED(xrEnumerateBoundSourcesForAction(session_, &info, n, &n, sources.data()))) {
            failed = true;
            continue;
        }
        sources.resize(n);
        for (XrPath p : sources) {
            XrInputSourceLocalizedNameGetInfo gi{XR_TYPE_INPUT_SOURCE_LOCALIZED_NAME_GET_INFO};
            gi.sourcePath = p;
            gi.whichComponents = XR_INPUT_SOURCE_LOCALIZED_NAME_USER_PATH_BIT |
                                 XR_INPUT_SOURCE_LOCALIZED_NAME_COMPONENT_BIT;
            uint32_t len = 0;
            if (XR_FAILED(xrGetInputSourceLocalizedName(session_, &gi, 0, &len, nullptr)) || len == 0) {
                failed = true;
                continue;
            }
            std::string s(len, '\0');
            if (XR_FAILED(xrGetInputSourceLocalizedName(session_, &gi, len, &len, s.data()))) {
                failed = true;
                continue;
            }
            s.resize(std::strlen(s.c_str()));
            char pb[XR_MAX_PATH_LENGTH] = {};
            uint32_t pl = 0;
            if (XR_FAILED(xrPathToString(instance_, p, sizeof(pb), &pl, pb))) pb[0] = '\0';
            raw.push_back(std::string(pb) + " -> \"" + s + "\"");
            const std::string shortName = shortenSourceName(s);
            if (!shortName.empty() && std::find(names.begin(), names.end(), shortName) == names.end()) {
                names.push_back(shortName);
            }
        }
    }
    std::string out;
    if (!names.empty()) {
        for (const std::string& nm : names) out += (out.empty() ? "" : ", ") + nm;
    } else if (failed) {
        out = "See SteamVR bindings";
    } else {
        // SteamVR lists sources of active controllers only: when every hand our
        // default puts this action on is off, say so instead of "Not bound".
        const unsigned hands = frameSuggestHands_[i];
        const bool leftOff = (hands & 1u) && !handActive(0);
        const bool rightOff = (hands & 2u) && !handActive(1);
        const bool anyOn = ((hands & 1u) && !leftOff) || ((hands & 2u) && !rightOff);
        if (hands != 0 && !anyOn) {
            out = (leftOff && rightOff) ? "Controllers off" : (leftOff ? "Left controller off" : "Right controller off");
        } else {
            out = "Not bound";
        }
    }
    // Logged whenever the result changes (SteamVR can swap bindings at any time).
    if (out != lastBinding_[i]) {
        for (const std::string& r : raw) PIXELVR_LOG_INFO("Binding [%s]: %s", kActionDefs[i].name, r.c_str());
        PIXELVR_LOG_INFO("Binding [%s] = \"%s\"", kActionDefs[i].name, out.c_str());
        lastBinding_[i] = out;
    }
    return out;
}

std::vector<std::string> XrContext::boundSourcePaths(InputAction a) {
    std::vector<std::string> out;
    const int i = static_cast<int>(a);
    if (i < 0 || i >= static_cast<int>(InputAction::Count) || !actionsReady_ || session_ == XR_NULL_HANDLE) {
        return out;
    }
    std::vector<XrAction> acts{actions_[i]};
    if (a == InputAction::LaserHand) acts.push_back(squeezeValueAction_);
    for (XrAction x : acts) {
        if (x == XR_NULL_HANDLE) continue;
        XrBoundSourcesForActionEnumerateInfo info{XR_TYPE_BOUND_SOURCES_FOR_ACTION_ENUMERATE_INFO};
        info.action = x;
        uint32_t n = 0;
        if (XR_FAILED(xrEnumerateBoundSourcesForAction(session_, &info, 0, &n, nullptr)) || n == 0) continue;
        std::vector<XrPath> sources(n);
        if (XR_FAILED(xrEnumerateBoundSourcesForAction(session_, &info, n, &n, sources.data()))) continue;
        sources.resize(n);
        for (XrPath p : sources) {
            char pb[XR_MAX_PATH_LENGTH] = {};
            uint32_t pl = 0;
            if (XR_FAILED(xrPathToString(instance_, p, sizeof(pb), &pl, pb)) || pb[0] == '\0') continue;
            std::string s(pb);
            if (std::find(out.begin(), out.end(), s) == out.end()) out.push_back(std::move(s));
        }
    }
    return out;
}

bool XrContext::handActive(int hand) const {
    if (session_ == XR_NULL_HANDLE || !actionsReady_ || hand < 0 || hand > 1) return false;
    XrInteractionProfileState ips{XR_TYPE_INTERACTION_PROFILE_STATE};
    return XR_SUCCEEDED(xrGetCurrentInteractionProfile(session_, handPaths_[hand], &ips)) &&
           ips.interactionProfile != XR_NULL_PATH;
}

void XrContext::triggerHaptic(int hand, float durationSec, float frequencyHz, float amplitude) {
    if (session_ == XR_NULL_HANDLE || hapticAction_ == XR_NULL_HANDLE) return;
    if (hand < 0 || hand >= 2 || handPaths_[hand] == XR_NULL_PATH) return;

    XrHapticVibration vib{XR_TYPE_HAPTIC_VIBRATION};
    vib.duration = static_cast<XrDuration>(durationSec * 1e9f);
    vib.frequency = frequencyHz;
    vib.amplitude = std::clamp(amplitude, 0.0f, 1.0f);

    XrHapticActionInfo hai{XR_TYPE_HAPTIC_ACTION_INFO};
    hai.action = hapticAction_;
    hai.subactionPath = handPaths_[hand];
    xrApplyHapticFeedback(session_, &hai, reinterpret_cast<const XrHapticBaseHeader*>(&vib));
}

void XrContext::handleSessionStateChange(const XrEventDataSessionStateChanged& ev,
                                         bool& exitRequested) {
    sessionState_ = ev.state;
    // FOCUSED = the app receives controller input; VISIBLE = SteamVR's system
    // layer (dashboard, keyboard, laser-mouse panel mode) holds the controllers.
    static const char* const kStateNames[] = {"UNKNOWN", "IDLE", "READY", "SYNCHRONIZED",
                                              "VISIBLE", "FOCUSED", "STOPPING",
                                              "LOSS_PENDING", "EXITING"};
    const int stateIdx = static_cast<int>(sessionState_);
    PIXELVR_LOG_INFO("Session state: %s",
                     (stateIdx >= 0 && stateIdx <= 8) ? kStateNames[stateIdx] : "?");

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
        case XR_SESSION_STATE_FOCUSED:
            // Bindings are edited in the dashboard while we are unfocused.
            bindingsDirty_ = true;
            break;
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
            case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
                PIXELVR_LOG_INFO("Interaction profile changed");
                bindingsDirty_ = true;  // the Controls page re-reads the bindings
                break;
            default:
                break;
        }
    }
    return true;
}

bool XrContext::userPresent() const {
    if (!sessionRunning_) return false;
    if (sessionState_ == XR_SESSION_STATE_STOPPING ||
        sessionState_ == XR_SESSION_STATE_IDLE ||
        sessionState_ == XR_SESSION_STATE_SYNCHRONIZED) {
        return false;
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
        if (!lastValidViews_.empty() && lastValidViewsTime_ > 0 &&
            displayTime > lastValidViewsTime_ &&
            (displayTime - lastValidViewsTime_) < 250000000LL) {
            views = lastValidViews_;
            ++viewFallbackStats_.poseFullFallbackFrames;
            shouldRender = true;
            return true;
        }
        shouldRender = false;
        return false;
    }

    const bool orientValid = (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;
    const bool posValid = (viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0;

    if (orientValid && posValid) {
        lastValidViews_ = views;
        lastValidViewsTime_ = displayTime;
        shouldRender = true;
        return true;
    }

    if (orientValid && !posValid && !lastValidViews_.empty()) {
        // Keep fresh orientation and fov, but borrow position from last valid pose
        for (size_t i = 0; i < views.size() && i < lastValidViews_.size(); ++i) {
            views[i].pose.position = lastValidViews_[i].pose.position;
        }
        ++viewFallbackStats_.posePartialFrames;
        shouldRender = true;
        return true;
    }

    if (!lastValidViews_.empty() && lastValidViewsTime_ > 0 &&
        displayTime > lastValidViewsTime_ &&
        (displayTime - lastValidViewsTime_) < 250000000LL) {
        views = lastValidViews_;
        ++viewFallbackStats_.poseFullFallbackFrames;
        shouldRender = true;
        return true;
    }

    shouldRender = false;
    return true;
}

bool XrContext::renderFrame(const RenderViewFn& renderView, const RenderLayersFn& beforeViews,
                            const RenderLayersFn& afterViews) {
    XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState frameState{XR_TYPE_FRAME_STATE};
    PIXELVR_XR_CHECK(instance_, xrWaitFrame(session_, &waitInfo, &frameState));

    XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
    PIXELVR_XR_CHECK(instance_, xrBeginFrame(session_, &beginInfo));

    syncActions();

    std::vector<XrCompositionLayerProjectionView> projectionViews;
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};

    lastDisplayTime_ = frameState.predictedDisplayTime;
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

    input_.stageValid = false;
    if (stageSpace_ != XR_NULL_HANDLE && input_.headValid) {
        XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
        if (XR_SUCCEEDED(xrLocateSpace(appSpace_, stageSpace_, frameState.predictedDisplayTime, &loc))) {
            constexpr XrSpaceLocationFlags need =
                XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
                XR_SPACE_LOCATION_POSITION_VALID_BIT;
            if ((loc.locationFlags & need) == need) {
                input_.stageValid = true;
                input_.appToStagePose = loc.pose;
                transformToStage(input_.headPos, input_.headForward,
                                 input_.headPosStage, input_.headForwardStage);
            }
        }
    }
    if (!input_.stageValid) {
        // Fallback: Standing stage eye level is ~1.65m above the physical floor
        input_.headPosStage[0] = input_.headPos[0];
        input_.headPosStage[1] = input_.headPos[1] + 1.65f;
        input_.headPosStage[2] = input_.headPos[2];
        input_.headForwardStage[0] = input_.headForward[0];
        input_.headForwardStage[1] = input_.headForward[1];
        input_.headForwardStage[2] = input_.headForward[2];
    }

    // Locate each controller's aim pose (for the pointer rays and models).
    for (int h = 0; h < 2; ++h) {
        input_.controllers[h].active = false;
        const bool canTrack = sessionFocused() || VrKeyboard::instance().isOpen();
        if (!canTrack || aimSpaces_[h] == XR_NULL_HANDLE) {
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

    // Locate each controller's grip pose (used to place the 3D controller model;
    // the model is authored around the grip, not the forward-pointing aim).
    for (int h = 0; h < 2; ++h) {
        input_.controllers[h].gripActive = false;
        const bool canTrack = sessionFocused() || VrKeyboard::instance().isOpen();
        if (!canTrack || gripSpaces_[h] == XR_NULL_HANDLE) {
            continue;
        }
        XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
        if (XR_FAILED(xrLocateSpace(gripSpaces_[h], appSpace_,
                                    frameState.predictedDisplayTime, &loc))) {
            continue;
        }
        constexpr XrSpaceLocationFlags need =
            XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
            XR_SPACE_LOCATION_POSITION_VALID_BIT;
        if ((loc.locationFlags & need) == need) {
            auto& c = input_.controllers[h];
            c.gripActive = true;
            c.gripPosition[0] = loc.pose.position.x;
            c.gripPosition[1] = loc.pose.position.y;
            c.gripPosition[2] = loc.pose.position.z;
            c.gripOrientation[0] = loc.pose.orientation.x;
            c.gripOrientation[1] = loc.pose.orientation.y;
            c.gripOrientation[2] = loc.pose.orientation.z;
            c.gripOrientation[3] = loc.pose.orientation.w;
        }
    }

    // Composition layers (UI panel, screens, cursor) collected from the
    // renderer: `beforeViews` first so its GPU work overlaps the eye
    // recording, `afterViews` once the eye passes have refreshed screen poses.
    std::vector<CompositionLayer> extra;
    if (viewsValid && beforeViews) {
        beforeViews(*this, views_.data(), static_cast<uint32_t>(views_.size()), extra);
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

    if (viewsValid && afterViews) {
        afterViews(*this, views_.data(), static_cast<uint32_t>(views_.size()), extra);
    }
    // Build the OpenXR structs (stable addresses: reserved up front).
    std::vector<XrCompositionLayerQuad> quads;
    std::vector<XrCompositionLayerCylinderKHR> cylinders;
    quads.reserve(extra.size());
    cylinders.reserve(extra.size());
    std::vector<XrCompositionLayerBaseHeader*> layers;
    layers.reserve(extra.size() + 1);
    layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer));
    if (maxLayerCount_ == 0) {
        XrSystemProperties props{XR_TYPE_SYSTEM_PROPERTIES};
        if (XR_SUCCEEDED(xrGetSystemProperties(instance_, systemId_, &props)) &&
            props.graphicsProperties.maxLayerCount > 0) {
            maxLayerCount_ = props.graphicsProperties.maxLayerCount;
        } else {
            maxLayerCount_ = XR_MIN_COMPOSITION_LAYERS_SUPPORTED;
        }
        PIXELVR_LOG_INFO("Runtime composites up to %u layers", maxLayerCount_);
    }
    for (const CompositionLayer& cl : extra) {
        if (cl.swapchain == XR_NULL_HANDLE) continue;
        // Past the runtime's limit the frame would be rejected: leave the
        // last layers out (the list is ordered bottom to top) instead.
        if (layers.size() >= maxLayerCount_) {
            if (!layerLimitLogged_) {
                PIXELVR_LOG_WARN("Layer limit (%u) reached: the topmost layers are left out", maxLayerCount_);
                layerLimitLogged_ = true;
            }
            break;
        }
        // Blended layers are rendered with premultiplied alpha (straight-alpha
        // blending onto a cleared transparent target).
        const XrCompositionLayerFlags flags =
            cl.blend ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT : 0;
        XrSwapchainSubImage sub{};
        sub.swapchain = cl.swapchain;
        sub.imageRect = cl.imageRect;
        sub.imageArrayIndex = 0;
        if (cl.cylinder && cylinderLayerEnabled_) {
            XrCompositionLayerCylinderKHR& c = cylinders.emplace_back();
            c = XrCompositionLayerCylinderKHR{XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR};
            c.layerFlags = flags;
            c.space = appSpace_;
            c.eyeVisibility = cl.eye;
            c.subImage = sub;
            c.pose = cl.pose;
            c.radius = cl.radius;
            c.centralAngle = cl.centralAngle;
            c.aspectRatio = cl.aspectRatio;
            layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&c));
        } else {
            XrCompositionLayerQuad& q = quads.emplace_back();
            q = XrCompositionLayerQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
            q.layerFlags = flags;
            q.space = appSpace_;
            q.eyeVisibility = cl.eye;
            q.subImage = sub;
            q.pose = cl.pose;
            q.size = {cl.width, cl.height};
            layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&q));
        }
    }

    XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
    endInfo.displayTime = frameState.predictedDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount = viewsValid ? static_cast<uint32_t>(layers.size()) : 0u;
    endInfo.layers = viewsValid ? layers.data() : nullptr;

    if (!viewsValid && frameState.shouldRender == XR_TRUE) {
        ++viewFallbackStats_.zeroLayerFrames;
    }

    const XrResult endRes = xrEndFrame(session_, &endInfo);
    if (XR_FAILED(endRes)) {
        char buf[XR_MAX_RESULT_STRING_SIZE];
        PIXELVR_LOG_ERROR("xrEndFrame failed: %s", xr_result_string(instance_, endRes, buf));
        return false;
    }
    return true;
}

void XrContext::transformToStage(const float localPos[3], const float localFwd[3],
                                 float stagePos[3], float stageFwd[3]) const {
    const auto& p = input_.appToStagePose.position;
    const auto& q = input_.appToStagePose.orientation;

    if (localPos && stagePos) {
        const float vx = localPos[0], vy = localPos[1], vz = localPos[2];
        const float tw = -q.x * vx - q.y * vy - q.z * vz;
        const float tx =  q.w * vx + q.y * vz - q.z * vy;
        const float ty =  q.w * vy + q.z * vx - q.x * vz;
        const float tz =  q.w * vz + q.x * vy - q.y * vx;
        stagePos[0] = p.x + (tx * q.w + tw * (-q.x) + ty * (-q.z) - tz * (-q.y));
        stagePos[1] = p.y + (ty * q.w + tw * (-q.y) + tz * (-q.x) - tx * (-q.z));
        stagePos[2] = p.z + (tz * q.w + tw * (-q.z) + tx * (-q.y) - ty * (-q.x));
    }
    if (localFwd && stageFwd) {
        const float fx = localFwd[0], fy = localFwd[1], fz = localFwd[2];
        const float ftw = -q.x * fx - q.y * fy - q.z * fz;
        const float ftx =  q.w * fx + q.y * fz - q.z * fy;
        const float fty =  q.w * fy + q.z * fx - q.x * fz;
        const float ftz =  q.w * fz + q.x * fy - q.y * fx;
        stageFwd[0] = ftx * q.w + ftw * (-q.x) + fty * (-q.z) - ftz * (-q.y);
        stageFwd[1] = fty * q.w + ftw * (-q.y) + ftz * (-q.x) - ftx * (-q.z);
        stageFwd[2] = ftz * q.w + ftw * (-q.z) + ftx * (-q.y) - fty * (-q.x);
    }
}

} // namespace pixelvr

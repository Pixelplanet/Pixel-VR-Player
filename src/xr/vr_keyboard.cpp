#include "vr_keyboard.hpp"
#include "util/logging.hpp"
#include <cstring>
#include <cmath>

#if defined(__linux__) || defined(__APPLE__)
#include <dlfcn.h>
#else
#include <windows.h>
#endif

#include "openvr.h"

namespace pixelvr {

namespace {

typedef intptr_t (*Fn_VR_InitInternal)(vr::EVRInitError*, vr::EVRApplicationType);
typedef void (*Fn_VR_ShutdownInternal)();
typedef intptr_t (*Fn_VR_GetGenericInterface)(const char*, vr::EVRInitError*);

Fn_VR_InitInternal g_VR_InitInternal = nullptr;
Fn_VR_ShutdownInternal g_VR_ShutdownInternal = nullptr;
Fn_VR_GetGenericInterface g_VR_GetGenericInterface = nullptr;

} // namespace

VrKeyboard& VrKeyboard::instance() {
    static VrKeyboard sInstance;
    return sInstance;
}

VrKeyboard::~VrKeyboard() {
    shutdown();
}

bool VrKeyboard::init() {
    if (available_) return true;

#if defined(__linux__)
    const char* paths[] = {
        "/opt/steamvr/bin/linuxarm64/libopenvr_api.so",
        "/opt/steamvr/bin/linux64/libopenvr_api.so",
        "libopenvr_api.so"
    };
    for (const char* path : paths) {
        libHandle_ = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (libHandle_) {
            PIXELVR_LOG_INFO("VrKeyboard: Loaded %s", path);
            break;
        }
    }
    if (!libHandle_) {
        PIXELVR_LOG_WARN("VrKeyboard: Could not find or load libopenvr_api.so: %s", dlerror());
        return false;
    }

    g_VR_InitInternal = reinterpret_cast<Fn_VR_InitInternal>(dlsym(libHandle_, "VR_InitInternal"));
    g_VR_ShutdownInternal = reinterpret_cast<Fn_VR_ShutdownInternal>(dlsym(libHandle_, "VR_ShutdownInternal"));
    g_VR_GetGenericInterface = reinterpret_cast<Fn_VR_GetGenericInterface>(dlsym(libHandle_, "VR_GetGenericInterface"));
#elif defined(_WIN32)
    HMODULE hMod = LoadLibraryA("openvr_api.dll");
    if (!hMod) {
        return false;
    }
    libHandle_ = hMod;
    g_VR_InitInternal = reinterpret_cast<Fn_VR_InitInternal>(GetProcAddress(hMod, "VR_InitInternal"));
    g_VR_ShutdownInternal = reinterpret_cast<Fn_VR_ShutdownInternal>(GetProcAddress(hMod, "VR_ShutdownInternal"));
    g_VR_GetGenericInterface = reinterpret_cast<Fn_VR_GetGenericInterface>(GetProcAddress(hMod, "VR_GetGenericInterface"));
#else
    return false;
#endif

    if (!g_VR_InitInternal || !g_VR_ShutdownInternal || !g_VR_GetGenericInterface) {
        PIXELVR_LOG_WARN("VrKeyboard: OpenVR symbols not resolved");
        shutdown();
        return false;
    }

    vr::EVRInitError err = vr::VRInitError_None;
    uint32_t token = static_cast<uint32_t>(g_VR_InitInternal(&err, vr::VRApplication_Overlay));
    if (token == 0 || err != vr::VRInitError_None) {
        PIXELVR_LOG_WARN("VrKeyboard: OpenVR overlay init returned %d", static_cast<int>(err));
        shutdown();
        return false;
    }

    vr::IVRSystem* sys = reinterpret_cast<vr::IVRSystem*>(
        g_VR_GetGenericInterface(vr::IVRSystem_Version, &err));
    if (!sys || err != vr::VRInitError_None) {
        PIXELVR_LOG_WARN("VrKeyboard: Failed to get IVRSystem interface %s (err=%d)",
                         vr::IVRSystem_Version, static_cast<int>(err));
        shutdown();
        return false;
    }

    vr::IVROverlay* ov = reinterpret_cast<vr::IVROverlay*>(
        g_VR_GetGenericInterface(vr::IVROverlay_Version, &err));
    if (!ov || err != vr::VRInitError_None) {
        PIXELVR_LOG_WARN("VrKeyboard: Failed to get IVROverlay interface %s (err=%d)",
                         vr::IVROverlay_Version, static_cast<int>(err));
        shutdown();
        return false;
    }

    vrSystem_ = sys;
    vrOverlay_ = ov;
    available_ = true;

    PIXELVR_LOG_INFO("VrKeyboard: Successfully initialized SteamVR overlay interface (sys=%p, ov=%p)",
                     reinterpret_cast<void*>(sys), reinterpret_cast<void*>(ov));
    return true;
}

void VrKeyboard::shutdown() {
    if (!available_) return;
    available_ = false;
    if (isOpen_) {
        hide();
    }
    if (g_VR_ShutdownInternal) {
        auto fn = g_VR_ShutdownInternal;
        g_VR_ShutdownInternal = nullptr;
        fn();
    }
    vrSystem_ = nullptr;
    vrOverlay_ = nullptr;
    if (libHandle_) {
#if defined(__linux__) || defined(__APPLE__)
        dlclose(libHandle_);
#elif defined(_WIN32)
        FreeLibrary(reinterpret_cast<HMODULE>(libHandle_));
#endif
        libHandle_ = nullptr;
    }
    g_VR_InitInternal = nullptr;
    g_VR_GetGenericInterface = nullptr;
    isOpen_ = false;
    doneSubmitted_ = false;
    kbConfigured_ = false;
}

bool VrKeyboard::configureKeyboardOverlay() {
    auto* ov = reinterpret_cast<vr::IVROverlay*>(vrOverlay_);
    if (!ov) return false;

    bool foundAny = false;
    vr::HmdMatrix34_t mat{};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            mat.m[r][c] = kbMat_[r][c];
        }
    }

    auto configureHandle = [&](vr::VROverlayHandle_t h, const char* name) {
        ov->SetOverlayTransformAbsolute(h, vr::TrackingUniverseStanding, &mat);
        ov->SetOverlayWidthInMeters(h, 1.05f);
        ov->SetOverlayInputMethod(h, vr::VROverlayInputMethod_Mouse);
        ov->SetOverlayFlag(h, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, true);
        ov->SetOverlayFlag(h, vr::VROverlayFlags_SendVRDiscreteScrollEvents, true);
        ov->ShowOverlay(h);
        foundAny = true;
        PIXELVR_LOG_INFO("VrKeyboard: Configured keyboard overlay %llu ('%s') with interactive laser mouse and standing transform",
                         static_cast<unsigned long long>(h), name ? name : "unknown");
    };

    // 1. Check known specific overlay keys
    const char* kbOverlayNames[] = {
        "VRKeyboard_uid0",
        "VRKeyboard_uid1",
        "VRKeyboard",
        "valve.steam.gamepadui.keyboard",
        "system.keyboard"
    };
    for (const char* name : kbOverlayNames) {
        vr::VROverlayHandle_t hKb = vr::k_ulOverlayHandleInvalid;
        if (ov->FindOverlay(name, &hKb) == vr::VROverlayError_None && hKb != vr::k_ulOverlayHandleInvalid) {
            configureHandle(hKb, name);
        }
    }

    // 2. Scan active overlay handles (1..128) to catch any dynamic uid variations (e.g. VRKeyboard_uid2726460308)
    for (uint64_t h = 1; h <= 128; ++h) {
        char keyBuf[256] = {0};
        vr::EVROverlayError kErr = vr::VROverlayError_None;
        uint32_t len = ov->GetOverlayKey(h, keyBuf, sizeof(keyBuf), &kErr);
        if (kErr == vr::VROverlayError_None && len > 0) {
            if (std::strstr(keyBuf, "keyboard") != nullptr ||
                std::strstr(keyBuf, "Keyboard") != nullptr ||
                std::strstr(keyBuf, "VRKeyboard") != nullptr) {
                configureHandle(h, keyBuf);
            }
        }
    }

    if (foundAny) {
        kbConfigured_ = true;
    }
    return foundAny;
}

bool VrKeyboard::show(const std::string& description,
                      const std::string& existingText,
                      bool isPassword,
                      int maxChars,
                      const float* headPos,
                      const float* headForward) {
    if (!available_ && !init()) {
        return false;
    }
    auto* sys = reinterpret_cast<vr::IVRSystem*>(vrSystem_);
    auto* ov = reinterpret_cast<vr::IVROverlay*>(vrOverlay_);
    if (!ov) return false;

    description_ = description;
    existingText_ = existingText;
    currentText_ = existingText;
    isPassword_ = isPassword;
    doneSubmitted_ = false;

    const vr::EGamepadTextInputMode mode = isPassword
        ? vr::k_EGamepadTextInputModePassword
        : vr::k_EGamepadTextInputModeNormal;

    // Calculate keyboard world matrix in TrackingUniverseStanding
    float hx = 0.0f, hy = 1.6f, hz = 0.0f;
    float fx = 0.0f, fz = -1.0f;

    if (headPos && headForward) {
        hx = headPos[0];
        hy = headPos[1];
        hz = headPos[2];
        fx = headForward[0];
        fz = headForward[2];
    } else if (sys) {
        vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
        sys->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0.0f, poses, vr::k_unMaxTrackedDeviceCount);
        if (poses[vr::k_unTrackedDeviceIndex_Hmd].bPoseIsValid) {
            const auto& hMat = poses[vr::k_unTrackedDeviceIndex_Hmd].mDeviceToAbsoluteTracking;
            hx = hMat.m[0][3];
            hy = hMat.m[1][3];
            hz = hMat.m[2][3];
            // OpenVR forward is -Z (col 2 of rotation matrix)
            fx = -hMat.m[0][2];
            fz = -hMat.m[2][2];
        }
    }

    // Flatten forward vector to horizontal plane
    float flen = std::sqrt(fx * fx + fz * fz);
    if (flen < 1e-3f) {
        fx = 0.0f;
        fz = -1.0f;
    } else {
        fx /= flen;
        fz /= flen;
    }
    const float yaw = std::atan2(-fx, -fz);

    // Place keyboard 1.05m in front of eyes, 0.35m below eye level
    const float dist = 1.05f;
    const float kbX = hx + fx * dist;
    const float kbY = hy - 0.35f;
    const float kbZ = hz + fz * dist;

    // Tilt upward toward eyes by ~22 degrees (-0.38 rad).
    // Top side slants backward away from user; face normal points up and toward eyes.
    const float tilt = -0.38f;
    vr::HmdMatrix34_t kbMat{};
    kbMat.m[0][0] = std::cos(yaw);
    kbMat.m[0][1] = std::sin(yaw) * std::sin(tilt);
    kbMat.m[0][2] = std::sin(yaw) * std::cos(tilt);
    kbMat.m[0][3] = kbX;

    kbMat.m[1][0] = 0.0f;
    kbMat.m[1][1] = std::cos(tilt);
    kbMat.m[1][2] = -std::sin(tilt);
    kbMat.m[1][3] = kbY;

    kbMat.m[2][0] = -std::sin(yaw);
    kbMat.m[2][1] = std::cos(yaw) * std::sin(tilt);
    kbMat.m[2][2] = std::cos(yaw) * std::cos(tilt);
    kbMat.m[2][3] = kbZ;

    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            kbMat_[r][c] = kbMat.m[r][c];
        }
    }
    kbConfigured_ = false;

    // 1. Set keyboard transform in standing universe BEFORE ShowKeyboard
    ov->SetKeyboardTransformAbsolute(vr::TrackingUniverseStanding, &kbMat);

    // 2. Invoke virtual keyboard
    vr::EVROverlayError err = ov->ShowKeyboard(
        mode,
        vr::k_EGamepadTextInputLineModeSingleLine,
        vr::KeyboardFlag_Modal,
        description.c_str(),
        static_cast<uint32_t>(maxChars),
        existingText.c_str(),
        0);

    if (err != vr::VROverlayError_None) {
        PIXELVR_LOG_WARN("VrKeyboard: ShowKeyboard failed with error %d", static_cast<int>(err));
        return false;
    }

    // 3. Set keyboard transform AFTER ShowKeyboard as well
    ov->SetKeyboardTransformAbsolute(vr::TrackingUniverseStanding, &kbMat);

    // 4. Configure overlay laser and transform immediately if already present
    configureKeyboardOverlay();

    isOpen_ = true;
    pollFramesSinceOpen_ = 0;
    PIXELVR_LOG_INFO("VrKeyboard: Shown virtual keyboard for '%s' at pos (%.2f, %.2f, %.2f) yaw=%.2f",
                     description.c_str(), kbX, kbY, kbZ, yaw);
    return true;
}

void VrKeyboard::hide() {
    if (!isOpen_) return;
    auto* ov = reinterpret_cast<vr::IVROverlay*>(vrOverlay_);
    if (ov) {
        ov->HideKeyboard();
    }
    isOpen_ = false;
    kbConfigured_ = false;
    doneSubmitted_ = false;
    PIXELVR_LOG_INFO("VrKeyboard: Hidden virtual keyboard");
}

void VrKeyboard::poll(std::function<void(const std::string&)> onDone,
                      std::function<void()> onCancel) {
    if (!available_ || !isOpen_) return;
    auto* sys = reinterpret_cast<vr::IVRSystem*>(vrSystem_);
    auto* ov = reinterpret_cast<vr::IVROverlay*>(vrOverlay_);
    if (!sys || !ov) return;

    vr::VREvent_t event{};
    while (sys->PollNextEvent(&event, sizeof(event))) {
        if (event.eventType == vr::VREvent_KeyboardCharInput) {
            char keyBuf[1024] = {0};
            uint32_t len = ov->GetKeyboardText(keyBuf, sizeof(keyBuf));
            if (len > 0) {
                currentText_ = std::string(keyBuf, len < sizeof(keyBuf) ? len : std::strlen(keyBuf));
            } else {
                // Fallback: check event.data.keyboard.cNewInput
                const char* inStr = event.data.keyboard.cNewInput;
                if (inStr && inStr[0] != 0) {
                    const unsigned char firstByte = static_cast<unsigned char>(inStr[0]);
                    if (firstByte == '\b' || firstByte == 0x7F || firstByte == 8) {
                        while (!currentText_.empty() && (static_cast<unsigned char>(currentText_.back()) & 0xC0) == 0x80) {
                            currentText_.pop_back();
                        }
                        if (!currentText_.empty()) {
                            currentText_.pop_back();
                        }
                    } else if (firstByte >= 32) {
                        currentText_ += inStr;
                    }
                }
            }
            PIXELVR_LOG_INFO("VrKeyboard: CharInput live text: '%s'", currentText_.c_str());
        } else if (event.eventType == vr::VREvent_KeyboardDone) {
            isOpen_ = false;
            kbConfigured_ = false;
            doneSubmitted_ = true;
            char buf[1024] = {0};
            uint32_t len = ov->GetKeyboardText(buf, sizeof(buf));
            std::string text = currentText_;
            if (len > 0) {
                text = std::string(buf, len < sizeof(buf) ? len : std::strlen(buf));
            }
            PIXELVR_LOG_INFO("VrKeyboard: User submitted text via KeyboardDone: '%s'", text.c_str());
            if (onDone) {
                onDone(text);
            }
        } else if (event.eventType == vr::VREvent_KeyboardClosed ||
                   event.eventType == vr::VREvent_KeyboardClosed_Global) {
            isOpen_ = false;
            kbConfigured_ = false;
            PIXELVR_LOG_INFO("VrKeyboard: Keyboard closed (doneSubmitted=%d)", doneSubmitted_ ? 1 : 0);
            if (!doneSubmitted_) {
                // On Steam Frame / SteamOS, pressing the checkmark or closing the keyboard issues KeyboardClosed
                // without KeyboardDone. Read the buffer and treat non-empty / modified input as submit!
                char buf[1024] = {0};
                uint32_t len = ov->GetKeyboardText(buf, sizeof(buf));
                std::string text = currentText_;
                if (len > 0) {
                    text = std::string(buf, len < sizeof(buf) ? len : std::strlen(buf));
                }
                if (!text.empty() || text != existingText_) {
                    PIXELVR_LOG_INFO("VrKeyboard: Treating KeyboardClosed as submit with text: '%s'", text.c_str());
                    doneSubmitted_ = true;
                    if (onDone) {
                        onDone(text);
                    }
                } else {
                    PIXELVR_LOG_INFO("VrKeyboard: Keyboard cancelled without text change");
                    if (onCancel) {
                        onCancel();
                    }
                }
            }
        }
    }

    // Keep currentText_ synchronized with the live SteamVR keyboard buffer every frame
    if (isOpen_) {
        pollFramesSinceOpen_++;
        // Maintain and reinforce keyboard overlay configuration across the first 90 frames (~1 sec)
        // to catch asynchronous creation by SteamVR's GamepadUI
        if (!kbConfigured_ || pollFramesSinceOpen_ < 90) {
            configureKeyboardOverlay();
        }

        char liveBuf[1024] = {0};
        uint32_t liveLen = ov->GetKeyboardText(liveBuf, sizeof(liveBuf));
        if (liveLen > 0) {
            currentText_ = std::string(liveBuf, liveLen < sizeof(liveBuf) ? liveLen : std::strlen(liveBuf));
        }
    }
}

std::string VrKeyboard::currentText() const {
    return currentText_;
}

} // namespace pixelvr

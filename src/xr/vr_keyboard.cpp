#include "vr_keyboard.hpp"
#include "util/logging.hpp"
#include <cstring>

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

    // Create a persistent input overlay so ShowKeyboardForOverlay can link
    // tracked controller pointer interactions directly to SteamVR's virtual keyboard.
    vr::VROverlayHandle_t hOverlay = vr::k_ulOverlayHandleInvalid;
    vr::EVROverlayError ovErr = ov->CreateOverlay("pixelvr.input_overlay", "PixelVR Input", &hOverlay);
    if (ovErr == vr::VROverlayError_None && hOverlay != vr::k_ulOverlayHandleInvalid) {
        inputOverlay_ = hOverlay;
        ov->SetOverlayInputMethod(hOverlay, vr::VROverlayInputMethod_Mouse);
        ov->SetOverlayFlag(hOverlay, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, true);
        PIXELVR_LOG_INFO("VrKeyboard: Created input overlay %llu with Mouse input method and MakeOverlaysInteractiveIfVisible",
                         static_cast<unsigned long long>(inputOverlay_));
    } else {
        PIXELVR_LOG_WARN("VrKeyboard: Could not create input overlay (err=%d)", static_cast<int>(ovErr));
    }

    PIXELVR_LOG_INFO("VrKeyboard: Successfully initialized SteamVR overlay interface (sys=%p, ov=%p)",
                     reinterpret_cast<void*>(sys), reinterpret_cast<void*>(ov));
    return true;
}

void VrKeyboard::shutdown() {
    if (isOpen_) {
        hide();
    }
    if (inputOverlay_ != 0 && vrOverlay_) {
        auto* ov = reinterpret_cast<vr::IVROverlay*>(vrOverlay_);
        ov->DestroyOverlay(inputOverlay_);
        inputOverlay_ = 0;
    }
    if (g_VR_ShutdownInternal) {
        g_VR_ShutdownInternal();
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
    g_VR_ShutdownInternal = nullptr;
    g_VR_GetGenericInterface = nullptr;
    available_ = false;
    isOpen_ = false;
    doneSubmitted_ = false;
}

bool VrKeyboard::show(const std::string& description,
                      const std::string& existingText,
                      bool isPassword,
                      int maxChars) {
    if (!available_ && !init()) {
        return false;
    }
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

    vr::EVROverlayError err = vr::VROverlayError_None;
    if (inputOverlay_ != 0) {
        ov->SetOverlayInputMethod(inputOverlay_, vr::VROverlayInputMethod_Mouse);
        ov->SetOverlayFlag(inputOverlay_, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, true);
        ov->SetOverlayWidthInMeters(inputOverlay_, 1.0f);

        // Position overlay 1.1m in front of HMD, tilted slightly down/up to match comfortable view
        vr::HmdMatrix34_t mat = {
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, -0.2f,
            0.0f, 0.0f, 1.0f, -1.1f
        };
        ov->SetOverlayTransformTrackedDeviceRelative(inputOverlay_, vr::k_unTrackedDeviceIndex_Hmd, &mat);

        // Transparent 1x1 pixel texture so overlay is valid and active without rendering visual artifacts
        static const uint32_t s_transparentPixel = 0x00000000;
        ov->SetOverlayRaw(inputOverlay_, const_cast<uint32_t*>(&s_transparentPixel), 1, 1, 4);

        // Tell keyboard to avoid overlay upper area and appear cleanly docked
        vr::HmdRect2_t avoidRect = {
            { 0.0f, 0.0f },
            { 1.0f, 0.2f }
        };
        ov->SetKeyboardPositionForOverlay(inputOverlay_, avoidRect);

        // Show overlay BEFORE opening keyboard so vrcompositor recognizes active interactive overlay
        ov->ShowOverlay(inputOverlay_);

        err = ov->ShowKeyboardForOverlay(
            inputOverlay_,
            mode,
            vr::k_EGamepadTextInputLineModeSingleLine,
            vr::KeyboardFlag_Modal,
            description.c_str(),
            static_cast<uint32_t>(maxChars),
            existingText.c_str(),
            0);
    } else {
        err = ov->ShowKeyboard(
            mode,
            vr::k_EGamepadTextInputLineModeSingleLine,
            vr::KeyboardFlag_Modal,
            description.c_str(),
            static_cast<uint32_t>(maxChars),
            existingText.c_str(),
            0);
    }

    if (err != vr::VROverlayError_None) {
        PIXELVR_LOG_WARN("VrKeyboard: ShowKeyboard failed with error %d", static_cast<int>(err));
        if (inputOverlay_ != 0) {
            ov->HideOverlay(inputOverlay_);
        }
        return false;
    }

    kbConfigured_ = false;
    // Attempt early configuration of keyboard overlays if already created
    const char* kbOverlayNames[] = {
        "valve.steam.gamepadui.keyboard",
        "system.keyboard"
    };
    for (const char* name : kbOverlayNames) {
        vr::VROverlayHandle_t hKb = vr::k_ulOverlayHandleInvalid;
        if (ov->FindOverlay(name, &hKb) == vr::VROverlayError_None && hKb != vr::k_ulOverlayHandleInvalid) {
            ov->SetOverlayFlag(hKb, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, true);
            ov->SetOverlayInputMethod(hKb, vr::VROverlayInputMethod_Mouse);
            kbConfigured_ = true;
            PIXELVR_LOG_INFO("VrKeyboard: Configured interactive laser mouse for overlay '%s'", name);
        }
    }

    isOpen_ = true;
    PIXELVR_LOG_INFO("VrKeyboard: Shown virtual keyboard for '%s' (initial text: '%s')",
                     description.c_str(), existingText.c_str());
    return true;
}

void VrKeyboard::hide() {
    if (!isOpen_) return;
    auto* ov = reinterpret_cast<vr::IVROverlay*>(vrOverlay_);
    if (ov) {
        ov->HideKeyboard();
        if (inputOverlay_ != 0) {
            ov->HideOverlay(inputOverlay_);
        }
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
            if (inputOverlay_ != 0 && ov) {
                ov->HideOverlay(inputOverlay_);
            }
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
            if (inputOverlay_ != 0 && ov) {
                ov->HideOverlay(inputOverlay_);
            }
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
        // Dynamically ensure SteamVR keyboard overlay has laser mouse flags as soon as spawned
        if (!kbConfigured_) {
            const char* kbOverlayNames[] = {
                "valve.steam.gamepadui.keyboard",
                "system.keyboard"
            };
            for (const char* name : kbOverlayNames) {
                vr::VROverlayHandle_t hKb = vr::k_ulOverlayHandleInvalid;
                if (ov->FindOverlay(name, &hKb) == vr::VROverlayError_None && hKb != vr::k_ulOverlayHandleInvalid) {
                    ov->SetOverlayFlag(hKb, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, true);
                    ov->SetOverlayInputMethod(hKb, vr::VROverlayInputMethod_Mouse);
                    kbConfigured_ = true;
                    PIXELVR_LOG_INFO("VrKeyboard: Configured interactive laser mouse for overlay '%s'", name);
                }
            }
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

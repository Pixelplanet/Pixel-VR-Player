#include "vr_keyboard.hpp"
#include "util/logging.hpp"
#include <cstring>
#include <cmath>
#include <chrono>

#if defined(__linux__) || defined(__APPLE__)
#include <dlfcn.h>
#if defined(__linux__)
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>
#endif
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

static uint64_t sKeyboardNonce = 1000;

#if defined(__linux__)
static void clearOrSetShmKeyboardBuffer(const std::string& text) {
    // SteamVR stores the active keyboard buffer across all GamepadUI CEF instances
    // in a shared memory segment of size 0x40cc0 (265408 bytes) named /dev/shm/u<uid>-Shm_*
    // at offset 0x3fe60.
    DIR* dir = opendir("/dev/shm");
    if (!dir) return;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (std::strstr(entry->d_name, "Shm_") != nullptr) {
            std::string path = std::string("/dev/shm/") + entry->d_name;
            struct stat st{};
            if (stat(path.c_str(), &st) == 0 && st.st_size == 0x40cc0) {
                int fd = open(path.c_str(), O_RDWR);
                if (fd >= 0) {
                    void* ptr = mmap(nullptr, 0x40cc0, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
                    close(fd);
                    if (ptr != MAP_FAILED) {
                        char* kbBuf = reinterpret_cast<char*>(ptr) + 0x3fe60;
                        std::memset(kbBuf, 0, 1024);
                        if (!text.empty()) {
                            std::size_t copyLen = std::min(text.length(), static_cast<std::size_t>(1023));
                            std::memcpy(kbBuf, text.c_str(), copyLen);
                        }
                        msync(ptr, 0x40cc0, MS_SYNC);
                        munmap(ptr, 0x40cc0);
                        PIXELVR_LOG_INFO("VrKeyboard: Updated shm %s at 0x3fe60 to '%s'",
                                         entry->d_name, text.c_str());
                    }
                }
            }
        }
    }
    closedir(dir);
}
#else
static void clearOrSetShmKeyboardBuffer(const std::string&) {}
#endif

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

    // Clear any stale shared memory keyboard buffer left by previous sessions
    clearOrSetShmKeyboardBuffer("");

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
    currentNonce_ = 0;
}

bool VrKeyboard::configureKeyboardOverlay() {
    auto* ov = reinterpret_cast<vr::IVROverlay*>(vrOverlay_);
    if (!ov) return false;

    bool foundAny = false;
    vr::HmdMatrix34_t mat{};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            mat.m[r][c] = kbMatStage_[r][c];
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
        PIXELVR_LOG_INFO("VrKeyboard: Configured keyboard overlay %llu ('%s') with interactive laser mouse at standing pos (%.2f, %.2f, %.2f)",
                         static_cast<unsigned long long>(h), name ? name : "unknown",
                         mat.m[0][3], mat.m[1][3], mat.m[2][3]);
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
                      const float* headPosLocal,
                      const float* headForwardLocal,
                      const float* headPosStage,
                      const float* headForwardStage) {
    if (!available_ && !init()) {
        return false;
    }
    auto* ov = reinterpret_cast<vr::IVROverlay*>(vrOverlay_);
    if (!ov) return false;

    if (isOpen_) {
        hide();
    }
    // Explicitly hide any existing keyboard session in SteamVR to clear lingering state
    ov->HideKeyboard();

    // Reset shared memory keyboard buffer to existingText
    clearOrSetShmKeyboardBuffer(existingText);

    currentNonce_ = ++sKeyboardNonce;
    description_ = description;
    existingText_ = existingText;
    currentText_ = existingText;
    isPassword_ = isPassword;
    maxChars_ = maxChars;
    doneSubmitted_ = false;
    userTyped_ = false;

    const vr::EGamepadTextInputMode mode = isPassword
        ? vr::k_EGamepadTextInputModePassword
        : vr::k_EGamepadTextInputModeNormal;

    // -------------------------------------------------------------------------
    // 1. Calculate Local transform (for OpenXR laser pointer collision in VulkanRenderer)
    // -------------------------------------------------------------------------
    float hx = 0.0f, hy = 0.0f, hz = 0.0f;
    float fx = 0.0f, fz = -1.0f;
    if (headPosLocal && headForwardLocal) {
        hx = headPosLocal[0];
        hy = headPosLocal[1];
        hz = headPosLocal[2];
        fx = headForwardLocal[0];
        fz = headForwardLocal[2];
    }
    float flen = std::sqrt(fx * fx + fz * fz);
    if (flen < 1e-3f) {
        fx = 0.0f;
        fz = -1.0f;
    } else {
        fx /= flen;
        fz /= flen;
    }
    const float yawLocal = std::atan2(-fx, -fz);

    // Keyboard placed 1.15m in front of eyes, 0.22m below eye level.
    // The floating modal dialog box in VulkanRenderer ends at ~hy + 0.03m; this places the top
    // of the keyboard (half-height 0.20m) at ~hy - 0.02m, creating an elegant ~5cm vertical gap
    // directly beneath the modal dialog.
    const float dist = 1.15f;
    const float kbLocalX = hx + fx * dist;
    const float kbLocalY = hy - 0.22f;
    const float kbLocalZ = hz + fz * dist;

    // Tilt upward toward eyes by -0.20 rad (~11.5 degrees)
    const float tilt = -0.20f;
    vr::HmdMatrix34_t kbMatLocal{};
    kbMatLocal.m[0][0] = std::cos(yawLocal);
    kbMatLocal.m[0][1] = std::sin(yawLocal) * std::sin(tilt);
    kbMatLocal.m[0][2] = std::sin(yawLocal) * std::cos(tilt);
    kbMatLocal.m[0][3] = kbLocalX;

    kbMatLocal.m[1][0] = 0.0f;
    kbMatLocal.m[1][1] = std::cos(tilt);
    kbMatLocal.m[1][2] = -std::sin(tilt);
    kbMatLocal.m[1][3] = kbLocalY;

    kbMatLocal.m[2][0] = -std::sin(yawLocal);
    kbMatLocal.m[2][1] = std::cos(yawLocal) * std::sin(tilt);
    kbMatLocal.m[2][2] = std::cos(yawLocal) * std::cos(tilt);
    kbMatLocal.m[2][3] = kbLocalZ;

    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            kbMatLocal_[r][c] = kbMatLocal.m[r][c];
        }
    }

    // -------------------------------------------------------------------------
    // 2. Calculate Stage transform (for SteamVR TrackingUniverseStanding overlay rendering)
    // -------------------------------------------------------------------------
    float shx = hx, shy = hy + 1.65f, shz = hz;
    float sfx = fx, sfz = fz;
    if (headPosStage && headForwardStage) {
        shx = headPosStage[0];
        shy = headPosStage[1];
        shz = headPosStage[2];
        sfx = headForwardStage[0];
        sfz = headForwardStage[2];
    }
    float sflen = std::sqrt(sfx * sfx + sfz * sfz);
    if (sflen < 1e-3f) {
        sfx = 0.0f;
        sfz = -1.0f;
    } else {
        sfx /= sflen;
        sfz /= sflen;
    }
    const float yawStage = std::atan2(-sfx, -sfz);

    const float kbStageX = shx + sfx * dist;
    const float kbStageY = shy - 0.22f; // In standing space (floor=0), shy ~1.65m, so kbStageY ~1.43m above floor!
    const float kbStageZ = shz + sfz * dist;

    vr::HmdMatrix34_t kbMatStage{};
    kbMatStage.m[0][0] = std::cos(yawStage);
    kbMatStage.m[0][1] = std::sin(yawStage) * std::sin(tilt);
    kbMatStage.m[0][2] = std::sin(yawStage) * std::cos(tilt);
    kbMatStage.m[0][3] = kbStageX;

    kbMatStage.m[1][0] = 0.0f;
    kbMatStage.m[1][1] = std::cos(tilt);
    kbMatStage.m[1][2] = -std::sin(tilt);
    kbMatStage.m[1][3] = kbStageY;

    kbMatStage.m[2][0] = -std::sin(yawStage);
    kbMatStage.m[2][1] = std::cos(yawStage) * std::sin(tilt);
    kbMatStage.m[2][2] = std::cos(yawStage) * std::cos(tilt);
    kbMatStage.m[2][3] = kbStageZ;

    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            kbMatStage_[r][c] = kbMatStage.m[r][c];
        }
    }
    kbConfigured_ = false;

    // 1. Set keyboard transform in standing universe BEFORE ShowKeyboard
    ov->SetKeyboardTransformAbsolute(vr::TrackingUniverseStanding, &kbMatStage);

    // 2. Invoke virtual keyboard with Modal flag
    vr::EVROverlayError err = ov->ShowKeyboard(
        mode,
        vr::k_EGamepadTextInputLineModeSingleLine,
        vr::KeyboardFlag_Modal,
        description.c_str(),
        static_cast<uint32_t>(maxChars),
        existingText.c_str(),
        currentNonce_);

    if (err != vr::VROverlayError_None) {
        PIXELVR_LOG_WARN("VrKeyboard: ShowKeyboard failed with error %d", static_cast<int>(err));
        return false;
    }

    // Reinforce existingText in shared memory so GamepadUI CEF reads it cleanly
    clearOrSetShmKeyboardBuffer(existingText);

    // 3. Reinforce keyboard transform AFTER ShowKeyboard
    ov->SetKeyboardTransformAbsolute(vr::TrackingUniverseStanding, &kbMatStage);

    // 4. Configure overlay laser and transform immediately if already present
    configureKeyboardOverlay();

    isOpen_ = true;
    pollFramesSinceOpen_ = 0;
    PIXELVR_LOG_INFO("VrKeyboard: Shown virtual keyboard for '%s' (nonce %llu, existing='%s') at stage pos (%.2f, %.2f, %.2f) local pos (%.2f, %.2f, %.2f)",
                     description.c_str(), static_cast<unsigned long long>(currentNonce_),
                     existingText.c_str(), kbStageX, kbStageY, kbStageZ, kbLocalX, kbLocalY, kbLocalZ);
    return true;
}

void VrKeyboard::hide() {
    if (!isOpen_) return;
    auto* ov = reinterpret_cast<vr::IVROverlay*>(vrOverlay_);
    if (ov) {
        ov->HideKeyboard();
    }
    clearOrSetShmKeyboardBuffer("");
    isOpen_ = false;
    kbConfigured_ = false;
    doneSubmitted_ = false;
    userTyped_ = false;
    currentNonce_ = 0;
    currentText_.clear();
    existingText_.clear();
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
            // Discard any events from old or stale keyboard sessions
            if (event.data.keyboard.uUserValue != 0 && event.data.keyboard.uUserValue != currentNonce_) {
                PIXELVR_LOG_INFO("VrKeyboard: Ignored stale CharInput (nonce %llu != current %llu)",
                                 static_cast<unsigned long long>(event.data.keyboard.uUserValue),
                                 static_cast<unsigned long long>(currentNonce_));
                continue;
            }

            const char* inStr = event.data.keyboard.cNewInput;
            if (inStr && inStr[0] != 0) {
                userTyped_ = true;
                const unsigned char firstByte = static_cast<unsigned char>(inStr[0]);
                if (firstByte == '\b' || firstByte == 0x7F || firstByte == 8) {
                    // Backspace: pop last UTF-8 character
                    while (!currentText_.empty() && (static_cast<unsigned char>(currentText_.back()) & 0xC0) == 0x80) {
                        currentText_.pop_back();
                    }
                    if (!currentText_.empty()) {
                        currentText_.pop_back();
                    }
                    PIXELVR_LOG_INFO("VrKeyboard: Backspace pressed, text now: '%s'", currentText_.c_str());
                } else if (firstByte == '\n' || firstByte == '\r') {
                    isOpen_ = false;
                    kbConfigured_ = false;
                    doneSubmitted_ = true;
                    std::string text = currentText_;
                    if (text.empty()) {
                        char textBuf[1024] = {0};
                        ov->GetKeyboardText(textBuf, sizeof(textBuf));
                        text = textBuf;
                    }
                    ov->HideKeyboard();
                    clearOrSetShmKeyboardBuffer("");
                    currentText_.clear();
                    existingText_.clear();
                    userTyped_ = false;
                    currentNonce_ = 0;
                    PIXELVR_LOG_INFO("VrKeyboard: User submitted text via Return key: '%s'", text.c_str());
                    if (onDone) {
                        onDone(text);
                    }
                    return;
                } else if (firstByte >= 32) {
                    std::string inStrStr = inStr;
                    // If inStr is a cumulative buffer that already starts with currentText_ (from cumulative keyboard mode),
                    // adopt it directly so characters are not doubled.
                    if (!currentText_.empty() && inStrStr.rfind(currentText_, 0) == 0) {
                        currentText_ = inStrStr;
                    } else if (currentText_.length() + inStrStr.length() <= static_cast<size_t>(maxChars_)) {
                        currentText_ += inStrStr;
                    }
                    PIXELVR_LOG_INFO("VrKeyboard: Input '%s', text now: '%s'", inStrStr.c_str(), currentText_.c_str());
                }
            }
        } else if (event.eventType == vr::VREvent_KeyboardDone) {
            isOpen_ = false;
            kbConfigured_ = false;
            doneSubmitted_ = true;
            std::string text = currentText_;
            if (text.empty()) {
                char textBuf[1024] = {0};
                ov->GetKeyboardText(textBuf, sizeof(textBuf));
                text = textBuf;
            }
            ov->HideKeyboard();
            clearOrSetShmKeyboardBuffer("");
            currentText_.clear();
            existingText_.clear();
            userTyped_ = false;
            currentNonce_ = 0;
            PIXELVR_LOG_INFO("VrKeyboard: User submitted text via KeyboardDone: '%s'", text.c_str());
            if (onDone) {
                onDone(text);
            }
            return;
        } else if (event.eventType == vr::VREvent_KeyboardClosed ||
                   event.eventType == vr::VREvent_KeyboardClosed_Global) {
            isOpen_ = false;
            kbConfigured_ = false;
            ov->HideKeyboard();
            clearOrSetShmKeyboardBuffer("");
            PIXELVR_LOG_INFO("VrKeyboard: Keyboard closed (doneSubmitted=%d)", doneSubmitted_ ? 1 : 0);
            if (!doneSubmitted_) {
                currentText_.clear();
                existingText_.clear();
                userTyped_ = false;
                currentNonce_ = 0;
                PIXELVR_LOG_INFO("VrKeyboard: Discarded input on KeyboardClosed");
                if (onCancel) {
                    onCancel();
                }
            }
            return;
        }
    }

    if (isOpen_) {
        pollFramesSinceOpen_++;
        // Maintain and reinforce keyboard overlay configuration across the first 90 frames (~1 sec)
        // until successfully configured
        if (!kbConfigured_ && pollFramesSinceOpen_ < 90) {
            configureKeyboardOverlay();
        }
    }
}

std::string VrKeyboard::currentText() const {
    return currentText_;
}

} // namespace pixelvr

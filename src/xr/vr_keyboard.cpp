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

// Reads the live keyboard buffer the GamepadUI keyboard maintains in shared memory
// (same segment/offset written by clearOrSetShmKeyboardBuffer). Returns true if a valid
// keyboard shm segment was found, writing the current full text (possibly empty) to out.
// This is the reliable source of the complete typed string: GetKeyboardText() on this
// keyboard only returns the latest single keystroke, not the accumulated buffer.
static bool readShmKeyboardBuffer(std::string& out) {
    out.clear();
    DIR* dir = opendir("/dev/shm");
    if (!dir) return false;

    bool found = false;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (std::strstr(entry->d_name, "Shm_") != nullptr) {
            std::string path = std::string("/dev/shm/") + entry->d_name;
            struct stat st{};
            if (stat(path.c_str(), &st) == 0 && st.st_size == 0x40cc0) {
                int fd = open(path.c_str(), O_RDONLY);
                if (fd >= 0) {
                    void* ptr = mmap(nullptr, 0x40cc0, PROT_READ, MAP_SHARED, fd, 0);
                    close(fd);
                    if (ptr != MAP_FAILED) {
                        const char* kbBuf = reinterpret_cast<const char*>(ptr) + 0x3fe60;
                        std::size_t len = strnlen(kbBuf, 1023);
                        found = true;
                        // Multiple segments can exist; prefer the one that actually holds
                        // text (the active keyboard's buffer may not be the first found).
                        if (len > 0 && out.empty()) {
                            out.assign(kbBuf, len);
                        }
                        munmap(ptr, 0x40cc0);
                    }
                }
            }
        }
    }
    closedir(dir);
    return found;
}
#else
static void clearOrSetShmKeyboardBuffer(const std::string&) {}
static bool readShmKeyboardBuffer(std::string& out) { out.clear(); return false; }
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

    vr::IVRSettings* settings = reinterpret_cast<vr::IVRSettings*>(
        g_VR_GetGenericInterface(vr::IVRSettings_Version, &err));

    vr::EVRInitError propErr = vr::VRInitError_None;
    void* props = reinterpret_cast<void*>(
        g_VR_GetGenericInterface("IVRProperties_001", &propErr));

    vr::EVRInitError focusErr = vr::VRInitError_None;
    void* inputFocus = reinterpret_cast<void*>(
        g_VR_GetGenericInterface("IVRInputFocusInternal_XXX", &focusErr));

    vrSystem_ = sys;
    vrOverlay_ = ov;
    vrSettings_ = settings;
    vrProperties_ = props;
    vrInputFocus_ = inputFocus;
    available_ = true;

    // Clear any stale shared memory keyboard buffer left by previous sessions
    clearOrSetShmKeyboardBuffer("");

    PIXELVR_LOG_INFO("VrKeyboard: Successfully initialized SteamVR overlay interface (sys=%p, ov=%p, settings=%p, props=%p, focus=%p)",
                     reinterpret_cast<void*>(sys), reinterpret_cast<void*>(ov),
                     reinterpret_cast<void*>(settings), props, inputFocus);
    return true;
}

void VrKeyboard::setLaserPointerActive(bool active) {
    // 1. Update dashboard.preferGamepadMode setting
    if (auto* settings = reinterpret_cast<vr::IVRSettings*>(vrSettings_)) {
        vr::EVRSettingsError sErr = vr::VRSettingsError_None;
        settings->SetBool("dashboard", "preferGamepadMode", !active, &sErr);
    }

    // 2. Clear or restore controller property 0x845 (2117, k_unBoolPropertyTag = 4) via IVRProperties_001.
    // On Steam Frame standalone, vrcompositor checks property 0x845 on each controller every frame;
    // when true, it zeroes m_ulActiveLaserHand and switches SystemPanelInteractionMode back to Gamepad (2)
    // unless the user manually presses the Grip laser-toggle button. Setting property 0x845 to false (0)
    // while the keyboard is open allows vrcompositor to activate system.pointer automatically.
    if (vrProperties_) {
        struct PropertyWrite {
            uint32_t prop;
            uint32_t writeType;
            uint32_t eSetError;
            void* pvBuffer;
            uint32_t unBufferSize;
            uint32_t unTag;
            uint32_t eError;
        };
        using FnWritePropertyBatch = uint32_t (*)(void*, uint64_t, PropertyWrite*, uint32_t);
        using FnTrackedDeviceToPropertyContainer = uint64_t (*)(void*, uint32_t);

        void** vtable = *reinterpret_cast<void***>(vrProperties_);
        auto writeBatch = reinterpret_cast<FnWritePropertyBatch>(vtable[1]);
        auto devToContainer = reinterpret_cast<FnTrackedDeviceToPropertyContainer>(vtable[3]);
        auto* sys = reinterpret_cast<vr::IVRSystem*>(vrSystem_);

        uint8_t val = active ? 0 : 1;
        for (uint32_t dev = 1; dev < 16; ++dev) {
            if (!active && sys && sys->GetTrackedDeviceClass(dev) != vr::TrackedDeviceClass_Controller) {
                continue;
            }
            uint64_t container = devToContainer(vrProperties_, dev);
            if (container != 0) {
                PropertyWrite pw{};
                pw.prop = 0x845;      // 2117: controller prefer-gamepad-over-laser property
                pw.writeType = 0;     // PropertyWrite_Set
                pw.eSetError = 0;
                pw.pvBuffer = &val;
                pw.unBufferSize = sizeof(val);
                pw.unTag = 4;         // k_unBoolPropertyTag
                pw.eError = 0;
                writeBatch(vrProperties_, container, &pw, 1);
            }
        }
    }

    // 3. Switch FocusState SystemPanelInteractionMode to LaserMouse (1) via IVRInputFocusInternal_XXX
    // vtable[12] (SetSystemPanelInteractionMode) so the laser pointer is immediately active even if
    // more than 10 seconds have elapsed since the dashboard last had focus.
    if (active && vrInputFocus_) {
        using FnSetSystemPanelInteractionMode = void (*)(void*, int);
        void** vtable = *reinterpret_cast<void***>(vrInputFocus_);
        auto setMode = reinterpret_cast<FnSetSystemPanelInteractionMode>(vtable[12]);
        setMode(vrInputFocus_, 1); // 1 = LaserMouse, 2 = Gamepad
    }
}

void VrKeyboard::shutdown() {
    if (!available_) return;
    available_ = false;
    if (isOpen_) {
        hide();
    }
    setLaserPointerActive(false);
    if (g_VR_ShutdownInternal) {
        auto fn = g_VR_ShutdownInternal;
        g_VR_ShutdownInternal = nullptr;
        fn();
    }
    vrSystem_ = nullptr;
    vrOverlay_ = nullptr;
    vrSettings_ = nullptr;
    vrProperties_ = nullptr;
    vrInputFocus_ = nullptr;
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

bool VrKeyboard::isUserPresent() const {
    auto* sys = reinterpret_cast<vr::IVRSystem*>(vrSystem_);
    if (!sys) return true;
    vr::EDeviceActivityLevel act = sys->GetTrackedDeviceActivityLevel(vr::k_unTrackedDeviceIndex_Hmd);
    if (act == vr::k_EDeviceActivityLevel_Idle ||
        act == vr::k_EDeviceActivityLevel_UserInteraction_Timeout ||
        act == vr::k_EDeviceActivityLevel_Standby) {
        return false;
    }
    return true;
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
        if (!kbConfigured_) {
            PIXELVR_LOG_INFO("VrKeyboard: Configured keyboard overlay %llu ('%s') with interactive laser mouse at standing pos (%.2f, %.2f, %.2f)",
                             static_cast<unsigned long long>(h), name ? name : "unknown",
                             mat.m[0][3], mat.m[1][3], mat.m[2][3]);
        }
    };

    // 1. Check known specific VR keyboard overlay keys (exclude valve.steam.gamepadui.keyboard
    // which is the 2D gamescope overlay and would collide with system.keyboard in 3D space)
    const char* kbOverlayNames[] = {
        "VRKeyboard_uid0",
        "VRKeyboard_uid1",
        "VRKeyboard",
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
            if (std::strcmp(keyBuf, "valve.steam.gamepadui.keyboard") == 0) {
                continue;
            }
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
    setLaserPointerActive(true);
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
    cursorPos_ = currentText_.size();
    escState_ = 0;
    escParams_.clear();
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

    // 0. Activate SteamVR's laser pointer (system.pointer) on the keyboard overlay immediately
    // without requiring the user to press the Grip button first.
    setLaserPointerActive(true);

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
        setLaserPointerActive(false);
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
    setLaserPointerActive(false);
    clearOrSetShmKeyboardBuffer("");
    isOpen_ = false;
    kbConfigured_ = false;
    doneSubmitted_ = false;
    userTyped_ = false;
    currentNonce_ = 0;
    currentText_.clear();
    existingText_.clear();
    cursorPos_ = 0;
    escState_ = 0;
    escParams_.clear();
    PIXELVR_LOG_INFO("VrKeyboard: Hidden virtual keyboard");
}

void VrKeyboard::moveCursor(int delta) {
    if (!isOpen_) return;
    if (cursorPos_ > currentText_.size()) {
        cursorPos_ = currentText_.size();
    }
    if (delta < 0) {
        int steps = -delta;
        while (steps > 0 && cursorPos_ > 0) {
            --cursorPos_;
            while (cursorPos_ > 0 && (static_cast<unsigned char>(currentText_[cursorPos_]) & 0xC0) == 0x80) {
                --cursorPos_;
            }
            --steps;
        }
    } else if (delta > 0) {
        int steps = delta;
        while (steps > 0 && cursorPos_ < currentText_.size()) {
            ++cursorPos_;
            while (cursorPos_ < currentText_.size() &&
                   (static_cast<unsigned char>(currentText_[cursorPos_]) & 0xC0) == 0x80) {
                ++cursorPos_;
            }
            --steps;
        }
    }
}

void VrKeyboard::poll(std::function<void(const std::string&)> onDone,
                      std::function<void()> onCancel) {
    if (!available_ || !isOpen_) return;
    auto* sys = reinterpret_cast<vr::IVRSystem*>(vrSystem_);
    auto* ov = reinterpret_cast<vr::IVROverlay*>(vrOverlay_);
    if (!sys || !ov) return;

    // Tears down the current session state and clears any lingering draft buffer so that
    // text can never persist into the next field or session.
    auto resetSession = [&]() {
        isOpen_ = false;
        kbConfigured_ = false;
        ov->HideKeyboard();
        setLaserPointerActive(false);
        clearOrSetShmKeyboardBuffer("");
        currentText_.clear();
        existingText_.clear();
        cursorPos_ = 0;
        escState_ = 0;
        escParams_.clear();
        userTyped_ = false;
        currentNonce_ = 0;
    };

    auto deleteBeforeCursor = [&]() {
        if (cursorPos_ > currentText_.size()) cursorPos_ = currentText_.size();
        if (cursorPos_ == 0 || currentText_.empty()) return;
        std::size_t p = cursorPos_;
        while (p > 0) {
            --p;
            if ((static_cast<unsigned char>(currentText_[p]) & 0xC0) != 0x80) {
                break;
            }
        }
        currentText_.erase(p, cursorPos_ - p);
        cursorPos_ = p;
    };

    auto deleteAtCursor = [&]() {
        if (cursorPos_ > currentText_.size()) cursorPos_ = currentText_.size();
        if (cursorPos_ >= currentText_.size() || currentText_.empty()) return;
        std::size_t p = cursorPos_ + 1;
        while (p < currentText_.size() &&
               (static_cast<unsigned char>(currentText_[p]) & 0xC0) == 0x80) {
            ++p;
        }
        currentText_.erase(cursorPos_, p - cursorPos_);
    };

    auto handleCsiCommand = [&](char finalChar, const std::string& params) {
        if (finalChar == 'D') {
            moveCursor(-1);
        } else if (finalChar == 'C') {
            moveCursor(1);
        } else if (finalChar == 'A' || finalChar == 'H') {
            cursorPos_ = 0;
        } else if (finalChar == 'B' || finalChar == 'F') {
            cursorPos_ = currentText_.size();
        } else if (finalChar == '~') {
            if (params == "3") {
                deleteAtCursor();
            } else if (params == "1" || params == "7") {
                cursorPos_ = 0;
            } else if (params == "4" || params == "8") {
                cursorPos_ = currentText_.size();
            }
        }
    };

    vr::VREvent_t event{};
    while (sys->PollNextEvent(&event, sizeof(event))) {
        const bool isKeyboardEvent =
            event.eventType == vr::VREvent_KeyboardCharInput ||
            event.eventType == vr::VREvent_KeyboardDone ||
            event.eventType == vr::VREvent_KeyboardClosed ||
            event.eventType == vr::VREvent_KeyboardClosed_Global;

        // Discard events belonging to an old or stale keyboard session. This guard must cover
        // every keyboard event (not just CharInput): a stale Done/Closed would otherwise leak
        // the previous field's text into the field currently being edited.
        if (isKeyboardEvent &&
            event.data.keyboard.uUserValue != 0 &&
            event.data.keyboard.uUserValue != currentNonce_) {
            PIXELVR_LOG_INFO("VrKeyboard: Ignored stale keyboard event (type %u, nonce %llu != current %llu)",
                             event.eventType,
                             static_cast<unsigned long long>(event.data.keyboard.uUserValue),
                             static_cast<unsigned long long>(currentNonce_));
            continue;
        }

        if (event.eventType == vr::VREvent_KeyboardCharInput) {
            const char* inStr = event.data.keyboard.cNewInput;
            char apiBuf[1024] = {0};
            uint32_t apiLen = ov->GetKeyboardText(apiBuf, sizeof(apiBuf));

            // Determine the keystroke payload: SteamVR's system.keyboard sends each
            // key press as a single-character delta via cNewInput (and mirrors that
            // single character in GetKeyboardText / shm 0x3fe60).
            std::string key;
            std::string shmKey;
            if (inStr && inStr[0] != '\0') {
                key.assign(inStr, strnlen(inStr, sizeof(event.data.keyboard.cNewInput)));
            } else if (apiLen > 0 && apiBuf[0] != '\0') {
                key.assign(apiBuf, strnlen(apiBuf, sizeof(apiBuf) - 1));
            } else if (readShmKeyboardBuffer(shmKey) && !shmKey.empty()) {
                key = shmKey;
            }

            if (!key.empty() && (key[0] == '\n' || key[0] == '\r')) {
                // Enter key submits the accumulated buffer.
                const std::string text = currentText_;
                doneSubmitted_ = true;
                resetSession();
                PIXELVR_LOG_INFO("VrKeyboard: User submitted text via Return key: '%s'", text.c_str());
                if (onDone) {
                    onDone(text);
                }
                return;
            }

            // SteamVR's virtual keyboard sends ANSI escape sequences (either "\x1b[D"/"\x1b[C"
            // or bare "[D"/"[C") when clicking its Left / Right cursor buttons.
            if (escState_ == 0 && (key == "[D" || key == "[C" || key == "[A" || key == "[B" ||
                                   key == "[H" || key == "[F" || key == "[3~")) {
                if (key == "[3~") {
                    deleteAtCursor();
                } else {
                    handleCsiCommand(key[1], "");
                }
            } else if (!key.empty() && (key[0] == '\b' || static_cast<unsigned char>(key[0]) == 0x7f) && key.size() == 1 && escState_ == 0) {
                deleteBeforeCursor();
            } else if (!key.empty()) {
                for (char c : key) {
                    const unsigned char uc = static_cast<unsigned char>(c);
                    if (escState_ == 1) {
                        if (c == '[' || c == 'O') {
                            escState_ = 2;
                            escParams_.clear();
                            continue;
                        }
                        escState_ = 0;
                        escParams_.clear();
                    } else if (escState_ == 2) {
                        if (uc >= 0x30 && uc <= 0x3F) {
                            escParams_.push_back(c);
                            continue;
                        }
                        if (uc >= 0x20 && uc <= 0x2F) {
                            continue;
                        }
                        if (uc >= 0x40 && uc <= 0x7E) {
                            handleCsiCommand(c, escParams_);
                        }
                        escState_ = 0;
                        escParams_.clear();
                        continue;
                    }

                    if (c == '\x1b') {
                        escState_ = 1;
                        escParams_.clear();
                    } else if (c == '\b' || uc == 0x7f) {
                        deleteBeforeCursor();
                    } else if (uc >= 32) {
                        if (currentText_.length() < static_cast<std::size_t>(maxChars_)) {
                            if (cursorPos_ > currentText_.size()) cursorPos_ = currentText_.size();
                            currentText_.insert(cursorPos_, 1, c);
                            ++cursorPos_;
                        }
                    }
                }
            }

            userTyped_ = true;
            PIXELVR_LOG_INFO("VrKeyboard: CharInput key='%s' api(len=%u)='%s' -> currentText='%s' cursor=%zu",
                             key.c_str(), apiLen, apiBuf, currentText_.c_str(), cursorPos_);
        } else if (event.eventType == vr::VREvent_KeyboardDone) {
            const std::string text = currentText_;
            doneSubmitted_ = true;
            resetSession();
            PIXELVR_LOG_INFO("VrKeyboard: User submitted text via KeyboardDone: '%s'", text.c_str());
            if (onDone) {
                onDone(text);
            }
            return;
        } else if (event.eventType == vr::VREvent_KeyboardClosed ||
                   event.eventType == vr::VREvent_KeyboardClosed_Global) {
            const bool wasSubmitted = doneSubmitted_;
            resetSession();
            doneSubmitted_ = false;
            PIXELVR_LOG_INFO("VrKeyboard: Keyboard closed (doneSubmitted=%d)", wasSubmitted ? 1 : 0);
            if (!wasSubmitted) {
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
        // Maintain and reinforce keyboard overlay configuration across the first 90 frames (~1.2 sec)
        // so that even if vrwebhelper spawns system.keyboard ~100ms after ShowKeyboard(), it gets
        // positioned and configured for laser mouse interaction.
        if (!kbConfigured_ || pollFramesSinceOpen_ < 90) {
            configureKeyboardOverlay();
        }
    }
}

std::string VrKeyboard::currentText() const {
    return currentText_;
}

} // namespace pixelvr

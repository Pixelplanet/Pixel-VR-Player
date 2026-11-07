#include "xr/vr_app_manifest.hpp"

#include "util/logging.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <dlfcn.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

#include "openvr.h"

namespace pixelvr {

namespace {

constexpr const char* kArg = "--register-vr-manifest";
// Steam's id variables: the child must not carry them, or SteamVR could take
// its connection for the app itself (and clear the app when it exits).
constexpr const char* kSteamIdVars[] = {"SteamGameId", "SteamAppId", "SteamOverlayGameId"};

// The shortcut's 32-bit app id from Steam's launch environment (0 = not
// launched by Steam). For a shortcut SteamGameId is the 64-bit game id
// (app id << 32 | 0x02000000).
uint32_t steamShortcutAppId() {
    for (const char* var : kSteamIdVars) {
        const char* v = std::getenv(var);
        if (!v || !*v) continue;
        char* end = nullptr;
        const unsigned long long id = std::strtoull(v, &end, 10);
        if (end == v || id == 0) continue;
        const uint64_t app = (id > 0xFFFFFFFFull) ? (id >> 32) : id;
        if (app != 0) return static_cast<uint32_t>(app);
    }
    return 0;
}

std::string configDir() {
    const char* home = std::getenv("HOME");
    return std::string(home ? home : ".") + "/.config/pixelvr";
}

std::string jsonEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out;
}

std::string exeDir() {
    std::error_code ec;
    const std::filesystem::path exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::string() : exe.parent_path().string();
}

}  // namespace

int registerVrAppManifest() {
#if defined(__linux__)
    const char* keyArg = std::getenv("PIXELVR_VR_APP_ID");
    const uint32_t appId = keyArg ? static_cast<uint32_t>(std::strtoul(keyArg, nullptr, 10)) : 0;
    if (appId == 0) {
        PIXELVR_LOG_WARN("VR manifest: no app id");
        return 2;
    }
    const std::string appKey = "steam.app." + std::to_string(appId);
    const uint64_t gameId = (static_cast<uint64_t>(appId) << 32) | 0x02000000ull;

    void* lib = nullptr;
    for (const char* path : {"/opt/steamvr/bin/linuxarm64/libopenvr_api.so", "/opt/steamvr/bin/linux64/libopenvr_api.so",
                             "libopenvr_api.so"}) {
        lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (lib) break;
    }
    if (!lib) {
        PIXELVR_LOG_WARN("VR manifest: libopenvr_api.so not found");
        return 3;
    }
    using FnInit = intptr_t (*)(vr::EVRInitError*, vr::EVRApplicationType);
    using FnShutdown = void (*)();
    using FnGet = intptr_t (*)(const char*, vr::EVRInitError*);
    auto init = reinterpret_cast<FnInit>(dlsym(lib, "VR_InitInternal"));
    auto shutdown = reinterpret_cast<FnShutdown>(dlsym(lib, "VR_ShutdownInternal"));
    auto get = reinterpret_cast<FnGet>(dlsym(lib, "VR_GetGenericInterface"));
    if (!init || !shutdown || !get) {
        PIXELVR_LOG_WARN("VR manifest: OpenVR symbols missing");
        return 3;
    }
    vr::EVRInitError err = vr::VRInitError_None;
    init(&err, vr::VRApplication_Utility);
    if (err != vr::VRInitError_None) {
        PIXELVR_LOG_WARN("VR manifest: OpenVR utility init failed (%d)", static_cast<int>(err));
        return 4;
    }
    auto* apps = reinterpret_cast<vr::IVRApplications*>(get(vr::IVRApplications_Version, &err));
    int rc = 0;
    if (!apps || err != vr::VRInitError_None) {
        PIXELVR_LOG_WARN("VR manifest: no %s (%d)", vr::IVRApplications_Version, static_cast<int>(err));
        rc = 4;
    } else if (apps->IsApplicationInstalled(appKey.c_str())) {
        PIXELVR_LOG_INFO("VR manifest: SteamVR already knows %s", appKey.c_str());
    } else {
        const std::string dir = configDir();
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        const std::string path = dir + "/pixelvr.vrmanifest";
        const std::string icon = exeDir() + "/icon.png";
        {
            std::ofstream f(path, std::ios::trunc);
            f << "{\n  \"source\": \"pixelvr\",\n  \"applications\": [{\n"
              << "    \"app_key\": \"" << appKey << "\",\n"
              << "    \"launch_type\": \"url\",\n"
              << "    \"url\": \"steam://rungameid/" << gameId << "\",\n";
            if (std::filesystem::is_regular_file(icon, ec)) {
                f << "    \"image_path\": \"" << jsonEscape(icon) << "\",\n";
            }
            f << "    \"strings\": { \"en_us\": { \"name\": \"Pixel VR Player\" } }\n  }]\n}\n";
        }
        const vr::EVRApplicationError ae = apps->AddApplicationManifest(path.c_str(), false);
        const bool ok = apps->IsApplicationInstalled(appKey.c_str());
        PIXELVR_LOG_INFO("VR manifest: registered %s for %s: %s (%d), installed now: %d", path.c_str(),
                         appKey.c_str(), apps->GetApplicationsErrorNameFromEnum(ae), static_cast<int>(ae),
                         static_cast<int>(ok));
        if (!ok) rc = 5;
    }
    shutdown();
    if (rc == 0) {
        // Remembered so later launches do not connect to SteamVR again.
        std::ofstream(configDir() + "/vr_manifest_registered") << appKey << "\n";
    }
    return rc;
#else
    return 1;
#endif
}

void spawnVrAppManifestRegistration() {
#if defined(__linux__)
    const uint32_t appId = steamShortcutAppId();
    {
        auto envOr = [](const char* v) { const char* s = std::getenv(v); return s ? s : "(unset)"; };
        PIXELVR_LOG_INFO("Steam launch ids: SteamGameId=%s SteamAppId=%s SteamOverlayGameId=%s -> app %u",
                         envOr("SteamGameId"), envOr("SteamAppId"), envOr("SteamOverlayGameId"), appId);
    }
    if (appId == 0) return;  // not started from Steam
    const std::string appKey = "steam.app." + std::to_string(appId);
    {
        std::ifstream f(configDir() + "/vr_manifest_registered");
        std::string done;
        if (f && std::getline(f, done) && done == appKey) return;
    }
    // The child's environment: ours without Steam's id variables, plus the id.
    std::vector<std::string> envStore;
    for (char** e = environ; e && *e; ++e) {
        bool steamId = false;
        for (const char* var : kSteamIdVars) {
            const std::size_t n = std::strlen(var);
            if (std::strncmp(*e, var, n) == 0 && (*e)[n] == '=') steamId = true;
        }
        if (!steamId) envStore.emplace_back(*e);
    }
    envStore.push_back("PIXELVR_VR_APP_ID=" + std::to_string(appId));
    std::thread([envStore = std::move(envStore)]() mutable {
        // After the session is up: nothing of this touches the launch itself.
        std::this_thread::sleep_for(std::chrono::seconds(8));
        std::vector<char*> envp;
        for (std::string& s : envStore) envp.push_back(s.data());
        envp.push_back(nullptr);
        std::string exe = "/proc/self/exe";
        std::string arg = kArg;
        char* argv[] = {exe.data(), arg.data(), nullptr};
        pid_t pid = 0;
        if (posix_spawn(&pid, exe.c_str(), nullptr, nullptr, argv, envp.data()) != 0) {
            PIXELVR_LOG_WARN("VR manifest: could not start the helper");
            return;
        }
        int status = 0;
        waitpid(pid, &status, 0);
    }).detach();
#endif
}

}  // namespace pixelvr

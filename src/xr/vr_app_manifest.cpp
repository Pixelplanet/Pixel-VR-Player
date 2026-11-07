#include "xr/vr_app_manifest.hpp"

#include "util/logging.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
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

constexpr const char* kArg = "--steamvr-prelaunch";
// Steam's id variables: the helper must not carry them, or SteamVR could take
// its connection for the app itself (and end the app's launch when it exits).
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

#if defined(__linux__)
// Our manifest for `appKey`: the "Up next" screen shows image_path (Steam's own
// games use their 460x215 store header there).
std::string manifestText(const std::string& appKey, uint32_t appId) {
    const uint64_t gameId = (static_cast<uint64_t>(appId) << 32) | 0x02000000ull;
    const std::string dir = exeDir();
    std::error_code ec;
    auto art = [&](const char* name) {
        const std::string p = dir + "/" + name;
        return std::filesystem::is_regular_file(p, ec) ? p : std::string();
    };
    std::string header = art("header.png");
    if (header.empty()) header = art("icon.png");
    const std::string capsule = art("capsule.png");
    std::string t = "{\n  \"source\": \"pixelvr\",\n  \"applications\": [{\n";
    t += "    \"app_key\": \"" + appKey + "\",\n";
    t += "    \"launch_type\": \"url\",\n";
    t += "    \"url\": \"steam://rungameid/" + std::to_string(gameId) + "\",\n";
    if (!header.empty()) t += "    \"image_path\": \"" + jsonEscape(header) + "\",\n";
    if (!capsule.empty()) t += "    \"image_path_capsule\": \"" + jsonEscape(capsule) + "\",\n";
    t += "    \"strings\": { \"en_us\": { \"name\": \"Pixel VR Player\" } }\n  }]\n}\n";
    return t;
}

// Registers ~/.config/pixelvr/pixelvr.vrmanifest for `appKey` when SteamVR does
// not know the key or the file's content changed (new art, moved install).
// SteamVR keeps what it read first, so a changed file is removed and re-added.
bool ensureManifest(vr::IVRApplications* apps, const std::string& appKey, uint32_t appId) {
    const std::string dir = configDir();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::string path = dir + "/pixelvr.vrmanifest";
    const std::string text = manifestText(appKey, appId);
    std::string current;
    {
        std::ifstream f(path, std::ios::binary);
        current.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    const bool installed = apps->IsApplicationInstalled(appKey.c_str());
    if (installed && current == text) return true;
    std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
    if (installed) apps->RemoveApplicationManifest(path.c_str());
    const vr::EVRApplicationError ae = apps->AddApplicationManifest(path.c_str(), false);
    const bool ok = apps->IsApplicationInstalled(appKey.c_str());
    PIXELVR_LOG_INFO("SteamVR prelaunch: %s %s for %s: %s, installed now: %d", installed ? "updated" : "registered",
                     path.c_str(), appKey.c_str(), apps->GetApplicationsErrorNameFromEnum(ae), static_cast<int>(ok));
    return ok;
}
#endif

}  // namespace

int runSteamVrPrelaunchHelper() {
#if defined(__linux__)
    const char* idArg = std::getenv("PIXELVR_VR_APP_ID");
    const uint32_t appId = idArg ? static_cast<uint32_t>(std::strtoul(idArg, nullptr, 10)) : 0;
    if (appId == 0) return 2;
    const std::string appKey = "steam.app." + std::to_string(appId);

    void* lib = nullptr;
    for (const char* path : {"/opt/steamvr/bin/linuxarm64/libopenvr_api.so", "/opt/steamvr/bin/linux64/libopenvr_api.so",
                             "libopenvr_api.so"}) {
        lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (lib) break;
    }
    if (!lib) {
        PIXELVR_LOG_WARN("SteamVR prelaunch: libopenvr_api.so not found");
        return 3;
    }
    using FnInit = intptr_t (*)(vr::EVRInitError*, vr::EVRApplicationType);
    using FnShutdown = void (*)();
    using FnGet = intptr_t (*)(const char*, vr::EVRInitError*);
    auto init = reinterpret_cast<FnInit>(dlsym(lib, "VR_InitInternal"));
    auto shutdown = reinterpret_cast<FnShutdown>(dlsym(lib, "VR_ShutdownInternal"));
    auto get = reinterpret_cast<FnGet>(dlsym(lib, "VR_GetGenericInterface"));
    if (!init || !shutdown || !get) return 3;
    vr::EVRInitError err = vr::VRInitError_None;
    init(&err, vr::VRApplication_Utility);
    if (err != vr::VRInitError_None) {
        PIXELVR_LOG_WARN("SteamVR prelaunch: OpenVR utility init failed (%d)", static_cast<int>(err));
        return 4;
    }
    int rc = 0;
    auto* apps = reinterpret_cast<vr::IVRApplications*>(get(vr::IVRApplications_Version, &err));
    if (!apps || err != vr::VRInitError_None) {
        rc = 4;
    } else if (!ensureManifest(apps, appKey, appId)) {
        rc = 5;
    } else {
        // What Steam does before starting one of its own VR games: SteamVR
        // enters its launching state for this key ("Up next"), and the
        // dashboard closes once the app connects.
        const vr::EVRApplicationError ae = apps->PerformApplicationPrelaunchCheck(appKey.c_str());
        PIXELVR_LOG_INFO("SteamVR prelaunch check for %s: %s", appKey.c_str(),
                         apps->GetApplicationsErrorNameFromEnum(ae));
        if (ae != vr::VRApplicationError_None) {
            rc = 6;
        } else {
            // Hold the launching state until the dashboard has closed. Our
            // OpenXR connection ends it ("ClearAppTransition: ProcessConnected"),
            // and the dashboard re-reads the state when its event handler runs:
            // connecting within ~25 ms made it miss "Starting" and stay on
            // "Resume Game" most of the time. Steam's own games connect seconds
            // later. Read-only queries; capped at 1.5 s.
            auto* overlay = reinterpret_cast<vr::IVROverlay*>(get(vr::IVROverlay_Version, &err));
            if (err != vr::VRInitError_None) overlay = nullptr;
            const auto t0 = std::chrono::steady_clock::now();
            auto elapsedMs = [&] {
                return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            };
            bool sawStarting = false, closed = false;
            while (elapsedMs() < 1500.0) {
                if (!sawStarting && apps->GetSceneApplicationState() == vr::EVRSceneApplicationState_Starting) {
                    sawStarting = true;
                }
                // Without the overlay interface: just give the dashboard 1 s.
                if (!overlay ? elapsedMs() >= 1000.0
                             : (sawStarting && elapsedMs() >= 400.0 && !overlay->IsDashboardVisible())) {
                    closed = overlay != nullptr;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            PIXELVR_LOG_INFO("SteamVR prelaunch: launching state %s, dashboard %s after %.0f ms",
                             sawStarting ? "seen" : "not seen",
                             closed ? "closed" : (overlay ? "still open" : "not queried"), elapsedMs());
        }
    }
    shutdown();
    return rc;
#else
    return 1;
#endif
}

void steamVrPrelaunch() {
#if defined(__linux__)
    const uint32_t appId = steamShortcutAppId();
    if (appId == 0) return;  // not started from Steam
    const auto t0 = std::chrono::steady_clock::now();
    // The helper's environment: ours without Steam's id variables, plus the id.
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
    std::vector<char*> envp;
    for (std::string& s : envStore) envp.push_back(s.data());
    envp.push_back(nullptr);
    std::string exe = "/proc/self/exe";
    std::string arg = kArg;
    char* argv[] = {exe.data(), arg.data(), nullptr};
    pid_t pid = 0;
    if (posix_spawn(&pid, exe.c_str(), nullptr, nullptr, argv, envp.data()) != 0) {
        PIXELVR_LOG_WARN("SteamVR prelaunch: could not start the helper");
        return;
    }
    // It must finish before our own OpenXR connection; give up after 3 s.
    int status = 0;
    bool done = false;
    while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(3)) {
        if (waitpid(pid, &status, WNOHANG) == pid) {
            done = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (!done) {
        PIXELVR_LOG_WARN("SteamVR prelaunch: helper still running after %.0f ms, continuing", ms);
        std::thread([pid] {
            int st = 0;
            waitpid(pid, &st, 0);
        }).detach();
        return;
    }
    PIXELVR_LOG_INFO("SteamVR prelaunch: helper exit %d after %.0f ms",
                     WIFEXITED(status) ? WEXITSTATUS(status) : -1, ms);
#endif
}

}  // namespace pixelvr

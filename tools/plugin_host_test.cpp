// Drives PluginHost with synthetic screens (no VR, no video) against the
// example plugin and a few broken test plugins, then checks what the example
// plugin received. Development tool, never part of a release.
//
//   plugin_host_test <build>/plugins/example
//
// Runs in a throw-away $HOME under /tmp.

#include "plugins/plugin_host.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using pixelvr::PluginAppSnapshot;
using pixelvr::PluginHost;
using pixelvr::PluginScreenSnapshot;

namespace {

int failures = 0;
void check(bool ok, const std::string& what) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++failures;
}

void writeFile(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << text;
}

std::string readFile(const fs::path& p) {
    std::ifstream f(p);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

int countOf(const std::string& hay, const std::string& needle) {
    int n = 0;
    for (size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + 1)) ++n;
    return n;
}

int findPlugin(const PluginHost& host, const std::string& id) {
    for (size_t i = 0; i < host.plugins().size(); ++i) {
        if (host.plugins()[i].id == id) return static_cast<int>(i);
    }
    return -1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <build>/plugins/example\n", argv[0]);
        return 2;
    }
    const fs::path exampleSrc = fs::absolute(argv[1]);
    char tmpl[] = "/tmp/pvrplugtest.XXXXXX";
    const char* tmp = mkdtemp(tmpl);
    if (!tmp) return 2;
    setenv("HOME", tmp, 1);
    const fs::path plugins = PluginHost::pluginsDir();
    fs::create_directories(plugins);
    fs::copy(exampleSrc, plugins / "example", fs::copy_options::recursive);
    writeFile(plugins / "crash/plugin.json",
              R"({"id":"test.crash","name":"Crash","api":1,"exec":["sh","run.sh"],"permissions":["playback"]})");
    writeFile(plugins / "crash/run.sh", "exit 3\n");
    writeFile(plugins / "silent/plugin.json",
              R"({"id":"test.silent","name":"Silent","api":1,"exec":["sh","run.sh"],"permissions":[]})");
    writeFile(plugins / "silent/run.sh", "exec sleep 30\n");
    writeFile(plugins / "future/plugin.json", R"({"id":"test.future","name":"Future","api":2,"exec":["x"]})");
    writeFile(fs::path(tmp) / ".config/pixelvr/plugins.json",
              R"({"plugins":{"org.pixelvr.example":{"enabled":true,"vault":false,"settings":{"verbose":"1"}},)"
              R"("test.crash":{"enabled":true},"test.silent":{"enabled":true},"test.future":{"enabled":true}}})");

    check(PluginHost::sanitizePath("smb://u:p@nas/share/a.mkv") == "smb://nas/share/a.mkv", "sanitizePath drops credentials");
    check(PluginHost::sanitizePath("https://h:32400/x/file.mkv?X-Plex-Token=t") == "https://h:32400/x/file.mkv",
          "sanitizePath drops the query");
    check(PluginHost::sanitizePath("/home/a@b/c.mkv") == "/home/a@b/c.mkv", "sanitizePath keeps local paths");

    PluginHost host;
    host.init();
    check(host.plugins().size() == 4, "found 4 plugins");
    const int ex = findPlugin(host, "org.pixelvr.example");
    const int fut = findPlugin(host, "test.future");
    check(ex >= 0, "example plugin found");
    check(fut >= 0 && host.plugins()[fut].status == "needs a newer Pixel VR Player", "api 2 plugin is refused");
    if (ex < 0) return 1;

    std::vector<PluginScreenSnapshot> screens(2);
    screens[0].selected = screens[0].audio = true;
    PluginAppSnapshot app;
    app.vaultUnlocked = true;
    std::vector<std::string> notices;
    bool sawGauge = false;
    const auto t0 = std::chrono::steady_clock::now();
    auto runUntil = [&](double sec, const std::function<void(double)>& step) {
        for (;;) {
            const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            if (t >= sec) break;
            if (step) step(t);
            host.update(screens, app);
            std::string n;
            while (host.takeNotice(n)) notices.push_back(n);
            if (host.plugins()[ex].gaugeValue >= 0.0f) sawGauge = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(11));
        }
    };

    runUntil(0.8, nullptr);
    check(host.plugins()[ex].pid > 0 && host.plugins()[ex].status == "Waiting for a video", "example running, status set");

    PluginScreenSnapshot& v = screens[0];
    v.kind = PluginScreenSnapshot::Kind::Video;
    v.path = "smb://user:secret@nas/share/Movies/Test Movie.mkv";
    v.fileName = "Test Movie.mkv";
    v.title = "Test Movie";
    v.source = "smb";
    v.duration = 120.0;
    runUntil(1.2, nullptr);
    double playFrom = 0.0, playAt = 1.2;
    v.playing = true;
    runUntil(2.2, [&](double t) { v.position = playFrom + (t - playAt); });
    playFrom = 60.0;  // seek while playing
    playAt = 2.2;
    runUntil(3.2, [&](double t) { v.position = playFrom + (t - playAt); });
    check(std::abs(host.plugins()[ex].gaugeValue - (61.0 / 120.0)) < 0.02, "gauge follows the extrapolated clock (~61 s of 120)");
    v.playing = false;
    screens[1].kind = PluginScreenSnapshot::Kind::Video;  // a vault video on the second screen
    screens[1].path = "/tmp/vault/secret-file.mp4";
    screens[1].fileName = "secret-file.mp4";
    screens[1].source = "local";
    screens[1].isPrivate = true;
    runUntil(3.6, nullptr);
    app.worn = false;
    runUntil(4.0, nullptr);
    screens[0] = PluginScreenSnapshot{};
    screens[0].selected = screens[0].audio = true;
    runUntil(4.6, nullptr);
    host.activateSetting(ex, 2);  // "Say hello"
    host.activateSetting(ex, 1);  // gauge: Seconds
    runUntil(7.0, nullptr);

    const int crash = findPlugin(host, "test.crash");
    const int silent = findPlugin(host, "test.silent");
    check(crash >= 0 && host.plugins()[crash].crashes >= 2, "crashing plugin is restarted with backoff");
    check(silent >= 0 && host.plugins()[silent].status == "Not responding", "silent plugin is stopped after 5 s");
    check(sawGauge, "gauge received");
    bool openedNotice = false, helloNotice = false;
    for (const std::string& n : notices) {
        openedNotice = openedNotice || n == "Example plugin: Opened Test Movie.mkv";
        helloNotice = helloNotice || n == "Example plugin: Hello from the example plugin";
    }
    check(openedNotice, "notice for the opened file");
    check(helloNotice, "action reached the plugin (hello notice)");

    host.shutdownAll();
    check(host.plugins()[ex].pid <= 0 && host.plugins()[silent].pid <= 0, "all plugins stopped on shutdown");

    const std::string log = readFile(fs::path(tmp) / ".local/share/pixelvr/plugin-data/org.pixelvr.example/events.log");
    check(countOf(log, "\"type\":\"hello\"") == 1, "hello received once");
    check(log.find("\"type\":\"screens\"") != std::string::npos, "screens received");
    check(log.find("\"fileName\":\"Test Movie.mkv\"") != std::string::npos, "media.open with the file name");
    check(log.find("secret") == std::string::npos, "no credentials and no vault file name reach the plugin");
    check(log.find("\"path\"") == std::string::npos, "no path without the playback.path permission");
    check(log.find("\"reason\":\"play\"") != std::string::npos, "play state");
    check(log.find("\"reason\":\"seek\"") != std::string::npos, "seek detected");
    check(log.find("\"reason\":\"pause\"") != std::string::npos, "pause state");
    check(countOf(log, "\"reason\":\"tick\"") >= 4, "periodic ticks while playing");
    check(log.find("\"private\":true") != std::string::npos, "vault video announced as private");
    check(log.find("\"worn\":false") != std::string::npos, "headset-off state");
    check(log.find("\"type\":\"media.close\",\"screen\":0") != std::string::npos, "media.close");
    check(log.find("\"gauge\":1") != std::string::npos, "settings change received");
    check(log.find("\"type\":\"action\"") != std::string::npos, "action received");
    check(log.find("\"type\":\"shutdown\"") != std::string::npos, "shutdown received");
    const std::string cfg = readFile(fs::path(tmp) / ".config/pixelvr/plugins.json");
    check(cfg.find("\"gauge\":\"1\"") != std::string::npos, "setting saved to plugins.json");

    std::printf("%s (%d failure%s); test home: %s\n", failures ? "FAILED" : "ALL PASSED", failures,
                failures == 1 ? "" : "s", tmp);
    if (!failures) fs::remove_all(tmp);
    return failures ? 1 : 0;
}

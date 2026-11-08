#include "plugins/plugin_host.hpp"

#include "../../browser/mini_json.hpp"
#include "util/logging.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#ifndef PIXELVR_VERSION_STRING
#define PIXELVR_VERSION_STRING "dev"
#endif

extern char** environ;

namespace pixelvr {

namespace fs = std::filesystem;

namespace {

using Clock = std::chrono::steady_clock;
constexpr size_t kMaxLine = 1u << 20;          // one message
constexpr size_t kMaxQueued = 4u << 20;        // unread output before a plugin is restarted
constexpr size_t kSkipTicksAbove = 256u << 10;  // a slow reader gets no periodic ticks
constexpr auto kHelloTimeout = std::chrono::seconds(5);
constexpr auto kTickEvery = std::chrono::milliseconds(250);
constexpr auto kNoticeGap = std::chrono::seconds(3);
constexpr int kMaxCrashes = 3;
const char* const kKnownPermissions[] = {"playback", "playback.path", "control", "files.sidecar", "ui", "network"};

std::string home() {
    const char* h = std::getenv("HOME");
    return h ? h : ".";
}
std::string configPath() { return home() + "/.config/pixelvr/plugins.json"; }
std::string dataDir(const std::string& id) { return home() + "/.local/share/pixelvr/plugin-data/" + id; }
std::string logPath(const std::string& id) { return home() + "/.cache/pixelvr/plugins/" + id + ".log"; }

// CLOCK_MONOTONIC in ms: the same clock a plugin reads (time.monotonic, steady_clock).
long long monoMs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<long long>(ts.tv_sec) * 1000LL + ts.tv_nsec / 1000000;
}
std::string num(double v) {
    char b[48];
    std::snprintf(b, sizeof(b), "%.3f", std::isfinite(v) ? v : 0.0);
    return b;
}
const char* jbool(bool b) { return b ? "true" : "false"; }

bool validId(const std::string& id) {
    if (id.empty() || id.size() > 96 || id[0] == '.') return false;
    return std::all_of(id.begin(), id.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '_';
    });
}
std::vector<std::string> stringArray(const pvrjson::Value* v) {
    std::vector<std::string> out;
    if (!v || v->type != pvrjson::Value::Type::Array) return out;
    for (const auto& e : v->array) {
        if (e.type == pvrjson::Value::Type::String) out.push_back(e.str);
    }
    return out;
}
std::string clip(std::string s, size_t n) {
    if (s.size() > n) s.resize(n);
    for (char& c : s) {
        if (static_cast<unsigned char>(c) < 0x20) c = ' ';
    }
    return s;
}
const char* kindName(PluginScreenSnapshot::Kind k) {
    return k == PluginScreenSnapshot::Kind::Video ? "video" : k == PluginScreenSnapshot::Kind::Web ? "web" : "empty";
}
const char* projectionName(int p) { return p == 1 ? "360" : p == 2 ? "180" : "flat"; }
const char* stereoName(int s) { return s == 1 ? "sbs" : s == 2 ? "tb" : "mono"; }

// Settings from a manifest or a ui.settings message; unknown types are skipped.
std::vector<PluginHost::Setting> parseSettings(const pvrjson::Value* arr) {
    std::vector<PluginHost::Setting> out;
    if (!arr || arr->type != pvrjson::Value::Type::Array) return out;
    for (const auto& e : arr->array) {
        PluginHost::Setting s;
        s.key = e.getString("key");
        s.type = e.getString("type");
        s.label = clip(e.getString("label"), 60);
        s.options = stringArray(e.get("options"));
        if (s.key.empty() || s.label.empty()) continue;
        const pvrjson::Value* d = e.get("default");
        if (s.type == "toggle") {
            s.def = (d && d->type == pvrjson::Value::Type::Bool && d->boolean) ? "1" : "0";
        } else if (s.type == "choice" || s.type == "segmented") {
            if (s.options.empty()) continue;
            int idx = d && d->type == pvrjson::Value::Type::Number ? static_cast<int>(d->number) : 0;
            idx = std::clamp(idx, 0, static_cast<int>(s.options.size()) - 1);
            s.def = std::to_string(idx);
        } else if (s.type == "text") {
            s.def = d && d->type == pvrjson::Value::Type::String ? d->str : std::string();
        } else if (s.type != "action") {
            continue;
        }
        out.push_back(std::move(s));
    }
    return out;
}

void loadManifest(const std::string& dir, PluginHost::Plugin& p) {
    p.dir = dir;
    p.id = fs::path(dir).filename().string();
    p.name = p.id;
    std::ifstream f(dir + "/plugin.json");
    if (!f) {
        p.manifestError = "plugin.json is missing";
        return;
    }
    const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    pvrjson::Value m;
    if (!pvrjson::parse(text, m) || m.type != pvrjson::Value::Type::Object) {
        p.manifestError = "plugin.json is not valid JSON";
        return;
    }
    if (!m.getString("id").empty()) p.id = m.getString("id");
    p.name = clip(m.getString("name").empty() ? p.id : m.getString("name"), 48);
    p.version = clip(m.getString("version"), 24);
    p.description = clip(m.getString("description"), 240);
    p.author = clip(m.getString("author"), 60);
    p.homepage = clip(m.getString("homepage"), 200);
    p.license = clip(m.getString("license"), 40);
    p.api = static_cast<int>(m.getNumber("api", 0));
    p.exec = stringArray(m.get("exec"));
    for (const std::string& perm : stringArray(m.get("permissions"))) {
        for (const char* known : kKnownPermissions) {
            if (perm == known) p.permissions.push_back(perm);
        }
    }
    p.sidecars = stringArray(m.get("sidecars"));
    p.settings = parseSettings(m.get("settings"));
    if (!validId(p.id)) {
        p.manifestError = "invalid plugin id";
    } else if (p.api > 1) {
        p.manifestError = "needs a newer Pixel VR Player";
    } else if (p.api != 1) {
        p.manifestError = "unsupported plugin API";
    } else if (p.exec.empty() || p.exec[0].empty()) {
        p.manifestError = "plugin.json has no \"exec\"";
    }
}

}  // namespace

bool PluginHost::Plugin::hasPermission(const std::string& perm) const {
    return std::find(permissions.begin(), permissions.end(), perm) != permissions.end();
}

PluginHost::~PluginHost() { shutdownAll(); }

std::string PluginHost::pluginsDir() { return home() + "/.local/share/pixelvr/plugins"; }

std::string PluginHost::sanitizePath(const std::string& path) {
    const size_t sch = path.find("://");
    if (sch == std::string::npos) return path;
    std::string out = path.substr(0, sch + 3);
    std::string rest = path.substr(sch + 3);
    const size_t cut = rest.find_first_of("?#");
    if (cut != std::string::npos) rest.resize(cut);
    const size_t slash = rest.find('/');
    const size_t at = rest.rfind('@', slash == std::string::npos ? std::string::npos : slash);
    if (at != std::string::npos && (slash == std::string::npos || at < slash)) rest.erase(0, at + 1);
    return out + rest;
}

void PluginHost::init() {
    initialised_ = true;
    rescan();
    loadConfig();
}

void PluginHost::rescan() {
    std::vector<Plugin> found;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(pluginsDir(), ec)) {
        if (!e.is_directory(ec)) continue;
        Plugin p;
        loadManifest(e.path().string(), p);
        found.push_back(std::move(p));
    }
    std::sort(found.begin(), found.end(), [](const Plugin& a, const Plugin& b) { return a.name < b.name; });
    // Plugins still installed keep their user configuration, running ones their
    // process (a changed manifest applies on the next restart).
    for (Plugin& n : found) {
        for (Plugin& o : plugins_) {
            if (o.id != n.id) continue;
            if (o.pid > 0 && o.dir == n.dir) {
                n = std::move(o);
                o.pid = -1;
                o.toFd = o.fromFd = -1;
            } else {
                n.enabled = o.enabled;
                n.vaultGrant = o.vaultGrant;
                n.values = o.values;
                n.crashes = o.crashes;
                n.restartAt = o.restartAt;
                if (n.manifestError.empty()) n.status = o.status;
            }
            break;
        }
    }
    for (Plugin& o : plugins_) {  // uninstalled while running
        if (o.pid <= 0) continue;
        kill(-o.pid, SIGKILL);
        waitpid(o.pid, nullptr, 0);
        if (o.toFd >= 0) close(o.toFd);
        if (o.fromFd >= 0) close(o.fromFd);
    }
    plugins_ = std::move(found);
    for (Plugin& p : plugins_) {
        if (!p.manifestError.empty()) {
            p.status = p.manifestError;
            p.statusLevel = "error";
        }
    }
}

void PluginHost::loadConfig() {
    std::ifstream f(configPath());
    if (!f) return;
    const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    pvrjson::Value cfg;
    if (!pvrjson::parse(text, cfg)) return;
    const pvrjson::Value* all = cfg.get("plugins");
    if (!all) return;
    for (Plugin& p : plugins_) {
        const pvrjson::Value* e = all->get(p.id);
        if (!e) continue;
        if (const pvrjson::Value* v = e->get("enabled")) p.enabled = v->type == pvrjson::Value::Type::Bool && v->boolean;
        if (const pvrjson::Value* v = e->get("vault")) p.vaultGrant = v->type == pvrjson::Value::Type::Bool && v->boolean;
        if (const pvrjson::Value* s = e->get("settings"); s && s->type == pvrjson::Value::Type::Object) {
            for (const auto& kv : s->object) {
                if (kv.second.type == pvrjson::Value::Type::String) p.values[kv.first] = kv.second.str;
            }
        }
        if (p.enabled && p.manifestError.empty()) p.status = "Starting...";
    }
}

void PluginHost::saveConfig() const {
    // Entries of plugins that are not installed right now are kept.
    pvrjson::Value old;
    {
        std::ifstream f(configPath());
        if (f) {
            const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            pvrjson::parse(text, old);
        }
    }
    std::string out = "{\"plugins\":{";
    bool first = true;
    for (const Plugin& p : plugins_) {
        if (!first) out += ',';
        first = false;
        out += pvrjson::quote(p.id) + ":{\"enabled\":" + jbool(p.enabled) + ",\"vault\":" + jbool(p.vaultGrant) +
               ",\"settings\":{";
        bool firstValue = true;
        for (const auto& kv : p.values) {
            if (!firstValue) out += ',';
            firstValue = false;
            out += pvrjson::quote(kv.first) + ":" + pvrjson::quote(kv.second);
        }
        out += "}}";
    }
    if (const pvrjson::Value* all = old.get("plugins"); all && all->type == pvrjson::Value::Type::Object) {
        for (const auto& kv : all->object) {
            const bool installed =
                std::any_of(plugins_.begin(), plugins_.end(), [&](const Plugin& p) { return p.id == kv.first; });
            if (installed) continue;
            if (!first) out += ',';
            first = false;
            out += pvrjson::quote(kv.first) + ":" + pvrjson::dump(kv.second);
        }
    }
    out += "}}\n";
    std::error_code ec;
    fs::create_directories(fs::path(configPath()).parent_path(), ec);
    const std::string tmp = configPath() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f) return;
        f << out;
    }
    fs::rename(tmp, configPath(), ec);
}

void PluginHost::start(Plugin& p) {
    if (p.pid > 0 || !p.manifestError.empty()) return;
    int inPipe[2];
    int outPipe[2];
    if (pipe2(inPipe, O_CLOEXEC) != 0) return;
    if (pipe2(outPipe, O_CLOEXEC) != 0) {
        close(inPipe[0]);
        close(inPipe[1]);
        return;
    }
    std::error_code ec;
    fs::create_directories(dataDir(p.id), ec);
    fs::create_directories(fs::path(logPath(p.id)).parent_path(), ec);
    if (fs::exists(logPath(p.id), ec) && fs::file_size(logPath(p.id), ec) > (1u << 20)) {
        fs::rename(logPath(p.id), logPath(p.id) + ".1", ec);
    }

    std::string prog = p.exec[0];
    bool searchPath = false;
    if (prog.find('/') != std::string::npos) {
        if (prog[0] != '/') prog = p.dir + "/" + prog;
    } else if (fs::exists(p.dir + "/" + prog, ec)) {
        prog = p.dir + "/" + prog;
    } else {
        searchPath = true;  // e.g. "python3"
    }
    std::vector<std::string> argStore = p.exec;
    argStore[0] = prog;
    std::vector<char*> argv;
    for (std::string& a : argStore) argv.push_back(a.data());
    argv.push_back(nullptr);

    // The player's own runtime environment (SteamVR libraries, overlay preloads,
    // bundled FFmpeg) must not leak into plugins.
    std::vector<std::string> envStore;
    for (char** e = environ; *e; ++e) {
        if (std::strncmp(*e, "LD_LIBRARY_PATH=", 16) == 0 || std::strncmp(*e, "LD_PRELOAD=", 11) == 0 ||
            std::strncmp(*e, "PIXELVR_", 8) == 0) {
            continue;
        }
        envStore.emplace_back(*e);
    }
    envStore.push_back("PIXELVR_PLUGIN_API=1");
    envStore.push_back("PIXELVR_PLUGIN_ID=" + p.id);
    envStore.push_back("PIXELVR_PLUGIN_DIR=" + p.dir);
    envStore.push_back("PIXELVR_PLUGIN_DATA=" + dataDir(p.id));
    std::vector<char*> envp;
    for (std::string& e : envStore) envp.push_back(e.data());
    envp.push_back(nullptr);

    const std::string log = logPath(p.id);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, inPipe[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&fa, outPipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
    posix_spawn_file_actions_addchdir_np(&fa, p.dir.c_str());
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t none;
    sigset_t defaults;
    sigemptyset(&none);
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGPIPE);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &defaults);
    posix_spawnattr_setpgroup(&attr, 0);  // own group: stop() also reaches its children
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETPGROUP);
    pid_t pid = -1;
    const int rc = searchPath ? posix_spawnp(&pid, prog.c_str(), &fa, &attr, argv.data(), envp.data())
                              : posix_spawn(&pid, prog.c_str(), &fa, &attr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    close(inPipe[0]);
    close(outPipe[1]);
    if (rc != 0) {
        close(inPipe[1]);
        close(outPipe[0]);
        p.status = std::string("Cannot start: ") + std::strerror(rc);
        p.statusLevel = "error";
        p.crashes = kMaxCrashes + 1;  // not a crash loop: a broken install
        PIXELVR_LOG_WARN("Plugin %s: cannot start %s: %s", p.id.c_str(), prog.c_str(), std::strerror(rc));
        return;
    }
    std::signal(SIGPIPE, SIG_IGN);  // a dead plugin must not kill the player
    p.pid = pid;
    p.toFd = inPipe[1];
    p.fromFd = outPipe[0];
    fcntl(p.toFd, F_SETFL, fcntl(p.toFd, F_GETFL) | O_NONBLOCK);
    fcntl(p.fromFd, F_SETFL, fcntl(p.fromFd, F_GETFL) | O_NONBLOCK);
    p.inBuf.clear();
    p.outBuf.clear();
    p.helloDone = false;
    p.stopping = false;
    p.startedAt = Clock::now();
    p.gaugeValue = -1.0f;
    p.status = "Starting...";
    p.statusLevel = "ok";
    PIXELVR_LOG_INFO("Plugin %s %s started (pid %d)", p.id.c_str(), p.version.c_str(), static_cast<int>(pid));
    sendHello(p);
}

void PluginHost::stop(Plugin& p, bool hard) {
    if (p.pid <= 0) return;
    if (!p.stopping) {
        send(p, "{\"type\":\"shutdown\"}");
        flush(p);
        p.stopping = true;
        p.termAt = Clock::now() + std::chrono::seconds(2);
        p.killAt = Clock::now() + std::chrono::seconds(3);
        p.status = "Stopping...";
    }
    if (hard) kill(-p.pid, SIGKILL);
}

void PluginHost::reap(Plugin& p) {
    if (p.pid <= 0) return;
    int st = 0;
    const pid_t r = waitpid(p.pid, &st, WNOHANG);
    if (r == 0 || (r < 0 && errno == EINTR)) return;
    if (p.toFd >= 0) close(p.toFd);
    if (p.fromFd >= 0) close(p.fromFd);
    p.toFd = p.fromFd = -1;
    p.pid = -1;
    p.helloDone = false;
    p.gaugeValue = -1.0f;
    const auto now = Clock::now();
    if (p.stopping) {
        p.stopping = false;
        p.status = p.enabled ? "Restarting..." : "Off";
        p.statusLevel = "ok";
        p.restartAt = now + std::chrono::milliseconds(200);
        return;
    }
    if (p.status == "Not responding") {  // killed for not answering hello: broken, not flaky
        p.crashes = kMaxCrashes + 1;
        return;
    }
    ++p.crashes;
    if (WIFEXITED(st)) {
        PIXELVR_LOG_WARN("Plugin %s exited with code %d", p.id.c_str(), WEXITSTATUS(st));
    } else if (WIFSIGNALED(st)) {
        PIXELVR_LOG_WARN("Plugin %s killed by signal %d", p.id.c_str(), WTERMSIG(st));
    }
    if (p.crashes > kMaxCrashes) {
        p.status = "Stopped after repeated crashes (see its log)";
        p.statusLevel = "error";
        return;
    }
    static const int kBackoffSec[] = {1, 5, 30};
    p.restartAt = now + std::chrono::seconds(kBackoffSec[std::min(p.crashes, 3) - 1]);
    p.status = "Stopped unexpectedly, restarting...";
    p.statusLevel = "warn";
}

void PluginHost::send(Plugin& p, const std::string& line) {
    if (p.pid <= 0 || p.toFd < 0) return;
    p.outBuf += line;
    p.outBuf += '\n';
    if (p.outBuf.size() > kMaxQueued) {
        PIXELVR_LOG_WARN("Plugin %s is not reading its input; restarting it", p.id.c_str());
        p.outBuf.clear();
        kill(-p.pid, SIGKILL);
    }
}

void PluginHost::flush(Plugin& p) {
    while (!p.outBuf.empty() && p.toFd >= 0) {
        const ssize_t n = write(p.toFd, p.outBuf.data(), p.outBuf.size());
        if (n > 0) {
            p.outBuf.erase(0, static_cast<size_t>(n));
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        break;  // EAGAIN (pipe full) or the plugin is gone
    }
}

void PluginHost::readFrom(Plugin& p) {
    if (p.fromFd < 0) return;
    char buf[65536];
    for (;;) {
        const ssize_t n = read(p.fromFd, buf, sizeof(buf));
        if (n > 0) {
            p.inBuf.append(buf, static_cast<size_t>(n));
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        break;  // EAGAIN or EOF (reap() notices the exit)
    }
    size_t start = 0;
    for (;;) {
        const size_t nl = p.inBuf.find('\n', start);
        if (nl == std::string::npos) break;
        if (nl > start) handleLine(p, p.inBuf.substr(start, nl - start));
        start = nl + 1;
        if (p.pid <= 0) break;
    }
    p.inBuf.erase(0, start);
    if (p.inBuf.size() > kMaxLine && p.pid > 0) {
        PIXELVR_LOG_WARN("Plugin %s sent a line over 1 MiB; restarting it", p.id.c_str());
        p.inBuf.clear();
        kill(-p.pid, SIGKILL);
    }
}

void PluginHost::handleLine(Plugin& p, const std::string& line) {
    pvrjson::Value m;
    if (!pvrjson::parse(line, m) || m.type != pvrjson::Value::Type::Object) return;
    const std::string type = m.getString("type");
    const pvrjson::Value* id = m.get("id");
    const bool ui = p.hasPermission("ui");
    if (type == "hello") {
        if (p.helloDone) return;
        p.helloDone = true;
        p.crashes = 0;
        if (p.status == "Starting...") p.status = "Running";
        sendScreens(p);
        sendAppState(p);
        for (int i = 0; i < static_cast<int>(tracks_.size()); ++i) {
            if (tracks_[i].mediaId.empty()) continue;
            sendOpen(p, i);
            sendState(p, i, "sync");
        }
    } else if (type == "status") {
        p.status = clip(m.getString("text"), 120);
        const std::string level = m.getString("level");
        p.statusLevel = (level == "warn" || level == "error") ? level : "ok";
    } else if (type == "log") {
        PIXELVR_LOG_INFO("Plugin %s: %s", p.id.c_str(), clip(m.getString("text"), 400).c_str());
    } else if (type == "notice" && ui) {
        const auto now = Clock::now();
        auto& last = lastNotice_[p.id];
        if (now - last < kNoticeGap) return;
        last = now;
        const std::string text = clip(m.getString("text"), 120);
        if (!text.empty() && notices_.size() < 8) notices_.push_back(p.name + ": " + text);
    } else if (type == "ui.gauge" && ui) {
        const pvrjson::Value* v = m.get("value");
        p.gaugeLabel = clip(m.getString("label"), 32);
        p.gaugeValue = (v && v->type == pvrjson::Value::Type::Number && v->number >= 0.0)
                           ? static_cast<float>(std::min(1.0, v->number))
                           : -1.0f;
    } else if (type == "ui.settings" && ui) {
        p.settings = parseSettings(m.get("settings"));
    } else if (id && id->type == pvrjson::Value::Type::Number) {
        // A request this host version does not offer (yet): answer, so the plugin does not wait.
        send(p, "{\"type\":\"result\",\"id\":" + pvrjson::dump(*id) + ",\"ok\":false,\"error\":" +
                    pvrjson::quote(type.empty() ? "missing type" : "unsupported: " + type) + "}");
    }
}

std::string PluginHost::settingsJson(const Plugin& p) const {
    std::string out = "{";
    bool first = true;
    for (const Setting& s : p.settings) {
        if (s.type == "action") continue;
        const auto it = p.values.find(s.key);
        const std::string v = it == p.values.end() ? s.def : it->second;
        if (!first) out += ',';
        first = false;
        out += pvrjson::quote(s.key) + ":";
        if (s.type == "toggle") {
            out += jbool(v == "1");
        } else if (s.type == "text") {
            out += pvrjson::quote(v);
        } else {
            out += std::to_string(std::clamp(std::atoi(v.c_str()), 0, std::max(0, static_cast<int>(s.options.size()) - 1)));
        }
    }
    return out + "}";
}

void PluginHost::sendHello(Plugin& p) {
    std::string granted = "[";
    for (size_t i = 0; i < p.permissions.size(); ++i) {
        if (i) granted += ',';
        granted += pvrjson::quote(p.permissions[i]);
    }
    granted += "]";
    send(p, std::string("{\"type\":\"hello\",\"api\":1,\"apiMinor\":0,\"app\":") + pvrjson::quote(PIXELVR_VERSION_STRING) +
                ",\"granted\":" + granted + ",\"settings\":" + settingsJson(p) + "}");
}

void PluginHost::sendSettings(Plugin& p) {
    if (p.helloDone) send(p, "{\"type\":\"settings\",\"settings\":" + settingsJson(p) + "}");
}

void PluginHost::sendScreens(Plugin& p) {
    if (!p.helloDone || !p.hasPermission("playback")) return;
    std::string out = "{\"type\":\"screens\",\"screens\":[";
    for (size_t i = 0; i < tracks_.size(); ++i) {
        const PluginScreenSnapshot& s = tracks_[i].last;
        if (i) out += ',';
        out += "{\"screen\":" + std::to_string(i) + ",\"kind\":\"" + kindName(s.kind) + "\",\"selected\":" +
               jbool(s.selected) + ",\"audio\":" + jbool(s.audio) + "}";
    }
    send(p, out + "]}");
}

void PluginHost::sendAppState(Plugin& p) {
    if (!p.helloDone) return;
    send(p, std::string("{\"type\":\"app.state\",\"focused\":") + jbool(app_.focused) + ",\"worn\":" + jbool(app_.worn) +
                ",\"exiting\":false}");
}

bool PluginHost::canSee(const Plugin& p, int screen) const {
    const PluginScreenSnapshot& s = tracks_[static_cast<size_t>(screen)].last;
    return !s.isPrivate || (p.vaultGrant && app_.vaultUnlocked);
}

void PluginHost::sendOpen(Plugin& p, int screen) {
    if (!p.helloDone || !p.hasPermission("playback")) return;
    const ScreenTrack& t = tracks_[static_cast<size_t>(screen)];
    std::string out = "{\"type\":\"media.open\",\"screen\":" + std::to_string(screen) + ",\"media\":" + pvrjson::quote(t.mediaId);
    if (!canSee(p, screen)) {
        send(p, out + ",\"private\":true}");
        return;
    }
    const PluginScreenSnapshot& s = t.last;
    out += ",\"private\":false,\"fileName\":" + pvrjson::quote(s.fileName) + ",\"title\":" + pvrjson::quote(s.title) +
           ",\"source\":" + pvrjson::quote(s.source) + ",\"duration\":" + num(s.duration) + ",\"projection\":\"" +
           projectionName(s.projection) + "\",\"stereo\":\"" + stereoName(s.stereo) + "\",\"sidecars\":[]";
    // Plex URLs carry the account token: never passed on, not even sanitised.
    if (p.hasPermission("playback.path") && s.source != "plex" && s.source != "http") {
        out += ",\"path\":" + pvrjson::quote(sanitizePath(s.path));
    }
    send(p, out + "}");
}

void PluginHost::sendState(Plugin& p, int screen, const char* reason) {
    if (!p.helloDone || !p.hasPermission("playback") || !canSee(p, screen)) return;
    const ScreenTrack& t = tracks_[static_cast<size_t>(screen)];
    const PluginScreenSnapshot& s = t.last;
    send(p, "{\"type\":\"media.state\",\"screen\":" + std::to_string(screen) + ",\"media\":" + pvrjson::quote(t.mediaId) +
                ",\"playing\":" + jbool(s.playing) + ",\"buffering\":" + jbool(s.buffering) + ",\"position\":" +
                num(s.position) + ",\"rate\":" + num(s.rate) + ",\"clock\":" + std::to_string(monoMs()) +
                ",\"reason\":\"" + reason + "\"}");
}

void PluginHost::sendClose(Plugin& p, int screen, const std::string& mediaId, const char* reason) {
    if (!p.helloDone || !p.hasPermission("playback")) return;
    send(p, "{\"type\":\"media.close\",\"screen\":" + std::to_string(screen) + ",\"media\":" + pvrjson::quote(mediaId) +
                ",\"reason\":\"" + reason + "\"}");
}

void PluginHost::broadcastScreens() {
    std::string sig;
    for (const ScreenTrack& t : tracks_) {
        sig += kindName(t.last.kind);
        sig += t.last.selected ? 'S' : '-';
        sig += t.last.audio ? 'A' : '-';
    }
    if (sig == screensSig_) return;
    screensSig_ = sig;
    for (Plugin& p : plugins_) sendScreens(p);
}

void PluginHost::update(const std::vector<PluginScreenSnapshot>& screens, const PluginAppSnapshot& app) {
    if (!initialised_) return;
    const auto now = Clock::now();

    // App state; the vault locking or unlocking changes what granted plugins may see.
    const bool vaultChanged = appKnown_ && app.vaultUnlocked != app_.vaultUnlocked;
    const bool appChanged = !appKnown_ || app.focused != app_.focused || app.worn != app_.worn;
    if (vaultChanged) {
        for (int i = 0; i < static_cast<int>(tracks_.size()); ++i) {
            if (tracks_[i].mediaId.empty() || !tracks_[i].last.isPrivate) continue;
            for (Plugin& p : plugins_) {
                if (p.vaultGrant) sendClose(p, i, tracks_[i].mediaId, "replaced");
            }
        }
    }
    app_ = app;
    appKnown_ = true;
    if (appChanged) {
        for (Plugin& p : plugins_) sendAppState(p);
    }
    if (vaultChanged) {
        for (int i = 0; i < static_cast<int>(tracks_.size()); ++i) {
            if (tracks_[i].mediaId.empty() || !tracks_[i].last.isPrivate) continue;
            for (Plugin& p : plugins_) {
                if (!p.vaultGrant) continue;
                sendOpen(p, i);
                sendState(p, i, "sync");
            }
        }
    }

    // Screens: diff against the last frame.
    if (tracks_.size() < screens.size()) tracks_.resize(screens.size());
    for (int i = 0; i < static_cast<int>(screens.size()); ++i) {
        const PluginScreenSnapshot& s = screens[static_cast<size_t>(i)];
        ScreenTrack& t = tracks_[static_cast<size_t>(i)];
        const bool isVideo = s.kind == PluginScreenSnapshot::Kind::Video && !s.path.empty();
        if (!t.mediaId.empty() && (!isVideo || s.path != t.last.path)) {
            const char* reason = !isVideo ? (t.last.ended ? "eof" : "stop") : "replaced";
            for (Plugin& p : plugins_) sendClose(p, i, t.mediaId, reason);
            t.mediaId.clear();
        }
        if (!isVideo) {
            t.last = s;
            continue;
        }
        if (t.mediaId.empty()) {
            t.mediaId = "m" + std::to_string(++mediaSerial_);
            t.last = s;
            for (Plugin& p : plugins_) {
                sendOpen(p, i);
                sendState(p, i, "open");
            }
            t.sentPosition = s.position;
            t.sentAt = now;
            t.nextTick = now + kTickEvery;
            continue;
        }
        const char* reason = nullptr;
        if (s.playing != t.last.playing) {
            reason = s.playing ? "play" : "pause";
        } else if (s.buffering != t.last.buffering) {
            reason = "buffer";
        } else if (std::fabs(s.rate - t.last.rate) > 1e-3) {
            reason = "rate";
        } else {
            const double elapsed = std::chrono::duration<double>(now - t.sentAt).count();
            const double expected =
                t.sentPosition + ((t.last.playing && !t.last.buffering) ? elapsed * t.last.rate : 0.0);
            if (std::fabs(s.position - expected) > 0.35) {
                reason = "seek";
            } else if (s.playing && now >= t.nextTick) {
                reason = "tick";
            }
        }
        t.last = s;
        if (!reason) continue;
        const bool tick = std::strcmp(reason, "tick") == 0;
        for (Plugin& p : plugins_) {
            if (tick && p.outBuf.size() > kSkipTicksAbove) continue;
            sendState(p, i, reason);
        }
        t.sentPosition = s.position;
        t.sentAt = now;
        t.nextTick = now + kTickEvery;
    }
    broadcastScreens();

    // Processes.
    for (Plugin& p : plugins_) {
        if (p.pid <= 0) {
            if (p.enabled && p.manifestError.empty() && p.crashes <= kMaxCrashes && now >= p.restartAt) start(p);
            continue;
        }
        readFrom(p);
        if (p.pid > 0 && !p.helloDone && !p.stopping && now - p.startedAt > kHelloTimeout) {
            PIXELVR_LOG_WARN("Plugin %s did not answer hello in time", p.id.c_str());
            p.status = "Not responding";
            p.statusLevel = "error";
            kill(-p.pid, SIGKILL);
        }
        if (p.stopping && now >= p.termAt) {
            kill(-p.pid, SIGTERM);
            p.termAt = Clock::time_point::max();
        }
        if (p.stopping && now >= p.killAt) kill(-p.pid, SIGKILL);
        flush(p);
        reap(p);
    }
}

void PluginHost::shutdownAll() {
    bool any = false;
    for (Plugin& p : plugins_) {
        if (p.pid <= 0) continue;
        stop(p, false);
        any = true;
    }
    if (!any) return;
    const auto start = Clock::now();
    for (;;) {
        bool alive = false;
        const auto now = Clock::now();
        for (Plugin& p : plugins_) {
            if (p.pid <= 0) continue;
            flush(p);
            if (now - start > std::chrono::seconds(2) && p.termAt != Clock::time_point::max()) {
                kill(-p.pid, SIGTERM);
                p.termAt = Clock::time_point::max();
            }
            if (now - start > std::chrono::seconds(3)) kill(-p.pid, SIGKILL);
            const bool wasStopping = p.stopping;
            reap(p);
            if (p.pid > 0) alive = true;
            else if (wasStopping) p.restartAt = Clock::time_point::max();
        }
        if (!alive) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

void PluginHost::setEnabled(int idx, bool on) {
    if (idx < 0 || idx >= static_cast<int>(plugins_.size())) return;
    Plugin& p = plugins_[static_cast<size_t>(idx)];
    p.enabled = on;
    p.crashes = 0;
    p.restartAt = {};
    if (on && p.pid <= 0 && p.manifestError.empty()) p.status = "Starting...";
    if (!on) {
        if (p.pid > 0) stop(p, false);
        else if (p.manifestError.empty()) p.status = "Off";
    }
    saveConfig();
}

void PluginHost::setVaultGrant(int idx, bool on) {
    if (idx < 0 || idx >= static_cast<int>(plugins_.size())) return;
    Plugin& p = plugins_[static_cast<size_t>(idx)];
    if (p.vaultGrant == on) return;
    // Re-announce private screens with the new level of detail.
    for (int i = 0; i < static_cast<int>(tracks_.size()); ++i) {
        if (tracks_[i].mediaId.empty() || !tracks_[i].last.isPrivate) continue;
        sendClose(p, i, tracks_[i].mediaId, "replaced");
    }
    p.vaultGrant = on;
    for (int i = 0; i < static_cast<int>(tracks_.size()); ++i) {
        if (tracks_[i].mediaId.empty() || !tracks_[i].last.isPrivate) continue;
        sendOpen(p, i);
        sendState(p, i, "sync");
    }
    saveConfig();
}

void PluginHost::restart(int idx) {
    if (idx < 0 || idx >= static_cast<int>(plugins_.size())) return;
    Plugin& p = plugins_[static_cast<size_t>(idx)];
    p.crashes = 0;
    p.restartAt = {};
    if (p.pid > 0) stop(p, false);
}

void PluginHost::activateSetting(int idx, int si) {
    if (idx < 0 || idx >= static_cast<int>(plugins_.size())) return;
    Plugin& p = plugins_[static_cast<size_t>(idx)];
    if (si < 0 || si >= static_cast<int>(p.settings.size())) return;
    const Setting& s = p.settings[static_cast<size_t>(si)];
    if (s.type == "action") {
        if (p.helloDone) send(p, "{\"type\":\"action\",\"action\":" + pvrjson::quote(s.key) + "}");
        return;
    }
    if (s.type == "text") return;  // edited through the keyboard: setTextSetting
    const auto it = p.values.find(s.key);
    const std::string cur = it == p.values.end() ? s.def : it->second;
    if (s.type == "toggle") {
        p.values[s.key] = cur == "1" ? "0" : "1";
    } else if (!s.options.empty()) {
        p.values[s.key] = std::to_string((std::atoi(cur.c_str()) + 1) % static_cast<int>(s.options.size()));
    }
    saveConfig();
    sendSettings(p);
}

void PluginHost::setTextSetting(int idx, int si, const std::string& text) {
    if (idx < 0 || idx >= static_cast<int>(plugins_.size())) return;
    Plugin& p = plugins_[static_cast<size_t>(idx)];
    if (si < 0 || si >= static_cast<int>(p.settings.size()) || p.settings[static_cast<size_t>(si)].type != "text") return;
    p.values[p.settings[static_cast<size_t>(si)].key] = clip(text, 400);
    saveConfig();
    sendSettings(p);
}

std::string PluginHost::settingValueLabel(int idx, int si) const {
    if (idx < 0 || idx >= static_cast<int>(plugins_.size())) return {};
    const Plugin& p = plugins_[static_cast<size_t>(idx)];
    if (si < 0 || si >= static_cast<int>(p.settings.size())) return {};
    const Setting& s = p.settings[static_cast<size_t>(si)];
    const auto it = p.values.find(s.key);
    const std::string v = it == p.values.end() ? s.def : it->second;
    if (s.type == "toggle") return v == "1" ? "On" : "Off";
    if (s.type == "text") return v.empty() ? "(empty)" : v;
    if (s.type == "action") return {};
    const int i = std::atoi(v.c_str());
    return (i >= 0 && i < static_cast<int>(s.options.size())) ? s.options[static_cast<size_t>(i)] : std::string();
}

bool PluginHost::takeNotice(std::string& out) {
    if (notices_.empty()) return false;
    out = std::move(notices_.front());
    notices_.pop_front();
    return true;
}

}  // namespace pixelvr

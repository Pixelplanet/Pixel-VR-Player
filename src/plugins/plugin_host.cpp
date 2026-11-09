#include "plugins/plugin_host.hpp"

#include "../../browser/mini_json.hpp"
#include "plugins/plugin_files.hpp"
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
constexpr size_t kMaxSidecarBytes = 16u << 20;
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

PluginHost::~PluginHost() {
    shutdownAll();
    stopWorker();
}

void PluginHost::queueJob(FileJob job) {
    {
        std::lock_guard<std::mutex> lk(jobMutex_);
        if (!worker_.joinable()) {
            workerStop_ = false;
            worker_ = std::thread([this] { workerLoop(); });
        }
        jobs_.push_back(std::move(job));
    }
    jobCv_.notify_one();
}

void PluginHost::workerLoop() {
    for (;;) {
        FileJob job;
        {
            std::unique_lock<std::mutex> lk(jobMutex_);
            jobCv_.wait(lk, [this] { return workerStop_ || !jobs_.empty(); });
            if (workerStop_) return;
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        FileResult r;
        r.job = job;
        r.ok = job.list ? pluginfiles::listFiles(job.uri, r.names, r.err)
                        : pluginfiles::readFile(job.uri, kMaxSidecarBytes, r.data, r.err);
        std::lock_guard<std::mutex> lk(jobMutex_);
        results_.push_back(std::move(r));
    }
}

void PluginHost::stopWorker() {
    {
        std::lock_guard<std::mutex> lk(jobMutex_);
        workerStop_ = true;
        jobs_.clear();
    }
    jobCv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void PluginHost::reply(Plugin& p, const std::string& id, bool ok, const std::string& dataOrError) {
    send(p, "{\"type\":\"result\",\"id\":" + id + ",\"ok\":" + (ok ? "true,\"data\":" + dataOrError
                                                                    : "false,\"error\":" + pvrjson::quote(dataOrError)) + "}");
}

std::vector<std::string> PluginHost::sidecarsFor(const Plugin& p, int screen) const {
    std::vector<std::string> out;
    const ScreenTrack& t = tracks_[static_cast<size_t>(screen)];
    if (!t.sidecarsKnown || !p.hasPermission("files.sidecar") || !canSee(p, screen)) return out;
    for (const std::string& name : t.sidecars) {
        for (const std::string& pat : p.sidecars) {
            if (pluginfiles::sidecarMatch(pat, t.base, name)) {
                out.push_back(name);
                break;
            }
        }
    }
    return out;
}

void PluginHost::sendSidecars(Plugin& p, int screen) {
    if (!p.helloDone || !p.hasPermission("playback") || !p.hasPermission("files.sidecar") || !canSee(p, screen)) return;
    std::string list;
    for (const std::string& n : sidecarsFor(p, screen)) list += (list.empty() ? "" : ",") + pvrjson::quote(n);
    send(p, "{\"type\":\"media.sidecars\",\"screen\":" + std::to_string(screen) + ",\"media\":" +
                pvrjson::quote(tracks_[static_cast<size_t>(screen)].mediaId) + ",\"sidecars\":[" + list + "]}");
}

void PluginHost::deliverFileResults() {
    std::deque<FileResult> done;
    {
        std::lock_guard<std::mutex> lk(jobMutex_);
        done.swap(results_);
    }
    for (FileResult& r : done) {
        const FileJob& j = r.job;
        if (j.screen < 0 || j.screen >= static_cast<int>(tracks_.size())) continue;
        ScreenTrack& t = tracks_[static_cast<size_t>(j.screen)];
        if (j.list) {
            if (t.mediaId != j.media) continue;  // another file opened meanwhile
            t.sidecarsKnown = true;
            t.sidecars.clear();
            for (const std::string& name : r.names) {
                const bool wanted = std::any_of(plugins_.begin(), plugins_.end(), [&](const Plugin& p) {
                    return std::any_of(p.sidecars.begin(), p.sidecars.end(),
                                       [&](const std::string& pat) { return pluginfiles::sidecarMatch(pat, t.base, name); });
                });
                if (wanted) t.sidecars.push_back(name);
            }
            if (!r.ok) PIXELVR_LOG_INFO("Plugins: no sidecars for screen %d (%s)", j.screen + 1, r.err.c_str());
            for (Plugin& p : plugins_) sendSidecars(p, j.screen);
            continue;
        }
        for (Plugin& p : plugins_) {
            if (p.id != j.pluginId || !p.helloDone) continue;
            if (!r.ok) {
                reply(p, j.reqId, false, r.err);
            } else if (pluginfiles::isUtf8(r.data)) {
                reply(p, j.reqId, true, "{\"name\":" + pvrjson::quote(j.name) + ",\"text\":" + pvrjson::quote(r.data) + "}");
            } else {
                reply(p, j.reqId, true, "{\"name\":" + pvrjson::quote(j.name) + ",\"base64\":\"" + pluginfiles::base64(r.data) + "\"}");
            }
        }
    }
}

const std::vector<float>& PluginHost::heatBins(int screen, int bins) {
    static const std::vector<float> none;
    if (screen < 0 || screen >= static_cast<int>(tracks_.size()) || bins <= 0) return none;
    const ScreenTrack& t = tracks_[static_cast<size_t>(screen)];
    const Plugin::Heat* heat = nullptr;
    std::string owner;
    for (const Plugin& p : plugins_) {
        if (p.pid <= 0 || !p.helloDone) continue;
        const auto it = p.heat.find(screen);
        if (it != p.heat.end() && !t.mediaId.empty() && it->second.media == t.mediaId) {
            heat = &it->second;
            owner = p.id;
            break;
        }
    }
    if (!heat || t.last.duration <= 0.0) return none;
    const std::string key = owner + "|" + t.mediaId + "|" + std::to_string(heatVersion_) + "|" + std::to_string(bins) +
                            "|" + std::to_string(static_cast<long long>(t.last.duration * 10.0));
    if (key == heatCacheKey_) return heatCache_;
    heatCacheKey_ = key;
    heatCache_.assign(static_cast<size_t>(bins), -1.0f);
    const double binMs = t.last.duration * 1000.0 / bins;
    for (const auto& s : heat->segments) {
        const int b0 = std::clamp(static_cast<int>(s[0] / binMs), 0, bins - 1);
        const int b1 = std::clamp(static_cast<int>(std::max(s[0], s[1] - 1.0f) / binMs), 0, bins - 1);
        for (int b = b0; b <= b1; ++b) heatCache_[static_cast<size_t>(b)] = std::max(heatCache_[static_cast<size_t>(b)], s[2]);
    }
    return heatCache_;
}

bool PluginHost::gauge(std::string& label, float& value) const {
    for (const Plugin& p : plugins_) {
        if (p.pid > 0 && p.helloDone && p.gaugeValue >= 0.0f) {
            label = p.gaugeLabel;
            value = p.gaugeValue;
            return true;
        }
    }
    label.clear();
    value = -1.0f;
    return false;
}

bool PluginHost::takeControl(ControlCommand& out) {
    if (controls_.empty()) return false;
    out = std::move(controls_.front());
    controls_.pop_front();
    return true;
}

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
    } else if (type == "ui.timeline" && ui) {
        const int screen = static_cast<int>(m.getNumber("screen", -1));
        if (screen < 0 || screen >= static_cast<int>(tracks_.size())) return;
        Plugin::Heat h;
        h.media = m.getString("media");
        if (const pvrjson::Value* tracks = m.get("tracks"); tracks && !tracks->array.empty()) {
            if (const pvrjson::Value* segs = tracks->array.front().get("segments")) {
                for (const auto& s : segs->array) {
                    if (s.array.size() < 3 || h.segments.size() >= 4000) continue;
                    h.segments.push_back({static_cast<float>(s.array[0].number), static_cast<float>(s.array[1].number),
                                          static_cast<float>(std::clamp(s.array[2].number, 0.0, 1.0))});
                }
            }
        }
        if (h.segments.empty()) p.heat.erase(screen);
        else p.heat[screen] = std::move(h);
        ++heatVersion_;
    } else if ((type == "files.read" || type == "control") && id && id->type == pvrjson::Value::Type::Number) {
        const std::string rid = pvrjson::dump(*id);
        const int screen = static_cast<int>(m.getNumber("screen", -1));
        const bool open = screen >= 0 && screen < static_cast<int>(tracks_.size()) &&
                          !tracks_[static_cast<size_t>(screen)].mediaId.empty() &&
                          tracks_[static_cast<size_t>(screen)].mediaId == m.getString("media");
        if (!p.hasPermission(type == "control" ? "control" : "files.sidecar")) {
            reply(p, rid, false, "permission not granted");
        } else if (!open || !canSee(p, screen)) {
            reply(p, rid, false, "that media is not open");
        } else if (type == "files.read") {
            const std::string name = m.getString("name");
            const auto allowed = sidecarsFor(p, screen);
            if (std::find(allowed.begin(), allowed.end(), name) == allowed.end()) {
                reply(p, rid, false, "not a sidecar of that media");
            } else {
                FileJob job;
                job.screen = screen;
                job.media = tracks_[static_cast<size_t>(screen)].mediaId;
                job.uri = pluginfiles::join(pluginfiles::parentOf(tracks_[static_cast<size_t>(screen)].last.path), name);
                job.pluginId = p.id;
                job.reqId = rid;
                job.name = name;
                queueJob(std::move(job));
            }
        } else {
            const std::string what = m.getString("do");
            if (what != "play" && what != "pause" && what != "seek") {
                reply(p, rid, false, "unsupported: " + what);
            } else {
                controls_.push_back({screen, what, std::max(0.0, m.getNumber("position"))});
                reply(p, rid, true, "{}");
            }
        }
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
           projectionName(s.projection) + "\",\"stereo\":\"" + stereoName(s.stereo) + "\",\"sidecars\":[";
    bool firstSidecar = true;
    for (const std::string& n : sidecarsFor(p, screen)) {
        out += (firstSidecar ? "" : ",") + pvrjson::quote(n);
        firstSidecar = false;
    }
    out += "]";
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
                ",\"playing\":" + jbool(s.playing) + ",\"buffering\":" + jbool(s.buffering) +
                ",\"scrubbing\":" + jbool(s.scrubbing) + ",\"position\":" +
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
            t.sidecarsKnown = false;
            t.sidecars.clear();
            t.base = pluginfiles::baseNameOf(s.path);
            const bool listable = s.source == "local" || s.source == "smb" || s.source == "sftp" || s.source == "webdav";
            const bool wanted = std::any_of(plugins_.begin(), plugins_.end(), [](const Plugin& p) {
                return p.hasPermission("files.sidecar") && !p.sidecars.empty();
            });
            if (listable && wanted) {
                FileJob job;
                job.list = true;
                job.screen = i;
                job.media = t.mediaId;
                job.uri = pluginfiles::parentOf(s.path);
                queueJob(std::move(job));
            }
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
        } else if (s.scrubbing != t.last.scrubbing) {
            reason = "scrub";
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
    deliverFileResults();

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
    stopWorker();
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

bool PluginHost::installPackage(const std::string& file, std::string& message) {
    std::error_code ec;
    fs::create_directories(pluginsDir(), ec);
    std::string tmpl = pluginsDir() + "/.install-XXXXXX";
    if (!mkdtemp(tmpl.data())) {
        message = "Cannot install: no room for a temporary folder";
        return false;
    }
    const std::string staging = tmpl;
    auto fail = [&](const std::string& why) {
        fs::remove_all(staging, ec);
        message = "Cannot install the plugin: " + why;
        PIXELVR_LOG_WARN("Plugin install %s: %s", file.c_str(), why.c_str());
        return false;
    };
    // GNU tar refuses absolute and ".." member names; links are rejected below.
    std::vector<std::string> argStore = {"tar", "-xzf", file, "-C", staging, "--no-same-owner"};
    std::vector<char*> argv;
    for (std::string& a : argStore) argv.push_back(a.data());
    argv.push_back(nullptr);
    pid_t pid = -1;
    if (posix_spawnp(&pid, "tar", nullptr, nullptr, argv.data(), environ) != 0) return fail("tar is missing");
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
    }
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) return fail("not a valid .pvrplugin file");
    std::string dir;
    int found = 0;
    for (const auto& e : fs::directory_iterator(staging, ec)) {
        if (e.is_directory(ec) && !e.is_symlink(ec) && fs::exists(e.path() / "plugin.json", ec)) {
            dir = e.path().string();
            ++found;
        }
    }
    if (found != 1) return fail("the package must contain exactly one plugin folder");
    for (const auto& e : fs::recursive_directory_iterator(staging, ec)) {
        if (e.is_symlink(ec)) return fail("the package contains links");
    }
    Plugin probe;
    loadManifest(dir, probe);
    if (!probe.manifestError.empty()) return fail(probe.manifestError);

    // Stop a running older version and wait for it (up to ~3 s).
    for (Plugin& p : plugins_) {
        if (p.id != probe.id || p.pid <= 0) continue;
        stop(p, false);
        const auto t0 = Clock::now();
        while (p.pid > 0) {
            flush(p);
            readFrom(p);
            const auto waited = Clock::now() - t0;
            if (waited > std::chrono::seconds(2)) kill(-p.pid, SIGTERM);
            if (waited > std::chrono::seconds(3)) {
                kill(-p.pid, SIGKILL);
                waitpid(p.pid, nullptr, 0);
            }
            reap(p);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    std::string target = pluginsDir() + "/" + probe.id;
    for (const Plugin& p : plugins_) {
        if (p.id == probe.id) target = p.dir;  // e.g. copied in by hand under another folder name
    }
    fs::remove_all(target + ".old", ec);
    if (fs::exists(target, ec)) fs::rename(target, target + ".old", ec);
    fs::rename(dir, target, ec);
    if (ec) {
        std::error_code ec2;
        fs::rename(target + ".old", target, ec2);  // put the old version back
        return fail(ec.message());
    }
    fs::remove_all(target + ".old", ec);
    fs::remove_all(staging, ec);
    rescan();
    bool enabled = false;
    for (const Plugin& p : plugins_) enabled = enabled || (p.id == probe.id && p.enabled);
    message = "Installed " + probe.name + " " + probe.version +
              (enabled ? ". It restarts with the new version." : ". Turn it on with its switch.");
    PIXELVR_LOG_INFO("Plugin %s %s installed from %s", probe.id.c_str(), probe.version.c_str(), file.c_str());
    return true;
}

bool PluginHost::probePackage(const std::string& file, PackageInfo& out) {
    std::error_code ec;
    const long long size = static_cast<long long>(fs::file_size(file, ec));
    if (ec) return false;
    const long long mtime = static_cast<long long>(fs::last_write_time(file, ec).time_since_epoch().count());
    auto it = packageCache_.find(file);
    if (it != packageCache_.end() && it->second.size == size && it->second.mtime == mtime) {
        out = it->second;
        return !out.id.empty();
    }
    PackageInfo info;
    info.size = size;
    info.mtime = mtime;
    char tmpl[] = "/tmp/pvrprobe.XXXXXX";
    if (mkdtemp(tmpl)) {
        const std::string staging = tmpl;
        std::vector<std::string> argStore = {"tar", "-xzf", file, "-C", staging, "--no-same-owner", "--wildcards", "*/plugin.json"};
        std::vector<char*> argv;
        for (auto& a : argStore) argv.push_back(a.data());
        argv.push_back(nullptr);
        pid_t pid = 0;
        if (posix_spawnp(&pid, "tar", nullptr, nullptr, argv.data(), environ) == 0) {
            int st = 0;
            waitpid(pid, &st, 0);
            for (const auto& e : fs::directory_iterator(staging, ec)) {
                if (!e.is_directory()) continue;
                Plugin probe;
                loadManifest(e.path().string(), probe);  // (the binary is not unpacked: only the fields matter)
                info.id = probe.id;
                info.name = probe.name;
                info.version = probe.version;
                break;
            }
        }
        fs::remove_all(staging, ec);
    }
    packageCache_[file] = info;
    out = info;
    return !info.id.empty();
}

int PluginHost::compareVersions(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() || j < b.size()) {
        long long x = 0, y = 0;
        while (i < a.size() && a[i] != '.') x = x * 10 + (std::isdigit(static_cast<unsigned char>(a[i])) ? a[i] - '0' : 0), ++i;
        while (j < b.size() && b[j] != '.') y = y * 10 + (std::isdigit(static_cast<unsigned char>(b[j])) ? b[j] - '0' : 0), ++j;
        if (x != y) return x < y ? -1 : 1;
        ++i;
        ++j;
    }
    return 0;
}

bool PluginHost::uninstall(int idx, std::string& message) {
    if (idx < 0 || idx >= static_cast<int>(plugins_.size())) return false;
    Plugin& p = plugins_[static_cast<size_t>(idx)];
    const std::string name = p.name, dir = p.dir, id = p.id;
    p.enabled = false;
    if (p.pid > 0) {
        stop(p, false);  // lets it stop its devices first
        const auto t0 = Clock::now();
        while (p.pid > 0) {
            flush(p);
            readFrom(p);
            const auto waited = Clock::now() - t0;
            if (waited > std::chrono::seconds(2)) kill(-p.pid, SIGTERM);
            if (waited > std::chrono::seconds(3)) {
                kill(-p.pid, SIGKILL);
                waitpid(p.pid, nullptr, 0);
            }
            reap(p);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
    rescan();
    saveConfig();
    if (ec) {
        message = "Could not remove " + name + ": " + ec.message();
        return false;
    }
    message = "Removed " + name + ".";
    PIXELVR_LOG_INFO("Plugin %s removed (%s)", id.c_str(), dir.c_str());
    return true;
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

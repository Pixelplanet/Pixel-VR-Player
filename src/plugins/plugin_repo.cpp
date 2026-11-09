#include "plugin_repo.hpp"

#include "../../browser/mini_json.hpp"
#include "../util/logging.hpp"

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#ifdef PIXELVR_HAVE_CURL
#include <curl/curl.h>
#endif

extern char** environ;

namespace pixelvr {
namespace fs = std::filesystem;

namespace {

#ifdef PIXELVR_HAVE_CURL
size_t toString(char* p, size_t s, size_t n, void* out) {
    static_cast<std::string*>(out)->append(p, s * n);
    return s * n;
}
size_t toFile(char* p, size_t s, size_t n, void* out) { return std::fwrite(p, s, n, static_cast<FILE*>(out)) * s; }
#endif

// GET into a string or a file; "" = fine, else the error.
std::string fetch(const std::string& url, std::string* body, FILE* file) {
#ifdef PIXELVR_HAVE_CURL
    CURL* c = curl_easy_init();
    if (!c) return "no network library";
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);  // GitHub release downloads redirect
    curl_easy_setopt(c, CURLOPT_USERAGENT, "PixelVRPlayer");
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
    if (body) {
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, toString);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, body);
    } else {
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, toFile);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, file);
    }
    const CURLcode rc = curl_easy_perform(c);
    curl_easy_cleanup(c);
    return rc == CURLE_OK ? std::string() : std::string(curl_easy_strerror(rc));
#else
    (void)url, (void)body, (void)file;
    return "this build has no network support";
#endif
}

std::string sha256Of(const std::string& file) {
    int pipeFd[2];
    if (pipe(pipeFd) != 0) return {};
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, pipeFd[1], 1);
    posix_spawn_file_actions_addclose(&fa, pipeFd[0]);
    std::string a0 = "sha256sum", a1 = file;
    char* argv[] = {a0.data(), a1.data(), nullptr};
    pid_t pid = 0;
    const bool ok = posix_spawnp(&pid, "sha256sum", &fa, nullptr, argv, environ) == 0;
    posix_spawn_file_actions_destroy(&fa);
    close(pipeFd[1]);
    std::string out;
    char buf[256];
    ssize_t n;
    while (ok && (n = read(pipeFd[0], buf, sizeof buf)) > 0) out.append(buf, static_cast<size_t>(n));
    close(pipeFd[0]);
    if (ok) waitpid(pid, nullptr, 0);
    return out.substr(0, out.find(' '));
}

}  // namespace

PluginRepo::~PluginRepo() { join(); }

void PluginRepo::join() {
    if (worker_.joinable()) worker_.join();
}

void PluginRepo::refresh(bool force) {
    if (busy_) return;
    {
        std::lock_guard<std::mutex> l(mu_);
        if (!force && fetchedAt_ != std::chrono::steady_clock::time_point{} &&
            std::chrono::steady_clock::now() - fetchedAt_ < std::chrono::minutes(10)) {
            return;
        }
        fetchedAt_ = std::chrono::steady_clock::now();
        if (entries_.empty()) state_ = "Loading the plugin list...";
    }
    join();
    busy_ = true;
    worker_ = std::thread([this] {
        std::string body;
        const std::string err = fetch(kIndexUrl, &body, nullptr);
        std::vector<Entry> list;
        pvrjson::Value root;
        if (err.empty() && pvrjson::parse(body, root)) {
            if (const pvrjson::Value* arr = root.get("plugins")) {
                for (const auto& p : arr->array) {
                    Entry e{p.getString("id"), p.getString("name"), p.getString("version"), p.getString("description"),
                            p.getString("url"), p.getString("sha256"), p.getString("minApp"),
                            static_cast<long long>(p.getNumber("size"))};
                    // Only packages from GitHub, with a checksum.
                    const bool trusted = e.url.rfind("https://github.com/", 0) == 0 ||
                                         e.url.rfind("https://raw.githubusercontent.com/", 0) == 0;
                    if (!e.id.empty() && !e.version.empty() && e.sha256.size() == 64 && trusted) list.push_back(e);
                }
            }
        }
        std::lock_guard<std::mutex> l(mu_);
        if (!err.empty() || list.empty()) {
            state_ = "Plugin list not available" + (err.empty() ? std::string() : " (" + err + ")");
            PIXELVR_LOG_WARN("Plugin repo: %s", state_.c_str());
            fetchedAt_ = std::chrono::steady_clock::now() - std::chrono::minutes(9);  // retry in a minute
        } else {
            entries_ = std::move(list);
            state_.clear();
            PIXELVR_LOG_INFO("Plugin repo: %zu plugins listed", entries_.size());
        }
        busy_ = false;
    });
}

std::vector<PluginRepo::Entry> PluginRepo::entries() const {
    std::lock_guard<std::mutex> l(mu_);
    return entries_;
}

std::string PluginRepo::state() const {
    std::lock_guard<std::mutex> l(mu_);
    return state_;
}

bool PluginRepo::downloading(const std::string& id) const {
    std::lock_guard<std::mutex> l(mu_);
    return busy_ && busyId_ == id;
}

void PluginRepo::download(const Entry& e) {
    if (busy_) return;
    join();
    {
        std::lock_guard<std::mutex> l(mu_);
        busyId_ = e.id;
    }
    busy_ = true;
    worker_ = std::thread([this, e] {
        const char* home = std::getenv("HOME");
        const std::string dir = std::string(home ? home : "/tmp") + "/.cache/pixelvr/plugins";
        std::error_code ec;
        fs::create_directories(dir, ec);
        const std::string file = dir + "/" + e.id + "-" + e.version + ".pvrplugin";
        std::string err;
        if (FILE* f = std::fopen(file.c_str(), "wb")) {
            err = fetch(e.url, nullptr, f);
            std::fclose(f);
        } else {
            err = "cannot write " + dir;
        }
        if (err.empty() && sha256Of(file) != e.sha256) err = "the download does not match its checksum";
        if (!err.empty()) fs::remove(file, ec);
        std::lock_guard<std::mutex> l(mu_);
        readyFile_ = err.empty() ? file : std::string();
        readyError_ = err.empty() ? std::string() : "Could not download " + e.name + ": " + err;
        ready_ = true;
        busyId_.clear();
        busy_ = false;
        PIXELVR_LOG_INFO("Plugin repo: %s %s download %s", e.id.c_str(), e.version.c_str(), err.empty() ? "ok" : err.c_str());
    });
}

bool PluginRepo::takeReady(std::string& file, std::string& error) {
    std::lock_guard<std::mutex> l(mu_);
    if (!ready_) return false;
    ready_ = false;
    file = readyFile_;
    error = readyError_;
    return true;
}

}  // namespace pixelvr

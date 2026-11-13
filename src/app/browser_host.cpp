#include "app/browser_host.hpp"

#include "../../browser/browser_protocol.hpp"
#include "../../browser/mini_json.hpp"
#include "util/logging.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;

namespace pixelvr {

namespace {

#ifndef PIXELVR_HAVE_BROWSER_HELPER
// Folder of the running executable (the helper lives in <exe dir>/browser).
std::string exeDir() {
    char buf[4096];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return ".";
    buf[n] = '\0';
    std::string p(buf);
    const size_t slash = p.rfind('/');
    return slash == std::string::npos ? std::string(".") : p.substr(0, slash);
}
#endif

// Starts the helper with `args` (after the program path), stdin/stdout piped.
pid_t spawnHelper(const std::vector<std::string>& args, int& toFd, int& fromFd) {
    toFd = fromFd = -1;
    int inPipe[2];
    int outPipe[2];
    if (pipe(inPipe) != 0) return -1;
    if (pipe(outPipe) != 0) {
        close(inPipe[0]);
        close(inPipe[1]);
        return -1;
    }
#ifdef PIXELVR_HAVE_BROWSER_HELPER
    // The helper is built into the player: run this executable in helper mode.
    std::string helper = "/proc/self/exe";
    {
        char buf[4096];
        const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n > 0) helper.assign(buf, static_cast<size_t>(n));
    }
    std::vector<std::string> argStore = {helper, "--pvr-browser"};
#else
    const std::string helper = exeDir() + "/browser/pvr-browser";
    std::vector<std::string> argStore = {helper};
#endif
    argStore.insert(argStore.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (std::string& a : argStore) argv.push_back(a.data());
    argv.push_back(nullptr);

    // The player's own library path (SteamVR runtime libraries) must not leak
    // into the browser.
    std::vector<std::string> envStore;
    for (char** e = environ; *e; ++e) {
        if (std::strncmp(*e, "LD_LIBRARY_PATH=", 16) == 0) continue;
        envStore.emplace_back(*e);
    }
    std::vector<char*> envp;
    for (std::string& e : envStore) envp.push_back(e.data());
    envp.push_back(nullptr);

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, inPipe[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&fa, outPipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&fa, inPipe[1]);
    posix_spawn_file_actions_addclose(&fa, outPipe[0]);
    pid_t pid = -1;
    const int rc = posix_spawn(&pid, helper.c_str(), &fa, nullptr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&fa);
    close(inPipe[0]);
    close(outPipe[1]);
    if (rc != 0) {
        PIXELVR_LOG_WARN("Browser: cannot start %s: %s", helper.c_str(), std::strerror(rc));
        close(inPipe[1]);
        close(outPipe[0]);
        return -1;
    }
    toFd = inPipe[1];
    fromFd = outPipe[0];
    fcntl(fromFd, F_SETFL, fcntl(fromFd, F_GETFL) | O_NONBLOCK);
    fcntl(toFd, F_SETFD, FD_CLOEXEC);
    fcntl(fromFd, F_SETFD, FD_CLOEXEC);
    std::signal(SIGPIPE, SIG_IGN);  // a dead helper must not kill the player
    return pid;
}

// Sends "quit", waits up to `waitMs`, then kills.
void stopHelper(pid_t& pid, int& toFd, int& fromFd, int waitMs) {
    if (pid > 0) {
        const char quit[] = "quit\n";
        if (toFd >= 0) {
            const ssize_t n = write(toFd, quit, sizeof(quit) - 1);
            (void)n;
        }
        bool exited = false;
        for (int i = 0; i < waitMs / 50 && !exited; ++i) {
            exited = waitpid(pid, nullptr, WNOHANG) == pid;
            if (!exited) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (!exited) {
            kill(pid, SIGKILL);
            waitpid(pid, nullptr, 0);
        }
        pid = -1;
    }
    if (toFd >= 0) close(toFd);
    if (fromFd >= 0) close(fromFd);
    toFd = fromFd = -1;
}

// The shared hidden Chromium: one launcher helper for all browser screens.
struct Launcher {
    pid_t pid = -1;
    int toFd = -1;
    int fromFd = -1;
    std::string out;
    int port = -1;
    std::string profile;  // Chromium profile in use (bookmarks)
    std::string error;    // last error the launcher reported
    std::chrono::steady_clock::time_point stopAt{};  // idle shutdown, when set
    int users = 0;
    bool failed = false;

    bool ensureStarted(int width, int height) {
        stopAt = {};  // in use again
        if (pid > 0) return true;
        failed = false;
        error.clear();
        port = -1;
        out.clear();
        pid = spawnHelper({"--launch", "--width", std::to_string(width), "--height", std::to_string(height)},
                          toFd, fromFd);
        if (pid <= 0) {
            failed = true;
            return false;
        }
        PIXELVR_LOG_INFO("Browser: starting Chromium (launcher pid %d)", static_cast<int>(pid));
        return true;
    }
    void poll() {
        if (pid <= 0) return;
        char buf[1024];
        for (;;) {
            const ssize_t n = read(fromFd, buf, sizeof(buf));
            if (n <= 0) break;
            out.append(buf, static_cast<size_t>(n));
        }
        size_t nl;
        while ((nl = out.find('\n')) != std::string::npos) {
            const std::string line = out.substr(0, nl);
            out.erase(0, nl + 1);
            if (line.rfind("profile ", 0) == 0) {
                profile = line.substr(8);
            } else if (line.rfind("port ", 0) == 0) {
                port = std::atoi(line.c_str() + 5);
                PIXELVR_LOG_INFO("Browser: Chromium ready on port %d", port);
            } else if (line.rfind("error ", 0) == 0) {
                PIXELVR_LOG_WARN("Browser: %s", line.c_str() + 6);
                error = line.substr(6);
            }
        }
        if (waitpid(pid, nullptr, WNOHANG) == pid) {
            PIXELVR_LOG_WARN("Browser: Chromium launcher exited");
            pid = -1;
            port = -1;
            failed = true;
            stopHelper(pid, toFd, fromFd, 0);
        }
    }
    void stop() {
        // Chromium takes ~3 s to quit: don't hold up the frame loop (the
        // headset watchdog would take a stall that long for an absent user).
        pid_t p = pid;
        int to = toFd, from = fromFd;
        pid = -1;
        toFd = fromFd = -1;
        port = -1;
        if (p > 0) std::thread([p, to, from]() mutable { stopHelper(p, to, from, 4000); }).detach();
    }
};

Launcher& launcher() {
    static Launcher l;
    return l;
}

int gShmCounter = 0;

}  // namespace

BrowserHost::~BrowserHost() { stop(); }

void BrowserHost::tickIdle() {
    Launcher& l = launcher();
    if (l.users == 0 && l.stopAt != std::chrono::steady_clock::time_point{} &&
        std::chrono::steady_clock::now() >= l.stopAt) {
        l.stopAt = {};
        l.stop();
    }
}

bool BrowserHost::start(const std::string& url, int width, int height, const Options& opt) {
    stop();
    options_ = opt;
    lastError_.clear();
    width_ = std::min(width, kBrowserMaxWidth);
    height_ = std::min(height, kBrowserMaxHeight);
    frameW_ = width_;
    frameH_ = height_;
    media_ = {};
    videoInfo_ = {};
    chapters_.clear();
    ++chaptersVersion_;
    mutedState_ = -1;
    volumeState_ = -1;
    focusEvents_.clear();
    plexPlays_.clear();
    saveFiles_.clear();
    fieldValueNew_ = false;

    // Shared memory for frames (the app owns and unlinks it).
    shmName_ = "/pixelvr-browser-" + std::to_string(getpid()) + "-" + std::to_string(++gShmCounter);
    shm_unlink(shmName_.c_str());
    const int fd = shm_open(shmName_.c_str(), O_CREAT | O_RDWR, 0600);
    if (fd < 0) {
        PIXELVR_LOG_WARN("Browser: shm_open failed: %s", std::strerror(errno));
        shmName_.clear();
        return false;
    }
    shmSize_ = browserShmSize(width_, kBrowserMaxHeight);  // room for the toolbar
    if (ftruncate(fd, static_cast<off_t>(shmSize_)) != 0) {
        close(fd);
        stop();
        return false;
    }
    shm_ = mmap(nullptr, shmSize_, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (shm_ == MAP_FAILED) {
        shm_ = nullptr;
        stop();
        return false;
    }
    auto* hd = static_cast<BrowserShmHeader*>(shm_);
    std::memcpy(hd->magic, "PVRBRW1", 8);
    hd->seq.store(0);
    lastSeq_ = 0;
    frame_.assign(static_cast<size_t>(width_) * kBrowserMaxHeight * 4, 0);

    if (!launcher().ensureStarted(width_, height_)) {
        stop();
        return false;
    }
    ++launcher().users;
    state_ = State::WaitingForBrowser;
    startUrl_ = url;
    url_ = url;
    title_.clear();
    loading_ = true;
    poll();
    return true;
}

bool BrowserHost::spawnPage(int port) {
    {
        std::lock_guard<std::mutex> lock(sendMutex_);
        std::vector<std::string> args = {"--port",   std::to_string(port), "--shm",   shmName_,
                                         "--url",    startUrl_,            "--width", std::to_string(width_),
                                         "--height", std::to_string(height_)};
        if (!launcher().profile.empty()) {
            args.push_back("--profile");
            args.push_back(launcher().profile);
        }
        if (options_.kiosk) args.push_back("--kiosk");
        if (options_.incognito) args.push_back("--incognito");
        if (options_.accent.size() == 6) {
            args.push_back("--accent");
            args.push_back(options_.accent);
        }
        if (options_.text.size() == 6) {
            args.push_back("--text");
            args.push_back(options_.text);
        }
        pid_ = spawnHelper(args, toHelper_, fromHelper_);
    }
    if (pid_ <= 0) return false;
    send(plexNative_ ? "plexnative 1" : "plexnative 0");
    PIXELVR_LOG_INFO("Browser: page helper pid %d (%dx%d) on %s", static_cast<int>(pid_), width_, height_,
                     startUrl_.c_str());
    return true;
}

void BrowserHost::stop() {
    {
        std::lock_guard<std::mutex> lock(sendMutex_);
        stopHelper(pid_, toHelper_, fromHelper_, 2000);
    }
    if (state_ != State::Idle) {
        state_ = State::Idle;
        Launcher& l = launcher();
        if (--l.users <= 0) {
            // Last browser screen: shut Chromium down a little later, so a new
            // screen soon after (e.g. YouTube -> Browser) reuses it with the
            // user's profile instead of finding it still locked.
            l.users = 0;
            l.stopAt = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        }
    }
    if (shm_) munmap(shm_, shmSize_);
    shm_ = nullptr;
    if (!shmName_.empty()) shm_unlink(shmName_.c_str());
    shmName_.clear();
    pendingOut_.clear();
}

void BrowserHost::poll() {
    if (state_ == State::WaitingForBrowser) {
        Launcher& l = launcher();
        l.poll();
        if (l.failed) {
            lastError_ = l.error.empty() ? "could not start Chromium" : l.error;
            stop();
            return;
        }
        if (l.port > 0) {
            if (!spawnPage(l.port)) {
                stop();
                return;
            }
            state_ = State::Running;
        }
        return;
    }
    if (state_ != State::Running) return;
    launcher().poll();
    char buf[4096];
    for (;;) {
        const ssize_t n = read(fromHelper_, buf, sizeof(buf));
        if (n <= 0) break;
        pendingOut_.append(buf, static_cast<size_t>(n));
    }
    size_t nl;
    while ((nl = pendingOut_.find('\n')) != std::string::npos) {
        handleLine(pendingOut_.substr(0, nl));
        pendingOut_.erase(0, nl + 1);
    }
    int status = 0;
    if (waitpid(pid_, &status, WNOHANG) == pid_) {
        PIXELVR_LOG_WARN("Browser: page helper exited (status %d)", status);
        pid_ = -1;
        stop();
    }
}

void BrowserHost::handleLine(const std::string& line) {
    const size_t sp = line.find(' ');
    const std::string cmd = line.substr(0, sp);
    const std::string rest = sp == std::string::npos ? std::string() : line.substr(sp + 1);
    if (cmd == "url") {
        url_ = rest;
    } else if (cmd == "title") {
        title_ = rest;
    } else if (cmd == "loading") {
        loading_ = (rest == "1");
    } else if (cmd == "media") {
        // "0" | "1 PAUSED POS DUR LIVE SEEKSTART SEEKEND"
        int present = 0, paused = 1, live = 0;
        double pos = 0.0, dur = 0.0, s0 = 0.0, s1 = 0.0;
        const int n = std::sscanf(rest.c_str(), "%d %d %lf %lf %d %lf %lf", &present, &paused, &pos, &dur, &live, &s0,
                                  &s1);
        media_ = {};
        if (n == 7 && present) {
            media_.seekStart = s0;
            media_.seekEnd = s1;
            media_.present = true;
            media_.paused = paused != 0;
            media_.position = pos;
            media_.duration = dur;
            media_.live = live != 0;
        }
    } else if (cmd == "chapters") {
        // "" | '{"v":VIDEO_ID,"c":[[START,TITLE],...]}'
        ChapterList list;
        pvrjson::Value v;
        if (!rest.empty() && pvrjson::parse(rest.c_str(), v)) {
            if (const pvrjson::Value* c = v.get("c"); c && c->type == pvrjson::Value::Type::Array) {
                for (const pvrjson::Value& e : c->array) {
                    if (e.type != pvrjson::Value::Type::Array || e.array.size() < 2) continue;
                    list.push_back({std::max(0.0, e.array[0].number), e.array[1].str});
                }
            }
        }
        std::sort(list.begin(), list.end(), [](const Chapter& a, const Chapter& b) { return a.start < b.start; });
        chapters_ = std::move(list);
        ++chaptersVersion_;
        if (!chapters_.empty()) PIXELVR_LOG_INFO("Browser: %zu chapters", chapters_.size());
    } else if (cmd == "videoinfo") {
        VideoInfo vi;
        int used = 0;
        if (std::sscanf(rest.c_str(), "%d %d %d %d %n", &vi.width, &vi.height, &vi.frames, &vi.dropped, &used) >= 4 &&
            used > 0) {
            pvrjson::Value c;
            if (pvrjson::parse(rest.c_str() + used, c)) vi.codecs = c.str;
            if (vi.codecs != videoInfo_.codecs || vi.width != videoInfo_.width || vi.height != videoInfo_.height) {
                PIXELVR_LOG_INFO("Browser: video %dx%d %s", vi.width, vi.height,
                                 vi.codecs.empty() ? "(codec unknown)" : vi.codecs.c_str());
            }
            videoInfo_ = vi;
        }
    } else if (cmd == "focus") {
        // "c|p 0" | "c|p 1 PASSWORD CURSOR <json string>"
        FocusEvent ev;
        ev.byClick = rest.rfind("c ", 0) == 0;
        const char* p = rest.c_str() + std::min<size_t>(2, rest.size());
        int focused = 0, pw = 0, cursor = 0, used = 0;
        if (std::sscanf(p, "%d %d %d %n", &focused, &pw, &cursor, &used) >= 3 && focused) {
            pvrjson::Value v;
            if (used > 0 && pvrjson::parse(p + used, v)) ev.value = v.str;
            ev.focused = true;
            ev.password = pw != 0;
            ev.cursor = cursor;
        }
        focusEvents_.push_back(std::move(ev));
    } else if (cmd == "plex") {
        pvrjson::Value v;
        if (pvrjson::parse(rest.c_str(), v) && !v.getString("url").empty()) {
            PlexPlay p;
            p.url = v.getString("url");
            p.title = v.getString("title");
            p.file = v.getString("file");
            p.offset = v.getNumber("offset");
            p.duration = v.getNumber("duration");
            if (!v.getString("service").empty()) p.service = v.getString("service");
            p.audioStream = static_cast<int>(v.getNumber("audio", -1));
            if (const pvrjson::Value* ep = v.get("episode")) p.episode = ep->boolean;
            p.episodeInfo = v.getString("info");
            plexPlays_.push_back(std::move(p));
        }
    } else if (cmd == "plexnext") {  // "plexnext none": no next episode
        plexNextNone_ = true;
    } else if (cmd == "savefile") {
        pvrjson::Value v;
        if (pvrjson::parse(rest.c_str(), v) && !v.getString("url").empty() && !v.getString("name").empty()) {
            saveFiles_.push_back({v.getString("url"), v.getString("name")});
        }
    } else if (cmd == "value") {
        int cursor = 0, used = 0;
        pvrjson::Value v;
        if (std::sscanf(rest.c_str(), "%d %n", &cursor, &used) >= 1 && used > 0 &&
            pvrjson::parse(rest.c_str() + used, v)) {
            fieldCursor_ = cursor;
            fieldValue_ = v.str;
            fieldValueNew_ = true;
        }
    } else if (cmd == "download") {
        PIXELVR_LOG_INFO("Browser: downloaded %s", rest.c_str());
    } else if (cmd == "error") {
        PIXELVR_LOG_WARN("Browser: %s", rest.c_str());
        lastError_ = rest;
    }
}

const uint8_t* BrowserHost::takeFrame() {
    if (!shm_ || state_ != State::Running) return nullptr;
    auto* hd = static_cast<const BrowserShmHeader*>(shm_);
    const uint64_t seq = hd->seq.load(std::memory_order_acquire);
    if (seq == lastSeq_ || (seq & 1u)) return nullptr;
    const int w = std::min(static_cast<int>(hd->width), width_);
    const int h = std::min(static_cast<int>(hd->height), kBrowserMaxHeight);
    if (w <= 0 || h <= 0) return nullptr;
    const auto* pixels = static_cast<const uint8_t*>(shm_) + sizeof(BrowserShmHeader);
    std::memcpy(frame_.data(), pixels, static_cast<size_t>(w) * h * 4);
    // Discard it if the helper started the next frame while we copied.
    std::atomic_thread_fence(std::memory_order_acquire);  // the copy happens before the re-check
    if (hd->seq.load(std::memory_order_relaxed) != seq) return nullptr;
    lastSeq_ = seq;
    frameW_ = w;
    frameH_ = h;
    return frame_.data();
}

void BrowserHost::send(const std::string& line) {
    std::lock_guard<std::mutex> lock(sendMutex_);
    if (toHelper_ < 0) return;
    const std::string l = line + "\n";
    const ssize_t n = write(toHelper_, l.data(), l.size());
    (void)n;
}

void BrowserHost::pauseMedia(bool includeLive) { send(includeLive ? "pausemedia 1" : "pausemedia 0"); }
void BrowserHost::toggleFullscreen() { send("fullscreen"); }

void BrowserHost::mediaSeek(double t) {
    char b[64];
    std::snprintf(b, sizeof(b), "media seek %.2f", t);
    send(b);
}

void BrowserHost::mediaSeekRelative(double d) {
    char b[64];
    std::snprintf(b, sizeof(b), "media seekrel %.2f", d);
    send(b);
}

void BrowserHost::setMuted(bool muted) {
    if (mutedState_ == (muted ? 1 : 0)) return;
    mutedState_ = muted ? 1 : 0;
    send(muted ? "mute 1" : "mute 0");
}

void BrowserHost::setVolume(float volume) {
    const int pct = static_cast<int>(std::lround(std::clamp(volume, 0.0f, 1.0f) * 100.0f));
    if (pct == volumeState_) return;
    volumeState_ = pct;
    send("volume " + std::to_string(pct));
}

void BrowserHost::setPlexNative(bool on) {
    if (on == plexNative_) return;
    plexNative_ = on;
    send(on ? "plexnative 1" : "plexnative 0");
}
void BrowserHost::plexTimeline(const char* state, int64_t ms) {
    send(std::string("plextimeline ") + state + " " + std::to_string(ms));
}
bool BrowserHost::takeSaveFile(SaveFile& out) {
    if (saveFiles_.empty()) return false;
    out = std::move(saveFiles_.front());
    saveFiles_.erase(saveFiles_.begin());
    return true;
}
bool BrowserHost::takePlexPlay(PlexPlay& out) {
    if (plexPlays_.empty()) return false;
    out = std::move(plexPlays_.front());
    plexPlays_.erase(plexPlays_.begin());
    return true;
}
bool BrowserHost::takeFocusEvent(FocusEvent& out) {
    if (focusEvents_.empty()) return false;
    out = std::move(focusEvents_.front());
    focusEvents_.erase(focusEvents_.begin());
    return true;
}

bool BrowserHost::takeFieldValue(int& cursor, std::string& value) {
    if (!fieldValueNew_) return false;
    fieldValueNew_ = false;
    cursor = fieldCursor_;
    value = fieldValue_;
    return true;
}

void BrowserHost::mouseMove(float x, float y) {
    if (std::fabs(x - lastMoveX_) < 0.5f && std::fabs(y - lastMoveY_) < 0.5f) return;
    lastMoveX_ = x;
    lastMoveY_ = y;
    char b[64];
    std::snprintf(b, sizeof(b), "move %.1f %.1f", x, y);
    send(b);
}

void BrowserHost::mouseDown(float x, float y) {
    char b[64];
    std::snprintf(b, sizeof(b), "down %.1f %.1f", x, y);
    send(b);
}

void BrowserHost::mouseUp(float x, float y) {
    char b[64];
    std::snprintf(b, sizeof(b), "up %.1f %.1f", x, y);
    send(b);
}

void BrowserHost::wheel(float x, float y, int deltaY) {
    char b[80];
    std::snprintf(b, sizeof(b), "wheel %.1f %.1f %d", x, y, deltaY);
    send(b);
}

void BrowserHost::key(const std::string& name) { send("key " + name); }
void BrowserHost::text(const std::string& utf8) { send("text " + utf8); }
void BrowserHost::navigate(const std::string& url) { send("nav " + url); }
void BrowserHost::openTab(const std::string& url) { send("tab " + url); }
void BrowserHost::back() { send("back"); }
void BrowserHost::forward() { send("forward"); }
void BrowserHost::reload() { send("reload"); }

}  // namespace pixelvr

#include "app/browser_host.hpp"

#include "../../browser/browser_protocol.hpp"
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
    const std::string helper = exeDir() + "/browser/pvr-browser";
    std::vector<std::string> argStore = {helper};
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
    int users = 0;
    bool failed = false;

    bool ensureStarted(int width, int height) {
        if (pid > 0) return true;
        failed = false;
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
            if (line.rfind("port ", 0) == 0) {
                port = std::atoi(line.c_str() + 5);
                PIXELVR_LOG_INFO("Browser: Chromium ready on port %d", port);
            } else if (line.rfind("error ", 0) == 0) {
                PIXELVR_LOG_WARN("Browser: %s", line.c_str() + 6);
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
        stopHelper(pid, toFd, fromFd, 4000);  // Chromium may take ~3 s to quit
        port = -1;
    }
};

Launcher& launcher() {
    static Launcher l;
    return l;
}

int gShmCounter = 0;

}  // namespace

BrowserHost::~BrowserHost() { stop(); }

bool BrowserHost::start(const std::string& url, int width, int height) {
    stop();
    width_ = std::min(width, kBrowserMaxWidth);
    height_ = std::min(height, kBrowserMaxHeight);

    // Shared memory for frames (the app owns and unlinks it).
    shmName_ = "/pixelvr-browser-" + std::to_string(getpid()) + "-" + std::to_string(++gShmCounter);
    shm_unlink(shmName_.c_str());
    const int fd = shm_open(shmName_.c_str(), O_CREAT | O_RDWR, 0600);
    if (fd < 0) {
        PIXELVR_LOG_WARN("Browser: shm_open failed: %s", std::strerror(errno));
        shmName_.clear();
        return false;
    }
    shmSize_ = browserShmSize(width_, height_);
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
    frame_.assign(static_cast<size_t>(width_) * height_ * 4, 0);

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
        pid_ = spawnHelper({"--port", std::to_string(port), "--shm", shmName_, "--url", startUrl_, "--width",
                            std::to_string(width_), "--height", std::to_string(height_)},
                           toHelper_, fromHelper_);
    }
    if (pid_ <= 0) return false;
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
            l.users = 0;
            l.stop();  // last browser screen: shut Chromium down
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
    if (cmd == "url") url_ = rest;
    else if (cmd == "title") title_ = rest;
    else if (cmd == "loading") loading_ = (rest == "1");
    else if (cmd == "error") PIXELVR_LOG_WARN("Browser: %s", rest.c_str());
}

const uint8_t* BrowserHost::takeFrame() {
    if (!shm_ || state_ != State::Running) return nullptr;
    auto* hd = static_cast<const BrowserShmHeader*>(shm_);
    const uint64_t seq = hd->seq.load(std::memory_order_acquire);
    if (seq == lastSeq_ || (seq & 1u)) return nullptr;
    const auto* pixels = static_cast<const uint8_t*>(shm_) + sizeof(BrowserShmHeader);
    std::memcpy(frame_.data(), pixels, frame_.size());
    // Discard it if the helper started the next frame while we copied.
    if (hd->seq.load(std::memory_order_acquire) != seq) return nullptr;
    lastSeq_ = seq;
    return frame_.data();
}

void BrowserHost::send(const std::string& line) {
    std::lock_guard<std::mutex> lock(sendMutex_);
    if (toHelper_ < 0) return;
    const std::string l = line + "\n";
    const ssize_t n = write(toHelper_, l.data(), l.size());
    (void)n;
}

void BrowserHost::pauseMedia() {
    send("eval document.querySelectorAll('video,audio').forEach(m=>m.pause())");
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
void BrowserHost::back() { send("back"); }
void BrowserHost::forward() { send("forward"); }
void BrowserHost::reload() { send("reload"); }

}  // namespace pixelvr

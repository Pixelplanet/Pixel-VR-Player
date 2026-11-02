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

// Folder of the running executable (the helper and its libraries live in
// <exe dir>/browser).
std::string exeDir() {
    char buf[4096];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return ".";
    buf[n] = '\0';
    std::string p(buf);
    const size_t slash = p.rfind('/');
    return slash == std::string::npos ? std::string(".") : p.substr(0, slash);
}

}  // namespace

BrowserHost::~BrowserHost() { stop(); }

bool BrowserHost::start(const std::string& url, int width, int height) {
    stop();
    width_ = std::min(width, kBrowserMaxWidth);
    height_ = std::min(height, kBrowserMaxHeight);

    // Shared memory for frames (the app owns and unlinks it).
    shmName_ = "/pixelvr-browser-" + std::to_string(getpid());
    shm_unlink(shmName_.c_str());
    const int fd = shm_open(shmName_.c_str(), O_CREAT | O_RDWR, 0600);
    if (fd < 0) {
        PIXELVR_LOG_WARN("Browser: shm_open failed: %s", std::strerror(errno));
        return false;
    }
    shmSize_ = browserShmSize(width_, height_);
    if (ftruncate(fd, static_cast<off_t>(shmSize_)) != 0) {
        close(fd);
        shm_unlink(shmName_.c_str());
        return false;
    }
    shm_ = mmap(nullptr, shmSize_, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (shm_ == MAP_FAILED) {
        shm_ = nullptr;
        shm_unlink(shmName_.c_str());
        return false;
    }
    auto* hd = static_cast<BrowserShmHeader*>(shm_);
    std::memcpy(hd->magic, "PVRBRW1", 8);
    hd->seq.store(0);
    lastSeq_ = 0;
    frame_.assign(static_cast<size_t>(width_) * height_ * 4, 0);

    int inPipe[2];
    int outPipe[2];
    if (pipe(inPipe) != 0) return false;
    if (pipe(outPipe) != 0) {
        close(inPipe[0]);
        close(inPipe[1]);
        return false;
    }

    const std::string dir = exeDir() + "/browser";
    const std::string helper = dir + "/pvr-browser";
    const std::string widthStr = std::to_string(width_);
    const std::string heightStr = std::to_string(height_);
    std::vector<std::string> argStore = {helper, "--shm", shmName_, "--url", url,
                                         "--width", widthStr, "--height", heightStr};
    std::vector<char*> argv;
    for (std::string& a : argStore) argv.push_back(a.data());
    argv.push_back(nullptr);

    // Environment: offscreen Qt, the private FFmpeg 6 next to the helper.
    std::vector<std::string> envStore;
    for (char** e = environ; *e; ++e) {
        if (std::strncmp(*e, "LD_LIBRARY_PATH=", 16) == 0 || std::strncmp(*e, "QT_QPA_PLATFORM=", 16) == 0) {
            continue;
        }
        envStore.emplace_back(*e);
    }
    envStore.push_back("LD_LIBRARY_PATH=" + dir + "/lib");
    envStore.push_back("QT_QPA_PLATFORM=offscreen");
    std::vector<char*> envp;
    for (std::string& e : envStore) envp.push_back(e.data());
    envp.push_back(nullptr);

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, inPipe[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&fa, outPipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&fa, inPipe[1]);
    posix_spawn_file_actions_addclose(&fa, outPipe[0]);
    const int rc = posix_spawn(&pid_, helper.c_str(), &fa, nullptr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&fa);
    close(inPipe[0]);
    close(outPipe[1]);
    if (rc != 0) {
        PIXELVR_LOG_WARN("Browser: cannot start %s: %s", helper.c_str(), std::strerror(rc));
        pid_ = -1;
        close(inPipe[1]);
        close(outPipe[0]);
        stop();
        return false;
    }
    toHelper_ = inPipe[1];
    fromHelper_ = outPipe[0];
    fcntl(fromHelper_, F_SETFL, fcntl(fromHelper_, F_GETFL) | O_NONBLOCK);
    fcntl(toHelper_, F_SETFD, FD_CLOEXEC);
    fcntl(fromHelper_, F_SETFD, FD_CLOEXEC);
    std::signal(SIGPIPE, SIG_IGN);  // a dead helper must not kill the player
    url_ = url;
    title_.clear();
    loading_ = true;
    PIXELVR_LOG_INFO("Browser: started helper pid %d (%dx%d) on %s", static_cast<int>(pid_), width_,
                     height_, url.c_str());
    return true;
}

void BrowserHost::stop() {
    if (pid_ > 0) {
        send("quit");
        bool exited = false;
        for (int i = 0; i < 40 && !exited; ++i) {  // up to ~2 s
            int status = 0;
            exited = waitpid(pid_, &status, WNOHANG) == pid_;
            if (!exited) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (!exited) {
            kill(pid_, SIGKILL);
            waitpid(pid_, nullptr, 0);
        }
        pid_ = -1;
    }
    if (toHelper_ >= 0) close(toHelper_);
    if (fromHelper_ >= 0) close(fromHelper_);
    toHelper_ = fromHelper_ = -1;
    if (shm_) munmap(shm_, shmSize_);
    shm_ = nullptr;
    if (!shmName_.empty()) shm_unlink(shmName_.c_str());
    shmName_.clear();
    pendingOut_.clear();
}

void BrowserHost::poll() {
    if (pid_ <= 0) return;
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
        PIXELVR_LOG_WARN("Browser: helper exited (status %d)", status);
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
}

const uint8_t* BrowserHost::takeFrame() {
    if (!shm_) return nullptr;
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
    if (toHelper_ < 0) return;
    const std::string l = line + "\n";
    const ssize_t n = write(toHelper_, l.data(), l.size());
    (void)n;
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

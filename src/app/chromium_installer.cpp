#include "app/chromium_installer.hpp"

#include "util/logging.hpp"

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace pixelvr {

ChromiumInstaller::~ChromiumInstaller() { cancel(); }

bool ChromiumInstaller::start() {
    if (state_ == State::Running) return true;
    int pipeFd[2];
    if (pipe(pipeFd) != 0) return false;
    std::vector<std::string> args = {"flatpak", "install", "--system", "-y", "--noninteractive",
                                     "flathub", "org.chromium.Chromium"};
    std::vector<char*> argv;
    for (std::string& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    // Our library path (SteamVR runtime) must not leak into flatpak.
    std::vector<std::string> envStore;
    for (char** e = environ; *e; ++e) {
        if (std::strncmp(*e, "LD_LIBRARY_PATH=", 16) != 0) envStore.emplace_back(*e);
    }
    std::vector<char*> envp;
    for (std::string& e : envStore) envp.push_back(e.data());
    envp.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, pipeFd[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&fa, pipeFd[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&fa, pipeFd[0]);
    const int rc = posix_spawnp(&pid_, "flatpak", &fa, nullptr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&fa);
    close(pipeFd[1]);
    if (rc != 0) {
        close(pipeFd[0]);
        pid_ = -1;
        state_ = State::Failed;
        error_ = "flatpak is not available";
        return false;
    }
    out_ = pipeFd[0];
    fcntl(out_, F_SETFL, fcntl(out_, F_GETFL) | O_NONBLOCK);
    fcntl(out_, F_SETFD, FD_CLOEXEC);
    state_ = State::Running;
    percent_ = -1;
    error_.clear();
    buffer_.clear();
    lastLine_.clear();
    PIXELVR_LOG_INFO("Installing Chromium (flatpak pid %d)", static_cast<int>(pid_));
    return true;
}

void ChromiumInstaller::cancel() {
    if (pid_ > 0) {
        kill(pid_, SIGTERM);
        waitpid(pid_, nullptr, 0);
        pid_ = -1;
    }
    if (out_ >= 0) close(out_);
    out_ = -1;
    if (state_ == State::Running) {
        state_ = State::Failed;
        error_ = "cancelled";
    }
}

void ChromiumInstaller::poll() {
    if (state_ != State::Running) return;
    char buf[4096];
    for (;;) {
        const ssize_t n = read(out_, buf, sizeof(buf));
        if (n <= 0) break;
        buffer_.append(buf, static_cast<size_t>(n));
    }
    // Progress lines end in \r (redrawn) or \n; keep the newest percentage
    // and the last line (the error, if it fails).
    size_t end;
    while ((end = buffer_.find_first_of("\r\n")) != std::string::npos) {
        const std::string line = buffer_.substr(0, end);
        buffer_.erase(0, end + 1);
        if (line.empty()) continue;
        lastLine_ = line;
        const size_t pct = line.rfind('%');
        if (pct != std::string::npos && pct > 0) {
            size_t start = pct;
            while (start > 0 && line[start - 1] >= '0' && line[start - 1] <= '9') --start;
            if (start < pct) percent_ = std::atoi(line.c_str() + start);
        }
    }
    int status = 0;
    if (waitpid(pid_, &status, WNOHANG) == pid_) {
        pid_ = -1;
        close(out_);
        out_ = -1;
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            state_ = State::Done;
            PIXELVR_LOG_INFO("Chromium installed");
        } else {
            state_ = State::Failed;
            error_ = lastLine_.empty() ? "installation failed" : lastLine_;
            PIXELVR_LOG_WARN("Chromium install failed: %s", error_.c_str());
        }
    }
}

}  // namespace pixelvr

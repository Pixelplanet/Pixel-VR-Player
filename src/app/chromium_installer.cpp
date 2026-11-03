#include "app/chromium_installer.hpp"

#include "util/logging.hpp"

#include <csignal>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace pixelvr {

ChromiumInstaller::~ChromiumInstaller() { cancel(); }

bool ChromiumInstaller::start() {
    if (state_ == State::Running) return true;
    // A pseudo-terminal, not a pipe: flatpak only shows progress (and flushes
    // its output as it goes) when writing to a terminal.
    const int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0) {
        if (master >= 0) close(master);
        return false;
    }
    const char* slaveName = ptsname(master);
    const int slave = slaveName ? open(slaveName, O_RDWR | O_NOCTTY) : -1;
    if (slave < 0) {
        close(master);
        return false;
    }
    winsize ws{};
    ws.ws_col = 160;  // wide enough that progress lines aren't cut
    ws.ws_row = 40;
    ioctl(slave, TIOCSWINSZ, &ws);
    // -y answers its questions; not --noninteractive, which hides the progress.
    std::vector<std::string> args = {"flatpak", "install", "--system", "-y", "flathub", "org.chromium.Chromium"};
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
    posix_spawn_file_actions_adddup2(&fa, slave, STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&fa, slave, STDERR_FILENO);
    posix_spawn_file_actions_addclose(&fa, master);
    const int rc = posix_spawnp(&pid_, "flatpak", &fa, nullptr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&fa);
    close(slave);
    if (rc != 0) {
        close(master);
        pid_ = -1;
        state_ = State::Failed;
        error_ = "flatpak is not available";
        return false;
    }
    out_ = master;
    fcntl(out_, F_SETFL, fcntl(out_, F_GETFL) | O_NONBLOCK);
    fcntl(out_, F_SETFD, FD_CLOEXEC);
    state_ = State::Running;
    percent_ = -1;
    rawPercent_ = -1;
    part_ = parts_ = 0;
    partPercent_ = 0;
    inEscape_ = false;
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
        const ssize_t n = read(out_, buf, sizeof(buf));  // EIO once flatpak is gone
        if (n <= 0) break;
        // Drop the terminal's escape sequences (the progress table redraws
        // itself with cursor moves), keep the text.
        for (ssize_t i = 0; i < n; ++i) {
            const char c = buf[i];
            if (c == '\x1b') {
                inEscape_ = true;
            } else if (inEscape_) {
                if ((c >= '@' && c <= '~') && c != '[') inEscape_ = false;
            } else {
                buffer_ += c;
            }
        }
    }
    // flatpak shows "Installing 2/3... 45%": the percentage is per part
    // (runtime pieces, locales, the app), so it is combined with the part
    // counter into one bar that only moves forward. A percentage before the
    // first counter (metadata) is ignored, unless no counter ever shows.
    auto digitAt = [&](size_t i) { return i < buffer_.size() && buffer_[i] >= '0' && buffer_[i] <= '9'; };
    for (size_t i = 0; i < buffer_.size(); ++i) {
        if (buffer_[i] == '/' && i > 0 && digitAt(i - 1) && digitAt(i + 1)) {
            size_t s = i;
            while (s > 0 && digitAt(s - 1)) --s;
            size_t e = i + 1;
            while (digitAt(e)) ++e;
            const int n = std::atoi(buffer_.c_str() + s);
            const int m = std::atoi(buffer_.c_str() + i + 1);
            // "2/3" after a space; not "MB/s" or a long object count
            if ((s == 0 || buffer_[s - 1] == ' ') && n >= 1 && n <= m && m <= 20) {
                if (n != part_ || m != parts_) partPercent_ = 0;
                part_ = n;
                parts_ = m;
            }
            i = e - 1;
        } else if (buffer_[i] == '%' && i > 0 && digitAt(i - 1)) {
            size_t s = i;
            while (s > 0 && digitAt(s - 1)) --s;
            if (i - s <= 3) {
                const int p = std::min(100, std::atoi(buffer_.c_str() + s));
                if (parts_ > 0) partPercent_ = p;
                else rawPercent_ = p;
            }
        }
    }
    int overall = parts_ > 0 ? ((part_ - 1) * 100 + partPercent_) / parts_ : rawPercent_;
    if (overall > percent_) percent_ = overall;
    // Whole lines are kept for the error message if it fails.
    size_t end;
    while ((end = buffer_.find_first_of("\r\n")) != std::string::npos) {
        const std::string line = buffer_.substr(0, end);
        buffer_.erase(0, end + 1);
        if (line.find_first_not_of(" \t") != std::string::npos) lastLine_ = line;
    }
    if (buffer_.size() > 4096) buffer_.erase(0, buffer_.size() - 1024);  // a line that never ends
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

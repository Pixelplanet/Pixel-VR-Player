#pragma once
// Installs Chromium (Flathub org.chromium.Chromium, as Discover does) for the
// browser screens, in the background: `flatpak install --system`. SteamOS's
// polkit rules let the logged-in user do that without a password.

#include <string>
#include <sys/types.h>

namespace pixelvr {

class ChromiumInstaller {
public:
    enum class State { Idle, Running, Done, Failed };

    ~ChromiumInstaller();
    bool start();
    void cancel();
    // Reads the installer's output; once per frame.
    void poll();
    State state() const { return state_; }
    int percent() const { return percent_; }          // -1 = not known yet
    const std::string& error() const { return error_; }  // when Failed
    void reset() {  // after the result was handled
        if (state_ != State::Running) state_ = State::Idle;
    }

private:
    State state_ = State::Idle;
    pid_t pid_ = -1;
    int out_ = -1;
    std::string buffer_;
    std::string lastLine_;
    bool inEscape_ = false;  // inside a terminal escape sequence
    int percent_ = -1;
    std::string error_;
};

}  // namespace pixelvr

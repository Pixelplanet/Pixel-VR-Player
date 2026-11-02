#pragma once
// Runs a pvr-browser page helper (see browser/pvr_browser.cpp) for one browser
// screen and talks to it: frames arrive through shared memory, input and
// navigation go out as text commands on its stdin, page events come back on
// its stdout. All browser screens share one hidden Chromium, started by a
// launcher helper with the first screen and stopped with the last.

#include <cstdint>
#include <string>
#include <sys/types.h>
#include <vector>

namespace pixelvr {

class BrowserHost {
public:
    BrowserHost() = default;
    ~BrowserHost();
    BrowserHost(const BrowserHost&) = delete;
    BrowserHost& operator=(const BrowserHost&) = delete;

    // Opens a page of width x height pixels on `url`. Returns at once; the
    // window appears once the shared Chromium is up (see poll()).
    bool start(const std::string& url, int width, int height);
    void stop();
    bool running() const { return state_ != State::Idle; }
    int width() const { return width_; }
    int height() const { return height_; }

    // Advances startup, reads helper events and notices exits; once per frame.
    void poll();
    // Latest complete frame (RGBA, width*height*4), or nullptr if nothing new.
    const uint8_t* takeFrame();

    // Input in page pixels.
    void mouseMove(float x, float y);
    void mouseDown(float x, float y);
    void mouseUp(float x, float y);
    void wheel(float x, float y, int deltaY);
    void key(const std::string& name);  // Enter, Backspace, Left, ...
    void text(const std::string& utf8);
    void navigate(const std::string& url);
    void back();
    void forward();
    void reload();

    const std::string& url() const { return url_; }
    const std::string& title() const { return title_; }
    bool loading() const { return loading_; }

private:
    enum class State { Idle, WaitingForBrowser, Running };
    bool spawnPage(int port);
    void send(const std::string& line);
    void handleLine(const std::string& line);

    State state_ = State::Idle;
    pid_t pid_ = -1;
    int toHelper_ = -1;    // helper stdin
    int fromHelper_ = -1;  // helper stdout (non-blocking)
    std::string shmName_;
    void* shm_ = nullptr;
    size_t shmSize_ = 0;
    int width_ = 0;
    int height_ = 0;
    uint64_t lastSeq_ = 0;
    std::vector<uint8_t> frame_;
    std::string pendingOut_;
    std::string startUrl_;
    std::string url_;
    std::string title_;
    bool loading_ = false;
    float lastMoveX_ = -1.0f;
    float lastMoveY_ = -1.0f;
};

}  // namespace pixelvr

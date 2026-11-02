#pragma once
// Runs the pvr-browser helper process (see browser/pvr_browser.cpp) and talks
// to it: frames arrive through shared memory, input and navigation go out as
// text commands on its stdin, page events come back on its stdout.

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

    // Starts the helper on `url` with a page of width x height pixels.
    bool start(const std::string& url, int width, int height);
    void stop();
    bool running() const { return pid_ > 0; }
    int width() const { return width_; }
    int height() const { return height_; }

    // Reads helper events and reaps it if it exited; call once per frame.
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
    void send(const std::string& line);
    void handleLine(const std::string& line);

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
    std::string url_;
    std::string title_;
    bool loading_ = false;
    float lastMoveX_ = -1.0f;
    float lastMoveY_ = -1.0f;
};

}  // namespace pixelvr

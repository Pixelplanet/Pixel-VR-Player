#pragma once
// Runs a pvr-browser page helper (see browser/pvr_browser.cpp) for one browser
// screen and talks to it: frames arrive through shared memory, input and
// navigation go out as text commands on its stdin, page events come back on
// its stdout. All browser screens share one hidden Chromium, started by a
// launcher helper with the first screen and stopped with the last.

#include "util/chapter.hpp"

#include <cstdint>
#include <mutex>
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

    struct Options {
        bool kiosk = false;      // just the page (YouTube), no toolbar
        bool incognito = false;  // private browser context
    };
    // Opens a page of width x height pixels on `url` (a full browser adds its
    // toolbar above the page). Returns at once; the window appears once the
    // shared Chromium is up (see poll()).
    bool start(const std::string& url, int width, int height, const Options& opt);
    void stop();
    bool running() const { return state_ != State::Idle; }
    bool kiosk() const { return options_.kiosk; }
    const std::string& startUrl() const { return startUrl_; }
    // Size of the last frame (page plus toolbar); input uses these pixels.
    int frameWidth() const { return frameW_; }
    int frameHeight() const { return frameH_; }

    // Shared Chromium: shuts it down once no browser screen used it for a
    // while (call once per frame). At app exit the launcher sees its input
    // close and quits Chromium itself.
    static void tickIdle();

    // Advances startup, reads helper events and notices exits; once per frame.
    void poll();
    // Latest complete frame (RGBA, frameWidth*frameHeight*4), or nullptr if
    // nothing new.
    const uint8_t* takeFrame();

    // Input in page pixels.
    void mouseMove(float x, float y);
    void mouseDown(float x, float y);
    void mouseUp(float x, float y);
    void wheel(float x, float y, int deltaY);
    void key(const std::string& name);  // Enter, Backspace, Left, ...
    void text(const std::string& utf8);
    void navigate(const std::string& url);
    void openTab(const std::string& url);  // new tab (kiosk: navigates)
    void back();
    void forward();
    void reload();
    void toggleFullscreen();  // F11 (or leaves the site's own fullscreen)
    // Pauses every <video>/<audio> in all tabs; livestreams only with
    // `includeLive`. Safe to call from another thread (the headset-removal
    // watchdog).
    void pauseMedia(bool includeLive);

    // The page's main video (media bridge for the dock).
    struct Media {
        bool present = false;
        bool paused = true;
        bool live = false;
        double position = 0.0;
        double duration = 0.0;
        double seekStart = 0.0;  // playable range (livestreams: rewind window)
        double seekEnd = 0.0;
    };
    const Media& media() const { return media_; }
    // The page video's details (stats overlay).
    struct VideoInfo {
        int width = 0;
        int height = 0;
        int frames = 0;   // decoded so far
        int dropped = 0;  // decoded too late to show
        std::string codecs;  // YouTube only, e.g. "av01.0.08M.08 (399) / opus (251)"
    };
    const VideoInfo& videoInfo() const { return videoInfo_; }
    // YouTube's chapters of the playing video; chaptersVersion() changes with them.
    const ChapterList& chapters() const { return chapters_; }
    unsigned chaptersVersion() const { return chaptersVersion_; }
    void mediaToggle() { send("media toggle"); }
    void mediaGoLive() { send("media live"); }
    void mediaSeek(double t);
    void mediaSeekRelative(double d);
    void setMuted(bool muted);
    void setVolume(float volume);  // 0..1, the page's media elements

    // Text fields: the page reports focus changes; the app types into
    // whatever is focused.
    struct FocusEvent {
        bool focused = false;
        bool byClick = false;  // right after a click (open the keyboard)
        bool password = false;
        int cursor = 0;
        std::string value;
    };
    bool takeFocusEvent(FocusEvent& out);
    // Latest value of the focused field after typing (cursor, text).
    bool takeFieldValue(int& cursor, std::string& value);
    void requestFocusReport() { send("keyboard"); }

    // Plex Web hand-off: with it on, pressing play on Plex's page is
    // intercepted and reported here (the page is paused) so the app can play
    // the original file itself.
    struct PlexPlay {
        std::string url;
        std::string title;
        double offset = 0.0;
        double duration = 0.0;
        std::string service = "plex";  // plex | jellyfin (same hand-off, docs/concepts/JELLYFIN_PLAN.md)
        int audioStream = -1;          // container index of the audio track chosen on the page (-1 = ours)
        bool episode = false;          // a TV episode: the next one starts when it ends
    };
    void setPlexNative(bool on);
    bool takePlexPlay(PlexPlay& out);
    // A Plex Web download the helper cancelled in Chromium: the app downloads `url` as `name`.
    struct SaveFile {
        std::string url;
        std::string name;
    };
    bool takeSaveFile(SaveFile& out);
    void plexRelease() { send("plexrelease"); }  // close the page's player, no timeline report
    void plexReturn(int64_t ms) { send("plexreturn " + std::to_string(ms)); }  // close Plex's player, report "stopped" at ms
    // An episode ended at ms: report it, and hand off the next one (a new PlexPlay), or answer "none".
    void plexNext(int64_t ms) { send("plexnext " + std::to_string(ms)); }
    bool takePlexNextNone() {
        const bool v = plexNextNone_;
        plexNextNone_ = false;
        return v;
    }
    void plexTimeline(const char* state, int64_t ms);  // playing | paused | stopped

    const std::string& url() const { return url_; }
    const std::string& title() const { return title_; }
    // The helper's last error (e.g. "Chromium is not installed ..."), for the user.
    const std::string& lastError() const { return lastError_; }
    bool loading() const { return loading_; }

private:
    enum class State { Idle, WaitingForBrowser, Running };
    bool spawnPage(int port);
    void send(const std::string& line);
    void handleLine(const std::string& line);

    State state_ = State::Idle;
    pid_t pid_ = -1;
    std::mutex sendMutex_;  // guards writes to / closing of toHelper_
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
    Options options_;
    std::string lastError_;
    int frameW_ = 0;
    int frameH_ = 0;
    Media media_;
    VideoInfo videoInfo_;
    ChapterList chapters_;
    unsigned chaptersVersion_ = 0;
    int mutedState_ = -1;
    int volumeState_ = -1;  // percent last sent
    std::vector<FocusEvent> focusEvents_;
    std::vector<PlexPlay> plexPlays_;
    bool plexNextNone_ = false;
    std::vector<SaveFile> saveFiles_;
    bool plexNative_ = true;
    bool fieldValueNew_ = false;
    int fieldCursor_ = 0;
    std::string fieldValue_;
    float lastMoveX_ = -1.0f;
    float lastMoveY_ = -1.0f;
};

}  // namespace pixelvr

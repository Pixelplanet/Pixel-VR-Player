#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pixelvr {

// A navigable file browser over local storage and SMB network shares. Pure model
// (no rendering): the app drives selection/navigation and the renderer draws the
// entries() list. SMB files are downloaded to local storage before playback.
class Browser {
public:
    struct Entry {
        std::string label;        // text shown in the list
        std::string path;         // local path or smb:// URI (empty for actions)
        bool isDir = false;       // navigable container
        bool isVideo = false;     // playable file
        bool isBack = false;      // ".." navigation
        bool isNetwork = false;   // "Network (SMB)" entry point
    };
    enum class Result { None, Navigated, Play };

    Browser();

    // (Re)opens the browser at the start location (local home directory).
    void open();
    bool isOpen() const { return open_; }
    void setOpen(bool o) { open_ = o; }

    const std::string& title() const { return title_; }
    const std::string& status() const { return status_; }
    const std::vector<Entry>& entries() const { return entries_; }
    int selected() const { return selected_; }
    void move(int delta);

    // Sets the highlighted entry directly (clamped); used by the VR pointer.
    void setSelected(int index) {
        const int n = static_cast<int>(entries_.size());
        selected_ = (n <= 0) ? 0 : (index < 0 ? 0 : (index >= n ? n - 1 : index));
    }

    // Activates the current selection. On Result::Play, `playPath` receives the
    // path or smb:// URI for the media engine (SMB files stream in place).
    Result activate(std::string& playPath);

    // Starts an async download of the focused SMB video to local storage (the
    // explicit "Download" action). Streaming is the default via activate().
    bool downloadFocused();

    // True while browsing an SMB share (as opposed to local storage).
    bool onNetwork() const { return inSmb_; }

    // Advances any in-progress SMB download. Returns Result::Play with a LOCAL
    // playPath when a download finishes; otherwise Result::None.
    Result poll(std::string& playPath);
    bool isBusy() const { return dlActive_.load() || scanning_.load(); }

    // Directory the "Download" action copies into (default ~/media).
    void setDownloadDir(const std::string& dir) { downloadDir_ = dir; }

    // Download state for a background status line shown even when the browser is
    // closed (downloads keep running while the panel is hidden).
    bool downloadActive() const { return dlActive_.load(); }
    std::string downloadText() const;

    ~Browser();
    Browser(const Browser&) = delete;
    Browser& operator=(const Browser&) = delete;

private:
    // Enumerated directory listing (local or SMB) produced off the main thread.
    struct ScanResult {
        std::vector<Entry> entries;
        std::string title;
        std::string status;
        std::string location;
        bool inSmb = false;
    };
    void beginScan(const std::string& location, bool isSmb);  // async navigate
    void runScan(std::string location, bool isSmb);           // worker body
    void scanLocalInto(const std::string& dir, ScanResult& out);
    void scanSmbInto(const std::string& uri, ScanResult& out);
    void startDownload(const std::string& uri, const std::string& name);

    bool open_ = false;
    bool inSmb_ = false;
    std::string title_;
    std::string status_;
    std::string location_;   // current local dir or smb:// URI
    std::string homeDir_;
    std::string lastLocation_;  // last folder visited (local or smb), for re-open
    bool lastInSmb_ = false;
    std::vector<Entry> entries_;
    int selected_ = 0;

    // Async directory scan (SMB discovery / large dirs) off the main thread.
    std::thread scanThread_;
    std::atomic<bool> scanning_{false};
    std::atomic<bool> scanReady_{false};  // results waiting to publish
    std::mutex scanMutex_;                 // guards scanResult_
    ScanResult scanResult_;

    // Async SMB download.
    std::thread dlThread_;
    std::atomic<bool> dlActive_{false};
    std::atomic<bool> dlDone_{false};
    std::atomic<bool> dlOk_{false};
    std::atomic<long long> dlBytes_{0};
    std::atomic<long long> dlTotal_{0};
    std::string dlLocalPath_;
    std::string dlName_;
    std::string downloadDir_;  // "" => default ~/media
};

} // namespace pixelvr

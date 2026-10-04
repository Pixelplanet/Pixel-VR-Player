#pragma once

#include <atomic>
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
    bool isBusy() const { return dlActive_.load(); }

    ~Browser();
    Browser(const Browser&) = delete;
    Browser& operator=(const Browser&) = delete;

private:
    void scanLocal(const std::string& dir);
    void scanSmb(const std::string& uri);  // "smb://" enumerates the network
    void startDownload(const std::string& uri, const std::string& name);

    bool open_ = false;
    bool inSmb_ = false;
    std::string title_;
    std::string status_;
    std::string location_;   // current local dir or smb:// URI
    std::string homeDir_;
    std::string lastLocalDir_;  // remembered across re-opens (until app restart)
    std::vector<Entry> entries_;
    int selected_ = 0;

    // Async SMB download.
    std::thread dlThread_;
    std::atomic<bool> dlActive_{false};
    std::atomic<bool> dlDone_{false};
    std::atomic<bool> dlOk_{false};
    std::atomic<long long> dlBytes_{0};
    std::atomic<long long> dlTotal_{0};
    std::string dlLocalPath_;
    std::string dlName_;
};

} // namespace pixelvr

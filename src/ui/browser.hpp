#pragma once

#include <atomic>
#include <chrono>
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
        std::string detail;       // human-readable type/size for the row
        long long size = 0;       // bytes (0 if unknown)
    };
    enum class Result { None, Navigated, Play };

    Browser();

    // (Re)opens the browser at the start location (local home directory).
    void open();
    // Opens the browser directly at a local directory (used when a finished
    // folder download is selected).
    void openPath(const std::string& localDir);
    bool isOpen() const { return open_; }
    // The download worker yields the shared SMB connection while the browser is
    // open, so directory listing stays responsive (downloads resume afterwards).
    void setOpen(bool o) {
        open_ = o;
        browseOpen_.store(o);
    }

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

    // Starts an async recursive copy of an SMB file OR folder into the download
    // directory (used after the caller confirms a folder download).
    void startDownload(const std::string& uri, const std::string& name,
                       bool isDir);

    // The entry currently selected (drives download/confirm), or nullptr.
    const Entry* focused() const;

    // True while browsing an SMB share (as opposed to local storage).
    bool onNetwork() const { return inSmb_; }

    // Advances any in-progress SMB download. Returns Result::Play with a LOCAL
    // playPath when a download finishes; otherwise Result::None.
    Result poll(std::string& playPath);
    bool isBusy() const { return dlActive_.load() || scanning_.load(); }
    // A directory scan is in flight (downloads are excluded: they run in a
    // separate process and must not block browsing).
    bool isScanning() const { return scanning_.load(); }

    // Directory the "Download" action copies into (default ~/media).
    void setDownloadDir(const std::string& dir) { downloadDir_ = dir; }

    // Download state for a background status line shown even when the browser is
    // closed (downloads keep running while the panel is hidden).
    bool downloadActive() const { return dlActive_.load(); }
    std::string downloadText() const;
    long long downloadBytes() const { return dlBytes_.load(); }
    long long downloadTotal() const { return dlTotal_.load(); }
    double downloadSpeed() const { return dlSpeed_; }  // bytes/sec
    const std::string& downloadName() const { return dlName_; }

    // Recent download jobs (for the downloads window), oldest first.
    struct DownloadJob {
        std::string name;
        std::string localPath;
        std::string uri;
        int state = 0;  // 0 downloading, 1 done, 2 failed, 3 stopped
        long long bytes = 0;
        long long total = 0;
        bool isDir = false;
    };
    const std::vector<DownloadJob>& downloadJobs() const { return dlJobs_; }
    bool hasDownloads() const { return !dlJobs_.empty(); }
    bool downloadPaused() const { return dlPaused_.load(); }
    void togglePause();   // SIGSTOP/SIGCONT the curl child
    void stopDownload();  // SIGKILL the child and drop the partial file
    // Removes finished/stopped/failed jobs (deleting any partial files).
    void clearFinished();

    // Background size probe for a folder (shown on the download confirmation).
    void measureFolder(const std::string& uri);
    bool measuring() const { return measuring_.load(); }
    long long measuredSize() const { return measuredSize_.load(); }

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
    // Navigation requested while a scan was already running (deferred so the
    // main thread never blocks joining a scan stuck on the SMB lock).
    std::string pendingScanLoc_;
    bool pendingScanSmb_ = false;
    bool hasPendingScan_ = false;
    void scanLocalInto(const std::string& dir, ScanResult& out);
    void scanSmbInto(const std::string& uri, ScanResult& out);
    void startDownloadFile(const std::string& uri, const std::string& local);
    void startDownloadFolder(const std::string& uri, const std::string& name);

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
    std::mutex scanMutex_;                // guards scanResult_
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
    std::vector<DownloadJob> dlJobs_;
    int dlActiveJob_ = -1;             // index of the job in flight, or -1
    std::atomic<bool> dlPaused_{false};
    std::atomic<bool> dlAbort_{false};
    std::atomic<bool> dlStopped_{false};
    std::atomic<bool> browseOpen_{false};  // worker yields while the panel is open
    // Downloads run as a CHILD PROCESS (curl) so they use their own SMB
    // connection and never contend with the browser's libsmbclient.
    std::atomic<int> dlPid_{-1};
    // Folder progress: bytes of completed files + the in-flight file's path.
    std::atomic<long long> dlBaseBytes_{0};
    std::mutex dlCurMutex_;
    std::string dlCurFile_;
    bool pausedByBrowse_ = false;  // child SIGSTOPped because the panel is open
    int spawnCurl(const std::string& uri, const std::string& local);
    // Folder size probe (background).
    std::thread measureThread_;
    std::atomic<bool> measuring_{false};
    std::atomic<long long> measuredSize_{0};
    double dlSpeed_ = 0.0;             // EMA bytes/sec for the status readout
    long long speedBytes_ = 0;
    std::chrono::steady_clock::time_point speedTime_{};
};

} // namespace pixelvr

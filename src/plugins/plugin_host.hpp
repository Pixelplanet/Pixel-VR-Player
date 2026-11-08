#pragma once
// Out-of-process plugins (docs/concepts/PLUGIN_SYSTEM_SPEC.md): each enabled
// plugin is a child process speaking JSON lines over stdin/stdout. The app
// hands the host one snapshot of its screens per frame; the host diffs it,
// emits events to the plugins allowed to see them and never blocks on them.

#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <sys/types.h>
#include <thread>
#include <vector>

namespace pixelvr {

// One screen as the app sees it this frame.
struct PluginScreenSnapshot {
    enum class Kind { Empty, Video, Web };
    Kind kind = Kind::Empty;
    bool selected = false;
    bool audio = false;        // this screen's sound is the one being heard
    // Video only. `path` may hold credentials or tokens: the host strips them.
    std::string path;          // identity of the open file (local path or network URI)
    std::string fileName;      // display file name (Plex: the title)
    std::string title;
    std::string source;        // local | smb | sftp | webdav | plex | http
    bool isPrivate = false;    // a vault item
    double duration = 0.0;     // seconds, 0 = unknown
    double position = 0.0;     // seconds, presentation time of the shown frame
    double rate = 1.0;
    bool playing = false;
    bool buffering = false;
    bool ended = false;        // reached the end of the file
    int projection = 0;        // 0 flat, 1 360, 2 180
    int stereo = 0;            // 0 mono, 1 SBS, 2 TB
};

struct PluginAppSnapshot {
    bool focused = true;       // XR session focused (no dashboard / system overlay)
    bool worn = true;          // headset on the user's head
    bool vaultUnlocked = false;
};

class PluginHost {
public:
    struct Setting {
        std::string key;
        std::string type;      // toggle | choice | segmented | action | text
        std::string label;
        std::vector<std::string> options;
        std::string def;       // default as stored: "1"/"0", option index, text
    };
    struct Plugin {
        // Manifest
        std::string dir, id, name, version, description, author, homepage, license;
        int api = 0;
        std::vector<std::string> exec, permissions, sidecars;
        std::vector<Setting> settings;
        std::string manifestError;  // non-empty: listed, but cannot run
        // User configuration (~/.config/pixelvr/plugins.json)
        bool enabled = false;
        bool vaultGrant = false;
        std::map<std::string, std::string> values;
        // Runtime
        pid_t pid = -1;
        int toFd = -1;
        int fromFd = -1;
        std::string inBuf, outBuf;
        bool helloDone = false;
        bool stopping = false;
        int crashes = 0;
        std::chrono::steady_clock::time_point startedAt{}, restartAt{}, termAt{}, killAt{};
        std::string status = "Off";
        std::string statusLevel = "ok";  // ok | warn | error
        std::string gaugeLabel;
        float gaugeValue = -1.0f;  // < 0: no gauge
        struct Heat {
            std::string media;
            std::vector<std::array<float, 3>> segments;  // startMs, endMs, heat 0..1
        };
        std::map<int, Heat> heat;  // ui.timeline per screen
        bool hasPermission(const std::string& p) const;
    };

    PluginHost() = default;
    ~PluginHost();
    PluginHost(const PluginHost&) = delete;
    PluginHost& operator=(const PluginHost&) = delete;

    // Finds installed plugins, loads the user configuration, starts enabled ones.
    void init();
    // Once per frame: diff the snapshot, emit events, read replies, restart or
    // stop processes. Never blocks.
    void update(const std::vector<PluginScreenSnapshot>& screens, const PluginAppSnapshot& app);
    // App exit: asks every plugin to stop, waits briefly, then kills.
    void shutdownAll();

    // Unpacks a .pvrplugin (gzip tar with one plugin folder) into the plugins
    // folder, replacing an older version (stopped first; it restarts if it was
    // on). Never runs the plugin: a new one stays off until the user enables it.
    bool installPackage(const std::string& file, std::string& message);

    std::vector<Plugin>& plugins() { return plugins_; }
    const std::vector<Plugin>& plugins() const { return plugins_; }
    void rescan();
    void setEnabled(int idx, bool on);
    void setVaultGrant(int idx, bool on);
    void restart(int idx);
    // Settings page: cycles a toggle / choice / segmented value, fires an action.
    void activateSetting(int idx, int settingIdx);
    void setTextSetting(int idx, int settingIdx, const std::string& text);
    std::string settingValueLabel(int idx, int settingIdx) const;

    // Short messages for the in-VR notice line (rate-limited per plugin).
    bool takeNotice(std::string& out);

    // Playback commands from plugins with the "control" permission; the app applies them.
    struct ControlCommand {
        int screen = 0;
        std::string action;  // play | pause | seek
        double position = 0.0;
    };
    bool takeControl(ControlCommand& out);

    // For the renderer: the first running plugin's heat track for what `screen`
    // shows, as `bins` equal-width bins (heat 0..1, -1 = none; empty = no track),
    // and the first visible gauge (false = none). Cached; cheap per frame.
    const std::vector<float>& heatBins(int screen, int bins);
    bool gauge(std::string& label, float& value) const;

    static std::string pluginsDir();
    static std::string sanitizePath(const std::string& path);  // removes credentials / query

private:
    using Clock = std::chrono::steady_clock;
    struct ScreenTrack {
        PluginScreenSnapshot last;
        std::string mediaId;     // "" = nothing open
        double sentPosition = 0.0;
        Clock::time_point sentAt{};
        Clock::time_point nextTick{};
        bool sidecarsKnown = false;
        std::string base;                     // media file name without extension
        std::vector<std::string> sidecars;    // files next to it matching any plugin's patterns
    };
    // Sidecar listing and reading run on a worker thread (network shares are slow).
    struct FileJob {
        bool list = false;
        int screen = 0;
        std::string media, uri, pluginId, reqId, name;
    };
    struct FileResult {
        FileJob job;
        bool ok = false;
        std::string err, data;
        std::vector<std::string> names;
    };
    void workerLoop();
    void stopWorker();
    void deliverFileResults();
    void queueJob(FileJob job);
    std::vector<std::string> sidecarsFor(const Plugin& p, int screen) const;
    void sendSidecars(Plugin& p, int screen);
    void reply(Plugin& p, const std::string& id, bool ok, const std::string& dataOrError);

    void loadConfig();
    void saveConfig() const;
    void start(Plugin& p);
    void stop(Plugin& p, bool kill);
    void reap(Plugin& p);
    void readFrom(Plugin& p);
    void handleLine(Plugin& p, const std::string& line);
    void send(Plugin& p, const std::string& line);
    void flush(Plugin& p);
    void sendHello(Plugin& p);
    void sendScreens(Plugin& p);
    void sendOpen(Plugin& p, int screen);
    void sendState(Plugin& p, int screen, const char* reason);
    void sendClose(Plugin& p, int screen, const std::string& mediaId, const char* reason);
    void sendAppState(Plugin& p);
    void sendSettings(Plugin& p);
    void broadcastScreens();
    bool canSee(const Plugin& p, int screen) const;  // vault rule
    std::string settingsJson(const Plugin& p) const;

    std::vector<Plugin> plugins_;
    std::vector<ScreenTrack> tracks_;
    PluginAppSnapshot app_;
    bool appKnown_ = false;
    std::string screensSig_;
    unsigned mediaSerial_ = 0;
    std::deque<std::string> notices_;
    std::map<std::string, Clock::time_point> lastNotice_;
    std::deque<ControlCommand> controls_;
    unsigned heatVersion_ = 0;  // bumped on every ui.timeline
    std::vector<float> heatCache_;
    std::string heatCacheKey_;
    bool initialised_ = false;
    std::thread worker_;
    std::mutex jobMutex_;
    std::condition_variable jobCv_;
    std::deque<FileJob> jobs_;
    std::deque<FileResult> results_;
    bool workerStop_ = false;
};

}  // namespace pixelvr

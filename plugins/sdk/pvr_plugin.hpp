#pragma once
// Header-only helper for Pixel VR Player plugins written in C++ (plugin API 1,
// docs/concepts/PLUGIN_SYSTEM_SPEC.md). A plugin is a normal program: the
// player writes JSON lines to its stdin and reads JSON lines from its stdout.
// stderr goes to ~/.cache/pixelvr/plugins/<id>.log.

#include "../../browser/mini_json.hpp"

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <ctime>
#include <deque>
#include <map>
#include <poll.h>
#include <string>
#include <unistd.h>
#include <vector>

namespace pvrplugin {

// CLOCK_MONOTONIC in ms: the clock the player stamps media.state with.
inline long long monoMs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<long long>(ts.tv_sec) * 1000LL + ts.tv_nsec / 1000000;
}

inline std::string num(double v) {
    char b[48];
    std::snprintf(b, sizeof(b), "%.4f", v);
    return b;
}

class Io {
public:
    Io() { std::signal(SIGPIPE, SIG_IGN); }

    // Waits up to `timeoutMs` for input on stdin (and on `extra` fds, whose
    // revents are filled in). Returns false once the player has gone away.
    bool pump(int timeoutMs, std::vector<pollfd>* extra = nullptr) {
        if (eof_) return false;
        std::vector<pollfd> fds{{STDIN_FILENO, POLLIN, 0}};
        if (extra) fds.insert(fds.end(), extra->begin(), extra->end());
        const int r = ::poll(fds.data(), fds.size(), timeoutMs);
        if (r < 0 && errno != EINTR) eof_ = true;
        if (extra) {
            for (size_t i = 0; i < extra->size(); ++i) (*extra)[i].revents = fds[i + 1].revents;
        }
        if (r > 0 && (fds[0].revents & (POLLIN | POLLHUP | POLLERR))) {
            char buf[65536];
            const ssize_t n = ::read(STDIN_FILENO, buf, sizeof(buf));
            if (n > 0) {
                in_.append(buf, static_cast<size_t>(n));
            } else if (n == 0 || errno != EINTR) {
                eof_ = true;
            }
        }
        size_t start = 0;
        for (size_t nl; (nl = in_.find('\n', start)) != std::string::npos; start = nl + 1) {
            pvrjson::Value v;
            if (nl > start && pvrjson::parse(std::string_view(in_).substr(start, nl - start), v)) {
                queue_.push_back(std::move(v));
            }
        }
        in_.erase(0, start);
        return !eof_ || !queue_.empty();
    }

    // Pops the next message from the player, if any.
    bool next(pvrjson::Value& out) {
        if (queue_.empty()) return false;
        out = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    bool closed() const { return eof_ && queue_.empty(); }

    void send(const std::string& json) {
        std::string line = json + "\n";
        size_t off = 0;
        while (off < line.size()) {
            const ssize_t n = ::write(STDOUT_FILENO, line.data() + off, line.size() - off);
            if (n > 0) {
                off += static_cast<size_t>(n);
            } else if (n < 0 && errno == EINTR) {
                continue;
            } else {
                eof_ = true;  // the player is gone
                return;
            }
        }
    }

    void hello(const std::string& name, const std::string& version) {
        send("{\"type\":\"hello\",\"api\":1,\"name\":" + pvrjson::quote(name) + ",\"version\":" + pvrjson::quote(version) + "}");
    }
    void status(const std::string& text, const char* level = "ok") {
        send("{\"type\":\"status\",\"text\":" + pvrjson::quote(text) + ",\"level\":\"" + level + "\"}");
    }
    void notice(const std::string& text) { send("{\"type\":\"notice\",\"text\":" + pvrjson::quote(text) + "}"); }
    void log(const std::string& text) { send("{\"type\":\"log\",\"level\":\"info\",\"text\":" + pvrjson::quote(text) + "}"); }
    // value < 0 hides the gauge.
    void gauge(const std::string& label, double value) {
        send("{\"type\":\"ui.gauge\",\"id\":\"main\",\"label\":" + pvrjson::quote(label) + ",\"value\":" + num(value) + "}");
    }
    // Requests answered by a {"type":"result","id":N,...} message; each returns N.
    int readSidecar(int screen, const std::string& media, const std::string& name) {
        const int id = ++lastId_;
        send("{\"type\":\"files.read\",\"id\":" + std::to_string(id) + ",\"screen\":" + std::to_string(screen) +
             ",\"media\":" + pvrjson::quote(media) + ",\"name\":" + pvrjson::quote(name) + "}");
        return id;
    }
    // what: play | pause | seek (position in seconds).
    int control(int screen, const std::string& media, const std::string& what, double position = 0.0) {
        const int id = ++lastId_;
        send("{\"type\":\"control\",\"id\":" + std::to_string(id) + ",\"screen\":" + std::to_string(screen) +
             ",\"media\":" + pvrjson::quote(media) + ",\"do\":" + pvrjson::quote(what) + ",\"position\":" + num(position) + "}");
        return id;
    }
    // Heat strip on the timeline: segments of [startMs, endMs, heat 0..1]; empty clears it.
    void timeline(int screen, const std::string& media, const std::string& label,
                  const std::vector<std::array<double, 3>>& segments) {
        std::string s;
        for (const auto& g : segments) {
            s += (s.empty() ? "[" : ",[") + std::to_string(static_cast<long long>(g[0])) + "," +
                 std::to_string(static_cast<long long>(g[1])) + "," + num(g[2]) + "]";
        }
        send("{\"type\":\"ui.timeline\",\"screen\":" + std::to_string(screen) + ",\"media\":" + pvrjson::quote(media) +
             ",\"tracks\":" + (segments.empty() ? std::string("[]") : "[{\"id\":\"main\",\"label\":" + pvrjson::quote(label) +
             ",\"segments\":[" + s + "]}]") + "}");
    }

private:
    std::string in_;
    std::deque<pvrjson::Value> queue_;
    bool eof_ = false;
    int lastId_ = 0;
};

// What the player reported about one screen, with the media clock extrapolated.
struct MediaClock {
    std::string media;  // "" = nothing open
    bool isPrivate = false;
    std::string fileName, title, source, path;
    std::vector<std::string> sidecars;  // files next to it this plugin may read (files.read)
    int screen = -1;
    double duration = 0.0;
    bool playing = false;
    bool buffering = false;
    bool scrubbing = false;  // the user is scrubbing: hold outputs still
    double position = 0.0;
    double rate = 1.0;
    long long clock = 0;

    bool open() const { return !media.empty(); }
    // Current media time in seconds.
    double now() const {
        if (!playing || buffering) return position;
        return position + static_cast<double>(monoMs() - clock) / 1000.0 * rate;
    }
};

// Tracks screens from the player's events; feed every message to handle().
class Screens {
public:
    // Returns true if `m` was a screen / media event.
    bool handle(const pvrjson::Value& m) {
        const std::string type = m.getString("type");
        if (type == "screens") {
            selected_ = audio_ = -1;
            if (const pvrjson::Value* arr = m.get("screens")) {
                for (const auto& s : arr->array) {
                    const int i = static_cast<int>(s.getNumber("screen", -1));
                    const pvrjson::Value* sel = s.get("selected");
                    const pvrjson::Value* aud = s.get("audio");
                    if (sel && sel->boolean) selected_ = i;
                    if (aud && aud->boolean) audio_ = i;
                }
            }
            return true;
        }
        const int screen = static_cast<int>(m.getNumber("screen", -1));
        if (screen < 0) return false;
        MediaClock& c = clocks_[screen];
        auto names = [](const pvrjson::Value* arr) {
            std::vector<std::string> out;
            if (arr) {
                for (const auto& e : arr->array) {
                    if (e.type == pvrjson::Value::Type::String) out.push_back(e.str);
                }
            }
            return out;
        };
        if (type == "media.sidecars") {
            if (m.getString("media") == c.media) c.sidecars = names(m.get("sidecars"));
            return true;
        }
        if (type == "media.open") {
            c = MediaClock{};
            c.screen = screen;
            c.sidecars = names(m.get("sidecars"));
            c.media = m.getString("media");
            const pvrjson::Value* pv = m.get("private");
            c.isPrivate = pv && pv->boolean;
            c.fileName = m.getString("fileName");
            c.title = m.getString("title");
            c.source = m.getString("source");
            c.path = m.getString("path");
            c.duration = m.getNumber("duration");
            return true;
        }
        if (type == "media.state") {
            if (m.getString("media") != c.media) return true;
            const pvrjson::Value* pl = m.get("playing");
            const pvrjson::Value* bu = m.get("buffering");
            c.playing = pl && pl->boolean;
            c.buffering = bu && bu->boolean;
            const pvrjson::Value* sb = m.get("scrubbing");
            c.scrubbing = sb && sb->boolean;
            c.position = m.getNumber("position");
            c.rate = m.getNumber("rate", 1.0);
            c.clock = static_cast<long long>(m.getNumber("clock", static_cast<double>(monoMs())));
            return true;
        }
        if (type == "media.close") {
            if (m.getString("media") == c.media) c = MediaClock{};
            return true;
        }
        return false;
    }

    const std::map<int, MediaClock>& all() const { return clocks_; }
    int selected() const { return selected_; }
    int audio() const { return audio_; }
    // The screen a plugin should follow: the one heard, else the selected one,
    // else any screen with something open.
    const MediaClock* focus() const {
        for (int want : {audio_, selected_}) {
            const auto it = clocks_.find(want);
            if (it != clocks_.end() && it->second.open()) return &it->second;
        }
        for (const auto& kv : clocks_) {
            if (kv.second.open()) return &kv.second;
        }
        return nullptr;
    }

private:
    std::map<int, MediaClock> clocks_;
    int selected_ = -1;
    int audio_ = -1;
};

}  // namespace pvrplugin

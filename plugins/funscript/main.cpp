// Funscript devices plugin: plays the .funscript next to a video on devices
// connected to Intiface Central (Buttplug), in sync with the player's clock.
// Built blind (no hardware); see docs/concepts/PLUGIN_SYSTEM_SPEC.md §10 and
// tools/funscript_plugin_test.py. The gauge above the dock always shows the
// script position, so sync can be checked without a device.

#include "../sdk/pvr_plugin.hpp"
#include "buttplug.hpp"
#include "mdns.hpp"
#include "funscript.hpp"

#include <cctype>
#include <climits>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace {

const int kOffsets[] = {-300, -250, -200, -150, -100, -50, 0, 50, 100, 150, 200, 250, 300};
const double kBottom[] = {0, 10, 20, 30};
const double kTop[] = {100, 90, 80, 70};
const double kSpeedLimit[] = {0, 500, 400, 300, 200};  // position units (0..100) per second
const double kVibeMax[] = {1.0, 0.75, 0.5, 0.25};

struct Settings {
    std::string server = "ws://127.0.0.1:12345";
    bool useIntiface = true;
    int offsetMs = 0;
    double bottom = 0.0, top = 100.0;
    double speedLimit = 0.0;
    bool vibeByPosition = false;
    double vibeMax = 1.0;
    bool invert = false;
    bool keepOnScrub = false;  // "While scrubbing: Keep running"
    std::vector<std::string> folders;
};

template <typename T, size_t N>
T pick(const T (&table)[N], const pvrjson::Value* s, const char* key, T fallback) {
    const pvrjson::Value* v = s ? s->get(key) : nullptr;
    if (!v || v->type != pvrjson::Value::Type::Number) return fallback;
    const int i = static_cast<int>(v->number);
    return (i >= 0 && i < static_cast<int>(N)) ? table[i] : fallback;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t");
    const size_t b = s.find_last_not_of(" \t");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

void readSettings(const pvrjson::Value* s, Settings& c) {
    if (!s) return;
    if (const pvrjson::Value* v = s->get("server"); v && v->type == pvrjson::Value::Type::String && !trim(v->str).empty()) {
        c.server = trim(v->str);
        if (c.server.find("://") == std::string::npos) c.server = "ws://" + c.server;
    }
    if (const pvrjson::Value* v = s->get("intiface")) c.useIntiface = v->boolean;
    if (const pvrjson::Value* v = s->get("invert")) c.invert = v->boolean;
    if (const pvrjson::Value* v = s->get("scrub"); v && v->type == pvrjson::Value::Type::Number) c.keepOnScrub = v->number >= 1.0;
    c.offsetMs = pick(kOffsets, s, "offset", c.offsetMs);
    c.bottom = pick(kBottom, s, "bottom", c.bottom);
    c.top = pick(kTop, s, "top", c.top);
    c.speedLimit = pick(kSpeedLimit, s, "speed", c.speedLimit);
    c.vibeMax = pick(kVibeMax, s, "vibeMax", c.vibeMax);
    if (const pvrjson::Value* v = s->get("vibe"); v && v->type == pvrjson::Value::Type::Number) c.vibeByPosition = v->number >= 1.0;
    if (const pvrjson::Value* v = s->get("folders"); v && v->type == pvrjson::Value::Type::String) {
        c.folders.clear();
        size_t start = 0;
        for (;;) {
            const size_t semi = v->str.find(';', start);
            const std::string part = trim(v->str.substr(start, semi == std::string::npos ? std::string::npos : semi - start));
            if (!part.empty()) c.folders.push_back(part[0] == '~' && std::getenv("HOME") ? std::getenv("HOME") + part.substr(1) : part);
            if (semi == std::string::npos) break;
            start = semi + 1;
        }
    }
}

std::string baseOf(const std::string& fileName) {
    const size_t dot = fileName.find_last_of('.');
    return dot == std::string::npos || dot == 0 ? fileName : fileName.substr(0, dot);
}

}  // namespace

int main() {
    pvrplugin::Io io;
    pvrplugin::Screens screens;
    Settings cfg;
    buttplug::Client bp;

    // Script for the followed media.
    std::string scriptMedia, scriptName, scriptText, pendingName;
    funscript::Script script;
    int pendingRead = -1;
    bool folderTried = false;
    // Device drive state.
    bool active = false;
    int seenSession = 0;     // the Intiface session the outputs were last sent in
    bool listHandled = false;
    long long holdUntil = 0;  // after a seek while playing: devices held still until then
    double fade = 1.0;        // output level: 0 while scrubbing / held, eases back to 1
    long long fadeAt = 0;
    bool fadedOut = false;
    double lastT = -1.0;
    long long lastLoop = 0, softUntil = 0, nextVibe = 0, lastVibeSent = 0;
    size_t sentFor = SIZE_MAX;
    double lastSentPos = -1.0, lastVibe = -1.0;
    bool focused = true, worn = true;
    long long nextConnect = 0, scanUntil = 0, nextGauge = 0, nextStatus = 0, lastRefusedNotice = -60000;
    bool gaugeShown = false;
    std::string connError, lastStatus;

    auto mapPos = [&](double pos) { return cfg.bottom + (cfg.top - cfg.bottom) * pos / 100.0; };
    auto stopDevices = [&]() {
        if (active) bp.stopAll();
        active = false;
        lastT = -1.0;
        sentFor = SIZE_MAX;
        lastVibe = -1.0;
    };
    auto applyScript = [&](const pvrplugin::MediaClock& c) {
        script = funscript::parse(scriptText);
        if (!script.ok()) {
            io.status("Cannot use " + scriptName + ": " + script.error, "warn");
            scriptName.clear();
            return;
        }
        if (cfg.invert) {
            for (auto& a : script.actions) a.pos = 100.0 - a.pos;
        }
        io.timeline(c.screen, c.media, "Funscript", funscript::heat(script, c.duration * 1000.0));
    };
    auto loadText = [&](const pvrplugin::MediaClock& c, const std::string& name, const std::string& text) {
        scriptName = name;
        scriptText = text;
        applyScript(c);
        if (script.ok()) io.notice("Funscript loaded: " + name);
    };
    auto discover = [&](const pvrplugin::MediaClock* c) {
        if (!c) {
            if (!scriptMedia.empty()) {
                stopDevices();
                scriptMedia.clear();
                script = {};
                scriptName.clear();
            }
            return;
        }
        if (c->media != scriptMedia) {
            stopDevices();
            scriptMedia = c->media;
            script = {};
            scriptName.clear();
            scriptText.clear();
            pendingRead = -1;
            folderTried = false;
        }
        if (script.ok() || pendingRead >= 0 || c->fileName.empty()) return;
        const std::string want = lower(baseOf(c->fileName) + ".funscript");
        for (const std::string& sc : c->sidecars) {
            if (lower(sc) == want) {
                pendingName = sc;
                pendingRead = io.readSidecar(c->screen, c->media, sc);
                return;
            }
        }
        if (folderTried) return;
        folderTried = true;  // script folders on the headset, matched by file name (covers Plex)
        for (const std::string& folder : cfg.folders) {
            std::error_code ec;
            for (const auto& e : std::filesystem::directory_iterator(folder, ec)) {
                if (lower(e.path().filename().string()) != want) continue;
                std::ifstream f(e.path());
                loadText(*c, e.path().filename().string(),
                         std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()));
                return;
            }
        }
    };

    for (;;) {
        std::vector<pollfd> extra;
        if (bp.fd() >= 0) extra.push_back({bp.fd(), POLLIN, 0});
        if (!io.pump(8, &extra)) break;
        pvrjson::Value m;
        while (io.next(m)) {
            const std::string type = m.getString("type");
            if (type == "hello") {
                readSettings(m.get("settings"), cfg);
                io.hello("Funscript devices", "0.1.0");
            } else if (type == "settings") {
                const bool wasInvert = cfg.invert;
                const std::string wasServer = cfg.server;
                readSettings(m.get("settings"), cfg);
                if (cfg.server != wasServer) {
                    stopDevices();
                    bp.disconnect();
                    nextConnect = 0;
                }
                if (cfg.invert != wasInvert && !scriptText.empty()) {
                    if (const pvrplugin::MediaClock* c = screens.focus()) applyScript(*c);
                }
            } else if (type == "action") {
                const std::string a = m.getString("action");
                if (a == "find") {
                    const std::vector<std::string> hosts = mdns::find();
                    if (hosts.empty()) {
                        io.notice("No Intiface found. In Intiface Central turn on Settings > Advanced > "
                                  "Broadcast Server Info via mDNS (and listen on all interfaces), or type the address");
                    } else {
                        const std::string url = "ws://" + hosts.front() + ":12345";
                        io.setSetting("server", url);  // saved; the settings message reconnects
                        io.notice("Found Intiface at " + hosts.front() + (hosts.size() > 1 ? " (" + std::to_string(hosts.size()) + " found, using the first)" : ""));
                    }
                } else if (a == "scan") {
                    if (bp.connected()) {
                        bp.startScanning();
                        scanUntil = pvrplugin::monoMs() + 10000;
                        io.notice("Scanning for devices for 10 s: switch them on / into pairing mode now");
                        nextStatus = 0;  // show "Scanning" at once
                    } else {
                        io.notice("Not connected to Intiface");
                    }
                } else if (a == "stop") {
                    stopDevices();
                    bp.stopAll();
                    io.notice("All devices stopped");
                }
            } else if (type == "app.state") {
                const pvrjson::Value* f = m.get("focused");
                const pvrjson::Value* w = m.get("worn");
                focused = !f || f->boolean;
                worn = !w || w->boolean;
            } else if (type == "result") {
                if (static_cast<int>(m.getNumber("id", -1)) != pendingRead) continue;
                pendingRead = -1;
                const pvrjson::Value* ok = m.get("ok");
                const pvrjson::Value* data = m.get("data");
                const pvrplugin::MediaClock* c = screens.focus();
                if (ok && ok->boolean && data && data->get("text") && c && c->media == scriptMedia) {
                    loadText(*c, pendingName, data->getString("text"));
                } else if (!(ok && ok->boolean)) {
                    io.status("Cannot read " + pendingName + ": " + m.getString("error"), "warn");
                }
            } else if (type == "shutdown") {
                stopDevices();
                bp.stopAll();
                bp.disconnect();
                io.gauge("", -1.0);
                return 0;
            } else {
                screens.handle(m);
            }
        }

        const long long now = pvrplugin::monoMs();
        // Intiface connection.
        if (cfg.useIntiface && !bp.connected() && !bp.connecting() && now >= nextConnect) {
            std::string err;
            connError = bp.connect(cfg.server, err) ? std::string() : err;
            if (connError.find("refused") != std::string::npos) {
                // Intiface takes one app at a time: a second one is refused while another is connected.
                const std::string busy = "refused - Intiface takes one app at a time: is another app (e.g. MultiFunPlayer) connected to it?";
                if (lastRefusedNotice + 60000 < now) {
                    lastRefusedNotice = now;
                    io.notice("Intiface refused the connection. It takes one app at a time: disconnect the other app (e.g. MultiFunPlayer)");
                }
                connError = busy;
            }
            nextConnect = now + 5000;
        }
        if (bp.connected() && bp.session() != seenSession) {
            // (Re)connected: nothing sent before counts, so the next loop starts fresh with a soft start
            // and re-sends the vibration. Without this a reconnect left the devices idle.
            seenSession = bp.session();
            listHandled = false;
            active = false;
            lastVibe = -1.0;
            sentFor = SIZE_MAX;
            lastStatus.clear();
        }
        if (bp.connected() && bp.deviceListSeen() && !listHandled) {
            listHandled = true;
            if (bp.devices().empty() && !scanUntil) {
                // Intiface may drop its devices with the old connection: look for them again.
                bp.startScanning();
                scanUntil = now + 10000;
                io.notice(seenSession > 1 ? "Reconnected to Intiface: scanning for devices" : "Connected to Intiface: scanning for devices");
            } else if (seenSession > 1) {
                io.notice("Reconnected to Intiface: " + std::to_string(bp.devices().size()) + " devices");
            }
        }
        if (!cfg.useIntiface && (bp.connected() || bp.connecting())) {
            stopDevices();
            bp.stopAll();
            bp.disconnect();
        }
        if ((bp.connected() || bp.connecting()) && !bp.poll(now)) {
            connError = "connection lost";
            active = false;
            nextConnect = now + 3000;
        }
        if (scanUntil && now > scanUntil) {
            bp.stopScanning();
            scanUntil = 0;
            const size_t n = bp.devices().size();
            io.notice("Scan finished: " + std::to_string(n) + (n == 1 ? " device" : " devices") + " connected");
            lastStatus.clear();  // the status line drops "Scanning"
        }

        // Follow the screen being heard.
        const pvrplugin::MediaClock* c = screens.focus();
        discover(c);
        const bool fresh = c && now - c->clock < 1500;  // the player ticks every 250 ms while playing
        const bool run = c && script.ok() && c->playing && !c->buffering && fresh && focused && worn;
        if (!run) {
            stopDevices();
        } else {
            const double rate = c->rate > 0.0 ? c->rate : 1.0;
            const double t = c->now() * 1000.0 + cfg.offsetMs * rate;
            const bool jumped = lastT < 0.0 || std::fabs((t - lastT) - static_cast<double>(now - lastLoop) * rate) > 300.0;
            const bool wasRunning = lastT >= 0.0;
            lastT = t;
            lastLoop = now;
            if (jumped && wasRunning) {
                // A seek while playing (scrubbing sends many in a row): hold the devices still until the
                // position settles, then glide back in. Without this every seek restarted the soft start,
                // commands flooded in and everything ran at 100 %.
                holdUntil = now + 400;
            }
            if (c->scrubbing && !cfg.keepOnScrub) holdUntil = std::max(holdUntil, now + 400);
            const bool held = now < holdUntil;
            // Gentle: while held the vibration fades out over ~0.3 s, then the devices stop; afterwards
            // the stroker glides in (soft start) and the vibration ramps up over ~0.6 s.
            const double fdt = fadeAt ? static_cast<double>(now - fadeAt) / 1000.0 : 0.0;
            fadeAt = now;
            fade = held ? std::max(0.0, fade - fdt / 0.3) : std::min(1.0, fade + fdt / 0.6);
            if (held && fade <= 0.0 && !fadedOut) {
                bp.stopAll();
                active = false;
                fadedOut = true;
            }
            if (!held) fadedOut = false;
            if (held && active && fade > 0.0) {
                // Fading out: vibration steps down at the usual 10 Hz; no new strokes.
                if (now >= nextVibe) {
                    nextVibe = now + 100;
                    const double v = lastVibe > 0.0 ? lastVibe * fade : 0.0;
                    for (const auto& kv : bp.devices()) {
                        if (!kv.second.scalars.empty()) bp.scalar(kv.second, v);
                        if (kv.second.rotate > 0) bp.rotate(kv.second, v, true);
                    }
                }
            }
            if (!held && (!active || jumped)) {
                // Soft start: glide to where the script will be in half a second.
                active = true;
                softUntil = now + 500;
                sentFor = SIZE_MAX;
                const double target = mapPos(funscript::positionAt(script, t + 500.0 * rate));
                for (const auto& kv : bp.devices()) {
                    if (kv.second.linear > 0) bp.linear(kv.first, 500, target / 100.0);
                }
                lastSentPos = target;
            }
            if (!held && now >= softUntil) {
                const size_t k = funscript::nextIndex(script, t);
                if (k < script.actions.size() && k != sentFor) {
                    double dur = (script.actions[k].at - t) / rate;
                    const double target = mapPos(script.actions[k].pos);
                    if (cfg.speedLimit > 0.0 && lastSentPos >= 0.0 && dur > 0.0) {
                        dur = std::max(dur, std::fabs(target - lastSentPos) / cfg.speedLimit * 1000.0);
                    }
                    for (const auto& kv : bp.devices()) {
                        if (kv.second.linear > 0) bp.linear(kv.first, static_cast<int>(std::max(20.0, dur)), target / 100.0);
                    }
                    sentFor = k;
                    lastSentPos = target;
                }
            }
            if (!held && now >= nextVibe) {
                nextVibe = now + 100;  // vibrators and rotators: at most 10 updates per second
                double v = cfg.vibeByPosition ? mapPos(funscript::positionAt(script, t)) / 100.0
                                              : std::clamp(funscript::speedBetween(script, t - 250.0, t + 250.0) / 400.0, 0.0, 1.0);
                v *= cfg.vibeMax * fade;
                if (std::fabs(v - lastVibe) > 0.02 || now - lastVibeSent > 1000) {
                    for (const auto& kv : bp.devices()) {
                        if (!kv.second.scalars.empty()) bp.scalar(kv.second, v);
                        if (kv.second.rotate > 0) bp.rotate(kv.second, v, true);
                    }
                    lastVibe = v;
                    lastVibeSent = now;
                }
            }
        }

        (void)nextGauge;  // (the debug "Script" gauge is gone)
        if (gaugeShown) {
            io.gauge("", -1.0);
            gaugeShown = false;
        }
        if (now >= nextStatus) {
            nextStatus = now + 500;
            std::string s = !c ? "Waiting for a video"
                            : script.ok() ? "Script: " + scriptName + " (" + std::to_string(script.actions.size()) + " points)"
                            : pendingRead >= 0 ? "Reading " + pendingName
                            : "No funscript for this video";
            const size_t n = bp.devices().size();
            s += !cfg.useIntiface ? "  |  Intiface off"
                 : bp.connected() ? "  |  Intiface: " + std::to_string(n) + (n == 1 ? " device" : " devices")
                 : "  |  Intiface not connected" + (connError.empty() ? std::string() : " (" + connError + ")");
            if (scanUntil) s += "  |  Scanning for devices (" + std::to_string(std::max(0LL, (scanUntil - now + 999) / 1000)) + " s)";
            if (s != lastStatus) {
                lastStatus = s;
                io.status(s, script.ok() && (!bp.connected() || n == 0) ? "warn" : "ok");
            }
        }
    }
    stopDevices();
    return 0;
}

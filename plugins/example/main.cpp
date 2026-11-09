// Example plugin: shows what a plugin receives and can do. It follows the
// selected screen, shows its progress as a gauge, announces opened files,
// reads a "<video name>.txt" next to the video and can pause the video. With
// "Log every event" on, it writes each message it gets to
// $PIXELVR_PLUGIN_DATA/events.log (used by tools/plugin_host_test).

#include "../sdk/pvr_plugin.hpp"

#include <cstdlib>
#include <fstream>

int main() {
    pvrplugin::Io io;
    pvrplugin::Screens screens;
    const char* dataDir = std::getenv("PIXELVR_PLUGIN_DATA");
    std::ofstream events;
    bool verbose = false;
    int gaugeMode = 0;  // 0 progress, 1 seconds (mod 60)
    long long nextGauge = 0;
    std::string readFor;  // media whose sidecar was requested

    auto applySettings = [&](const pvrjson::Value* s) {
        if (!s) return;
        if (const pvrjson::Value* v = s->get("verbose")) verbose = v->boolean;
        gaugeMode = static_cast<int>(s->getNumber("gauge", gaugeMode));
        if (verbose && !events.is_open() && dataDir) events.open(std::string(dataDir) + "/events.log", std::ios::app);
    };
    auto readFirstSidecar = [&]() {
        const pvrplugin::MediaClock* c = screens.focus();
        if (c && !c->sidecars.empty() && readFor != c->media) {
            readFor = c->media;
            io.readSidecar(c->screen, c->media, c->sidecars.front());
        }
    };

    while (io.pump(50)) {
        pvrjson::Value m;
        while (io.next(m)) {
            const std::string type = m.getString("type");
            if (verbose && events.is_open()) events << pvrjson::dump(m) << '\n' << std::flush;
            if (type == "hello") {
                applySettings(m.get("settings"));  // may open the event log: log the hello itself too
                if (verbose && events.is_open()) events << pvrjson::dump(m) << '\n' << std::flush;
                io.hello("Example plugin", "0.2.0");
                io.status("Waiting for a video");
            } else if (type == "settings") {
                applySettings(m.get("settings"));
            } else if (type == "action") {
                const pvrplugin::MediaClock* c = screens.focus();
                if (m.getString("action") == "hello") io.notice("Hello from the example plugin");
                if (m.getString("action") == "pause" && c) io.control(c->screen, c->media, "pause");
            } else if (type == "result") {
                const pvrjson::Value* ok = m.get("ok");
                const pvrjson::Value* data = m.get("data");
                if (ok && ok->boolean && data && data->get("text")) {
                    io.status("Read " + data->getString("name") + " (" + std::to_string(data->getString("text").size()) + " bytes)");
                }
            } else if (type == "shutdown") {
                io.gauge("", -1.0);
                return 0;
            } else if (screens.handle(m)) {
                if (type == "media.open") {
                    const pvrjson::Value* pv = m.get("private");
                    if (pv && pv->boolean) {
                        io.status("A private video is playing");
                    } else {
                        io.status("Watching " + m.getString("fileName"));
                        io.notice("Opened " + m.getString("fileName"));
                        // A demo heat strip: calm first half, intense second half.
                        const double ms = m.getNumber("duration") * 1000.0;
                        if (ms > 0) {
                            io.timeline(static_cast<int>(m.getNumber("screen")), m.getString("media"), "Example",
                                        {{0.0, ms / 2, 0.2}, {ms / 2, ms, 0.9}});
                        }
                    }
                } else if (type == "media.close" && !screens.focus()) {
                    io.status("Waiting for a video");
                }
                readFirstSidecar();
            }
        }
        if (pvrplugin::monoMs() >= nextGauge) {
            nextGauge = pvrplugin::monoMs() + 100;
            const pvrplugin::MediaClock* c = screens.focus();
            if (!c || c->isPrivate) {
                io.gauge("", -1.0);
            } else if (gaugeMode == 1) {
                const double t = c->now();
                io.gauge("Seconds", (t - 60.0 * static_cast<long long>(t / 60.0)) / 60.0);
            } else {
                io.gauge("Progress", c->duration > 0 ? c->now() / c->duration : 0.0);
            }
        }
    }
    return 0;
}

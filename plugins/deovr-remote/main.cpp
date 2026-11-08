// DeoVR-compatible remote-control bridge: serves DeoVR's documented protocol
// (https://deovr.com/documentation, section 8) on TCP 23554, so sync tools
// that follow DeoVR (MultiFunPlayer, FunFlixDroid, ScriptPlayer, XBVR) can
// follow Pixel VR Player. Packets: 4-byte little-endian length + UTF-8 JSON;
// an empty packet is a ping. We report the screen being heard once a second
// and on every change, and accept play / pause / seek from the client.

#include "../sdk/pvr_plugin.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace {

struct Client {
    int fd = -1;
    std::string in;
    long long lastHeard = 0;
};

std::string packet(const std::string& json) {
    const uint32_t n = static_cast<uint32_t>(json.size());
    std::string p(4, '\0');
    for (int i = 0; i < 4; ++i) p[static_cast<size_t>(i)] = static_cast<char>((n >> (8 * i)) & 0xFF);
    return p + json;
}

bool sendAll(int fd, const std::string& data) {
    size_t off = 0;
    while (off < data.size()) {
        const ssize_t n = ::send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n > 0) {
            off += static_cast<size_t>(n);
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            return false;  // gone, or not reading (a client that cannot keep up is dropped)
        }
    }
    return true;
}

std::string lanAddresses() {
    std::string out;
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return out;
    for (ifaddrs* a = list; a; a = a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET || (a->ifa_flags & IFF_LOOPBACK)) continue;
        char ip[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(a->ifa_addr)->sin_addr, ip, sizeof(ip));
        out += (out.empty() ? "" : ", ") + std::string(ip);
    }
    freeifaddrs(list);
    return out;
}

}  // namespace

int main() {
    pvrplugin::Io io;
    pvrplugin::Screens screens;
    int port = 23554;
    bool acceptControl = true;
    int listener = -1;
    std::string listenError;
    std::vector<Client> clients;
    long long nextReport = 0, nextStatus = 0;
    std::string lastSig, lastStatus;

    auto openListener = [&]() {
        if (listener >= 0) ::close(listener);
        listener = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        int one = 1;
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(port));
        addr.sin_addr.s_addr = htonl(INADDR_ANY);  // sync tools run on another PC or phone
        if (::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(listener, 4) != 0) {
            listenError = std::string("port ") + std::to_string(port) + " is busy: " + std::strerror(errno);
            ::close(listener);
            listener = -1;
        } else {
            listenError.clear();
        }
    };
    auto report = [&]() -> std::string {
        const pvrplugin::MediaClock* c = screens.focus();
        if (!c || c->fileName.empty()) return std::string();  // nothing, or a private (vault) video: ping only
        return "{\"path\":" + pvrjson::quote(c->fileName) + ",\"duration\":" + pvrplugin::num(c->duration) +
               ",\"currentTime\":" + pvrplugin::num(c->now()) + ",\"playbackSpeed\":" + pvrplugin::num(c->rate) +
               ",\"playerState\":" + (c->playing ? "0" : "1") + "}";
    };
    auto broadcast = [&](const std::string& json) {
        const std::string p = packet(json);
        for (Client& cl : clients) {
            if (cl.fd >= 0 && !sendAll(cl.fd, p)) {
                ::close(cl.fd);
                cl.fd = -1;
            }
        }
    };
    auto handlePacket = [&](const std::string& json) {
        if (json.empty() || !acceptControl) return;  // ping
        pvrjson::Value v;
        const pvrplugin::MediaClock* c = screens.focus();
        if (!pvrjson::parse(json, v) || !c || c->fileName.empty()) return;
        if (const pvrjson::Value* t = v.get("currentTime"); t && t->type == pvrjson::Value::Type::Number &&
                                                           std::fabs(t->number - c->now()) > 0.5) {
            io.control(c->screen, c->media, "seek", std::max(0.0, t->number));
        }
        if (const pvrjson::Value* s = v.get("playerState"); s && s->type == pvrjson::Value::Type::Number) {
            const bool play = s->number < 0.5;
            if (play != c->playing) io.control(c->screen, c->media, play ? "play" : "pause");
        }
    };

    for (;;) {
        std::vector<pollfd> extra;
        if (listener >= 0) extra.push_back({listener, POLLIN, 0});
        for (const Client& cl : clients) extra.push_back({cl.fd, POLLIN, 0});
        if (!io.pump(20, &extra)) break;
        pvrjson::Value m;
        while (io.next(m)) {
            const std::string type = m.getString("type");
            if (type == "hello" || type == "settings") {
                const pvrjson::Value* s = m.get("settings");
                const int oldPort = port;
                if (s) {
                    if (const pvrjson::Value* v = s->get("control")) acceptControl = v->boolean;
                    static const int kPorts[] = {23554, 23555, 23556};
                    const int i = static_cast<int>(s->getNumber("port", 0));
                    port = kPorts[i >= 0 && i < 3 ? i : 0];
                }
                if (type == "hello") io.hello("DeoVR-compatible sync", "0.1.0");
                if (type == "hello" || port != oldPort) openListener();
            } else if (type == "shutdown") {
                for (Client& cl : clients) ::close(cl.fd);
                if (listener >= 0) ::close(listener);
                return 0;
            } else if (type == "result" || type == "app.state" || type == "action") {
                // nothing to do
            } else if (screens.handle(m) && (type == "media.state" || type == "media.open" || type == "media.close")) {
                lastSig.clear();  // report changes at once, not only on the 1 s tick
            }
        }
        const long long now = pvrplugin::monoMs();
        if (listener >= 0) {
            for (;;) {
                const int fd = ::accept4(listener, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
                if (fd < 0) break;
                clients.push_back({fd, std::string(), now});
                lastSig.clear();
            }
        }
        for (Client& cl : clients) {
            char buf[8192];
            for (;;) {
                const ssize_t n = ::recv(cl.fd, buf, sizeof(buf), MSG_DONTWAIT);
                if (n > 0) {
                    cl.in.append(buf, static_cast<size_t>(n));
                    cl.lastHeard = now;
                    continue;
                }
                if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                    ::close(cl.fd);
                    cl.fd = -1;
                }
                break;
            }
            while (cl.fd >= 0 && cl.in.size() >= 4) {
                uint32_t len = 0;
                for (int i = 0; i < 4; ++i) len |= static_cast<uint32_t>(static_cast<unsigned char>(cl.in[static_cast<size_t>(i)])) << (8 * i);
                if (len > (1u << 20)) {
                    ::close(cl.fd);
                    cl.fd = -1;
                    break;
                }
                if (cl.in.size() < 4 + len) break;
                handlePacket(cl.in.substr(4, len));
                cl.in.erase(0, 4 + len);
            }
            if (cl.fd >= 0 && now - cl.lastHeard > 10000) {  // DeoVR itself gives up after 3 s
                ::close(cl.fd);
                cl.fd = -1;
            }
        }
        clients.erase(std::remove_if(clients.begin(), clients.end(), [](const Client& c) { return c.fd < 0; }), clients.end());

        const std::string json = report();
        const pvrplugin::MediaClock* c = screens.focus();
        const std::string sig = c ? c->media + (c->playing ? "P" : "-") + pvrplugin::num(c->position) : std::string("none");
        if (!clients.empty() && (now >= nextReport || sig != lastSig)) {
            broadcast(json);
            nextReport = now + 1000;
            lastSig = sig;
        }
        if (now >= nextStatus) {
            nextStatus = now + 1000;
            const std::string s = !listenError.empty()
                                      ? "Cannot listen: " + listenError
                                      : "Sync apps connect to " + lanAddresses() + " port " + std::to_string(port) + "  |  " +
                                            std::to_string(clients.size()) + (clients.size() == 1 ? " app connected" : " apps connected");
            if (s != lastStatus) {
                lastStatus = s;
                io.status(s, listenError.empty() ? "ok" : "error");
            }
        }
    }
    return 0;
}

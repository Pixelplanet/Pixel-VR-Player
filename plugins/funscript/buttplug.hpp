#pragma once
// Minimal Buttplug client (message spec v3, which Intiface Central still
// accepts) over a plain ws:// WebSocket. Built and tested blind against
// tools/fake_intiface.py; no hardware was available.
//   [{"RequestServerInfo":{"Id":1,"ClientName":"...","MessageVersion":3}}]
//   [{"LinearCmd":{"Id":n,"DeviceIndex":d,"Vectors":[{"Index":0,"Duration":ms,"Position":0..1}]}}]
//   [{"ScalarCmd":{"Id":n,"DeviceIndex":d,"Scalars":[{"Index":i,"Scalar":0..1,"ActuatorType":"Vibrate"}]}}]
//   [{"RotateCmd":{"Id":n,"DeviceIndex":d,"Rotations":[{"Index":i,"Speed":0..1,"Clockwise":true}]}}]
//   [{"StopAllDevices":{"Id":n}}]

#include "../../browser/mini_json.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <random>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace buttplug {

class WebSocket {
public:
    ~WebSocket() { close(); }
    int fd() const { return fd_; }
    bool open() const { return fd_ >= 0; }

    // ws://host:port[/path]; blocks for at most `timeoutMs`.
    bool connect(const std::string& url, int timeoutMs, std::string& err) {
        close();
        std::string rest = url;
        if (rest.rfind("ws://", 0) == 0) rest = rest.substr(5);
        else if (rest.find("://") != std::string::npos) {
            err = "only ws:// addresses are supported";
            return false;
        }
        const size_t slash = rest.find('/');
        const std::string path = slash == std::string::npos ? "/" : rest.substr(slash);
        std::string hostPort = rest.substr(0, slash);
        std::string host = hostPort, port = "12345";
        const size_t colon = hostPort.rfind(':');
        if (colon != std::string::npos && hostPort.find(']') == std::string::npos) {
            host = hostPort.substr(0, colon);
            port = hostPort.substr(colon + 1);
        }
        if (!host.empty() && host.front() == '[' && host.back() == ']') host = host.substr(1, host.size() - 2);
        addrinfo hints{};
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res) {
            err = "cannot resolve " + host;
            return false;
        }
        int fd = -1;
        for (addrinfo* a = res; a && fd < 0; a = a->ai_next) {
            fd = ::socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK, a->ai_protocol);
            if (fd < 0) continue;
            if (::connect(fd, a->ai_addr, a->ai_addrlen) != 0 && errno != EINPROGRESS) {
                ::close(fd);
                fd = -1;
                continue;
            }
            pollfd p{fd, POLLOUT, 0};
            int soErr = 0;
            socklen_t len = sizeof(soErr);
            if (::poll(&p, 1, timeoutMs) != 1 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &soErr, &len) != 0 || soErr != 0) {
                ::close(fd);
                fd = -1;
            }
        }
        freeaddrinfo(res);
        if (fd < 0) {
            err = "nothing is listening at " + hostPort;
            return false;
        }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        fd_ = fd;
        std::random_device rd;
        unsigned char key[16];
        for (unsigned char& k : key) k = static_cast<unsigned char>(rd());
        const std::string req = "GET " + path + " HTTP/1.1\r\nHost: " + hostPort +
                                "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + b64(key, 16) +
                                "\r\nSec-WebSocket-Version: 13\r\n\r\n";
        if (!writeAll(req.data(), req.size(), timeoutMs)) {
            err = "handshake failed";
            close();
            return false;
        }
        // Read the HTTP answer up to the blank line; anything after it is frame data.
        std::string head;
        for (;;) {
            pollfd p{fd_, POLLIN, 0};
            if (::poll(&p, 1, timeoutMs) != 1) break;
            char buf[4096];
            const ssize_t n = ::read(fd_, buf, sizeof(buf));
            if (n <= 0) break;
            head.append(buf, static_cast<size_t>(n));
            const size_t end = head.find("\r\n\r\n");
            if (end != std::string::npos) {
                in_ = head.substr(end + 4);
                head.resize(end);
                break;
            }
            if (head.size() > 16384) break;
        }
        if (head.rfind("HTTP/1.1 101", 0) != 0) {
            err = "not a WebSocket server";
            close();
            return false;
        }
        return true;
    }

    void close() {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
        in_.clear();
        partial_.clear();
    }

    bool sendText(const std::string& s) { return sendFrame(0x1, s); }

    // Reads what is available; complete text messages go to `out`. False = disconnected.
    bool poll(std::vector<std::string>& out) {
        if (fd_ < 0) return false;
        for (;;) {
            char buf[65536];
            const ssize_t n = ::read(fd_, buf, sizeof(buf));
            if (n > 0) {
                in_.append(buf, static_cast<size_t>(n));
                continue;
            }
            if (n == 0) {
                close();
                return false;
            }
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            close();
            return false;
        }
        for (;;) {
            if (in_.size() < 2) break;
            const unsigned char b0 = static_cast<unsigned char>(in_[0]);
            const unsigned char b1 = static_cast<unsigned char>(in_[1]);
            uint64_t len = b1 & 0x7F;
            size_t off = 2;
            if (len == 126) {
                if (in_.size() < 4) break;
                len = (static_cast<uint64_t>(static_cast<unsigned char>(in_[2])) << 8) | static_cast<unsigned char>(in_[3]);
                off = 4;
            } else if (len == 127) {
                if (in_.size() < 10) break;
                len = 0;
                for (int i = 0; i < 8; ++i) len = (len << 8) | static_cast<unsigned char>(in_[2 + i]);
                off = 10;
            }
            const bool masked = b1 & 0x80;
            unsigned char mask[4] = {0, 0, 0, 0};
            if (masked) {
                if (in_.size() < off + 4) break;
                std::memcpy(mask, in_.data() + off, 4);
                off += 4;
            }
            if (len > (16u << 20)) {
                close();
                return false;
            }
            if (in_.size() < off + len) break;
            std::string payload = in_.substr(off, static_cast<size_t>(len));
            in_.erase(0, off + static_cast<size_t>(len));
            if (masked) {
                for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<char>(payload[i] ^ mask[i & 3]);
            }
            const int op = b0 & 0x0F;
            const bool fin = b0 & 0x80;
            if (op == 0x8) {
                close();
                return false;
            }
            if (op == 0x9) {
                sendFrame(0xA, payload);
                continue;
            }
            if (op == 0x1 || op == 0x0) {
                partial_ += payload;
                if (fin) {
                    out.push_back(std::move(partial_));
                    partial_.clear();
                }
            }
        }
        return fd_ >= 0;
    }

private:
    static std::string b64(const unsigned char* d, size_t n) {
        static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string o;
        for (size_t i = 0; i < n; i += 3) {
            const uint32_t v = (d[i] << 16) | (i + 1 < n ? d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
            o += t[(v >> 18) & 63];
            o += t[(v >> 12) & 63];
            o += i + 1 < n ? t[(v >> 6) & 63] : '=';
            o += i + 2 < n ? t[v & 63] : '=';
        }
        return o;
    }
    bool writeAll(const char* p, size_t n, int timeoutMs) {
        while (n > 0 && fd_ >= 0) {
            const ssize_t w = ::send(fd_, p, n, MSG_NOSIGNAL);
            if (w > 0) {
                p += w;
                n -= static_cast<size_t>(w);
                continue;
            }
            if (w < 0 && errno == EINTR) continue;
            if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                pollfd pf{fd_, POLLOUT, 0};
                if (::poll(&pf, 1, timeoutMs) == 1) continue;
            }
            return false;
        }
        return n == 0;
    }
    bool sendFrame(int op, const std::string& s) {
        if (fd_ < 0) return false;
        std::string f;
        f += static_cast<char>(0x80 | op);
        const size_t n = s.size();
        if (n < 126) {
            f += static_cast<char>(0x80 | n);
        } else if (n < 65536) {
            f += static_cast<char>(0x80 | 126);
            f += static_cast<char>((n >> 8) & 0xFF);
            f += static_cast<char>(n & 0xFF);
        } else {
            f += static_cast<char>(0x80 | 127);
            for (int i = 7; i >= 0; --i) f += static_cast<char>((static_cast<uint64_t>(n) >> (8 * i)) & 0xFF);
        }
        unsigned char mask[4];
        for (unsigned char& m : mask) m = static_cast<unsigned char>(rng_());
        f.append(reinterpret_cast<const char*>(mask), 4);
        for (size_t i = 0; i < n; ++i) f += static_cast<char>(s[i] ^ mask[i & 3]);
        if (!writeAll(f.data(), f.size(), 500)) {
            close();
            return false;
        }
        return true;
    }

    int fd_ = -1;
    std::string in_;
    std::string partial_;
    std::minstd_rand rng_{std::random_device{}()};
};

struct Device {
    int index = 0;
    std::string name;
    int linear = 0;                       // LinearCmd features
    std::vector<std::string> scalars;     // ScalarCmd features (actuator types)
    int rotate = 0;                       // RotateCmd features
};

class Client {
public:
    bool connected() const { return ws_.open() && ready_; }
    bool connecting() const { return ws_.open() && !ready_; }
    int session() const { return session_; }             // +1 per handshake (reconnects included)
    bool deviceListSeen() const { return listSeen_; }   // the server answered RequestDeviceList
    int fd() const { return ws_.fd(); }
    const std::map<int, Device>& devices() const { return devices_; }
    const std::string& serverName() const { return serverName_; }
    const std::string& lastError() const { return lastError_; }

    bool connect(const std::string& url, std::string& err) {
        disconnect();
        if (!ws_.connect(url, 2000, err)) return false;
        send("RequestServerInfo", "\"ClientName\":\"Pixel VR Player\",\"MessageVersion\":3");
        return true;
    }
    void disconnect() {
        ws_.close();
        ready_ = false;
        devices_.clear();
    }

    // Call when the socket is readable (or periodically). False = connection lost.
    bool poll(long long nowMs) {
        std::vector<std::string> msgs;
        const bool alive = ws_.poll(msgs);
        for (const std::string& m : msgs) handle(m);
        if (!alive) {
            ready_ = false;
            devices_.clear();
            return false;
        }
        if (ready_ && maxPingMs_ > 0 && nowMs - lastPing_ > maxPingMs_ / 2) {
            lastPing_ = nowMs;
            send("Ping", "");
        }
        return true;
    }

    void startScanning() { send("StartScanning", ""); }
    void stopScanning() { send("StopScanning", ""); }
    void stopAll() {
        if (connected()) send("StopAllDevices", "");
    }
    void linear(int dev, int durationMs, double position01) {
        send("LinearCmd", "\"DeviceIndex\":" + std::to_string(dev) + ",\"Vectors\":[{\"Index\":0,\"Duration\":" +
                              std::to_string(std::max(1, durationMs)) + ",\"Position\":" + num(position01) + "}]");
    }
    void scalar(const Device& d, double value01) {
        std::string list;
        for (size_t i = 0; i < d.scalars.size(); ++i) {
            list += (i ? "," : "") + std::string("{\"Index\":") + std::to_string(i) + ",\"Scalar\":" + num(value01) +
                    ",\"ActuatorType\":" + pvrjson::quote(d.scalars[i]) + "}";
        }
        if (!list.empty()) send("ScalarCmd", "\"DeviceIndex\":" + std::to_string(d.index) + ",\"Scalars\":[" + list + "]");
    }
    void rotate(const Device& d, double speed01, bool clockwise) {
        std::string list;
        for (int i = 0; i < d.rotate; ++i) {
            list += (i ? "," : "") + std::string("{\"Index\":") + std::to_string(i) + ",\"Speed\":" + num(speed01) +
                    ",\"Clockwise\":" + (clockwise ? "true" : "false") + "}";
        }
        if (!list.empty()) send("RotateCmd", "\"DeviceIndex\":" + std::to_string(d.index) + ",\"Rotations\":[" + list + "]");
    }

private:
    static std::string num(double v) {
        char b[32];
        std::snprintf(b, sizeof(b), "%.3f", std::clamp(v, 0.0, 1.0));
        return b;
    }
    void send(const std::string& type, const std::string& fields) {
        if (!ws_.open()) return;
        ws_.sendText("[{\"" + type + "\":{\"Id\":" + std::to_string(nextId_++) + (fields.empty() ? "" : "," + fields) + "}}]");
    }
    void addDevice(const pvrjson::Value& d) {
        Device dev;
        dev.index = static_cast<int>(d.getNumber("DeviceIndex"));
        dev.name = d.getString("DeviceDisplayName").empty() ? d.getString("DeviceName") : d.getString("DeviceDisplayName");
        if (const pvrjson::Value* msgs = d.get("DeviceMessages")) {
            if (const pvrjson::Value* l = msgs->get("LinearCmd")) dev.linear = static_cast<int>(l->array.size());
            if (const pvrjson::Value* r = msgs->get("RotateCmd")) dev.rotate = static_cast<int>(r->array.size());
            if (const pvrjson::Value* s = msgs->get("ScalarCmd")) {
                for (const auto& f : s->array) dev.scalars.push_back(f.getString("ActuatorType").empty() ? "Vibrate" : f.getString("ActuatorType"));
            }
        }
        devices_[dev.index] = dev;
    }
    void handle(const std::string& text) {
        pvrjson::Value root;
        if (!pvrjson::parse(text, root) || root.type != pvrjson::Value::Type::Array) return;
        for (const auto& entry : root.array) {
            for (const auto& kv : entry.object) {
                const std::string& type = kv.first;
                const pvrjson::Value& m = kv.second;
                if (type == "ServerInfo") {
                    ready_ = true;
                    ++session_;
                    listSeen_ = false;
                    serverName_ = m.getString("ServerName");
                    maxPingMs_ = static_cast<long long>(m.getNumber("MaxPingTime"));
                    send("RequestDeviceList", "");
                } else if (type == "DeviceList") {
                    devices_.clear();
                    listSeen_ = true;
                    if (const pvrjson::Value* list = m.get("Devices")) {
                        for (const auto& d : list->array) addDevice(d);
                    }
                } else if (type == "DeviceAdded") {
                    addDevice(m);
                } else if (type == "DeviceRemoved") {
                    devices_.erase(static_cast<int>(m.getNumber("DeviceIndex", -1)));
                } else if (type == "Error") {
                    lastError_ = m.getString("ErrorMessage");
                }
            }
        }
    }

    WebSocket ws_;
    bool ready_ = false;
    int session_ = 0;
    bool listSeen_ = false;
    int nextId_ = 1;
    long long maxPingMs_ = 0;
    long long lastPing_ = 0;
    std::string serverName_, lastError_;
    std::map<int, Device> devices_;
};

}  // namespace buttplug

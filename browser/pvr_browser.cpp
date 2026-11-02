// pvr-browser: drives the user's installed Chromium for PixelVR's browser screen.
//
// Starts Chromium hidden (headless, the Flathub org.chromium.Chromium or a
// `chromium` on PATH) with a persistent profile in ~/.config/pixelvr/chromium,
// and remote-controls one page over the Chrome DevTools Protocol:
//   - Page.startScreencast frames (JPEG) are decoded into shared memory
//     (--shm NAME, created by the app): BrowserShmHeader + RGBA pixels,
//     published with a sequence counter (odd while writing),
//   - stdin, one command per line (coordinates in page pixels):
//       move X Y | down X Y | up X Y | wheel X Y DY | key NAME | text UTF8
//       nav URL | back | forward | reload | quit
//   - stdout, one event per line: ready | url URL | title TEXT | loading 0|1
//     | error TEXT
#include "browser_protocol.hpp"
#include "mini_json.hpp"

#include <turbojpeg.h>

#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <random>
#include <spawn.h>
#include <string>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace {

using Clock = std::chrono::steady_clock;

void emitLine(const std::string& line) {
    const std::string l = line + "\n";
    std::fwrite(l.data(), 1, l.size(), stdout);
    std::fflush(stdout);
}

// ---------------------------------------------------------------- base64 --
std::vector<uint8_t> base64Decode(std::string_view in) {
    static int8_t table[256];
    static bool init = false;
    if (!init) {
        std::memset(table, -1, sizeof(table));
        const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int k = 0; k < 64; ++k) table[static_cast<uint8_t>(chars[k])] = static_cast<int8_t>(k);
        init = true;
    }
    std::vector<uint8_t> out;
    out.reserve(in.size() * 3 / 4);
    uint32_t acc = 0;
    int bits = 0;
    for (const char ch : in) {
        const int8_t v = table[static_cast<uint8_t>(ch)];
        if (v < 0) continue;  // padding / whitespace
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((acc >> bits) & 0xFF));
        }
    }
    return out;
}

std::string base64Encode(const uint8_t* data, size_t n) {
    const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t k = 0; k < n; k += 3) {
        const uint32_t v = (static_cast<uint32_t>(data[k]) << 16) |
                           (k + 1 < n ? static_cast<uint32_t>(data[k + 1]) << 8 : 0) |
                           (k + 2 < n ? static_cast<uint32_t>(data[k + 2]) : 0);
        out += chars[(v >> 18) & 63];
        out += chars[(v >> 12) & 63];
        out += k + 1 < n ? chars[(v >> 6) & 63] : '=';
        out += k + 2 < n ? chars[v & 63] : '=';
    }
    return out;
}

// --------------------------------------------------------------- network --
int tcpConnect(int port) {
    const int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    const int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return fd;
}

bool sendAll(int fd, const void* data, size_t n) {
    const auto* p = static_cast<const uint8_t*>(data);
    while (n > 0) {
        const ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) {
                pollfd pfd{fd, POLLOUT, 0};
                poll(&pfd, 1, 1000);
                continue;
            }
            return false;
        }
        p += w;
        n -= static_cast<size_t>(w);
    }
    return true;
}

std::string httpGet(int port, const std::string& path) {
    const int fd = tcpConnect(port);
    if (fd < 0) return {};
    const std::string req = "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
    sendAll(fd, req.data(), req.size());
    // Chrome's DevTools server keeps the connection open, so read exactly
    // Content-Length bytes (with a timeout as a safety net).
    timeval tv{3, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    std::string resp;
    size_t need = std::string::npos;
    size_t bodyStart = std::string::npos;
    char buf[8192];
    while (need == std::string::npos || resp.size() < bodyStart + need) {
        const ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        resp.append(buf, static_cast<size_t>(n));
        if (bodyStart == std::string::npos) {
            const size_t end = resp.find("\r\n\r\n");
            if (end == std::string::npos) continue;
            bodyStart = end + 4;
            std::string head = resp.substr(0, end);
            for (char& c : head) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            const size_t cl = head.find("content-length:");
            if (cl == std::string::npos) break;
            need = static_cast<size_t>(std::strtoul(head.c_str() + cl + 15, nullptr, 10));
        }
    }
    close(fd);
    return bodyStart == std::string::npos ? std::string() : resp.substr(bodyStart);
}

// Minimal WebSocket client (RFC 6455): masked text frames out, reassembled
// text messages in, ping/pong.
class WebSocket {
public:
    ~WebSocket() { closeSocket(); }
    int fd() const { return fd_; }

    bool connectTo(int port, const std::string& path) {
        fd_ = tcpConnect(port);
        if (fd_ < 0) return false;
        uint8_t key[16];
        std::random_device rd;
        for (uint8_t& b : key) b = static_cast<uint8_t>(rd());
        const std::string req = "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\n"
                                "Connection: Upgrade\r\nSec-WebSocket-Key: " + base64Encode(key, 16) +
                                "\r\nSec-WebSocket-Version: 13\r\n\r\n";
        if (!sendAll(fd_, req.data(), req.size())) return false;
        std::string resp;
        char c;
        while (resp.find("\r\n\r\n") == std::string::npos) {
            const ssize_t n = recv(fd_, &c, 1, 0);
            if (n <= 0) return false;
            resp += c;
        }
        if (resp.find(" 101 ") == std::string::npos) return false;
        fcntl(fd_, F_SETFL, fcntl(fd_, F_GETFL) | O_NONBLOCK);
        return true;
    }

    bool sendText(const std::string& msg) { return sendFrame(0x1, msg.data(), msg.size()); }

    // Reads what is available; appends complete messages. False = closed.
    bool receive(std::vector<std::string>& out) {
        char buf[65536];
        for (;;) {
            const ssize_t n = recv(fd_, buf, sizeof(buf), 0);
            if (n > 0) {
                rx_.append(buf, static_cast<size_t>(n));
                continue;
            }
            if (n == 0) return false;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            return false;
        }
        size_t pos = 0;
        while (rx_.size() - pos >= 2) {
            const auto* p = reinterpret_cast<const uint8_t*>(rx_.data() + pos);
            const bool fin = (p[0] & 0x80) != 0;
            const int opcode = p[0] & 0x0F;
            const bool masked = (p[1] & 0x80) != 0;
            uint64_t len = p[1] & 0x7F;
            size_t hdr = 2;
            if (len == 126) {
                if (rx_.size() - pos < 4) break;
                len = (static_cast<uint64_t>(p[2]) << 8) | p[3];
                hdr = 4;
            } else if (len == 127) {
                if (rx_.size() - pos < 10) break;
                len = 0;
                for (int k = 0; k < 8; ++k) len = (len << 8) | p[2 + k];
                hdr = 10;
            }
            if (masked) hdr += 4;
            if (rx_.size() - pos < hdr + len) break;
            std::string payload = rx_.substr(pos + hdr, static_cast<size_t>(len));
            if (masked) {
                const uint8_t* mask = p + hdr - 4;
                for (size_t k = 0; k < payload.size(); ++k) payload[k] = static_cast<char>(payload[k] ^ mask[k & 3]);
            }
            pos += hdr + static_cast<size_t>(len);
            if (opcode == 0x8) return false;  // close
            if (opcode == 0x9) {               // ping
                sendFrame(0xA, payload.data(), payload.size());
                continue;
            }
            if (opcode == 0xA) continue;  // pong
            if (opcode == 0x1 || opcode == 0x2) fragment_ = std::move(payload);
            else if (opcode == 0x0) fragment_ += payload;
            if (fin) {
                out.push_back(std::move(fragment_));
                fragment_.clear();
            }
        }
        rx_.erase(0, pos);
        return true;
    }

private:
    bool sendFrame(int opcode, const char* data, size_t n) {
        std::string f;
        f += static_cast<char>(0x80 | opcode);
        if (n < 126) {
            f += static_cast<char>(0x80 | n);
        } else if (n < 65536) {
            f += static_cast<char>(0x80 | 126);
            f += static_cast<char>((n >> 8) & 0xFF);
            f += static_cast<char>(n & 0xFF);
        } else {
            f += static_cast<char>(0x80 | 127);
            for (int k = 7; k >= 0; --k) f += static_cast<char>((static_cast<uint64_t>(n) >> (8 * k)) & 0xFF);
        }
        uint8_t mask[4];
        for (uint8_t& b : mask) b = static_cast<uint8_t>(rng_());
        f.append(reinterpret_cast<const char*>(mask), 4);
        const size_t start = f.size();
        f.append(data, n);
        for (size_t k = 0; k < n; ++k) f[start + k] = static_cast<char>(f[start + k] ^ mask[k & 3]);
        return sendAll(fd_, f.data(), f.size());
    }
    void closeSocket() {
        if (fd_ >= 0) close(fd_);
        fd_ = -1;
    }

    int fd_ = -1;
    std::string rx_;
    std::string fragment_;
    std::minstd_rand rng_{std::random_device{}()};
};

// --------------------------------------------------------------- Chromium --
bool commandSucceeds(const std::vector<std::string>& args) {
    std::vector<char*> argv;
    std::vector<std::string> store = args;
    for (std::string& a : store) argv.push_back(a.data());
    argv.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    pid_t pid = -1;
    const int rc = posix_spawnp(&pid, store[0].c_str(), &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) return false;
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// Launches Chromium in its own process group; returns its pid or -1.
pid_t launchChromium(const std::string& profile, int width, int height) {
    std::vector<std::string> args;
    if (commandSucceeds({"flatpak", "info", "org.chromium.Chromium"})) {
        args = {"flatpak", "run", "--command=chromium", "org.chromium.Chromium"};
    } else if (commandSucceeds({"sh", "-c", "command -v chromium"})) {
        args = {"chromium"};
    } else {
        return -1;
    }
    const std::vector<std::string> flags = {
        "--headless=new",
        "--remote-debugging-port=0",
        "--user-data-dir=" + profile,
        "--window-size=" + std::to_string(width) + "," + std::to_string(height),
        "--no-first-run",
        "--no-default-browser-check",
        "--autoplay-policy=no-user-gesture-required",
        "about:blank",
    };
    args.insert(args.end(), flags.begin(), flags.end());
    std::vector<char*> argv;
    for (std::string& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    // Chromium's chatter must not reach our stdout (that is the protocol).
    posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attr, 0);
    pid_t pid = -1;
    const int rc = posix_spawnp(&pid, args[0].c_str(), &fa, &attr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    return rc == 0 ? pid : -1;
}

void stopChromium(pid_t pid) {
    if (pid <= 0) return;
    kill(-pid, SIGTERM);
    for (int k = 0; k < 30; ++k) {
        if (waitpid(pid, nullptr, WNOHANG) == pid) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    kill(-pid, SIGKILL);
    waitpid(pid, nullptr, 0);
}

// Chromium writes the chosen debugging port to <profile>/DevToolsActivePort.
int waitForDevToolsPort(const std::string& profile, pid_t pid, int timeoutMs) {
    const auto end = Clock::now() + std::chrono::milliseconds(timeoutMs);
    while (Clock::now() < end) {
        std::ifstream f(profile + "/DevToolsActivePort");
        int port = 0;
        if (f >> port && port > 0) return port;
        if (waitpid(pid, nullptr, WNOHANG) == pid) return -1;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return -1;
}

// WebSocket path of the first normal page target.
std::string findPageTarget(int port) {
    for (int attempt = 0; attempt < 50; ++attempt) {
        pvrjson::Value list;
        if (pvrjson::parse(httpGet(port, "/json/list"), list) && list.type == pvrjson::Value::Type::Array) {
            for (const pvrjson::Value& t : list.array) {
                if (t.getString("type") != "page") continue;
                const std::string ws = t.getString("webSocketDebuggerUrl");
                const size_t p = ws.find("/devtools/");
                if (p != std::string::npos) return ws.substr(p);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return {};
}

struct KeyInfo {
    const char* key;
    const char* code;
    int vk;
    const char* text;
};

const KeyInfo* keyInfo(const std::string& name) {
    static const struct { const char* name; KeyInfo info; } kKeys[] = {
        {"Enter", {"Enter", "Enter", 13, "\r"}},
        {"Backspace", {"Backspace", "Backspace", 8, nullptr}},
        {"Delete", {"Delete", "Delete", 46, nullptr}},
        {"Left", {"ArrowLeft", "ArrowLeft", 37, nullptr}},
        {"Up", {"ArrowUp", "ArrowUp", 38, nullptr}},
        {"Right", {"ArrowRight", "ArrowRight", 39, nullptr}},
        {"Down", {"ArrowDown", "ArrowDown", 40, nullptr}},
        {"Escape", {"Escape", "Escape", 27, nullptr}},
        {"Tab", {"Tab", "Tab", 9, nullptr}},
        {"Space", {" ", "Space", 32, " "}},
    };
    for (const auto& k : kKeys) {
        if (name == k.name) return &k.info;
    }
    return nullptr;
}

volatile std::sig_atomic_t gStop = 0;
void onSignal(int) { gStop = 1; }

}  // namespace

int main(int argc, char** argv) {
    std::string shmName;
    std::string url = "https://www.youtube.com";
    int width = 1280;
    int height = 720;
    for (int i = 1; i + 1 < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--shm") shmName = argv[++i];
        else if (a == "--url") url = argv[++i];
        else if (a == "--width") width = std::atoi(argv[++i]);
        else if (a == "--height") height = std::atoi(argv[++i]);
    }
    width = std::clamp(width, 320, kBrowserMaxWidth);
    height = std::clamp(height, 240, kBrowserMaxHeight);
    std::signal(SIGTERM, onSignal);
    std::signal(SIGINT, onSignal);
    std::signal(SIGPIPE, SIG_IGN);

    // Shared memory from the app.
    const int shmFd = shm_open(shmName.c_str(), O_RDWR, 0600);
    if (shmName.empty() || shmFd < 0) {
        emitLine("error cannot open shared memory");
        return 2;
    }
    const size_t shmSize = browserShmSize(width, height);
    void* shmMem = mmap(nullptr, shmSize, PROT_READ | PROT_WRITE, MAP_SHARED, shmFd, 0);
    close(shmFd);
    if (shmMem == MAP_FAILED) {
        emitLine("error cannot map shared memory");
        return 2;
    }
    auto* header = static_cast<BrowserShmHeader*>(shmMem);
    uint8_t* pixels = static_cast<uint8_t*>(shmMem) + sizeof(BrowserShmHeader);

    // Chromium with a persistent profile (cookies and logins survive).
    const char* home = std::getenv("HOME");
    const std::string profile = std::string(home ? home : "/tmp") + "/.config/pixelvr/chromium";
    mkdir((std::string(home ? home : "/tmp") + "/.config/pixelvr").c_str(), 0700);
    mkdir(profile.c_str(), 0700);
    unlink((profile + "/DevToolsActivePort").c_str());
    const pid_t chrome = launchChromium(profile, width, height);
    if (chrome <= 0) {
        emitLine("error Chromium is not installed (install it from Discover)");
        return 3;
    }
    const int port = waitForDevToolsPort(profile, chrome, 20000);
    const std::string target = port > 0 ? findPageTarget(port) : std::string();
    WebSocket ws;
    if (target.empty() || !ws.connectTo(port, target)) {
        emitLine("error could not connect to Chromium");
        stopChromium(chrome);
        return 4;
    }

    int nextId = 1;
    auto cdp = [&](const char* method, const std::string& params = "{}") {
        const int id = nextId++;
        ws.sendText("{\"id\":" + std::to_string(id) + ",\"method\":\"" + method + "\",\"params\":" + params + "}");
        return id;
    };
    cdp("Page.enable");
    // The headless window reserves room for (invisible) browser UI; pin the
    // page viewport to exactly our frame size so input maps 1:1.
    cdp("Emulation.setDeviceMetricsOverride", "{\"width\":" + std::to_string(width) + ",\"height\":" +
                                                  std::to_string(height) +
                                                  ",\"deviceScaleFactor\":1,\"mobile\":false}");
    cdp("Page.startScreencast", "{\"format\":\"jpeg\",\"quality\":80,\"maxWidth\":" + std::to_string(width) +
                                    ",\"maxHeight\":" + std::to_string(height) + "}");
    cdp("Page.navigate", "{\"url\":" + pvrjson::quote(url) + "}");
    emitLine("ready");

    tjhandle jpeg = tjInitDecompress();
    bool mouseDown = false;
    std::string pendingIn;
    std::string lastUrl;
    std::string lastTitle;
    int lastLoading = -1;
    int stateRequest = -1;
    auto nextStatePoll = Clock::now();

    auto mouse = [&](const char* type, double x, double y, const char* button, int clicks) {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "{\"type\":\"%s\",\"x\":%.1f,\"y\":%.1f,\"button\":\"%s\",\"buttons\":%d,\"clickCount\":%d}",
                      type, x, y, button, mouseDown ? 1 : 0, clicks);
        cdp("Input.dispatchMouseEvent", buf);
    };
    auto evaluate = [&](const std::string& expr) {
        return cdp("Runtime.evaluate", "{\"expression\":" + pvrjson::quote(expr) + ",\"returnByValue\":true}");
    };

    auto handleCommand = [&](const std::string& line) {
        const size_t sp = line.find(' ');
        const std::string cmd = line.substr(0, sp);
        const std::string rest = sp == std::string::npos ? std::string() : line.substr(sp + 1);
        double x = 0, y = 0;
        int d = 0;
        if (cmd == "move" && std::sscanf(rest.c_str(), "%lf %lf", &x, &y) == 2) {
            mouse("mouseMoved", x, y, mouseDown ? "left" : "none", 0);
        } else if (cmd == "down" && std::sscanf(rest.c_str(), "%lf %lf", &x, &y) == 2) {
            mouseDown = true;
            mouse("mousePressed", x, y, "left", 1);
        } else if (cmd == "up" && std::sscanf(rest.c_str(), "%lf %lf", &x, &y) == 2) {
            mouseDown = false;
            mouse("mouseReleased", x, y, "left", 1);
        } else if (cmd == "wheel" && std::sscanf(rest.c_str(), "%lf %lf %d", &x, &y, &d) == 3) {
            // Our DY follows wheel angles (positive = up); CDP's deltaY is the
            // scroll amount (positive = down).
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "{\"type\":\"mouseWheel\",\"x\":%.1f,\"y\":%.1f,\"deltaX\":0,\"deltaY\":%d}", x, y, -d);
            cdp("Input.dispatchMouseEvent", buf);
        } else if (cmd == "key") {
            if (const KeyInfo* k = keyInfo(rest)) {
                std::string down = std::string("{\"type\":\"keyDown\",\"key\":") + pvrjson::quote(k->key) +
                                   ",\"code\":" + pvrjson::quote(k->code) +
                                   ",\"windowsVirtualKeyCode\":" + std::to_string(k->vk);
                if (k->text) down += ",\"text\":" + pvrjson::quote(k->text);
                cdp("Input.dispatchKeyEvent", down + "}");
                cdp("Input.dispatchKeyEvent", std::string("{\"type\":\"keyUp\",\"key\":") + pvrjson::quote(k->key) +
                                                  ",\"code\":" + pvrjson::quote(k->code) +
                                                  ",\"windowsVirtualKeyCode\":" + std::to_string(k->vk) + "}");
            }
        } else if (cmd == "text" && !rest.empty()) {
            cdp("Input.insertText", "{\"text\":" + pvrjson::quote(rest) + "}");
        } else if (cmd == "nav" && !rest.empty()) {
            std::string target = rest;
            if (target.find("://") == std::string::npos) target = "https://" + target;
            cdp("Page.navigate", "{\"url\":" + pvrjson::quote(target) + "}");
        } else if (cmd == "back") {
            evaluate("history.back()");
        } else if (cmd == "forward") {
            evaluate("history.forward()");
        } else if (cmd == "reload") {
            cdp("Page.reload");
        } else if (cmd == "quit") {
            gStop = 1;
        }
    };

    auto handleMessage = [&](const std::string& text) {
        pvrjson::Value msg;
        if (!pvrjson::parse(text, msg)) return;
        const std::string method = msg.getString("method");
        if (method == "Page.screencastFrame") {
            const pvrjson::Value* params = msg.get("params");
            if (!params) return;
            cdp("Page.screencastFrameAck", "{\"sessionId\":" +
                                               std::to_string(static_cast<long long>(params->getNumber("sessionId"))) +
                                               "}");
            const std::vector<uint8_t> data = base64Decode(params->getString("data"));
            int w = 0, h = 0, subsamp = 0, colorspace = 0;
            if (data.empty() ||
                tjDecompressHeader3(jpeg, data.data(), static_cast<unsigned long>(data.size()), &w, &h, &subsamp,
                                    &colorspace) != 0) {
                return;
            }
            w = std::min(w, width);
            h = std::min(h, height);
            const uint64_t seq = header->seq.load(std::memory_order_relaxed);
            header->seq.store(seq + 1, std::memory_order_release);  // odd: writing
            tjDecompress2(jpeg, data.data(), static_cast<unsigned long>(data.size()), pixels, w, width * 4, h,
                          TJPF_RGBA, TJFLAG_FASTDCT);
            header->width = static_cast<uint32_t>(width);
            header->height = static_cast<uint32_t>(height);
            header->seq.store(seq + 2, std::memory_order_release);  // even: frame ready
            return;
        }
        if (msg.getNumber("id", -1) == stateRequest) {
            // [href, title, readyState] from the state poll.
            const pvrjson::Value* result = msg.get("result");
            const pvrjson::Value* inner = result ? result->get("result") : nullptr;
            pvrjson::Value state;
            if (inner && pvrjson::parse(inner->getString("value"), state) &&
                state.type == pvrjson::Value::Type::Array && state.array.size() == 3) {
                if (state.array[0].str != lastUrl) emitLine("url " + (lastUrl = state.array[0].str));
                if (state.array[1].str != lastTitle) emitLine("title " + (lastTitle = state.array[1].str));
                const int loading = state.array[2].str == "complete" ? 0 : 1;
                if (loading != lastLoading) emitLine((lastLoading = loading) ? "loading 1" : "loading 0");
            }
            stateRequest = -1;
        }
    };

    int rc = 0;
    bool chromeGone = false;
    while (!gStop) {
        pollfd fds[2] = {{STDIN_FILENO, POLLIN, 0}, {ws.fd(), POLLIN, 0}};
        poll(fds, 2, 100);
        if (fds[0].revents & (POLLIN | POLLHUP)) {
            char buf[4096];
            const ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n <= 0) break;  // the app went away
            pendingIn.append(buf, static_cast<size_t>(n));
            size_t nl;
            while ((nl = pendingIn.find('\n')) != std::string::npos) {
                handleCommand(pendingIn.substr(0, nl));
                pendingIn.erase(0, nl + 1);
            }
        }
        if (fds[1].revents) {
            std::vector<std::string> messages;
            if (!ws.receive(messages)) {
                emitLine("error Chromium closed the connection");
                rc = 5;
                break;
            }
            for (const std::string& m : messages) handleMessage(m);
        }
        if (Clock::now() >= nextStatePoll && stateRequest < 0) {
            stateRequest = evaluate("JSON.stringify([location.href, document.title, document.readyState])");
            nextStatePoll = Clock::now() + std::chrono::milliseconds(500);
        }
        if (waitpid(chrome, nullptr, WNOHANG) == chrome) {
            emitLine("error Chromium exited");
            rc = 6;
            chromeGone = true;
            break;
        }
    }

    tjDestroy(jpeg);
    if (!chromeGone) stopChromium(chrome);
    munmap(shmMem, shmSize);
    return rc;
}

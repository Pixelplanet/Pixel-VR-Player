// pvr-browser: drives the user's installed Chromium for PixelVR's browser screens.
//
// Two modes:
//   pvr-browser --launch --width W --height H
//     Starts Chromium hidden (headless; the Flathub org.chromium.Chromium or a
//     `chromium` on PATH) on the user's own profile (falling back to
//     ~/.config/pixelvr/chromium while their Chromium has it open), prints
//     "profile PATH" and "port N" and keeps Chromium running until stdin
//     closes or "quit".
//   pvr-browser --port N --shm NAME --url URL --width W --height H
//               [--profile PATH] [--kiosk] [--incognito]
//     One browser screen: its own Chromium windows (tabs, popups) and a
//     toolbar page (tabs, address bar, bookmarks; hidden with --kiosk) over
//     the Chrome DevTools Protocol; closes only its windows when it exits.
//     --incognito uses a private browser context:
//   - Page.startScreencast frames (JPEG) of the toolbar and the active tab
//     are composited into shared memory (--shm NAME, created by the app,
//     sized for kBrowserMaxHeight rows): BrowserShmHeader (current frame
//     size) + RGBA pixels, published with a sequence counter (odd while
//     writing); the page is --height rows, the toolbar adds to the frame,
//   - stdin, one command per line (coordinates in frame pixels):
//       move X Y | down X Y | up X Y | wheel X Y DY | key NAME | text UTF8
//       nav URL | tab URL | back | forward | reload | fullscreen | eval JS
//       media toggle|play|pause|live|seek T|seekrel D | mute 0|1
//       volume PERCENT | pausemedia LIVE(0|1) | keyboard | quit
//   - stdout, one event per line: ready | url URL | title TEXT | loading 0|1
//     | media 0 | media 1 PAUSED POS DUR LIVE SEEKSTART SEEKEND
//     | videoinfo W H FRAMES DROPPED "CODECS"
//     | focus c|p 0 | focus c|p 1 PASSWORD CURSOR "VALUE" (c = after a click)
//     | value CURSOR "VALUE" | download NAME | eval RESULT | error TEXT
#include "browser_protocol.hpp"
#include "mini_json.hpp"
#include "toolbar_html.hpp"

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
#include <functional>
#include <iterator>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <optional>
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

enum class ChromiumKind { None, Flatpak, Native };

ChromiumKind findChromium() {
    if (commandSucceeds({"flatpak", "info", "org.chromium.Chromium"})) return ChromiumKind::Flatpak;
    if (commandSucceeds({"sh", "-c", "command -v chromium"})) return ChromiumKind::Native;
    return ChromiumKind::None;
}

// Launches Chromium in its own process group; returns its pid or -1.
pid_t launchChromium(ChromiumKind kind, const std::string& profile, int width, int height) {
    std::vector<std::string> args;
    if (kind == ChromiumKind::Flatpak) args = {"flatpak", "run", "--command=chromium", "org.chromium.Chromium"};
    else if (kind == ChromiumKind::Native) args = {"chromium"};
    else return -1;
    const std::vector<std::string> flags = {
        "--headless=new",
        "--remote-debugging-port=0",
        "--user-data-dir=" + profile,
        // Same cookie key as a desktop session without a keyring, so the
        // user's existing logins can be read.
        "--password-store=basic",
        "--window-size=" + std::to_string(width) + "," + std::to_string(height),
        // Headless pretends to have an 800x600 monitor; fullscreen video would
        // shrink to that inside our frame.
        "--screen-info={0,0 " + std::to_string(width) + "x" + std::to_string(height) + "}",
        "--no-first-run",
        "--no-default-browser-check",
        "--autoplay-policy=no-user-gesture-required",
        // Headless renders in software by default; draw pages on the GPU
        // (ANGLE on Vulkan / Mesa Turnip). Video decoding stays on the CPU:
        // Chromium has no hardware decoder for this platform.
        "--enable-gpu",
        "--ignore-gpu-blocklist",
        "--use-angle=vulkan",
        "--enable-features=Vulkan,DefaultANGLEVulkan,VulkanFromANGLE",
        "about:blank",
    };
    args.insert(args.end(), flags.begin(), flags.end() - 1);
    // Extra flags for experiments: PVR_CHROMIUM_FLAGS="--flag1 --flag2".
    if (const char* extra = std::getenv("PVR_CHROMIUM_FLAGS")) {
        std::string word;
        for (const char* p = extra;; ++p) {
            if (*p == ' ' || *p == '\0') {
                if (!word.empty()) args.push_back(word);
                word.clear();
                if (*p == '\0') break;
            } else {
                word += *p;
            }
        }
    }
    args.push_back(flags.back());  // the start URL stays last
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

// Waits for the reply to request `id` on `ws` (other messages are dropped).
bool waitForReply(WebSocket& ws, int id, pvrjson::Value& reply, int timeoutMs) {
    const auto end = Clock::now() + std::chrono::milliseconds(timeoutMs);
    while (Clock::now() < end) {
        pollfd pfd{ws.fd(), POLLIN, 0};
        poll(&pfd, 1, 100);
        std::vector<std::string> messages;
        if (!ws.receive(messages)) return false;
        for (const std::string& m : messages) {
            if (pvrjson::parse(m, reply) && reply.getNumber("id", -1) == id) return true;
        }
    }
    return false;
}

std::string browserSocketPath(int port) {
    pvrjson::Value version;
    if (!pvrjson::parse(httpGet(port, "/json/version"), version)) return {};
    const std::string ws = version.getString("webSocketDebuggerUrl");
    const size_t p = ws.find("/devtools/");
    return p == std::string::npos ? std::string() : ws.substr(p);
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
        {"Home", {"Home", "Home", 36, nullptr}},
        {"End", {"End", "End", 35, nullptr}},
        {"PageUp", {"PageUp", "PageUp", 33, nullptr}},
        {"PageDown", {"PageDown", "PageDown", 34, nullptr}},
    };
    for (const auto& k : kKeys) {
        if (name == k.name) return &k.info;
    }
    return nullptr;
}

volatile std::sig_atomic_t gStop = 0;
void onSignal(int) { gStop = 1; }

// --launch: start the shared Chromium, report its port, keep it alive.
int runLauncher(int width, int height) {
    // Prefer the user's own Chromium profile so desktop logins carry over. A
    // profile can only be open once: if their Chromium is running, it hands
    // our launch over and exits, and we fall back to a profile of our own.
    const char* homeEnv = std::getenv("HOME");
    const std::string home = homeEnv ? homeEnv : "/tmp";
    const ChromiumKind kind = findChromium();
    if (kind == ChromiumKind::None) {
        emitLine("error Chromium is not installed (install it from Discover)");
        return 3;
    }
    const std::string userProfile = kind == ChromiumKind::Flatpak
                                        ? home + "/.var/app/org.chromium.Chromium/config/chromium"
                                        : home + "/.config/chromium";
    const std::string ownProfile = home + "/.config/pixelvr/chromium";
    pid_t chrome = -1;
    int port = -1;
    std::string usedProfile;
    for (const std::string& profile : {userProfile, ownProfile}) {
        if (profile == ownProfile) {
            mkdir((home + "/.config/pixelvr").c_str(), 0700);
            mkdir(profile.c_str(), 0700);
        }
        unlink((profile + "/DevToolsActivePort").c_str());
        chrome = launchChromium(kind, profile, width, height);
        if (chrome <= 0) break;
        port = waitForDevToolsPort(profile, chrome, 20000);
        usedProfile = profile;
        if (port > 0) break;
        stopChromium(chrome);
        chrome = -1;
        emitLine("error Chromium profile in use; using a separate profile (close Chromium to share logins)");
    }
    if (chrome <= 0) {
        emitLine("error could not start Chromium");
        return 3;
    }
    emitLine("profile " + usedProfile);
    emitLine("port " + std::to_string(port));
    while (!gStop) {
        pollfd pfd{STDIN_FILENO, POLLIN, 0};
        poll(&pfd, 1, 200);
        if (pfd.revents & (POLLIN | POLLHUP)) {
            char buf[256];
            const ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n <= 0 || std::string(buf, static_cast<size_t>(n)).find("quit") != std::string::npos) break;
        }
        if (waitpid(chrome, nullptr, WNOHANG) == chrome) {
            emitLine("error Chromium exited");
            return 6;
        }
    }
    stopChromium(chrome);
    return 0;
}

// ------------------------------------------------------------ screen mode --
// One browser screen: its own Chromium windows (tabs) plus the toolbar page,
// all driven over one browser-level DevTools connection with flattened
// sessions. The frame in shared memory is the toolbar (when shown) stacked
// above the active tab's page; input in frame pixels is routed to whichever
// of the two is under the pointer.

constexpr int kTabsH = 34;       // toolbar_html.hpp: #tabs
constexpr int kNavH = 44;        // #nav
constexpr int kBookmarksH = 30;  // #bmbar (only with bookmarks on the bar)
constexpr const char* kHomeUrl = "https://www.google.com";

struct ScreenOptions {
    int port = -1;
    std::string shmName;
    std::string url;
    std::string profile;
    int width = 1280;
    int height = 720;  // page viewport; the toolbar adds to the frame
    bool kiosk = false;
    bool incognito = false;
};

struct Tab {
    std::string targetId;
    std::string session;
    std::string url;
    std::string title;
    std::string openerId;
    bool loading = false;
    bool canBack = false;
    bool canFwd = false;
};

struct Pixels {
    std::vector<uint8_t> px;
    int w = 0;
    int h = 0;
};

// Chrome stores times as microseconds since 1601-01-01.
std::string chromeTimeNow() {
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    return std::to_string(static_cast<long long>(us) + 11644473600000000LL);
}

std::string randomGuid() {
    std::random_device rd;
    uint8_t b[16];
    for (uint8_t& x : b) x = static_cast<uint8_t>(rd());
    b[6] = static_cast<uint8_t>((b[6] & 0x0F) | 0x40);
    b[8] = static_cast<uint8_t>((b[8] & 0x3F) | 0x80);
    char out[40];
    std::snprintf(out, sizeof(out), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1],
                  b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return out;
}

// The user's Chromium bookmarks (<profile>/Default/Bookmarks). Headless
// Chromium has no bookmark UI, so the star edits the file directly; desktop
// Chromium picks the changes up on its next start.
class Bookmarks {
public:
    void setProfile(const std::string& profile) {
        if (!profile.empty()) path_ = profile + "/Default/Bookmarks";
    }
    // Re-reads the file when it changed; true if it did.
    bool refresh() {
        if (path_.empty()) return false;
        struct stat st {};
        const long long mt = (stat(path_.c_str(), &st) == 0) ? static_cast<long long>(st.st_mtime) : -1;
        if (mt == mtime_ && loaded_) return false;
        mtime_ = mt;
        loaded_ = true;
        doc_ = pvrjson::Value{};
        std::ifstream f(path_);
        if (f) {
            const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            pvrjson::parse(text, doc_);
        }
        return true;
    }
    // {bar:[...], other:[...]} in the toolbar's format.
    std::string toolbarJson() const {
        std::string bar = "[]", other = "[]";
        if (const pvrjson::Value* roots = doc_.get("roots")) {
            if (const pvrjson::Value* b = roots->get("bookmark_bar")) bar = nodes(b->get("children"));
            std::string o = "[]";
            if (const pvrjson::Value* ot = roots->get("other")) o = nodes(ot->get("children"));
            std::string m = "[]";
            if (const pvrjson::Value* sy = roots->get("synced")) m = nodes(sy->get("children"));
            // Mobile bookmarks join "other" (rarely used on a headset).
            if (m != "[]") o = o == "[]" ? m : o.substr(0, o.size() - 1) + "," + m.substr(1);
            other = o;
        }
        return "{\"bar\":" + bar + ",\"other\":" + other + "}";
    }
    bool hasBarItems() const {
        const pvrjson::Value* roots = doc_.get("roots");
        const pvrjson::Value* b = roots ? roots->get("bookmark_bar") : nullptr;
        const pvrjson::Value* c = b ? b->get("children") : nullptr;
        return c && c->type == pvrjson::Value::Type::Array && !c->array.empty();
    }
    bool contains(const std::string& url) const {
        const pvrjson::Value* roots = doc_.get("roots");
        if (!roots || url.empty()) return false;
        for (const auto& kv : roots->object) {
            if (containsIn(kv.second, url)) return true;
        }
        return false;
    }
    // Adds `url` to the bookmarks bar, or removes it everywhere if present.
    void toggle(const std::string& url, const std::string& title) {
        if (path_.empty() || url.empty()) return;
        refresh();
        if (doc_.type != pvrjson::Value::Type::Object || !doc_.get("roots")) doc_ = emptyDocument();
        pvrjson::Value* roots = mutableGet(doc_, "roots");
        if (contains(url)) {
            for (auto& kv : roots->object) removeFrom(kv.second, url);
        } else {
            pvrjson::Value* bar = mutableGet(*roots, "bookmark_bar");
            if (!bar) return;
            pvrjson::Value* children = mutableGet(*bar, "children");
            if (!children) {
                bar->object.emplace_back("children", pvrjson::Value{});
                children = &bar->object.back().second;
                children->type = pvrjson::Value::Type::Array;
            }
            pvrjson::Value node;
            node.type = pvrjson::Value::Type::Object;
            auto str = [](const std::string& s) {
                pvrjson::Value v;
                v.type = pvrjson::Value::Type::String;
                v.str = s;
                return v;
            };
            node.object = {{"date_added", str(chromeTimeNow())},
                           {"guid", str(randomGuid())},
                           {"id", str(std::to_string(maxId(doc_) + 1))},
                           {"name", str(title.empty() ? url : title)},
                           {"type", str("url")},
                           {"url", str(url)}};
            children->array.push_back(std::move(node));
        }
        // Chrome only logs a checksum mismatch; leave it out rather than stale.
        doc_.object.erase(std::remove_if(doc_.object.begin(), doc_.object.end(),
                                         [](const auto& kv) { return kv.first == "checksum"; }),
                          doc_.object.end());
        const std::string dir = path_.substr(0, path_.rfind('/'));
        mkdir(dir.c_str(), 0700);
        const std::string tmp = path_ + ".pvr-tmp";
        {
            std::ofstream f(tmp, std::ios::trunc);
            if (!f) return;
            f << pvrjson::dump(doc_);
        }
        std::rename(tmp.c_str(), path_.c_str());
        struct stat st {};
        if (stat(path_.c_str(), &st) == 0) mtime_ = static_cast<long long>(st.st_mtime);
    }

private:
    static pvrjson::Value* mutableGet(pvrjson::Value& v, const char* key) {
        for (auto& kv : v.object) {
            if (kv.first == key) return &kv.second;
        }
        return nullptr;
    }
    static std::string nodes(const pvrjson::Value* children) {
        if (!children || children->type != pvrjson::Value::Type::Array) return "[]";
        std::string out = "[";
        for (const pvrjson::Value& c : children->array) {
            const std::string type = c.getString("type");
            if (type != "url" && type != "folder") continue;
            if (out.size() > 1) out += ',';
            out += "{\"n\":" + pvrjson::quote(c.getString("name"));
            if (type == "url") out += ",\"u\":" + pvrjson::quote(c.getString("url"));
            else out += ",\"c\":" + nodes(c.get("children"));
            out += '}';
        }
        return out + "]";
    }
    static bool containsIn(const pvrjson::Value& n, const std::string& url) {
        if (n.getString("type") == "url" && n.getString("url") == url) return true;
        if (const pvrjson::Value* c = n.get("children")) {
            for (const pvrjson::Value& k : c->array) {
                if (containsIn(k, url)) return true;
            }
        }
        return false;
    }
    static void removeFrom(pvrjson::Value& n, const std::string& url) {
        pvrjson::Value* c = mutableGet(n, "children");
        if (!c) return;
        c->array.erase(std::remove_if(c->array.begin(), c->array.end(),
                                      [&](const pvrjson::Value& k) {
                                          return k.getString("type") == "url" && k.getString("url") == url;
                                      }),
                       c->array.end());
        for (pvrjson::Value& k : c->array) removeFrom(k, url);
    }
    static long long maxId(const pvrjson::Value& n) {
        long long m = std::atoll(n.getString("id").c_str());
        for (const auto& kv : n.object) {
            if (kv.second.type == pvrjson::Value::Type::Object) m = std::max(m, maxId(kv.second));
            if (kv.second.type == pvrjson::Value::Type::Array) {
                for (const pvrjson::Value& k : kv.second.array) m = std::max(m, maxId(k));
            }
        }
        return m;
    }
    static pvrjson::Value emptyDocument() {
        // Chrome's fixed GUIDs for the three permanent folders.
        const std::string text =
            "{\"roots\":{"
            "\"bookmark_bar\":{\"children\":[],\"date_added\":\"" + chromeTimeNow() +
            "\",\"guid\":\"0bc5d13f-2cba-5d74-951f-3f233fe6c908\",\"id\":\"1\",\"name\":\"Bookmarks bar\","
            "\"type\":\"folder\"},"
            "\"other\":{\"children\":[],\"date_added\":\"" + chromeTimeNow() +
            "\",\"guid\":\"82b081ec-3dd3-529c-8475-ab6c344590dd\",\"id\":\"2\",\"name\":\"Other bookmarks\","
            "\"type\":\"folder\"},"
            "\"synced\":{\"children\":[],\"date_added\":\"" + chromeTimeNow() +
            "\",\"guid\":\"4cf2e351-0e85-532b-bb37-df045d8f8d0f\",\"id\":\"3\",\"name\":\"Mobile bookmarks\","
            "\"type\":\"folder\"}},\"version\":1}";
        pvrjson::Value v;
        pvrjson::parse(text, v);
        return v;
    }

    std::string path_;
    pvrjson::Value doc_;
    long long mtime_ = -2;
    bool loaded_ = false;
};

// Probes evaluated in a page. EDITABLE yields the focused text field (through
// shadow roots and same-origin frames) or null.
const char* const kEditableJs =
    "(()=>{let e=document.activeElement;"
    "try{for(;;){if(e&&e.shadowRoot&&e.shadowRoot.activeElement){e=e.shadowRoot.activeElement;continue}"
    "if(e&&(e.tagName==='IFRAME'||e.tagName==='FRAME')&&e.contentDocument){e=e.contentDocument.activeElement;continue}"
    "break}}catch(_){}"
    "if(!e)return null;const t=(e.type||'').toLowerCase();"
    "const ed=e.isContentEditable||e.tagName==='TEXTAREA'||(e.tagName==='INPUT'&&!['button','submit','checkbox',"
    "'radio','range','color','file','image','reset','hidden'].includes(t));"
    "if(!ed)return null;"
    "const v=e.isContentEditable?(e.innerText||''):(e.value||'');"
    "let c=v.length;try{if(typeof e.selectionStart==='number')c=e.selectionStart}catch(_){}"
    "return {pw:t==='password'?1:0,v:v.slice(0,2000),c:c}})()";

// Native <select> popups are drawn outside the page (not in the screencast):
// open them as a toolbar menu instead. Runs in every page of our tabs.
const char* const kSelectJs =
    "(()=>{if(window.__pvrSel)return;window.__pvrSel=1;"
    "document.addEventListener('mousedown',e=>{"
    "const s=e.target&&e.target.closest&&e.target.closest('select');"
    "if(!s||s.multiple||s.size>1||s.disabled||e.button!==0)return;"
    "e.preventDefault();e.stopPropagation();s.focus();window.__pvrSelEl=s;"
    "const r=s.getBoundingClientRect();"
    "const o=[...s.options].map(x=>[x.text,x.disabled?1:0,"
    "x.parentElement&&x.parentElement.tagName==='OPTGROUP'?x.parentElement.label:'']);"
    "try{pvrSelect(JSON.stringify({o:o,i:s.selectedIndex,x:r.left,y:r.top,h:r.height,w:r.width}))}catch(_){}"
    "},true);"
    "window.__pvrPick=i=>{const s=window.__pvrSelEl;if(!s||i<0||i>=s.options.length)return;"
    "s.selectedIndex=i;s.dispatchEvent(new Event('input',{bubbles:true}));"
    "s.dispatchEvent(new Event('change',{bubbles:true}))};"
    "})()";

// The page's main video: the largest one that has media loaded.
const char* const kBestVideoJs =
    "(()=>{let b=null,ba=0;for(const v of document.querySelectorAll('video')){"
    "const r=v.getBoundingClientRect(),a=r.width*r.height;if(v.readyState>0&&a>=ba){ba=a;b=v}}return b})()";

// Whether media element v is a livestream: endless, or YouTube's player in
// live mode (its live videos report a finite but meaningless duration).
const char* const kIsLiveJs =
    "(v=>{if(!isFinite(v.duration))return true;"
    "const b=document.querySelector('.ytp-live-badge');"
    "return !!(document.querySelector('.ytp-time-display.ytp-live')||(b&&b.offsetParent!==null&&"
    "getComputedStyle(b).display!=='none'))})";

int runScreen(const ScreenOptions& opt) {
    const int W = opt.width;
    const int pageH = opt.height;

    const int shmFd = shm_open(opt.shmName.c_str(), O_RDWR, 0600);
    if (opt.shmName.empty() || shmFd < 0) {
        emitLine("error cannot open shared memory");
        return 2;
    }
    const size_t shmSize = browserShmSize(W, kBrowserMaxHeight);
    void* shmMem = mmap(nullptr, shmSize, PROT_READ | PROT_WRITE, MAP_SHARED, shmFd, 0);
    close(shmFd);
    if (shmMem == MAP_FAILED) {
        emitLine("error cannot map shared memory");
        return 2;
    }
    auto* header = static_cast<BrowserShmHeader*>(shmMem);
    uint8_t* shmPixels = static_cast<uint8_t*>(shmMem) + sizeof(BrowserShmHeader);

    WebSocket ws;
    const std::string browserPath = browserSocketPath(opt.port);
    if (opt.port <= 0 || browserPath.empty() || !ws.connectTo(opt.port, browserPath)) {
        emitLine("error could not reach Chromium");
        return 4;
    }

    // ---- DevTools plumbing: one socket, many sessions.
    int nextId = 1;
    std::vector<std::string> backlog;  // messages read while waiting for a reply
    std::vector<std::pair<int, std::function<void(const pvrjson::Value&)>>> pending;
    auto send = [&](const std::string& method, const std::string& params, const std::string& session) {
        const int id = nextId++;
        std::string m = "{\"id\":" + std::to_string(id) + ",\"method\":\"" + method + "\",\"params\":" + params;
        if (!session.empty()) m += ",\"sessionId\":" + pvrjson::quote(session);
        ws.sendText(m + "}");
        return id;
    };
    auto sendThen = [&](const std::string& method, const std::string& params, const std::string& session,
                        std::function<void(const pvrjson::Value&)> onReply) {
        pending.emplace_back(send(method, params, session), std::move(onReply));
    };
    // Blocking call (startup and new tabs): the reply's "result", others queued.
    auto call = [&](const std::string& method, const std::string& params, const std::string& session,
                    pvrjson::Value& result) {
        const int id = send(method, params, session);
        const auto end = Clock::now() + std::chrono::seconds(10);
        while (Clock::now() < end) {
            pollfd pfd{ws.fd(), POLLIN, 0};
            poll(&pfd, 1, 50);
            std::vector<std::string> msgs;
            if (!ws.receive(msgs)) return false;
            for (std::string& m : msgs) {
                pvrjson::Value v;
                if (m.find("\"id\":" + std::to_string(id) + ",") != std::string::npos && pvrjson::parse(m, v) &&
                    v.getNumber("id", -1) == id) {
                    if (v.get("error")) return false;
                    if (const pvrjson::Value* r = v.get("result")) result = *r;
                    return true;
                }
                backlog.push_back(std::move(m));
            }
        }
        return false;
    };
    auto evalIn = [&](const std::string& session, const std::string& expr,
                      std::function<void(const pvrjson::Value*)> onValue = nullptr) {
        const std::string params = "{\"expression\":" + pvrjson::quote(expr) +
                                   ",\"returnByValue\":true,\"userGesture\":true,\"awaitPromise\":false}";
        if (!onValue) {
            send("Runtime.evaluate", params, session);
            return;
        }
        sendThen("Runtime.evaluate", params, session, [onValue](const pvrjson::Value& msg) {
            const pvrjson::Value* r = msg.get("result");
            const pvrjson::Value* inner = r ? r->get("result") : nullptr;
            onValue(inner ? inner->get("value") : nullptr);
        });
    };

    // ---- Browser-wide setup.
    std::string contextId;
    if (opt.incognito) {
        pvrjson::Value r;
        if (call("Target.createBrowserContext", "{\"disposeOnDetach\":true}", "", r)) {
            contextId = r.getString("browserContextId");
        }
        if (contextId.empty()) emitLine("error could not open a private window");
    }
    const std::string ctxParam = contextId.empty() ? "" : ",\"browserContextId\":" + pvrjson::quote(contextId);
    send("Target.setDiscoverTargets", "{\"discover\":true}", "");
    {
        const char* homeEnv = std::getenv("HOME");
        const std::string downloads = std::string(homeEnv ? homeEnv : "/tmp") + "/Downloads";
        send("Browser.setDownloadBehavior", "{\"behavior\":\"allow\",\"downloadPath\":" + pvrjson::quote(downloads) +
                                                ",\"eventsEnabled\":true" + ctxParam + "}",
             "");
    }
    std::string userAgent;
    {
        pvrjson::Value version;
        if (pvrjson::parse(httpGet(opt.port, "/json/version"), version)) {
            userAgent = version.getString("User-Agent");
            const size_t h = userAgent.find("HeadlessChrome");
            if (h != std::string::npos) userAgent.replace(h, 14, "Chrome");
        }
    }

    // ---- Screen state.
    Bookmarks bookmarks;
    bookmarks.setProfile(opt.profile);
    bookmarks.refresh();
    std::vector<Tab> tabs;
    int active = -1;
    std::string toolbar;  // toolbar page session
    std::string toolbarTarget;
    Pixels tbPx;
    Pixels pagePx;
    std::string lastPageJpeg;  // base64, for the menu backdrop
    bool userFullscreen = false;  // F11
    bool pageFullscreen = false;  // the site's own fullscreen (video player)
    bool revealed = false;        // F11 mode: toolbar shown while pointing at the top
    Clock::time_point hideRevealAt{};
    bool expanded = false;        // toolbar covers the frame (menu / dialog)
    struct Dialog {
        std::string session;
        std::string type;
        std::string message;
        std::string def;
    };
    std::optional<Dialog> dialog;
    std::string downloadText;
    Clock::time_point downloadClearAt{};
    std::vector<std::pair<std::string, std::string>> downloads;  // guid, name
    std::string selectSession;  // page whose <select> is open as a menu
    std::string selectJson;     // its options (from kSelectJs), empty = none
    bool muted = false;
    int volumePct = 100;
    std::string pressSession;  // where the current press went
    double pressOffsetY = 0.0;
    bool mouseDown = false;
    std::string keySession;  // keyboard target: page or toolbar
    std::string lastToolbarState;
    bool toolbarReady = false;
    std::string lastMedia;
    std::string lastVideoInfo;
    int lastPageEditable = 0;
    Clock::time_point probeAt{};
    std::string probeSession;
    bool probeIsClick = false;
    Clock::time_point valueAt{};
    std::string lastUrl, lastTitle;
    int lastLoading = -1;
    auto nextPoll = Clock::now();
    auto nextBookmarkCheck = Clock::now() + std::chrono::seconds(3);

    auto activeTab = [&]() -> Tab* { return (active >= 0 && active < static_cast<int>(tabs.size())) ? &tabs[active] : nullptr; };
    auto tabBySession = [&](const std::string& s) -> int {
        for (size_t i = 0; i < tabs.size(); ++i) {
            if (tabs[i].session == s) return static_cast<int>(i);
        }
        return -1;
    };
    auto tabByTarget = [&](const std::string& t) -> int {
        for (size_t i = 0; i < tabs.size(); ++i) {
            if (tabs[i].targetId == t) return static_cast<int>(i);
        }
        return -1;
    };
    auto barsH = [&]() { return kTabsH + kNavH + (bookmarks.hasBarItems() ? kBookmarksH : 0); };
    auto barsShown = [&]() { return !opt.kiosk && !userFullscreen && !pageFullscreen; };
    auto barsVisible = [&]() { return barsShown() || (revealed && !opt.kiosk && !pageFullscreen); };
    auto frameH = [&]() { return barsShown() ? barsH() + pageH : pageH; };
    auto pageTop = [&]() { return barsShown() ? barsH() : 0; };

    auto compose = [&]() {
        const int fh = std::min(frameH(), kBrowserMaxHeight);
        const uint64_t seq = header->seq.load(std::memory_order_relaxed);
        header->seq.store(seq + 1, std::memory_order_release);  // odd: writing
        const size_t rowBytes = static_cast<size_t>(W) * 4;
        auto blit = [&](const Pixels& src, int dstY, int rows) {
            rows = std::min({rows, src.h, fh - dstY});
            const size_t n = static_cast<size_t>(std::min(src.w, W)) * 4;
            for (int y = 0; y < rows; ++y) {
                std::memcpy(shmPixels + (static_cast<size_t>(dstY + y)) * rowBytes, src.px.data() + static_cast<size_t>(y) * src.w * 4, n);
            }
        };
        // Expanded: the toolbar covers the frame, once its first frame at full
        // height has arrived (until then the usual layout stays).
        if (expanded && tbPx.h >= fh) {
            blit(tbPx, 0, fh);
        } else {
            if (!pagePx.px.empty()) blit(pagePx, pageTop(), pageH);
            if (barsVisible() && !tbPx.px.empty()) blit(tbPx, 0, barsH());
        }
        header->width = static_cast<uint32_t>(W);
        header->height = static_cast<uint32_t>(fh);
        header->seq.store(seq + 2, std::memory_order_release);  // even: frame ready
    };

    auto toolbarHeight = [&]() { return expanded ? frameH() : barsH(); };
    int toolbarViewH = 0;
    auto sizeToolbar = [&]() {
        if (toolbar.empty()) return;
        const int h = toolbarHeight();
        if (h == toolbarViewH) return;
        toolbarViewH = h;
        send("Emulation.setDeviceMetricsOverride",
             "{\"width\":" + std::to_string(W) + ",\"height\":" + std::to_string(h) +
                 ",\"deviceScaleFactor\":1,\"mobile\":false}",
             toolbar);
    };

    auto pushToolbar = [&](bool force = false) {
        if (toolbar.empty() || !toolbarReady) return;
        const Tab* t = activeTab();
        std::string s = "{\"tabs\":[";
        for (size_t i = 0; i < tabs.size(); ++i) {
            if (i) s += ',';
            s += "{\"t\":" + pvrjson::quote(tabs[i].title) + ",\"u\":" + pvrjson::quote(tabs[i].url) +
                 ",\"l\":" + (tabs[i].loading ? "1" : "0") + "}";
        }
        s += "],\"a\":" + std::to_string(active);
        s += ",\"url\":" + pvrjson::quote(t ? t->url : "");
        s += std::string(",\"back\":") + (t && t->canBack ? "1" : "0");
        s += std::string(",\"fwd\":") + (t && t->canFwd ? "1" : "0");
        s += std::string(",\"loading\":") + (t && t->loading ? "1" : "0");
        s += std::string(",\"inc\":") + (contextId.empty() ? "0" : "1");
        s += ",\"bm\":" + bookmarks.toolbarJson();
        s += std::string(",\"starred\":") + (t && bookmarks.contains(t->url) ? "1" : "0");
        s += std::string(",\"bars\":") + (barsVisible() ? "1" : "0");
        s += std::string(",\"exp\":") + (expanded ? "1" : "0");
        s += ",\"fh\":" + std::to_string(frameH());
        s += ",\"dl\":" + (downloadText.empty() ? std::string("null") : pvrjson::quote(downloadText));
        s += ",\"sel\":" + (selectJson.empty() ? std::string("null") : selectJson);
        if (dialog) {
            s += ",\"dlg\":{\"k\":" + pvrjson::quote(dialog->type) + ",\"m\":" + pvrjson::quote(dialog->message) +
                 ",\"d\":" + pvrjson::quote(dialog->def) + "}";
        } else {
            s += ",\"dlg\":null";
        }
        s += "}";
        if (!force && s == lastToolbarState) return;
        lastToolbarState = s;
        evalIn(toolbar, "window.pvr&&pvr.set(" + s + ")");
    };

    auto setExpanded = [&](bool on) {
        if (on == expanded) return;
        expanded = on;
        if (on && !lastPageJpeg.empty()) {
            evalIn(toolbar, "window.pvr&&pvr.backdrop('data:image/jpeg;base64," + lastPageJpeg + "')");
        }
        sizeToolbar();
        pushToolbar();
        compose();
    };

    auto emitActiveState = [&]() {
        const Tab* t = activeTab();
        if (!t) return;
        if (t->url != lastUrl) emitLine("url " + (lastUrl = t->url));
        if (t->title != lastTitle) emitLine("title " + (lastTitle = t->title));
        const int l = t->loading ? 1 : 0;
        if (l != lastLoading) emitLine((lastLoading = l) ? "loading 1" : "loading 0");
    };

    auto requestHistory = [&](const std::string& session) {
        sendThen("Page.getNavigationHistory", "{}", session, [&, session](const pvrjson::Value& msg) {
            const int i = tabBySession(session);
            const pvrjson::Value* r = msg.get("result");
            if (i < 0 || !r) return;
            const int cur = static_cast<int>(r->getNumber("currentIndex", 0));
            const pvrjson::Value* e = r->get("entries");
            const int n = e ? static_cast<int>(e->array.size()) : 0;
            tabs[i].canBack = cur > 0;
            tabs[i].canFwd = cur + 1 < n;
            pushToolbar();
        });
    };

    auto setupTab = [&](Tab& t) {
        send("Page.enable", "{}", t.session);
        if (!userAgent.empty()) {
            send("Emulation.setUserAgentOverride", "{\"userAgent\":" + pvrjson::quote(userAgent) + "}", t.session);
        }
        // The headless window reserves room for (invisible) browser UI; pin the
        // viewport to the page area so input maps 1:1. The screen is as large
        // as the page, so the site's own fullscreen fills it.
        const std::string w = std::to_string(W), h = std::to_string(pageH);
        send("Emulation.setDeviceMetricsOverride",
             "{\"width\":" + w + ",\"height\":" + h + ",\"screenWidth\":" + w + ",\"screenHeight\":" + h +
                 ",\"deviceScaleFactor\":1,\"mobile\":false}",
             t.session);
        // Every window believes it has focus (caret, focus events, typing).
        send("Emulation.setFocusEmulationEnabled", "{\"enabled\":true}", t.session);
        send("Runtime.enable", "{}", t.session);  // binding calls arrive only with Runtime on
        send("Runtime.addBinding", "{\"name\":\"pvrSelect\"}", t.session);
        send("Page.addScriptToEvaluateOnNewDocument", "{\"source\":" + pvrjson::quote(kSelectJs) + "}", t.session);
        evalIn(t.session, kSelectJs);  // popups adopted after they loaded
        send("Runtime.runIfWaitingForDebugger", "{}", t.session);
    };

    auto attachTab = [&](const std::string& targetId, const std::string& opener) -> int {
        pvrjson::Value r;
        if (!call("Target.attachToTarget", "{\"targetId\":" + pvrjson::quote(targetId) + ",\"flatten\":true}", "", r)) {
            return -1;
        }
        Tab t;
        t.targetId = targetId;
        t.session = r.getString("sessionId");
        t.openerId = opener;
        setupTab(t);
        int at = static_cast<int>(tabs.size());
        const int op = tabByTarget(opener);
        if (op >= 0) at = op + 1;
        tabs.insert(tabs.begin() + at, std::move(t));
        if (active >= at) ++active;
        return at;
    };

    auto activate = [&](int i) {
        if (i < 0 || i >= static_cast<int>(tabs.size())) return;
        if (active == i && activeTab()) return;
        if (Tab* old = activeTab()) send("Page.stopScreencast", "{}", old->session);
        active = i;
        Tab& t = tabs[i];
        send("Page.startScreencast",
             "{\"format\":\"jpeg\",\"quality\":80,\"maxWidth\":" + std::to_string(W) +
                 ",\"maxHeight\":" + std::to_string(pageH) + "}",
             t.session);
        lastMedia.clear();
        lastPageEditable = 0;
        pageFullscreen = false;
        requestHistory(t.session);
        emitActiveState();
        pushToolbar();
        compose();
    };

    auto openTab = [&](const std::string& url, bool makeActive) -> int {
        pvrjson::Value r;
        if (!call("Target.createTarget", "{\"url\":\"about:blank\",\"newWindow\":true" + ctxParam + "}", "", r)) {
            return -1;
        }
        const int i = attachTab(r.getString("targetId"), "");
        if (i < 0) return -1;
        tabs[i].url = url;
        tabs[i].loading = true;
        send("Page.navigate", "{\"url\":" + pvrjson::quote(url) + "}", tabs[i].session);
        if (makeActive) activate(i);
        return i;
    };

    auto removeTab = [&](int i) {
        if (i < 0 || i >= static_cast<int>(tabs.size())) return;
        const bool wasActive = (i == active);
        if (dialog && dialog->session == tabs[i].session) {
            dialog.reset();
            setExpanded(false);
        }
        tabs.erase(tabs.begin() + i);
        if (active > i) --active;
        if (tabs.empty()) {
            active = -1;
            openTab(opt.kiosk ? opt.url : kHomeUrl, true);
            return;
        }
        if (wasActive) {
            active = -1;
            activate(std::min(i, static_cast<int>(tabs.size()) - 1));
        } else {
            pushToolbar();
        }
    };

    auto closeTab = [&](int i) {
        if (i < 0 || i >= static_cast<int>(tabs.size())) return;
        send("Target.closeTarget", "{\"targetId\":" + pvrjson::quote(tabs[i].targetId) + "}", "");
        removeTab(i);
    };

    // Media of the active tab, through the page's own player.
    auto mediaCommand = [&](const std::string& what) {
        const Tab* t = activeTab();
        if (!t) return;
        std::string js;
        if (what == "toggle") js = "v.paused?v.play():v.pause()";
        else if (what == "play") js = "v.play()";
        else if (what == "pause") js = "v.pause()";
        else if (what.rfind("seekrel ", 0) == 0) js = "v.currentTime=Math.max(0,v.currentTime+(" + std::to_string(std::atof(what.c_str() + 8)) + "))";
        else if (what.rfind("seek ", 0) == 0) js = "v.currentTime=" + std::to_string(std::max(0.0, std::atof(what.c_str() + 5)));
        else if (what == "live") {
            // Back to the live edge: YouTube's own LIVE button, else the end
            // of what can be played.
            js = "const b=document.querySelector('.ytp-live-badge'),yp=document.getElementById('movie_player');"
                 "if(b&&b.offsetParent!==null){b.click()}"
                 "else if(yp&&typeof yp.seekToLiveHead==='function'){yp.seekToLiveHead()}"
                 "else if(v.seekable.length){v.currentTime=Math.max(0,v.seekable.end(v.seekable.length-1)-1)}"
                 "if(v.paused)v.play()";
        }
        else return;
        evalIn(t->session, std::string("(()=>{const v=") + kBestVideoJs + ";if(v){" + js + "}})()");
        nextPoll = Clock::now() + std::chrono::milliseconds(120);  // show the change soon
    };
    auto applyMute = [&](const std::string& session) {
        evalIn(session, std::string("(()=>{const M=") + (muted ? "1" : "0") +
                            ";for(const m of document.querySelectorAll('video,audio')){"
                            "if(M&&!m.muted){m.muted=true;m.__pvrM=1}else if(!M&&m.__pvrM){m.muted=false;delete m.__pvrM}}})()");
    };

    // Screen volume: set on the page's media once per change (or when new
    // ones appear), so the site's own volume control still works in between.
    auto applyVolume = [&](const std::string& session) {
        evalIn(session, "(()=>{const V=" + std::to_string(volumePct) +
                            "/100;for(const m of document.querySelectorAll('video,audio')){"
                            "if(m.__pvrV!==V){m.volume=V;m.__pvrV=V}}})()");
    };

    auto toggleFullscreen = [&]() {
        if (const Tab* t = activeTab(); t && pageFullscreen) {
            evalIn(t->session, "document.fullscreenElement&&document.exitFullscreen()");
            return;
        }
        if (opt.kiosk) return;
        userFullscreen = !userFullscreen;
        revealed = false;
        sizeToolbar();
        pushToolbar();
        compose();
    };

    auto probeFocus = [&](const std::string& session, bool click) {
        evalIn(session, kEditableJs, [&, session, click](const pvrjson::Value* v) {
            const bool editable = v && v->type == pvrjson::Value::Type::Object;
            if (session != toolbar) lastPageEditable = editable ? 1 : 0;
            if (!editable) {
                if (click) emitLine("focus c 0");
                return;
            }
            keySession = session;
            emitLine(std::string("focus ") + (click ? "c" : "p") + " 1 " +
                     std::to_string(static_cast<int>(v->getNumber("pw"))) + " " +
                     std::to_string(static_cast<int>(v->getNumber("c"))) + " " + pvrjson::quote(v->getString("v")));
        });
    };

    auto handleToolbar = [&](const std::string& payload) {
        pvrjson::Value m;
        if (!pvrjson::parse(payload, m)) return;
        const std::string t = m.getString("t");
        Tab* tab = activeTab();
        if (t == "nav" && tab) {
            send("Page.navigate", "{\"url\":" + pvrjson::quote(m.getString("u")) + "}", tab->session);
            keySession = tab->session;
            setExpanded(false);
        } else if (t == "back" && tab) {
            evalIn(tab->session, "history.back()");
        } else if (t == "fwd" && tab) {
            evalIn(tab->session, "history.forward()");
        } else if (t == "reload" && tab) {
            send("Page.reload", "{}", tab->session);
        } else if (t == "stop" && tab) {
            send("Page.stopLoading", "{}", tab->session);
        } else if (t == "home" && tab) {
            send("Page.navigate", "{\"url\":" + pvrjson::quote(kHomeUrl) + "}", tab->session);
        } else if (t == "tab") {
            activate(static_cast<int>(m.getNumber("i", -1)));
        } else if (t == "close") {
            closeTab(static_cast<int>(m.getNumber("i", -1)));
        } else if (t == "new") {
            openTab(kHomeUrl, true);
            // Like Chrome: a new tab starts in the address bar.
            keySession = toolbar;
            evalIn(toolbar, "document.getElementById('url').focus()");
            probeAt = Clock::now() + std::chrono::milliseconds(120);
            probeSession = toolbar;
            probeIsClick = true;
        } else if (t == "full") {
            toggleFullscreen();
        } else if (t == "star" && tab) {
            bookmarks.toggle(tab->url, tab->title);
            sizeToolbar();
            pushToolbar();
            compose();
        } else if (t == "menu") {
            const pvrjson::Value* on = m.get("on");
            if (!(on && on->boolean)) selectJson.clear();
            setExpanded(on && on->boolean);
            pushToolbar();
        } else if (t == "pick") {
            if (!selectSession.empty()) {
                evalIn(selectSession, "window.__pvrPick&&__pvrPick(" +
                                          std::to_string(static_cast<int>(m.getNumber("i", -1))) + ")");
            }
            selectJson.clear();
            setExpanded(false);
            pushToolbar();
        } else if (t == "dialog" && dialog) {
            const pvrjson::Value* ok = m.get("ok");
            send("Page.handleJavaScriptDialog",
                 std::string("{\"accept\":") + (ok && ok->boolean ? "true" : "false") +
                     ",\"promptText\":" + pvrjson::quote(m.getString("v")) + "}",
                 dialog->session);
            dialog.reset();
            setExpanded(false);
            pushToolbar();
        } else if (t == "blur") {
            if (keySession == toolbar) {
                emitLine("focus p 0");
                if (tab) keySession = tab->session;
            }
        }
    };

    auto decodeInto = [&](tjhandle jpeg, const std::string& b64, Pixels& out) {
        const std::vector<uint8_t> data = base64Decode(b64);
        int w = 0, h = 0, subsamp = 0, colorspace = 0;
        if (data.empty() || tjDecompressHeader3(jpeg, data.data(), static_cast<unsigned long>(data.size()), &w, &h,
                                                &subsamp, &colorspace) != 0) {
            return false;
        }
        w = std::min(w, W);
        h = std::min(h, kBrowserMaxHeight);
        out.px.resize(static_cast<size_t>(w) * h * 4);
        out.w = w;
        out.h = h;
        return tjDecompress2(jpeg, data.data(), static_cast<unsigned long>(data.size()), out.px.data(), w, w * 4, h,
                             TJPF_RGBA, TJFLAG_FASTDCT) == 0;
    };
    tjhandle jpeg = tjInitDecompress();
    int statFrames = 0;           // page frames since the last "stats"
    long long statDecodeUs = 0;   // time spent decoding + compositing them
    auto statSince = Clock::now();

    auto handleMessage = [&](const std::string& text) {
        pvrjson::Value msg;
        if (!pvrjson::parse(text, msg)) return;
        if (const pvrjson::Value* idv = msg.get("id")) {
            const int id = static_cast<int>(idv->number);
            for (size_t k = 0; k < pending.size(); ++k) {
                if (pending[k].first == id) {
                    auto fn = std::move(pending[k].second);
                    pending.erase(pending.begin() + static_cast<long>(k));
                    if (fn) fn(msg);
                    break;
                }
            }
            return;
        }
        const std::string method = msg.getString("method");
        const std::string session = msg.getString("sessionId");
        const pvrjson::Value* params = msg.get("params");
        if (!params) return;
        if (method == "Page.screencastFrame") {
            send("Page.screencastFrameAck",
                 "{\"sessionId\":" + std::to_string(static_cast<long long>(params->getNumber("sessionId"))) + "}",
                 session);
            const Tab* t = activeTab();
            if (session == toolbar) {
                if (decodeInto(jpeg, params->getString("data"), tbPx)) compose();
            } else if (t && session == t->session) {
                lastPageJpeg = params->getString("data");
                const auto t0 = Clock::now();
                if (decodeInto(jpeg, lastPageJpeg, pagePx)) compose();
                ++statFrames;
                statDecodeUs += std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count();
            }
        } else if (method == "Runtime.bindingCalled" && session == toolbar) {
            handleToolbar(params->getString("payload"));
        } else if (method == "Runtime.bindingCalled" && params->getString("name") == "pvrSelect") {
            const Tab* t = activeTab();
            pvrjson::Value sel;
            if (!t || t->session != session || !pvrjson::parse(params->getString("payload"), sel)) return;
            selectSession = session;
            selectJson = params->getString("payload");
            pushToolbar();
            setExpanded(true);
        } else if (method == "Page.loadEventFired" && session == toolbar) {
            toolbarReady = true;
            pushToolbar(true);
        } else if (method == "Page.javascriptDialogOpening") {
            const int i = tabBySession(session);
            if (i < 0) return;
            activate(i);
            dialog = Dialog{session, params->getString("type"), params->getString("message"),
                            params->getString("defaultPrompt")};
            pushToolbar();
            setExpanded(true);
        } else if (method == "Page.javascriptDialogClosed") {
            if (dialog && dialog->session == session) {
                dialog.reset();
                setExpanded(false);
                pushToolbar();
            }
        } else if (method == "Page.frameStartedLoading" || method == "Page.frameStoppedLoading") {
            const int i = tabBySession(session);
            if (i < 0 || params->getString("frameId") != tabs[i].targetId) return;
            tabs[i].loading = method == "Page.frameStartedLoading";
            if (!tabs[i].loading) requestHistory(session);
            if (i == active) emitActiveState();
            pushToolbar();
        } else if (method == "Page.navigatedWithinDocument") {
            requestHistory(session);
        } else if (method == "Target.targetInfoChanged") {
            const pvrjson::Value* info = params->get("targetInfo");
            const int i = info ? tabByTarget(info->getString("targetId")) : -1;
            if (i < 0) return;
            const std::string url = info->getString("url");
            const std::string title = info->getString("title");
            if (url != tabs[i].url) requestHistory(tabs[i].session);
            tabs[i].url = url;
            tabs[i].title = title;
            if (i == active) emitActiveState();
            pushToolbar();
        } else if (method == "Target.targetCreated") {
            // Popups and target=_blank links from our tabs become tabs here.
            const pvrjson::Value* info = params->get("targetInfo");
            if (!info || info->getString("type") != "page") return;
            const std::string id = info->getString("targetId");
            const std::string opener = info->getString("openerId");
            if (opener.empty() || tabByTarget(opener) < 0 || tabByTarget(id) >= 0) return;
            const int i = attachTab(id, opener);
            if (i >= 0) {
                tabs[i].url = info->getString("url");
                activate(i);
            }
        } else if (method == "Target.targetDestroyed") {
            const int i = tabByTarget(params->getString("targetId"));
            if (i >= 0) removeTab(i);
        } else if (method == "Target.detachedFromTarget") {
            const int i = tabBySession(params->getString("sessionId"));
            if (i >= 0) removeTab(i);
        } else if (method == "Browser.downloadWillBegin") {
            if (tabByTarget(params->getString("frameId")) < 0) return;  // another screen's
            downloads.emplace_back(params->getString("guid"), params->getString("suggestedFilename"));
            downloadText = downloads.back().second;
            downloadClearAt = {};
            pushToolbar();
        } else if (method == "Browser.downloadProgress") {
            const std::string guid = params->getString("guid");
            auto it = std::find_if(downloads.begin(), downloads.end(), [&](const auto& d) { return d.first == guid; });
            if (it == downloads.end()) return;
            const std::string state = params->getString("state");
            const double total = params->getNumber("totalBytes");
            const double got = params->getNumber("receivedBytes");
            if (state == "inProgress") {
                downloadText = it->second + (total > 0 ? "  " + std::to_string(static_cast<int>(100.0 * got / total)) + "%" : "");
            } else {
                downloadText = (state == "completed" ? "Saved: " : "Failed: ") + it->second;
                if (state == "completed") emitLine("download " + it->second);
                downloadClearAt = Clock::now() + std::chrono::seconds(6);
                downloads.erase(it);
            }
            pushToolbar();
        }
    };

    // ---- Toolbar page.
    {
        pvrjson::Value r;
        if (call("Target.createTarget", "{\"url\":\"about:blank\",\"newWindow\":true}", "", r)) {
            toolbarTarget = r.getString("targetId");
            pvrjson::Value a;
            if (call("Target.attachToTarget", "{\"targetId\":" + pvrjson::quote(toolbarTarget) + ",\"flatten\":true}",
                     "", a)) {
                toolbar = a.getString("sessionId");
            }
        }
        if (toolbar.empty()) {
            emitLine("error could not open the toolbar");
            return 4;
        }
        send("Page.enable", "{}", toolbar);
        send("Runtime.enable", "{}", toolbar);
        send("Runtime.addBinding", "{\"name\":\"pvrSend\"}", toolbar);
        send("Emulation.setFocusEmulationEnabled", "{\"enabled\":true}", toolbar);
        sizeToolbar();
        send("Page.startScreencast",
             "{\"format\":\"jpeg\",\"quality\":92,\"maxWidth\":" + std::to_string(W) +
                 ",\"maxHeight\":" + std::to_string(kBrowserMaxHeight) + "}",
             toolbar);
        const std::string html = pvrbrowser::kToolbarHtml;
        send("Page.navigate",
             "{\"url\":" +
                 pvrjson::quote("data:text/html;base64," +
                                base64Encode(reinterpret_cast<const uint8_t*>(html.data()), html.size())) +
                 "}",
             toolbar);
    }
    // ---- First tab.
    if (openTab(opt.url.empty() ? kHomeUrl : opt.url, true) < 0) {
        emitLine("error could not open a Chromium window");
        send("Target.closeTarget", "{\"targetId\":" + pvrjson::quote(toolbarTarget) + "}", "");
        return 4;
    }
    keySession = tabs[active].session;
    emitLine("ready");

    // ---- Commands from the app.
    auto mouse = [&](const std::string& session, const char* type, double x, double y, const char* button, int clicks) {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "{\"type\":\"%s\",\"x\":%.1f,\"y\":%.1f,\"button\":\"%s\",\"buttons\":%d,\"clickCount\":%d}", type, x,
                      y, button, mouseDown ? 1 : 0, clicks);
        send("Input.dispatchMouseEvent", buf, session);
    };
    // Which page is under frame row y, and its own y offset.
    auto routeAt = [&](double y, double& offsetY) -> std::string {
        offsetY = 0.0;
        if (expanded) return toolbar;
        if (barsVisible() && y < barsH()) return toolbar;
        const Tab* t = activeTab();
        offsetY = pageTop();
        return t ? t->session : std::string();
    };
    auto updateReveal = [&](double y) {
        if (opt.kiosk || !userFullscreen || pageFullscreen || mouseDown) return;
        if (!revealed && y < 8.0) {
            revealed = true;
            hideRevealAt = {};
            pushToolbar();
            compose();
        } else if (revealed) {
            if (y <= barsH() + 24) hideRevealAt = {};
            else if (hideRevealAt == Clock::time_point{}) hideRevealAt = Clock::now() + std::chrono::milliseconds(700);
        }
    };
    auto keyEvent = [&](const KeyInfo* k) {
        if (keySession.empty()) return;
        std::string down = std::string("{\"type\":\"keyDown\",\"key\":") + pvrjson::quote(k->key) +
                           ",\"code\":" + pvrjson::quote(k->code) + ",\"windowsVirtualKeyCode\":" + std::to_string(k->vk);
        if (k->text) down += ",\"text\":" + pvrjson::quote(k->text);
        send("Input.dispatchKeyEvent", down + "}", keySession);
        send("Input.dispatchKeyEvent", std::string("{\"type\":\"keyUp\",\"key\":") + pvrjson::quote(k->key) +
                                           ",\"code\":" + pvrjson::quote(k->code) +
                                           ",\"windowsVirtualKeyCode\":" + std::to_string(k->vk) + "}",
             keySession);
    };

    auto handleCommand = [&](const std::string& line) {
        const size_t sp = line.find(' ');
        const std::string cmd = line.substr(0, sp);
        const std::string rest = sp == std::string::npos ? std::string() : line.substr(sp + 1);
        double x = 0, y = 0;
        int d = 0;
        Tab* tab = activeTab();
        if (cmd == "move" && std::sscanf(rest.c_str(), "%lf %lf", &x, &y) == 2) {
            updateReveal(y);
            double off = 0.0;
            const std::string s = mouseDown ? pressSession : routeAt(y, off);
            if (mouseDown) off = pressOffsetY;
            if (!s.empty()) mouse(s, "mouseMoved", x, y - off, mouseDown ? "left" : "none", 0);
        } else if (cmd == "down" && std::sscanf(rest.c_str(), "%lf %lf", &x, &y) == 2) {
            pressSession = routeAt(y, pressOffsetY);
            if (pressSession.empty()) return;
            // Clicking the page leaves the address bar (and the other way round).
            if (pressSession != toolbar && keySession == toolbar) {
                evalIn(toolbar, "document.activeElement&&document.activeElement.blur()");
            }
            mouseDown = true;
            mouse(pressSession, "mousePressed", x, y - pressOffsetY, "left", 1);
        } else if (cmd == "up" && std::sscanf(rest.c_str(), "%lf %lf", &x, &y) == 2) {
            if (!mouseDown) return;
            mouseDown = false;
            mouse(pressSession, "mouseReleased", x, y - pressOffsetY, "left", 1);
            probeSession = pressSession;
            probeIsClick = true;
            probeAt = Clock::now() + std::chrono::milliseconds(150);
        } else if (cmd == "wheel" && std::sscanf(rest.c_str(), "%lf %lf %d", &x, &y, &d) == 3) {
            double off = 0.0;
            const std::string s = routeAt(y, off);
            // Our DY follows wheel angles (positive = up); CDP's deltaY is the
            // scroll amount (positive = down).
            char buf[160];
            std::snprintf(buf, sizeof(buf), "{\"type\":\"mouseWheel\",\"x\":%.1f,\"y\":%.1f,\"deltaX\":0,\"deltaY\":%d}",
                          x, y - off, -d);
            if (!s.empty()) send("Input.dispatchMouseEvent", buf, s);
        } else if (cmd == "key") {
            if (rest == "F11") {
                toggleFullscreen();
            } else if (const KeyInfo* k = keyInfo(rest)) {
                keyEvent(k);
                valueAt = Clock::now() + std::chrono::milliseconds(80);
            }
        } else if (cmd == "text" && !rest.empty()) {
            if (!keySession.empty()) send("Input.insertText", "{\"text\":" + pvrjson::quote(rest) + "}", keySession);
            valueAt = Clock::now() + std::chrono::milliseconds(80);
        } else if (cmd == "nav" && !rest.empty() && tab) {
            std::string target = rest;
            // "scheme://..." or a scheme without slashes (data:, about:, ...) is
            // a URL already; "host:port" is not.
            const size_t colon = target.find(':');
            const std::string scheme = colon == std::string::npos ? std::string() : target.substr(0, colon);
            const bool hasScheme =
                !scheme.empty() && std::all_of(scheme.begin(), scheme.end(), [](char c) { return std::isalpha(static_cast<unsigned char>(c)); }) &&
                (target.compare(colon, 3, "://") == 0 || scheme == "data" || scheme == "about" || scheme == "chrome" ||
                 scheme == "mailto" || scheme == "blob" || scheme == "javascript");
            if (!hasScheme) target = "https://" + target;
            send("Page.navigate", "{\"url\":" + pvrjson::quote(target) + "}", tab->session);
        } else if (cmd == "tab" && !rest.empty()) {
            if (opt.kiosk && tab) send("Page.navigate", "{\"url\":" + pvrjson::quote(rest) + "}", tab->session);
            else openTab(rest, true);
        } else if (cmd == "back" && tab) {
            if (expanded && !dialog) {
                selectJson.clear();
                setExpanded(false);  // B closes an open menu first
                pushToolbar();
            } else if (tab->canBack) {
                evalIn(tab->session, "history.back()");
            } else if (!tab->openerId.empty() && tabs.size() > 1) {
                closeTab(active);  // a popup with nothing to go back to: close it
            }
        } else if (cmd == "forward" && tab) {
            evalIn(tab->session, "history.forward()");
        } else if (cmd == "reload" && tab) {
            send("Page.reload", "{}", tab->session);
        } else if (cmd == "fullscreen") {
            toggleFullscreen();
        } else if (cmd == "media") {
            mediaCommand(rest);
        } else if (cmd == "mute") {
            muted = rest == "1";
            for (const Tab& t : tabs) applyMute(t.session);
        } else if (cmd == "volume") {
            volumePct = std::clamp(std::atoi(rest.c_str()), 0, 100);
            for (const Tab& t : tabs) applyVolume(t.session);
        } else if (cmd == "pausemedia") {
            // Headset off: pause everything, livestreams only when asked to.
            const std::string js = std::string("(()=>{const L=") + (rest == "1" ? "1" : "0") +
                                   ";const live=" + kIsLiveJs +
                                   ";for(const m of document.querySelectorAll('video,audio')){"
                                   "if(!L&&live(m))continue;m.pause()}})()";
            for (const Tab& t : tabs) evalIn(t.session, js);
        } else if (cmd == "keyboard") {
            // The app opened its keyboard by hand: report the field, if any.
            probeSession = keySession.empty() && tab ? tab->session : keySession;
            probeIsClick = false;
            probeAt = Clock::now();
        } else if (cmd == "eval" && !rest.empty() && tab) {
            evalIn(tab->session, rest, [](const pvrjson::Value* v) {
                std::string out = "undefined";
                if (v && v->type == pvrjson::Value::Type::String) out = v->str;
                else if (v) out = pvrjson::dump(*v);
                for (char& c : out) {
                    if (c == '\n') c = ' ';
                }
                emitLine("eval " + out);
            });
        } else if (cmd == "stats") {  // debugging: page frame rate and decode cost
            const double secs = std::chrono::duration<double>(Clock::now() - statSince).count();
            char b[128];
            std::snprintf(b, sizeof(b), "eval stats %.1f fps, %.1f ms per frame", statFrames / std::max(secs, 0.001),
                          statFrames ? statDecodeUs / 1000.0 / statFrames : 0.0);
            emitLine(b);
            statFrames = 0;
            statDecodeUs = 0;
            statSince = Clock::now();
        } else if (cmd == "gpuinfo") {  // debugging: Chromium's GPU / video decode status
            sendThen("SystemInfo.getInfo", "{}", "", [](const pvrjson::Value& msg) {
                const pvrjson::Value* r = msg.get("result");
                const pvrjson::Value* gpu = r ? r->get("gpu") : nullptr;
                if (!gpu) {
                    emitLine("eval no gpu info");
                    return;
                }
                if (const pvrjson::Value* f = gpu->get("featureStatus")) emitLine("eval features " + pvrjson::dump(*f));
                if (const pvrjson::Value* d = gpu->get("devices")) emitLine("eval devices " + pvrjson::dump(*d));
                if (const pvrjson::Value* v = gpu->get("videoDecoding")) emitLine("eval decode " + pvrjson::dump(*v));
            });
        } else if (cmd == "tbeval" && !rest.empty()) {  // debugging the toolbar page
            evalIn(toolbar, rest, [](const pvrjson::Value* v) {
                emitLine("eval " + (v ? (v->type == pvrjson::Value::Type::String ? v->str : pvrjson::dump(*v))
                                      : std::string("undefined")));
            });
        } else if (cmd == "quit") {
            gStop = 1;
        }
    };

    // ---- Periodic page state: fullscreen, focused field, media.
    auto pollState = [&]() {
        if (!toolbarReady && !toolbar.empty()) {
            // The load event can pass while a tab is being set up; ask directly.
            evalIn(toolbar, "typeof window.pvr", [&](const pvrjson::Value* v) {
                if (toolbarReady || !v || v->str != "object") return;
                toolbarReady = true;
                pushToolbar(true);
                compose();
            });
        }
        const Tab* t = activeTab();
        if (!t) return;
        const std::string session = t->session;
        const std::string js = std::string("(()=>{const M=") + (muted ? "1" : "0") +
                               ";for(const m of document.querySelectorAll('video,audio')){"
                               "if(M&&!m.muted){m.muted=true;m.__pvrM=1}else if(!M&&m.__pvrM){m.muted=false;delete m.__pvrM}"
                               "if(m.__pvrV!==" + std::to_string(volumePct) + "/100){m.volume=" + std::to_string(volumePct) +
                               "/100;m.__pvrV=m.volume}}"
                               "const v=" + kBestVideoJs +
                               ";let md=null;if(v){const live=" + kIsLiveJs + "(v);"
                               "const s=v.seekable,n=s.length;let s0=n?s.start(0):0,s1=n?s.end(n-1):0;"
                               // YouTube's live media ends an hour past the live edge; its
                               // player knows the real rewind window (in its own clock).
                               "const yp=document.getElementById('movie_player');"
                               "if(live&&yp&&yp.getProgressState){try{const p=yp.getProgressState(),o=p.current-v.currentTime;"
                               "if(isFinite(p.seekableStart)&&isFinite(p.seekableEnd)&&isFinite(o)){s0=p.seekableStart-o;"
                               "s1=p.seekableEnd-o}}catch(_){}}"
                               "md=[v.paused?1:0,v.currentTime||0,isFinite(v.duration)?v.duration:0,live?1:0,s0,s1]}"
                               // Video details for the stats overlay: codec (YouTube's
                               // player knows it), size, decoded / dropped frames.
                               "let vi=null;if(v){const q=v.getVideoPlaybackQuality?v.getVideoPlaybackQuality():null;"
                               "let c='';try{const mp=document.getElementById('movie_player');if(mp&&mp.getStatsForNerds)c=mp.getStatsForNerds().codecs||''}catch(_){}"
                               "vi=[v.videoWidth,v.videoHeight,q?q.totalVideoFrames:0,q?q.droppedVideoFrames:0,c]}"
                               "return {f:document.fullscreenElement?1:0,e:" + kEditableJs + "?1:0,m:md,vi:vi}})()";
        evalIn(session, js, [&, session](const pvrjson::Value* v) {
            const Tab* cur = activeTab();
            if (!v || !cur || cur->session != session) return;
            const bool fs = v->getNumber("f") != 0;
            if (fs != pageFullscreen) {
                pageFullscreen = fs;
                revealed = false;
                sizeToolbar();
                pushToolbar();
                compose();
            }
            const int editable = static_cast<int>(v->getNumber("e"));
            if (lastPageEditable && !editable && keySession == session) emitLine("focus p 0");
            lastPageEditable = editable;
            std::string media = "media 0";
            const pvrjson::Value* m = v->get("m");
            if (m && m->type == pvrjson::Value::Type::Array && m->array.size() == 6) {
                char b[160];
                std::snprintf(b, sizeof(b), "media 1 %d %.2f %.2f %d %.2f %.2f", static_cast<int>(m->array[0].number),
                              m->array[1].number, m->array[2].number, static_cast<int>(m->array[3].number),
                              m->array[4].number, m->array[5].number);
                media = b;
            }
            if (media != lastMedia) emitLine(lastMedia = media);
            const pvrjson::Value* vi = v->get("vi");
            if (vi && vi->type == pvrjson::Value::Type::Array && vi->array.size() == 5) {
                char b[96];
                std::snprintf(b, sizeof(b), "videoinfo %d %d %d %d ", static_cast<int>(vi->array[0].number),
                              static_cast<int>(vi->array[1].number), static_cast<int>(vi->array[2].number),
                              static_cast<int>(vi->array[3].number));
                const std::string line = b + pvrjson::quote(vi->array[4].str);
                if (line != lastVideoInfo) emitLine(lastVideoInfo = line);
            }
        });
    };

    int rc = 0;
    std::string pendingIn;
    while (!gStop) {
        for (size_t k = 0; k < backlog.size(); ++k) {
            const std::string m = std::move(backlog[k]);
            handleMessage(m);
        }
        backlog.clear();
        pollfd fds[2] = {{STDIN_FILENO, POLLIN, 0}, {ws.fd(), POLLIN, 0}};
        poll(fds, 2, 50);
        if (fds[0].revents & (POLLIN | POLLHUP)) {
            char buf[8192];
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
        const auto now = Clock::now();
        if (now >= nextPoll) {
            pollState();
            nextPoll = now + std::chrono::milliseconds(400);
        }
        if (probeAt != Clock::time_point{} && now >= probeAt) {
            probeAt = {};
            if (!probeSession.empty()) probeFocus(probeSession, probeIsClick);
        }
        if (valueAt != Clock::time_point{} && now >= valueAt && !keySession.empty()) {
            valueAt = {};
            evalIn(keySession, kEditableJs, [&](const pvrjson::Value* v) {
                if (!v || v->type != pvrjson::Value::Type::Object) return;
                emitLine("value " + std::to_string(static_cast<int>(v->getNumber("c"))) + " " +
                         pvrjson::quote(v->getString("v")));
            });
        }
        if (revealed && hideRevealAt != Clock::time_point{} && now >= hideRevealAt && !expanded) {
            revealed = false;
            hideRevealAt = {};
            pushToolbar();
            compose();
        }
        if (downloadClearAt != Clock::time_point{} && now >= downloadClearAt) {
            downloadClearAt = {};
            downloadText.clear();
            pushToolbar();
        }
        if (now >= nextBookmarkCheck) {
            nextBookmarkCheck = now + std::chrono::seconds(3);
            if (bookmarks.refresh()) {
                sizeToolbar();
                pushToolbar();
                compose();
            }
        }
    }

    tjDestroy(jpeg);
    if (rc == 0) {
        // Our windows only; the launcher owns Chromium.
        for (const Tab& t : tabs) send("Target.closeTarget", "{\"targetId\":" + pvrjson::quote(t.targetId) + "}", "");
        send("Target.closeTarget", "{\"targetId\":" + pvrjson::quote(toolbarTarget) + "}", "");
        if (!contextId.empty()) {
            send("Target.disposeBrowserContext", "{\"browserContextId\":" + pvrjson::quote(contextId) + "}", "");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
    munmap(shmMem, shmSize);
    return rc;
}

}  // namespace

int main(int argc, char** argv) {
    ScreenOptions opt;
    opt.url = "https://www.youtube.com";
    bool launch = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--launch") launch = true;
        else if (a == "--kiosk") opt.kiosk = true;
        else if (a == "--incognito") opt.incognito = true;
        else if (i + 1 >= argc) break;
        else if (a == "--shm") opt.shmName = argv[++i];
        else if (a == "--url") opt.url = argv[++i];
        else if (a == "--width") opt.width = std::atoi(argv[++i]);
        else if (a == "--height") opt.height = std::atoi(argv[++i]);
        else if (a == "--port") opt.port = std::atoi(argv[++i]);
        else if (a == "--profile") opt.profile = argv[++i];
    }
    opt.width = std::clamp(opt.width, 320, kBrowserMaxWidth);
    // Room for the toolbar above the page.
    opt.height = std::clamp(opt.height, 240, kBrowserMaxHeight - (kTabsH + kNavH + kBookmarksH));
    std::signal(SIGTERM, onSignal);
    std::signal(SIGINT, onSignal);
    std::signal(SIGPIPE, SIG_IGN);
    if (launch) return runLauncher(opt.width, opt.height);
    return runScreen(opt);
}

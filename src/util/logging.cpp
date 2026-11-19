#include "util/logging.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>

namespace pixelvr::log {

namespace {

std::atomic<Level> g_level{
#ifdef NDEBUG
    Level::Info
#else
    Level::Debug
#endif
};

std::mutex g_mutex;

const char* level_tag(Level level) {
    switch (level) {
        case Level::Trace: return "TRACE";
        case Level::Debug: return "DEBUG";
        case Level::Info:  return "INFO ";
        case Level::Warn:  return "WARN ";
        case Level::Error: return "ERROR";
        default:           return "?????";
    }
}

// Reads PIXELVR_LOG once on first use to allow overriding the threshold.
Level resolve_initial_level() {
    const char* env = std::getenv("PIXELVR_LOG");
    if (env == nullptr) {
        return g_level.load(std::memory_order_relaxed);
    }
    if (std::strcmp(env, "trace") == 0) return Level::Trace;
    if (std::strcmp(env, "debug") == 0) return Level::Debug;
    if (std::strcmp(env, "info") == 0)  return Level::Info;
    if (std::strcmp(env, "warn") == 0)  return Level::Warn;
    if (std::strcmp(env, "error") == 0) return Level::Error;
    if (std::strcmp(env, "none") == 0)  return Level::None;
    return g_level.load(std::memory_order_relaxed);
}

std::once_flag g_env_once;
std::atomic<Redactor> g_redactor{nullptr};

// Private local folders (set_private_roots). Copied out under the lock, so the filter never holds
// it while it works; `g_have_roots` keeps the common case (no vault) to one atomic load.
std::mutex g_roots_mutex;
std::mutex g_roots_change_mutex;  // (one change at a time: two add_private_root calls never lose one)
// Never destroyed: detached threads (FFmpeg, web-play wake-up, subtitle catch-up) may still log during exit.
std::shared_ptr<const std::vector<std::string>>& roots_ref() {
    static auto* p = new std::shared_ptr<const std::vector<std::string>>();
    return *p;
}
std::atomic<bool> g_have_roots{false};

// The whole log's budget: a burst of startup lines passes, a flood does not (a broken hardware
// decoder once wrote 35,000 FFmpeg lines a second, 39 MB, while the headset starved). g_mutex.
constexpr double kLinesPerSec = 200.0;
constexpr double kBytesPerSec = 64.0 * 1024.0;
constexpr double kBurstLines = 2000.0;
constexpr double kBurstBytes = 512.0 * 1024.0;
constexpr std::size_t kMaxLineBytes = 8192;  // (cut after the filters, so a cut never splits what they hide)
double g_tokens_lines = kBurstLines;
double g_tokens_bytes = kBurstBytes;
int64_t g_tokens_at_ns = 0;
uint64_t g_suppressed = 0;

// Plex media URLs carry the account token; never write it out.
std::string redact_token(std::string_view in) {
    // Plex and Jellyfin / Emby account tokens in URLs and auth headers.
    std::string s(in);
    for (const std::string_view kTok : {std::string_view("X-Plex-Token="), std::string_view("api_key="),
                                        std::string_view("ApiKey="), std::string_view("Token=\"")}) {
        for (std::size_t p = 0; (p = s.find(kTok, p)) != std::string::npos;) {
            p += kTok.size();
            std::size_t e = p;
            while (e < s.size() && s[e] != '&' && s[e] != '"' && s[e] != ' ' && s[e] != '\n' && s[e] != '\\') ++e;
            s.replace(p, e - p, "***");
            p += 3;
        }
    }
    return s;
}

// A character that can continue a file or folder name ("/vault" must not hide "/vaults").
bool name_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
           c == '.' || static_cast<unsigned char>(c) >= 0x80;
}

std::string normalized_root(std::string r) {
    while (r.size() > 1 && r.back() == '/') r.pop_back();
    return r;
}

// Same rule as the vault-share filter: from the path to its closing quote when it is quoted, else to
// the end of the line (a file name may hold spaces, so nothing short of that is safe).
void redact_private_paths(std::string& s, const std::vector<std::string>& roots) {
    for (const std::string& r : roots) {
        for (std::size_t p = 0; (p = s.find(r, p)) != std::string::npos;) {
            const std::size_t after = p + r.size();
            if (after < s.size() && name_char(s[after])) {
                p = after;
                continue;
            }
            const char before = p > 0 ? s[p - 1] : '\0';
            std::size_t end = s.size();
            if (before == '"' || before == '\'') {
                const std::size_t q = s.find(before, after);
                if (q != std::string::npos) end = q;
            }
            s.replace(p, end - p, "(private)");
            p += 9;
        }
    }
}

std::shared_ptr<const std::vector<std::string>> roots_snapshot() {
    if (!g_have_roots.load(std::memory_order_acquire)) return nullptr;
    std::lock_guard<std::mutex> lock(g_roots_mutex);
    return roots_ref();
}

// The list with each root once, and also as a file:// URL spells it (spaces as %20).
void store_roots(std::vector<std::string> list) {
    std::vector<std::string> out;
    for (std::string& r0 : list) {
        const std::string r = normalized_root(std::move(r0));
        if (r.size() < 2 || r[0] != '/') continue;  // never "/" (that would hide every path)
        std::string enc;
        for (const char c : r) {
            const unsigned char u = static_cast<unsigned char>(c);
            if (c == ' ' || u >= 0x80) {  // (a non-ASCII folder name is %-encoded byte by byte)
                static const char kHex[] = "0123456789ABCDEF";
                enc += '%';
                enc += kHex[u >> 4];
                enc += kHex[u & 0x0f];
            } else {
                enc += c;
            }
        }
        auto keep = [&out](const std::string& v) {
            if (std::find(out.begin(), out.end(), v) == out.end()) out.push_back(v);
        };
        keep(r);
        keep(enc);
    }
    // Longest first: a root inside another one is then hidden as a whole.
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
    std::shared_ptr<const std::vector<std::string>> fresh = std::make_shared<std::vector<std::string>>(std::move(out));
    const bool any = !fresh->empty();
    std::lock_guard<std::mutex> lock(g_roots_mutex);
    roots_ref() = std::move(fresh);
    g_have_roots.store(any, std::memory_order_release);
}

int64_t steady_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

void set_redactor(Redactor fn) { g_redactor.store(fn, std::memory_order_release); }

void set_private_roots(const std::vector<std::string>& roots) {
    std::lock_guard<std::mutex> change(g_roots_change_mutex);
    store_roots(roots);
}

void add_private_root(const std::string& root) {
    std::lock_guard<std::mutex> change(g_roots_change_mutex);
    std::vector<std::string> list;
    {
        std::lock_guard<std::mutex> lock(g_roots_mutex);
        if (roots_ref()) list = *roots_ref();
    }
    list.push_back(root);
    store_roots(std::move(list));
}

bool is_private_path(std::string_view path) {
    const auto roots = roots_snapshot();
    if (!roots) return false;
    for (const std::string& r : *roots) {
        if (path.size() >= r.size() && path.compare(0, r.size(), r) == 0 &&
            (path.size() == r.size() || path[r.size()] == '/')) {
            return true;
        }
    }
    return false;
}

void set_level(Level level) {
    g_level.store(level, std::memory_order_relaxed);
}

Level level() {
    std::call_once(g_env_once, [] { g_level.store(resolve_initial_level()); });
    return g_level.load(std::memory_order_relaxed);
}

void write(Level msg_level, std::string_view message) {
    if (static_cast<int>(msg_level) < static_cast<int>(level())) {
        return;
    }

    std::timespec ts{};
    std::timespec_get(&ts, TIME_UTC);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &ts.tv_sec);
#else
    localtime_r(&ts.tv_sec, &tm);
#endif
    char stamp[32];
    std::snprintf(stamp, sizeof(stamp), "%02d:%02d:%02d.%03ld", tm.tm_hour,
                  tm.tm_min, tm.tm_sec, ts.tv_nsec / 1'000'000);

    std::string clean = redact_token(message);
    if (const Redactor r = g_redactor.load(std::memory_order_acquire)) clean = r(clean);
    if (const auto roots = roots_snapshot()) redact_private_paths(clean, *roots);
    if (clean.size() > kMaxLineBytes) {
        clean.resize(kMaxLineBytes);
        clean += " [...]";
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    const int64_t now = steady_ns();
    if (g_tokens_at_ns != 0) {
        const double sec = static_cast<double>(now - g_tokens_at_ns) / 1e9;
        g_tokens_lines = std::min(kBurstLines, g_tokens_lines + sec * kLinesPerSec);
        g_tokens_bytes = std::min(kBurstBytes, g_tokens_bytes + sec * kBytesPerSec);
    }
    g_tokens_at_ns = now;
    const double bytes = static_cast<double>(clean.size() + 24);
    if (g_tokens_lines < 1.0 || g_tokens_bytes < bytes) {
        ++g_suppressed;  // (said once the budget allows a line again)
        return;
    }
    g_tokens_lines -= 1.0;
    g_tokens_bytes -= bytes;
    if (g_suppressed > 0) {
        std::fprintf(stderr, "[%s] %s  %llu log lines were not written (too many a second)\n", stamp,
                     level_tag(Level::Warn), static_cast<unsigned long long>(g_suppressed));
        g_suppressed = 0;
    }
    std::fprintf(stderr, "[%s] %s  %.*s\n", stamp, level_tag(msg_level),
                 static_cast<int>(clean.size()), clean.data());
}

void writef(Level msg_level, const char* fmt, ...) {
    if (static_cast<int>(msg_level) < static_cast<int>(level())) {
        return;
    }

    // The whole line, however long: the filters must see all of an address before anything is cut
    // (a cut inside a share name would leave the rest of the name unrecognised).
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    va_list again;
    va_copy(again, args);
    const int n = std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    if (n < 0) {
        va_end(again);
        return;
    }
    if (static_cast<std::size_t>(n) < sizeof(buffer)) {
        va_end(again);
        write(msg_level, std::string_view(buffer, static_cast<std::size_t>(n)));
        return;
    }
    std::string big(static_cast<std::size_t>(n) + 1, '\0');
    std::vsnprintf(big.data(), big.size(), fmt, again);
    va_end(again);
    big.resize(static_cast<std::size_t>(n));
    write(msg_level, big);
}

} // namespace pixelvr::log

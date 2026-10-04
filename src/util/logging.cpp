#include "util/logging.hpp"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>

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

} // namespace

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

    std::lock_guard<std::mutex> lock(g_mutex);
    std::fprintf(stderr, "[%s] %s  %.*s\n", stamp, level_tag(msg_level),
                 static_cast<int>(message.size()), message.data());
}

void writef(Level msg_level, const char* fmt, ...) {
    if (static_cast<int>(msg_level) < static_cast<int>(level())) {
        return;
    }

    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    const int n = std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    if (n < 0) {
        return;
    }
    const std::size_t len =
        (static_cast<std::size_t>(n) < sizeof(buffer)) ? static_cast<std::size_t>(n)
                                                       : sizeof(buffer) - 1;
    write(msg_level, std::string_view(buffer, len));
}

} // namespace pixelvr::log

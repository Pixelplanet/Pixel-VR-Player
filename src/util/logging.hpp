#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace pixelvr::log {

enum class Level : int {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
    None = 5,
};

// Threshold below which messages are dropped. Defaults to Info (or Debug in
// debug builds). May be overridden at runtime via the PIXELVR_LOG env var
// (trace|debug|info|warn|error).
void set_level(Level level);
Level level();

// Thread-safe; writes one timestamped line to stderr.
void write(Level level, std::string_view message);

// printf-style convenience. Kept out of the header hot path via a single vararg
// sink in the .cpp.
[[gnu::format(printf, 2, 3)]] void writef(Level level, const char* fmt, ...);

} // namespace pixelvr::log

#define PIXELVR_LOG_TRACE(...) ::pixelvr::log::writef(::pixelvr::log::Level::Trace, __VA_ARGS__)
#define PIXELVR_LOG_DEBUG(...) ::pixelvr::log::writef(::pixelvr::log::Level::Debug, __VA_ARGS__)
#define PIXELVR_LOG_INFO(...)  ::pixelvr::log::writef(::pixelvr::log::Level::Info, __VA_ARGS__)
#define PIXELVR_LOG_WARN(...)  ::pixelvr::log::writef(::pixelvr::log::Level::Warn, __VA_ARGS__)
#define PIXELVR_LOG_ERROR(...) ::pixelvr::log::writef(::pixelvr::log::Level::Error, __VA_ARGS__)

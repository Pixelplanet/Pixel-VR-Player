#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

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

// Thread-safe; writes one timestamped line to stderr. Every line passes the filters below first,
// and the whole log is held to a few hundred lines (64 KiB) a second, so no flood from any source
// can grow it by megabytes a second (what was held back is counted in one line afterwards).
void write(Level level, std::string_view message);

// A filter every line passes before it is written (e.g. addresses of vault shares replaced): set
// once at start-up. It must not log itself.
using Redactor = std::string (*)(std::string_view);
void set_redactor(Redactor fn);

// Local folders whose contents are private (the vault folder, its .incoming downloads, the vault on
// the SD card). In every line, a path that starts with one of them is replaced with "(private)" up to
// its closing quote, or else to the end of the line: one rule for every place that logs a path.
// set_private_roots replaces the list; add_private_root adds one. Call them at start-up and whenever
// the vault moves (any thread; never from inside a filter).
void set_private_roots(const std::vector<std::string>& roots);
void add_private_root(const std::string& root);
// True when `path` lies in (or is) one of those folders.
bool is_private_path(std::string_view path);

// printf-style convenience. Kept out of the header hot path via a single vararg
// sink in the .cpp.
[[gnu::format(printf, 2, 3)]] void writef(Level level, const char* fmt, ...);

} // namespace pixelvr::log

#define PIXELVR_LOG_TRACE(...) ::pixelvr::log::writef(::pixelvr::log::Level::Trace, __VA_ARGS__)
#define PIXELVR_LOG_DEBUG(...) ::pixelvr::log::writef(::pixelvr::log::Level::Debug, __VA_ARGS__)
#define PIXELVR_LOG_INFO(...)  ::pixelvr::log::writef(::pixelvr::log::Level::Info, __VA_ARGS__)
#define PIXELVR_LOG_WARN(...)  ::pixelvr::log::writef(::pixelvr::log::Level::Warn, __VA_ARGS__)
#define PIXELVR_LOG_ERROR(...) ::pixelvr::log::writef(::pixelvr::log::Level::Error, __VA_ARGS__)

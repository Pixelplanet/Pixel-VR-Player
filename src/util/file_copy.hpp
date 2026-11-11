// Plain file copies that are on disk before they report success (fsync of the file and its folder), and
// collision-free names. Used where the source is deleted afterwards (moves, vault export).
#pragma once

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace pixelvr::filecopy {

inline void syncDir(const std::string& dir) {
    const int fd = ::open(dir.empty() ? "." : dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return;
    ::fsync(fd);
    ::close(fd);
}

// Copies src to dst through dst + ".part"; fsync, size check, rename, folder fsync. progress(done,total);
// cancelled() true = stop (nothing is left behind).
inline bool copyFileSynced(const std::string& src, const std::string& dst, std::string& err,
                           const std::function<void(uint64_t, uint64_t)>& progress = {},
                           const std::function<bool()>& cancelled = {}) {
    namespace fs = std::filesystem;
    const int in = ::open(src.c_str(), O_RDONLY | O_CLOEXEC);
    if (in < 0) {
        err = "cannot open " + fs::path(src).filename().string();
        return false;
    }
    struct stat st {};
    ::fstat(in, &st);
    const uint64_t total = static_cast<uint64_t>(st.st_size);
    const std::string tmp = dst + ".part";
    const int out = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, (st.st_mode & 0777) | 0600);
    if (out < 0) {
        ::close(in);
        err = "cannot write " + fs::path(dst).filename().string();
        return false;
    }
    std::vector<char> buf(4u << 20);
    uint64_t done = 0;
    bool ok = true, stopped = false;
    for (;;) {
        if (cancelled && cancelled()) {
            ok = false;
            stopped = true;
            break;
        }
        const ssize_t r = ::read(in, buf.data(), buf.size());
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) ok = false;
        if (r <= 0) break;
        for (ssize_t off = 0; off < r;) {
            const ssize_t w = ::write(out, buf.data() + off, static_cast<size_t>(r - off));
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) {
                ok = false;
                break;
            }
            off += w;
        }
        if (!ok) break;
        done += static_cast<uint64_t>(r);
        if (progress) progress(done, total);
    }
    ok = ok && done == total && ::fsync(out) == 0;
    ::close(in);
    ::close(out);
    std::error_code ec;
    if (ok && fs::file_size(tmp, ec) != total) ok = false;
    if (ok) fs::rename(tmp, dst, ec);
    if (!ok || ec) {
        fs::remove(tmp, ec);
        err = stopped ? "stopped" : "copying failed (disk full?)";
        return false;
    }
    syncDir(fs::path(dst).parent_path().string());
    return true;
}

// "<dir>/<name>", or "<stem>_N<ext>" (folders: "<name>_N") when that exists already. `taken` lets the
// caller add names that exist in another form (e.g. the real names of encrypted files).
inline std::string freeName(const std::string& dir, const std::string& name, bool isDir,
                            const std::function<bool(const std::string&)>& taken = {}) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path p(name);
    const std::string stem = isDir ? name : p.stem().string();
    const std::string ext = isDir ? std::string() : p.extension().string();
    std::string cand = name;
    for (int n = 1; fs::exists(fs::path(dir) / cand, ec) || (taken && taken(cand)); ++n) {
        cand = stem + "_" + std::to_string(n) + ext;
    }
    return cand;
}

// Same file system (a rename moves instantly; otherwise copy + delete).
inline bool sameDevice(const std::string& a, const std::string& b) {
    struct stat sa {}, sb {};
    return ::stat(a.c_str(), &sa) == 0 && ::stat(b.c_str(), &sb) == 0 && sa.st_dev == sb.st_dev;
}

}  // namespace pixelvr::filecopy

// Plain file copies that are on disk before they report success (fsync of the file and its folder), and
// collision-free names. Used where the source is deleted afterwards (moves, vault export).
#pragma once

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace pixelvr::filecopy {

// A temp file next to `dst` that nobody else uses: hidden (".<name>.<random>.pvtmp", so the browser
// skips it), created with O_EXCL. Returns the fd (-1 on failure) and the name.
inline int openUniqueTemp(const std::string& dst, std::string& tmp, mode_t mode) {
    namespace fs = std::filesystem;
    const fs::path p(dst);
    static thread_local unsigned seq = 0;
    for (int attempt = 0; attempt < 50; ++attempt) {
        char tag[40];
        std::snprintf(tag, sizeof tag, "%x%x%x", static_cast<unsigned>(::getpid()), ++seq,
                      static_cast<unsigned>(reinterpret_cast<uintptr_t>(&tmp) >> 4) + static_cast<unsigned>(attempt));
        tmp = (p.parent_path() / ("." + p.filename().string() + "." + tag + ".pvtmp")).string();
        const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode);
        if (fd >= 0 || errno != EEXIST) return fd;
    }
    return -1;
}

// rename() that never replaces an existing file (a download finishing under the same name).
inline bool renameNoReplace(const std::string& from, const std::string& to) {
    if (::renameat2(AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), RENAME_NOREPLACE) == 0) return true;
    if (errno != EINVAL && errno != ENOSYS) return false;  // EEXIST: someone else's file is there
    // File systems without RENAME_NOREPLACE (exFAT / FAT on cards): check, then rename.
    struct stat st {};
    if (::lstat(to.c_str(), &st) == 0) return false;
    return ::rename(from.c_str(), to.c_str()) == 0;
}

inline void syncFile(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    ::fsync(fd);
    ::close(fd);
}

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
    std::string tmp;
    const int out = openUniqueTemp(dst, tmp, (st.st_mode & 0777) | 0600);
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
        if (done % (32u << 20) < static_cast<uint64_t>(r)) ::fdatasync(out);  // no GBs of unwritten data
        if (progress) progress(done, total);
    }
    ok = ok && done == total && ::fsync(out) == 0;
    ::close(in);
    ::close(out);
    std::error_code ec;
    if (ok && fs::file_size(tmp, ec) != total) ok = false;
    const bool renamed = ok && renameNoReplace(tmp, dst);
    if (!renamed) {
        fs::remove(tmp, ec);
        err = stopped ? "stopped" : ok ? "a file with that name appeared meanwhile" : "copying failed (disk full?)";
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
    // "<name>.part" is someone's download in progress: that name is taken too.
    for (int n = 1; fs::exists(fs::path(dir) / cand, ec) || fs::exists(fs::path(dir) / (cand + ".part"), ec) ||
                    (taken && taken(cand));
         ++n) {
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

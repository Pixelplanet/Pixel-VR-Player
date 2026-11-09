#include "plugins/plugin_files.hpp"

#include "net/remote_fs.hpp"
#include "util/smb_lock.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <unistd.h>
#include <fnmatch.h>
#include <fstream>
#include <iterator>
#include <mutex>

#ifdef PIXELVR_HAVE_SMB
#include "net/smb_credentials.hpp"

#include <cstring>
#include <fcntl.h>
#include <libsmbclient.h>
#endif

namespace pixelvr::pluginfiles {

namespace fs = std::filesystem;

#ifdef PIXELVR_HAVE_SMB
namespace {
// Own context per job, like MediaEngine: the global smbc_init() state only
// exists once the file browser has browsed SMB.
void smbAuth(SMBCCTX*, const char* srv, const char* shr, char* wg, int wglen, char* un, int unlen, char* pw, int pwlen) {
    std::string u, p, w;
    if (srv != nullptr && SmbCredentialsManager::instance().findCredentials(srv, shr ? shr : "", u, p, w)) {
        if (un && unlen > 0) {
            std::strncpy(un, u.c_str(), unlen - 1);
            un[unlen - 1] = '\0';
        }
        if (pw && pwlen > 0) {
            std::strncpy(pw, p.c_str(), pwlen - 1);
            pw[pwlen - 1] = '\0';
        }
        if (wg && wglen > 0 && !w.empty()) {
            std::strncpy(wg, w.c_str(), wglen - 1);
            wg[wglen - 1] = '\0';
        }
        return;
    }
    if (un != nullptr && unlen > 0) un[0] = '\0';  // guest / anonymous
    if (pw != nullptr && pwlen > 0) pw[0] = '\0';
}

struct SmbContext {
    SMBCCTX* ctx = nullptr;
    SmbContext() {
        std::lock_guard<std::mutex> lk(smbGlobalLock());
        ctx = smbc_new_context();
        if (!ctx) return;
        smbc_setFunctionAuthDataWithContext(ctx, &smbAuth);
        if (smbc_init_context(ctx) == nullptr) {
            smbc_free_context(ctx, 1);
            ctx = nullptr;
        }
    }
    ~SmbContext() {
        if (!ctx) return;
        std::lock_guard<std::mutex> lk(smbGlobalLock());
        smbc_free_context(ctx, 1);
    }
};
}  // namespace
#endif

namespace {
// Network URIs may carry a query (tokens); local file names may contain '?' or '#'.
std::string withoutQuery(const std::string& path) {
    const size_t sch = path.find("://");
    if (sch == std::string::npos) return path;
    const size_t q = path.find_first_of("?#", sch + 3);
    return q == std::string::npos ? path : path.substr(0, q);
}
}  // namespace

std::string parentOf(const std::string& path) {
    const std::string p = withoutQuery(path);
    const size_t slash = p.find_last_of('/');
    return slash == std::string::npos ? std::string() : p.substr(0, slash);
}

std::string baseNameOf(const std::string& path) {
    std::string name = withoutQuery(path);
    name = name.substr(name.find_last_of('/') + 1);
    const size_t dot = name.find_last_of('.');
    return dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
}

std::string join(const std::string& dir, const std::string& name) {
    return dir.empty() || dir.back() == '/' ? dir + name : dir + "/" + name;
}

bool listFiles(const std::string& dir, std::vector<std::string>& names, std::string& err) {
    names.clear();
    if (dir.rfind("smb://", 0) == 0) {
#ifdef PIXELVR_HAVE_SMB
        SmbContext c;
        SMBCFILE* d = nullptr;
        if (c.ctx) {
            std::lock_guard<std::mutex> lk(smbGlobalLock());
            d = smbc_getFunctionOpendir(c.ctx)(c.ctx, dir.c_str());
        }
        if (!d) {
            err = "cannot list the folder";
            return false;
        }
        for (;;) {
            std::lock_guard<std::mutex> lk(smbGlobalLock());
            struct smbc_dirent* de = smbc_getFunctionReaddir(c.ctx)(c.ctx, d);
            if (!de) break;
            if (de->smbc_type == SMBC_FILE) names.emplace_back(de->name);
        }
        std::lock_guard<std::mutex> lk(smbGlobalLock());
        smbc_getFunctionClosedir(c.ctx)(c.ctx, d);
        return true;
#else
        err = "SMB support not built";
        return false;
#endif
    }
    if (isRemoteFsPath(dir)) {
        std::vector<RemoteEntry> list;
        if (!remoteList(dir, list, err)) return false;
        for (const RemoteEntry& e : list) {
            if (!e.dir && !e.name.empty()) names.push_back(e.name);
        }
        return true;
    }
    if (dir.find("://") != std::string::npos) {
        err = "not a folder";
        return false;
    }
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (e.symlink_status(ec).type() == fs::file_type::regular) names.push_back(e.path().filename().string());  // links are never sidecars
    }
    if (ec) err = ec.message();
    return !ec;
}

bool readFile(const std::string& uri, std::size_t maxBytes, std::string& out, std::string& err) {
    out.clear();
    if (uri.rfind("smb://", 0) == 0) {
#ifdef PIXELVR_HAVE_SMB
        SmbContext c;
        SMBCFILE* fd = nullptr;
        if (c.ctx) {
            std::lock_guard<std::mutex> lk(smbGlobalLock());
            fd = smbc_getFunctionOpen(c.ctx)(c.ctx, uri.c_str(), O_RDONLY, 0);
        }
        if (!fd) {
            err = "cannot open the file";
            return false;
        }
        char buf[65536];
        bool ok = true;
        for (;;) {
            ssize_t n = 0;
            {
                std::lock_guard<std::mutex> lk(smbGlobalLock());
                n = smbc_getFunctionRead(c.ctx)(c.ctx, fd, buf, sizeof(buf));
            }
            if (n < 0) {
                ok = false;
                err = "read error";
                break;
            }
            if (n == 0) break;
            out.append(buf, static_cast<size_t>(n));
            if (out.size() > maxBytes) {
                ok = false;
                err = "file too large";
                break;
            }
        }
        std::lock_guard<std::mutex> lk(smbGlobalLock());
        smbc_getFunctionClose(c.ctx)(c.ctx, fd);
        return ok;
#else
        err = "SMB support not built";
        return false;
#endif
    }
    if (isRemoteFsPath(uri)) {
        auto f = remoteOpen(uri, err);
        if (!f) return false;
        if (f->size() > static_cast<int64_t>(maxBytes)) {
            err = "file too large";
            return false;
        }
        std::vector<uint8_t> buf(65536);
        for (;;) {
            const int n = f->read(buf.data(), static_cast<int>(buf.size()));
            if (n < 0) {
                err = "read error";
                return false;
            }
            if (n == 0) break;
            out.append(reinterpret_cast<const char*>(buf.data()), static_cast<size_t>(n));
            if (out.size() > maxBytes) {
                err = "file too large";
                return false;
            }
        }
        return true;
    }
    if (uri.find("://") != std::string::npos) {
        err = "unsupported location";
        return false;
    }
    // Only an ordinary file: a link next to the video could point anywhere (e.g. at login data).
    std::error_code ec;
    if (fs::symlink_status(uri, ec).type() != fs::file_type::regular || ec) {
        err = "not a regular file";
        return false;
    }
    if (fs::file_size(uri, ec) > maxBytes || ec) {
        err = ec ? ec.message() : "file too large";
        return false;
    }
    const int fd = ::open(uri.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);  // no link swapped in meanwhile
    if (fd < 0) {
        err = "cannot open the file";
        return false;
    }
    out.clear();
    char buf[65536];
    ssize_t n;
    while ((n = ::read(fd, buf, sizeof buf)) > 0 && out.size() <= maxBytes) out.append(buf, static_cast<size_t>(n));
    ::close(fd);
    if (out.size() > maxBytes) {
        err = "file too large";
        return false;
    }
    return true;
}

bool sidecarMatch(const std::string& pattern, const std::string& base, const std::string& name) {
    static const std::string kBase = "{base}";
    if (pattern.rfind(kBase, 0) != 0 || name.size() <= base.size()) return false;
    for (size_t i = 0; i < base.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(base[i])) != std::tolower(static_cast<unsigned char>(name[i]))) return false;
    }
    const std::string rest = pattern.substr(kBase.size());
    return fnmatch(rest.c_str(), name.c_str() + base.size(), FNM_CASEFOLD | FNM_PERIOD) == 0;
}

bool isUtf8(const std::string& s) {
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t n = 0;  // continuation bytes
        if (c == 0) return false;
        if (c >= 0x80) {
            if ((c >> 5) == 0x6) n = 1;
            else if ((c >> 4) == 0xE) n = 2;
            else if ((c >> 3) == 0x1E) n = 3;
            else return false;
        }
        if (i + n >= s.size()) return false;
        for (size_t k = 1; k <= n; ++k) {
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
        }
        i += n + 1;
    }
    return true;
}

std::string base64(const std::string& s) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((s.size() + 2) / 3 * 4);
    for (size_t i = 0; i < s.size(); i += 3) {
        const uint32_t v = (static_cast<unsigned char>(s[i]) << 16) |
                           (i + 1 < s.size() ? static_cast<unsigned char>(s[i + 1]) << 8 : 0) |
                           (i + 2 < s.size() ? static_cast<unsigned char>(s[i + 2]) : 0);
        out += t[(v >> 18) & 63];
        out += t[(v >> 12) & 63];
        out += i + 1 < s.size() ? t[(v >> 6) & 63] : '=';
        out += i + 2 < s.size() ? t[v & 63] : '=';
    }
    return out;
}

}  // namespace pixelvr::pluginfiles

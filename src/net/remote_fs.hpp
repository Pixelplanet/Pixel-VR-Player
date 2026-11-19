#pragma once
// Network file access beyond SMB: SFTP (libssh) and WebDAV (libcurl). Both are
// optional at build time (PIXELVR_HAVE_LIBSSH / PIXELVR_HAVE_CURL).
//
// URIs: sftp://host[:port]/abs/path, dav://host[:port]/path (http) and
// davs://host[:port]/path (https). Credentials come from SmbCredentialsManager,
// matched by protocol, host and port.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pixelvr {

struct RemoteEntry {
    std::string name;
    bool dir = false;
    uint64_t size = 0;
    int64_t mtime = 0;
};

// Sequential reader with random access. Calls block; one thread at a time.
class RemoteFile {
public:
    virtual ~RemoteFile() = default;
    virtual int64_t size() const = 0;
    virtual int read(uint8_t* buf, int n) = 0;  // <0 error, 0 eof
    virtual bool seek(int64_t pos) = 0;
    // File name the server suggests (Content-Disposition; http(s) only), else "".
    virtual std::string fileName() const { return {}; }
};

inline bool hasPrefix(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }
// Handled by remote_fs.cpp (everything but SMB).
inline bool isRemoteFsPath(const std::string& p) {
    return hasPrefix(p, "sftp://") || hasPrefix(p, "dav://") || hasPrefix(p, "davs://");
}
// A plain web URL: only ever downloaded (read-only), never browsed.
inline bool isHttpUrl(const std::string& p) { return hasPrefix(p, "http://") || hasPrefix(p, "https://"); }
// Any network path: SMB or one of the above.
inline bool isRemotePath(const std::string& p) { return hasPrefix(p, "smb://") || isRemoteFsPath(p); }
// "SMB" / "SFTP" / "WebDAV" for display; empty for local paths.
inline const char* remoteProtocolLabel(const std::string& p) {
    if (hasPrefix(p, "smb://")) return "SMB";
    if (hasPrefix(p, "sftp://")) return "SFTP";
    if (hasPrefix(p, "dav://") || hasPrefix(p, "davs://")) return "WebDAV";
    return "";
}

struct RemoteUri {
    std::string protocol;  // sftp | dav | davs | http | https
    std::string url;       // http(s) only: the address, used verbatim (query string included)
    std::string host;
    int port = 0;          // 0 = the protocol's default
    std::string path;      // starts with '/'
};
bool parseRemoteUri(const std::string& uri, RemoteUri& out);
// "sftp://host:22/a/b" -> "sftp://host:22/a" ("" above the server root)
std::string remoteParent(const std::string& uri);

// A share that needs a login, or refused the one given (SFTP / WebDAV): `err` is then exactly
// kRemoteLoginError, so callers can ask for a login instead of showing a plain failure. Where a
// login was refused, further tries fail at once until it changes (no wrong password sent again).
extern const char* const kRemoteLoginError;
inline bool isRemoteLoginError(const std::string& err) { return err == kRemoteLoginError; }
// Access denied where the same login already worked on that server: a folder or file the login
// may not open (not a login problem: no sign-in is asked for, nothing is blocked).
extern const char* const kRemotePermissionError;
inline bool isRemotePermissionError(const std::string& err) { return err == kRemotePermissionError; }

bool remoteList(const std::string& uri, std::vector<RemoteEntry>& out, std::string& err);
std::unique_ptr<RemoteFile> remoteOpen(const std::string& uri, std::string& err);

}  // namespace pixelvr

#include "net/remote_fs.hpp"

#include "net/smb_credentials.hpp"
#include "util/logging.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <strings.h>

#ifdef PIXELVR_HAVE_LIBSSH
#include <fcntl.h>
#include <unistd.h>
#include <libssh/libssh.h>
#include <libssh/sftp.h>
#endif
#ifdef PIXELVR_HAVE_CURL
#include <curl/curl.h>
#endif

namespace pixelvr {

namespace {

[[maybe_unused]] int defaultPort(const std::string& protocol) {
    if (protocol == "sftp") return 22;
    if (protocol == "dav") return 80;
    return 443;
}

[[maybe_unused]] std::string urlDecode(const std::string& in) {
    std::string out;
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size() && std::isxdigit(static_cast<unsigned char>(in[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(in[i + 2]))) {
            out += static_cast<char>(std::stoi(in.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            out += in[i];
        }
    }
    return out;
}

[[maybe_unused]] std::string urlEncodePath(const std::string& in) {
    std::string out;
    for (const unsigned char c : in) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/') {
            out += static_cast<char>(c);
        } else {
            char b[4];
            std::snprintf(b, sizeof(b), "%%%02X", c);
            out += b;
        }
    }
    return out;
}

SmbShare credentialsFor(const RemoteUri& u) {
    SmbShare s;
    SmbCredentialsManager::instance().findRemote(u.protocol, u.host, u.port, s);
    return s;
}

[[maybe_unused]] std::string serverKey(const RemoteUri& u, const SmbShare& cred) {
    return u.protocol + "|" + cred.username + "@" + u.host + ":" + std::to_string(u.port ? u.port : defaultPort(u.protocol));
}

}  // namespace

bool parseRemoteUri(const std::string& uri, RemoteUri& out) {
    const size_t sch = uri.find("://");
    if (sch == std::string::npos) return false;
    out = RemoteUri{};
    out.protocol = uri.substr(0, sch);
    if (out.protocol != "sftp" && out.protocol != "dav" && out.protocol != "davs") return false;
    const size_t slash = uri.find('/', sch + 3);
    std::string authority = uri.substr(sch + 3, slash == std::string::npos ? std::string::npos : slash - sch - 3);
    out.path = slash == std::string::npos ? "/" : uri.substr(slash);
    std::string hostPart = authority;
    const size_t at = hostPart.rfind('@');
    if (at != std::string::npos) hostPart = hostPart.substr(at + 1);
    const size_t colon = hostPart.rfind(':');
    if (colon != std::string::npos && hostPart.find(']', colon) == std::string::npos) {
        out.port = std::atoi(hostPart.c_str() + colon + 1);
        hostPart = hostPart.substr(0, colon);
    }
    if (hostPart.size() > 1 && hostPart.front() == '[' && hostPart.back() == ']') {
        hostPart = hostPart.substr(1, hostPart.size() - 2);
    }
    out.host = hostPart;
    return !out.host.empty();
}

std::string remoteParent(const std::string& uri) {
    const size_t sch = uri.find("://");
    if (sch == std::string::npos) return "";
    const size_t slash = uri.find('/', sch + 3);
    if (slash == std::string::npos) return "";
    std::string u = uri;
    while (u.size() > slash + 1 && u.back() == '/') u.pop_back();
    if (u.size() <= slash + 1) return "";  // the server root
    const size_t cut = u.find_last_of('/');
    return cut <= slash ? u.substr(0, slash + 1) : u.substr(0, cut);
}

// ------------------------------------------------------------------ SFTP --
#ifdef PIXELVR_HAVE_LIBSSH
namespace {

constexpr int kSftpDepth = 24;
constexpr size_t kSftpMaxRead = 128u << 10;

void sshInitOnce() {
    static std::once_flag once;
    std::call_once(once, [] { ssh_init(); });
}

struct SshConn {
    ssh_session session = nullptr;
    sftp_session sftp = nullptr;
    std::mutex mutex;  // sftp sessions take one caller at a time

    ~SshConn() {
        if (sftp) sftp_free(sftp);
        if (session) {
            ssh_disconnect(session);
            ssh_free(session);
        }
    }
};

std::string knownHostsPath() {
    const char* home = std::getenv("HOME");
    const std::string dir = std::string(home ? home : ".") + "/.config/pixelvr";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir + "/known_hosts";
}

std::shared_ptr<SshConn> sshConnect(const RemoteUri& u, const SmbShare& cred, std::string& err) {
    sshInitOnce();
    auto conn = std::make_shared<SshConn>();
    conn->session = ssh_new();
    if (!conn->session) {
        err = "cannot create SSH session";
        return nullptr;
    }
    ssh_session s = conn->session;
    const int port = u.port ? u.port : 22;
    const long timeout = 15;
    ssh_options_set(s, SSH_OPTIONS_HOST, u.host.c_str());
    ssh_options_set(s, SSH_OPTIONS_PORT, &port);
    ssh_options_set(s, SSH_OPTIONS_TIMEOUT, &timeout);
    if (!cred.username.empty()) ssh_options_set(s, SSH_OPTIONS_USER, cred.username.c_str());
    const std::string kh = knownHostsPath();
    ssh_options_set(s, SSH_OPTIONS_KNOWNHOSTS, kh.c_str());
    if (const char* home = std::getenv("HOME")) {
        for (const char* name : {"id_rsa", "id_ecdsa", "id_ed25519"}) {
            const std::string key = std::string(home) + "/.ssh/" + name;
            if (access(key.c_str(), R_OK) == 0) ssh_options_set(s, SSH_OPTIONS_ADD_IDENTITY, key.c_str());
        }
    }
    if (ssh_connect(s) != SSH_OK) {
        err = std::string("cannot connect to ") + u.host + ": " + ssh_get_error(s);
        return nullptr;
    }
    switch (ssh_session_is_known_server(s)) {
        case SSH_KNOWN_HOSTS_OK:
            break;
        case SSH_KNOWN_HOSTS_CHANGED:
        case SSH_KNOWN_HOSTS_OTHER:
            err = "host key changed for " + u.host;
            return nullptr;
        case SSH_KNOWN_HOSTS_ERROR:
            err = std::string("host key check failed: ") + ssh_get_error(s);
            return nullptr;
        default:  // first contact: trust and remember
            if (ssh_session_update_known_hosts(s) != SSH_OK) {
                PIXELVR_LOG_WARN("SFTP: could not store the host key of %s", u.host.c_str());
            }
            break;
    }
    int rc = ssh_userauth_none(s, nullptr);
    if (rc != SSH_AUTH_SUCCESS) {
        rc = ssh_userauth_publickey_auto(s, nullptr, "");
    }
    if (rc != SSH_AUTH_SUCCESS && !cred.password.empty() && !cred.anonymous) {
        rc = ssh_userauth_password(s, nullptr, cred.password.c_str());
        if (rc != SSH_AUTH_SUCCESS) {
            // Some servers only take the password as keyboard-interactive.
            rc = ssh_userauth_kbdint(s, nullptr, nullptr);
            while (rc == SSH_AUTH_INFO) {
                const int n = ssh_userauth_kbdint_getnprompts(s);
                for (int i = 0; i < n; ++i) ssh_userauth_kbdint_setanswer(s, static_cast<unsigned>(i), cred.password.c_str());
                rc = ssh_userauth_kbdint(s, nullptr, nullptr);
            }
        }
    }
    if (rc != SSH_AUTH_SUCCESS) {
        err = "authentication failed (check the user name and password)";
        return nullptr;
    }
    conn->sftp = sftp_new(s);
    if (!conn->sftp || sftp_init(conn->sftp) != SSH_OK) {
        err = std::string("SFTP is not available on ") + u.host + ": " + ssh_get_error(s);
        return nullptr;
    }
    return conn;
}

std::mutex gSshCacheMutex;
std::map<std::string, std::shared_ptr<SshConn>> gSshCache;

std::shared_ptr<SshConn> cachedConn(const RemoteUri& u, const SmbShare& cred, bool fresh, std::string& err) {
    const std::string key = serverKey(u, cred);
    {
        std::lock_guard<std::mutex> lk(gSshCacheMutex);
        auto it = gSshCache.find(key);
        if (it != gSshCache.end()) {
            if (!fresh) return it->second;
            gSshCache.erase(it);
        }
    }
    auto conn = sshConnect(u, cred, err);  // the handshake is slow: not under the lock
    if (conn) {
        std::lock_guard<std::mutex> lk(gSshCacheMutex);
        gSshCache[key] = conn;
    }
    return conn;
}

// "/~" and "/~/x" are the home folder and a path in it (relative for the server).
std::string sftpPath(const std::string& p) {
    if (p == "/~" || p == "/~/") return ".";
    return p.rfind("/~/", 0) == 0 ? p.substr(3) : p;
}

bool sftpList(const RemoteUri& u, std::vector<RemoteEntry>& out, std::string& err) {
    const SmbShare cred = credentialsFor(u);
    for (int attempt = 0; attempt < 2; ++attempt) {
        auto conn = cachedConn(u, cred, attempt > 0, err);
        if (!conn) return false;
        std::lock_guard<std::mutex> lk(conn->mutex);
        sftp_dir dir = sftp_opendir(conn->sftp, sftpPath(u.path).c_str());
        if (!dir) {
            err = std::string("cannot open folder: ") + ssh_get_error(conn->session);
            if (attempt == 0 && !ssh_is_connected(conn->session)) continue;
            return false;
        }
        std::vector<RemoteEntry> entries;
        while (sftp_attributes a = sftp_readdir(conn->sftp, dir)) {
            std::string name = a->name ? a->name : "";
            if (!name.empty() && name != "." && name != "..") {
                RemoteEntry e;
                e.name = name;
                e.size = a->size;
                e.mtime = static_cast<int64_t>(a->mtime);
                e.dir = a->type == SSH_FILEXFER_TYPE_DIRECTORY;
                if (a->type == SSH_FILEXFER_TYPE_SYMLINK) {
                    const std::string full = (u.path.back() == '/' ? u.path : u.path + "/") + name;
                    if (sftp_attributes t = sftp_stat(conn->sftp, sftpPath(full).c_str())) {
                        e.dir = t->type == SSH_FILEXFER_TYPE_DIRECTORY;
                        e.size = t->size;
                        sftp_attributes_free(t);
                    }
                }
                entries.push_back(std::move(e));
            }
            sftp_attributes_free(a);
        }
        const bool ok = sftp_dir_eof(dir) != 0;
        sftp_closedir(dir);
        if (!ok) {
            err = std::string("folder listing interrupted: ") + ssh_get_error(conn->session);
            if (attempt == 0) continue;
            return false;
        }
        out = std::move(entries);
        return true;
    }
    return false;
}

class SftpFile : public RemoteFile {
public:
    ~SftpFile() override {
        for (Req& r : queue_) sftp_aio_free(r.aio);
        queue_.clear();
        if (file_) sftp_close(file_);
    }

    bool open(const RemoteUri& u, std::string& err) {
        conn_ = sshConnect(u, credentialsFor(u), err);
        if (!conn_) return false;
        sftp_attributes st = sftp_stat(conn_->sftp, sftpPath(u.path).c_str());
        if (!st) {
            err = std::string("cannot read file: ") + ssh_get_error(conn_->session);
            return false;
        }
        size_ = static_cast<int64_t>(st->size);
        sftp_attributes_free(st);
        file_ = sftp_open(conn_->sftp, sftpPath(u.path).c_str(), O_RDONLY, 0);
        if (!file_) {
            err = std::string("cannot open file: ") + ssh_get_error(conn_->session);
            return false;
        }
        chunkLen_ = kSftpMaxRead;
        if (sftp_limits_t lim = sftp_limits(conn_->sftp)) {
            if (lim->max_read_length > 0) chunkLen_ = std::min<size_t>(chunkLen_, static_cast<size_t>(lim->max_read_length));
            sftp_limits_free(lim);
        } else {
            chunkLen_ = 32768;
        }
        buf_.resize(chunkLen_);
        scratch_.resize(chunkLen_);
        return true;
    }

    int64_t size() const override { return size_; }

    int read(uint8_t* out, int n) override {
        if (n <= 0) return 0;
        if (pos_ >= size_) return 0;
        if (have_ == haveOff_ && !fillChunk()) return failed_ ? -1 : 0;
        const size_t take = std::min<size_t>(static_cast<size_t>(n), have_ - haveOff_);
        std::memcpy(out, buf_.data() + haveOff_, take);
        haveOff_ += take;
        pos_ += static_cast<int64_t>(take);
        return static_cast<int>(take);
    }

    bool seek(int64_t p) override {
        if (p < 0) return false;
        drain();
        have_ = haveOff_ = 0;
        pos_ = reqPos_ = p;
        failed_ = false;
        return true;
    }

private:
    struct Req {
        sftp_aio aio = nullptr;
        int64_t off = 0;
        size_t len = 0;
    };

    void drain() {
        for (Req& r : queue_) sftp_aio_wait_read(&r.aio, scratch_.data(), scratch_.size());
        queue_.clear();
    }

    bool fillChunk() {
        while (queue_.size() < static_cast<size_t>(kSftpDepth) && reqPos_ < size_) {
            const size_t want = static_cast<size_t>(std::min<int64_t>(static_cast<int64_t>(chunkLen_), size_ - reqPos_));
            sftp_aio aio = nullptr;
            sftp_seek64(file_, static_cast<uint64_t>(reqPos_));
            const ssize_t sent = sftp_aio_begin_read(file_, want, &aio);
            if (sent <= 0) {
                if (queue_.empty()) {
                    failed_ = true;
                    return false;
                }
                break;
            }
            queue_.push_back({aio, reqPos_, static_cast<size_t>(sent)});
            reqPos_ += sent;
        }
        if (queue_.empty()) return false;
        Req front = queue_.front();
        queue_.pop_front();
        const ssize_t got = sftp_aio_wait_read(&front.aio, buf_.data(), buf_.size());
        if (got < 0) {
            failed_ = true;
            drain();
            return false;
        }
        if (got == 0) return false;
        if (static_cast<size_t>(got) < front.len) {  // short read: restart the pipeline after it
            drain();
            reqPos_ = front.off + got;
        }
        have_ = static_cast<size_t>(got);
        haveOff_ = 0;
        return true;
    }

    std::shared_ptr<SshConn> conn_;
    sftp_file file_ = nullptr;
    int64_t size_ = 0;
    int64_t pos_ = 0;     // next byte handed to the caller
    int64_t reqPos_ = 0;  // next offset to request
    size_t chunkLen_ = 0;
    std::deque<Req> queue_;
    std::vector<uint8_t> buf_;      // the chunk being served
    std::vector<uint8_t> scratch_;  // discarded replies
    size_t have_ = 0;
    size_t haveOff_ = 0;
    bool failed_ = false;
};

}  // namespace
#endif

// ---------------------------------------------------------------- WebDAV --
#ifdef PIXELVR_HAVE_CURL
namespace {

constexpr int64_t kDavRange = 4 << 20;

void curlInitOnce() {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

std::string davUrl(const RemoteUri& u, const std::string& path) {
    std::string url = u.protocol == "davs" ? "https://" : "http://";
    url += u.host.find(':') != std::string::npos ? "[" + u.host + "]" : u.host;
    if (u.port) url += ":" + std::to_string(u.port);
    return url + urlEncodePath(path);
}

struct Collected {
    std::string body;
    std::string contentRange;
    int64_t limit = -1;  // stop receiving past this many bytes
    int64_t received = 0;
};

size_t onBody(char* p, size_t sz, size_t n, void* ud) {
    auto* c = static_cast<Collected*>(ud);
    const size_t bytes = sz * n;
    if (c->limit >= 0) {
        const size_t room = static_cast<size_t>(std::max<int64_t>(0, c->limit - c->received));
        const size_t take = std::min(bytes, room);
        c->body.append(p, take);
        c->received += static_cast<int64_t>(take);
        return take < bytes ? 0 : bytes;  // 0 aborts a server that ignores Range
    }
    c->body.append(p, bytes);
    return bytes;
}

size_t onHeader(char* p, size_t sz, size_t n, void* ud) {
    auto* c = static_cast<Collected*>(ud);
    const std::string line(p, sz * n);
    if (line.size() > 14 && strncasecmp(line.c_str(), "content-range:", 14) == 0) c->contentRange = line.substr(14);
    return sz * n;
}

// One keep-alive handle per request thread of use; owner locks around it.
struct DavHandle {
    CURL* curl = nullptr;
    std::mutex mutex;
    DavHandle() : curl(curl_easy_init()) {}
    ~DavHandle() {
        if (curl) curl_easy_cleanup(curl);
    }
};

void davPrepare(CURL* c, const RemoteUri& u, const SmbShare& cred, const std::string& path, Collected& col) {
    curl_easy_reset(c);
    const std::string url = davUrl(u, path);
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    if (!cred.username.empty() && !cred.anonymous) {
        curl_easy_setopt(c, CURLOPT_USERNAME, cred.username.c_str());
        curl_easy_setopt(c, CURLOPT_PASSWORD, cred.password.c_str());
        curl_easy_setopt(c, CURLOPT_HTTPAUTH, static_cast<long>(CURLAUTH_ANY));
    }
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "PixelVRPlayer");
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, onBody);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &col);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, onHeader);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &col);
}

std::string httpError(long code, CURLcode rc, CURL* c) {
    if (rc != CURLE_OK && rc != CURLE_WRITE_ERROR) return curl_easy_strerror(rc);
    (void)c;
    if (code == 401 || code == 403) return "authentication failed (check the user name and password)";
    if (code == 404) return "not found";
    return "HTTP " + std::to_string(code);
}

std::mutex gDavCacheMutex;
std::map<std::string, std::shared_ptr<DavHandle>> gDavCache;

std::shared_ptr<DavHandle> davHandle(const RemoteUri& u, const SmbShare& cred) {
    std::lock_guard<std::mutex> lk(gDavCacheMutex);
    auto& h = gDavCache[serverKey(u, cred)];
    if (!h) h = std::make_shared<DavHandle>();
    return h;
}

struct XmlResponse {
    std::string href;
    bool dir = false;
    std::string length, modified;
};

std::string xmlText(std::string s) {
    static const std::pair<const char*, char> kEnt[] = {{"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}};
    for (const auto& e : kEnt) {
        for (size_t p = 0; (p = s.find(e.first, p)) != std::string::npos; ++p) s.replace(p, std::strlen(e.first), 1, e.second);
    }
    return s;
}

std::vector<XmlResponse> parsePropfind(const std::string& xml) {
    std::vector<XmlResponse> out;
    XmlResponse cur;
    std::string text, field;
    for (size_t i = 0; i < xml.size();) {
        if (xml[i] != '<') {
            const size_t e = xml.find('<', i);
            text.assign(xml, i, e == std::string::npos ? std::string::npos : e - i);
            i = e == std::string::npos ? xml.size() : e;
            continue;
        }
        const size_t e = xml.find('>', i);
        if (e == std::string::npos) break;
        std::string tag = xml.substr(i + 1, e - i - 1);
        i = e + 1;
        if (tag.empty() || tag[0] == '?' || tag[0] == '!') continue;
        const bool closing = tag[0] == '/';
        const bool selfClosing = tag.back() == '/';
        if (closing) tag.erase(0, 1);
        if (selfClosing) tag.pop_back();
        const size_t sp = tag.find_first_of(" \t\r\n");
        if (sp != std::string::npos) tag.erase(sp);
        const size_t colon = tag.rfind(':');
        if (colon != std::string::npos) tag.erase(0, colon + 1);
        std::transform(tag.begin(), tag.end(), tag.begin(), ::tolower);
        if (closing || selfClosing) {
            if (closing && tag == "href") cur.href = xmlText(text);
            else if (closing && tag == "getcontentlength") cur.length = xmlText(text);
            else if (closing && tag == "getlastmodified") cur.modified = xmlText(text);
            else if (closing && tag == "response") {
                out.push_back(cur);
                cur = XmlResponse{};
            }
            if (selfClosing && tag == "collection") cur.dir = true;
            text.clear();
        } else {
            if (tag == "collection") cur.dir = true;
            text.clear();
        }
    }
    return out;
}

bool davList(const RemoteUri& u, std::vector<RemoteEntry>& out, std::string& err) {
    curlInitOnce();
    const SmbShare cred = credentialsFor(u);
    auto h = davHandle(u, cred);
    std::string folder = u.path;
    if (folder.empty() || folder.back() != '/') folder += '/';
    Collected col;
    long code = 0;
    CURLcode rc;
    {
        std::lock_guard<std::mutex> lk(h->mutex);
        davPrepare(h->curl, u, cred, folder, col);
        static const char kBody[] =
            "<?xml version=\"1.0\"?><d:propfind xmlns:d=\"DAV:\"><d:prop><d:resourcetype/><d:getcontentlength/>"
            "<d:getlastmodified/></d:prop></d:propfind>";
        curl_slist* hdr = nullptr;
        hdr = curl_slist_append(hdr, "Depth: 1");
        hdr = curl_slist_append(hdr, "Content-Type: application/xml");
        curl_easy_setopt(h->curl, CURLOPT_CUSTOMREQUEST, "PROPFIND");
        curl_easy_setopt(h->curl, CURLOPT_POSTFIELDS, kBody);
        curl_easy_setopt(h->curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(sizeof(kBody) - 1));
        curl_easy_setopt(h->curl, CURLOPT_HTTPHEADER, hdr);
        rc = curl_easy_perform(h->curl);
        curl_easy_getinfo(h->curl, CURLINFO_RESPONSE_CODE, &code);
        curl_slist_free_all(hdr);
    }
    if (rc != CURLE_OK || code != 207) {
        err = httpError(code, rc, nullptr);
        return false;
    }
    std::string selfPath = folder;
    while (selfPath.size() > 1 && selfPath.back() == '/') selfPath.pop_back();
    out.clear();
    for (const XmlResponse& r : parsePropfind(col.body)) {
        std::string href = r.href;
        const size_t sch = href.find("://");
        if (sch != std::string::npos) {
            const size_t sl = href.find('/', sch + 3);
            href = sl == std::string::npos ? "/" : href.substr(sl);
        }
        std::string path = urlDecode(href);
        while (path.size() > 1 && path.back() == '/') path.pop_back();
        if (path == selfPath || path.empty()) continue;
        RemoteEntry e;
        e.name = path.substr(path.find_last_of('/') + 1);
        if (e.name.empty()) continue;
        e.dir = r.dir;
        e.size = std::strtoull(r.length.c_str(), nullptr, 10);
        if (!r.modified.empty()) {
            std::tm tm{};
            if (strptime(r.modified.c_str(), "%a, %d %b %Y %H:%M:%S", &tm)) e.mtime = static_cast<int64_t>(timegm(&tm));
        }
        out.push_back(std::move(e));
    }
    return true;
}

class DavFile : public RemoteFile {
public:
    bool open(const RemoteUri& u, std::string& err) {
        curlInitOnce();
        uri_ = u;
        cred_ = credentialsFor(u);
        curl_ = curl_easy_init();
        if (!curl_) {
            err = "cannot start the HTTP client";
            return false;
        }
        Collected col;
        davPrepare(curl_, uri_, cred_, uri_.path, col);
        curl_easy_setopt(curl_, CURLOPT_NOBODY, 1L);
        CURLcode rc = curl_easy_perform(curl_);
        long code = 0;
        curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &code);
        curl_off_t len = -1;
        if (rc == CURLE_OK && code / 100 == 2) curl_easy_getinfo(curl_, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &len);
        if (len < 0) {  // HEAD refused: ask for one byte
            std::string e;
            int64_t total = -1;
            if (fetch(0, 1, &total, &e) && total >= 0) len = total;
            chunk_.clear();
            if (len < 0) {
                err = e.empty() ? httpError(code, rc, curl_) : e;
                return false;
            }
        }
        size_ = len;
        return true;
    }

    ~DavFile() override {
        if (curl_) curl_easy_cleanup(curl_);
    }

    int64_t size() const override { return size_; }

    int read(uint8_t* out, int n) override {
        if (n <= 0) return 0;
        if (pos_ >= size_) return 0;
        if (pos_ < chunkOff_ || pos_ >= chunkOff_ + static_cast<int64_t>(chunk_.size())) {
            const int64_t want = std::min<int64_t>(kDavRange, size_ - pos_);
            std::string e;
            if (!fetch(pos_, want, nullptr, &e) && !fetch(pos_, want, nullptr, &e)) return -1;  // one retry
            chunkOff_ = pos_;
        }
        const size_t at = static_cast<size_t>(pos_ - chunkOff_);
        const size_t take = std::min<size_t>(static_cast<size_t>(n), chunk_.size() - at);
        std::memcpy(out, chunk_.data() + at, take);
        pos_ += static_cast<int64_t>(take);
        return static_cast<int>(take);
    }

    bool seek(int64_t p) override {
        if (p < 0) return false;
        pos_ = p;
        return true;
    }

private:
    // Fetches [from, from+len) into chunk_.
    bool fetch(int64_t from, int64_t len, int64_t* total, std::string* err) {
        Collected col;
        col.limit = len;
        davPrepare(curl_, uri_, cred_, uri_.path, col);
        const std::string range = std::to_string(from) + "-" + std::to_string(from + len - 1);
        curl_easy_setopt(curl_, CURLOPT_RANGE, range.c_str());
        const CURLcode rc = curl_easy_perform(curl_);
        long code = 0;
        curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &code);
        if (!(rc == CURLE_OK || (rc == CURLE_WRITE_ERROR && col.received >= len)) ||
            (code != 206 && !(code == 200 && from == 0)) || col.body.empty()) {
            if (err) *err = httpError(code, rc, curl_);
            return false;
        }
        if (total) {
            const size_t slash = col.contentRange.find('/');
            *total = slash == std::string::npos ? -1 : std::atoll(col.contentRange.c_str() + slash + 1);
        }
        chunk_.assign(col.body.begin(), col.body.end());
        return true;
    }

    RemoteUri uri_;
    SmbShare cred_;
    CURL* curl_ = nullptr;
    int64_t size_ = 0;
    int64_t pos_ = 0;
    int64_t chunkOff_ = 0;
    std::vector<char> chunk_;
};

}  // namespace
#endif

// ---------------------------------------------------------------- facade --
bool remoteList(const std::string& uri, std::vector<RemoteEntry>& out, std::string& err) {
    RemoteUri u;
    if (!parseRemoteUri(uri, u)) {
        err = "not a network address";
        return false;
    }
    if (u.protocol == "sftp") {
#ifdef PIXELVR_HAVE_LIBSSH
        return sftpList(u, out, err);
#else
        err = "SFTP support not built";
        return false;
#endif
    }
#ifdef PIXELVR_HAVE_CURL
    return davList(u, out, err);
#else
    err = "WebDAV support not built";
    return false;
#endif
}

std::unique_ptr<RemoteFile> remoteOpen(const std::string& uri, std::string& err) {
    RemoteUri u;
    if (!parseRemoteUri(uri, u)) {
        err = "not a network address";
        return nullptr;
    }
    if (u.protocol == "sftp") {
#ifdef PIXELVR_HAVE_LIBSSH
        auto f = std::make_unique<SftpFile>();
        if (!f->open(u, err)) return nullptr;
        return f;
#else
        err = "SFTP support not built";
        return nullptr;
#endif
    }
#ifdef PIXELVR_HAVE_CURL
    auto f = std::make_unique<DavFile>();
    if (!f->open(u, err)) return nullptr;
    return f;
#else
    err = "WebDAV support not built";
    return nullptr;
#endif
}

}  // namespace pixelvr

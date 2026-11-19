#include "smb_credentials.hpp"
#include "util/logging.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <random>
#include <sstream>

#if defined(__linux__) || defined(__APPLE__)
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace pixelvr {
namespace fs = std::filesystem;

namespace {

// Clean portable ChaCha20 stream cipher implementation
// RFC 7539 / 8439
static uint32_t rotl32(uint32_t x, int n) {
    return (x << n) | (x >> (32 - n));
}

#define CHACHA20_QUARTERROUND(a, b, c, d) \
    a += b; d ^= a; d = rotl32(d, 16); \
    c += d; b ^= c; b = rotl32(b, 12); \
    a += b; d ^= a; d = rotl32(d, 8);  \
    c += d; b ^= c; b = rotl32(b, 7);

static void chacha20_block(uint32_t out[16], const uint32_t in[16]) {
    for (int i = 0; i < 16; ++i) out[i] = in[i];
    for (int i = 0; i < 10; ++i) {
        CHACHA20_QUARTERROUND(out[0], out[4], out[8],  out[12]);
        CHACHA20_QUARTERROUND(out[1], out[5], out[9],  out[13]);
        CHACHA20_QUARTERROUND(out[2], out[6], out[10], out[14]);
        CHACHA20_QUARTERROUND(out[3], out[7], out[11], out[15]);
        CHACHA20_QUARTERROUND(out[0], out[5], out[10], out[15]);
        CHACHA20_QUARTERROUND(out[1], out[6], out[11], out[12]);
        CHACHA20_QUARTERROUND(out[2], out[7], out[8],  out[13]);
        CHACHA20_QUARTERROUND(out[3], out[4], out[9],  out[14]);
    }
    for (int i = 0; i < 16; ++i) out[i] += in[i];
}

static void chacha20_crypt(const uint8_t key[32], const uint8_t nonce[12], uint32_t counter,
                           const uint8_t* in, uint8_t* out, std::size_t len) {
    uint32_t state[16];
    // Constants "expand 32-byte k"
    state[0] = 0x61707865;
    state[1] = 0x3320646e;
    state[2] = 0x79622d32;
    state[3] = 0x6b206574;
    for (int i = 0; i < 8; ++i) {
        state[4 + i] = (static_cast<uint32_t>(key[i * 4 + 0])) |
                       (static_cast<uint32_t>(key[i * 4 + 1]) << 8) |
                       (static_cast<uint32_t>(key[i * 4 + 2]) << 16) |
                       (static_cast<uint32_t>(key[i * 4 + 3]) << 24);
    }
    state[12] = counter;
    for (int i = 0; i < 3; ++i) {
        state[13 + i] = (static_cast<uint32_t>(nonce[i * 4 + 0])) |
                        (static_cast<uint32_t>(nonce[i * 4 + 1]) << 8) |
                        (static_cast<uint32_t>(nonce[i * 4 + 2]) << 16) |
                        (static_cast<uint32_t>(nonce[i * 4 + 3]) << 24);
    }

    uint32_t block[16];
    uint8_t keyStream[64];
    std::size_t offset = 0;
    while (offset < len) {
        chacha20_block(block, state);
        for (int i = 0; i < 16; ++i) {
            keyStream[i * 4 + 0] = static_cast<uint8_t>(block[i] & 0xFF);
            keyStream[i * 4 + 1] = static_cast<uint8_t>((block[i] >> 8) & 0xFF);
            keyStream[i * 4 + 2] = static_cast<uint8_t>((block[i] >> 16) & 0xFF);
            keyStream[i * 4 + 3] = static_cast<uint8_t>((block[i] >> 24) & 0xFF);
        }
        state[12]++;
        const std::size_t chunk = std::min<std::size_t>(64, len - offset);
        for (std::size_t i = 0; i < chunk; ++i) {
            out[offset + i] = in[offset + i] ^ keyStream[i];
        }
        offset += chunk;
    }
}

static std::string toHex(const uint8_t* data, std::size_t len) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < len; ++i) {
        oss << std::setw(2) << static_cast<int>(data[i]);
    }
    return oss.str();
}

static std::vector<uint8_t> fromHex(const std::string& hex) {
    std::vector<uint8_t> bytes;
    if (hex.length() % 2 != 0) return bytes;
    bytes.reserve(hex.length() / 2);
    for (std::size_t i = 0; i < hex.length(); i += 2) {
        const std::string byteStr = hex.substr(i, 2);
        char* end = nullptr;
        unsigned long b = std::strtoul(byteStr.c_str(), &end, 16);
        if (end == byteStr.c_str() + 2) {
            bytes.push_back(static_cast<uint8_t>(b));
        } else {
            return {};
        }
    }
    return bytes;
}

static std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

} // namespace

static int defaultPortOf(const std::string& protocol) {
    if (protocol == "sftp") return 22;
    if (protocol == "dav") return 80;
    if (protocol == "davs") return 443;
    return 0;
}

static std::string lowered(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}

// Same server: protocol and host match; for SFTP / WebDAV the port too.
static bool sameServer(const SmbShare& s, const std::string& protocol, const std::string& hostLower, int port) {
    if (s.protocol != protocol || lowered(s.host) != hostLower) return false;
    if (protocol == "smb") return true;
    const int def = defaultPortOf(protocol);
    return (s.port ? s.port : def) == (port ? port : def);
}

SmbCredentialsManager& SmbCredentialsManager::instance() {
    static SmbCredentialsManager sInstance;
    return sInstance;
}

SmbCredentialsManager::SmbCredentialsManager() {
    load();
    // Older versions left smbclient auth files (with the password) next to the shares file; the
    // login now goes to smbclient through a pipe, so remove them.
    std::error_code ec;
    const fs::path dir = fs::path(getStoragePath()).parent_path();
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string n = it->path().filename().string();
        if (n.rfind("auth_", 0) == 0 && n.size() > 9 && n.compare(n.size() - 4, 4, ".tmp") == 0) {
            std::error_code rec;
            fs::remove(it->path(), rec);
        }
    }
}

std::string SmbCredentialsManager::getStoragePath() const {
#ifdef _WIN32
    const char* appData = std::getenv("APPDATA");
    if (appData) return std::string(appData) + "/PixelVR/smb_shares.json";
#else
    const char* home = std::getenv("HOME");
    if (home) return std::string(home) + "/.config/pixelvr/smb_shares.json";
#endif
    return "pixelvr_smb_shares.json";
}

std::string SmbCredentialsManager::getDeviceKey() const {
    std::string seed = "PixelVR-Frame-SMB-2026-Salt!";
#if defined(__linux__)
    std::ifstream mid("/etc/machine-id");
    if (mid.is_open()) {
        std::string id;
        std::getline(mid, id);
        id = trim(id);
        if (!id.empty()) {
            seed += ":" + id;
        }
    }
#endif
    const char* user = std::getenv("USER");
    if (user) seed += ":" + std::string(user);

    // Derive 32-byte key using double FNV-1a / DJB2 mixing
    uint64_t h1 = 14695981039346656037ULL;
    uint64_t h2 = 5381;
    for (char c : seed) {
        h1 = (h1 ^ static_cast<uint8_t>(c)) * 1099511628211ULL;
        h2 = ((h2 << 5) + h2) + static_cast<uint8_t>(c);
    }
    uint8_t keyBytes[32];
    for (int i = 0; i < 8; ++i) {
        keyBytes[i + 0] = static_cast<uint8_t>((h1 >> (i * 8)) & 0xFF);
        keyBytes[i + 8] = static_cast<uint8_t>((h2 >> (i * 8)) & 0xFF);
        keyBytes[i + 16] = static_cast<uint8_t>(((h1 ^ h2) >> (i * 8)) & 0xFF);
        keyBytes[i + 24] = static_cast<uint8_t>(((h1 + h2) >> (i * 8)) & 0xFF);
    }
    return std::string(reinterpret_cast<const char*>(keyBytes), 32);
}

std::string SmbCredentialsManager::encryptPassword(const std::string& plain,
                                                   const std::string& key,
                                                   std::string& outIvHex) const {
    if (plain.empty()) {
        outIvHex.clear();
        return "";
    }
    uint8_t nonce[12];
    std::random_device rd;
    for (int i = 0; i < 12; ++i) {
        nonce[i] = static_cast<uint8_t>(rd() & 0xFF);
    }
    outIvHex = toHex(nonce, 12);

    std::vector<uint8_t> cipher(plain.length());
    chacha20_crypt(reinterpret_cast<const uint8_t*>(key.data()), nonce, 1,
                   reinterpret_cast<const uint8_t*>(plain.data()), cipher.data(), plain.length());
    return toHex(cipher.data(), cipher.size());
}

std::string SmbCredentialsManager::decryptPassword(const std::string& cipherHex,
                                                   const std::string& key,
                                                   const std::string& ivHex) const {
    if (cipherHex.empty() || ivHex.empty()) return "";
    std::vector<uint8_t> cipher = fromHex(cipherHex);
    std::vector<uint8_t> nonce = fromHex(ivHex);
    if (cipher.empty() || nonce.size() != 12) return "";

    std::string plain(cipher.size(), '\0');
    chacha20_crypt(reinterpret_cast<const uint8_t*>(key.data()), nonce.data(), 1,
                   cipher.data(), reinterpret_cast<uint8_t*>(plain.data()), cipher.size());
    return plain;
}

void SmbCredentialsManager::load() {
    std::lock_guard<std::mutex> lk(mutex_);
    shares_.clear();  // (logins typed this session stay: they are not in the file)

    const std::string path = getStoragePath();
    std::ifstream f(path);
    if (!f.is_open()) {
        return;
    }

    std::string content((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
    f.close();

    const std::string key = getDeviceKey();

    // Lightweight robust JSON parser for share objects
    std::size_t pos = 0;
    while ((pos = content.find('{', pos)) != std::string::npos) {
        std::size_t endObj = content.find('}', pos);
        if (endObj == std::string::npos) break;

        std::string obj = content.substr(pos, endObj - pos + 1);
        pos = endObj + 1;

        auto extractField = [&](const std::string& keyName) -> std::string {
            const std::string pattern = "\"" + keyName + "\"";
            std::size_t kpos = obj.find(pattern);
            if (kpos == std::string::npos) return "";
            std::size_t colon = obj.find(':', kpos + pattern.length());
            if (colon == std::string::npos) return "";
            std::size_t startVal = obj.find_first_not_of(" \t\r\n", colon + 1);
            if (startVal == std::string::npos) return "";
            if (obj[startVal] == '\"') {
                std::size_t endQuote = obj.find('\"', startVal + 1);
                if (endQuote != std::string::npos) {
                    return obj.substr(startVal + 1, endQuote - startVal - 1);
                }
            } else {
                std::size_t endVal = obj.find_first_of(",}\r\n", startVal);
                return trim(obj.substr(startVal, endVal - startVal));
            }
            return "";
        };

        SmbShare s;
        s.id = extractField("id");
        s.name = extractField("name");
        s.host = extractField("host");
        s.share = extractField("share");
        s.workgroup = extractField("workgroup");
        if (s.workgroup.empty()) s.workgroup = "WORKGROUP";
        s.username = extractField("username");
        s.anonymous = (extractField("anonymous") == "true");
        s.protocol = extractField("protocol");
        if (s.protocol != "sftp" && s.protocol != "dav" && s.protocol != "davs") s.protocol = "smb";
        s.port = std::atoi(extractField("port").c_str());
        // Files from before the switch have no such field: their passwords were saved.
        s.savePassword = (extractField("save_password") != "false");

        std::string passCipher = extractField("password_enc");
        std::string passIv = extractField("iv");
        // (With saving off a stored password is ignored; the next save leaves it out.)
        if (s.savePassword && !passCipher.empty() && !passIv.empty()) {
            s.password = decryptPassword(passCipher, key, passIv);
        }

        if (!s.host.empty()) {
            if (s.name.empty()) {
                s.name = s.host + (!s.share.empty() ? ("/" + s.share) : "");
            }
            if (s.id.empty()) {
                s.id = s.host + (!s.share.empty() ? ("/" + s.share) : "");
            }

            shares_.push_back(s);
        }
    }

    PIXELVR_LOG_INFO("SmbCredentialsManager: Loaded %zu configured shares from %s",
                     shares_.size(), path.c_str());
}

void SmbCredentialsManager::save() {
    std::lock_guard<std::mutex> lk(mutex_);
    saveLocked();
}

void SmbCredentialsManager::saveLocked() {
    const std::string path = getStoragePath();

    std::error_code ec;
    fs::path p(path);
    fs::create_directories(p.parent_path(), ec);

#if defined(__linux__) || defined(__APPLE__)
    // Ensure configuration directory is protected (0700)
    chmod(p.parent_path().string().c_str(), 0700);
#endif

    const std::string key = getDeviceKey();

    std::ostringstream json;
    json << "[\n";
    for (std::size_t i = 0; i < shares_.size(); ++i) {
        const auto& s = shares_[i];
        std::string ivHex;
        // Saving off: no password in the file at all (not even an encrypted one).
        std::string encPass = s.savePassword ? encryptPassword(s.password, key, ivHex) : std::string();

        json << "  {\n";
        json << "    \"id\": \"" << s.id << "\",\n";
        json << "    \"name\": \"" << s.name << "\",\n";
        json << "    \"host\": \"" << s.host << "\",\n";
        json << "    \"share\": \"" << s.share << "\",\n";
        json << "    \"workgroup\": \"" << s.workgroup << "\",\n";
        json << "    \"username\": \"" << s.username << "\",\n";
        json << "    \"password_enc\": \"" << encPass << "\",\n";
        json << "    \"iv\": \"" << ivHex << "\",\n";
        json << "    \"save_password\": " << (s.savePassword ? "true" : "false") << ",\n";
        json << "    \"protocol\": \"" << s.protocol << "\",\n";
        json << "    \"port\": " << s.port << ",\n";
        json << "    \"anonymous\": " << (s.anonymous ? "true" : "false") << "\n";
        json << "  }" << (i + 1 < shares_.size() ? ",\n" : "\n");
    }
    json << "]\n";

    std::ofstream f(path, std::ios::trunc);
    if (f.is_open()) {
        f << json.str();
        f.close();

#if defined(__linux__) || defined(__APPLE__)
        // Enforce strict OS-level permissions: readable & writable ONLY by steamos (0600)
        chmod(path.c_str(), 0600);
#endif
        PIXELVR_LOG_INFO("SmbCredentialsManager: Saved %zu shares to %s (mode 0600)",
                         shares_.size(), path.c_str());
    } else {
        PIXELVR_LOG_WARN("SmbCredentialsManager: Failed to write %s", path.c_str());
    }
}

std::vector<SmbShare> SmbCredentialsManager::shares() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return shares_;
}

namespace {

// The same login: SMB by host and share, SFTP / WebDAV by server (as findRemote matches).
bool sameLogin(const SmbShare& s, const std::string& protocol, const std::string& host, const std::string& share,
               int port) {
    if (protocol != "smb") return sameServer(s, protocol, lowered(host), port);
    return s.protocol == "smb" && lowered(s.host) == lowered(host) && lowered(s.share) == lowered(share);
}

// Where a server checks a login: an SMB share ("smb://host:0/share"; the server itself for its
// share list), an SFTP / WebDAV server ("sftp://host:22"). Lower case, port filled in.
std::string scopeKey(const std::string& protocol, const std::string& host, const std::string& share, int port) {
    const int effPort = port ? port : defaultPortOf(protocol);
    std::string k = protocol + "://" + lowered(host) + ":" + std::to_string(protocol == "smb" ? 0 : effPort);
    if (protocol == "smb" && !share.empty()) k += "/" + lowered(share);
    return k;
}

// `key` is `scope` or lies below it.
bool inScope(const std::string& key, const std::string& scope) {
    return key == scope || (key.size() > scope.size() && key.compare(0, scope.size(), scope) == 0 &&
                            key[scope.size()] == '/');
}

// A network address, split: protocol, server, SMB share and the scope its login is checked in.
struct Address {
    std::string protocol;
    std::string host;
    std::string share;  // SMB only: the first path part ("" = the server's share list)
    int port = 0;
    std::string scope;  // scopeKey() of the address
};

bool parseAddress(const std::string& uri, Address& a) {
    const std::size_t sch = uri.find("://");
    if (sch == std::string::npos) return false;
    a.protocol = lowered(uri.substr(0, sch));
    if (a.protocol != "smb" && a.protocol != "sftp" && a.protocol != "dav" && a.protocol != "davs") return false;
    const std::string rest = uri.substr(sch + 3);
    const std::size_t slash = rest.find('/');
    std::string authority = rest.substr(0, slash);
    std::string path = slash == std::string::npos ? std::string() : rest.substr(slash);
    if (const std::size_t at = authority.rfind('@'); at != std::string::npos) authority = authority.substr(at + 1);
    a.port = 0;
    if (!authority.empty() && authority[0] == '[') {  // [IPv6]:port
        const std::size_t close = authority.find(']');
        if (close == std::string::npos) return false;
        a.host = authority.substr(1, close - 1);
        if (close + 1 < authority.size() && authority[close + 1] == ':') a.port = std::atoi(authority.c_str() + close + 2);
    } else {
        const std::size_t colon = authority.rfind(':');
        if (colon != std::string::npos && authority.find(':') == colon) {
            a.host = authority.substr(0, colon);
            a.port = std::atoi(authority.c_str() + colon + 1);
        } else {
            a.host = authority;
        }
    }
    if (a.host.empty()) return false;
    if (a.protocol == "smb") a.port = 0;
    a.share.clear();
    if (a.protocol == "smb") {
        std::size_t b = path.find_first_not_of('/');
        if (b != std::string::npos) a.share = path.substr(b, path.find('/', b) == std::string::npos ? std::string::npos
                                                                                                  : path.find('/', b) - b);
    }
    a.scope = scopeKey(a.protocol, a.host, a.share, a.port);
    return true;
}

}  // namespace

const SmbShare* SmbCredentialsManager::loginFor(const std::string& protocol, const std::string& host,
                                                const std::string& share, int port) const {
    const std::string h = lowered(host);
    if (protocol != "smb") {
        for (const auto& s : session_) {
            if (sameServer(s, protocol, h, port)) return &s;
        }
        for (const auto& s : shares_) {
            if (sameServer(s, protocol, h, port)) return &s;
        }
        return nullptr;
    }
    const std::string sh = lowered(share);
    // A share's own login beats one for the whole server; at the same level a session login wins.
    auto exact = [&](const std::vector<SmbShare>& v) -> const SmbShare* {
        if (sh.empty()) return nullptr;
        for (const auto& s : v) {
            if (s.protocol == "smb" && lowered(s.host) == h && lowered(s.share) == sh) return &s;
        }
        return nullptr;
    };
    auto server = [&](const std::vector<SmbShare>& v) -> const SmbShare* {
        for (const auto& s : v) {
            if (s.protocol == "smb" && lowered(s.host) == h && s.share.empty()) return &s;
        }
        return nullptr;
    };
    if (const SmbShare* m = exact(session_)) return m;
    if (const SmbShare* m = exact(shares_)) return m;
    if (const SmbShare* m = server(session_)) return m;
    return server(shares_);
}

const SmbShare* SmbCredentialsManager::loginForUri(const std::string& uri) const {
    Address a;
    if (!parseAddress(uri, a)) return nullptr;
    return loginFor(a.protocol, a.host, a.share, a.port);
}

namespace {
std::size_t stampOf(const SmbShare* m) {
    if (m == nullptr || m->anonymous) return std::hash<std::string>{}("guest");
    return std::hash<std::string>{}(m->username + '\x1f' + m->password + '\x1f' + m->workgroup);
}
}  // namespace

std::size_t SmbCredentialsManager::loginStamp(const std::string& uri) const { return stampOf(loginForUri(uri)); }

void SmbCredentialsManager::forgetRefusedLocked(const std::string& scope, const std::size_t* stamp) {
    refused_.erase(std::remove_if(refused_.begin(), refused_.end(),
                                  [&](const std::pair<std::string, std::size_t>& r) {
                                      return inScope(r.first, scope) && (stamp == nullptr || r.second == *stamp);
                                  }),
                   refused_.end());
}

void SmbCredentialsManager::addOrUpdateShare(const SmbShare& share) {
    if (share.host.empty()) return;

    std::lock_guard<std::mutex> lk(mutex_);
    SmbShare stored = share;
    if (!share.savePassword) stored.password.clear();  // (in memory below, never in the file)
    if (share.savePassword && share.password.empty() && !share.anonymous) {
        // Saving switched on for a share signed into this session: the form shows no password
        // (none is saved), so the one that works is saved instead of an empty one.
        for (const auto& s : session_) {
            if (sameLogin(s, share.protocol, share.host, share.share, share.port) && s.username == share.username) {
                stored.password = s.password;
                break;
            }
        }
    }
    bool found = false;
    for (auto& s : shares_) {
        if (s.protocol == share.protocol && s.host == share.host && s.share == share.share &&
            (share.protocol == "smb" || s.port == share.port)) {
            s = stored;
            found = true;
            break;
        }
    }
    if (!found) {
        shares_.push_back(stored);
    }
    // Edited: a password typed for this session only is forgotten, unless the form carries one.
    session_.erase(std::remove_if(session_.begin(), session_.end(),
                                  [&](const SmbShare& s) {
                                      return sameLogin(s, share.protocol, share.host, share.share, share.port);
                                  }),
                   session_.end());
    if (!share.savePassword && !share.password.empty() && !share.anonymous) session_.push_back(share);
    // The share was saved by hand: its login gets one try again (only on this server).
    forgetRefusedLocked(scopeKey(share.protocol, share.host, "", share.port), nullptr);
    saveLocked();
}

bool SmbCredentialsManager::deleteShare(const std::string& host, const std::string& share,
                                        const std::string& protocol, int port) {
    bool removed = false;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto gone = [&](const SmbShare& s) {
            if (s.protocol == protocol && s.host == host && (protocol == "smb" || s.port == port)) {
                if (share.empty() || s.share == share || s.share.empty()) {
                    return true;
                }
            }
            return false;
        };
        auto it = std::remove_if(shares_.begin(), shares_.end(), gone);
        if (it != shares_.end()) {
            shares_.erase(it, shares_.end());
            removed = true;
        }
        session_.erase(std::remove_if(session_.begin(), session_.end(), gone), session_.end());
        forgetRefusedLocked(scopeKey(protocol, host, "", port), nullptr);
        if (removed) saveLocked();
    }
    if (removed) {
        PIXELVR_LOG_INFO("SmbCredentialsManager: Deleted share for host '%s'", host.c_str());
    }
    return removed;
}

bool SmbCredentialsManager::findRemote(const std::string& protocol, const std::string& host, int port,
                                       SmbShare& out) const {
    std::lock_guard<std::mutex> lk(mutex_);
    if (const SmbShare* m = loginFor(protocol, host, "", port)) {
        out = *m;
        return true;
    }
    return false;
}

bool SmbCredentialsManager::findShare(const std::string& hostIn,
                                      const std::string& shareIn,
                                      SmbShare& outShare,
                                      const std::string& protocol,
                                      int port) const {
    std::lock_guard<std::mutex> lk(mutex_);
    if (hostIn.empty()) return false;

    std::string host = hostIn;
    std::transform(host.begin(), host.end(), host.begin(), ::tolower);
    std::string share = shareIn;
    std::transform(share.begin(), share.end(), share.begin(), ::tolower);

    for (const auto& s : shares_) {
        std::string sHost = s.host;
        std::transform(sHost.begin(), sHost.end(), sHost.begin(), ::tolower);
        if (s.protocol == protocol && sHost == host && (protocol == "smb" || sameServer(s, protocol, host, port))) {
            std::string sShare = s.share;
            std::transform(sShare.begin(), sShare.end(), sShare.begin(), ::tolower);
            if (sShare == share || (share.empty() && sShare.empty())) {
                outShare = s;
                return true;
            }
        }
    }
    return false;
}

bool SmbCredentialsManager::findCredentials(const std::string& hostIn,
                                           const std::string& shareIn,
                                           std::string& outUser,
                                           std::string& outPass,
                                           std::string& outWorkgroup) const {
    std::lock_guard<std::mutex> lk(mutex_);
    if (hostIn.empty()) return false;
    const SmbShare* m = loginFor("smb", hostIn, shareIn, 0);
    if (m && !m->anonymous) {
        outUser = m->username;
        outPass = m->password;
        outWorkgroup = m->workgroup.empty() ? "WORKGROUP" : m->workgroup;
        return true;
    }
    return false;
}

std::string SmbCredentialsManager::authFileText(const std::string& host, const std::string& share) const {
    std::string user, pass, wg;
    if (!findCredentials(host, share, user, pass, wg) || user.empty()) return "";
    std::string text = "username = " + user + "\n";
    text += "password = " + pass + "\n";
    text += "domain = " + (wg.empty() ? std::string("WORKGROUP") : wg) + "\n";
    std::fill(pass.begin(), pass.end(), '\0');
    return text;
}

bool SmbCredentialsManager::loginTargetFor(const std::string& uri, LoginTarget& out) const {
    Address a;
    if (!parseAddress(uri, a)) return false;
    std::lock_guard<std::mutex> lk(mutex_);
    out = LoginTarget{};
    out.protocol = a.protocol;
    out.host = a.host;
    out.port = a.port;
    // The saved share whose login the address uses (the same order as loginFor, saved ones only).
    const SmbShare* saved = nullptr;
    const std::string h = lowered(a.host);
    for (const auto& s : shares_) {
        if (a.protocol != "smb" ? sameServer(s, a.protocol, h, a.port)
                                : (s.protocol == "smb" && lowered(s.host) == h && !a.share.empty() &&
                                   lowered(s.share) == lowered(a.share))) {
            saved = &s;
            break;
        }
    }
    if (saved == nullptr && a.protocol == "smb") {
        for (const auto& s : shares_) {
            if (s.protocol == "smb" && lowered(s.host) == h && s.share.empty()) {
                saved = &s;
                break;
            }
        }
    }
    if (saved != nullptr) {
        out.saved = true;
        out.host = saved->host;
        out.share = saved->share;
        out.port = saved->port;
        out.savePassword = saved->savePassword;
        out.username = saved->username;
        out.label = saved->name;
    } else {
        out.share = a.protocol == "smb" ? a.share : std::string();
    }
    if (out.label.empty()) out.label = out.host + (a.protocol == "smb" && !out.share.empty() ? "/" + out.share : "");
    const SmbShare* inUse = loginFor(a.protocol, a.host, a.share, a.port);
    if (inUse != nullptr && !inUse->anonymous && !inUse->username.empty()) out.username = inUse->username;
    return true;
}

void SmbCredentialsManager::setSessionLogin(const LoginTarget& t, const std::string& user, const std::string& pass) {
    std::lock_guard<std::mutex> lk(mutex_);
    std::string workgroup = "WORKGROUP";
    for (const auto& s : shares_) {
        if (sameLogin(s, t.protocol, t.host, t.share, t.port) && !s.workgroup.empty()) workgroup = s.workgroup;
    }
    session_.erase(std::remove_if(session_.begin(), session_.end(),
                                  [&](const SmbShare& s) { return sameLogin(s, t.protocol, t.host, t.share, t.port); }),
                   session_.end());
    SmbShare s;
    s.protocol = t.protocol;
    s.host = t.host;
    s.share = t.share;
    s.port = t.port;
    s.username = user;
    s.password = pass;
    s.workgroup = workgroup;
    s.name = t.label;
    s.id = t.host + (t.share.empty() ? "" : "/" + t.share);
    s.savePassword = false;
    session_.push_back(s);
    // The user typed this login, so it gets its try even where it was refused before. Other
    // refusals stay: when this one fails and is dropped, the login it replaced is still not sent.
    const std::size_t stamp = stampOf(&s);
    forgetRefusedLocked(scopeKey(t.protocol, t.host, t.share, t.port), &stamp);
}

void SmbCredentialsManager::keepSessionLogin(const LoginTarget& t, bool savePassword) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto sit = std::find_if(session_.begin(), session_.end(),
                            [&](const SmbShare& s) { return sameLogin(s, t.protocol, t.host, t.share, t.port); });
    if (sit == session_.end()) return;
    SmbShare* saved = nullptr;
    for (auto& s : shares_) {
        if (s.protocol == t.protocol && lowered(s.host) == lowered(t.host) && lowered(s.share) == lowered(t.share) &&
            (t.protocol == "smb" || sameServer(s, t.protocol, lowered(t.host), t.port))) {
            saved = &s;
            break;
        }
    }
    if (savePassword) {
        if (saved != nullptr) {
            saved->username = sit->username;
            saved->password = sit->password;
            saved->anonymous = false;
            saved->savePassword = true;
        } else {
            SmbShare s = *sit;
            s.savePassword = true;
            s.anonymous = false;
            if (s.name.empty()) s.name = s.id;
            shares_.push_back(s);
        }
        // The saved share holds it now. An empty password that worked stays in memory too, so it is
        // not asked for again until the player quits (a share saved without a password asks first).
        if (!sit->password.empty()) session_.erase(sit);
        saveLocked();
        return;
    }
    if (saved != nullptr) {  // the user name is saved, the password stays in memory (session_)
        saved->username = sit->username;
        saved->password.clear();
        saved->anonymous = false;
        saved->savePassword = false;
        saveLocked();
    }
    // (A share that was not saved stays unsaved: nothing is written.)
}

void SmbCredentialsManager::dropSessionLogin(const LoginTarget& t) {
    std::lock_guard<std::mutex> lk(mutex_);
    session_.erase(std::remove_if(session_.begin(), session_.end(),
                                  [&](const SmbShare& s) { return sameLogin(s, t.protocol, t.host, t.share, t.port); }),
                   session_.end());
}

bool SmbCredentialsManager::loginMissing(const std::string& uri) const {
    Address a;
    if (!parseAddress(uri, a) || a.protocol == "sftp") return false;
    std::lock_guard<std::mutex> lk(mutex_);
    const SmbShare* m = loginFor(a.protocol, a.host, a.share, a.port);
    if (m == nullptr || m->anonymous || !m->password.empty()) return false;
    // No password: one typed in this session (an empty one confirmed in the sign-in dialog) is
    // used; else a share whose password is not saved, or that has a user name but an empty saved
    // password, asks first.
    const bool typedThisSession = !session_.empty() && m >= session_.data() && m < session_.data() + session_.size();
    if (typedThisSession) return false;
    return !m->savePassword || !m->username.empty();
}

bool SmbCredentialsManager::loginGiven(const std::string& uri) const {
    std::lock_guard<std::mutex> lk(mutex_);
    const SmbShare* m = loginForUri(uri);
    return m != nullptr && !m->anonymous && (!m->username.empty() || !m->password.empty());
}

bool SmbCredentialsManager::loginHasPassword(const std::string& uri) const {
    std::lock_guard<std::mutex> lk(mutex_);
    const SmbShare* m = loginForUri(uri);
    return m != nullptr && !m->anonymous && !m->password.empty();
}

bool SmbCredentialsManager::noteLoginRefused(const std::string& uri) {
    Address a;
    if (!parseAddress(uri, a)) return true;
    std::lock_guard<std::mutex> lk(mutex_);
    const std::size_t stamp = loginStamp(uri);
    for (const auto& w : worked_) {
        if (w.first == a.scope && w.second == stamp) return false;  // the login is fine: no permission there
    }
    for (const auto& r : refused_) {
        if (r.first == a.scope && r.second == stamp) return true;
    }
    if (refused_.size() >= 64) refused_.erase(refused_.begin());
    refused_.emplace_back(a.scope, stamp);
    return true;
}

void SmbCredentialsManager::noteLoginWorked(const std::string& uri) {
    Address a;
    if (!parseAddress(uri, a)) return;
    std::lock_guard<std::mutex> lk(mutex_);
    const std::size_t stamp = loginStamp(uri);
    refused_.erase(std::remove(refused_.begin(), refused_.end(), std::make_pair(a.scope, stamp)), refused_.end());
    for (const auto& w : worked_) {
        if (w.first == a.scope && w.second == stamp) return;
    }
    if (worked_.size() >= 64) worked_.erase(worked_.begin());
    worked_.emplace_back(a.scope, stamp);
}

bool SmbCredentialsManager::loginRefused(const std::string& uri) const {
    Address a;
    if (!parseAddress(uri, a)) return false;
    std::lock_guard<std::mutex> lk(mutex_);
    if (refused_.empty()) return false;
    const std::size_t stamp = loginStamp(uri);
    for (const auto& r : refused_) {
        // The same share / server with the same login (a changed login gets its try).
        if (r.first == a.scope && r.second == stamp) return true;
    }
    return false;
}

bool SmbCredentialsManager::isLoginErrno(int e) { return e == EACCES || e == EPERM; }

} // namespace pixelvr

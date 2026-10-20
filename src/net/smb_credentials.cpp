#include "smb_credentials.hpp"
#include "util/logging.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
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

SmbCredentialsManager& SmbCredentialsManager::instance() {
    static SmbCredentialsManager sInstance;
    return sInstance;
}

SmbCredentialsManager::SmbCredentialsManager() {
    load();
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
    shares_.clear();

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

        std::string passCipher = extractField("password_enc");
        std::string passIv = extractField("iv");
        if (!passCipher.empty() && !passIv.empty()) {
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
        std::string encPass = encryptPassword(s.password, key, ivHex);

        json << "  {\n";
        json << "    \"id\": \"" << s.id << "\",\n";
        json << "    \"name\": \"" << s.name << "\",\n";
        json << "    \"host\": \"" << s.host << "\",\n";
        json << "    \"share\": \"" << s.share << "\",\n";
        json << "    \"workgroup\": \"" << s.workgroup << "\",\n";
        json << "    \"username\": \"" << s.username << "\",\n";
        json << "    \"password_enc\": \"" << encPass << "\",\n";
        json << "    \"iv\": \"" << ivHex << "\",\n";
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

void SmbCredentialsManager::addOrUpdateShare(const SmbShare& share) {
    if (share.host.empty()) return;

    {
        std::lock_guard<std::mutex> lk(mutex_);
        bool found = false;
        for (auto& s : shares_) {
            if (s.host == share.host && s.share == share.share) {
                s = share;
                found = true;
                break;
            }
        }
        if (!found) {
            shares_.push_back(share);
        }
    }
    save();
}

bool SmbCredentialsManager::deleteShare(const std::string& host, const std::string& share) {
    bool removed = false;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = std::remove_if(shares_.begin(), shares_.end(), [&](const SmbShare& s) {
            if (s.host == host) {
                if (share.empty() || s.share == share || s.share.empty()) {
                    return true;
                }
            }
            return false;
        });
        if (it != shares_.end()) {
            shares_.erase(it, shares_.end());
            removed = true;
        }
    }
    if (removed) {
        save();
        PIXELVR_LOG_INFO("SmbCredentialsManager: Deleted share for host '%s'", host.c_str());
    }
    return removed;
}

bool SmbCredentialsManager::findShare(const std::string& hostIn,
                                      const std::string& shareIn,
                                      SmbShare& outShare) const {
    std::lock_guard<std::mutex> lk(mutex_);
    if (hostIn.empty()) return false;

    std::string host = hostIn;
    std::transform(host.begin(), host.end(), host.begin(), ::tolower);
    std::string share = shareIn;
    std::transform(share.begin(), share.end(), share.begin(), ::tolower);

    for (const auto& s : shares_) {
        std::string sHost = s.host;
        std::transform(sHost.begin(), sHost.end(), sHost.begin(), ::tolower);
        if (sHost == host) {
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

    std::string host = hostIn;
    std::transform(host.begin(), host.end(), host.begin(), ::tolower);
    std::string share = shareIn;
    std::transform(share.begin(), share.end(), share.begin(), ::tolower);

    const SmbShare* bestMatch = nullptr;
    for (const auto& s : shares_) {
        std::string sHost = s.host;
        std::transform(sHost.begin(), sHost.end(), sHost.begin(), ::tolower);
        if (sHost == host) {
            std::string sShare = s.share;
            std::transform(sShare.begin(), sShare.end(), sShare.begin(), ::tolower);
            if (!share.empty() && sShare == share) {
                bestMatch = &s; // Exact share match takes highest priority
                break;
            }
            if (sShare.empty() && !bestMatch) {
                bestMatch = &s; // Host-level wildcard match
            }
        }
    }

    if (bestMatch && !bestMatch->anonymous) {
        outUser = bestMatch->username;
        outPass = bestMatch->password;
        outWorkgroup = bestMatch->workgroup.empty() ? "WORKGROUP" : bestMatch->workgroup;
        return true;
    }
    return false;
}

std::string SmbCredentialsManager::getAuthFilePath(const std::string& host, const std::string& share) {
    std::string user, pass, wg;
    if (!findCredentials(host, share, user, pass, wg)) {
        return "";
    }
    if (user.empty()) return "";

    std::string basePath = getStoragePath();
    fs::path p(basePath);
    std::string safeHost = host;
    for (char& c : safeHost) {
        if (!isalnum(c)) c = '_';
    }
    fs::path authFile = p.parent_path() / ("auth_" + safeHost + ".tmp");

    std::ofstream f(authFile.string(), std::ios::trunc);
    if (!f.is_open()) return "";

    f << "username = " << user << "\n";
    f << "password = " << pass << "\n";
    f << "domain = " << (wg.empty() ? "WORKGROUP" : wg) << "\n";
    f.close();

#if defined(__linux__) || defined(__APPLE__)
    chmod(authFile.string().c_str(), 0600);
#endif
    return authFile.string();
}

} // namespace pixelvr

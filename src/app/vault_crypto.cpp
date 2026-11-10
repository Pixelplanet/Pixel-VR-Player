#include "app/vault_crypto.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>

#include <openssl/evp.h>
#include <openssl/rand.h>

namespace fs = std::filesystem;

namespace pixelvr::vaultcrypto {

namespace {

std::mutex keyMu;
uint8_t gKey[32];
bool gHasKey = false;

constexpr char kMagic[6] = {'P', 'V', 'R', 'V', '1', 1};
constexpr int kPbkdfIter = 200000;

// counter = nonce + block (128-bit big-endian add)
void counterAt(const uint8_t nonce[16], uint64_t block, uint8_t out[16]) {
    std::memcpy(out, nonce, 16);
    uint64_t carry = block;
    for (int i = 15; i >= 0 && carry; --i) {
        const uint64_t sum = static_cast<uint64_t>(out[i]) + (carry & 0xFF);
        out[i] = static_cast<uint8_t>(sum);
        carry = (carry >> 8) + (sum >> 8);
    }
}

// XORs `n` bytes at stream position `offset` with the AES-256-CTR keystream (in place).
bool ctrXor(const uint8_t key[32], const uint8_t nonce[16], uint64_t offset, uint8_t* data, size_t n) {
    EVP_CIPHER_CTX* c = EVP_CIPHER_CTX_new();
    if (!c) return false;
    uint8_t iv[16];
    counterAt(nonce, offset / 16, iv);
    bool ok = EVP_EncryptInit_ex(c, EVP_aes_256_ctr(), nullptr, key, iv) == 1;
    int len = 0;
    const size_t skip = offset % 16;
    if (ok && skip) {  // advance inside the first block
        uint8_t pad[16] = {};
        ok = EVP_EncryptUpdate(c, pad, &len, pad, static_cast<int>(skip)) == 1;
    }
    size_t done = 0;
    while (ok && done < n) {
        const int chunk = static_cast<int>(std::min<size_t>(n - done, 1u << 20));
        ok = EVP_EncryptUpdate(c, data + done, &len, data + done, chunk) == 1;
        done += static_cast<size_t>(chunk);
    }
    EVP_CIPHER_CTX_free(c);
    return ok;
}

// The name uses its own counter range (nonce with the top byte flipped).
void nameNonce(const uint8_t nonce[16], uint8_t out[16]) {
    std::memcpy(out, nonce, 16);
    out[0] ^= 0xFF;
}

bool currentKey(uint8_t out[32]) {
    std::lock_guard<std::mutex> l(keyMu);
    if (!gHasKey) return false;
    std::memcpy(out, gKey, 32);
    return true;
}

bool deriveKek(const std::string& combo, const uint8_t salt[16], uint8_t out[32]) {
    return PKCS5_PBKDF2_HMAC(combo.data(), static_cast<int>(combo.size()), salt, 16, kPbkdfIter, EVP_sha256(), 32, out) == 1;
}

// vault.key: salt(16) iv(12) ct(32) tag(16)
bool wrap(const std::string& keyFile, const std::string& combo, const uint8_t master[32], std::string& err) {
    uint8_t salt[16], iv[12], kek[32], ct[32], tag[16];
    if (RAND_bytes(salt, 16) != 1 || RAND_bytes(iv, 12) != 1 || !deriveKek(combo, salt, kek)) {
        err = "random / key derivation failed";
        return false;
    }
    EVP_CIPHER_CTX* c = EVP_CIPHER_CTX_new();
    int len = 0;
    bool ok = c && EVP_EncryptInit_ex(c, EVP_aes_256_gcm(), nullptr, kek, iv) == 1 &&
              EVP_EncryptUpdate(c, ct, &len, master, 32) == 1 && EVP_EncryptFinal_ex(c, ct + len, &len) == 1 &&
              EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_GET_TAG, 16, tag) == 1;
    EVP_CIPHER_CTX_free(c);
    if (!ok) {
        err = "key wrap failed";
        return false;
    }
    const std::string tmp = keyFile + ".tmp";
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        o.write(reinterpret_cast<char*>(salt), 16);
        o.write(reinterpret_cast<char*>(iv), 12);
        o.write(reinterpret_cast<char*>(ct), 32);
        o.write(reinterpret_cast<char*>(tag), 16);
        if (!o) {
            err = "cannot write " + keyFile;
            return false;
        }
    }
    ::chmod(tmp.c_str(), 0600);
    std::error_code ec;
    fs::rename(tmp, keyFile, ec);
    if (ec) err = ec.message();
    return !ec;
}

bool unwrap(const std::string& keyFile, const std::string& combo, uint8_t master[32], std::string& err) {
    std::ifstream in(keyFile, std::ios::binary);
    uint8_t salt[16], iv[12], ct[32], tag[16], kek[32];
    in.read(reinterpret_cast<char*>(salt), 16);
    in.read(reinterpret_cast<char*>(iv), 12);
    in.read(reinterpret_cast<char*>(ct), 32);
    in.read(reinterpret_cast<char*>(tag), 16);
    if (!in) {
        err = "no vault key";
        return false;
    }
    if (!deriveKek(combo, salt, kek)) {
        err = "key derivation failed";
        return false;
    }
    EVP_CIPHER_CTX* c = EVP_CIPHER_CTX_new();
    int len = 0;
    bool ok = c && EVP_DecryptInit_ex(c, EVP_aes_256_gcm(), nullptr, kek, iv) == 1 &&
              EVP_DecryptUpdate(c, master, &len, ct, 32) == 1 &&
              EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_TAG, 16, tag) == 1 && EVP_DecryptFinal_ex(c, master + len, &len) == 1;
    EVP_CIPHER_CTX_free(c);
    if (!ok) err = "wrong combination or damaged vault key";
    return ok;
}

struct RawHeader {
    uint8_t nonce[16];
    uint64_t size;
    std::string name;
};

bool readHeader(int fd, RawHeader& h, const uint8_t key[32]) {
    uint8_t buf[kHeaderSize];
    if (::pread(fd, buf, kHeaderSize, 0) != kHeaderSize || std::memcmp(buf, kMagic, 6) != 0) return false;
    std::memcpy(h.nonce, buf + 6, 16);
    h.size = 0;
    for (int i = 0; i < 8; ++i) h.size |= static_cast<uint64_t>(buf[22 + i]) << (8 * i);
    const size_t nl = static_cast<size_t>(buf[30]) | (static_cast<size_t>(buf[31]) << 8);
    if (nl > kHeaderSize - 32) return false;
    h.name.assign(reinterpret_cast<char*>(buf + 32), nl);
    if (key) {
        uint8_t nn[16];
        nameNonce(h.nonce, nn);
        ctrXor(key, nn, 0, reinterpret_cast<uint8_t*>(h.name.data()), nl);
    }
    return true;
}

}  // namespace

void setKey(const uint8_t key[32]) {
    std::lock_guard<std::mutex> l(keyMu);
    std::memcpy(gKey, key, 32);
    gHasKey = true;
}

void clearKey() {
    std::lock_guard<std::mutex> l(keyMu);
    OPENSSL_cleanse(gKey, 32);
    gHasKey = false;
}

bool hasKey() {
    std::lock_guard<std::mutex> l(keyMu);
    return gHasKey;
}

bool createKeyFile(const std::string& keyFile, const std::string& combo, std::string& err) {
    uint8_t master[32];
    if (RAND_bytes(master, 32) != 1) {
        err = "no random source";
        return false;
    }
    const bool ok = wrap(keyFile, combo, master, err);
    if (ok) setKey(master);
    OPENSSL_cleanse(master, 32);
    return ok;
}

bool unlockKeyFile(const std::string& keyFile, const std::string& combo, std::string& err) {
    uint8_t master[32];
    if (!unwrap(keyFile, combo, master, err)) return false;
    setKey(master);
    OPENSSL_cleanse(master, 32);
    return true;
}

bool rewrapKeyFile(const std::string& keyFile, const std::string& oldCombo, const std::string& newCombo, std::string& err) {
    uint8_t master[32];
    if (!unwrap(keyFile, oldCombo, master, err)) return false;
    const bool ok = wrap(keyFile, newCombo, master, err);
    OPENSSL_cleanse(master, 32);
    return ok;
}

bool isEncrypted(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    char m[6] = {};
    const bool ok = ::pread(fd, m, 6, 0) == 6 && std::memcmp(m, kMagic, 6) == 0;
    ::close(fd);
    return ok;
}

bool encryptFile(const std::string& src, const std::string& dstDir, std::string& outPath, std::string& err,
                 const std::function<void(uint64_t, uint64_t)>& progress) {
    uint8_t key[32];
    if (!currentKey(key)) {
        err = "vault locked";
        return false;
    }
    const int in = ::open(src.c_str(), O_RDONLY | O_CLOEXEC);
    if (in < 0) {
        err = "cannot open " + src;
        return false;
    }
    struct stat st {};
    ::fstat(in, &st);
    const uint64_t total = static_cast<uint64_t>(st.st_size);
    uint8_t rnd[8], nonce[16];
    RAND_bytes(rnd, 8);
    RAND_bytes(nonce, 16);
    nonce[8] = nonce[9] = 0;  // headroom so the counter never wraps into the name range
    char hex[17];
    for (int i = 0; i < 8; ++i) std::snprintf(hex + 2 * i, 3, "%02x", rnd[i]);
    outPath = (fs::path(dstDir) / (std::string(hex) + kExtension)).string();
    const std::string tmp = outPath + ".part";
    const int out = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (out < 0) {
        ::close(in);
        err = "cannot write into the vault";
        return false;
    }
    // header
    std::vector<uint8_t> hdr(kHeaderSize, 0);
    std::memcpy(hdr.data(), kMagic, 6);
    std::memcpy(hdr.data() + 6, nonce, 16);
    for (int i = 0; i < 8; ++i) hdr[22 + i] = static_cast<uint8_t>(total >> (8 * i));
    std::string name = fs::path(src).filename().string();
    if (name.size() > kHeaderSize - 64) name.resize(kHeaderSize - 64);
    hdr[30] = static_cast<uint8_t>(name.size());
    hdr[31] = static_cast<uint8_t>(name.size() >> 8);
    std::memcpy(hdr.data() + 32, name.data(), name.size());
    uint8_t nn[16];
    nameNonce(nonce, nn);
    ctrXor(key, nn, 0, hdr.data() + 32, name.size());
    bool ok = ::write(out, hdr.data(), kHeaderSize) == kHeaderSize;
    std::vector<uint8_t> buf(4u << 20);
    uint64_t done = 0;
    while (ok) {
        const ssize_t r = ::read(in, buf.data(), buf.size());
        if (r < 0) ok = false;
        if (r <= 0) break;
        ok = ctrXor(key, nonce, done, buf.data(), static_cast<size_t>(r)) &&
             ::write(out, buf.data(), static_cast<size_t>(r)) == r;
        done += static_cast<uint64_t>(r);
        if (progress) progress(done, total);
    }
    ok = ok && done == total && ::fsync(out) == 0;
    ::close(in);
    ::close(out);
    OPENSSL_cleanse(key, 32);
    std::error_code ec;
    if (ok) fs::rename(tmp, outPath, ec);
    if (!ok || ec) {
        fs::remove(tmp, ec);
        err = "encrypting failed (disk full?)";
        return false;
    }
    return true;
}

bool decryptFile(const std::string& src, const std::string& dstPath, std::string& err) {
    Reader r;
    if (!r.open(src)) {
        err = "cannot read the vault file";
        return false;
    }
    const std::string tmp = dstPath + ".part";
    std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
    std::vector<uint8_t> buf(4u << 20);
    uint64_t off = 0;
    while (off < r.size()) {
        const long n = r.read(off, buf.data(), buf.size());
        if (n <= 0) break;
        o.write(reinterpret_cast<char*>(buf.data()), n);
        off += static_cast<uint64_t>(n);
    }
    o.close();
    std::error_code ec;
    if (off != r.size() || !o) {
        fs::remove(tmp, ec);
        err = "decrypting failed (disk full?)";
        return false;
    }
    fs::rename(tmp, dstPath, ec);
    if (ec) err = ec.message();
    return !ec;
}

Reader::~Reader() { close(); }

bool Reader::open(const std::string& path) {
    close();
    uint8_t key[32];
    if (!currentKey(key)) return false;
    fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd_ < 0) return false;
    RawHeader h;
    if (!readHeader(fd_, h, key)) {
        OPENSSL_cleanse(key, 32);
        close();
        return false;
    }
    OPENSSL_cleanse(key, 32);
    std::memcpy(nonce_, h.nonce, 16);
    size_ = h.size;
    name_ = h.name;
    return true;
}

void Reader::close() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
}

long Reader::read(uint64_t offset, uint8_t* out, size_t n) {
    if (fd_ < 0) return -1;
    if (offset >= size_) return 0;
    n = static_cast<size_t>(std::min<uint64_t>(n, size_ - offset));
    const ssize_t r = ::pread(fd_, out, n, static_cast<off_t>(kHeaderSize + offset));
    if (r <= 0) return r < 0 ? -1 : 0;
    uint8_t key[32];
    if (!currentKey(key)) return -1;  // locked meanwhile
    const bool ok = ctrXor(key, nonce_, offset, out, static_cast<size_t>(r));
    OPENSSL_cleanse(key, 32);
    return ok ? r : -1;
}

std::string storedName(const std::string& path) {
    uint8_t key[32];
    if (!currentKey(key)) return {};
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return {};
    RawHeader h;
    const bool ok = readHeader(fd, h, key);
    ::close(fd);
    OPENSSL_cleanse(key, 32);
    return ok ? h.name : std::string();
}

bool setStoredName(const std::string& path, const std::string& name) {
    uint8_t key[32];
    if (!currentKey(key) || name.empty() || name.size() > kHeaderSize - 64) return false;
    const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) return false;
    RawHeader h;
    bool ok = readHeader(fd, h, nullptr);
    if (ok) {
        std::vector<uint8_t> field(kHeaderSize - 30, 0);  // length + name + zero padding up to the header end
        field[0] = static_cast<uint8_t>(name.size());
        field[1] = static_cast<uint8_t>(name.size() >> 8);
        std::memcpy(field.data() + 2, name.data(), name.size());
        uint8_t nn[16];
        nameNonce(h.nonce, nn);
        ctrXor(key, nn, 0, field.data() + 2, name.size());
        ok = ::pwrite(fd, field.data(), field.size(), 30) == static_cast<ssize_t>(field.size()) && ::fsync(fd) == 0;
    }
    ::close(fd);
    OPENSSL_cleanse(key, 32);
    return ok;
}

}  // namespace pixelvr::vaultcrypto

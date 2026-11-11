// Vault encryption: AES-256-CTR with the CPU's AES instructions (OpenSSL libcrypto).
//
// Encrypted file (".pvrv", random file name): a 4096-byte header, then the contents.
//   header: "PVRV1" + version byte, 16-byte nonce, plain size (u64 LE), name length (u16 LE),
//           the original file name (encrypted with a separate counter range), zero padding.
//   contents: AES-256-CTR, counter = nonce + offset / 16, so any offset can be read directly (seeking).
// The master key is random; vault.key stores it wrapped (AES-256-GCM) with a key derived from the
// unlock combination (PBKDF2-SHA256), so a combination change only re-wraps it.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace pixelvr::vaultcrypto {

constexpr int kHeaderSize = 4096;
constexpr const char* kExtension = ".pvrv";

// The active master key (set after unlocking; cleared on lock).
void setKey(const uint8_t key[32]);
void clearKey();
bool hasKey();

// vault.key: create (new random master key) or unwrap with the combination text.
bool createKeyFile(const std::string& keyFile, const std::string& combo, std::string& err);
bool unlockKeyFile(const std::string& keyFile, const std::string& combo, std::string& err);
bool rewrapKeyFile(const std::string& keyFile, const std::string& oldCombo, const std::string& newCombo, std::string& err);

bool isEncrypted(const std::string& path);  // has the PVRV1 header
bool hasExtension(const std::string& path);  // ends in ".pvrv" (no file access)

using Progress = std::function<void(uint64_t done, uint64_t total)>;
using Cancelled = std::function<bool()>;  // true = stop (the partial output is removed)

// Encrypts `src` into `dstDir`; outPath = the new file. The stored (real) name is `storedName`, or the
// source's file name; the file name on disk is `diskName`, or a random one. File and folder are synced.
bool encryptFile(const std::string& src, const std::string& dstDir, std::string& outPath, std::string& err,
                 const Progress& progress = {}, const std::string& storedName = {},
                 const Cancelled& cancelled = {}, const std::string& diskName = {});
// Decrypts an encrypted file back to a plain one (moving out of the vault); synced before it returns.
bool decryptFile(const std::string& src, const std::string& dstPath, std::string& err,
                 const Progress& progress = {}, const Cancelled& cancelled = {});
// Copies an encrypted file into `dstDir` with a FRESH nonce (a byte copy would share its keystream with
// the original: renaming one of them later would expose both names). Same options as encryptFile.
bool reencryptFile(const std::string& src, const std::string& dstDir, std::string& outPath, std::string& err,
                   const Progress& progress = {}, const std::string& storedName = {},
                   const Cancelled& cancelled = {}, const std::string& diskName = {});
// Small buffers (thumbnails): nonce + AES-256-CTR. False without the key.
bool encryptBlob(const std::string& plain, std::string& out);
bool decryptBlob(const std::string& blob, std::string& out);
// fsync of a folder (makes a rename / new file in it durable).
void fsyncDir(const std::string& dir);

// Reading an encrypted file at any offset.
class Reader {
public:
    ~Reader();
    bool open(const std::string& path);
    void close();
    uint64_t size() const { return size_; }
    const std::string& name() const { return name_; }
    // Reads up to n plain bytes at `offset`; returns the count (0 at the end, -1 on error).
    long read(uint64_t offset, uint8_t* out, size_t n);

private:
    int fd_ = -1;
    uint64_t size_ = 0;
    uint8_t nonce_[16] = {};
    std::string name_;
};

// The original name stored in an encrypted file's header ("" if unreadable / no key).
std::string storedName(const std::string& path);
// Changes that stored name (renaming inside the vault; the file name on disk stays random).
bool setStoredName(const std::string& path, const std::string& name);

}  // namespace pixelvr::vaultcrypto

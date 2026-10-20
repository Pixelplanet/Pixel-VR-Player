#pragma once

#include <string>
#include <vector>
#include <mutex>

namespace pixelvr {

struct SmbShare {
    std::string id;         // Unique identifier (typically host or host:share)
    std::string name;       // User-friendly display label (e.g. "Home NAS")
    std::string host;       // IP address or hostname (e.g. "192.168.1.50")
    std::string share;      // Optional default share name (e.g. "movies", or empty for all)
    std::string workgroup;  // Workgroup/Domain (defaults to "WORKGROUP")
    std::string username;   // SMB Username (or empty for anonymous)
    std::string password;   // SMB Password (decrypted in memory, encrypted on disk)
    bool anonymous = false; // True for guest / public shares
};

class SmbCredentialsManager {
public:
    static SmbCredentialsManager& instance();

    // Loads configured shares from ~/.config/pixelvr/smb_shares.json
    void load();

    // Saves configured shares to ~/.config/pixelvr/smb_shares.json with strict 0600 permissions
    void save();

    // Returns all configured SMB shares
    std::vector<SmbShare> shares() const;

    // Adds a new share or updates an existing one (matched by host + share)
    void addOrUpdateShare(const SmbShare& share);

    // Deletes a configured share and wipes its stored credentials
    bool deleteShare(const std::string& host, const std::string& share = "");

    // Lookup a configured share
    bool findShare(const std::string& host,
                   const std::string& share,
                   SmbShare& outShare) const;

    // Lookup credentials for a given host and optional share
    bool findCredentials(const std::string& host,
                         const std::string& share,
                         std::string& outUser,
                         std::string& outPass,
                         std::string& outWorkgroup) const;

    // Generates or updates a secure temporary Samba auth file (mode 0600) for smbclient -A
    // Returns the absolute file path, or empty string if anonymous / no credentials.
    std::string getAuthFilePath(const std::string& host, const std::string& share);

private:
    SmbCredentialsManager();
    ~SmbCredentialsManager() = default;

    std::string getStoragePath() const;
    std::string getDeviceKey() const;

    std::string encryptPassword(const std::string& plain, const std::string& key, std::string& outIv) const;
    std::string decryptPassword(const std::string& cipherHex, const std::string& key, const std::string& ivHex) const;

    mutable std::mutex mutex_;
    std::vector<SmbShare> shares_;
};

} // namespace pixelvr

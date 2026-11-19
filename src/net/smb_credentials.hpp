#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>
#include <mutex>

namespace pixelvr {

struct SmbShare {
    std::string id;         // Unique identifier (typically host or host:share)
    std::string name;       // User-friendly display label (e.g. "Home NAS")
    std::string host;       // IP address or hostname (e.g. "192.168.1.50")
    std::string share;      // Optional default share name (e.g. "movies", or empty for all); start folder for sftp/dav
    std::string workgroup;  // Workgroup/Domain (defaults to "WORKGROUP")
    std::string username;   // SMB Username (or empty for anonymous)
    std::string password;   // SMB Password (decrypted in memory, encrypted on disk)
    bool anonymous = false; // True for guest / public shares
    std::string protocol = "smb";  // "smb" | "sftp" | "dav" | "davs"
    int port = 0;                  // 0 = the protocol's default (22 / 80 / 443)
    // Off: the password is never written to the file; one typed in is kept in memory until the
    // player quits, and the share asks again after that.
    bool savePassword = true;
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

    // Adds a new share or updates an existing one (matched by host + share). With savePassword off
    // its password is kept in memory for this session only (none: the share asks when opened).
    void addOrUpdateShare(const SmbShare& share);

    // Deletes a configured share and wipes its stored credentials (and a session password)
    bool deleteShare(const std::string& host, const std::string& share = "",
                     const std::string& protocol = "smb", int port = 0);

    // Lookup a configured share (as saved: a session password is not part of it)
    bool findShare(const std::string& host,
                   const std::string& share,
                   SmbShare& outShare,
                   const std::string& protocol = "smb",
                   int port = 0) const;

    // Lookup of an SFTP / WebDAV server by protocol, host and port (a session login first).
    bool findRemote(const std::string& protocol, const std::string& host, int port, SmbShare& out) const;

    // Lookup credentials for a given SMB host and optional share (a session login first)
    bool findCredentials(const std::string& host,
                         const std::string& share,
                         std::string& outUser,
                         std::string& outPass,
                         std::string& outWorkgroup) const;

    // The text of a Samba auth file (username / password / domain lines) for smbclient -A, handed
    // over through a pipe so no password lands on the disk. Empty for guest access.
    std::string authFileText(const std::string& host, const std::string& share) const;

    // --- Signing in when a share asks for it ---
    // Where a network address signs in: the saved share whose login it uses, else the address's own
    // share (SMB) or server (SFTP / WebDAV).
    struct LoginTarget {
        std::string protocol = "smb";
        std::string host;
        std::string share;          // SMB share ("" = the whole server); SFTP / WebDAV: the saved start folder
        int port = 0;
        bool saved = false;         // a saved share covers the address
        std::string username;       // the user name in use (saved or typed this session)
        bool savePassword = true;   // the saved share's setting (on for a share that is not saved)
        std::string label;          // for messages: the share's nickname, else host/share
    };
    bool loginTargetFor(const std::string& uri, LoginTarget& out) const;
    // A login typed in the sign-in dialog, in memory: every lookup uses it from now on. Allows one
    // more try where a login was refused before.
    void setSessionLogin(const LoginTarget& t, const std::string& user, const std::string& pass);
    // That login worked: keep it as the switch says. On: saved with the share (a share that was not
    // saved is added). Off: the password stays in memory only; a saved share keeps the user name.
    void keepSessionLogin(const LoginTarget& t, bool savePassword);
    // That login was refused: forget it (the saved login, if any, applies again).
    void dropSessionLogin(const LoginTarget& t);

    // No password is in use for a share with a user name (not saved, or saved empty) and none was
    // typed in this session: ask before trying, so no logon with an empty password is sent. Not for
    // SFTP, which may sign in with a key.
    bool loginMissing(const std::string& uri) const;
    // A user name or password is in use for the address (else guest access).
    bool loginGiven(const std::string& uri) const;
    // A password is in use for the address (so a refusal means "not accepted", not "needs one").
    bool loginHasPassword(const std::string& uri) const;
    // Logins are remembered per SMB share (per server for an SMB share list, SFTP and WebDAV), the
    // scope a server checks a login in, together with which login it was.
    //
    // An access at `uri` was refused (EACCES / EPERM, HTTP 401 / 403). If this same login already
    // worked there this session, it is a missing permission (a folder or file ACL), not the login:
    // nothing is recorded and false is returned. Otherwise the refusal is recorded and true is
    // returned: further automatic tries anywhere in that share / on that server wait until the
    // login changes, so a wrong password is not sent again and again (account lockout).
    bool noteLoginRefused(const std::string& uri);
    // An access at `uri` worked with the login in use (a folder listed, a file opened).
    void noteLoginWorked(const std::string& uri);
    bool loginRefused(const std::string& uri) const;
    bool loginBlocked(const std::string& uri) const { return loginMissing(uri) || loginRefused(uri); }
    // libsmbclient's errno for a refused or missing login (logon failure, access denied).
    static bool isLoginErrno(int e);

private:
    SmbCredentialsManager();
    ~SmbCredentialsManager() = default;

    std::string getStoragePath() const;
    std::string getDeviceKey() const;
    void saveLocked();

    std::string encryptPassword(const std::string& plain, const std::string& key, std::string& outIv) const;
    std::string decryptPassword(const std::string& cipherHex, const std::string& key, const std::string& ivHex) const;

    // The entry whose login an address uses (session logins first), nullptr = none (guest).
    const SmbShare* loginFor(const std::string& protocol, const std::string& host, const std::string& share,
                             int port) const;
    const SmbShare* loginForUri(const std::string& uri) const;
    std::size_t loginStamp(const std::string& uri) const;  // tells logins apart (memory only)
    // Forgets the refusals recorded in this scope key and below (all logins, or only `stamp`).
    void forgetRefusedLocked(const std::string& scope, const std::size_t* stamp);

    mutable std::mutex mutex_;
    std::vector<SmbShare> shares_;   // saved (a share with savePassword off holds no password here)
    std::vector<SmbShare> session_;  // logins typed this session: never written anywhere
    // Scope key (share or server), login stamp: logins refused there / that worked there.
    std::vector<std::pair<std::string, std::size_t>> refused_;
    std::vector<std::pair<std::string, std::size_t>> worked_;
};

} // namespace pixelvr

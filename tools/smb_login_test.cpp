// Credential logic of network shares (SmbCredentialsManager): "Save password" on / off, logins
// typed for one session, refused logins, the shares file. Development tool, never released. Runs
// in a scratch HOME (never the user's shares file), in two phases so a "player restart" is a new
// process:
//
//   smb_login_test <scratch dir> write   (then)   smb_login_test <scratch dir> read
//
// Only made-up hosts and passwords; nothing here talks to a server. Passwords are never printed.

#include "app/vault_manager.hpp"
#include "net/smb_credentials.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <sys/stat.h>

namespace fs = std::filesystem;
using pixelvr::SmbCredentialsManager;
using pixelvr::SmbShare;

namespace {

int failures = 0;
void check(bool ok, const std::string& what) {
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++failures;
}

const char* const kSavedPw = "Saved-Pw-1234";
const char* const kSessionPw = "Session-Pw-5678";
const char* const kTypedPw = "Typed-Pw-9012";
const char* const kNewSharePw = "Found-Pw-3456";
const char* const kVaultPw = "Vault-Pw-7788";
const char* const kVaultUser = "vaultuser";
const char* const kVaultHost = "vault.test";
const char* const kVaultLabel = "Hidden NAS";

bool listed(const std::string& host) {
    for (const SmbShare& s : SmbCredentialsManager::instance().shares()) {
        if (s.host == host) return true;
    }
    return false;
}

bool privateUri(const std::string& uri) { return pixelvr::VaultManager::isPrivatePath(uri); }

std::string fileText(const std::string& path) {
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// The JSON object of the share with this host (the file is written one field per line).
std::string objectOf(const std::string& json, const std::string& host) {
    const std::size_t h = json.find("\"host\": \"" + host + "\"");
    if (h == std::string::npos) return "";
    const std::size_t b = json.rfind('{', h);
    const std::size_t e = json.find('}', h);
    return json.substr(b, e - b + 1);
}

std::string fieldOf(const std::string& obj, const std::string& name) {
    const std::size_t k = obj.find("\"" + name + "\": ");
    if (k == std::string::npos) return "<missing>";
    std::size_t v = k + name.size() + 4;
    std::size_t end = obj.find_first_of(",\n", v);
    std::string s = obj.substr(v, end - v);
    if (s.size() >= 2 && s.front() == '"') s = s.substr(1, s.size() - 2);
    return s;
}

std::string passOf(const std::string& host, const std::string& share) {
    std::string u, p, w;
    SmbCredentialsManager::instance().findCredentials(host, share, u, p, w);
    return p;
}

SmbShare makeShare(const std::string& host, const std::string& share, const std::string& user, const std::string& pw,
                   bool save) {
    SmbShare s;
    s.host = host;
    s.share = share;
    s.username = user;
    s.password = pw;
    s.workgroup = "WORKGROUP";
    s.name = host + (share.empty() ? "" : "/" + share);
    s.id = s.name;
    s.savePassword = save;
    return s;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: smb_login_test <scratch dir> write|read\n");
        return 2;
    }
    const fs::path home = fs::absolute(argv[1]);
    const std::string phase = argv[2];
    fs::create_directories(home);
    setenv("HOME", home.c_str(), 1);  // before the manager first loads
    const std::string file = (home / ".config/pixelvr/smb_shares.json").string();
    const fs::path staleAuth = home / ".config/pixelvr/auth_stale_host.tmp";

    if (phase == "write") {
        std::error_code ec;
        fs::remove_all(home / ".config", ec);
        fs::remove_all(home / ".pixelvr_vault", ec);
        // A shares file as older versions wrote it: no "save_password" field.
        {
            SmbCredentialsManager& m = SmbCredentialsManager::instance();
            m.addOrUpdateShare(makeShare("old.test", "media", "olduser", kSavedPw, true));
            std::string json = fileText(file);
            const std::string line = "    \"save_password\": true,\n";
            const std::size_t at = json.find(line);
            check(at != std::string::npos, "the file carries save_password");
            if (at != std::string::npos) json.erase(at, line.size());
            std::ofstream(file, std::ios::trunc) << json;
            m.load();
            SmbShare s;
            check(m.findShare("old.test", "media", s) && s.savePassword, "old file: the share loads, saving on");
            check(passOf("old.test", "media") == kSavedPw, "old file: its saved password still works");
        }
        SmbCredentialsManager& m = SmbCredentialsManager::instance();

        // Saving off, with a password typed in the share form: memory only.
        m.addOrUpdateShare(makeShare("nosave.test", "", "alice", kSessionPw, false));
        {
            const std::string obj = objectOf(fileText(file), "nosave.test");
            check(!obj.empty(), "save off: the share is in the file");
            check(fieldOf(obj, "password_enc").empty() && fieldOf(obj, "iv").empty(), "save off: no password in the file");
            check(fieldOf(obj, "save_password") == "false", "save off: the file says so");
            check(fieldOf(obj, "username") == "alice", "save off: the user name is saved");
            check(passOf("nosave.test", "movies") == kSessionPw, "save off: this session uses the typed password");
            check(!m.loginMissing("smb://nosave.test/movies"), "save off: not asked again this session");
        }
        // Switching saving off for a share that had a saved password removes it from the file.
        m.addOrUpdateShare(makeShare("switch.test", "tv", "bob", kSavedPw, true));
        check(!fieldOf(objectOf(fileText(file), "switch.test"), "password_enc").empty(), "save on: password stored");
        m.addOrUpdateShare(makeShare("switch.test", "tv", "bob", "", false));
        check(fieldOf(objectOf(fileText(file), "switch.test"), "password_enc").empty(), "switched off: stored password removed");
        check(m.loginMissing("smb://switch.test/tv/folder"), "switched off, nothing typed: asks when opened");
        check(!m.loginMissing("smb://old.test/media"), "saved password: not asked");

        // The sign-in dialog on a saved share with saving off: kept in memory only.
        SmbCredentialsManager::LoginTarget t;
        check(m.loginTargetFor("smb://switch.test/tv/folder/sub", t) && t.saved && t.share == "tv" && !t.savePassword &&
                  t.username == "bob",
              "sign-in target: the saved share, its user name and setting");
        m.setSessionLogin(t, "bob", kTypedPw);
        check(passOf("switch.test", "tv") == kTypedPw, "sign-in: the typed password is used at once");
        m.keepSessionLogin(t, false);
        check(fieldOf(objectOf(fileText(file), "switch.test"), "password_enc").empty(), "kept with saving off: not in the file");
        check(passOf("switch.test", "tv") == kTypedPw, "kept with saving off: works for this session");

        // Editing the share forgets the session password.
        m.addOrUpdateShare(makeShare("switch.test", "tv", "bob", "", false));
        check(passOf("switch.test", "tv").empty() && m.loginMissing("smb://switch.test/tv"),
              "edited: the session password is forgotten");
        // Sign in again, then delete: forgotten too.
        m.setSessionLogin(t, "bob", kTypedPw);
        m.keepSessionLogin(t, false);
        m.deleteShare("switch.test", "tv");
        check(passOf("switch.test", "tv").empty(), "deleted: the session password is forgotten");

        // A share found by browsing (not saved): saving off writes nothing, saving on adds it.
        const std::size_t before = m.shares().size();
        check(m.loginTargetFor("smb://found.test/photos/2024", t) && !t.saved && t.share == "photos" && t.savePassword,
              "not saved: the address's own share, saving on by default");
        m.setSessionLogin(t, "carol", kNewSharePw);
        m.keepSessionLogin(t, false);
        check(m.shares().size() == before && fileText(file).find("found.test") == std::string::npos,
              "not saved, saving off: nothing written");
        check(passOf("found.test", "photos") == kNewSharePw, "not saved, saving off: works for this session");
        m.loginTargetFor("smb://found2.test/docs", t);
        m.setSessionLogin(t, "dave", kNewSharePw);
        m.keepSessionLogin(t, true);
        check(m.shares().size() == before + 1 && !fieldOf(objectOf(fileText(file), "found2.test"), "password_enc").empty(),
              "not saved, saving on: added with its password");
        // A refused login is dropped again: the share is back to what it had.
        m.loginTargetFor("smb://old.test/media", t);
        m.setSessionLogin(t, "olduser", "wrong");
        m.dropSessionLogin(t);
        check(passOf("old.test", "media") == kSavedPw, "dropped sign-in: the saved login applies again");

        // Refused logins: remembered per share and login (the scope the server checks a login in).
        check(m.noteLoginRefused("smb://old.test/media/private/film.mkv"), "refused: recorded as a login problem");
        check(m.loginRefused("smb://old.test/media/private/sub") && m.loginRefused("smb://old.test/media") &&
                  m.loginRefused("smb://old.test/media/other/song.flac"),
              "refused: every folder and file of the share is blocked");
        check(!m.loginRefused("smb://old.test/tv") && !m.loginRefused("smb://old.test"),
              "refused: other shares and the share list are not");
        m.loginTargetFor("smb://old.test/media", t);
        m.setSessionLogin(t, "olduser", kTypedPw);
        check(!m.loginRefused("smb://old.test/media/private"), "a newly typed login gets its try");
        m.dropSessionLogin(t);
        check(m.loginRefused("smb://old.test/media"), "that login refused and dropped: the saved one is still blocked");
        m.setSessionLogin(t, "olduser", kSavedPw);
        check(!m.loginRefused("smb://old.test/media"), "the refused login typed again by hand gets one try");
        m.noteLoginRefused("smb://old.test/media");
        m.dropSessionLogin(t);
        check(m.loginRefused("smb://old.test/media"), "refused again: blocked again");
        check(!m.loginRefused("sftp://x.test/home"), "other servers are not affected");
        // SFTP / WebDAV: one login per server.
        m.noteLoginRefused("dav://dav.test:8080/a/b/file.mkv");
        check(m.loginRefused("dav://dav.test:8080/other") && !m.loginRefused("dav://dav.test:8081/a"),
              "WebDAV: blocked on that server (and port) only");

        // A login that worked in the share: access denied is a permission, not the login.
        m.noteLoginWorked("smb://old.test/tv/films");
        check(!m.noteLoginRefused("smb://old.test/tv/System Volume Information"),
              "worked in the share: an access-denied folder is no login problem");
        check(!m.loginRefused("smb://old.test/tv/films"), "worked in the share: nothing is blocked");
        check(m.noteLoginRefused("smb://old.test/docs"), "another share: the login has not worked there");

        // A user name with an empty saved password: asked first (no empty-password logon sent).
        m.addOrUpdateShare(makeShare("empty.test", "media", "erin", "", true));
        check(m.loginMissing("smb://empty.test/media/x") && m.loginGiven("smb://empty.test/media") &&
                  !m.loginHasPassword("smb://empty.test/media"),
              "user name, empty saved password: asks, says it needs a password");
        check(m.loginHasPassword("smb://old.test/media"), "a saved password is in use");
        m.loginTargetFor("smb://empty.test/media", t);
        m.setSessionLogin(t, "erin", "");
        check(!m.loginMissing("smb://empty.test/media"), "an empty password confirmed in the dialog is tried");
        m.keepSessionLogin(t, true);
        check(!m.loginMissing("smb://empty.test/media"), "kept: not asked again this session");

        // Saving switched on in the form (whose password field is empty) keeps the session password.
        m.addOrUpdateShare(makeShare("turnon.test", "music", "frank", "", false));
        m.loginTargetFor("smb://turnon.test/music", t);
        m.setSessionLogin(t, "frank", kTypedPw);
        m.keepSessionLogin(t, false);
        m.addOrUpdateShare(makeShare("turnon.test", "music", "frank", "", true));
        check(passOf("turnon.test", "music") == kTypedPw &&
                  !fieldOf(objectOf(fileText(file), "turnon.test"), "password_enc").empty(),
              "saving switched on: the session password is saved, not an empty one");

        // smbclient gets the login as text (through a pipe), never as a file.
        check(!m.authFileText("old.test", "media").empty(), "auth text for a saved login");
        check(m.authFileText("guest.test", "public").empty(), "no auth text for guest access");
        {
            bool authFile = false;
            for (const auto& e : fs::directory_iterator(home / ".config/pixelvr")) {
                if (e.path().filename().string().rfind("auth_", 0) == 0) authFile = true;
            }
            check(!authFile, "no auth file written");
        }
        // For the next phase: an auth file as older versions left it.
        std::ofstream(staleAuth.string()) << "username = x\n";

        // --- Vault shares (the vault of this scratch HOME, a fresh key) ---
        pixelvr::VaultManager& vm = pixelvr::VaultManager::instance();
        {
            SmbShare early = makeShare(kVaultHost, "secret", kVaultUser, kVaultPw, true);
            early.vault = true;
            check(!m.addOrUpdateShare(early) && !listed(kVaultHost), "vault locked: a vault share can't be added");
        }
        vm.unlock();
        check(vm.isUnlocked() && m.vaultSharesOpen(), "vault unlocked: its share list is open");
        SmbShare vs = makeShare(kVaultHost, "secret", kVaultUser, kVaultPw, true);
        vs.name = kVaultLabel;
        vs.vault = true;
        check(m.addOrUpdateShare(vs), "vault share saved");
        {
            const std::string json = fileText(file);
            check(json.find(kVaultHost) == std::string::npos && json.find(kVaultUser) == std::string::npos &&
                      json.find(kVaultLabel) == std::string::npos,
                  "vault share: not in smb_shares.json");
            const std::string raw = fileText(vm.sharesFilePath());
            check(!raw.empty(), "vault share: the encrypted list exists");
            check(raw.find(kVaultHost) == std::string::npos && raw.find(kVaultUser) == std::string::npos &&
                      raw.find(kVaultPw) == std::string::npos && raw.find(kVaultLabel) == std::string::npos &&
                      raw.find("PVRSHARES") == std::string::npos,
                  "vault share: host, user, password and name not in plain text in the encrypted list");
            struct stat vst{};
            check(stat(vm.sharesFilePath().c_str(), &vst) == 0 && (vst.st_mode & 0777) == 0600, "the encrypted list is 0600");
        }
        check(listed(kVaultHost) && passOf(kVaultHost, "secret") == kVaultPw, "unlocked: listed, its login works");
        check(privateUri("smb://vault.test/secret/Film.mkv") && privateUri("smb://VAULT.test/Secret/a/b") &&
                  privateUri("smb://vault.test/secret"),
              "addresses in the vault share are private");
        check(!privateUri("smb://vault.test/other/x.mkv") && !privateUri("smb://vault.test") &&
                  !privateUri("smb://old.test/media/x.mkv"),
              "other shares of that server, the share list and other servers are not");
        // SFTP / WebDAV: the whole server (with its port) is private.
        SmbShare sv = makeShare("vsftp.test", "/srv", "sv", kVaultPw, true);
        sv.protocol = "sftp";
        sv.port = 2222;
        sv.vault = true;
        m.addOrUpdateShare(sv);
        check(privateUri("sftp://vsftp.test:2222/home/x/a.mkv") && !privateUri("sftp://vsftp.test/home/x/a.mkv"),
              "SFTP vault share: that server and port only");
        // A whole SMB server in the vault: a share found on it and signed into is saved in the vault too.
        SmbShare whole = makeShare("vsrv.test", "", "w", kVaultPw, true);
        whole.vault = true;
        m.addOrUpdateShare(whole);
        SmbCredentialsManager::LoginTarget ft;
        m.loginTargetFor("smb://vsrv.test/movies/x", ft);
        m.setSessionLogin(ft, "found", kTypedPw);
        m.keepSessionLogin(ft, true);
        check(fileText(file).find("vsrv.test") == std::string::npos && listed("vsrv.test") &&
                  passOf("vsrv.test", "movies") == kTypedPw,
              "signed into a share of a vault server with saving on: saved in the vault, not smb_shares.json");
        m.addOrUpdateShare(makeShare("vsrv.test", "music", "w2", kSavedPw, true));  // (asked for as a normal share)
        check(fileText(file).find("vsrv.test") == std::string::npos, "a share added on a vault server stays in the vault");
        // The log filter.
        {
            const std::string a = SmbCredentialsManager::redactForLog("Loaded smb://vault.test/secret/My Film.mkv on screen 1");
            check(a.find("vault.test") == std::string::npos && a.find("My Film") == std::string::npos &&
                      a.find("(private)") != std::string::npos,
                  "log: an unquoted vault address is cut out to the end of the line (" + a + ")");
            const std::string b = SmbCredentialsManager::redactForLog("beginScan 'smb://vault.test/secret/A B' smb=1");
            check(b == "beginScan '(private)' smb=1", "log: a quoted vault address is replaced (" + b + ")");
            const std::string c = SmbCredentialsManager::redactForLog("Loaded smb://old.test/media/x.mkv on screen 1");
            check(c == "Loaded smb://old.test/media/x.mkv on screen 1", "log: other addresses stay");
            const std::string d = SmbCredentialsManager::redactForLog("Download: cannot open smb://vault.test/sec%72et/x: denied");
            check(d.find("vault.test") == std::string::npos, "log: an escaped vault address is cut out too");
        }
        // A login typed for it and a refused one: both forgotten when the vault locks.
        SmbCredentialsManager::LoginTarget vt;
        check(m.loginTargetFor("smb://vault.test/secret/x", vt) && vt.saved && vt.label == kVaultLabel,
              "the sign-in dialog finds the vault share while unlocked");
        m.setSessionLogin(vt, kVaultUser, kTypedPw);
        m.noteLoginRefused("smb://vault.test/secret/x");
        check(m.loginRefused("smb://vault.test/secret/y"), "a refused login is recorded in the vault share");
        vm.lock();
        check(!listed(kVaultHost) && !listed("vsrv.test") && !listed("vsftp.test"), "locked: no vault share is listed");
        check(passOf(kVaultHost, "secret").empty() && passOf("vsrv.test", "movies").empty(),
              "locked: their logins (saved and typed this session) are forgotten");
        check(!m.loginRefused("smb://vault.test/secret/x"), "locked: the refused-login record is forgotten");
        check(!m.loginTargetFor("smb://vault.test/secret/x", vt) || (!vt.saved && vt.label != kVaultLabel),
              "locked: the sign-in dialog knows nothing about it");
        check(privateUri("smb://vault.test/secret/Film.mkv"), "locked: its addresses stay private until the player quits");
        check(!m.addOrUpdateShare(vs), "locked: a vault share can't be saved");
        check(fileText(file).find(kVaultHost) == std::string::npos, "locked: still not in smb_shares.json");
        vm.unlock();
        check(listed(kVaultHost) && passOf(kVaultHost, "secret") == kVaultPw, "unlocked again: back with its password");
        // Out of the vault and back in.
        SmbShare normal = vs;
        normal.vault = false;
        m.addOrUpdateShare(normal);
        check(fileText(file).find(kVaultHost) != std::string::npos && !privateUri("smb://vault.test/secret/x"),
              "made normal: in smb_shares.json, no longer private");
        m.addOrUpdateShare(vs);
        check(fileText(file).find(kVaultHost) == std::string::npos && privateUri("smb://vault.test/secret/x"),
              "made a vault share again: out of smb_shares.json, private");
        vm.lock();  // (the next phase starts locked, like a restart)

        struct stat st{};
        check(stat(file.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600, "the shares file is 0600");
        const std::string all = fileText(file);
        check(all.find(kSessionPw) == std::string::npos && all.find(kTypedPw) == std::string::npos &&
                  all.find(kSavedPw) == std::string::npos && all.find(kNewSharePw) == std::string::npos &&
                  all.find(kVaultPw) == std::string::npos,
              "no password in plain text in the file");
    } else {
        // "After a restart": a new process loads the file.
        SmbCredentialsManager& m = SmbCredentialsManager::instance();
        check(!fs::exists(staleAuth), "an old auth file is removed at start");
        check(passOf("nosave.test", "x").empty(), "restart, saving off: the session password is gone");
        check(m.loginMissing("smb://nosave.test/x"), "restart, saving off: the first access asks");
        SmbCredentialsManager::LoginTarget t;
        check(m.loginTargetFor("smb://nosave.test/x", t) && t.username == "alice" && !t.savePassword,
              "restart: the dialog is prefilled with the saved user name");
        check(passOf("old.test", "media") == kSavedPw, "restart: the saved password still works");
        check(passOf("found2.test", "docs") == kNewSharePw, "restart: a share added with saving on works");
        check(passOf("found.test", "photos").empty(), "restart: a share signed into without saving is gone");
        check(!m.loginMissing("sftp://nosave.test/x"), "SFTP is never asked before trying (keys)");
        check(m.loginMissing("smb://empty.test/media"), "restart: an empty saved password asks again");
        check(passOf("turnon.test", "music") == kTypedPw, "restart: the password saved by switching on works");

        // Vault shares after a restart: nothing of them until the vault is unlocked.
        pixelvr::VaultManager& vm = pixelvr::VaultManager::instance();
        check(!listed(kVaultHost) && !listed("vsrv.test") && passOf(kVaultHost, "secret").empty(),
              "restart, locked: no vault share is known");
        check(fileText(file).find(kVaultHost) == std::string::npos, "restart: not in smb_shares.json");
        vm.unlock();
        check(listed(kVaultHost) && passOf(kVaultHost, "secret") == kVaultPw && listed("vsftp.test"),
              "restart, unlocked: the vault shares come back from the encrypted list");
        check(privateUri("smb://vault.test/secret/x.mkv"), "restart, unlocked: their addresses are private");
        vm.lock();
        // pixelvr --vault-export: the vault's shares become normal shares.
        const int rc = pixelvr::VaultManager::runCli("export", (home / "export").string());
        const std::string json = fileText(file);
        check(rc == 0 && json.find(kVaultHost) != std::string::npos && json.find("vsftp.test") != std::string::npos &&
                  !fs::exists(vm.sharesFilePath()),
              "vault export: the vault shares move to smb_shares.json, the encrypted list is gone");
        check(json.find(kVaultPw) == std::string::npos, "vault export: no password in plain text");
    }
    std::printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}

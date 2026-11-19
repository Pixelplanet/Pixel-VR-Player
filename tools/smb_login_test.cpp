// Credential logic of network shares (SmbCredentialsManager): "Save password" on / off, logins
// typed for one session, refused logins, the shares file. Development tool, never released. Runs
// in a scratch HOME (never the user's shares file), in two phases so a "player restart" is a new
// process:
//
//   smb_login_test <scratch dir> write   (then)   smb_login_test <scratch dir> read
//
// Only made-up hosts and passwords; nothing here talks to a server. Passwords are never printed.

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

        // Refused logins: remembered per folder and login, cleared by a new login.
        m.noteLoginRefused("smb://old.test/media/private");
        check(m.loginRefused("smb://old.test/media/private/sub"), "refused: the folder below is blocked too");
        check(!m.loginRefused("smb://old.test/media"), "refused: the share's other folders are not");
        m.noteLoginRefused("smb://old.test/media/private/film.mkv", true);
        check(m.loginRefused("smb://old.test/media/private/other.mkv"), "refused file: its folder is remembered");
        m.loginTargetFor("smb://old.test/media", t);
        m.setSessionLogin(t, "olduser", kTypedPw);
        check(!m.loginRefused("smb://old.test/media/private"), "a newly typed login gets its try");
        m.dropSessionLogin(t);
        check(!m.loginRefused("sftp://x.test/home"), "other servers are not affected");

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

        struct stat st{};
        check(stat(file.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600, "the shares file is 0600");
        const std::string all = fileText(file);
        check(all.find(kSessionPw) == std::string::npos && all.find(kTypedPw) == std::string::npos &&
                  all.find(kSavedPw) == std::string::npos && all.find(kNewSharePw) == std::string::npos,
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
    }
    std::printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}

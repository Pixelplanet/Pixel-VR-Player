#include "net/smb_credentials.hpp"
#include <cassert>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <sys/stat.h>

using namespace pixelvr;
namespace fs = std::filesystem;

int main() {
    std::cout << "[TEST] Starting SmbCredentialsManager verification..." << std::endl;
    auto& mgr = SmbCredentialsManager::instance();

    // 1. Add a protected share
    SmbShare share1;
    share1.id = "192.168.1.100/movies";
    share1.name = "Living Room NAS";
    share1.host = "192.168.1.100";
    share1.share = "movies";
    share1.workgroup = "MYGROUP";
    share1.username = "nasadmin";
    share1.password = "SuperSecretPassword123!";
    share1.anonymous = false;

    mgr.addOrUpdateShare(share1);
    std::cout << "[PASS] Added protected share." << std::endl;

    // 2. Query credentials
    std::string user, pass, wg;
    bool found = mgr.findCredentials("192.168.1.100", "movies", user, pass, wg);
    assert(found);
    assert(user == "nasadmin");
    assert(pass == "SuperSecretPassword123!");
    assert(wg == "MYGROUP");
    std::cout << "[PASS] Retrieved exact credentials." << std::endl;

    // 3. Check storage file permissions (must be 0600)
    const char* home = std::getenv("HOME");
    std::string configPath = std::string(home ? home : ".") + "/.config/pixelvr/smb_shares.json";
    struct stat st;
    assert(stat(configPath.c_str(), &st) == 0);
    mode_t perm = st.st_mode & 0777;
    std::cout << "[INFO] File permissions: 0" << std::oct << perm << std::dec << std::endl;
    assert((perm & 0077) == 0); // No permissions for group or others
    std::cout << "[PASS] Config file has strict 0600 permissions." << std::endl;

    // 4. Verify password is encrypted on disk
    std::ifstream cfgFile(configPath);
    std::string content((std::istreambuf_iterator<char>(cfgFile)), std::istreambuf_iterator<char>());
    assert(content.find("SuperSecretPassword123!") == std::string::npos);
    std::cout << "[PASS] Password is encrypted on disk (no plaintext leak)." << std::endl;

    // 5. Test auth file generation
    std::string authPath = mgr.getAuthFilePath("192.168.1.100", "movies");
    assert(!authPath.empty());
    assert(stat(authPath.c_str(), &st) == 0);
    mode_t authPerm = st.st_mode & 0777;
    assert((authPerm & 0077) == 0); // Auth file must also be 0600
    std::cout << "[PASS] Temporary Samba auth file generated with 0600 permissions." << std::endl;

    // 6. Delete share
    bool deleted = mgr.deleteShare("192.168.1.100", "movies");
    assert(deleted);
    found = mgr.findCredentials("192.168.1.100", "movies", user, pass, wg);
    assert(!found);
    std::cout << "[PASS] Deleted share successfully." << std::endl;

    std::cout << "[ALL TESTS PASSED] SmbCredentialsManager is verified and secure!" << std::endl;
    return 0;
}

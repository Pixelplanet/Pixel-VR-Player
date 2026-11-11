// Move / Copy / vault test for FileOps, VaultManager paths and the uninstall export. Development tool,
// never released. Runs in a scratch HOME (never the user's vault):
//
//   fileops_test <scratch dir under ~> [--with-sdcard]
//
// --with-sdcard also moves a test vault to the inserted SD card and back (needs a card and no
// <card>/.pixelvr_vault yet).

#include "app/file_ops.hpp"
#include "app/vault_crypto.hpp"
#include "app/vault_manager.hpp"
#include "util/sdcard.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;
namespace vc = pixelvr::vaultcrypto;
using pixelvr::FileOps;
using pixelvr::VaultManager;

static int failures = 0;
static void check(bool ok, const std::string& what) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++failures;
}

static void writeFile(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << s;
}

static std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static std::string plainOf(const std::string& pvrv) {
    vc::Reader r;
    if (!r.open(pvrv)) return "<unreadable>";
    std::string out(r.size(), '\0');
    r.read(0, reinterpret_cast<uint8_t*>(out.data()), out.size());
    return out;
}

// Runs one request and waits for its result.
static FileOps::Result run(FileOps::Request r) {
    FileOps::instance().queue(std::move(r));
    FileOps::Result res;
    for (int i = 0; i < 600; ++i) {
        if (FileOps::instance().takeResult(res)) return res;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    res.message = "timeout";
    return res;
}

static FileOps::Request req(FileOps::Mode m, std::vector<std::string> src, const std::string& dest, const std::string& label) {
    FileOps::Request r;
    r.mode = m;
    r.sources = std::move(src);
    r.destDir = dest;
    r.label = label;
    return r;
}

// Encrypted files in `dir` by real name -> path.
static std::map<std::string, std::string> vaultFiles(const std::string& dir) {
    std::map<std::string, std::string> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (vc::hasExtension(e.path().string())) out[vc::storedName(e.path().string())] = e.path().string();
    }
    return out;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: fileops_test <scratch dir under ~> [--with-sdcard]\n");
        return 2;
    }
    const bool withCard = argc > 2 && std::strcmp(argv[2], "--with-sdcard") == 0;
    const fs::path home = fs::absolute(argv[1]);
    fs::remove_all(home);
    fs::create_directories(home / ".config/pixelvr");
    setenv("HOME", home.c_str(), 1);  // before the first VaultManager::instance()

    VaultManager& vm = VaultManager::instance();
    const std::string vault = vm.vaultDir();
    check(vault == (home / ".pixelvr_vault").string(), "vault folder in the scratch HOME: " + vault);
    check(vm.isVaultPath(vault + "/a/b.mkv") && vm.isVaultPath(vault), "paths inside the vault count");
    check(!vm.isVaultPath(vault + "_old/x.mkv") && !vm.isVaultPath(vault + ".bak"), "'.pixelvr_vault_old' is not the vault ('/' boundary)");
    check(VaultManager::isPrivatePath("/home/x/Videos/abcd.pvrv"), "any .pvrv is private");
    check(!VaultManager::isPrivatePath((home / "Videos/a.mkv").string()), "a plain file outside the vault is not private");
    check(!pixelvr::removableMissing((home / "x").string()) && pixelvr::removableMissing("/run/media/nobody/NOCARD/x"),
          "removableMissing: home fine, an unmounted /run/media path missing");

    vm.unlock();  // creates the key in the scratch HOME
    for (int i = 0; i < 100 && vm.migrating(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    check(vm.isUnlocked() && vc::hasKey(), "unlocked with a fresh key");

    const fs::path videos = home / "Videos", out = home / "Out";
    fs::create_directories(videos);
    fs::create_directories(out);

    // --- plain <-> plain ---------------------------------------------------------------------
    writeFile(videos / "A.mkv", "movie A");
    auto r = run(req(FileOps::Mode::Copy, {(videos / "A.mkv").string()}, out.string(), "A.mkv"));
    check(r.ok && readFile(out / "A.mkv") == "movie A" && fs::exists(videos / "A.mkv"), "Copy plain -> plain keeps the source: " + r.message);
    r = run(req(FileOps::Mode::Copy, {(videos / "A.mkv").string()}, out.string(), "A.mkv"));
    check(r.ok && fs::exists(out / "A_1.mkv"), "Copy again: a free name (A_1.mkv)");
    r = run(req(FileOps::Mode::Move, {(videos / "A.mkv").string()}, out.string(), "A.mkv"));
    check(r.ok && !fs::exists(videos / "A.mkv") && fs::exists(out / "A_2.mkv") && r.moved.size() == 1, "Move plain -> plain");

    // --- checks the picker uses ----------------------------------------------------------------
    fs::create_directories(videos / "Season 1" / "Extras");
    check(!FileOps::checkTarget(FileOps::Mode::Move, (videos / "Season 1").string(), true, (videos / "Season 1" / "Extras").string()).empty(),
          "a folder can't go into itself");
    check(!FileOps::checkTarget(FileOps::Mode::Move, (out / "A_1.mkv").string(), false, out.string()).empty(),
          "Move into the folder it is in is refused");
    check(FileOps::checkTarget(FileOps::Mode::Copy, (out / "A_1.mkv").string(), false, out.string()).empty(),
          "Copy into the same folder is fine");
    check(!FileOps::checkTarget(FileOps::Mode::Copy, (out / "A_1.mkv").string(), false, "smb://nas/share").empty(),
          "network targets are refused");

    // --- sidecars ------------------------------------------------------------------------------
    writeFile(videos / "Clip.mp4", std::string(300000, 'v'));
    writeFile(videos / "Clip.funscript", "{\"actions\":[]}");
    writeFile(videos / "Clip.en.srt", "1\n00:00:01,000 --> 00:00:02,000\nHi\n");
    writeFile(videos / "Clipper.funscript", "{}");
    auto side = FileOps::sidecarsOf((videos / "Clip.mp4").string(), FileOps::defaultSidecarPatterns());
    check(side.size() == 2, "sidecars of Clip.mp4: Clip.funscript + Clip.en.srt, not Clipper.funscript (" + std::to_string(side.size()) + ")");

    // --- plain -> vault (with sidecars) --------------------------------------------------------
    std::vector<std::string> src{(videos / "Clip.mp4").string()};
    src.insert(src.end(), side.begin(), side.end());
    r = run(req(FileOps::Mode::Move, src, vault, "Clip.mp4"));
    auto inVault = vaultFiles(vault);
    check(r.ok && inVault.count("Clip.mp4") && inVault.count("Clip.funscript") && inVault.count("Clip.en.srt"),
          "Move to the vault: video + 2 sidecars encrypted under their real names: " + r.message);
    check(!fs::exists(videos / "Clip.mp4") && !fs::exists(videos / "Clip.funscript") && r.intoPrivate, "sources gone, result marked private");
    check(plainOf(inVault["Clip.mp4"]) == std::string(300000, 'v'), "encrypted content reads back");
    const auto vside = FileOps::sidecarsOf(inVault["Clip.mp4"], FileOps::defaultSidecarPatterns());
    check(vside.size() == 2, "sidecars of a vault file found by real name (" + std::to_string(vside.size()) + ")");

    // Same real name again: stored as Clip_1.mp4, never two "Clip.mp4".
    writeFile(videos / "Clip.mp4", "second clip");
    r = run(req(FileOps::Mode::Move, {(videos / "Clip.mp4").string()}, vault, "Clip.mp4"));
    inVault = vaultFiles(vault);
    check(r.ok && inVault.count("Clip_1.mp4") && plainOf(inVault["Clip_1.mp4"]) == "second clip", "real-name collision -> Clip_1.mp4");

    // --- folder -> vault subfolder, plain file -> vault subfolder -------------------------------
    writeFile(videos / "Season 1" / "E01.mkv", "e01");
    writeFile(videos / "Season 1" / "Extras" / "Making of.mkv", "mk");
    r = run(req(FileOps::Mode::Move, {(videos / "Season 1").string()}, vault, "Season 1"));
    bool anyPlain = false;
    for (const auto& e : fs::recursive_directory_iterator(fs::path(vault) / "Season 1")) {
        if (e.is_regular_file() && !vc::hasExtension(e.path().string())) anyPlain = true;
    }
    check(r.ok && fs::is_directory(fs::path(vault) / "Season 1" / "Extras") && !anyPlain && !fs::exists(videos / "Season 1"),
          "folder into the vault: every file encrypted right away (subfolders too): " + r.message);
    writeFile(videos / "E02.mkv", "e02");
    r = run(req(FileOps::Mode::Move, {(videos / "E02.mkv").string()}, (fs::path(vault) / "Season 1").string(), "E02.mkv"));
    check(r.ok && vaultFiles((fs::path(vault) / "Season 1").string()).count("E02.mkv"), "plain file into a vault SUBFOLDER is encrypted");

    // --- vault -> vault copy: fresh nonce ------------------------------------------------------
    fs::create_directories(fs::path(vault) / "Copies");
    r = run(req(FileOps::Mode::Copy, {inVault["Clip.mp4"]}, (fs::path(vault) / "Copies").string(), "Clip.mp4"));
    const auto copies = vaultFiles((fs::path(vault) / "Copies").string());
    bool fresh = false;
    if (copies.count("Clip.mp4")) {
        const std::string a = readFile(inVault["Clip.mp4"]).substr(6, 16), b = readFile(copies.at("Clip.mp4")).substr(6, 16);
        fresh = a != b;
    }
    check(r.ok && copies.count("Clip.mp4") && fresh && plainOf(copies.at("Clip.mp4")) == std::string(300000, 'v'),
          "vault -> vault copy re-encrypted with a fresh nonce");

    // --- vault -> plain ------------------------------------------------------------------------
    r = run(req(FileOps::Mode::Move, {inVault["Clip_1.mp4"]}, out.string(), "Clip_1.mp4"));
    check(r.ok && readFile(out / "Clip_1.mp4") == "second clip" && !fs::exists(inVault["Clip_1.mp4"]) && r.fromPrivate,
          "Move out of the vault: decrypted under the real name, encrypted file gone");
    r = run(req(FileOps::Mode::Copy, {(fs::path(vault) / "Season 1").string()}, out.string(), "Season 1"));
    check(r.ok && readFile(out / "Season 1" / "E01.mkv") == "e01" && readFile(out / "Season 1" / "Extras" / "Making of.mkv") == "mk" &&
              fs::exists(fs::path(vault) / "Season 1"),
          "Copy a vault FOLDER out: every file decrypted, the vault keeps its copy: " + r.message);

    // --- a locked vault stops vault work, sources stay -----------------------------------------
    vm.lock();
    writeFile(videos / "Locked.mkv", "locked");
    r = run(req(FileOps::Mode::Move, {(videos / "Locked.mkv").string()}, vault, "Locked.mkv"));
    check(!r.ok && fs::exists(videos / "Locked.mkv"), "vault locked: nothing moved in (" + r.message + ")");
    check(vc::storedName(inVault["Clip.mp4"]).empty(), "locked: real names unreadable");
    vm.unlock();
    for (int i = 0; i < 100 && vm.migrating(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // --- the vault on the SD card and back (optional, real card) -------------------------------
    if (withCard) {
        const std::string card = pixelvr::sdCardMount();
        const std::string cardVault = VaultManager::cardVaultDirFor(card);
        if (card.empty() || fs::exists(cardVault)) {
            check(false, "SD card test needs a card without " + cardVault);
        } else {
            const auto before = vaultFiles((fs::path(vault) / "Season 1").string());
            FileOps::Request mv;
            mv.relocateVault = true;
            mv.mode = FileOps::Mode::Copy;
            mv.sources = {vault};
            mv.destDir = cardVault;
            mv.toSdCard = true;
            mv.cardId = "test0123456789ab";
            mv.label = "the vault";
            r = run(mv);
            check(r.ok && r.relocated && vm.location() == VaultManager::Location::SdCard && vm.available(),
                  "vault moved to the SD card: " + r.message);
            check(vm.vaultDir() == cardVault && vaultFiles((fs::path(cardVault) / "Season 1").string()).size() == before.size(),
                  "same files on the card");
            check(fs::exists(fs::path(vault) / ".vault-on-sdcard") && vaultFiles(vault).empty(), "internal folder: only the marker left");
            bool sameNames = true;
            for (const auto& kv : vaultFiles((fs::path(cardVault) / "Season 1").string())) {
                if (!before.count(kv.first) || fs::path(before.at(kv.first)).filename() != fs::path(kv.second).filename()) sameNames = false;
            }
            check(sameNames, "random file names kept (thumbnails stay valid)");
            FileOps::Request back = mv;
            back.sources = {cardVault};
            back.destDir = vm.internalVaultDir();
            back.toSdCard = false;
            r = run(back);
            check(r.ok && vm.location() == VaultManager::Location::Internal && !fs::exists(cardVault) &&
                      vaultFiles((fs::path(vault) / "Season 1").string()).size() == before.size(),
                  "and back to the headset: " + r.message);
            fs::remove_all(cardVault);  // never leave a test vault on the card
        }
    }

    // --- uninstall export ----------------------------------------------------------------------
    vm.lock();
    const fs::path exportDir = home / "PixelVR Vault";
    const int rc = VaultManager::runCli("export", exportDir.string());
    bool leftEncrypted = false;
    for (const auto& e : fs::recursive_directory_iterator(vault)) {
        if (e.is_regular_file() && vc::hasExtension(e.path().string())) leftEncrypted = true;
    }
    check(rc == 0 && readFile(exportDir / "Clip.mp4") == std::string(300000, 'v') &&
              readFile(exportDir / "Season 1" / "Extras" / "Making of.mkv") == "mk" && readFile(exportDir / "Copies" / "Clip.mp4").size() == 300000 &&
              !leftEncrypted,
          "--vault-export: everything decrypted with folders kept, the vault empty (rc " + std::to_string(rc) + ")");

    fs::remove_all(home);
    std::printf("%s (%d failures)\n", failures ? "FAILED" : "ALL PASSED", failures);
    return failures;
}

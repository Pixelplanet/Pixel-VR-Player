#include "app/vault_manager.hpp"
#include <cassert>
#include <iostream>
#include <fstream>
#include <filesystem>

using namespace pixelvr;
namespace fs = std::filesystem;

int main() {
    std::cout << "[TEST] Starting VaultManager verification..." << std::endl;
    auto& vm = VaultManager::instance();

    // Reset to default combo sequence for reproducible testing
    vm.startRecordingCombo();
    vm.feedRecordButton(XrInputState::BtnA);
    vm.feedRecordButton(XrInputState::BtnB);
    vm.feedRecordButton(XrInputState::BtnB);
    vm.feedRecordButton(XrInputState::BtnA);
    vm.feedRecordButton(XrInputState::BtnX);
    vm.feedRecordButton(XrInputState::BtnY);

    // 1. Initial locked state
    vm.lock();
    assert(!vm.isUnlocked());
    std::cout << "[PASS] Initial state is locked." << std::endl;

    // 2. Discrete grip requirement (soft squeeze / grip=false should not advance)
    auto now = std::chrono::steady_clock::now();
    assert(vm.feedInput(XrInputState::BtnA, false, now) == VaultManager::ComboResult::None);
    assert(vm.feedInput(XrInputState::BtnB, false, now) == VaultManager::ComboResult::None);
    assert(!vm.isUnlocked());
    std::cout << "[PASS] Soft squeeze / no grip full press does not trigger combo." << std::endl;

    // 3. Step through default combo: A -> B -> B -> A -> X -> Y
    assert(vm.feedInput(XrInputState::BtnA, true, now) == VaultManager::ComboResult::StepMatched);
    assert(vm.feedInput(XrInputState::BtnB, true, now) == VaultManager::ComboResult::StepMatched);
    assert(vm.feedInput(XrInputState::BtnB, true, now) == VaultManager::ComboResult::StepMatched);
    assert(vm.feedInput(XrInputState::BtnA, true, now) == VaultManager::ComboResult::StepMatched);
    assert(vm.feedInput(XrInputState::BtnX, true, now) == VaultManager::ComboResult::StepMatched);
    assert(vm.feedInput(XrInputState::BtnY, true, now) == VaultManager::ComboResult::Unlocked);
    assert(vm.isUnlocked());
    std::cout << "[PASS] Cheat code A-B-B-A-X-Y with grip full press unlocks vault!" << std::endl;

    // Verify vault directories exist and have 0700 permissions
    assert(fs::exists(vm.vaultDir()));
    assert(fs::exists(vm.thumbsDir()));
    auto perms = fs::status(vm.vaultDir()).permissions();
    assert((perms & fs::perms::others_all) == fs::perms::none);
    std::cout << "[PASS] Vault directory created with secure 0700 permissions: " << vm.vaultDir() << std::endl;

    // 4. Test re-lock via combo
    assert(vm.feedInput(XrInputState::BtnA, true, now) == VaultManager::ComboResult::StepMatched);
    assert(vm.feedInput(XrInputState::BtnB, true, now) == VaultManager::ComboResult::StepMatched);
    assert(vm.feedInput(XrInputState::BtnB, true, now) == VaultManager::ComboResult::StepMatched);
    assert(vm.feedInput(XrInputState::BtnA, true, now) == VaultManager::ComboResult::StepMatched);
    assert(vm.feedInput(XrInputState::BtnX, true, now) == VaultManager::ComboResult::StepMatched);
    assert(vm.feedInput(XrInputState::BtnY, true, now) == VaultManager::ComboResult::Locked);
    assert(!vm.isUnlocked());
    std::cout << "[PASS] Entering combo again locks the vault." << std::endl;

    // 5. Test Headset removal auto-lock with 3-second debounce
    vm.unlock();
    assert(vm.isUnlocked());
    assert(!vm.tick(now, false)); // Momentary flicker (<3s) does NOT lock
    assert(vm.isUnlocked());
    assert(vm.tick(now + std::chrono::milliseconds(3100), false)); // >=3s continuous loss locks
    assert(!vm.isUnlocked());
    std::cout << "[PASS] Auto-lock on headset removal successfully locks vault after 3s debounce." << std::endl;

    // 6. Test Inactivity timeout (enabled vs disabled)
    vm.unlock();
    vm.setAutoLockTimeoutSec(2); // 2 seconds for test
    vm.recordActivity(now);
    assert(!vm.tick(now, true)); // immediate tick does not lock
    assert(vm.isUnlocked());
    assert(vm.tick(now + std::chrono::seconds(3), true)); // 3s later -> timeout
    assert(!vm.isUnlocked());
    std::cout << "[PASS] Inactivity timeout successfully locks vault when enabled." << std::endl;

    // Test Inactivity timeout disabled (default)
    vm.unlock();
    vm.setAutoLockTimeoutSec(0); // 0 = disabled
    vm.recordActivity(now);
    assert(!vm.tick(now + std::chrono::hours(1), true)); // 1 hour later -> still unlocked because disabled
    assert(vm.isUnlocked());
    std::cout << "[PASS] Inactivity timeout = 0 disables timeout auto-lock." << std::endl;

    // 7. Test Move-to-Vault for single file and collision resolution
    vm.unlock();
    std::string testSrc = "/tmp/pixelvr_test_clip.mp4";
    {
        std::ofstream f(testSrc);
        f << "dummy video content 12345";
    }
    std::string outPath1;
    std::string err1 = vm.moveFileToVault(testSrc, outPath1);
    assert(err1.empty());
    assert(!fs::exists(testSrc));
    assert(fs::exists(outPath1));
    std::cout << "[PASS] Move file to vault succeeded: " << outPath1 << std::endl;

    // Collision test: create another file with same name
    {
        std::ofstream f(testSrc);
        f << "dummy video content second version";
    }
    std::string outPath2;
    std::string err2 = vm.moveFileToVault(testSrc, outPath2);
    assert(err2.empty());
    assert(!fs::exists(testSrc));
    assert(fs::exists(outPath2));
    assert(outPath1 != outPath2);
    std::cout << "[PASS] Collision resolved properly: " << outPath2 << std::endl;

    // Clean up test files
    fs::remove(outPath1);
    fs::remove(outPath2);

    // 8. Test Move-to-Vault for entire directory and recursive permissions
    std::string testDir = "/tmp/pixelvr_test_folder";
    fs::create_directories(testDir);
    {
        std::ofstream f1(testDir + "/subvideo1.mp4");
        f1 << "sub video 1 content";
        std::ofstream f2(testDir + "/subvideo2.mp4");
        f2 << "sub video 2 content";
    }
    std::string outDir1;
    std::string errDir1 = vm.moveFileToVault(testDir, outDir1);
    assert(errDir1.empty());
    assert(!fs::exists(testDir));
    assert(fs::exists(outDir1));
    assert(fs::is_directory(outDir1));
    assert(fs::exists(outDir1 + "/subvideo1.mp4"));
    assert(fs::exists(outDir1 + "/subvideo2.mp4"));

    // Check directory permissions
    auto dirPerms = fs::status(outDir1).permissions();
    assert((dirPerms & fs::perms::others_all) == fs::perms::none);
    std::cout << "[PASS] Move entire folder to vault succeeded with 0700 permissions: " << outDir1 << std::endl;

    // Folder collision test: recreate folder and move again
    fs::create_directories(testDir);
    {
        std::ofstream f(testDir + "/subvideo3.mp4");
        f << "sub video 3 content";
    }
    std::string outDir2;
    std::string errDir2 = vm.moveFileToVault(testDir, outDir2);
    assert(errDir2.empty());
    assert(!fs::exists(testDir));
    assert(fs::exists(outDir2));
    assert(outDir1 != outDir2);
    assert(fs::exists(outDir2 + "/subvideo3.mp4"));
    std::cout << "[PASS] Folder collision resolved properly: " << outDir2 << std::endl;

    // Clean up test directories
    fs::remove_all(outDir1);
    fs::remove_all(outDir2);

    // 9. Custom combo recording
    vm.startRecordingCombo();
    assert(vm.isRecordingCombo());
    assert(!vm.feedRecordButton(XrInputState::BtnX));
    assert(!vm.feedRecordButton(XrInputState::BtnX));
    assert(!vm.feedRecordButton(XrInputState::BtnY));
    assert(!vm.feedRecordButton(XrInputState::BtnY));
    assert(!vm.feedRecordButton(XrInputState::BtnA));
    assert(vm.feedRecordButton(XrInputState::BtnB)); // 6th button finishes
    assert(!vm.isRecordingCombo());
    assert(vm.comboString() == "X X Y Y A B");
    std::cout << "[PASS] Custom combo recording verified: " << vm.comboString() << std::endl;

    // Test new combo works
    vm.lock();
    assert(vm.feedInput(XrInputState::BtnX, true, now) == VaultManager::ComboResult::StepMatched);
    assert(vm.feedInput(XrInputState::BtnX, true, now) == VaultManager::ComboResult::StepMatched);
    assert(vm.feedInput(XrInputState::BtnY, true, now) == VaultManager::ComboResult::StepMatched);
    assert(vm.feedInput(XrInputState::BtnY, true, now) == VaultManager::ComboResult::StepMatched);
    assert(vm.feedInput(XrInputState::BtnA, true, now) == VaultManager::ComboResult::StepMatched);
    assert(vm.feedInput(XrInputState::BtnB, true, now) == VaultManager::ComboResult::Unlocked);
    assert(vm.isUnlocked());
    std::cout << "[PASS] New custom combo successfully unlocked vault!" << std::endl;

    // Restore default combo
    vm.startRecordingCombo();
    vm.feedRecordButton(XrInputState::BtnA);
    vm.feedRecordButton(XrInputState::BtnB);
    vm.feedRecordButton(XrInputState::BtnB);
    vm.feedRecordButton(XrInputState::BtnA);
    vm.feedRecordButton(XrInputState::BtnX);
    vm.feedRecordButton(XrInputState::BtnY);
    vm.lock();

    std::cout << "\n*** ALL VAULT MANAGER UNIT TESTS PASSED PERFECTLY! ***" << std::endl;
    return 0;
}

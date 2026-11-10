// Vault encryption test: key file, encrypt a file, random-offset reads against the original, decrypt back,
// wrong combination refused, and read throughput with 3 readers at once. Development tool, never released.
//
//   vault_crypto_test <some video file> <scratch dir>

#include "app/vault_crypto.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <thread>
#include <vector>

namespace vc = pixelvr::vaultcrypto;
namespace fs = std::filesystem;

static int failures = 0;
static void check(bool ok, const std::string& what) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++failures;
}

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    const std::string src = argv[1], dir = argv[2];
    fs::create_directories(dir);
    const std::string keyFile = dir + "/vault.key";
    std::string err, enc;
    check(vc::createKeyFile(keyFile, "A B X Y A B", err), "key file created " + err);
    check(vc::encryptFile(src, dir, enc, err), "encrypted -> " + fs::path(enc).filename().string() + " " + err);
    check(vc::isEncrypted(enc), "header recognised");
    check(vc::storedName(enc) == fs::path(src).filename().string(), "real name stored encrypted: " + vc::storedName(enc));
    // the name must not appear in the file
    {
        std::ifstream f(enc, std::ios::binary);
        std::string head(vc::kHeaderSize, '\0');
        f.read(head.data(), head.size());
        check(head.find(fs::path(src).stem().string().substr(0, 6)) == std::string::npos, "name not readable in the file");
    }
    vc::clearKey();
    check(!vc::unlockKeyFile(keyFile, "A A A A A A", err), "wrong combination refused (" + err + ")");
    check(vc::unlockKeyFile(keyFile, "A B X Y A B", err), "right combination unlocks");
    vc::Reader r;
    check(r.open(enc), "reader opens");
    check(r.size() == fs::file_size(src), "size matches");
    std::ifstream plain(src, std::ios::binary);
    std::mt19937_64 rng(42);
    bool same = true;
    for (int i = 0; i < 300 && same; ++i) {
        const uint64_t off = rng() % r.size();
        const size_t n = 1 + rng() % 70000;
        std::vector<uint8_t> a(n), b(n);
        const long got = r.read(off, a.data(), n);
        plain.clear();
        plain.seekg(static_cast<std::streamoff>(off));
        plain.read(reinterpret_cast<char*>(b.data()), static_cast<std::streamsize>(n));
        const long want = static_cast<long>(plain.gcount());
        same = got == want && std::memcmp(a.data(), b.data(), static_cast<size_t>(want)) == 0;
    }
    check(same, "300 random reads match the original (unaligned offsets / lengths)");
    // throughput: 3 readers in parallel over the whole file
    {
        const auto t0 = std::chrono::steady_clock::now();
        std::vector<std::thread> th;
        for (int k = 0; k < 3; ++k) th.emplace_back([&] {
            vc::Reader rr;
            rr.open(enc);
            std::vector<uint8_t> buf(1 << 20);
            for (uint64_t off = 0; off < rr.size();) {
                const long n = rr.read(off, buf.data(), buf.size());
                if (n <= 0) break;
                off += static_cast<uint64_t>(n);
            }
        });
        for (auto& t : th) t.join();
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("INFO  3 readers x %.0f MB in %.2f s = %.0f MB/s total (incl. disk)\n", r.size() / 1e6, s, 3 * r.size() / 1e6 / s);
    }
    const std::string back = dir + "/back.bin";
    check(vc::decryptFile(enc, back, err) && fs::file_size(back) == fs::file_size(src), "decrypted back " + err);
    fs::remove(back);
    fs::remove(enc);
    fs::remove(keyFile);
    std::printf("%s (%d failures)\n", failures ? "FAILED" : "ALL PASSED", failures);
    return failures;
}

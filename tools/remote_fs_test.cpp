// Development tool for src/net/remote_fs.cpp (not part of the release).
//   remote_fs_test cred <sftp|dav|davs> <host> <port> <user> <password> [folder]
//   remote_fs_test shares                 list the stored shares
//   remote_fs_test ls <uri>
//   remote_fs_test get <uri> <outfile>     copy the whole file (http(s) URLs work too)
//   remote_fs_test read <uri> [seeks [local-copy]]   sequential read, then random seeks
//                                          (bytes compared with the local copy when given)
// Run with HOME pointing at a scratch directory: credentials are stored in
// $HOME/.config/pixelvr/smb_shares.json like the player does.

#include "net/remote_fs.hpp"
#include "net/smb_credentials.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

using namespace pixelvr;
using Clock = std::chrono::steady_clock;

static double secondsSince(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: see the header of this file\n");
        return 2;
    }
    const std::string cmd = argv[1];
    if (cmd == "cred" && argc >= 7) {
        SmbShare s;
        s.protocol = argv[2];
        s.host = argv[3];
        s.port = std::atoi(argv[4]);
        s.username = argv[5];
        s.password = argv[6];
        s.share = argc > 7 ? argv[7] : "";
        s.name = s.id = s.host;
        SmbCredentialsManager::instance().addOrUpdateShare(s);
        return 0;
    }
    if (cmd == "shares") {
        for (const SmbShare& s : SmbCredentialsManager::instance().shares()) {
            std::printf("%s host=%s port=%d share=%s user=%s pass=%s anon=%d\n", s.protocol.c_str(), s.host.c_str(), s.port,
                        s.share.c_str(), s.username.c_str(), s.password.empty() ? "-" : "set", s.anonymous ? 1 : 0);
        }
        return 0;
    }
    if (cmd == "ls") {
        std::vector<RemoteEntry> entries;
        std::string err;
        const auto t0 = Clock::now();
        if (!remoteList(argv[2], entries, err)) {
            std::printf("list failed: %s\n", err.c_str());
            return 1;
        }
        for (const RemoteEntry& e : entries) {
            std::printf("%c %12llu %lld %s\n", e.dir ? 'd' : '-', static_cast<unsigned long long>(e.size),
                        static_cast<long long>(e.mtime), e.name.c_str());
        }
        std::printf("%zu entries in %.3f s\n", entries.size(), secondsSince(t0));
        return 0;
    }
    if (cmd == "get" && argc >= 4) {
        std::string err;
        auto f = remoteOpen(argv[2], err);
        if (!f) {
            std::printf("open failed: %s\n", err.c_str());
            return 1;
        }
        FILE* out = std::fopen(argv[3], "wb");
        std::vector<uint8_t> buf(1 << 20);
        int64_t got = 0;
        const auto t0 = Clock::now();
        for (;;) {
            const int n = f->read(buf.data(), static_cast<int>(buf.size()));
            if (n < 0) {
                std::printf("read error at %lld\n", static_cast<long long>(got));
                return 1;
            }
            if (n == 0) break;
            std::fwrite(buf.data(), 1, static_cast<size_t>(n), out);
            got += n;
        }
        std::fclose(out);
        std::printf("got %lld of %lld bytes in %.1f s\n", static_cast<long long>(got), static_cast<long long>(f->size()), secondsSince(t0));
        return got == f->size() ? 0 : 1;
    }
    if (cmd == "read") {
        std::string err;
        const auto t0 = Clock::now();
        auto f = remoteOpen(argv[2], err);
        if (!f) {
            std::printf("open failed: %s\n", err.c_str());
            return 1;
        }
        const int64_t size = f->size();
        std::printf("opened in %.3f s, size %lld\n", secondsSince(t0), static_cast<long long>(size));
        std::vector<uint8_t> buf(256 << 10);
        const int64_t limit = std::min<int64_t>(size, 1024LL << 20);  // sequential: first GiB at most
        int64_t got = 0;
        const auto t1 = Clock::now();
        while (got < limit) {
            const int n = f->read(buf.data(), static_cast<int>(buf.size()));
            if (n < 0) {
                std::printf("read error at %lld\n", static_cast<long long>(got));
                return 1;
            }
            if (n == 0) break;
            got += n;
        }
        const double t = secondsSince(t1);
        std::printf("sequential: %.1f MB in %.2f s = %.1f MB/s\n", got / 1e6, t, got / 1e6 / std::max(t, 1e-9));
        const int seeks = argc > 3 ? std::atoi(argv[3]) : 0;
        std::mt19937_64 rng(42);
        int bad = 0;
        FILE* local = argc > 4 ? std::fopen(argv[4], "rb") : nullptr;
        std::vector<uint8_t> ref(256 << 10);
        const auto t2 = Clock::now();
        for (int i = 0; i < seeks; ++i) {
            const int64_t pos = static_cast<int64_t>(rng() % static_cast<uint64_t>(std::max<int64_t>(1, size - (2 << 20))));
            if (!f->seek(pos)) {
                ++bad;
                continue;
            }
            int64_t read = 0;
            while (read < (1 << 20)) {
                const int n = f->read(buf.data(), static_cast<int>(buf.size()));
                if (n <= 0) break;
                if (local && read == 0) {
                    fseeko(local, static_cast<off_t>(pos), SEEK_SET);
                    const size_t r = std::fread(ref.data(), 1, static_cast<size_t>(n), local);
                    if (r != static_cast<size_t>(n) || std::memcmp(ref.data(), buf.data(), r) != 0) {
                        std::printf("DATA MISMATCH at %lld\n", static_cast<long long>(pos));
                        ++bad;
                    }
                }
                read += n;
            }
            if (read < (1 << 20)) ++bad;
        }
        if (seeks > 0) {
            std::printf("%d seeks + 1 MiB reads: %.2f s (%.0f ms each), %d failed\n", seeks, secondsSince(t2),
                        secondsSince(t2) * 1000.0 / seeks, bad);
        }
        return bad ? 1 : 0;
    }
    return 2;
}

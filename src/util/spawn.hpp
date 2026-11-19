// Child processes get only the descriptors they are handed. Libraries we don't control open theirs
// without O_CLOEXEC (FFmpeg's v4l2m2m opens the hardware video decoder that way): a child that
// inherits the decoder keeps its session alive in the driver until the child exits, and every new
// 8K session is refused meanwhile. So every posix_spawn closes all descriptors from `firstFree` up in
// the child, after its own dup2s (keep a dup2 onto 3 with firstFree = 4).
#pragma once

#include <dirent.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <string>
#include <vector>

extern char** environ;

namespace pixelvr::spawnutil {

// Adds "close every descriptor >= firstFree" to `fa` (call it after the dup2 / open actions).
inline void closeFrom(posix_spawn_file_actions_t* fa, int firstFree) {
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
    if (posix_spawn_file_actions_addclosefrom_np(fa, firstFree) == 0) return;
#endif
    // (Older C library: every descriptor open now. A close of one that is shut by the time of the
    // spawn is ignored by the child.)
    DIR* d = opendir("/proc/self/fd");
    if (d == nullptr) return;
    const int self = dirfd(d);
    std::vector<int> fds;
    while (const dirent* e = readdir(d)) {
        char* end = nullptr;
        const long fd = std::strtol(e->d_name, &end, 10);
        if (end == e->d_name || *end != '\0') continue;
        if (fd >= firstFree && fd != self) fds.push_back(static_cast<int>(fd));
    }
    closedir(d);
    for (const int fd : fds) posix_spawn_file_actions_addclose(fa, fd);
}

// Runs `args` (args[0] searched in PATH) with stdout into `out` (stdin and stderr /dev/null, nothing
// else inherited) and waits for it. A replacement for popen(), whose shell child inherits every
// descriptor. False if it could not be started.
inline bool runCapture(const std::vector<std::string>& args, std::string& out, std::size_t maxBytes = 1u << 20) {
    out.clear();
    if (args.empty()) return false;
    int p[2] = {-1, -1};
    if (pipe2(p, O_CLOEXEC) != 0) return false;
    std::vector<std::string> store = args;
    std::vector<char*> argv;
    for (std::string& a : store) argv.push_back(a.data());
    argv.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, p[1], 1);  // (dup2 clears close-on-exec)
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    closeFrom(&fa, 3);
    pid_t pid = -1;
    const int rc = posix_spawnp(&pid, argv[0], &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    ::close(p[1]);
    if (rc != 0) {
        ::close(p[0]);
        return false;
    }
    char buf[4096];
    for (;;) {
        const ssize_t n = ::read(p[0], buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        if (out.size() < maxBytes) out.append(buf, static_cast<std::size_t>(n));
    }
    ::close(p[0]);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
    }
    return true;
}

}  // namespace pixelvr::spawnutil

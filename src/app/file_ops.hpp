// Moving and copying files and folders on a worker thread (the file browser's Move / Copy, "Move to
// Vault", moving the vault to / from the SD card). Every way in or out of the vault goes through here:
//   plain  -> plain  rename on the same drive, else copy (fsync, size check) then delete the source
//   plain  -> vault  encrypted (the real name is stored inside; no two files with the same real name)
//   vault  -> plain  decrypted under the real name
//   vault  -> vault  renamed on the same drive (Move), else re-encrypted with a fresh nonce
// Folders are handled file by file when they cross the vault boundary or the drive. A source is deleted
// only after its copy is complete. When the vault locks midway the job stops; sources are kept.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pixelvr {

class FileOps {
public:
    enum class Mode { Move, Copy };

    struct Request {
        Mode mode = Mode::Move;
        std::vector<std::string> sources;  // the item first, then its sidecars (same folder)
        std::string destDir;
        std::string label;                 // what the user sees (a vault file's real name)
        // Moving the whole vault (Settings > Vault location): sources[0] = the current vault folder,
        // destDir = the new vault folder (created here), toSdCard / cardId for VaultManager.
        bool relocateVault = false;
        bool toSdCard = false;
        std::string cardId;
    };

    struct Moved {
        std::string from, to;
    };

    struct Result {
        bool ok = false;
        bool privateText = false;   // the message names vault files (hide it once the vault is locked)
        bool intoPrivate = false;   // the destination is in the vault
        bool fromPrivate = false;
        bool relocated = false;
        bool relocation = false;    // a vault move was tried (ok or not)
        Mode mode = Mode::Move;
        std::string message;
        std::vector<Moved> moved;   // Move only: each source that is gone now, and where it went
        std::string destDir;
    };

    struct Status {
        bool busy = false;
        bool copy = false;
        bool privateLabel = false;
        bool relocating = false;
        std::string label;
        uint64_t done = 0, total = 0;
        int waiting = 0;  // requests queued behind the running one
    };

    static FileOps& instance();

    void queue(Request r);
    Status status() const;
    bool takeResult(Result& out);  // frame thread
    bool busy() const { return busy_.load(); }

    // Why `destDir` can't take `src` ("" = it can). Frame thread, cheap (a few stat calls).
    static std::string checkTarget(Mode mode, const std::string& src, bool srcIsDir, const std::string& destDir);
    // The target folder as a short phrase for messages ("the vault", "the SD card", "Movies").
    static std::string placeName(const std::string& dir);
    // Files next to a video that belong to it ("8 Mile.funscript", "8 Mile.en.srt"): `patterns` use
    // {base} like plugin sidecars. Encrypted neighbours are matched by their real names. Not for folders.
    static std::vector<std::string> sidecarsOf(const std::string& path, const std::vector<std::string>& patterns);
    // The patterns that always travel with a video (subtitles, scripts).
    static const std::vector<std::string>& defaultSidecarPatterns();

    ~FileOps();

private:
    FileOps() = default;
    void loop();
    void run(const Request& r, Result& res);

    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Request> jobs_;
    std::deque<Result> results_;
    Status status_;
    std::atomic<bool> busy_{false};
    std::atomic<bool> stop_{false};
    std::thread worker_;
};

}  // namespace pixelvr

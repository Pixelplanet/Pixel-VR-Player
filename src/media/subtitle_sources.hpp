// Subtitle files from places MediaEngine::open() does not look at itself: next to a video on a network
// share (smb://, sftp://, dav(s)://), next to an encrypted vault video (decrypted in memory only), and the
// external subtitle streams a Plex / Jellyfin server offers for an item. Each file is read whole (they
// are small) and handed to MediaEngine::addSubtitleData. All of it blocks on the network, so it runs on
// SubtitleFinder's worker and never delays the start of playback.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pixelvr {

// One subtitle file, read whole.
struct SubtitleSource {
    std::string fileName;  // its extension picks the reader; a vault file's real name: memory only, never logged
    std::string label;     // for the track list ("File next to the video", "File: forced.de", the server's title)
    std::string language;  // "en" / "eng", or empty
    int flags = 0;         // MediaEngine::StreamInfo flags
    int serverId = -1;     // Plex / Jellyfin: the stream's id on the server
    std::shared_ptr<const std::string> data;
};

// An external subtitle stream of a Plex / Jellyfin item.
struct ServerSubtitle {
    int id = -1;           // the server's id for the stream (Plex: stream id, Jellyfin: stream index)
    std::string url;       // http(s), with the token it needs: never logged
    std::string codec;     // srt / subrip / ass / ssa / vtt / webvtt / pgs / pgssub
    std::string language;
    std::string title;
    bool forced = false;
    bool hearingImpaired = false;
};

namespace subsources {

// True for videos whose subtitle files open() cannot find itself: on a network share, or encrypted.
bool needsLookup(const std::string& videoPath);
// "Movie.forced.de.srt" for a video named "Movie": label, language and flags as for local files (data
// stays empty). False: not a subtitle file of that video, or a format that is not read from memory.
bool describeSidecar(const std::string& videoBase, const std::string& name, SubtitleSource& out);
// ".srt" for a server's codec name; empty for formats that are not offered (VobSub, ...).
std::string extensionForCodec(const std::string& codec);
// The subtitle files next to `videoPath`, read whole (at most 8). Blocks.
std::vector<SubtitleSource> findNextTo(const std::string& videoPath);
// Downloads a server's external subtitles (at most 8, the one with id `wanted` first). Blocks.
std::vector<SubtitleSource> fetchFromServer(const std::vector<ServerSubtitle>& subs, int wanted);

}  // namespace subsources

// Runs the lookups on one worker thread; the frame thread asks and later takes the answers.
class SubtitleFinder {
public:
    struct Job {
        int slot = 0;
        uint64_t serial = 0;                 // the asker's own number for this load, handed back in the result
        std::string videoPath;               // looked next to, when needsLookup() says so
        std::vector<ServerSubtitle> server;  // Plex / Jellyfin: what the server offers
        int wantedServerId = -1;             // fetched first
    };
    struct Result {
        int slot = 0;
        uint64_t serial = 0;
        std::vector<SubtitleSource> found;
    };
    ~SubtitleFinder();
    void request(Job job);
    bool take(Result& out);

private:
    // Shared with the worker, which is detached: at exit it may still sit in a network call.
    struct State {
        std::mutex mutex;
        std::condition_variable cv;
        std::deque<Job> jobs;
        std::deque<Result> results;
        bool running = false;
        bool stop = false;
    };
    static void loop(std::shared_ptr<State> st);
    std::shared_ptr<State> st_ = std::make_shared<State>();
};

}  // namespace pixelvr

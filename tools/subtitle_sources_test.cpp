// Subtitle files of an encrypted vault video, without the player and without the real vault:
//   subtitle_sources_test <folder with one video and its subtitle files> <empty work folder>
// Encrypts copies of the folder's files into the work folder with a throwaway key (random names on
// disk, the real names inside), opens the encrypted video, lets subsources::findNextTo find and decrypt
// its subtitle files in memory, adds them to the engine and looks for the first entry of each.
// Exit code 0 when every found file showed something.
#include "app/vault_crypto.hpp"
#include "media/media_engine.hpp"
#include "media/subtitle_sources.hpp"

#include <cstdio>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    namespace fs = std::filesystem;
    using namespace pixelvr;
    // --next-to <video on a network share>: only the lookup (how many subtitle files, their sizes; no
    // names). Exit code 0 when there is at least one.
    if (argc == 3 && std::string(argv[1]) == "--next-to") {
        const auto found = subsources::findNextTo(argv[2]);
        std::printf("%d subtitle file(s):", static_cast<int>(found.size()));
        for (const SubtitleSource& s : found) {
            const size_t dot = s.fileName.find_last_of('.');
            std::printf(" %s %zu bytes [%s];", dot == std::string::npos ? "?" : s.fileName.c_str() + dot,
                        s.data ? s.data->size() : 0, s.language.c_str());
        }
        std::printf("\n");
        return found.empty() ? 1 : 0;
    }
    if (argc < 3) {
        std::fprintf(stderr, "usage: subtitle_sources_test <folder> <work folder> | --next-to <video on a share>\n");
        return 2;
    }
    const fs::path src(argv[1]), work = fs::absolute(argv[2]);  // (the lookup wants a full path, as the player has)
    std::error_code ec;
    fs::create_directories(work, ec);
    uint8_t key[32];
    std::random_device rd;
    for (uint8_t& b : key) b = static_cast<uint8_t>(rd());
    vaultcrypto::setKey(key);

    std::string video;
    std::vector<std::string> made;
    for (const auto& e : fs::directory_iterator(src, ec)) {
        if (!e.is_regular_file()) continue;
        std::string out, err;
        if (!vaultcrypto::encryptFile(e.path().string(), work.string(), out, err)) {
            std::fprintf(stderr, "encrypt failed: %s\n", err.c_str());
            return 2;
        }
        made.push_back(out);
        std::string ext = e.path().extension().string();
        if (ext == ".mkv" || ext == ".mp4" || ext == ".ts") video = out;
    }
    int result = 1;
    if (video.empty()) {
        std::fprintf(stderr, "no video (.mkv .mp4 .ts) in the folder\n");
    } else {
        MediaEngine media;
        if (!media.open(video)) {
            std::fprintf(stderr, "open failed: %s\n", media.lastError().c_str());
        } else {
            std::printf("opened the encrypted video: %dx%d %.1f s, %d track(s) of its own\n", media.width(), media.height(),
                        media.duration(), static_cast<int>(media.subtitleStreams().size()));
            const auto found = subsources::findNextTo(video);
            std::printf("found %d subtitle file(s) next to it\n", static_cast<int>(found.size()));
            int shown = 0;
            for (const SubtitleSource& s : found) {
                MediaEngine::StreamInfo info;
                info.label = s.label;
                info.language = s.language;
                info.flags = s.flags;
                info.title = s.fileName;
                const int index = media.addSubtitleData(info, s.fileName, s.data);
                media.setSubtitleStream(index);
                std::string first;
                double at = -1.0;
                const double end = 3600.0;  // (a test file may be shorter than its subtitle files)
                for (double t = 0.0; t < end && at < 0.0; t += 0.25) {
                    std::string text;
                    media.subtitleAt(t, text);
                    if (const auto pic = media.subtitlePictureAt(t)) {
                        text = "picture " + std::to_string(pic->w) + "x" + std::to_string(pic->h);
                    }
                    if (!text.empty()) {
                        first = text;
                        at = t;
                    }
                }
                for (char& c : first) {
                    if (c == '\n') c = '|';
                }
                std::printf("  %s  [%s] %s flags %d  %zu bytes -> track %d (selected %d), first entry at %.2f s: %s\n",
                            s.fileName.c_str(), s.language.c_str(), s.label.c_str(), s.flags, s.data ? s.data->size() : 0,
                            index, media.subtitleStream(), at, at < 0.0 ? "(nothing)" : first.c_str());
                if (at >= 0.0) ++shown;
            }
            media.close();
            result = (!found.empty() && shown == static_cast<int>(found.size())) ? 0 : 1;
        }
    }
    for (const std::string& f : made) fs::remove(f, ec);
    vaultcrypto::clearKey();
    std::printf("%s\n", result == 0 ? "PASS" : "FAIL");
    return result;
}

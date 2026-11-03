#pragma once
// Audio files: embedded cover art and the "now playing" card shown on the
// screen (and as the file browser thumbnail) instead of video.

#include <cstdint>
#include <string>
#include <vector>

struct AVFormatContext;

namespace pixelvr {

// Music view layout on a screen, in its model units (the screen spans x
// -1.6..1.6, y -0.9..0.9): the cover sits in the left column; the renderer
// draws the track info, buttons and queue around it.
namespace music {
constexpr float kCoverCx = -0.80f;
constexpr float kCoverCy = 0.25f;
constexpr float kCoverHalf = 0.48f;
}  // namespace music

struct AudioTags {
    std::string title;
    std::string artist;
    std::string album;
    double duration = 0.0;  // seconds, 0 if unknown
};
// Tags of an opened file (format metadata, else the audio stream's: Ogg/Opus).
void readAudioTags(AVFormatContext* fmt, AudioTags& out);
// Opens `path` just for its tags; false if it can't be read.
bool probeAudioTags(const std::string& path, AudioTags& out);

// True for a stream that is only a still image attached to the file (MP3 /
// FLAC / M4A cover art), which FFmpeg lists as a video stream.
bool isAttachedPicture(const AVFormatContext* fmt, int streamIndex);

// Index of the real video stream (cover art excluded), or -1 for audio files.
int findVideoStream(AVFormatContext* fmt);

// Decodes the file's embedded cover art to RGBA; false if there is none.
bool decodeCoverArt(AVFormatContext* fmt, std::vector<uint8_t>& rgba, int& width, int& height);

// Renders the card: the cover over a blurred, darkened copy of itself, or a
// music-note placeholder when `cover` is empty. RGBA, w x h. `musicLayout`
// puts the cover where the music view expects it (else centred: thumbnails).
void composeAudioCard(const std::vector<uint8_t>& cover, int coverW, int coverH, int w, int h,
                      std::vector<uint8_t>& out, bool musicLayout = false);

}  // namespace pixelvr

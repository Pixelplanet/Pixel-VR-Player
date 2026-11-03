#pragma once
// Audio files: embedded cover art and the "now playing" card shown on the
// screen (and as the file browser thumbnail) instead of video.

#include <cstdint>
#include <vector>

struct AVFormatContext;

namespace pixelvr {

// True for a stream that is only a still image attached to the file (MP3 /
// FLAC / M4A cover art), which FFmpeg lists as a video stream.
bool isAttachedPicture(const AVFormatContext* fmt, int streamIndex);

// Index of the real video stream (cover art excluded), or -1 for audio files.
int findVideoStream(AVFormatContext* fmt);

// Decodes the file's embedded cover art to RGBA; false if there is none.
bool decodeCoverArt(AVFormatContext* fmt, std::vector<uint8_t>& rgba, int& width, int& height);

// Renders the card: the cover centred over a blurred, darkened copy of
// itself, or a music-note placeholder when `cover` is empty. RGBA, w x h.
void composeAudioCard(const std::vector<uint8_t>& cover, int coverW, int coverH, int w, int h,
                      std::vector<uint8_t>& out);

}  // namespace pixelvr

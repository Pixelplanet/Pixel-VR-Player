// Text subtitles drawn by libass: every script (shaping, right-to-left) and .ass / .ssa styling.
// The lines of a track are handed over as ASS events; a worker thread renders what is on at the
// playback time into one RGBA picture, which the renderer draws like a picture subtitle.
#pragma once

#include "media/media_engine.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace pixelvr {

class SubtitleText {
public:
    // Null when the player was built without libass or libass could not start.
    static std::shared_ptr<SubtitleText> create();
    ~SubtitleText();
    SubtitleText(const SubtitleText&) = delete;
    SubtitleText& operator=(const SubtitleText&) = delete;

    // A font that came with the video (Matroska attachment). Before openTrack / openFile.
    void addFont(const std::string& name, const uint8_t* data, size_t size);
    // An .ass / .ssa file: libass reads it itself (styles, embedded fonts). False: not readable.
    bool openFile(const std::string& path);
    // A track fed line by line. `header` is the script header (FFmpeg's subtitle_header); empty: a plain
    // default. ownStyle: the format has no styling of its own (SubRip, WebVTT ...), so the player's look
    // is used (a sans font found through fontconfig, outline and soft shadow).
    // language: the track's language code ("ja", "chi" ...), when known. With the player's own look it
    // picks the font for scripts where the system's order of fonts is a poor guide (Chinese, Japanese
    // and Korean share their signs but not the way they are drawn).
    void openTrack(const std::string& header, bool ownStyle, const std::string& language = std::string());
    // One line: "ReadOrder,Layer,Style,Name,MarginL,MarginR,MarginV,Effect,Text" (FFmpeg's event form),
    // shown from `start` for `length` seconds. A line given twice (read again after a seek) is kept once.
    void addEvent(const std::string& line, double start, double length);
    // The video's displayed size (for scripts whose outlines are not scaled with the picture).
    void setVideoSize(int w, int h);
    // The canvas lines are laid out on: the visible picture's shape.
    void setFrame(int w, int h);
    // What is on at `seconds`, else null. The worker renders for this time; the answer is the newest
    // finished picture (one display frame behind at most). wait: until this very time is rendered.
    std::shared_ptr<const MediaEngine::SubtitlePicture> pictureAt(double seconds, bool wait);

private:
    SubtitleText();
    struct Impl;
    std::unique_ptr<Impl> d_;
};

// Text that is not valid UTF-8 is taken as Windows-1252 (old .srt files) and converted.
std::string subtitleUtf8(const std::string& in);
// A plain line ('\n' between rows) as an ASS event for SubtitleText::addEvent.
std::string subtitleEventFromPlain(const std::string& text);

}  // namespace pixelvr

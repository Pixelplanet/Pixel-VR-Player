#pragma once

#include <string>
#include <vector>

namespace pixelvr {

// The preferred-subtitles setting: which subtitle track a file starts with when nothing was chosen for it
// by hand. Plain data in, a track index out (no FFmpeg, no settings), so tools/subtitle_pick_test can
// check it without a headset.

enum class SubtitleMode : int {
    Off = 0,          // files start without subtitles
    ForeignOnly = 1,  // a "forced" track: only the lines spoken in another language than the film's
    Always = 2,       // a full track
};

// One subtitle track as the picker sees it.
struct SubtitleCandidate {
    int index = -1;        // what MediaEngine::setSubtitleStream takes
    std::string language;  // as the file states it ("ger", "deu", "de", "pt-BR", "German", ...) or empty
    bool usable = true;    // false: the player cannot show this format
    bool forced = false;   // foreign-language parts only
    bool hearingImpaired = false;  // full, with sound descriptions
    bool commentary = false;
};

// One name for a language however the file spells it: the ISO 639-2 bibliographic code ("ger" for
// de / deu / ger / German, "chi" for zh / zho / chi / zh-Hans, "por" for pt / por / pt-BR ...).
// Languages the table does not know come back lower-cased as they are; empty and "und" give "".
std::string canonicalLanguage(const std::string& code);

// English name of a language for messages ("German"); the upper-cased code when it has no name here.
std::string languageName(const std::string& code);

// The language in what stands between a video's name and a subtitle file's extension ("en", "forced.de",
// "en.hi", "pt-BR.sdh"): the canonical code of the first part that is a language, else "".
std::string languageFromFileTag(const std::string& tag);

// The track to switch on, or -1 for none. `wantedLanguage` empty means "same as the audio", which is
// `audioLanguage` (the track that plays). The first matching track in list order wins.
//   ForeignOnly: a forced track in that language (never a commentary one).
//   Always:      a full track in that language (not forced, not commentary); one without sound
//                descriptions before one with.
// A track whose language is not stated never matches, and neither does anything when the wanted
// language is not known (same as audio, and the audio track states none).
int pickSubtitleTrack(const std::vector<SubtitleCandidate>& tracks, SubtitleMode mode,
                      const std::string& wantedLanguage, const std::string& audioLanguage);

// The languages the settings list offers. Entry 0 is "Same as audio" (code "").
struct SubtitleLanguageOption {
    const char* name;
    const char* code;
};
extern const SubtitleLanguageOption kSubtitleLanguages[];
extern const int kSubtitleLanguageCount;

}  // namespace pixelvr

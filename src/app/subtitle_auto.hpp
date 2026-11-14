#pragma once

#include "app/subtitle_pick.hpp"
#include "media/media_engine.hpp"

#include <string>
#include <vector>

namespace pixelvr {

// The language of a subtitle track. A file next to the video named "film.forced.de.srt" states none
// as a track would: its label carries what stands after the video's name, the language is in there.
inline std::string subtitleTrackLanguage(const MediaEngine::StreamInfo& st) {
    if (st.language.empty() && (st.flags & MediaEngine::StreamInfo::kFromFile) && st.label.rfind("File: ", 0) == 0) {
        return languageFromFileTag(st.label.substr(6));
    }
    return st.language;
}

// The open file's subtitle tracks as the preferred-subtitles picker takes them.
inline std::vector<SubtitleCandidate> subtitleCandidates(const MediaEngine& engine) {
    using SI = MediaEngine::StreamInfo;
    std::vector<SubtitleCandidate> tracks;
    for (const SI& st : engine.subtitleStreams()) {
        SubtitleCandidate t;
        t.index = st.index;
        t.language = subtitleTrackLanguage(st);
        t.usable = st.usable;
        t.forced = (st.flags & SI::kForced) != 0;
        t.hearingImpaired = (st.flags & SI::kHearingImpaired) != 0;
        t.commentary = (st.flags & SI::kCommentary) != 0;
        tracks.push_back(t);
    }
    return tracks;
}

// The language of the audio track that plays ("same as audio"), or empty when the file states none.
inline std::string playingAudioLanguage(const MediaEngine& engine) {
    const int cur = engine.currentAudioStream();
    for (const auto& a : engine.audioStreams()) {
        if (a.index == cur) return a.language;
    }
    return {};
}

}  // namespace pixelvr

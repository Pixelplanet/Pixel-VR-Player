// Checks the preferred-subtitles picker (src/app/subtitle_pick): language matching and which track a
// file starts with. Development tool, never released. Exit code 0 = all checks passed.
//
//   subtitle_pick_test            the checks
//   subtitle_pick_test <file>     also: what each setting would pick for this file's tracks

#include "app/subtitle_auto.hpp"
#include "app/subtitle_pick.hpp"

#include <cstdio>
#include <string>
#include <vector>

using namespace pixelvr;

namespace {

int failures = 0;
int checks = 0;

void expectStr(const char* what, const std::string& got, const std::string& want) {
    ++checks;
    if (got == want) return;
    ++failures;
    std::printf("FAIL %s: got \"%s\", want \"%s\"\n", what, got.c_str(), want.c_str());
}

void expectInt(const char* what, int got, int want) {
    ++checks;
    if (got == want) return;
    ++failures;
    std::printf("FAIL %s: got %d, want %d\n", what, got, want);
}

SubtitleCandidate track(int index, const char* lang, bool forced = false, bool hearing = false,
                        bool commentary = false, bool usable = true) {
    SubtitleCandidate c;
    c.index = index;
    c.language = lang;
    c.forced = forced;
    c.hearingImpaired = hearing;
    c.commentary = commentary;
    c.usable = usable;
    return c;
}

// Opens the file the way the player does and prints, per setting, the track that would be switched on.
int showFile(const char* path) {
    MediaEngine media;
    if (!media.open(path)) {
        std::printf("open failed: %s\n", path);
        return 2;
    }
    const std::vector<SubtitleCandidate> tracks = subtitleCandidates(media);
    const std::string audio = playingAudioLanguage(media);
    std::printf("audio that plays: [%s] -> %s\n", audio.c_str(), canonicalLanguage(audio).c_str());
    for (const SubtitleCandidate& t : tracks) {
        std::printf("track %5d  [%s] -> %-3s %s%s%s%s\n", t.index, t.language.c_str(), canonicalLanguage(t.language).c_str(),
                    t.forced ? " forced" : " full", t.hearingImpaired ? " sound-descriptions" : "",
                    t.commentary ? " commentary" : "", t.usable ? "" : " UNUSABLE");
    }
    for (int i = 0; i < kSubtitleLanguageCount; ++i) {
        const int forced = pickSubtitleTrack(tracks, SubtitleMode::ForeignOnly, kSubtitleLanguages[i].code, audio);
        const int always = pickSubtitleTrack(tracks, SubtitleMode::Always, kSubtitleLanguages[i].code, audio);
        if (i > 0 && forced < 0 && always < 0) continue;  // (languages this file has nothing in)
        std::printf("%-14s foreign parts only: %d   always: %d\n", kSubtitleLanguages[i].name, forced, always);
    }
    media.close();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1) return showFile(argv[1]);
    // --- Language spellings ---------------------------------------------------------------------
    expectStr("de", canonicalLanguage("de"), "ger");
    expectStr("deu", canonicalLanguage("deu"), "ger");
    expectStr("ger", canonicalLanguage("ger"), "ger");
    expectStr("GER", canonicalLanguage("GER"), "ger");
    expectStr("German", canonicalLanguage("German"), "ger");
    expectStr("fr", canonicalLanguage("fr"), "fre");
    expectStr("fra", canonicalLanguage("fra"), "fre");
    expectStr("fre", canonicalLanguage("fre"), "fre");
    expectStr("zh", canonicalLanguage("zh"), "chi");
    expectStr("zho", canonicalLanguage("zho"), "chi");
    expectStr("chi", canonicalLanguage("chi"), "chi");
    expectStr("zh-Hans", canonicalLanguage("zh-Hans"), "chi");
    expectStr("nl", canonicalLanguage("nl"), "dut");
    expectStr("nld", canonicalLanguage("nld"), "dut");
    expectStr("dut", canonicalLanguage("dut"), "dut");
    expectStr("en", canonicalLanguage("en"), "eng");
    expectStr("en-US", canonicalLanguage("en-US"), "eng");
    expectStr("es", canonicalLanguage("es"), "spa");
    expectStr("it", canonicalLanguage("it"), "ita");
    expectStr("pt-BR", canonicalLanguage("pt-BR"), "por");
    expectStr("pl", canonicalLanguage("pl"), "pol");
    expectStr("ru", canonicalLanguage("ru"), "rus");
    expectStr("ja", canonicalLanguage("ja"), "jpn");
    expectStr("ko", canonicalLanguage("ko"), "kor");
    expectStr("cs", canonicalLanguage("cs"), "cze");
    expectStr("ces", canonicalLanguage("ces"), "cze");
    expectStr("und", canonicalLanguage("und"), "");
    expectStr("empty", canonicalLanguage(""), "");
    expectStr("unknown code stays", canonicalLanguage("TLH"), "tlh");
    // Every language the settings list offers is one the table knows under that code.
    for (int i = 1; i < kSubtitleLanguageCount; ++i) {
        expectStr(kSubtitleLanguages[i].name, canonicalLanguage(kSubtitleLanguages[i].code), kSubtitleLanguages[i].code);
        expectStr(kSubtitleLanguages[i].name, languageName(kSubtitleLanguages[i].code), kSubtitleLanguages[i].name);
    }
    expectStr("name of deu", languageName("deu"), "German");
    expectStr("name of unknown", languageName("tlh"), "TLH");

    // --- Subtitle files next to the video ------------------------------------------------------
    expectStr("tag en", languageFromFileTag("en"), "eng");
    expectStr("tag forced.de", languageFromFileTag("forced.de"), "ger");
    expectStr("tag en.hi", languageFromFileTag("en.hi"), "eng");
    expectStr("tag hi", languageFromFileTag("hi"), "hin");
    expectStr("tag pt-BR.sdh", languageFromFileTag("pt-BR.sdh"), "por");
    expectStr("tag forced", languageFromFileTag("forced"), "");
    expectStr("tag empty", languageFromFileTag(""), "");

    // --- Which track a file starts with --------------------------------------------------------
    // A typical Blu-ray rip: German forced, German full, English SDH, English full, a commentary.
    const std::vector<SubtitleCandidate> film = {
        track(3, "ger", true),
        track(4, "deu"),
        track(5, "eng", false, true),
        track(6, "en"),
        track(7, "eng", false, false, true),
    };
    expectInt("off", pickSubtitleTrack(film, SubtitleMode::Off, "ger", "ger"), -1);
    expectInt("forced, German", pickSubtitleTrack(film, SubtitleMode::ForeignOnly, "ger", "eng"), 3);
    expectInt("forced, German spelled de", pickSubtitleTrack(film, SubtitleMode::ForeignOnly, "de", "eng"), 3);
    expectInt("forced, same as audio (deu)", pickSubtitleTrack(film, SubtitleMode::ForeignOnly, "", "deu"), 3);
    expectInt("forced, same as audio (eng): none", pickSubtitleTrack(film, SubtitleMode::ForeignOnly, "", "eng"), -1);
    expectInt("forced, French: none", pickSubtitleTrack(film, SubtitleMode::ForeignOnly, "fre", "ger"), -1);
    expectInt("always, German: the full one", pickSubtitleTrack(film, SubtitleMode::Always, "ger", "eng"), 4);
    expectInt("always, English: without descriptions first", pickSubtitleTrack(film, SubtitleMode::Always, "eng", "ger"), 6);
    expectInt("always, same as audio (en)", pickSubtitleTrack(film, SubtitleMode::Always, "", "en"), 6);
    expectInt("always, French: none", pickSubtitleTrack(film, SubtitleMode::Always, "fre", "ger"), -1);
    expectInt("same as audio, audio unknown", pickSubtitleTrack(film, SubtitleMode::Always, "", ""), -1);
    expectInt("same as audio, audio und", pickSubtitleTrack(film, SubtitleMode::ForeignOnly, "", "und"), -1);

    // Only a track with sound descriptions: the fallback for Always.
    const std::vector<SubtitleCandidate> sdhOnly = {track(2, "eng", false, true), track(3, "eng", false, false, true)};
    expectInt("always, SDH fallback", pickSubtitleTrack(sdhOnly, SubtitleMode::Always, "eng", ""), 2);

    // Only a forced track: Always takes nothing; only full tracks: ForeignOnly takes nothing.
    const std::vector<SubtitleCandidate> forcedOnly = {track(2, "ger", true)};
    expectInt("always, only forced there", pickSubtitleTrack(forcedOnly, SubtitleMode::Always, "ger", "ger"), -1);
    const std::vector<SubtitleCandidate> fullOnly = {track(2, "ger")};
    expectInt("forced, only full there", pickSubtitleTrack(fullOnly, SubtitleMode::ForeignOnly, "ger", "ger"), -1);

    // Unusable tracks never; the next usable one in the language does.
    const std::vector<SubtitleCandidate> unusable = {track(2, "ger", false, false, false, false), track(3, "ger")};
    expectInt("unusable skipped", pickSubtitleTrack(unusable, SubtitleMode::Always, "ger", ""), 3);
    const std::vector<SubtitleCandidate> allUnusable = {track(2, "ger", true, false, false, false)};
    expectInt("only unusable", pickSubtitleTrack(allUnusable, SubtitleMode::ForeignOnly, "ger", ""), -1);

    // A forced commentary track is not the film's foreign parts; tracks without a language never match.
    const std::vector<SubtitleCandidate> odd = {track(2, "ger", true, false, true), track(3, "", true), track(4, "")};
    expectInt("forced commentary", pickSubtitleTrack(odd, SubtitleMode::ForeignOnly, "ger", "ger"), -1);
    expectInt("no language", pickSubtitleTrack(odd, SubtitleMode::Always, "", ""), -1);

    // A subtitle file next to the video (index 10000+) counts like a track in the file.
    const std::vector<SubtitleCandidate> withFile = {track(2, "eng"), track(10000, "de")};
    expectInt("file next to the video", pickSubtitleTrack(withFile, SubtitleMode::Always, "ger", "eng"), 10000);

    // The first match in list order wins.
    const std::vector<SubtitleCandidate> two = {track(5, "fra", true), track(6, "fre", true)};
    expectInt("first of two", pickSubtitleTrack(two, SubtitleMode::ForeignOnly, "fr", ""), 5);

    std::printf("%d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

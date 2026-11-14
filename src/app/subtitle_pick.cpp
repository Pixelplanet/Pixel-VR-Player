#include "app/subtitle_pick.hpp"

#include <cctype>
#include <cstring>

namespace pixelvr {

const SubtitleLanguageOption kSubtitleLanguages[] = {
    {"Same as audio", ""}, {"English", "eng"},  {"German", "ger"},  {"French", "fre"},   {"Spanish", "spa"},
    {"Italian", "ita"},    {"Portuguese", "por"}, {"Dutch", "dut"}, {"Polish", "pol"},   {"Russian", "rus"},
    {"Japanese", "jpn"},   {"Chinese", "chi"},  {"Korean", "kor"},
};
const int kSubtitleLanguageCount = static_cast<int>(sizeof(kSubtitleLanguages) / sizeof(kSubtitleLanguages[0]));

namespace {

// Canonical code (ISO 639-2 bibliographic), English name, and every other spelling files use for it:
// the 639-1 code, the terminologic 639-2 code, the name, and a few common non-standard tags.
struct Language {
    const char* code;
    const char* name;
    const char* aliases;  // space separated
};
const Language kLanguages[] = {
    {"eng", "English", "en english"},
    {"ger", "German", "de deu german deutsch"},
    {"fre", "French", "fr fra french francais"},
    {"spa", "Spanish", "es esp spanish espanol"},
    {"ita", "Italian", "it italian italiano"},
    {"por", "Portuguese", "pt pob portuguese"},
    {"dut", "Dutch", "nl nld dutch"},
    {"pol", "Polish", "pl polish"},
    {"rus", "Russian", "ru russian"},
    {"jpn", "Japanese", "ja jp japanese"},
    {"chi", "Chinese", "zh zho cmn yue chs cht chinese"},
    {"kor", "Korean", "ko korean"},
    // Not in the settings list, but "Same as audio" has to match them too.
    {"swe", "Swedish", "sv swedish"},
    {"dan", "Danish", "da danish"},
    {"nor", "Norwegian", "no nb nob nn nno norwegian"},
    {"fin", "Finnish", "fi finnish"},
    {"cze", "Czech", "cs ces czech"},
    {"hun", "Hungarian", "hu hungarian"},
    {"gre", "Greek", "el ell greek"},
    {"tur", "Turkish", "tr turkish"},
    {"ara", "Arabic", "ar arabic"},
    {"heb", "Hebrew", "he iw hebrew"},
    {"hin", "Hindi", "hi hindi"},
    {"tha", "Thai", "th thai"},
    {"ukr", "Ukrainian", "uk ukrainian"},
    {"rum", "Romanian", "ro ron romanian"},
    {"vie", "Vietnamese", "vi vietnamese"},
    {"ind", "Indonesian", "id indonesian"},
};

std::string lowered(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// The table entry for one lower-case word, or null.
const Language* findLanguage(const std::string& word) {
    if (word.empty()) return nullptr;
    for (const Language& l : kLanguages) {
        if (word == l.code) return &l;
        const char* p = l.aliases;
        while (*p) {
            const char* end = std::strchr(p, ' ');
            const std::size_t len = end ? static_cast<std::size_t>(end - p) : std::strlen(p);
            if (len == word.size() && word.compare(0, len, p, len) == 0) return &l;
            p += len;
            if (*p == ' ') ++p;
        }
    }
    return nullptr;
}

// "pt-BR" / "zh_Hans" -> "pt" / "zh": the part before a region or script.
std::string primaryPart(const std::string& low) {
    const std::size_t cut = low.find_first_of("-_");
    return cut == std::string::npos ? low : low.substr(0, cut);
}

}  // namespace

std::string canonicalLanguage(const std::string& code) {
    std::string low = lowered(code);
    while (!low.empty() && low.back() == ' ') low.pop_back();
    while (!low.empty() && low.front() == ' ') low.erase(low.begin());
    if (low.empty() || low == "und" || low == "unknown") return {};
    if (const Language* l = findLanguage(low)) return l->code;
    const std::string primary = primaryPart(low);
    if (primary.empty() || primary == "und") return {};
    if (const Language* l = findLanguage(primary)) return l->code;
    return primary;
}

std::string languageName(const std::string& code) {
    const std::string canon = canonicalLanguage(code);
    if (const Language* l = findLanguage(canon)) return l->name;
    std::string up = canon;
    for (char& c : up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return up;
}

std::string languageFromFileTag(const std::string& tag) {
    const std::string low = lowered(tag);
    std::size_t at = 0;
    bool first = true;
    while (at <= low.size()) {
        std::size_t end = low.find_first_of(". ", at);
        if (end == std::string::npos) end = low.size();
        const std::string part = primaryPart(low.substr(at, end - at));
        // "hi" after another part marks a hearing-impaired file ("en.hi"); alone it is Hindi.
        const bool hearingMark = part == "hi" && !first;
        if (!part.empty() && !hearingMark) {
            if (const Language* l = findLanguage(part)) return l->code;
        }
        if (!part.empty()) first = false;
        at = end + 1;
    }
    return {};
}

int pickSubtitleTrack(const std::vector<SubtitleCandidate>& tracks, SubtitleMode mode,
                      const std::string& wantedLanguage, const std::string& audioLanguage) {
    if (mode == SubtitleMode::Off) return -1;
    const std::string wantedCanon = canonicalLanguage(wantedLanguage);
    const std::string lang = wantedCanon.empty() ? canonicalLanguage(audioLanguage) : wantedCanon;
    if (lang.empty()) return -1;
    int withDescriptions = -1;  // the fallback: the first match that has sound descriptions
    for (const SubtitleCandidate& t : tracks) {
        if (!t.usable || t.index < 0 || t.commentary) continue;
        if (canonicalLanguage(t.language) != lang) continue;
        if (t.forced != (mode == SubtitleMode::ForeignOnly)) continue;
        if (!t.hearingImpaired) return t.index;
        if (withDescriptions < 0) withDescriptions = t.index;
    }
    return withDescriptions;
}

}  // namespace pixelvr

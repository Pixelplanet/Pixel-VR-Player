#include "ui/button_labels.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace pixelvr {

namespace {

// What each action does, per context. nullptr = no label there. Plain ASCII /
// Latin-1 the UI font draws: '\xC2\xB7' is the middle dot, '\xE2\x80\x93' the
// en dash (never U+2212). Nothing here may hint at the vault: the grip is
// only "Laser Hand", and there are no combo hints.
enum : unsigned {
    kNeedsMedia = 1u << 0,     // watching: only with something open on the selected screen
    kNeedsMusic = 1u << 1,     // watching: only for music (track buttons)
    kNeedsControls = 1u << 2,  // watching: only while the player controls are shown
    kNotWeb = 1u << 3,         // watching: not on a web page (it gets the D-pad itself)
    kSingleSource = 1u << 4,   // only when bound to one plain button (chords are skipped)
    kNotInDialog = 1u << 5,    // menus: not with a dialog / action menu up (they take no tabs)
    kMenusNeedOptions = 1u << 6,   // menus: only where Options opens / closes File Actions
    kMenusNeedRecenter = 1u << 7,  // menus: only where the menu recenters (hub, Downloads, Settings)
    kMenusStick = 1u << 8,         // menus: a stick / D-pad move (not in Downloads or the download confirm)
    kMenusLeftRight = 1u << 9,     // menus: left / right, which dialogs mostly ignore
    kNotLaserOnWeb = 1u << 10,     // watching: not while the laser is on a web page (the D-pad)
};
struct LabelTexts {
    InputAction action;
    const char* menus;     // hub, settings, downloads, action menu, dialogs
    const char* watching;  // no menu open
    const char* sphere;    // watching 360 / 180 (nullptr: as watching)
    const char* webPage;   // watching, laser on a web page (nullptr: as watching; kNotWeb omits it)
    const char* keyboard;  // the in-app keyboard is open
    unsigned flags;
};
const LabelTexts kLabelTexts[] = {
    {InputAction::Select, "Select", "Select / Drag", nullptr, nullptr, "Type", 0},
    {InputAction::LaserHand, "Laser Hand", "Laser Hand", nullptr, nullptr, nullptr, 0},
    {InputAction::PlayOk, "OK", "Play / Pause", nullptr, nullptr, nullptr, kNeedsMedia},
    {InputAction::Back, "Back", "Hide Controls", nullptr, "Page Back", "Cancel", kNeedsControls},
    {InputAction::MainMenu, "Close Menu", "Main Menu", nullptr, nullptr, nullptr, 0},
    {InputAction::ToggleUi, nullptr, "Show / Hide UI", nullptr, nullptr, nullptr, 0},
    {InputAction::Options, "Options", nullptr, nullptr, nullptr, nullptr, kMenusNeedOptions},  // Stage 2: playback
    {InputAction::Recenter, "Recenter", "Recenter", nullptr, nullptr, nullptr, kMenusNeedRecenter},
    {InputAction::Previous, "Previous Tab", "Previous Track", "Hold: Pan \xC2\xB7 Tilt", nullptr, nullptr,
     kNeedsMedia | kNeedsMusic | kNotInDialog},
    {InputAction::Next, "Next Tab", "Next Track", "Hold: Pan \xC2\xB7 Tilt", nullptr, nullptr,
     kNeedsMedia | kNeedsMusic | kNotInDialog},
    {InputAction::DpadUp, "Up", "Volume +", nullptr, nullptr, nullptr, kNeedsMedia | kNotWeb | kNotLaserOnWeb | kMenusStick},
    {InputAction::DpadDown, "Down", "Volume \xE2\x80\x93", nullptr, nullptr, nullptr,
     kNeedsMedia | kNotWeb | kNotLaserOnWeb | kMenusStick},
    {InputAction::DpadLeft, "Left", "\xE2\x80\x93" "10 s", nullptr, nullptr, nullptr,
     kNeedsMedia | kNotWeb | kNotLaserOnWeb | kMenusStick | kMenusLeftRight},
    {InputAction::DpadRight, "Right", "+10 s", nullptr, nullptr, nullptr,
     kNeedsMedia | kNotWeb | kNotLaserOnWeb | kMenusStick | kMenusLeftRight},
    {InputAction::StickRight, "Navigate", "Scrub \xC2\xB7 Distance", "Scrub \xC2\xB7 Zoom", "Scrub \xC2\xB7 Scroll",
     "Move Cursor", kNeedsMedia | kMenusStick},
    {InputAction::StickLeft, "Navigate", "Skip 30 s \xC2\xB7 Distance", "Skip 30 s \xC2\xB7 Zoom", "Skip 30 s \xC2\xB7 Scroll",
     "Move Cursor", kNeedsMedia | kMenusStick},
    {InputAction::ShowControls, "Hide Labels", "Hide Labels", nullptr, nullptr, nullptr, 0},
    // No default binding (SteamVR chords / long presses).
    {InputAction::Mute, nullptr, "Mute", nullptr, nullptr, nullptr, kSingleSource},
    {InputAction::Stop, nullptr, "Stop", nullptr, nullptr, nullptr, kSingleSource | kNeedsMedia},
    {InputAction::Bookmarks, "Bookmarks", "Bookmarks", nullptr, nullptr, nullptr, kSingleSource},
    {InputAction::NextScreen, nullptr, "Next Screen", nullptr, nullptr, nullptr, kSingleSource},
    {InputAction::PreviousScreen, nullptr, "Previous Screen", nullptr, nullptr, nullptr, kSingleSource},
    {InputAction::SkipBack, nullptr, "Back 30 s", nullptr, nullptr, nullptr, kSingleSource | kNeedsMedia},
    {InputAction::SkipForward, nullptr, "Forward 30 s", nullptr, nullptr, nullptr, kSingleSource | kNeedsMedia},
    {InputAction::LockScreens, nullptr, "Lock Screens", nullptr, nullptr, nullptr, kSingleSource | kNeedsMedia},
    {InputAction::AudioTrack, nullptr, "Audio Track", nullptr, nullptr, nullptr, kSingleSource | kNotWeb},
    {InputAction::Format3d, nullptr, "3D Format", nullptr, nullptr, nullptr,
     kSingleSource | kNeedsMedia | kNotWeb},
};

const LabelTexts* textsFor(InputAction a) {
    for (const LabelTexts& t : kLabelTexts) {
        if (t.action == a) return &t;
    }
    return nullptr;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string capitalized(std::string s) {
    if (!s.empty()) s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    return s;
}

} // namespace

std::string buttonLabelText(InputAction a, const ButtonLabelState& s) {
    const LabelTexts* t = textsFor(a);
    if (t == nullptr) return std::string();
    using C = ButtonLabelState::Context;
    if (s.context == C::Keyboard) return t->keyboard ? t->keyboard : "";
    if (s.context == C::Menus) {
        if ((t->flags & kNotInDialog) && s.dialog) return std::string();
        if ((t->flags & kMenusNeedOptions) && !s.optionsWork) return std::string();
        if ((t->flags & kMenusNeedRecenter) && !s.recenterWorks) return std::string();
        if ((t->flags & kMenusStick) && s.noStick) return std::string();
        if ((t->flags & kMenusLeftRight) && s.dialog && !s.dialogLeftRight) return std::string();
        return t->menus ? t->menus : "";
    }
    if ((t->flags & kNeedsMedia) && !s.mediaOpen) return std::string();
    // Flat video: a held bumper turns the sticks' up / down into the screen's size.
    const bool flatVideo = !s.sphere && s.mediaOpen && !s.music && !s.web;
    if (flatVideo && (a == InputAction::Previous || a == InputAction::Next)) {
        return s.panShift ? "Release: Scrub \xC2\xB7 Distance" : "Hold: Size";
    }
    if (flatVideo && s.panShift && (a == InputAction::StickRight || a == InputAction::StickLeft)) return "Size";
    if ((t->flags & kNeedsMusic) && !s.music && !(s.sphere && t->sphere)) return std::string();
    if ((t->flags & kNotWeb) && s.web) return std::string();
    if ((t->flags & kNotLaserOnWeb) && s.laserOnWeb) return std::string();
    // On a web page the page takes B (page back) and the stick's Y (scroll).
    if (s.laserOnWeb && t->webPage && !(s.sphere && t->sphere)) return t->webPage;
    if ((t->flags & kNeedsControls) && !s.controlsVisible) return std::string();
    // 360 / 180 with a bumper held: both sticks pan (left / right) and tilt (up / down) the view.
    if (s.sphere && s.panShift && (a == InputAction::StickRight || a == InputAction::StickLeft)) {
        return "Pan · Tilt";
    }
    if (s.sphere && s.panShift && (a == InputAction::Previous || a == InputAction::Next)) return "Release: Scrub · Zoom";
    const char* w = (s.sphere && t->sphere) ? t->sphere : t->watching;
    return w ? w : "";
}

bool parseLabelSource(const std::string& path, int& hand, std::string& component, std::string& detail) {
    static const std::string kLeft = "/user/hand/left/input/";
    static const std::string kRight = "/user/hand/right/input/";
    std::string rest;
    if (path.compare(0, kLeft.size(), kLeft) == 0) {
        hand = 0;
        rest = path.substr(kLeft.size());
    } else if (path.compare(0, kRight.size(), kRight) == 0) {
        hand = 1;
        rest = path.substr(kRight.size());
    } else {
        return false;
    }
    const std::size_t slash = rest.find('/');
    const std::string id = lower(rest.substr(0, slash));
    const std::string sub = (slash == std::string::npos) ? std::string() : lower(rest.substr(slash + 1));
    detail.clear();
    if (id.compare(0, 5, "dpad_") == 0) {
        component = "dpad";
        detail = id.substr(5);  // up / down / left / right
        return true;
    }
    if (id == "a" || id == "b" || id == "x" || id == "y" || id == "menu" || id == "view" || id == "system") {
        component = "button_" + id;
    } else if (id == "squeeze" || id == "grip") {
        component = "button_grip";
    } else if (id == "trigger" || id == "bumper") {
        component = id;
    } else if (id == "thumbstick" || id == "joystick") {
        component = "thumbstick";
    } else if (id == "trackpad") {
        component = "trackpad";
    } else {
        return false;  // poses, haptics, anything we can't point at
    }
    // Sticks / pads emulating a D-pad (".../thumbstick/dpad_up"), clicks, touches.
    if (sub.compare(0, 5, "dpad_") == 0) {
        detail = sub.substr(5);
    } else if (sub == "click" && (component == "thumbstick" || component == "trackpad")) {
        detail = "click";
    } else if (sub == "touch") {
        detail = "touch";
    }
    return true;
}

std::string labelComponentKey(const std::string& name) {
    std::string s = lower(name);
    for (const char* drop : {"frame_controller_left", "frame_controller_right", "button"}) {
        const std::string d = drop;
        for (std::size_t p = s.find(d); p != std::string::npos; p = s.find(d)) s.erase(p, d.size());
    }
    s.erase(std::remove_if(s.begin(), s.end(), [](char c) { return c == '_' || c == '-' || c == ' '; }),
            s.end());
    if (s == "squeeze") s = "grip";
    if (s == "joystick") s = "thumbstick";
    return s;
}

std::vector<ButtonLabelCard> buildButtonLabelCards(
    const std::vector<std::vector<std::string>>& boundSources, const ButtonLabelState& s) {
    struct Line {
        int order;  // InputAction, then dpad direction: the order lines appear in
        std::string prefix;
        std::string text;
        std::string direction;  // dpad direction, for the menus' "Up / Down / Left / Right"
    };
    struct Pending {
        int hand;
        std::string component;
        std::vector<Line> lines;
    };
    std::vector<Pending> cards;
    const int n = std::min(static_cast<int>(boundSources.size()), static_cast<int>(InputAction::Count));
    for (int i = 0; i < n; ++i) {
        const InputAction a = static_cast<InputAction>(i);
        const LabelTexts* t = textsFor(a);
        if (t == nullptr) continue;
        const std::vector<std::string>& src = boundSources[static_cast<std::size_t>(i)];
        if (src.empty()) continue;
        if ((t->flags & kSingleSource) && src.size() != 1) continue;  // a chord
        const std::string text = buttonLabelText(a, s);
        if (text.empty()) continue;
        for (const std::string& path : src) {
            int hand = 1;
            std::string component, detail;
            if (!parseLabelSource(path, hand, component, detail)) continue;
            Line line;
            line.text = text;
            int dirOrder = 0;
            if (detail == "up" || detail == "down" || detail == "left" || detail == "right") {
                line.direction = capitalized(detail);
                line.prefix = line.direction + ": ";
                dirOrder = detail == "up" ? 1 : detail == "down" ? 2 : detail == "left" ? 3 : 4;
            } else if (detail == "click") {
                line.prefix = "Click: ";
            } else if (detail == "touch") {
                line.prefix = "Touch: ";
            }
            line.order = i * 8 + dirOrder;
            auto it = std::find_if(cards.begin(), cards.end(), [&](const Pending& p) {
                return p.hand == hand && p.component == component;
            });
            if (it == cards.end()) {
                cards.push_back({hand, component, {}});
                it = cards.end() - 1;
            }
            const bool dup = std::any_of(it->lines.begin(), it->lines.end(), [&](const Line& l) {
                return l.prefix == line.prefix && l.text == line.text;
            });
            if (!dup) it->lines.push_back(std::move(line));
        }
    }
    std::vector<ButtonLabelCard> out;
    out.reserve(cards.size());
    for (Pending& p : cards) {
        std::stable_sort(p.lines.begin(), p.lines.end(),
                         [](const Line& x, const Line& y) { return x.order < y.order; });
        ButtonLabelCard c;
        c.hand = p.hand;
        c.component = p.component;
        // Directions that just move the highlight ("Up: Up", ...) read as one
        // line: "Up / Down / Left / Right".
        std::string plainDirs;
        for (const Line& l : p.lines) {
            if (!l.direction.empty() && l.text == l.direction) {
                plainDirs += (plainDirs.empty() ? "" : " / ") + l.direction;
            }
        }
        // A D-pad direction that does something of its own gets its own card
        // on that side of the pad (the direction is where it sits, so no
        // "Up: " prefix).
        std::vector<ButtonLabelCard> dirCards;
        bool dirsAdded = false;
        for (const Line& l : p.lines) {
            if (!l.direction.empty() && l.text == l.direction) {
                if (!dirsAdded) c.lines.push_back(plainDirs);
                dirsAdded = true;
                continue;
            }
            if (!l.direction.empty() && p.component == "dpad") {
                std::string dir = l.direction;
                for (char& ch : dir) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                auto it = std::find_if(dirCards.begin(), dirCards.end(),
                                       [&](const ButtonLabelCard& d) { return d.direction == dir; });
                if (it == dirCards.end()) {
                    ButtonLabelCard d;
                    d.hand = p.hand;
                    d.component = p.component;
                    d.direction = dir;
                    dirCards.push_back(std::move(d));
                    it = dirCards.end() - 1;
                }
                it->lines.push_back(l.text);
                continue;
            }
            c.lines.push_back(l.prefix + l.text);
        }
        if (!c.lines.empty()) out.push_back(std::move(c));
        for (ButtonLabelCard& d : dirCards) out.push_back(std::move(d));
    }
    return out;
}

// ---------------------------------------------------------------------------
// SteamVR controller json (component frames), read-only.

namespace {

// Just enough JSON for SteamVR's render-model descriptions.
struct Json {
    enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
    double number = 0.0;
    std::string string;
    std::vector<Json> array;
    std::vector<std::pair<std::string, Json>> object;

    const Json* get(const char* key) const {
        if (type != Type::Object) return nullptr;
        for (const auto& kv : object) {
            if (kv.first == key) return &kv.second;
        }
        return nullptr;
    }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& s) : s_(s) {}
    bool parse(Json& out) {
        if (!value(out, 0)) return false;
        ws();
        return p_ == s_.size();
    }

private:
    const std::string& s_;
    std::size_t p_ = 0;

    void ws() {
        while (p_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[p_]))) ++p_;
    }
    bool lit(const char* w) {
        const std::size_t n = std::char_traits<char>::length(w);
        if (s_.compare(p_, n, w) != 0) return false;
        p_ += n;
        return true;
    }
    bool str(std::string& out) {
        if (p_ >= s_.size() || s_[p_] != '"') return false;
        ++p_;
        out.clear();
        while (p_ < s_.size() && s_[p_] != '"') {
            char c = s_[p_++];
            if (c == '\\' && p_ < s_.size()) {
                const char e = s_[p_++];
                switch (e) {
                    case 'n': c = '\n'; break;
                    case 't': c = '\t'; break;
                    case 'r': c = '\r'; break;
                    case 'b': c = '\b'; break;
                    case 'f': c = '\f'; break;
                    case 'u':  // names here are ASCII: keep a placeholder
                        p_ = std::min(p_ + 4, s_.size());
                        c = '?';
                        break;
                    default: c = e; break;
                }
            }
            out.push_back(c);
        }
        if (p_ >= s_.size()) return false;
        ++p_;
        return true;
    }
    bool value(Json& out, int depth) {
        if (depth > 32) return false;
        ws();
        if (p_ >= s_.size()) return false;
        const char c = s_[p_];
        if (c == '{') {
            ++p_;
            out.type = Json::Type::Object;
            ws();
            if (p_ < s_.size() && s_[p_] == '}') {
                ++p_;
                return true;
            }
            while (true) {
                ws();
                std::string key;
                if (!str(key)) return false;
                ws();
                if (p_ >= s_.size() || s_[p_] != ':') return false;
                ++p_;
                Json v;
                if (!value(v, depth + 1)) return false;
                out.object.emplace_back(std::move(key), std::move(v));
                ws();
                if (p_ < s_.size() && s_[p_] == ',') {
                    ++p_;
                    continue;
                }
                if (p_ < s_.size() && s_[p_] == '}') {
                    ++p_;
                    return true;
                }
                return false;
            }
        }
        if (c == '[') {
            ++p_;
            out.type = Json::Type::Array;
            ws();
            if (p_ < s_.size() && s_[p_] == ']') {
                ++p_;
                return true;
            }
            while (true) {
                Json v;
                if (!value(v, depth + 1)) return false;
                out.array.push_back(std::move(v));
                ws();
                if (p_ < s_.size() && s_[p_] == ',') {
                    ++p_;
                    continue;
                }
                if (p_ < s_.size() && s_[p_] == ']') {
                    ++p_;
                    return true;
                }
                return false;
            }
        }
        if (c == '"') {
            out.type = Json::Type::String;
            return str(out.string);
        }
        if (lit("true") || lit("false")) {
            out.type = Json::Type::Bool;
            return true;
        }
        if (lit("null")) {
            out.type = Json::Type::Null;
            return true;
        }
        const char* begin = s_.c_str() + p_;
        char* end = nullptr;
        out.number = std::strtod(begin, &end);
        if (end == begin) return false;
        p_ += static_cast<std::size_t>(end - begin);
        out.type = Json::Type::Number;
        return true;
    }
};

bool readVec3(const Json* v, float out[3]) {
    if (v == nullptr || v->type != Json::Type::Array || v->array.size() != 3) return false;
    for (int k = 0; k < 3; ++k) {
        if (v->array[static_cast<std::size_t>(k)].type != Json::Type::Number) return false;
        out[k] = static_cast<float>(v->array[static_cast<std::size_t>(k)].number);
    }
    return true;
}

} // namespace

bool loadSteamVrControllerComponents(int hand, std::vector<SteamVrComponent>& out, std::string& pathOut) {
    out.clear();
    pathOut.clear();
    const std::string side = hand == 0 ? "left" : "right";
    const std::string rel = "/drivers/frame_controller/resources/rendermodels/frame_controller_" + side +
                            "/frame_controller_" + side + ".json";
    std::vector<std::string> roots = {"/opt/steamvr"};
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        roots.push_back(std::string(home) + "/.local/share/Steam/steamapps/common/SteamVR");
        roots.push_back(std::string(home) + "/.steam/steam/steamapps/common/SteamVR");
    }
    for (const std::string& root : roots) {
        std::ifstream f(root + rel, std::ios::binary);
        if (!f.is_open()) continue;
        std::stringstream ss;
        ss << f.rdbuf();
        const std::string text = ss.str();
        if (text.empty() || text.size() > (4u << 20)) continue;
        Json doc;
        JsonParser parser(text);
        if (!parser.parse(doc)) continue;
        const Json* comps = doc.get("components");
        if (comps == nullptr || comps->type != Json::Type::Object) continue;
        for (const auto& kv : comps->object) {
            const Json* local = kv.second.get("component_local");
            if (local == nullptr) continue;
            SteamVrComponent c;
            c.name = kv.first;
            if (!readVec3(local->get("origin"), c.origin)) continue;
            readVec3(local->get("rotate_xyz"), c.rotateXyz);
            if (const Json* motion = kv.second.get("motion"); motion != nullptr) {
                float centre[3];
                if (readVec3(motion->get("center"), centre)) {
                    for (int k = 0; k < 3; ++k) c.motionCentre[k] = centre[k];
                    c.haveMotionCentre = true;
                }
            }
            out.push_back(std::move(c));
        }
        if (!out.empty()) {
            pathOut = root + rel;
            return true;
        }
    }
    return false;
}

} // namespace pixelvr

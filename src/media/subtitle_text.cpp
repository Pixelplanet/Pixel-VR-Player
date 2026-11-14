#include "media/subtitle_text.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

#ifdef PIXELVR_HAVE_LIBASS
extern "C" {
#include <ass/ass.h>
}
#endif

namespace pixelvr {

std::string subtitleUtf8(const std::string& in) {
    // Valid UTF-8 (ASCII included) stays as it is.
    bool valid = true;
    for (size_t i = 0; i < in.size() && valid;) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        const int more = c < 0x80 ? 0 : (c >= 0xC2 && c < 0xE0) ? 1 : (c >= 0xE0 && c < 0xF0) ? 2 : (c >= 0xF0 && c < 0xF5) ? 3 : -1;
        if (more < 0 || i + static_cast<size_t>(more) >= in.size()) {
            valid = false;
            break;
        }
        for (int k = 1; k <= more; ++k) {
            if ((static_cast<unsigned char>(in[i + static_cast<size_t>(k)]) & 0xC0) != 0x80) valid = false;
        }
        i += static_cast<size_t>(more) + 1;
    }
    if (valid) return in;
    // Windows-1252: 0x80..0x9F are its own signs, the rest is Latin-1.
    static const uint16_t high[32] = {0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
                                      0x2039, 0x0152, 0x008D, 0x017D, 0x008F, 0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
                                      0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178};
    std::string out;
    out.reserve(in.size() + in.size() / 4);
    for (const char ch : in) {
        const unsigned char c = static_cast<unsigned char>(ch);
        const unsigned cp = c < 0x80 ? c : (c < 0xA0 ? high[c - 0x80] : c);
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

std::string subtitleEventFromPlain(const std::string& text) {
    std::string out = "0,0,Default,,0,0,0,,";
    for (const char c : text) {
        if (c == '\n') out += "\\N";
        else if (c == '{' || c == '}') (out += '\\') += c;  // (not the start of a style block)
        else if (c != '\r') out += c;
    }
    return out;
}

#ifdef PIXELVR_HAVE_LIBASS

namespace {

// What plain formats are laid out on (FFmpeg's decoders use the same numbers for their events).
const char kPlainHeader[] =
    "[Script Info]\n"
    "ScriptType: v4.00+\n"
    "PlayResX: 384\n"
    "PlayResY: 288\n"
    "ScaledBorderAndShadow: yes\n"
    "\n"
    "[V4+ Styles]\n"
    "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, "
    "Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, "
    "MarginV, Encoding\n"
    "Style: Default,sans-serif,16,&Hffffff,&Hffffff,&H0,&H0,0,0,0,0,100,100,0,0,1,1,0,2,10,10,10,1\n"
    "\n"
    "[Events]\n"
    "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n";

// libass's own messages name fonts and files: none of that goes to the log.
void quietMessages(int, const char*, va_list, void*) {}

std::atomic<uint64_t> gNextTextPicId{1};

// The font family for a line of a track without styling of its own, when the line's script needs a
// choice ("" otherwise: the style's sans font, and for signs it lacks whatever fontconfig lists first).
// Arabic script: the plain sans face (fontconfig's order puts a calligraphic Urdu face first, which
// comes out small). Chinese / Japanese / Korean: the face for the track's language.
const char* scriptFont(const std::string& line, const std::string& language) {
    bool arabic = false, cjk = false;
    for (size_t i = 0; i < line.size();) {
        const unsigned char c = static_cast<unsigned char>(line[i]);
        const int more = c < 0xC0 ? 0 : c < 0xE0 ? 1 : c < 0xF0 ? 2 : 3;
        unsigned cp = more == 0 ? c : (c & (0x3F >> more));
        for (int k = 1; k <= more && i + static_cast<size_t>(k) < line.size(); ++k) {
            cp = (cp << 6) | (static_cast<unsigned char>(line[i + static_cast<size_t>(k)]) & 0x3F);
        }
        i += static_cast<size_t>(more) + 1;
        arabic = arabic || (cp >= 0x0600 && cp <= 0x06FF) || (cp >= 0x0750 && cp <= 0x077F) || (cp >= 0xFB50 && cp <= 0xFDFF) ||
                 (cp >= 0xFE70 && cp <= 0xFEFF);
        cjk = cjk || (cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xAC00 && cp <= 0xD7AF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
              (cp >= 0x20000 && cp <= 0x3FFFF);
    }
    if (arabic) return "Noto Sans Arabic";
    if (!cjk) return "";
    std::string l = language;
    for (char& ch : l) ch = static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
    auto is = [&](const char* code) { return l.rfind(code, 0) == 0; };
    if (is("ja") || is("jpn")) return "Noto Sans CJK JP";
    if (is("ko")) return "Noto Sans CJK KR";
    if (is("zh") || is("chi") || is("zho") || is("cmn") || is("yue")) {
        const bool traditional = l.find("tw") != std::string::npos || l.find("hk") != std::string::npos ||
                                 l.find("hant") != std::string::npos || is("yue");
        return traditional ? "Noto Sans CJK TC" : "Noto Sans CJK SC";
    }
    return "";
}

}  // namespace

struct SubtitleText::Impl {
    // libass (libMutex): the worker renders, the others set the track up.
    std::mutex libMutex;
    ASS_Library* lib = nullptr;
    ASS_Renderer* renderer = nullptr;
    ASS_Track* track = nullptr;
    bool fontsSet = false;
    int frameW = 0, frameH = 0;    // what the renderer is set to
    int videoW = 0, videoH = 0;
    bool dirty = false;            // a change that was not published yet (moving lines are not published every frame)
    size_t shownPixels = 0;        // the size of the published picture
    bool shownSome = false;        // the published state has a picture
    std::chrono::steady_clock::time_point lastPublish{};

    // Lines waiting to be put into the track (pendingMutex): added by the decode thread, taken by the worker.
    struct Pending {
        std::string line;
        long long startMs = 0, lengthMs = 0;
    };
    std::mutex pendingMutex;
    std::vector<Pending> pending;
    std::unordered_set<uint64_t> seen;  // start + text of every line taken, against doubles
    bool ownStyle = false;              // pendingMutex; the track has the player's look (openTrack)
    std::string language;               // pendingMutex
    std::atomic<bool> havePending{false};

    // The request and its answer (mutex).
    std::mutex mutex;
    std::condition_variable wake, doneCv;
    std::thread worker;
    bool stop = false;
    uint64_t wantSerial = 0, doneSerial = 0;
    double wantTime = -1.0;
    bool wantAtOnce = false;       // the asker waits for this very render
    int wantW = 1920, wantH = 1080;
    std::shared_ptr<const MediaEngine::SubtitlePicture> shown;

    ~Impl() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stop = true;
        }
        wake.notify_all();
        if (worker.joinable()) worker.join();
        if (track) ass_free_track(track);
        if (renderer) ass_renderer_done(renderer);
        if (lib) ass_library_done(lib);
    }

    // The look of formats without styling of their own: the system's sans font, an outline and a soft
    // shadow so lines stay readable on a bright picture.
    void applyOwnStyle() {
        for (int i = 0; i < track->n_styles; ++i) {
            ASS_Style& st = track->styles[i];
            std::free(st.FontName);
            st.FontName = strdup("sans-serif");
            st.Outline = 1.2;
            st.Shadow = 0.5;
            st.BackColour = 0x00000060;  // the shadow: black, a little see-through
            st.MarginV = 12;
        }
    }

    void takePending() {
        if (!havePending.exchange(false)) return;
        std::vector<Pending> lines;
        {
            std::lock_guard<std::mutex> lock(pendingMutex);
            lines.swap(pending);
        }
        if (track == nullptr) return;
        for (const Pending& p : lines) {
            if (track->n_events >= 100000) break;  // (not a subtitle track any more)
            ass_process_chunk(track, p.line.data(), static_cast<int>(p.line.size()), p.startMs, p.lengthMs);
        }
    }

    // Renders the track at `seconds` on a w x h canvas. True: `out` is the new state (null: nothing on).
    bool render(double seconds, int w, int h, bool atOnce, std::shared_ptr<const MediaEngine::SubtitlePicture>& out) {
        std::lock_guard<std::mutex> lock(libMutex);
        if (track == nullptr || renderer == nullptr) {
            out.reset();
            const bool was = shownSome;
            shownSome = false;
            return was;
        }
        takePending();
        if (!fontsSet) {  // (here, not on the caller's thread: fontconfig takes a moment the first time)
            ass_set_fonts(renderer, nullptr, "sans-serif", ASS_FONTPROVIDER_AUTODETECT, nullptr, 1);
            fontsSet = true;
        }
        bool force = false;
        if (w != frameW || h != frameH) {
            frameW = w;
            frameH = h;
            ass_set_frame_size(renderer, w, h);
            ass_set_storage_size(renderer, videoW > 0 ? videoW : w, videoH > 0 ? videoH : h);
            ass_set_pixel_aspect(renderer, 1.0);  // the canvas has square pixels whatever was cut off the video
            force = true;
        }
        int change = 0;
        ASS_Image* img = ass_render_frame(renderer, track, std::llround(seconds * 1000.0), &change);
        if (change == 0 && !force && !dirty) return false;
        // Moving lines (karaoke, fades) change every frame; each published picture is a texture upload:
        // 20 a second, fewer when the picture is large (signs spread over the frame).
        const auto now = std::chrono::steady_clock::now();
        const int gapMs = shownPixels > 1500000 ? 200 : (shownPixels > 400000 ? 100 : 50);
        if (!force && !atOnce && shownSome && img != nullptr && now - lastPublish < std::chrono::milliseconds(gapMs)) {
            dirty = true;
            return false;
        }
        dirty = false;
        lastPublish = now;
        // The box around everything that is drawn.
        int x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1;
        for (const ASS_Image* i = img; i != nullptr; i = i->next) {
            if (i->w <= 0 || i->h <= 0) continue;
            x0 = std::min(x0, i->dst_x);
            y0 = std::min(y0, i->dst_y);
            x1 = std::max(x1, i->dst_x + i->w);
            y1 = std::max(y1, i->dst_y + i->h);
        }
        x0 = std::max(x0, 0);
        y0 = std::max(y0, 0);
        x1 = std::min(x1, w);
        y1 = std::min(y1, h);
        if (x1 <= x0 || y1 <= y0) {
            out.reset();
            shownSome = false;
            return true;
        }
        auto pic = std::make_shared<MediaEngine::SubtitlePicture>();
        pic->id = (1ull << 62) + gNextTextPicId.fetch_add(1);  // (apart from the ids of picture tracks)
        pic->canvasW = w;
        pic->canvasH = h;
        pic->x = x0;
        pic->y = y0;
        pic->w = x1 - x0;
        pic->h = y1 - y0;
        pic->rgba.assign(static_cast<size_t>(pic->w) * pic->h * 4, 0);
        // Each libass image is one colour with a coverage map: laid over what is there, bottom to top.
        for (const ASS_Image* i = img; i != nullptr; i = i->next) {
            const unsigned r = (i->color >> 24) & 0xFF, g = (i->color >> 16) & 0xFF, b = (i->color >> 8) & 0xFF;
            const unsigned opacity = 255 - (i->color & 0xFF);
            if (opacity == 0) continue;
            const int cx0 = std::max(i->dst_x, x0), cx1 = std::min(i->dst_x + i->w, x1);
            for (int y = std::max(i->dst_y, y0); y < std::min(i->dst_y + i->h, y1); ++y) {
                const uint8_t* src = i->bitmap + static_cast<ptrdiff_t>(y - i->dst_y) * i->stride;
                uint8_t* dst = pic->rgba.data() + (static_cast<size_t>(y - y0) * pic->w + static_cast<size_t>(cx0 - x0)) * 4;
                for (int x = cx0; x < cx1; ++x, dst += 4) {
                    const unsigned sa = (src[x - i->dst_x] * opacity + 127) / 255;
                    if (sa == 0) continue;
                    const unsigned da = dst[3];
                    if (sa == 255 || da == 0) {
                        dst[0] = static_cast<uint8_t>(r);
                        dst[1] = static_cast<uint8_t>(g);
                        dst[2] = static_cast<uint8_t>(b);
                        dst[3] = static_cast<uint8_t>(sa);
                        continue;
                    }
                    // Straight alpha "over": the colours are weighted by what each layer covers.
                    const unsigned under = da * (255 - sa);            // * 255
                    const unsigned oa = sa * 255 + under;              // * 255
                    dst[0] = static_cast<uint8_t>((r * sa * 255 + dst[0] * under) / oa);
                    dst[1] = static_cast<uint8_t>((g * sa * 255 + dst[1] * under) / oa);
                    dst[2] = static_cast<uint8_t>((b * sa * 255 + dst[2] * under) / oa);
                    dst[3] = static_cast<uint8_t>((oa + 127) / 255);
                }
            }
        }
        shownPixels = static_cast<size_t>(pic->w) * pic->h;
        out = std::move(pic);
        shownSome = true;
        return true;
    }

    void run() {
        std::unique_lock<std::mutex> lock(mutex);
        while (!stop) {
            wake.wait(lock, [&] { return stop || wantSerial != doneSerial; });
            if (stop) break;
            const uint64_t serial = wantSerial;
            const double t = wantTime;
            const int w = wantW, h = wantH;
            const bool atOnce = wantAtOnce;
            lock.unlock();
            std::shared_ptr<const MediaEngine::SubtitlePicture> pic;
            const bool changed = render(t, w, h, atOnce, pic);
            lock.lock();
            if (changed) shown = std::move(pic);
            doneSerial = serial;
            doneCv.notify_all();
        }
    }
};

SubtitleText::SubtitleText() : d_(std::make_unique<Impl>()) {}
SubtitleText::~SubtitleText() = default;

std::shared_ptr<SubtitleText> SubtitleText::create() {
    std::shared_ptr<SubtitleText> t(new SubtitleText());
    t->d_->lib = ass_library_init();
    if (t->d_->lib == nullptr) return nullptr;
    ass_set_message_cb(t->d_->lib, &quietMessages, nullptr);
    ass_set_extract_fonts(t->d_->lib, 1);  // fonts inside an .ass file are used (kept in memory)
    t->d_->renderer = ass_renderer_init(t->d_->lib);
    if (t->d_->renderer == nullptr) return nullptr;
    return t;
}

void SubtitleText::addFont(const std::string& name, const uint8_t* data, size_t size) {
    if (data == nullptr || size == 0 || size > (64u << 20)) return;
    std::lock_guard<std::mutex> lock(d_->libMutex);
    ass_add_font(d_->lib, name.c_str(), reinterpret_cast<const char*>(data), static_cast<int>(size));
}

bool SubtitleText::openFile(const std::string& path) {
    std::lock_guard<std::mutex> lock(d_->libMutex);
    if (d_->track != nullptr) return false;
    std::string p = path;
    d_->track = ass_read_file(d_->lib, p.data(), nullptr);
    return d_->track != nullptr && d_->track->n_events > 0;
}

void SubtitleText::openTrack(const std::string& header, bool ownStyle, const std::string& language) {
    {
        std::lock_guard<std::mutex> lock(d_->pendingMutex);
        d_->ownStyle = ownStyle;
        d_->language = language;
    }
    std::lock_guard<std::mutex> lock(d_->libMutex);
    if (d_->track != nullptr) return;
    d_->track = ass_new_track(d_->lib);
    if (d_->track == nullptr) return;
    std::string h = header.empty() ? std::string(kPlainHeader) : header;
    ass_process_codec_private(d_->track, h.data(), static_cast<int>(h.size()));
    ass_set_check_readorder(d_->track, 0);  // (FFmpeg numbers lines anew after a seek: doubles are sorted out in addEvent)
    if (ownStyle) d_->applyOwnStyle();
}

void SubtitleText::addEvent(const std::string& line, double start, double length) {
    if (line.empty()) return;
    Impl::Pending p;
    p.line = subtitleUtf8(line);
    p.startMs = std::llround(start * 1000.0);
    p.lengthMs = std::llround((length > 0.0 ? length : 4.0) * 1000.0);
    // The same line again (its packet read once more after a seek): the number in front differs, the rest not.
    const size_t comma = p.line.find(',');
    const uint64_t key = std::hash<std::string>()(comma == std::string::npos ? p.line : p.line.substr(comma)) * 1000003ull +
                         static_cast<uint64_t>(p.startMs);
    std::lock_guard<std::mutex> lock(d_->pendingMutex);
    if (!d_->seen.insert(key).second) return;
    if (d_->ownStyle) {
        // The font for this line's script goes in front of its text (the ninth field).
        const char* font = scriptFont(p.line, d_->language);
        size_t text = 0;
        int commas = 0;
        for (; commas < 8; ++commas) {
            const size_t at = p.line.find(',', text);
            if (at == std::string::npos) break;
            text = at + 1;
        }
        if (font[0] != '\0' && commas == 8) p.line.insert(text, std::string("{\\fn") + font + "}");
    }
    d_->pending.push_back(std::move(p));
    d_->havePending.store(true);
}

void SubtitleText::setVideoSize(int w, int h) {
    std::lock_guard<std::mutex> lock(d_->libMutex);
    d_->videoW = w;
    d_->videoH = h;
    d_->frameW = d_->frameH = 0;  // (set again at the next render)
}

void SubtitleText::setFrame(int w, int h) {
    if (w < 16 || h < 16) return;
    std::lock_guard<std::mutex> lock(d_->mutex);
    w = std::min(w, 4096);
    h = std::min(h, 2304);
    if (w != d_->wantW || h != d_->wantH) d_->wantTime = -1.0e9;  // (rendered anew even while paused)
    d_->wantW = w;
    d_->wantH = h;
}

std::shared_ptr<const MediaEngine::SubtitlePicture> SubtitleText::pictureAt(double seconds, bool wait) {
    std::unique_lock<std::mutex> lock(d_->mutex);
    if (!d_->worker.joinable()) d_->worker = std::thread([impl = d_.get()] { impl->run(); });
    // The same moment again (paused) with nothing new to draw needs no render.
    if (wait || seconds != d_->wantTime || d_->havePending.load()) {
        d_->wantTime = seconds;
        d_->wantAtOnce = wait;
        const uint64_t mine = ++d_->wantSerial;
        d_->wake.notify_one();
        if (wait) d_->doneCv.wait_for(lock, std::chrono::seconds(3), [&] { return d_->doneSerial >= mine; });
    }
    return d_->shown;
}

#else  // no libass: the renderer's own font draws plain lines

struct SubtitleText::Impl {};
SubtitleText::SubtitleText() = default;
SubtitleText::~SubtitleText() = default;
std::shared_ptr<SubtitleText> SubtitleText::create() { return nullptr; }
void SubtitleText::addFont(const std::string&, const uint8_t*, size_t) {}
bool SubtitleText::openFile(const std::string&) { return false; }
void SubtitleText::openTrack(const std::string&, bool, const std::string&) {}
void SubtitleText::addEvent(const std::string&, double, double) {}
void SubtitleText::setVideoSize(int, int) {}
void SubtitleText::setFrame(int, int) {}
std::shared_ptr<const MediaEngine::SubtitlePicture> SubtitleText::pictureAt(double, bool) { return nullptr; }

#endif

}  // namespace pixelvr

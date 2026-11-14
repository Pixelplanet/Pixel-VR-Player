#include "media/subtitle_sources.hpp"

#include "app/vault_crypto.hpp"
#include "media/media_engine.hpp"
#include "net/remote_fs.hpp"
#include "plugins/plugin_files.hpp"
#include "util/logging.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

namespace pixelvr {

namespace {

constexpr size_t kMaxFiles = 8;
constexpr size_t kMaxTextBytes = 8u << 20;      // .srt .ass .ssa .vtt
constexpr size_t kMaxPictureBytes = 64u << 20;  // .sup (a Blu-ray track of a long film)

std::string lower(std::string s) {
    for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

std::string extensionOf(const std::string& name) {
    const size_t dot = name.find_last_of('.');
    return dot == std::string::npos ? std::string() : lower(name.substr(dot));
}

size_t maxBytesFor(const std::string& ext) { return ext == ".sup" ? kMaxPictureBytes : kMaxTextBytes; }

// A whole file behind a web address, at most `maxBytes`.
bool readUrl(const std::string& url, size_t maxBytes, std::string& out, std::string& err) {
    out.clear();
    auto f = remoteOpen(url, err);
    if (!f) return false;
    if (f->size() > static_cast<int64_t>(maxBytes)) {
        err = "file too large";
        return false;
    }
    std::vector<uint8_t> buf(65536);
    for (;;) {
        const int n = f->read(buf.data(), static_cast<int>(buf.size()));
        if (n < 0) {
            err = "read error";
            return false;
        }
        if (n == 0) break;
        out.append(reinterpret_cast<const char*>(buf.data()), static_cast<size_t>(n));
        if (out.size() > maxBytes) {
            err = "file too large";
            return false;
        }
    }
    return !out.empty();
}

}  // namespace

namespace subsources {

bool needsLookup(const std::string& videoPath) {
    if (isRemotePath(videoPath)) return true;
    return !videoPath.empty() && videoPath[0] == '/' &&
           (vaultcrypto::hasExtension(videoPath) || vaultcrypto::isEncrypted(videoPath));
}

bool describeSidecar(const std::string& videoBase, const std::string& name, SubtitleSource& out) {
    const std::string ext = extensionOf(name);
    // (.idx + .sub is not offered here: FFmpeg opens the .sub by file name, which needs a local file.)
    if (ext != ".srt" && ext != ".ass" && ext != ".ssa" && ext != ".vtt" && ext != ".sup") return false;
    if (videoBase.empty() || name.size() <= videoBase.size() + 3 || name[videoBase.size()] != '.') return false;
    if (lower(name.substr(0, videoBase.size())) != lower(videoBase)) return false;
    // What stands between the video's name and the extension ("en", "forced.de"), as for local files.
    std::string tag = name.substr(videoBase.size(), name.size() - videoBase.size() - ext.size());
    if (!tag.empty() && tag[0] == '.') tag = tag.substr(1);
    const std::string lowTag = lower(tag);
    out = SubtitleSource{};
    out.fileName = name;
    out.language = (tag.size() == 2 || tag.size() == 3) ? tag : std::string();
    out.flags = MediaEngine::StreamInfo::kFromFile;
    if (lowTag.find("forced") != std::string::npos) out.flags |= MediaEngine::StreamInfo::kForced;
    // "hi" alone is Hindi; it means hearing-impaired only after a language ("en.hi", "en.hi.forced").
    const bool hiPart = lowTag.find(".hi.") != std::string::npos ||
                        (lowTag.size() > 3 && lowTag.compare(lowTag.size() - 3, 3, ".hi") == 0);
    if (lowTag.find("sdh") != std::string::npos || hiPart) out.flags |= MediaEngine::StreamInfo::kHearingImpaired;
    out.label = tag.empty() || !out.language.empty() ? "File next to the video" : "File: " + tag;
    return true;
}

std::string extensionForCodec(const std::string& codec) {
    const std::string c = lower(codec);
    if (c == "srt" || c == "subrip") return ".srt";
    if (c == "ass" || c == "ssa") return ".ass";
    if (c == "vtt" || c == "webvtt") return ".vtt";
    if (c == "pgs" || c == "pgssub" || c == "sup" || c == "hdmv_pgs_subtitle") return ".sup";
    return {};
}

std::vector<SubtitleSource> findNextTo(const std::string& videoPath) {
    std::vector<SubtitleSource> found;
    if (!needsLookup(videoPath)) return found;
    // A vault video's files carry random names: its real name, and its neighbours' real names, are
    // stored inside them. Nothing about those is logged.
    const bool vault = !isRemotePath(videoPath);
    std::string realName = vault ? vaultcrypto::storedName(videoPath) : videoPath;
    const std::string base = pluginfiles::baseNameOf(realName);
    const std::string dir = pluginfiles::parentOf(videoPath);
    if (base.empty() || dir.empty()) return found;
    std::vector<std::string> names;
    std::string err;
    if (!pluginfiles::listFiles(dir, names, err, vault)) {
        if (!vault) PIXELVR_LOG_INFO("Subtitles: the video's folder on the share could not be listed (%s)", err.c_str());
        return found;
    }
    std::sort(names.begin(), names.end());
    size_t skippedPairs = 0;
    for (const std::string& name : names) {
        if (found.size() >= kMaxFiles) break;
        SubtitleSource s;
        if (!describeSidecar(base, name, s)) {
            if (extensionOf(name) == ".idx" && name.size() > base.size() &&
                lower(name.substr(0, base.size())) == lower(base)) {
                ++skippedPairs;
            }
            continue;
        }
        std::string data;
        if (!pluginfiles::readFile(pluginfiles::join(dir, name), maxBytesFor(extensionOf(name)), data, err, vault) ||
            data.empty()) {
            if (!vault) PIXELVR_LOG_INFO("Subtitles: a subtitle file on the share could not be read (%s)", err.c_str());
            continue;
        }
        s.data = std::make_shared<const std::string>(std::move(data));
        found.push_back(std::move(s));
    }
    if (!vault) {
        PIXELVR_LOG_INFO("Subtitles: %d file(s) next to the video on the share", static_cast<int>(found.size()));
        if (skippedPairs > 0) {
            PIXELVR_LOG_INFO("Subtitles: %d .idx / .sub pair(s) on the share left out (they are only read from local files)",
                             static_cast<int>(skippedPairs));
        }
    }
    return found;
}

std::vector<SubtitleSource> fetchFromServer(const std::vector<ServerSubtitle>& subs, int wanted) {
    std::vector<SubtitleSource> found;
    // The one chosen on the page first, so it is certainly among those that are fetched.
    std::vector<const ServerSubtitle*> order;
    for (const ServerSubtitle& s : subs) {
        if (s.id == wanted) order.push_back(&s);
    }
    for (const ServerSubtitle& s : subs) {
        if (s.id != wanted) order.push_back(&s);
    }
    int failed = 0, unknown = 0;
    for (const ServerSubtitle* sp : order) {
        if (found.size() >= kMaxFiles) break;
        const std::string ext = extensionForCodec(sp->codec);
        if (ext.empty() || !isHttpUrl(sp->url)) {
            ++unknown;
            continue;
        }
        std::string data, err;
        if (!readUrl(sp->url, maxBytesFor(ext), data, err)) {  // (the address carries a token: not logged)
            ++failed;
            continue;
        }
        SubtitleSource s;
        s.fileName = "server" + ext;
        s.label = sp->title.empty() ? std::string("From the server") : sp->title;
        s.language = sp->language;
        s.flags = MediaEngine::StreamInfo::kFromServer;
        const std::string lowTitle = lower(sp->title);
        if (sp->forced || lowTitle.find("forced") != std::string::npos) s.flags |= MediaEngine::StreamInfo::kForced;
        if (sp->hearingImpaired || lowTitle.find("sdh") != std::string::npos) {
            s.flags |= MediaEngine::StreamInfo::kHearingImpaired;
        }
        s.serverId = sp->id;
        s.data = std::make_shared<const std::string>(std::move(data));
        found.push_back(std::move(s));
    }
    if (!subs.empty()) {
        PIXELVR_LOG_INFO("Subtitles: %d of the server's %d external track(s) loaded (%d failed, %d in a format that is not read)",
                         static_cast<int>(found.size()), static_cast<int>(subs.size()), failed, unknown);
    }
    return found;
}

}  // namespace subsources

SubtitleFinder::~SubtitleFinder() {
    {
        std::lock_guard<std::mutex> lk(st_->mutex);
        st_->stop = true;
        st_->jobs.clear();
    }
    st_->cv.notify_all();  // not joined: the worker may be blocked on the network; it owns its state
}

void SubtitleFinder::request(Job job) {
    {
        std::lock_guard<std::mutex> lk(st_->mutex);
        // A newer load of the same screen replaces a lookup that has not started yet.
        st_->jobs.erase(std::remove_if(st_->jobs.begin(), st_->jobs.end(), [&](const Job& j) { return j.slot == job.slot; }),
                        st_->jobs.end());
        st_->jobs.push_back(std::move(job));
        if (!st_->running) {
            st_->running = true;
            std::thread(&SubtitleFinder::loop, st_).detach();
        }
    }
    st_->cv.notify_one();
}

bool SubtitleFinder::take(Result& out) {
    std::lock_guard<std::mutex> lk(st_->mutex);
    if (st_->results.empty()) return false;
    out = std::move(st_->results.front());
    st_->results.pop_front();
    return true;
}

void SubtitleFinder::loop(std::shared_ptr<State> st) {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lk(st->mutex);
            st->cv.wait(lk, [&] { return st->stop || !st->jobs.empty(); });
            if (st->stop) return;
            job = std::move(st->jobs.front());
            st->jobs.pop_front();
        }
        Result r;
        r.slot = job.slot;
        r.serial = job.serial;
        r.found = subsources::findNextTo(job.videoPath);
        std::vector<SubtitleSource> server = subsources::fetchFromServer(job.server, job.wantedServerId);
        for (SubtitleSource& s : server) r.found.push_back(std::move(s));
        std::lock_guard<std::mutex> lk(st->mutex);
        if (st->stop) return;
        st->results.push_back(std::move(r));
    }
}

}  // namespace pixelvr

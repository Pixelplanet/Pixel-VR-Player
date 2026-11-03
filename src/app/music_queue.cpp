#include "app/music_queue.hpp"

#include "media/cover_art.hpp"

#include <algorithm>
#include <numeric>
#include <random>

namespace pixelvr {

namespace {
std::string fileTitle(const std::string& path) {
    std::string name = path.substr(path.find_last_of('/') + 1);
    const size_t dot = name.find_last_of('.');
    return dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
}
}  // namespace

MusicQueue::~MusicQueue() { stopProbe(); }

void MusicQueue::stopProbe() {
    stopProbe_.store(true);
    if (probe_.joinable()) probe_.join();
    stopProbe_.store(false);
}

void MusicQueue::set(const std::vector<std::string>& paths, int current) {
    stopProbe();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tracks_.clear();
        for (const std::string& p : paths) tracks_.push_back({p, fileTitle(p), {}, 0.0});
        current_ = tracks_.empty() ? -1 : std::clamp(current, 0, static_cast<int>(tracks_.size()) - 1);
        rebuildOrder();
    }
    // Tags in the background, starting with the tracks around the current one.
    probe_ = std::thread([this, paths, current] {
        std::vector<int> todo(paths.size());
        std::iota(todo.begin(), todo.end(), 0);
        std::stable_sort(todo.begin(), todo.end(), [current](int a, int b) {
            const auto key = [current](int i) { return i >= current ? i - current : 100000 + i; };
            return key(a) < key(b);
        });
        for (int i : todo) {
            if (stopProbe_.load()) return;
            if (paths[static_cast<size_t>(i)].empty() || paths[static_cast<size_t>(i)][0] != '/') continue;
            AudioTags tags;
            if (!probeAudioTags(paths[static_cast<size_t>(i)], tags)) continue;
            std::lock_guard<std::mutex> lock(mutex_);
            if (i >= static_cast<int>(tracks_.size()) || tracks_[static_cast<size_t>(i)].path != paths[static_cast<size_t>(i)]) return;
            Track& t = tracks_[static_cast<size_t>(i)];
            if (!tags.title.empty()) t.title = tags.title;
            t.artist = tags.artist;
            t.duration = tags.duration;
        }
    });
}

void MusicQueue::clear() {
    stopProbe();
    std::lock_guard<std::mutex> lock(mutex_);
    tracks_.clear();
    order_.clear();
    current_ = -1;
}

bool MusicQueue::empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tracks_.empty();
}

int MusicQueue::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<int>(tracks_.size());
}

int MusicQueue::current() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_;
}

void MusicQueue::setCurrent(int index) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (index >= 0 && index < static_cast<int>(tracks_.size())) current_ = index;
}

int MusicQueue::indexOf(const std::string& path) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = 0; i < tracks_.size(); ++i) {
        if (tracks_[i].path == path) return static_cast<int>(i);
    }
    return -1;
}

MusicQueue::Track MusicQueue::track(int index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return (index >= 0 && index < static_cast<int>(tracks_.size())) ? tracks_[static_cast<size_t>(index)] : Track{};
}

double MusicQueue::totalDuration() const {
    std::lock_guard<std::mutex> lock(mutex_);
    double sum = 0.0;
    for (const Track& t : tracks_) sum += t.duration;
    return sum;
}

int MusicQueue::next(bool finished) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (tracks_.empty() || current_ < 0) return -1;
    if (finished && repeat_ == Repeat::One) return current_;
    const auto it = std::find(order_.begin(), order_.end(), current_);
    const size_t pos = static_cast<size_t>(it - order_.begin());
    if (pos + 1 < order_.size()) return order_[pos + 1];
    return repeat_ == Repeat::Off ? -1 : order_.front();
}

int MusicQueue::previous() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (tracks_.empty() || current_ < 0) return -1;
    const auto it = std::find(order_.begin(), order_.end(), current_);
    const size_t pos = static_cast<size_t>(it - order_.begin());
    if (pos > 0 && pos < order_.size()) return order_[pos - 1];
    return repeat_ == Repeat::Off ? current_ : order_.back();
}

bool MusicQueue::shuffle() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return shuffle_;
}

void MusicQueue::setShuffle(bool on) {
    std::lock_guard<std::mutex> lock(mutex_);
    shuffle_ = on;
    rebuildOrder();
}

MusicQueue::Repeat MusicQueue::repeat() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return repeat_;
}

void MusicQueue::cycleRepeat() {
    std::lock_guard<std::mutex> lock(mutex_);
    repeat_ = repeat_ == Repeat::Off ? Repeat::All : (repeat_ == Repeat::All ? Repeat::One : Repeat::Off);
}

void MusicQueue::rebuildOrder() {
    order_.resize(tracks_.size());
    std::iota(order_.begin(), order_.end(), 0);
    if (!shuffle_ || order_.empty()) return;
    // Shuffled: the current track first, the rest in random order after it.
    static std::mt19937 rng{std::random_device{}()};
    std::shuffle(order_.begin(), order_.end(), rng);
    const auto it = std::find(order_.begin(), order_.end(), current_);
    if (it != order_.end()) std::iter_swap(order_.begin(), it);
}

}  // namespace pixelvr

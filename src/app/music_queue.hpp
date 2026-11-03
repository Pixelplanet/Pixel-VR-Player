#pragma once
// Music playlist of one screen: the audio files of the folder a track was
// opened from, in folder order, with shuffle / repeat and the next / previous
// logic. Tags (title, artist, duration) are read in the background.

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pixelvr {

class MusicQueue {
public:
    struct Track {
        std::string path;
        std::string title;   // tag, else the file name
        std::string artist;  // tag, may be empty
        double duration = 0.0;
    };
    enum class Repeat { Off, All, One };

    MusicQueue() = default;
    ~MusicQueue();
    MusicQueue(const MusicQueue&) = delete;
    MusicQueue& operator=(const MusicQueue&) = delete;

    // Replaces the queue; `current` is the track playing now.
    void set(const std::vector<std::string>& paths, int current);
    void clear();
    bool empty() const;
    int size() const;
    int current() const;
    void setCurrent(int index);
    int indexOf(const std::string& path) const;
    Track track(int index) const;  // copy; safe while tags are being read
    double totalDuration() const;  // of the tracks read so far

    // Index to play after / before the current one in play order, or -1.
    // `finished` = the current track ended by itself (Repeat::One replays it).
    int next(bool finished) const;
    int previous() const;

    bool shuffle() const;
    void setShuffle(bool on);
    Repeat repeat() const;
    void cycleRepeat();  // Off -> All -> One -> Off

private:
    void rebuildOrder();  // mutex held
    void stopProbe();

    mutable std::mutex mutex_;
    std::vector<Track> tracks_;
    std::vector<int> order_;  // play order (indices into tracks_)
    int current_ = -1;
    bool shuffle_ = false;
    Repeat repeat_ = Repeat::Off;
    std::thread probe_;
    std::atomic<bool> stopProbe_{false};
};

}  // namespace pixelvr

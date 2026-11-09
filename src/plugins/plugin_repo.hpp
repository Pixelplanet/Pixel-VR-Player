// The built-in plugin source: the plugin list (index.json) in the Pixel VR Player plugin repository on
// GitHub. Packages it offers are downloaded in the background and checked against the list's SHA-256
// before the host installs them. (Packages the user puts into Downloads are offered separately.)
#pragma once

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pixelvr {

class PluginRepo {
public:
    static constexpr const char* kIndexUrl =
        "https://raw.githubusercontent.com/Pixelplanet/Pixel-VR-Player-Plugins/main/index.json";

    struct Entry {
        std::string id, name, version, description, url, sha256, minApp;
        long long size = 0;
    };

    ~PluginRepo();
    // Loads the list in the background (at most every few minutes unless forced).
    void refresh(bool force = false);
    std::vector<Entry> entries() const;
    std::string state() const;  // "" when fine, else e.g. "Loading the plugin list..." / an error
    // Downloads one package; when done (and the checksum matches) takeReady() hands over the file.
    void download(const Entry& e);
    bool downloading(const std::string& id) const;
    bool takeReady(std::string& file, std::string& error);

private:
    void join();
    mutable std::mutex mu_;
    std::vector<Entry> entries_;
    std::string state_, busyId_, readyFile_, readyError_;
    bool ready_ = false;
    std::chrono::steady_clock::time_point fetchedAt_{};
    std::thread worker_;
    std::atomic<bool> busy_{false};
};

}  // namespace pixelvr

#pragma once

#include <functional>
#include <string>
#include <cstdint>

namespace pixelvr {

class VrKeyboard {
public:
    static VrKeyboard& instance();

    // Initializes OpenVR overlay interface dynamically if SteamVR is running.
    bool init();
    void shutdown();

    bool isAvailable() const { return available_; }
    bool isOpen() const { return isOpen_; }

    // Opens the native SteamVR / SteamOS VR on-screen virtual keyboard.
    // e.g. description: "Enter SMB Server IP", existingText: "192.168.1.100", isPassword: false
    bool show(const std::string& description,
              const std::string& existingText,
              bool isPassword = false,
              int maxChars = 256);

    // Closes the virtual keyboard if open.
    void hide();

    // Call once per frame in the main event loop to poll OpenVR keyboard events.
    void poll(std::function<void(const std::string&)> onDone,
              std::function<void()> onCancel);

    // Retrieves current entered buffer
    std::string currentText() const;
    const std::string& description() const { return description_; }
    bool isPassword() const { return isPassword_; }

private:
    VrKeyboard() = default;
    ~VrKeyboard();

    bool available_ = false;
    bool isOpen_ = false;
    bool doneSubmitted_ = false;
    bool kbConfigured_ = false;
    std::string description_;
    std::string existingText_;
    std::string currentText_;
    bool isPassword_ = false;
    void* libHandle_ = nullptr;
    void* vrSystem_ = nullptr;
    void* vrOverlay_ = nullptr;
    uint64_t inputOverlay_ = 0;
};

} // namespace pixelvr

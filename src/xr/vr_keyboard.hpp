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
              int maxChars = 256,
              const float* headPosLocal = nullptr,
              const float* headForwardLocal = nullptr,
              const float* headPosStage = nullptr,
              const float* headForwardStage = nullptr);

    // Closes the virtual keyboard if open.
    void hide();

    // Queries OpenVR HMD activity level (k_EDeviceActivityLevel_UserInteraction = worn).
    // Returns true if worn, or true if OpenVR is not available / indeterminate.
    bool isUserPresent() const;

    // Call once per frame in the main event loop to poll OpenVR keyboard events.
    void poll(std::function<void(const std::string&)> onDone,
              std::function<void()> onCancel);

    // Retrieves current entered buffer
    std::string currentText() const;
    int cursorPos() const { return static_cast<int>(cursorPos_); }
    void moveCursor(int delta);
    const std::string& description() const { return description_; }
    bool isPassword() const { return isPassword_; }
    const float* transform() const { return &kbMatLocal_[0][0]; }

private:
    VrKeyboard() = default;
    ~VrKeyboard();

    bool configureKeyboardOverlay();
    void setLaserPointerActive(bool active);

    bool available_ = false;
    bool isOpen_ = false;
    bool doneSubmitted_ = false;
    bool userTyped_ = false;
    bool kbConfigured_ = false;
    int pollFramesSinceOpen_ = 0;
    float kbMatLocal_[3][4]{};
    float kbMatStage_[3][4]{};
    uint64_t currentNonce_ = 0;
    int maxChars_ = 256;
    std::string description_;
    std::string existingText_;
    std::string currentText_;
    std::size_t cursorPos_ = 0;
    int escState_ = 0;
    std::string escParams_;
    bool isPassword_ = false;
    void* libHandle_ = nullptr;
    void* vrSystem_ = nullptr;
    void* vrOverlay_ = nullptr;
    void* vrSettings_ = nullptr;
    void* vrProperties_ = nullptr;
    void* vrInputFocus_ = nullptr;
};

} // namespace pixelvr

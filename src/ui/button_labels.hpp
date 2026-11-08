#pragma once

// Floating button labels: which controller button does what right now.
// The texts live in one table (button_labels.cpp); the live SteamVR bindings
// decide which button each one is drawn next to; the renderer places the cards.

#include "xr/xr_context.hpp"

#include <string>
#include <vector>

namespace pixelvr {

// One card: the lines shown next to one button (component) of one controller.
struct ButtonLabelCard {
    int hand = 1;           // 0 = left, 1 = right
    std::string component;  // render-model component: "button_a", "dpad", "thumbstick", ...
    std::vector<std::string> lines;
};

// What the player is doing, for the label texts.
struct ButtonLabelState {
    enum class Context { Menus, Watching, Keyboard };
    Context context = Context::Watching;
    bool dialog = false;           // menus: an action menu / dialog is up over the hub
    bool dialogLeftRight = false;  // ... and it takes left / right (the two-button delete confirm)
    bool optionsWork = false;      // menus: Options opens / closes File Actions (Files tab, or a menu is up)
    bool recenterWorks = false;    // menus: the hub, Downloads or Settings is open (they recenter)
    bool noStick = false;          // menus: Downloads or the download confirm (no stick / D-pad there)
    bool laserOnWeb = false;       // watching: the laser is on a web page (B = page back, stick Y scrolls)
    bool mediaOpen = false;        // the selected screen has something open
    bool web = false;              // ... and it is a web page (the D-pad leaves pages alone)
    bool music = false;            // ... and it is music (Previous / Next = tracks)
    bool sphere = false;           // 360 / 180 projection
    bool controlsVisible = false;  // the player controls are shown (Back hides them)
};

// The text for `a` in this state; empty when the action does nothing there.
std::string buttonLabelText(InputAction a, const ButtonLabelState& s);

// Builds the cards from each action's bound source paths (indexed by
// InputAction, e.g. "/user/hand/right/input/a/click").
std::vector<ButtonLabelCard> buildButtonLabelCards(
    const std::vector<std::vector<std::string>>& boundSources, const ButtonLabelState& s);

// "/user/hand/right/input/thumbstick/click" -> hand 1, "thumbstick", detail
// "click". Dpad directions give the direction as detail ("up"). False when the
// path is not a controller button we can point at.
bool parseLabelSource(const std::string& path, int& hand, std::string& component, std::string& detail);

// Node / component name reduced for matching ("frame_controller_right_a_button",
// "button_a" and "a_button" all give "a").
std::string labelComponentKey(const std::string& name);

// One component frame from SteamVR's controller render-model json.
struct SteamVrComponent {
    std::string name;
    float origin[3] = {0.0f, 0.0f, 0.0f};
    float rotateXyz[3] = {0.0f, 0.0f, 0.0f};  // degrees
};
// Reads the Steam Frame controller json for `hand` from the SteamVR install
// (read-only, at runtime). False when it is not there or cannot be parsed.
bool loadSteamVrControllerComponents(int hand, std::vector<SteamVrComponent>& out, std::string& pathOut);

} // namespace pixelvr

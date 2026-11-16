#pragma once

namespace pixelvr {

// One focus for the lists the laser and the stick / D-pad both move (file grid, Settings rows,
// menus and dialogs): whichever was used last owns it. A stick / D-pad step, a button that opened
// something, or a new page gives the focus to the buttons; a beam resting on the panel then
// highlights nothing. The laser takes the focus back once its spot on the panel has moved a few
// centimetres from where it was at that moment, or with a trigger pull.
class LaserFocus {
public:
    static constexpr float kTakeBackDistance = 0.03f;  // metres on the panel (hand tremor stays below)

    // Every frame, before the lists read their input: where the laser is on the panel
    // (onPanel false: not on it) and whether the trigger was pulled this frame.
    void update(bool onPanel, float x, float y, bool trigger) {
        onPanel_ = onPanel;
        x_ = x;
        y_ = y;
        if (laserOwns_ || !onPanel) return;
        if (trigger) {
            laserOwns_ = true;
            return;
        }
        if (!anchored_) {  // the beam came onto the panel: measure from here
            anchored_ = true;
            ax_ = x;
            ay_ = y;
            return;
        }
        const float dx = x - ax_;
        const float dy = y - ay_;
        if (dx * dx + dy * dy > kTakeBackDistance * kTakeBackDistance) laserOwns_ = true;
    }
    // The buttons own the focus from now on (until the laser moves or the trigger is pulled).
    void hold() {
        laserOwns_ = false;
        anchored_ = onPanel_;
        ax_ = x_;
        ay_ = y_;
    }
    // A hovered row / card may take the selection only while this is true.
    bool laserOwns() const { return laserOwns_; }

private:
    bool laserOwns_ = true;
    bool onPanel_ = false;
    float x_ = 0.0f, y_ = 0.0f;
    bool anchored_ = false;
    float ax_ = 0.0f, ay_ = 0.0f;
};

}  // namespace pixelvr

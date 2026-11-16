#pragma once

// The UI's shared design tokens: one text-size scale, one control-height scale, padding steps,
// corner radii and border widths in metres, the panel surfaces, the fixed meaning colours (danger,
// success, warning), the text roles and the one recipe for hover / focus / selected. Every screen
// the renderer draws by hand (hub, dialogs, dock, keyboard, toasts, music view) takes its sizes and
// colours from here, so the same control looks and measures the same everywhere.
//
// Sizes are panel metres (the hub is 1.80 x 1.12 m). Text sizes are line heights at text size
// 100 %; the Text Size setting multiplies them. Colours are sRGB, RGBA.

#include "ui/ui_colors.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace pixelvr::uitok {

using Rgba = std::array<float, 4>;

// --- Text sizes: five steps, nothing smaller than Caption ---
constexpr float kTextTitle = 0.038f;      // page headers, dialog titles, the dock's time
constexpr float kTextLarge = 0.034f;      // the folder path, the keyboard's input line
constexpr float kTextBody = 0.030f;       // rows, buttons, pills, field values, keys, dialog text
constexpr float kTextSecondary = 0.026f;  // card names, subtitles, hints, rail sections
constexpr float kTextCaption = 0.022f;    // labels, badges, details, column captions (the floor)

// --- Control half-heights: four steps ---
constexpr float kCtlS = 0.028f;   // toolbar buttons, segments, action-menu rows, form fields, close X
constexpr float kCtlM = 0.034f;   // dialog footers (Save / Cancel / Apply), dock pills, list pills
constexpr float kCtlL = 0.040f;   // keyboard keys
constexpr float kCtlXL = 0.046f;  // the two answers of a confirm dialog
// Rows stacked in a dialog: one pitch for a kCtlS row plus a gap.
constexpr float kRowGap = 0.012f;

// --- Padding steps ---
constexpr float kPadXS = 0.008f;
constexpr float kPadS = 0.016f;
constexpr float kPadM = 0.030f;
constexpr float kPadL = 0.050f;  // dialog frame: title and content inset from the edges

// --- Corner radii (metres, the same corner whatever the element's size) ---
constexpr float kRadiusS = 0.012f;   // keys, chips, small buttons inside rows
constexpr float kRadiusM = 0.016f;   // cards, list lines, wells
constexpr float kRadiusL = 0.040f;   // hub, dock, every dialog, keyboard sheet, backdrops
constexpr float kRadiusPill = 1.0f;  // capsule (any element: the radius is clamped to half its height)

// --- Border widths (metres) ---
constexpr float kBorderRest = 0.0020f;    // every element at rest, hover, selected
constexpr float kBorderStrong = 0.0030f;  // focus (what A presses), editing, glass panel rims

// --- Icon half-sizes ---
constexpr float kIconS = 0.014f;  // inside toolbar buttons and row chips
constexpr float kIconM = 0.017f;  // rail tabs, dock pills, track rows
constexpr float kIconL = 0.022f;  // round dock buttons (both rows)

// --- Focus glow ---
constexpr float kGlowSpread = 0.014f;
constexpr float kGlowAlpha = 0.26f;

// --- Surfaces: one neutral slate glass for hub, dock, dialogs and keyboard ---
// Large surfaces take the Panel Transparency setting; the accent is reserved for focus,
// selection and progress.
constexpr Rgba kGlassPanel = {0.078f, 0.092f, 0.115f, 0.93f};
constexpr Rgba kGlassRail = {0.050f, 0.060f, 0.078f, 0.60f};
constexpr Rgba kGlassCard = {0.112f, 0.130f, 0.160f, 0.90f};
constexpr Rgba kGlassCardHover = {0.150f, 0.175f, 0.220f, 0.97f};
constexpr Rgba kGlassControl = {0.135f, 0.155f, 0.195f, 0.92f};
constexpr Rgba kGlassWell = {0.035f, 0.042f, 0.058f, 0.92f};
constexpr Rgba kGlassVeil = {0.85f, 0.90f, 1.00f, 0.09f};  // neutral selection on the rail
constexpr Rgba kGlassRim = {0.70f, 0.78f, 0.90f, 0.34f};
constexpr Rgba kGlassRimFaint = {0.70f, 0.78f, 0.90f, 0.13f};
constexpr Rgba kGlassDivider = {0.70f, 0.78f, 0.90f, 0.12f};
constexpr Rgba kTrack = {0.16f, 0.19f, 0.25f, 0.95f};    // empty part of a bar / slider / switch
constexpr Rgba kKnob = {0.97f, 0.98f, 1.00f, 1.00f};     // slider / switch knob, caret
constexpr Rgba kKnobOff = {0.62f, 0.67f, 0.75f, 0.98f};  // switch knob while off, grab handle
constexpr Rgba kBadgeBg = {0.03f, 0.04f, 0.06f, 0.80f};  // dark chip over a picture (format, duration, number)
// Backdrop behind every dialog and the keyboard: one dimming for all overlays.
constexpr Rgba kScrim = {0.0f, 0.0f, 0.0f, 0.72f};
// Label colours on a filled button, picked by contrast (labelOn).
constexpr Rgba kLabelDark = {0.03f, 0.05f, 0.09f, 1.0f};
constexpr Rgba kLabelLight = {0.97f, 0.98f, 1.0f, 1.0f};
// The modal surface: the panel glass, a little more opaque so the dialog reads over the page.
inline Rgba modalSurface(float opacity) {
    return {kGlassPanel[0], kGlassPanel[1], kGlassPanel[2], 0.98f * (0.55f + 0.45f * opacity)};
}

// --- Meaning colours: fixed, never taken from the accent ---
struct Semantic {
    Rgba fill;   // a button / row that does it (Delete)
    Rgba hover;  // the same under the laser or the focus
    Rgba rim;
    Rgba text;   // text and icons in this meaning on the dark panels
    Rgba tint;   // a whole card in this state (a finished download)
    float hue;   // 0..1, for the accent collision test
};
constexpr Semantic kDanger = {{0.58f, 0.12f, 0.18f, 0.95f}, {0.78f, 0.18f, 0.24f, 1.0f}, {0.95f, 0.35f, 0.42f, 0.95f},
                              {1.0f, 0.55f, 0.55f, 1.0f},   {0.16f, 0.06f, 0.08f, 0.92f}, 352.0f / 360.0f};
constexpr Semantic kSuccess = {{0.14f, 0.60f, 0.36f, 0.95f}, {0.20f, 0.72f, 0.44f, 1.0f}, {0.45f, 0.90f, 0.62f, 0.60f},
                               {0.45f, 0.92f, 0.58f, 1.0f},  {0.070f, 0.130f, 0.110f, 0.92f}, 148.0f / 360.0f};
constexpr Semantic kWarning = {{0.55f, 0.36f, 0.08f, 0.95f}, {0.72f, 0.48f, 0.12f, 1.0f}, {1.0f, 0.72f, 0.36f, 0.95f},
                               {1.0f, 0.72f, 0.36f, 1.0f},   {0.15f, 0.11f, 0.05f, 0.92f}, 34.0f / 360.0f};
// The vault's gold (not a warning: moving into the vault is safe).
constexpr Semantic kVault = {{0.28f, 0.22f, 0.08f, 0.95f}, {0.45f, 0.36f, 0.12f, 1.0f}, {0.95f, 0.82f, 0.35f, 0.95f},
                             {1.0f, 0.86f, 0.45f, 1.0f},   {0.14f, 0.12f, 0.06f, 0.92f}, 46.0f / 360.0f};
// The count badge on Downloads / the dock's Main Menu pill: the signal red.
constexpr Rgba kBadgeRed = {0.92f, 0.24f, 0.28f, 0.98f};
// The subtitle / video track icons: a neutral grey that no accent can match.
constexpr Rgba kNeutralIcon = {0.72f, 0.78f, 0.88f, 1.0f};

// --- Colour maths ---
inline float contrast(const float a[3], const float b[3]) {
    const float la = uicolor::luminance(a), lb = uicolor::luminance(b);
    return (std::max(la, lb) + 0.05f) / (std::min(la, lb) + 0.05f);
}
inline float hueDistance(float a, float b) {
    const float d = std::fabs(a - b);
    return std::min(d, 1.0f - d);
}
// The picked accent looks like this meaning colour (close hue, clearly coloured): an element in that
// meaning then also shows its label / icon or a strong rim, so it does not rely on colour alone.
inline bool collides(const float accent[3], const Semantic& s) {
    float h = 0.0f, sat = 0.0f, v = 0.0f;
    uicolor::rgbToHsv(accent, h, sat, v);
    return sat >= 0.35f && v >= 0.30f && hueDistance(h, s.hue) < 28.0f / 360.0f;
}
// The label colour (dark or light) that reads best on a fill.
inline Rgba labelOn(const float fill[3]) {
    return contrast(kLabelDark.data(), fill) >= contrast(kLabelLight.data(), fill) ? kLabelDark : kLabelLight;
}
// The primary button: accent x kPrimaryRest at rest, x 1.0 under the laser / focus.
constexpr float kPrimaryRest = 0.85f;
// Its label: the one (dark or light) with the better contrast in BOTH states, so the label does not
// flip colour when the laser moves onto the button.
inline Rgba labelOnAccent(const float accent[3]) {
    const float rest[3] = {accent[0] * kPrimaryRest, accent[1] * kPrimaryRest, accent[2] * kPrimaryRest};
    auto worst = [&](const Rgba& l) { return std::min(contrast(l.data(), rest), contrast(l.data(), accent)); };
    return worst(kLabelDark) >= worst(kLabelLight) ? kLabelDark : kLabelLight;
}
// The surface text is guarded against: the lightest one text sits on (a hovered card).
constexpr Rgba kTextGuardSurface = kGlassCardHover;
constexpr float kSecondaryMix = 0.30f, kSecondaryContrast = 4.5f;
constexpr float kDisabledMix = 0.55f, kDisabledContrast = 3.0f;
// `text` mixed toward the panel colour by `mix` (no channel bias), then moved back toward `text`
// until it has `minContrast` on the guard surface. The primary text colour is already guarded
// (uicolor::usable), so the result never needs to be brighter than the primary.
inline Rgba textRole(const float text[3], float mix, float minContrast) {
    auto at = [&](float m, float out[3]) {
        for (int k = 0; k < 3; ++k) out[k] = text[k] + (kGlassPanel[k] - text[k]) * m;
    };
    float c[3];
    at(mix, c);
    if (contrast(c, kTextGuardSurface.data()) < minContrast) {
        float lo = 0.0f, hi = mix;  // the largest mix that still has the contrast
        for (int i = 0; i < 16; ++i) {
            const float m = 0.5f * (lo + hi);
            at(m, c);
            (contrast(c, kTextGuardSurface.data()) >= minContrast ? lo : hi) = m;
        }
        at(lo, c);
    }
    return {c[0], c[1], c[2], 1.0f};
}
inline Rgba textSecondary(const float text[3]) { return textRole(text, kSecondaryMix, kSecondaryContrast); }
inline Rgba textDisabled(const float text[3]) { return textRole(text, kDisabledMix, kDisabledContrast); }
// The accent where it colours glyphs or thin icons: at least as bright as the text guard
// (a dark accent stays fine for fills, borders and bars).
inline Rgba accentText(const float accent[3]) {
    float c[3];
    uicolor::toFloat(uicolor::ensureLuminance(uicolor::fromFloat(accent), uicolor::kMinTextLuminance), c);
    return {c[0], c[1], c[2], 1.0f};
}

// --- The one recipe for interactive states ---
// hover = lighter fill and a brighter rim; focus (what A presses) = accent rim at the strong width
// plus an accent glow; selected (the value in use, the open tab) = accent-tinted fill with a softer
// accent rim; pressed = the click ring drawn where the press landed (shared with the laser click).
// Disabled halves the fill and the rim.
struct State {
    bool hover = false;
    bool focus = false;
    bool selected = false;
    bool disabled = false;
};
struct Look {
    Rgba fill;
    Rgba rim;
    float border;  // metres
    bool glow;
    Rgba glowCol;
};
inline Rgba accentTint(const float accent[3], bool hover) {
    const float k = hover ? 0.34f : 0.26f;
    return {accent[0] * k, accent[1] * k, accent[2] * (k + 0.06f), 0.96f};
}
// rest / hoverFill: the element's own surfaces (a card, a control, a well).
inline Look look(State s, const Rgba& rest, const Rgba& hoverFill, const float accent[3]) {
    Look l{};
    const bool lit = (s.hover || s.focus) && !s.disabled;
    l.fill = s.selected ? accentTint(accent, lit) : (lit ? hoverFill : rest);
    l.rim = s.selected ? Rgba{accent[0], accent[1], accent[2], 0.70f} : (lit ? kGlassRim : kGlassRimFaint);
    l.border = kBorderRest;
    if (s.focus) {
        l.rim = {accent[0], accent[1], accent[2], 1.0f};
        l.border = kBorderStrong;
    }
    l.glow = s.focus;
    l.glowCol = {accent[0], accent[1], accent[2], kGlowAlpha};
    if (s.disabled) {
        l.fill[3] *= 0.5f;
        l.rim[3] *= 0.5f;
    }
    return l;
}

}  // namespace pixelvr::uitok

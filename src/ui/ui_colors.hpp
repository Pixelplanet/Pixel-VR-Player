#pragma once

// The UI's two theme colours (text and accent): the suggested swatches, hex / HSV conversion and
// the guard rails that keep a picked colour usable on the dark panels. Shared by the Settings
// menu (which stores the colours) and the renderer (which draws with them and draws the picker).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace pixelvr::uicolor {

constexpr int kSwatchCount = 5;
struct Swatch {
    const char* name;
    uint32_t rgb;  // 0xRRGGBB
};
// The five fixed choices each row had before the picker (index = the old saved value).
constexpr Swatch kTextSwatches[kSwatchCount] = {
    {"Pure White", 0xF7FAFF}, {"Ice Cyan", 0xD1F2FF}, {"Warm Cream", 0xFFF5E0},
    {"Soft Gold", 0xFFE694},  {"Mint Green", 0xC7FADB},
};
constexpr Swatch kAccentSwatches[kSwatchCount] = {
    {"Cyber Cyan", 0x40C7FF},   {"Emerald Green", 0x2ED985}, {"Amber Gold", 0xFFAD33},
    {"Royal Purple", 0xAD6BFF}, {"Crimson Red", 0xF5475C},
};
inline const Swatch* swatches(bool accent) { return accent ? kAccentSwatches : kTextSwatches; }
// The swatch a colour is, or -1 for a colour of the user's own.
inline int swatchIndex(bool accent, uint32_t rgb) {
    for (int i = 0; i < kSwatchCount; ++i) {
        if (swatches(accent)[i].rgb == (rgb & 0xFFFFFFu)) return i;
    }
    return -1;
}

inline void toFloat(uint32_t rgb, float out[3]) {
    out[0] = static_cast<float>((rgb >> 16) & 0xFF) / 255.0f;
    out[1] = static_cast<float>((rgb >> 8) & 0xFF) / 255.0f;
    out[2] = static_cast<float>(rgb & 0xFF) / 255.0f;
}
inline uint32_t fromFloat(const float c[3]) {
    auto ch = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return (ch(c[0]) << 16) | (ch(c[1]) << 8) | ch(c[2]);
}
// "RRGGBB" (upper case, no '#').
inline std::string toHex(uint32_t rgb) {
    char b[8];
    std::snprintf(b, sizeof(b), "%06X", static_cast<unsigned>(rgb & 0xFFFFFFu));
    return b;
}
// Reads "RRGGBB" or "#RRGGBB"; false for anything else.
inline bool parseHex(const std::string& s, uint32_t& rgb) {
    const std::size_t start = (!s.empty() && s[0] == '#') ? 1 : 0;
    if (s.size() - start != 6) return false;
    uint32_t v = 0;
    for (std::size_t i = start; i < s.size(); ++i) {
        const char c = s[i];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        v = (v << 4) | static_cast<uint32_t>(d);
    }
    rgb = v;
    return true;
}

// Hue, saturation and brightness, all 0..1 (hue 1 = one full turn).
inline void hsvToRgb(float h, float s, float v, float out[3]) {
    h = h - std::floor(h);
    const float x = h * 6.0f;
    const int i = static_cast<int>(x) % 6;
    const float f = x - std::floor(x);
    const float p = v * (1.0f - s), q = v * (1.0f - s * f), t = v * (1.0f - s * (1.0f - f));
    switch (i) {
        case 0: out[0] = v; out[1] = t; out[2] = p; break;
        case 1: out[0] = q; out[1] = v; out[2] = p; break;
        case 2: out[0] = p; out[1] = v; out[2] = t; break;
        case 3: out[0] = p; out[1] = q; out[2] = v; break;
        case 4: out[0] = t; out[1] = p; out[2] = v; break;
        default: out[0] = v; out[1] = p; out[2] = q; break;
    }
}
// A grey (or black) has no hue: `h` then keeps the value it came in with.
inline void rgbToHsv(const float c[3], float& h, float& s, float& v) {
    const float mx = std::max(c[0], std::max(c[1], c[2]));
    const float mn = std::min(c[0], std::min(c[1], c[2]));
    const float d = mx - mn;
    v = mx;
    s = mx > 1e-5f ? d / mx : 0.0f;
    if (d < 1e-5f) return;
    float hh;
    if (mx == c[0]) hh = (c[1] - c[2]) / d + (c[1] < c[2] ? 6.0f : 0.0f);
    else if (mx == c[1]) hh = (c[2] - c[0]) / d + 2.0f;
    else hh = (c[0] - c[1]) / d + 4.0f;
    h = hh / 6.0f;
}

// How bright a colour looks (0 = black, 1 = white).
inline float luminance(const float c[3]) {
    auto lin = [](float v) { return std::pow(std::clamp(v, 0.0f, 1.0f), 2.2f); };
    return 0.2126f * lin(c[0]) + 0.7152f * lin(c[1]) + 0.0722f * lin(c[2]);
}
// The panels are dark, so a colour that is too dark on them is brightened: first at its own hue
// (up to full brightness), then, if that is still too dark (a deep blue), towards white.
constexpr float kMinTextLuminance = 0.30f;    // text stays readable
constexpr float kMinAccentLuminance = 0.12f;  // highlights, sliders and accent-coloured text stay visible
inline uint32_t ensureLuminance(uint32_t rgb, float minLum) {
    float c[3];
    toFloat(rgb, c);
    // (A little slack: a colour this function made stays as it is after rounding to 8 bits.)
    if (luminance(c) >= minLum - 0.006f) return rgb & 0xFFFFFFu;
    const float mx = std::max(c[0], std::max(c[1], c[2]));
    if (mx < 0.02f) c[0] = c[1] = c[2] = 0.02f;  // black has no hue to keep: it becomes a grey
    const float top = 1.0f / std::max(mx, 0.02f);
    float full[3] = {c[0] * top, c[1] * top, c[2] * top};
    float out[3];
    if (luminance(full) >= minLum) {
        float lo = 1.0f, hi = top;  // the smallest brightening that is enough
        for (int i = 0; i < 16; ++i) {
            const float k = 0.5f * (lo + hi);
            const float t[3] = {c[0] * k, c[1] * k, c[2] * k};
            (luminance(t) >= minLum ? hi : lo) = k;
        }
        for (int k = 0; k < 3; ++k) out[k] = c[k] * hi;
    } else {
        float lo = 0.0f, hi = 1.0f;  // the smallest share of white that is enough
        for (int i = 0; i < 16; ++i) {
            const float m = 0.5f * (lo + hi);
            const float t[3] = {full[0] + (1.0f - full[0]) * m, full[1] + (1.0f - full[1]) * m,
                                full[2] + (1.0f - full[2]) * m};
            (luminance(t) >= minLum ? hi : lo) = m;
        }
        for (int k = 0; k < 3; ++k) out[k] = full[k] + (1.0f - full[k]) * hi;
    }
    return fromFloat(out);
}
// The colour the UI really uses for a picked one.
inline uint32_t usable(bool accent, uint32_t rgb) {
    return ensureLuminance(rgb, accent ? kMinAccentLuminance : kMinTextLuminance);
}

}  // namespace pixelvr::uicolor

// Checks the UI colour code without the headset's display: the colour helpers (hex, HSV, the
// guard rails against too dark colours) and what the Settings menu does with the two colours
// (old saved choices become their colour, the picker pages, saving). Development tool, never
// released. It works in a temporary HOME, so the real settings are not touched.
//
//   ui_color_test

#include "gfx/ui_tokens.hpp"
#include "ui/settings_menu.hpp"
#include "ui/ui_colors.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace pixelvr;

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

std::string slurp(const std::string& path) {
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

float lum(uint32_t rgb) {
    float c[3];
    uicolor::toFloat(rgb, c);
    return uicolor::luminance(c);
}

// Opens the picker page from its row on UI & Appearance.
bool openPicker(SettingsMenu& m, const char* rowLabel) {
    m.setSection(1);
    for (int i = 0; i < static_cast<int>(m.rows().size()); ++i) {
        if (m.rows()[static_cast<std::size_t>(i)].label == rowLabel) {
            m.setSelected(i);
            m.activate();
            return true;
        }
    }
    return false;
}

}  // namespace

int main() {
    char tmpl[] = "/tmp/pvr-colour-test-XXXXXX";
    const char* home = mkdtemp(tmpl);
    if (!home) {
        std::printf("no temporary folder\n");
        return 2;
    }
    setenv("HOME", home, 1);
    const std::string conf = std::string(home) + "/.config/pixelvr/settings.conf";
    std::filesystem::create_directories(std::string(home) + "/.config/pixelvr");

    // --- helpers ---
    // The swatches are the colours the fixed choices had (the renderer's old float tables).
    const float oldText[5][3] = {{0.97f, 0.98f, 1.00f}, {0.82f, 0.95f, 1.00f}, {1.00f, 0.96f, 0.88f},
                                 {1.00f, 0.90f, 0.58f}, {0.78f, 0.98f, 0.86f}};
    const float oldAccent[5][3] = {{0.25f, 0.78f, 1.00f}, {0.18f, 0.85f, 0.52f}, {1.00f, 0.68f, 0.20f},
                                   {0.68f, 0.42f, 1.00f}, {0.96f, 0.28f, 0.36f}};
    bool same = true, untouched = true;
    for (int i = 0; i < 5; ++i) {
        same = same && uicolor::fromFloat(oldText[i]) == uicolor::kTextSwatches[i].rgb &&
               uicolor::fromFloat(oldAccent[i]) == uicolor::kAccentSwatches[i].rgb;
        untouched = untouched && uicolor::usable(false, uicolor::kTextSwatches[i].rgb) == uicolor::kTextSwatches[i].rgb &&
                    uicolor::usable(true, uicolor::kAccentSwatches[i].rgb) == uicolor::kAccentSwatches[i].rgb;
    }
    check(same, "swatches equal the old fixed colours");
    check(untouched, "the guard rails leave every swatch as it is");

    uint32_t c = 0;
    check(uicolor::parseHex("40c7ff", c) && c == 0x40C7FF && uicolor::parseHex("#40C7FF", c) && c == 0x40C7FF, "hex is read");
    check(!uicolor::parseHex("40c7f", c) && !uicolor::parseHex("40c7fg", c) && !uicolor::parseHex("", c), "bad hex is refused");
    check(uicolor::toHex(0x0A0B0C) == "0A0B0C", "hex is written");

    bool round = true;
    for (uint32_t rgb : {0x40C7FFu, 0xF5475Cu, 0x123456u, 0xFFFFFFu, 0x808080u, 0x00FF00u, 0xFF00FFu}) {
        float f[3], back[3], h = 0.0f, s = 0.0f, v = 0.0f;
        uicolor::toFloat(rgb, f);
        uicolor::rgbToHsv(f, h, s, v);
        uicolor::hsvToRgb(h, s, v, back);
        round = round && uicolor::fromFloat(back) == rgb;
    }
    check(round, "RGB -> HSV -> RGB gives the same colour");

    bool bright = true, stable = true;
    for (uint32_t rgb : {0x000000u, 0x0000FFu, 0x200000u, 0x101010u, 0x003300u, 0x4B0082u, 0x00007Fu}) {
        const uint32_t t = uicolor::usable(false, rgb), a = uicolor::usable(true, rgb);
        std::printf("      %06X -> text %06X (lum %.3f), accent %06X (lum %.3f)\n", rgb, t, lum(t), a, lum(a));
        bright = bright && lum(t) >= uicolor::kMinTextLuminance - 0.006f && lum(a) >= uicolor::kMinAccentLuminance - 0.006f;
        stable = stable && uicolor::usable(false, t) == t && uicolor::usable(true, a) == a;
    }
    check(bright, "too dark colours are brightened to the minimum");
    check(stable, "a brightened colour is not changed again");
    {
        float f[3], h = 0.0f, s = 0.0f, v = 0.0f, hb = 0.0f;
        uicolor::toFloat(0x0000FF, f);
        uicolor::rgbToHsv(f, h, s, v);
        uicolor::toFloat(uicolor::usable(false, 0x0000FF), f);
        uicolor::rgbToHsv(f, hb, s, v);
        check(std::fabs(h - hb) < 0.01f, "a brightened blue is still blue");
    }

    // --- design tokens: text roles, labels on accent fills, meaning colours ---
    {
        // Every text role keeps its contrast on the lightest surface text sits on, for the
        // swatches and for the darkest text colours the guard allows.
        bool roles = true, order = true;
        std::vector<uint32_t> texts;
        for (const auto& sw : uicolor::kTextSwatches) texts.push_back(sw.rgb);
        for (uint32_t rgb : {0x000000u, 0x0000FFu, 0x400000u, 0x606060u}) texts.push_back(uicolor::usable(false, rgb));
        const float* surf = uitok::kTextGuardSurface.data();
        for (uint32_t rgb : texts) {
            float t[3];
            uicolor::toFloat(rgb, t);
            const auto sec = uitok::textSecondary(t), dis = uitok::textDisabled(t);
            const float cp = uitok::contrast(t, surf), cs = uitok::contrast(sec.data(), surf),
                        cd = uitok::contrast(dis.data(), surf);
            std::printf("      text %06X: primary %.1f:1, secondary %.1f:1, disabled %.1f:1\n", rgb, cp, cs, cd);
            roles = roles && cs >= std::min(cp, uitok::kSecondaryContrast) - 0.05f &&
                    cd >= std::min(cp, uitok::kDisabledContrast) - 0.05f;
            order = order && cp >= cs - 0.01f && cs >= cd - 0.01f;
        }
        check(roles, "secondary and disabled text keep their contrast guard");
        check(order, "primary >= secondary >= disabled");

        // A label on an accent fill is readable for any accent (stock and picked, guarded), at rest
        // and under the laser, with one label colour for both.
        bool labels = true;
        std::vector<uint32_t> accents;
        for (const auto& sw : uicolor::kAccentSwatches) accents.push_back(sw.rgb);
        for (uint32_t rgb : {0xFFFF00u, 0x00FFAAu, 0x1A237Eu, 0x4B0082u, 0x000000u, 0x808080u, 0xFFFFFFu})
            accents.push_back(uicolor::usable(true, rgb));
        for (uint32_t rgb : accents) {
            float a[3];
            uicolor::toFloat(rgb, a);
            const auto lab = uitok::labelOnAccent(a);
            for (float k : {uitok::kPrimaryRest, 1.0f}) {
                const float fill[3] = {a[0] * k, a[1] * k, a[2] * k};
                const float c = uitok::contrast(lab.data(), fill);
                std::printf("      accent %06X x%.2f: label %s %.1f:1\n", rgb, k, lab[0] > 0.5f ? "light" : "dark", c);
                labels = labels && c >= 3.5f;
            }
        }
        check(labels, "the primary button's label has at least 3.5:1 (bold text) on every accent");

        // Accent text is never darker than the text guard.
        float deep[3];
        uicolor::toFloat(uicolor::usable(true, 0x1A237E), deep);
        check(uicolor::luminance(uitok::accentText(deep).data()) >= uicolor::kMinTextLuminance - 0.01f,
              "a dark accent is brightened for text");

        // Collisions: Crimson looks like Delete, Emerald like Done, Amber like a warning; Cyan / Purple like none.
        auto coll = [](uint32_t rgb, const uitok::Semantic& s) {
            float a[3];
            uicolor::toFloat(rgb, a);
            return uitok::collides(a, s);
        };
        check(coll(0xF5475C, uitok::kDanger) && coll(0x2ED985, uitok::kSuccess) && coll(0xFFAD33, uitok::kWarning),
              "red / green / amber accents are recognised as close to the meaning colours");
        check(!coll(0x40C7FF, uitok::kDanger) && !coll(0x40C7FF, uitok::kSuccess) && !coll(0x40C7FF, uitok::kWarning) &&
                  !coll(0xAD6BFF, uitok::kDanger) && !coll(0x808080, uitok::kDanger),
              "cyan, purple and grey accents collide with none");
    }

    // --- Settings: a file from before the picker ---
    {
        std::ofstream f(conf);
        f << "ui_text_scale=2\nui_text_color=3\nui_accent_color=4\n";
    }
    {
        SettingsMenu m;
        check(m.uiTextRgb() == uicolor::kTextSwatches[3].rgb && m.uiAccentRgb() == uicolor::kAccentSwatches[4].rgb,
              "old saved choices become their colours");
        m.saveConfig();
        const std::string s = slurp(conf);
        check(s.find("ui_text_rgb=FFE694") != std::string::npos && s.find("ui_accent_rgb=F5475C") != std::string::npos &&
                  s.find("ui_text_color=") == std::string::npos,
              "they are saved as RGB");

        // The picker pages.
        check(openPicker(m, "Accent Color") && m.colorPickerTarget() == 1 && m.rows().empty(), "the accent row opens its picker");
        std::printf("      title: %s | status: %s\n", m.title().c_str(), m.status().c_str());
        float f3[3], h = 0.0f, s2 = 0.0f, v = 0.0f;
        uicolor::toFloat(uicolor::kAccentSwatches[4].rgb, f3);
        uicolor::rgbToHsv(f3, h, s2, v);
        check(std::fabs(m.pickerHue() - h) < 1e-4f && std::fabs(m.pickerSat() - s2) < 1e-4f && std::fabs(m.pickerVal() - v) < 1e-4f,
              "the markers start on the colour in use");
        m.pickerSetHue(1.0f / 3.0f);  // green
        m.pickerSetSatVal(1.0f, 1.0f);
        check(m.uiAccentRgb() == 0x00FF00 && !m.pickerAdjusted(), "dragging changes the colour at once");
        check(slurp(conf).find("ui_accent_rgb=F5475C") != std::string::npos, "... without writing the file on every step");
        m.pickerSetSatVal(1.0f, 0.05f);  // nearly black
        check(m.pickerAdjusted() && lum(m.uiAccentRgb()) >= uicolor::kMinAccentLuminance - 0.006f && m.pickerVal() == 0.05f,
              "a nearly black accent is brightened, the marker stays where it was put");
        m.pickerSetSatVal(0.5f, 0.9f);
        const uint32_t picked = m.uiAccentRgb();
        check(m.navigateBack() && m.section() == 1 && m.selected() == 2, "Back returns to the row");
        check(slurp(conf).find("ui_accent_rgb=" + uicolor::toHex(picked)) != std::string::npos, "Back keeps (saves) the colour");
        std::printf("      row value: %s\n", m.rows()[2].value.c_str());

        check(openPicker(m, "Text Color") && m.colorPickerTarget() == 0, "the text row opens its picker");
        m.pickerSetHue(2.0f / 3.0f);  // blue
        m.pickerSetSatVal(1.0f, 1.0f);
        check(m.pickerAdjusted() && lum(m.uiTextRgb()) >= uicolor::kMinTextLuminance - 0.006f, "a pure blue text colour is lightened");
        m.pickerCommit();
        m.pickerPickSwatch(1);
        check(m.uiTextRgb() == uicolor::kTextSwatches[1].rgb && slurp(conf).find("ui_text_rgb=D1F2FF") != std::string::npos,
              "a suggested colour is taken and saved");
        m.pickerReset();
        check(m.uiTextRgb() == uicolor::kTextSwatches[0].rgb, "Reset gives the default");
        m.pickerSetSatVal(0.3f, 0.8f);
        m.close();  // leaving Settings mid-change keeps the colour too
        check(slurp(conf).find("ui_text_rgb=" + uicolor::toHex(m.uiTextRgb())) != std::string::npos, "closing Settings saves the colour");
        m.open();
        check(m.colorPickerTarget() < 0 && m.section() == 1, "Settings reopens on UI & Appearance");
    }
    {
        SettingsMenu m;  // a new start reads the RGB values
        const std::string s = slurp(conf);
        check(s.find("ui_text_rgb=" + uicolor::toHex(m.uiTextRgb())) != std::string::npos &&
                  s.find("ui_accent_rgb=" + uicolor::toHex(m.uiAccentRgb())) != std::string::npos && m.uiAccentRgb() != 0x40C7FF,
              "the saved colours are read back");
    }
    {
        std::ofstream f(conf);  // a hand-edited file: black text, a broken accent value
        f << "ui_text_rgb=000000\nui_accent_rgb=zzzzzz\nui_accent_color=2\n";
    }
    {
        SettingsMenu m;
        check(lum(m.uiTextRgb()) >= uicolor::kMinTextLuminance - 0.006f, "black text from the file is brightened");
        check(m.uiAccentRgb() == uicolor::kAccentSwatches[2].rgb, "a broken RGB value falls back to the old choice");
    }

    std::error_code ec;
    std::filesystem::remove_all(home, ec);
    std::printf("%s\n", failures == 0 ? "ALL OK" : "FAILURES");
    return failures == 0 ? 0 : 1;
}

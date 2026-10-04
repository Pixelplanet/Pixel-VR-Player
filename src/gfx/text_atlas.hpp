#pragma once

#include <cstdint>
#include <vector>

namespace pixelvr {

// A single rasterized glyph: atlas UV rect + placement metrics (pixels).
struct Glyph {
    float u0 = 0.0f, v0 = 0.0f, u1 = 0.0f, v1 = 0.0f;
    float width = 0.0f, height = 0.0f;
    float bearingX = 0.0f, bearingY = 0.0f;
    float advance = 0.0f;
};

// An R8 coverage atlas of ASCII 32..126 plus per-glyph metrics.
struct FontAtlas {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels;  // single channel (coverage)
    Glyph glyphs[128];            // indexed by ASCII code
    float lineHeight = 0.0f;
    float ascent = 0.0f;
    bool ok = false;
};

// Rasterizes printable ASCII from a TrueType font into one R8 atlas. Returns
// ok=false if FreeType is unavailable or the font cannot be opened.
FontAtlas buildFontAtlas(const char* ttfPath, int pixelHeight);

} // namespace pixelvr

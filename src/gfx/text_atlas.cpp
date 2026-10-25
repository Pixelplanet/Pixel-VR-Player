#include "gfx/text_atlas.hpp"

#include "util/logging.hpp"

#ifdef PIXELVR_HAVE_FREETYPE
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MODULE_H
#endif

namespace pixelvr {

FontAtlas buildFontAtlas(const char* ttfPath, int pixelHeight) {
    FontAtlas atlas;
#ifdef PIXELVR_HAVE_FREETYPE
    FT_Library lib = nullptr;
    if (FT_Init_FreeType(&lib) != 0) {
        PIXELVR_LOG_ERROR("FreeType: init failed");
        return atlas;
    }
    // Expand the distance field ramp from FreeType's default 2px to 8px.
    // At typical VR UI viewing distances, 8 font pixels produces a smooth,
    // noise-free gradient spanning multiple screen pixels, completely eliminating
    // edge shimmering during head movement.
    constexpr FT_UInt kSpread = 8;
    FT_Property_Set(lib, "sdf", "spread", &kSpread);
    FT_Property_Set(lib, "bsdf", "spread", &kSpread);
    atlas.spread = static_cast<float>(kSpread);

    FT_Face face = nullptr;
    if (FT_New_Face(lib, ttfPath, 0, &face) != 0) {
        PIXELVR_LOG_ERROR("FreeType: cannot open %s", ttfPath);
        FT_Done_FreeType(lib);
        return atlas;
    }
    FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(pixelHeight));

    constexpr int kFirst = 32;
    constexpr int kLast = 255;
    constexpr int kCols = 16;
    const int count = kLast - kFirst + 1;
    const int rows = (count + kCols - 1) / kCols;
    const int cell = pixelHeight * 2;  // room for the glyph plus the SDF border

    atlas.width = kCols * cell;
    atlas.height = rows * cell;
    atlas.pixels.assign(static_cast<std::size_t>(atlas.width) * atlas.height, 0);
    atlas.lineHeight = face->size->metrics.height / 64.0f;
    atlas.ascent = face->size->metrics.ascender / 64.0f;

    for (int ch = kFirst; ch <= kLast; ++ch) {
        if (ch >= 137 && ch < 160) {
            continue;
        }
        FT_ULong codepoint = static_cast<FT_ULong>(ch);
        if (ch == 127) codepoint = 0x2022UL;      // bullet (•)
        else if (ch == 128) codepoint = 0x20ACUL; // Euro (€)
        else if (ch == 129) codepoint = 0x2013UL; // en-dash (–)
        else if (ch == 130) codepoint = 0x2014UL; // em-dash (—)
        else if (ch == 131) codepoint = 0x2018UL; // left single quote (‘)
        else if (ch == 132) codepoint = 0x2019UL; // right single quote (’)
        else if (ch == 133) codepoint = 0x201CUL; // left double quote (“)
        else if (ch == 134) codepoint = 0x201DUL; // right double quote (”)
        else if (ch == 135) codepoint = 0x2026UL; // ellipsis (…)
        else if (ch == 136) codepoint = 0x2122UL; // trademark (™)

        if (FT_Load_Char(face, codepoint, FT_LOAD_DEFAULT) != 0) {
            if (ch == 127 && FT_Load_Char(face, 0x00B7UL, FT_LOAD_DEFAULT) == 0) {
                // fallback to middle dot
            } else if (ch == 160 && FT_Load_Char(face, 0x0020UL, FT_LOAD_DEFAULT) == 0) {
                // fallback to regular space for NBSP
            } else {
                continue;
            }
        }
        const FT_GlyphSlot g = face->glyph;
        // Render a signed-distance field (crisp at any VR distance); fall back to
        // a coverage bitmap if the SDF module is unavailable.
        if (FT_Render_Glyph(g, FT_RENDER_MODE_SDF) == 0) {
            atlas.sdf = true;
        } else {
            FT_Render_Glyph(g, FT_RENDER_MODE_NORMAL);
        }
        const int idx = ch - kFirst;
        const int cx = (idx % kCols) * cell;
        const int cy = (idx / kCols) * cell;
        const int gw = static_cast<int>(g->bitmap.width);
        const int gh = static_cast<int>(g->bitmap.rows);
        const int pitch = g->bitmap.pitch;  // may be negative (bottom-up)
        for (int y = 0; y < gh; ++y) {
            const unsigned char* src =
                g->bitmap.buffer + static_cast<std::ptrdiff_t>(y) * pitch;
            uint8_t* dst =
                &atlas.pixels[static_cast<std::size_t>(cy + y) * atlas.width + cx];
            for (int x = 0; x < gw; ++x) {
                dst[x] = src[x];
            }
        }
        Glyph& gl = atlas.glyphs[ch];
        gl.u0 = static_cast<float>(cx) / atlas.width;
        gl.v0 = static_cast<float>(cy) / atlas.height;
        gl.u1 = static_cast<float>(cx + gw) / atlas.width;
        gl.v1 = static_cast<float>(cy + gh) / atlas.height;
        gl.width = static_cast<float>(gw);
        gl.height = static_cast<float>(gh);
        gl.bearingX = static_cast<float>(g->bitmap_left);
        gl.bearingY = static_cast<float>(g->bitmap_top);
        gl.advance = g->advance.x / 64.0f;
    }

    FT_Done_Face(face);
    FT_Done_FreeType(lib);
    atlas.ok = true;
    PIXELVR_LOG_INFO("Font atlas %dx%d from %s (%dpx%s)", atlas.width, atlas.height,
                     ttfPath, pixelHeight, atlas.sdf ? " SDF" : "");
#else
    (void)ttfPath;
    (void)pixelHeight;
#endif
    return atlas;
}

} // namespace pixelvr

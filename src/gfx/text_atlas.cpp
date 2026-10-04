#include "gfx/text_atlas.hpp"

#include "util/logging.hpp"

#ifdef PIXELVR_HAVE_FREETYPE
#include <ft2build.h>
#include FT_FREETYPE_H
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
    FT_Face face = nullptr;
    if (FT_New_Face(lib, ttfPath, 0, &face) != 0) {
        PIXELVR_LOG_ERROR("FreeType: cannot open %s", ttfPath);
        FT_Done_FreeType(lib);
        return atlas;
    }
    FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(pixelHeight));

    constexpr int kFirst = 32;
    constexpr int kLast = 126;
    constexpr int kCols = 16;
    const int count = kLast - kFirst + 1;
    const int rows = (count + kCols - 1) / kCols;
    const int cell = pixelHeight + pixelHeight / 2 + 2;  // generous glyph box

    atlas.width = kCols * cell;
    atlas.height = rows * cell;
    atlas.pixels.assign(static_cast<std::size_t>(atlas.width) * atlas.height, 0);
    atlas.lineHeight = face->size->metrics.height / 64.0f;
    atlas.ascent = face->size->metrics.ascender / 64.0f;

    for (int ch = kFirst; ch <= kLast; ++ch) {
        if (FT_Load_Char(face, static_cast<FT_ULong>(ch), FT_LOAD_RENDER) != 0) {
            continue;
        }
        const FT_GlyphSlot g = face->glyph;
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
    PIXELVR_LOG_INFO("Font atlas %dx%d from %s (%dpx)", atlas.width, atlas.height,
                     ttfPath, pixelHeight);
#else
    (void)ttfPath;
    (void)pixelHeight;
#endif
    return atlas;
}

} // namespace pixelvr

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <vector>

namespace ember::ui {

// ---------------------------------------------------------------------------------------------
// Built-in bitmap font: Ember's own 5x7 pixel font, drawn for this project (nothing is taken from
// any game). Upper-case letters, digits and a little punctuation; lower-case input is shown as
// upper case and anything else as '?'. Glyphs live in an 8x8-pixel grid cell inside a 16-column
// atlas, so the atlas is 128 pixels wide. Each glyph occupies a 6x8 pixel advance box (5 columns
// plus one blank column; 7 rows plus one blank row).
// ---------------------------------------------------------------------------------------------

inline constexpr int kCell = 8;
inline constexpr int kAtlasColumns = 16;
inline constexpr int kAdvance = 6;      ///< pixels from one glyph to the next
inline constexpr int kLineHeight = 9;   ///< pixels from one line to the next

struct FontAtlas {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba; ///< white with alpha = coverage
    int glyph_count = 0;
    int solid_index = 0;            ///< a fully white cell, for plain rectangles
};

/// Built on first use.
[[nodiscard]] const FontAtlas& Font();

/// Cell index for a character (lower case maps to upper case, unknown characters to '?').
[[nodiscard]] int GlyphIndex(char c);

struct UvRect {
    float u0, v0, u1, v1;
};
/// Texture coordinates of the 6x8 advance box of a cell.
[[nodiscard]] UvRect GlyphUv(int cell_index);
/// Texture coordinates of a point well inside the solid cell.
[[nodiscard]] UvRect SolidUv();

} // namespace ember::ui

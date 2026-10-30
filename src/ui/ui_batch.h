// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace ember::ui {

/// Packed 0xAABBGGRR, which is what an R8G8B8A8_UNORM vertex attribute reads on little-endian.
[[nodiscard]] constexpr std::uint32_t Rgba(int r, int g, int b, int a = 255) {
    return static_cast<std::uint32_t>(r) | static_cast<std::uint32_t>(g) << 8 |
           static_cast<std::uint32_t>(b) << 16 | static_cast<std::uint32_t>(a) << 24;
}

struct UiVertex {
    float x, y; ///< layout units (see menu.h), origin top left
    float u, v; ///< font atlas coordinates
    std::uint32_t color;
};
static_assert(sizeof(UiVertex) == 20);

/// Collects 2D quads for one frame. Everything is a textured quad from the font atlas: plain
/// rectangles use its solid cell, text uses its glyph cells.
class UiBatch {
public:
    static constexpr std::size_t kMaxQuads = 4096;

    void Clear() { vertices_.clear(); }
    void AddRect(float x, float y, float w, float h, std::uint32_t color);
    /// Border of `thickness` drawn inside the rectangle.
    void AddFrame(float x, float y, float w, float h, float thickness, std::uint32_t color);
    /// Draws `text` with its top-left at (x, y); `scale` is layout units per font pixel.
    void AddText(std::string_view text, float x, float y, float scale, std::uint32_t color);

    [[nodiscard]] static float TextWidth(std::string_view text, float scale);
    [[nodiscard]] static float TextHeight(float scale);

    [[nodiscard]] const std::vector<UiVertex>& Vertices() const noexcept { return vertices_; }

private:
    void Quad(float x, float y, float w, float h, float u0, float v0, float u1, float v1,
              std::uint32_t color);
    std::vector<UiVertex> vertices_;
};

} // namespace ember::ui

// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/ui_batch.h"

#include "ui/font.h"

namespace ember::ui {

void UiBatch::Quad(float x, float y, float w, float h, float u0, float v0, float u1, float v1,
                   std::uint32_t color) {
    if (vertices_.size() + 6 > kMaxQuads * 6 || w <= 0.0f || h <= 0.0f) return;
    const UiVertex a{x, y, u0, v0, color}, b{x + w, y, u1, v0, color}, c{x + w, y + h, u1, v1, color},
        d{x, y + h, u0, v1, color};
    vertices_.insert(vertices_.end(), {a, b, c, a, c, d});
}

void UiBatch::AddRect(float x, float y, float w, float h, std::uint32_t color) {
    const UvRect uv = SolidUv();
    Quad(x, y, w, h, uv.u0, uv.v0, uv.u1, uv.v1, color);
}

void UiBatch::AddFrame(float x, float y, float w, float h, float t, std::uint32_t color) {
    AddRect(x, y, w, t, color);
    AddRect(x, y + h - t, w, t, color);
    AddRect(x, y + t, t, h - 2 * t, color);
    AddRect(x + w - t, y + t, t, h - 2 * t, color);
}

void UiBatch::AddText(std::string_view text, float x, float y, float scale, std::uint32_t color) {
    float pen = x;
    for (const char c : text) {
        if (c != ' ') {
            const UvRect uv = GlyphUv(GlyphIndex(c));
            Quad(pen, y, kAdvance * scale, kCell * scale, uv.u0, uv.v0, uv.u1, uv.v1, color);
        }
        pen += kAdvance * scale;
    }
}

float UiBatch::TextWidth(std::string_view text, float scale) {
    // The last glyph's blank column is not counted, so centred text is optically centred.
    return text.empty() ? 0.0f : (static_cast<float>(text.size()) * kAdvance - 1) * scale;
}

float UiBatch::TextHeight(float scale) {
    return 7.0f * scale;
}

} // namespace ember::ui

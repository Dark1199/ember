// SPDX-License-Identifier: GPL-3.0-or-later
#include <cassert>
#include <cstdio>
#include <string>

#include "ui/font.h"
#include "ui/menu.h"
#include "ui/ui_batch.h"

using namespace ember::ui;

static bool Lit(const FontAtlas& a, int cell, int x, int y) {
    const int ox = (cell % kAtlasColumns) * kCell, oy = (cell / kAtlasColumns) * kCell;
    return a.rgba[(static_cast<std::size_t>(oy + y) * a.width + ox + x) * 4 + 3] != 0;
}

int main() {
    const FontAtlas& a = Font();
    assert(a.width == 128 && a.height % 8 == 0 && a.rgba.size() == std::size_t(a.width) * a.height * 4);
    // 'A': top row ".###.", bar row "#####", nothing in the spacing column or row.
    const int A = GlyphIndex('A');
    assert(!Lit(a, A, 0, 0) && Lit(a, A, 1, 0) && Lit(a, A, 3, 0) && !Lit(a, A, 4, 0));
    assert(Lit(a, A, 0, 3) && Lit(a, A, 4, 3) && !Lit(a, A, 5, 3) && !Lit(a, A, 0, 7));
    assert(GlyphIndex('a') == A && GlyphIndex('~') == GlyphIndex('?') && GlyphIndex(' ') == 0);
    // every glyph has at least one lit pixel except space; solid cell is fully lit
    for (int g = 1; g < a.glyph_count; ++g) {
        int lit = 0;
        for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x) lit += Lit(a, g, x, y);
        assert(lit >= 3);
    }
    for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x) assert(Lit(a, a.solid_index, x, y));
    const UvRect s = SolidUv();
    assert(s.u0 >= 0 && s.u1 <= 1 && s.v0 >= 0 && s.v1 <= 1 && s.u1 > s.u0);
    for (int g = 0; g <= a.glyph_count; ++g) {
        const UvRect uv = GlyphUv(g);
        assert(uv.u0 >= 0 && uv.u1 <= 1 && uv.v0 >= 0 && uv.v1 <= 1);
    }

    UiBatch batch;
    batch.AddRect(10, 10, 100, 50, Rgba(255, 0, 0));
    assert(batch.Vertices().size() == 6);
    batch.AddText("A B", 0, 0, 2.0f, Rgba(255, 255, 255)); // space draws nothing
    assert(batch.Vertices().size() == 6 + 12);
    assert(UiBatch::TextWidth("AB", 2.0f) == (2 * 6 - 1) * 2.0f && UiBatch::TextWidth("", 2.0f) == 0.0f);
    batch.AddFrame(0, 0, 50, 50, 3, 0xFFFFFFFFu);
    assert(batch.Vertices().size() == 18 + 24);
    batch.AddRect(0, 0, -5, 10, 0); // degenerate: ignored
    assert(batch.Vertices().size() == 42);
    for (int i = 0; i < 5000; ++i) batch.AddRect(0, 0, 1, 1, 0);
    assert(batch.Vertices().size() <= UiBatch::kMaxQuads * 6);
    batch.Clear();
    assert(batch.Vertices().empty());

    // Menu flow on a wide and a narrow screen.
    for (const float aspect : {2.15f, 16.0f / 9.0f, 0.5f}) {
        Menu menu;
        MenuInfo info;
        info.model_ready = true;
        assert(menu.CurrentScreen() == Screen::Title && !menu.OnBack());
        menu.OnTap(0.5f, 0.5f, aspect, info); // any tap leaves the title
        assert(menu.CurrentScreen() == Screen::Menu);
        const auto buttons = menu.Buttons(aspect, info);
        assert(buttons.size() == 4);
        assert(buttons[0].rect.w <= 240.0f && buttons[0].rect.h <= 44.0f); // compact targets
        const float width = kLayoutHeight * aspect;
        for (size_t i = 0; i < buttons.size(); ++i) {
            const Rect& r = buttons[i].rect;
            assert(r.x >= 0 && r.x + r.w <= width && r.y + r.h <= kLayoutHeight);
            if (i) assert(r.y >= buttons[i - 1].rect.y + buttons[i - 1].rect.h); // no overlap
        }
        const auto centre = [&](const Rect& r, float& nx, float& ny) {
            nx = (r.x + r.w / 2) / width; ny = (r.y + r.h / 2) / kLayoutHeight;
        };
        float nx, ny;
        centre(buttons[2].rect, nx, ny); // STATUS
        menu.OnTap(nx, ny, aspect, info);
        assert(menu.CurrentScreen() == Screen::Info);
        centre(menu.Buttons(aspect, info)[0].rect, nx, ny); // BACK
        menu.OnTap(nx, ny, aspect, info);
        assert(menu.CurrentScreen() == Screen::Menu);
        centre(buttons[0].rect, nx, ny); // VIEW MODEL
        menu.OnTap(nx, ny, aspect, info);
        assert(menu.CurrentScreen() == Screen::Viewer && menu.OnBack() && menu.CurrentScreen() == Screen::Menu);
        MenuInfo none; // no model: the view button does nothing
        centre(buttons[0].rect, nx, ny);
        menu.OnTap(nx, ny, aspect, none);
        assert(menu.CurrentScreen() == Screen::Menu);
        menu.OnTap(0.01f, 0.99f, aspect, info); // empty corner
        assert(menu.CurrentScreen() == Screen::Menu && !menu.WantsQuit());
        centre(buttons[1].rect, nx, ny); // NEW GAME
        assert(menu.OnTap(nx, ny, aspect, info) == Menu::kStartPlay && menu.CurrentScreen() == Screen::Play);
        assert(menu.Buttons(aspect, info).size() == 1 && menu.OnBack() && menu.CurrentScreen() == Screen::Menu);
        centre(buttons[1].rect, nx, ny);
        menu.OnTap(nx, ny, aspect, none); // no model: stays in the menu
        assert(menu.CurrentScreen() == Screen::Menu);
        centre(buttons[3].rect, nx, ny);
        assert(menu.OnTap(nx, ny, aspect, info) == Menu::kQuit && menu.WantsQuit());
        // drawing every screen stays inside the batch limit and produces geometry
        for (const Screen sc : {Screen::Title, Screen::Menu, Screen::Info, Screen::Viewer, Screen::Play}) {
            Menu m2;
            if (sc != Screen::Title) m2.OnTap(0.5f, 0.5f, aspect, info);
            if (sc == Screen::Info) { auto b = m2.Buttons(aspect, info); centre(b[2].rect, nx, ny); m2.OnTap(nx, ny, aspect, info); }
            if (sc == Screen::Play) { auto b = m2.Buttons(aspect, info); centre(b[1].rect, nx, ny); m2.OnTap(nx, ny, aspect, info); }
            if (sc == Screen::Viewer) { auto b = m2.Buttons(aspect, info); centre(b[0].rect, nx, ny); m2.OnTap(nx, ny, aspect, info); }
            assert(m2.CurrentScreen() == sc);
            info.lines = {"GPU: ADRENO 612", "RESOLUTION SCALE: 50%"};
            UiBatch b;
            m2.Draw(b, aspect, info);
            assert(!b.Vertices().empty() && b.Vertices().size() % 6 == 0);
        }
    }
    { // EnterPlay skips the title and menu, but never leaves another screen
        Menu m;
        m.EnterPlay();
        assert(m.CurrentScreen() == Screen::Play);
        assert(m.OnBack() && m.CurrentScreen() == Screen::Menu);
        Menu v;
        MenuInfo info; info.model_ready = true;
        v.OnTap(0.5f, 0.5f, 16.0f / 9.0f, info); // title -> menu
        v.EnterPlay();
        assert(v.CurrentScreen() == Screen::Play);
    }
    std::puts("ui_test: ok");
    return 0;
}

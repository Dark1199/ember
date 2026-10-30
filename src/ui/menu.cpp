// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/menu.h"

#include "ui/font.h"

#include <algorithm>
#include <cmath>

namespace ember::ui {
namespace {

enum ButtonId { kView = 1, kInfo = 2, kQuitButton = 3, kBack = 4, kNewGame = 5 };

// Sizes are in layout units (720 tall). Buttons are about half the first version's size; the
// text scale stays a whole number of layout units per font pixel so pixels remain even.
constexpr float kButtonW = 230.0f, kButtonH = 40.0f, kButtonGap = 10.0f, kTextScale = 3.0f;
constexpr float kBackW = 110.0f, kBackH = 34.0f;

constexpr std::uint32_t kInk = Rgba(238, 230, 223);
constexpr std::uint32_t kDim = Rgba(160, 148, 138);
constexpr std::uint32_t kAccent = Rgba(232, 101, 10);
constexpr std::uint32_t kPanel = Rgba(20, 16, 14, 200);

} // namespace

std::vector<Menu::Button> Menu::Buttons(float aspect, const MenuInfo& info) const {
    const float width = kLayoutHeight * aspect;
    // Buttons are 420 x 76 units, but never wider than 80% of a narrow screen.
    const float bw = std::min(kButtonW, width * 0.8f), bh = kButtonH, gap = kButtonGap;
    const float x = (width - bw) / 2;
    std::vector<Button> out;
    if (screen_ == Screen::Menu) {
        float y = kLayoutHeight * 0.36f;
        for (const auto& [id, label] : {std::pair{static_cast<int>(kView), info.model_ready ? "VIEW MODEL" : "NO MODEL YET"},
                                        std::pair{static_cast<int>(kNewGame), "NEW GAME"},
                                        std::pair{static_cast<int>(kInfo), "STATUS"},
                                        std::pair{static_cast<int>(kQuitButton), "QUIT"}}) {
            out.push_back({{x, y, bw, bh}, label, id});
            y += bh + gap;
        }
    } else if (screen_ == Screen::Info || screen_ == Screen::Viewer || screen_ == Screen::Play) {
        out.push_back({{20.0f, 20.0f, kBackW, kBackH}, screen_ == Screen::Play ? "MENU" : "BACK", kBack});
    }
    return out;
}

Menu::Action Menu::OnTap(float nx, float ny, float aspect, const MenuInfo& info) {
    const float x = nx * kLayoutHeight * aspect, y = ny * kLayoutHeight;
    if (screen_ == Screen::Title) {
        screen_ = Screen::Menu;
        return kNone;
    }
    for (const Button& b : Buttons(aspect, info)) {
        if (!b.rect.Contains(x, y)) continue;
        switch (b.id) {
        case kView:
            if (info.model_ready) screen_ = Screen::Viewer;
            break;
        case kNewGame:
            if (info.model_ready) {
                screen_ = Screen::Play;
                return kStartPlay;
            }
            break;
        case kInfo:
            screen_ = Screen::Info;
            break;
        case kQuitButton:
            quit_ = true;
            return kQuit;
        case kBack:
            screen_ = Screen::Menu;
            break;
        }
        return kNone;
    }
    return kNone;
}

bool Menu::OnBack() {
    switch (screen_) {
    case Screen::Title:
        return false;
    case Screen::Menu:
        screen_ = Screen::Title;
        return true;
    default:
        screen_ = Screen::Menu;
        return true;
    }
}

void Menu::Draw(UiBatch& batch, float aspect, const MenuInfo& info) const {
    const float width = kLayoutHeight * aspect;
    const auto centered = [&](const std::string& text, float y, float scale, std::uint32_t color) {
        batch.AddText(text, (width - UiBatch::TextWidth(text, scale)) / 2, y, scale, color);
    };
    switch (screen_) {
    case Screen::Title: {
        // Title scale is chosen so the word fits 70% of the width on narrow screens.
        const float scale = std::min(22.0f, width * 0.7f / ((5 * kAdvance - 1)));
        centered("EMBER", kLayoutHeight * 0.28f, scale, kAccent);
        centered("AN OPEN SOURCE ENGINE FOR YOUR OWN GAME FILES", kLayoutHeight * 0.28f + 9 * scale, 3.0f, kDim);
        const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(time_) * 3.0f);
        centered(info.loading ? "READING GAME FILES..." : "TAP TO START", kLayoutHeight * 0.78f, 5.0f,
                 Rgba(238, 230, 223, static_cast<int>(110 + 145 * pulse)));
        break;
    }
    case Screen::Menu:
        batch.AddRect(0, 0, width, kLayoutHeight, Rgba(10, 8, 7, 170));
        centered("EMBER", kLayoutHeight * 0.10f, 10.0f, kAccent);
        for (const Button& b : Buttons(aspect, info)) {
            const bool disabled = (b.id == kView || b.id == kNewGame) && !info.model_ready;
            batch.AddRect(b.rect.x, b.rect.y, b.rect.w, b.rect.h, kPanel);
            batch.AddFrame(b.rect.x, b.rect.y, b.rect.w, b.rect.h, 2.0f, disabled ? kDim : kAccent);
            const float scale = std::min(kTextScale, std::floor((b.rect.w - 16) / UiBatch::TextWidth(b.label, 1.0f)));
            batch.AddText(b.label, b.rect.x + (b.rect.w - UiBatch::TextWidth(b.label, scale)) / 2,
                          b.rect.y + (b.rect.h - UiBatch::TextHeight(scale)) / 2, scale, disabled ? kDim : kInk);
        }
        break;
    case Screen::Info: {
        batch.AddRect(0, 0, width, kLayoutHeight, Rgba(10, 8, 7, 215));
        float y = 130.0f;
        for (const std::string& line : info.lines) {
            const float scale = std::min(4.0f, (width - 80.0f) / std::max(UiBatch::TextWidth(line, 1.0f), 1.0f));
            batch.AddText(line, 40.0f, y, scale, kInk);
            y += 10 * scale + 10.0f;
        }
        for (const Button& b : Buttons(aspect, info)) {
            batch.AddRect(b.rect.x, b.rect.y, b.rect.w, b.rect.h, kPanel);
            batch.AddFrame(b.rect.x, b.rect.y, b.rect.w, b.rect.h, 2.0f, kAccent);
            batch.AddText(b.label, b.rect.x + (b.rect.w - UiBatch::TextWidth(b.label, kTextScale)) / 2,
                          b.rect.y + (b.rect.h - UiBatch::TextHeight(kTextScale)) / 2, kTextScale, kInk);
        }
        break;
    }
    case Screen::Viewer:
    case Screen::Play:
        for (const Button& b : Buttons(aspect, info)) {
            batch.AddRect(b.rect.x, b.rect.y, b.rect.w, b.rect.h, kPanel);
            batch.AddFrame(b.rect.x, b.rect.y, b.rect.w, b.rect.h, 2.0f, kAccent);
            batch.AddText(b.label, b.rect.x + (b.rect.w - UiBatch::TextWidth(b.label, kTextScale)) / 2,
                          b.rect.y + (b.rect.h - UiBatch::TextHeight(kTextScale)) / 2, kTextScale, kInk);
        }
        break;
    }
}

} // namespace ember::ui

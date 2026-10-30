// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <vector>

#include "ui/ui_batch.h"

namespace ember::ui {

/// Layout space: the screen as the user holds it, always kLayoutHeight units tall and
/// kLayoutHeight * aspect units wide, origin top left. Touch positions arrive normalised (0..1 of
/// the screen), so layout never depends on the pixel size, the resolution scale or the display
/// rotation; the renderer maps layout units to the framebuffer.
inline constexpr float kLayoutHeight = 720.0f;

enum class Screen { Title, Menu, Info, Viewer, Play };

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    [[nodiscard]] bool Contains(float px, float py) const {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
};

/// What the menu shows about the running engine.
struct MenuInfo {
    bool game_found = false;
    bool loading = false;
    bool model_ready = false;
    std::vector<std::string> lines; ///< shown on the Info screen
};

/// Title -> Menu -> (Viewer | Info), plus Quit. Pure logic (no Vulkan), so it is tested on the
/// host. Draw() only fills a UiBatch.
class Menu {
public:
    enum Action { kNone = 0, kQuit = 1, kStartPlay = 2 };

    [[nodiscard]] Screen CurrentScreen() const noexcept { return screen_; }
    [[nodiscard]] bool WantsQuit() const noexcept { return quit_; }

    void Update(double dt_seconds) { time_ += dt_seconds; }

    /// A finger went down at (nx, ny) in 0..1 screen coordinates. Returns what happened.
    Action OnTap(float nx, float ny, float aspect, const MenuInfo& info);

    /// The Android back button / gesture. Returns true when it was used (not at the title).
    bool OnBack();

    void Draw(UiBatch& batch, float aspect, const MenuInfo& info) const;

    /// Button rectangles of the current screen, in layout units (for tests and hit testing).
    struct Button {
        Rect rect;
        std::string label;
        int id;
    };
    [[nodiscard]] std::vector<Button> Buttons(float aspect, const MenuInfo& info) const;

private:
    Screen screen_ = Screen::Title;
    bool quit_ = false;
    double time_ = 0.0;
};

} // namespace ember::ui

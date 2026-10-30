// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string_view>

namespace ember::app {

/// User settings, written by the launcher's settings screen to `settings.txt` in the app's files
/// folder as `key=value` lines (see SettingsStore.java). Unknown keys and bad values are ignored,
/// so a damaged or older file can never stop the engine from starting.
struct Settings {
    int resolution_scale_percent = 100; ///< 25..100: share of the screen's pixels per side
    int frame_cap = 0;                  ///< frames per second, 0 = display refresh rate
    bool shadows = true;                ///< shadow pass on/off (no shadow pass exists yet)
};

inline constexpr int kMinResolutionScale = 25;
inline constexpr int kMaxResolutionScale = 100;
inline constexpr int kMaxFrameCap = 240;

/// Parses the file's text. Values are clamped into range; anything unreadable keeps the default.
[[nodiscard]] Settings ParseSettings(std::string_view text);

/// Size to render at for a screen of `width` x `height` pixels: each side scaled, at least 64.
struct ScaledSize {
    int width;
    int height;
};
[[nodiscard]] ScaledSize ScaleResolution(int width, int height, int percent);

} // namespace ember::app

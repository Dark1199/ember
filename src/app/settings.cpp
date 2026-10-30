// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/settings.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <string>

namespace ember::app {
namespace {

std::string_view Trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

bool ParseInt(std::string_view s, int& out) {
    int value = 0;
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    if (ec != std::errc() || end != s.data() + s.size()) return false;
    out = value;
    return true;
}

} // namespace

Settings ParseSettings(std::string_view text) {
    Settings settings;
    while (!text.empty()) {
        const auto newline = text.find('\n');
        std::string_view line = Trim(text.substr(0, newline));
        text = newline == std::string_view::npos ? std::string_view{} : text.substr(newline + 1);
        const auto eq = line.find('=');
        if (line.empty() || line.front() == '#' || eq == std::string_view::npos) continue;
        const std::string_view key = Trim(line.substr(0, eq));
        const std::string_view value = Trim(line.substr(eq + 1));
        int number = 0;
        if (key == "resolution_scale") {
            if (ParseInt(value, number)) {
                settings.resolution_scale_percent = std::clamp(number, kMinResolutionScale, kMaxResolutionScale);
            }
        } else if (key == "frame_cap") {
            if (ParseInt(value, number)) settings.frame_cap = std::clamp(number, 0, kMaxFrameCap);
        } else if (key == "shadows") {
            if (value == "0" || value == "off" || value == "false") settings.shadows = false;
            else if (value == "1" || value == "on" || value == "true") settings.shadows = true;
        }
    }
    return settings;
}

ScaledSize ScaleResolution(int width, int height, int percent) {
    percent = std::clamp(percent, kMinResolutionScale, kMaxResolutionScale);
    const auto scale = [percent](int v) { return std::max(64, v * percent / 100); };
    return {std::min(scale(width), std::max(width, 1)), std::min(scale(height), std::max(height, 1))};
}

} // namespace ember::app

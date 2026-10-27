// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/engine.h"

#include <cmath>
#include <fstream>

#include "core/log.h"

namespace ember::app {

std::filesystem::path LocateGameRoot(const std::filesystem::path& external_files_dir) {
    std::ifstream override_file(external_files_dir / "game_root.txt");
    std::string line;
    if (override_file && std::getline(override_file, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        if (!line.empty()) {
            return line;
        }
    }
    return external_files_dir / "game";
}

bool Engine::Startup(const Config& config) {
    EMBER_LOG("Game root: %s", config.game_root.string().c_str());
    std::string error;
    if (!vfs_.MountDirectory(config.game_root, &error)) {
        EMBER_WARN("No game files mounted: %s", error.c_str());
        EMBER_WARN("Copy your own installation into that folder, or put its path in game_root.txt");
        return false;
    }
    game_found_ = true;
    EMBER_LOG("Mounted %zu files", vfs_.FileCount());
    for (const std::string& name : vfs_.TopLevel()) {
        EMBER_LOG("  top level: %s", name.c_str());
    }
    return true;
}

bool Engine::Frame(gfx::Renderer& renderer, double dt_seconds) {
    time_ += dt_seconds;
    // Placeholder output: slow dark blue breathing when game files are mounted, dark red when not,
    // so a glance at the screen tells the user whether the game folder was found.
    const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(time_) * 1.5f);
    if (game_found_) {
        renderer.SetClearColor(0.02f, 0.03f + 0.05f * pulse, 0.10f + 0.10f * pulse);
    } else {
        renderer.SetClearColor(0.10f + 0.10f * pulse, 0.02f, 0.02f);
    }
    return renderer.DrawFrame({}); // no draw commands yet
}

} // namespace ember::app

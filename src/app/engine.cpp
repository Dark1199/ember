// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/engine.h"

#include <chrono>
#include <cmath>
#include <fstream>
#include <vector>

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
    struct DoneGuard {
        std::atomic<bool>& flag;
        ~DoneGuard() {
            flag.store(false);
        }
    } done{loading_};
    EMBER_LOG("Game root: %s", config.game_root.string().c_str());
    const auto started = std::chrono::steady_clock::now();
    std::string error;
    if (!vfs_.MountDirectory(config.game_root, &error)) {
        EMBER_WARN("No game files mounted: %s", error.c_str());
        EMBER_WARN("Copy your own installation into that folder, or put its path in game_root.txt");
        return false;
    }
    EMBER_LOG("Mounted %zu files", vfs_.FileCount());
    for (const std::string& name : vfs_.TopLevel()) {
        EMBER_LOG("  top level: %s", name.c_str());
    }

    std::vector<std::string> archive_errors;
    const std::size_t archives = vfs_.MountArchivesFromIndex(&archive_errors);
    EMBER_LOG("Mounted %zu archive(s), %zu packed files", archives, vfs_.PackedFileCount());
    for (const std::string& problem : archive_errors) {
        // Includes the first bytes of the index so a wrong format assumption can be diagnosed.
        EMBER_WARN("Archive problem: %s", problem.c_str());
    }
    game_found_.store(true);
    EMBER_LOG("Startup finished in %.2f s",
              std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
    return true;
}

bool Engine::Frame(gfx::Renderer& renderer, double dt_seconds) {
    time_ += dt_seconds;
    // Placeholder output so a glance at the screen tells the user what state the app is in:
    // grey = still scanning the game folder, blue = game folder mounted, red = not found.
    const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(time_) * 1.5f);
    if (loading_.load()) {
        renderer.SetClearColor(0.04f + 0.03f * pulse, 0.04f + 0.03f * pulse, 0.04f + 0.03f * pulse);
    } else if (game_found_.load()) {
        renderer.SetClearColor(0.02f, 0.03f + 0.05f * pulse, 0.10f + 0.10f * pulse);
    } else {
        renderer.SetClearColor(0.10f + 0.10f * pulse, 0.02f, 0.02f);
    }
    return renderer.DrawFrame({}); // no draw commands yet
}

} // namespace ember::app

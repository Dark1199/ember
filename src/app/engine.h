// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <filesystem>
#include <string>

#include "gfx/renderer.h"
#include "vfs/vfs.h"

namespace ember::app {

struct Config {
    /// Directory the user copied their own game installation into.
    std::filesystem::path game_root;
    /// Run the slow full integrity check of every archive entry after mounting.
    bool verify_archives = false;
    /// Where the verification result is written (empty: not written).
    std::filesystem::path verify_report;
};

/// Picks the game directory. `external_files_dir` is the app's private external storage
/// (on Android: /storage/emulated/0/Android/data/<package>/files).
///   1. If `<external_files_dir>/game_root.txt` exists, its first line is the path to use.
///   2. Otherwise `<external_files_dir>/game`.
std::filesystem::path LocateGameRoot(const std::filesystem::path& external_files_dir);

/// Platform independent engine state. Startup order:
///   LocateGameRoot -> Engine::Startup (mount game files) -> renderer attached by the platform
///   layer -> Engine::Frame once per display frame.
class Engine {
public:
    /// Mounts the game directory and logs what was found so a wrong path is obvious from the log.
    /// Returns false if the directory is missing; the engine still runs (renders only the
    /// placeholder) so the first-run experience on a device is a diagnosable window, not a crash.
    bool Startup(const Config& config);

    /// Advances the engine and draws one frame.
    bool Frame(gfx::Renderer& renderer, double dt_seconds);

    [[nodiscard]] const vfs::Vfs& Files() const noexcept {
        return vfs_;
    }
    [[nodiscard]] bool GameFound() const noexcept {
        return game_found_.load();
    }
    /// True until Startup has finished. Startup may run on a worker thread while Frame runs on
    /// the render thread; Files() must only be used once this is false.
    [[nodiscard]] bool Loading() const noexcept {
        return loading_.load();
    }

private:
    vfs::Vfs vfs_;
    std::atomic<bool> game_found_{false};
    std::atomic<bool> loading_{true};
    double time_ = 0.0;
};

} // namespace ember::app

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>

#include "app/model_loader.h"
#include "gfx/mesh_renderer.h"
#include "app/settings.h"
#include "gfx/camera.h"
#include "gfx/renderer.h"
#include "gfx/ui_renderer.h"
#include "ui/menu.h"
#include "vfs/vfs.h"

namespace ember::app {

struct Config {
    /// Directory the user copied their own game installation into.
    std::filesystem::path game_root;
    /// Run the slow full integrity check of every archive entry after mounting.
    bool verify_archives = false;
    /// Where the verification result is written (empty: not written).
    std::filesystem::path verify_report;
    Settings settings;
    /// Load a whole map block from its MSB and start in the free camera inside it. When no map
    /// can be loaded the engine falls back to showing the first clean model it finds.
    bool boot_to_world = true;
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

    /// Frees GPU resources; call from the render thread before the renderer shuts down.
    void ReleaseGpu(gfx::Renderer& renderer);

    /// A finger went down at (nx, ny), 0..1 of the screen as the user holds it. Render thread.
    void OnTap(float nx, float ny);
    /// Multi-touch input. `action`: 0 down, 1 move, 2 up/cancel. Coordinates as for OnTap.
    void OnPointer(int id, int action, float nx, float ny);
    /// The back button. Returns true when the engine used it (otherwise the app may close).
    bool OnBack();
    [[nodiscard]] bool WantsQuit() const noexcept { return menu_.WantsQuit(); }

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

    // First-mesh path: the worker thread finds and decodes a model, the render thread uploads it.
    std::mutex model_mutex_;
    std::optional<LoadedModel> pending_model_;
    std::vector<LoadedTexture> pending_textures_;
    bool pending_world_ = false;
    float pending_world_start_[3] = {};
    std::atomic<bool> model_ready_{false};
    bool model_uploaded_ = false;
    bool pipeline_failed_ = false;
    gfx::MeshRenderer mesh_;
    gfx::UiRenderer ui_renderer_;
    bool ui_failed_ = false;
    ui::Menu menu_;
    ui::UiBatch batch_;
    Settings settings_;
    std::string model_description_;
    std::size_t texture_count_ = 0;
    std::size_t archive_count_ = 0;
    std::size_t packed_count_ = 0;
    float aspect_ = 16.0f / 9.0f;

    // Play mode ("sandbox"): free-fly camera over the loaded model. Left half of the screen is a
    // movement stick, right half turns the view.
    struct Touch {
        int id = -1;
        float origin_x = 0, origin_y = 0, x = 0, y = 0;
    };
    gfx::FreeFly camera_;
    Touch move_touch_, look_touch_;
    bool was_playing_ = false;
    float model_radius_ = 1.0f;
    float model_centre_[3] = {};
    float shown_min_[3] = {};
    float shown_max_[3] = {};

    // World boot: set by the worker thread together with the pending model.
    bool boot_to_world_ = false;
    bool world_mode_ = false;      ///< the uploaded mesh is a map block, not a single model
    bool auto_entered_ = false;    ///< the boot already switched to play once
    float world_start_[3] = {};
    std::string world_summary_;
    float cam_near_ = 0.1f, cam_far_ = 100.0f, cam_speed_ = 1.0f;
};

} // namespace ember::app

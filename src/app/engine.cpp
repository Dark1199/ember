// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <vector>

#include "core/log.h"
#include "gfx/camera.h"
#include "vfs/survey.h"

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
    // Boot is done here: mounting only reads the small index files (about 10 ms for the four
    // archives). Entry tables of bundles are read when a bundle is opened, so nothing else has to
    // be analysed up front, and no cache is needed.
    game_found_.store(true);
    if (!config.verify_archives) {
        loading_.store(false);
    }
    EMBER_LOG("Startup finished in %.3f s",
              std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());

    if (config.verify_archives) {
        // Opt-in and slow (reads all game data). Everything below only reads the mounted
        // archives, so the render thread keeps running; the screen stays in its "busy" state
        // until the check has finished.
        EMBER_LOG("verify: structural survey and full integrity pass requested");
        std::string report;
        const auto emit = [&report](const std::string& line) {
            EMBER_LOG("%s", line.c_str());
            report += line + "\n";
        };
        for (const std::string& line : vfs::SurveyArchives(vfs_.Archives())) {
            emit(line);
        }
        vfs::VerifyArchives(vfs_.Archives(), emit);
        if (!config.verify_report.empty()) {
            // Its own file: ember.log is replaced on every launch, this one is kept until the
            // next verification so the launcher can show the last result.
            std::ofstream(config.verify_report, std::ios::trunc) << report;
        }
        EMBER_LOG("verify: finished in %.1f s",
                  std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
    }

    // First mesh: find the first model that decodes cleanly and hand it to the render thread. The
    // summary line says how many files were examined and why candidates were rejected, so if
    // nothing is shown the log still says what the real data looked like.
    std::string summary;
    auto model = FindFirstModel(vfs_.Archives(), 8.0, &summary);
    EMBER_LOG("model search: %s", summary.c_str());
    if (model) {
        EMBER_LOG("model: %s", model->description.c_str());
        {
            std::lock_guard<std::mutex> lock(model_mutex_);
            pending_model_ = std::move(model);
        }
        model_ready_.store(true);
    } else {
        EMBER_WARN("model: none passed the checks; showing the placeholder screen");
    }
    return true;
}

void Engine::ReleaseGpu(gfx::Renderer& renderer) {
    mesh_.Release(renderer);
}

bool Engine::Frame(gfx::Renderer& renderer, double dt_seconds) {
    time_ += dt_seconds;
    // Placeholder output so a glance at the screen tells the user what state the app is in:
    // grey = still scanning the game folder, blue = game folder mounted, red = not found.
    const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(time_) * 1.5f);
    if (loading_.load()) {
        renderer.SetClearColor(0.04f + 0.03f * pulse, 0.04f + 0.03f * pulse, 0.04f + 0.03f * pulse);
    } else if (game_found_.load()) {
        renderer.SetClearColor(0.02f, 0.04f + 0.06f * pulse, 0.16f + 0.14f * pulse);
    } else {
        renderer.SetClearColor(0.10f + 0.10f * pulse, 0.02f, 0.02f);
    }
    // Plain rectangles (attachment clears, no pipeline needed): a bar sweeping back and forth so
    // a live engine never looks frozen, and one tile per mounted archive once loading is done.
    if (model_ready_.load() && !model_uploaded_ && !pipeline_failed_ &&
        renderer.Device() != VK_NULL_HANDLE) {
        std::optional<LoadedModel> model;
        {
            std::lock_guard<std::mutex> lock(model_mutex_);
            model = std::move(pending_model_);
            pending_model_.reset();
        }
        if (model) {
            std::string error;
            if (!mesh_.Initialize(renderer, &error) || !mesh_.Upload(renderer, model->positions, model->indices, &error)) {
                EMBER_ERROR("mesh upload failed: %s", error.c_str());
                pipeline_failed_ = true;
            } else {
                for (int i = 0; i < 3; ++i) {
                    shown_min_[i] = model->bounds_min[i];
                    shown_max_[i] = model->bounds_max[i];
                }
                model_uploaded_ = true;
                EMBER_LOG("mesh uploaded: %zu vertices, %zu indices", model->positions.size() / 3,
                          model->indices.size());
            }
        }
    }
    const bool show_mesh = model_uploaded_ && mesh_.Ready();
    if (show_mesh) {
        renderer.SetClearColor(0.05f, 0.05f, 0.07f);
    }
    const std::size_t archives = vfs_.ArchiveCount();
    const bool loaded = !loading_.load();
    const double t = time_;
    return renderer.DrawFrame([&](const gfx::FrameContext& ctx) {
        if (show_mesh) {
            const int turns = renderer.PreRotationQuarterTurns();
            const float w = static_cast<float>(ctx.extent.width), h = static_cast<float>(ctx.extent.height);
            // With a quarter-turn pre-rotation the image is stored sideways relative to the display.
            const float aspect = (turns & 1) ? h / w : w / h;
            const gfx::Mat4 mvp = gfx::OrbitMvp(shown_min_, shown_max_, static_cast<float>(t), aspect, turns);
            mesh_.Draw(ctx, mvp.data(), shown_min_[1], shown_max_[1]);
            return;
        }
        const auto rect = [&](std::int32_t x, std::int32_t y, std::uint32_t w, std::uint32_t h,
                              float r, float g, float b) {
            VkClearAttachment attachment{};
            attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            attachment.colorAttachment = 0;
            attachment.clearValue.color.float32[0] = r;
            attachment.clearValue.color.float32[1] = g;
            attachment.clearValue.color.float32[2] = b;
            attachment.clearValue.color.float32[3] = 1.0f;
            const VkClearRect area{{{x, y}, {w, h}}, 0, 1};
            vkCmdClearAttachments(ctx.cmd, 1, &attachment, 1, &area);
        };
        const std::uint32_t W = ctx.extent.width, H = ctx.extent.height;
        const std::uint32_t track_h = std::max<std::uint32_t>(H / 40, 4);
        const auto track_y = static_cast<std::int32_t>(H / 2);
        rect(static_cast<std::int32_t>(W / 8), track_y, W - W / 4, track_h, 0.12f, 0.10f, 0.09f);
        const double phase = std::fmod(t * 0.8, 2.0);
        const double pos = phase < 1.0 ? phase : 2.0 - phase; // ping-pong 0..1
        const std::uint32_t seg = W / 10;
        const auto seg_x = static_cast<std::int32_t>(W / 8 + pos * (W - W / 4 - seg));
        rect(seg_x, track_y, seg, track_h, 0.91f, 0.40f, 0.04f);
        if (loaded) {
            const std::uint32_t tile = std::max<std::uint32_t>(H / 14, 8);
            const std::uint32_t gap = tile / 2;
            for (std::size_t i = 0; i < std::max<std::size_t>(archives, 1); ++i) {
                const bool have = i < archives;
                rect(static_cast<std::int32_t>(W / 8 + i * (tile + gap)),
                     track_y + static_cast<std::int32_t>(track_h + tile), tile, tile,
                     have ? 0.30f : 0.45f, have ? 0.75f : 0.10f, have ? 0.35f : 0.10f);
            }
        }
    });
}

} // namespace ember::app

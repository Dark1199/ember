// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <vector>

#include "core/log.h"
#include "app/world_loader.h"
#include "formats/msb.h"
#include "gfx/camera.h"
#include "ui/font.h"
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
    settings_ = config.settings;
    boot_to_world_ = config.boot_to_world;
    // World probe: does the archive hold the files a world loader would start from? Paths follow
    // community documentation of the PC release; only existence is checked (nothing is parsed).
    // Reported after mounting below.
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
    archive_count_ = archives;
    packed_count_ = vfs_.PackedFileCount();
    for (const std::string& problem : archive_errors) {
        // Includes the first bytes of the index so a wrong format assumption can be diagnosed.
        EMBER_WARN("Archive problem: %s", problem.c_str());
    }
    // Boot is done here: mounting only reads the small index files (about 10 ms for the four
    // archives). Entry tables of bundles are read when a bundle is opened, so nothing else has to
    // be analysed up front, and no cache is needed.
    {
        const char* const candidates[] = {"/map/MapStudio/m10_00_00_00.msb", "/map/MapStudio/m10_01_00_00.msb",
                                          "/map/MapStudio/m12_00_00_00.msb", "/param/GameParam/GameParam.parambnd",
                                          "/param/GameParam/GameParam.parambnd.dcx", "/event/common.emevd",
                                          "/event/m10_00_00_00.emevd", "/event/m10_00_00_00.emevd.dcx"};
        std::string hits, misses;
        for (const char* path : candidates) {
            (vfs_.Exists(path) ? hits : misses) += std::string(path) + " ";
        }
        EMBER_LOG("world probe: present: %s| absent: %s", hits.empty() ? "(none) " : hits.c_str(),
                  misses.empty() ? "(none)" : misses.c_str());
        // Parse every map block that is present. One summary line each lets the layout notes in
        // docs/formats/msb.md be checked against the real files from the device log alone.
        for (const char* path : {"/map/MapStudio/m10_00_00_00.msb", "/map/MapStudio/m10_01_00_00.msb",
                                 "/map/MapStudio/m12_00_00_00.msb"}) {
            if (!vfs_.Exists(path)) continue;
            std::string msb_error;
            const auto bytes = vfs_.ReadAllDecoded(path, &msb_error);
            if (!bytes) {
                EMBER_WARN("msb: %s unreadable: %s", path, msb_error.c_str());
                continue;
            }
            if (const auto msb = formats::ParseMsb(*bytes, &msb_error)) {
                EMBER_LOG("msb: %s: %s", path, formats::SummarizeMsb(*msb).c_str());
            } else {
                EMBER_WARN("%s: %s", path, msb_error.c_str());
            }
        }
    }
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

    // World first: load a whole map block from its MSB. Every step reports into the log (counters,
    // the first unresolved model names with the paths tried) because the model file naming is not
    // verified yet. If no map block yields anything, fall back to the first clean single model.
    std::optional<LoadedModel> model;
    std::vector<std::string> wanted_textures;
    std::uint32_t texture_max_dimension = 1024;
    float world_start[3] = {};
    bool world_loaded = false;
    if (config.boot_to_world) {
        WorldOptions options;
        for (const char* path : {"/map/MapStudio/m10_01_00_00.msb", "/map/MapStudio/m10_00_00_00.msb",
                                 "/map/MapStudio/m12_00_00_00.msb"}) {
            if (!vfs_.Exists(path)) continue;
            std::string world_summary;
            auto world = LoadWorld(vfs_, path, options, &world_summary);
            EMBER_LOG("world: %s", world_summary.c_str());
            world_summary_ = world_summary;
            if (!world) continue;
            EMBER_LOG("world: %s; %zu vertices, %zu triangles, %zu texture keys, start (%.0f %.0f %.0f)",
                      world->model.description.c_str(), world->model.positions.size() / 3,
                      world->model.indices.size() / 3, world->hot_textures.size(), world->start[0],
                      world->start[1], world->start[2]);
            wanted_textures = world->hot_textures;
            if (wanted_textures.size() > 90) wanted_textures.resize(90); // renderer slot budget
            texture_max_dimension = 512;                                  // memory: ~1.4 MB each
            for (int i = 0; i < 3; ++i) world_start[i] = world->start[i];
            model = std::move(world->model);
            world_loaded = true;
            break;
        }
    }
    if (!model) {
        if (config.boot_to_world) EMBER_WARN("world: no map block loaded; falling back to a single model");
        // Find the first model that decodes cleanly. The summary line says how many files were
        // examined and why candidates were rejected, so if nothing is shown the log still says
        // what the real data looked like.
        std::string summary;
        model = FindFirstModel(vfs_.Archives(), 8.0, &summary);
        EMBER_LOG("model search: %s", summary.c_str());
        if (model) wanted_textures = model->material_texture_keys;
    }
    if (model) {
        EMBER_LOG("model: %s", model->description.c_str());
        model_description_ = model->description;
        std::string tex_summary;
        auto textures = FindTextures(vfs_.Archives(), wanted_textures, world_loaded ? 25.0 : 10.0,
                                     &tex_summary, texture_max_dimension);
        EMBER_LOG("textures: %s", tex_summary.c_str());
        {
            std::lock_guard<std::mutex> lock(model_mutex_);
            pending_model_ = std::move(model);
            pending_textures_ = std::move(textures);
            pending_world_ = world_loaded;
            for (int i = 0; i < 3; ++i) pending_world_start_[i] = world_start[i];
        }
        model_ready_.store(true);
    } else {
        EMBER_WARN("model: none passed the checks; showing the placeholder screen");
    }
    return true;
}

void Engine::ReleaseGpu(gfx::Renderer& renderer) {
    ui_renderer_.Release(renderer);
    mesh_.Release(renderer);
}

namespace {
ui::MenuInfo MakeInfo(bool found, bool loading, bool model_ready, const Settings& s,
                      std::size_t archives, std::size_t packed, std::size_t textures,
                      const std::string& model) {
    ui::MenuInfo info;
    info.game_found = found;
    info.loading = loading;
    info.model_ready = model_ready;
    info.lines = {std::string("GAME FILES: ") + (found ? "FOUND" : "NOT FOUND"),
                  "ARCHIVES: " + std::to_string(archives) + "  PACKED FILES: " + std::to_string(packed),
                  std::string("MODEL: ") + (model_ready ? "LOADED" : "NONE YET") +
                      "  TEXTURES: " + std::to_string(textures),
                  "RESOLUTION SCALE: " + std::to_string(s.resolution_scale_percent) + "%",
                  "FRAME CAP: " + (s.frame_cap ? std::to_string(s.frame_cap) + " FPS" : std::string("DISPLAY RATE")),
                  std::string("SHADOWS: ") + (s.shadows ? "ON" : "OFF") + " (NO SHADOW PASS YET)",
                  "CHANGE SETTINGS IN THE LAUNCHER"};
    return info;
}
} // namespace

void Engine::OnTap(float nx, float ny) {
    const ui::MenuInfo info = MakeInfo(game_found_.load(), loading_.load(), model_uploaded_, settings_,
                                       archive_count_, packed_count_, texture_count_, model_description_);
    menu_.OnTap(nx, ny, aspect_, info);
}

void Engine::OnPointer(int id, int action, float nx, float ny) {
    const bool playing = menu_.CurrentScreen() == ui::Screen::Play;
    if (action == 0) {
        if (playing) {
            // Buttons first; anything else becomes a stick or look touch.
            const ui::MenuInfo info = MakeInfo(game_found_.load(), loading_.load(), model_uploaded_, settings_,
                                               archive_count_, packed_count_, texture_count_, model_description_);
            const float lx = nx * ui::kLayoutHeight * aspect_, ly = ny * ui::kLayoutHeight;
            for (const auto& b : menu_.Buttons(aspect_, info)) {
                if (b.rect.Contains(lx, ly)) {
                    menu_.OnTap(nx, ny, aspect_, info);
                    return;
                }
            }
            Touch& t = nx < 0.5f ? move_touch_ : look_touch_;
            if (t.id < 0) t = {id, nx, ny, nx, ny};
        } else {
            OnTap(nx, ny);
        }
    } else if (action == 1) {
        if (look_touch_.id == id) {
            camera_.Look((nx - look_touch_.x) * aspect_ * 2.6f, -(ny - look_touch_.y) * 2.6f);
            look_touch_.x = nx;
            look_touch_.y = ny;
        } else if (move_touch_.id == id) {
            move_touch_.x = nx;
            move_touch_.y = ny;
        }
    } else {
        if (look_touch_.id == id) look_touch_ = {};
        if (move_touch_.id == id) move_touch_ = {};
    }
}

bool Engine::OnBack() {
    return menu_.OnBack();
}

bool Engine::Frame(gfx::Renderer& renderer, double dt_seconds) {
    time_ += dt_seconds;
    menu_.Update(dt_seconds);
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
        std::vector<LoadedTexture> textures;
        {
            std::lock_guard<std::mutex> lock(model_mutex_);
            model = std::move(pending_model_);
            textures = std::move(pending_textures_);
            pending_model_.reset();
            world_mode_ = pending_world_;
            for (int i = 0; i < 3; ++i) world_start_[i] = pending_world_start_[i];
        }
        if (model) {
            std::string error;
            std::vector<gfx::MeshRange> ranges;
            for (const auto& r : model->ranges) ranges.push_back({r.first_index, r.index_count, r.material_index});
            if (!mesh_.Initialize(renderer, &error) ||
                !mesh_.Upload(renderer, model->positions, model->uvs, model->indices, ranges, &error)) {
                EMBER_ERROR("mesh upload failed: %s", error.c_str());
                pipeline_failed_ = true;
            } else {
                for (int i = 0; i < 3; ++i) {
                    shown_min_[i] = model->bounds_min[i];
                    shown_max_[i] = model->bounds_max[i];
                }
                std::size_t bound = 0;
                for (const LoadedTexture& t : textures) {
                    std::string terr;
                    const int slot = mesh_.AddTexture(renderer, t.levels, &terr);
                    if (slot < 0) {
                        EMBER_WARN("texture %s upload failed: %s", t.key.c_str(), terr.c_str());
                        continue;
                    }
                    for (std::size_t m = 0; m < model->material_texture_keys.size(); ++m) {
                        if (model->material_texture_keys[m] == t.key) {
                            mesh_.BindMaterialTexture(static_cast<std::uint32_t>(m), slot);
                            ++bound;
                        }
                    }
                }
                EMBER_LOG("textures bound to %zu of %zu materials", bound, model->material_texture_keys.size());
                texture_count_ = textures.size();
                for (int i = 0; i < 3; ++i) model_centre_[i] = (model->bounds_min[i] + model->bounds_max[i]) * 0.5f;
                float r2 = 0.0f;
                for (int i = 0; i < 3; ++i) {
                    const float half = (model->bounds_max[i] - model->bounds_min[i]) * 0.5f;
                    r2 += half * half;
                }
                model_radius_ = std::max(std::sqrt(r2), 0.5f);
                if (world_mode_) {
                    // A map block is hundreds of metres across: a walking-pace-plus fly speed and a
                    // near plane that does not clip the floor, instead of values scaled to its size.
                    cam_near_ = 0.3f;
                    cam_far_ = 1500.0f;
                    cam_speed_ = 25.0f;
                } else {
                    cam_near_ = model_radius_ * 0.01f;
                    cam_far_ = model_radius_ * 12.0f;
                    cam_speed_ = model_radius_ * 0.6f;
                }
                model_uploaded_ = true;
                EMBER_LOG("mesh uploaded: %zu vertices, %zu indices", model->positions.size() / 3,
                          model->indices.size());
            }
        }
    }
    if (!ui_failed_ && !ui_renderer_.Ready() && renderer.Device() != VK_NULL_HANDLE) {
        std::string error;
        if (ui_renderer_.Initialize(renderer, &error)) {
            EMBER_LOG("UI renderer ready (font atlas %dx%d)", ui::Font().width, ui::Font().height);
        } else {
            EMBER_ERROR("UI renderer failed: %s", error.c_str());
            ui_failed_ = true;
        }
    }
    {
        const float w = static_cast<float>(renderer.Extent().width), h = static_cast<float>(renderer.Extent().height);
        if (w > 0 && h > 0) aspect_ = (renderer.PreRotationQuarterTurns() & 1) ? h / w : w / h;
    }
    const bool ui_on = ui_renderer_.Ready();
    const ui::MenuInfo menu_info = MakeInfo(game_found_.load(), loading_.load(), model_uploaded_, settings_,
                                            archive_count_, packed_count_, texture_count_, model_description_);
    if (boot_to_world_ && world_mode_ && model_uploaded_ && !auto_entered_) {
        auto_entered_ = true; // boot straight into the world; BACK still opens the menu
        menu_.EnterPlay();
    }
    const bool playing = menu_.CurrentScreen() == ui::Screen::Play && model_uploaded_;
    if (playing && !was_playing_ && world_mode_) {
        camera_.LookAtPoint({world_start_[0], world_start_[1] + 20.0f, world_start_[2]},
                            {world_start_[0], world_start_[1] + 10.0f, world_start_[2] - 40.0f});
        move_touch_ = look_touch_ = {};
    } else if (playing && !was_playing_) {
        // Entering play: start outside the model, looking at its centre.
        camera_.LookAtPoint({model_centre_[0], model_centre_[1] + model_radius_ * 0.4f,
                             model_centre_[2] + model_radius_ * 1.8f},
                            {model_centre_[0], model_centre_[1], model_centre_[2]});
        move_touch_ = look_touch_ = {};
    }
    was_playing_ = playing;
    if (playing && move_touch_.id >= 0) {
        const float dx = std::clamp((move_touch_.x - move_touch_.origin_x) * aspect_ / 0.12f, -1.0f, 1.0f);
        const float dy = std::clamp((move_touch_.y - move_touch_.origin_y) / 0.12f, -1.0f, 1.0f);
        camera_.Move(-dy, dx, cam_speed_, static_cast<float>(dt_seconds));
    }
    batch_.Clear();
    if (ui_on) {
        menu_.Draw(batch_, aspect_, menu_info);
        if (playing) {
            const float w = ui::kLayoutHeight * aspect_;
            const auto text = [&](const char* t, float y) {
                batch_.AddText(t, (w - ui::UiBatch::TextWidth(t, 2.0f)) / 2, y, 2.0f, ui::Rgba(160, 148, 138));
            };
            text(world_mode_ ? "WORLD VIEWER: FREE CAMERA OVER THE MAP (NO GAMEPLAY YET)"
                             : "SANDBOX: FREE CAMERA OVER THE LOADED MODEL (NOT GAMEPLAY)", 24.0f);
            text("LEFT HALF: MOVE   RIGHT HALF: LOOK", 44.0f);
            if (move_touch_.id >= 0) { // stick: base and knob
                const float bx = move_touch_.origin_x * w, by = move_touch_.origin_y * ui::kLayoutHeight;
                batch_.AddFrame(bx - 40, by - 40, 80, 80, 2.0f, ui::Rgba(232, 101, 10, 160));
                batch_.AddRect(move_touch_.x * w - 14, move_touch_.y * ui::kLayoutHeight - 14, 28, 28,
                               ui::Rgba(232, 101, 10, 200));
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
            const gfx::Mat4 mvp = playing ? camera_.Mvp(aspect, cam_near_, cam_far_, turns)
                                          : gfx::OrbitMvp(shown_min_, shown_max_, static_cast<float>(t), aspect, turns);
            mesh_.Draw(ctx, mvp.data(), shown_min_[1], shown_max_[1]);
        }
        if (ui_on) {
            ui_renderer_.Draw(ctx, batch_, ui::kLayoutHeight * aspect_, ui::kLayoutHeight,
                              renderer.PreRotationQuarterTurns());
        }
        if (show_mesh || ui_on) return;
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

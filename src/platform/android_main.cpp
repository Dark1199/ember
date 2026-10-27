// SPDX-License-Identifier: GPL-3.0-or-later
// NativeActivity entry point: owns the Android lifecycle and connects the window to the renderer.
#include <android/native_activity.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>

#include <chrono>
#include <vector>
#include <string>

#include "app/engine.h"
#include "core/log.h"
#include "gfx/renderer.h"

namespace {

struct AppState {
    ember::gfx::Renderer renderer;
    ember::app::Engine engine;
    bool instance_ready = false;
    bool running = false; // between APP_CMD_RESUME and APP_CMD_PAUSE
};

void AttachWindow(android_app* app, AppState& state) {
    if (!app->window || !state.instance_ready || state.renderer.HasSurface()) {
        return;
    }
    const VkAndroidSurfaceCreateInfoKHR info{
        .sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
        .window = app->window,
    };
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (vkCreateAndroidSurfaceKHR(state.renderer.Instance(), &info, nullptr, &surface) !=
        VK_SUCCESS) {
        EMBER_ERROR("vkCreateAndroidSurfaceKHR failed");
        return;
    }
    std::string error;
    if (!state.renderer.AttachSurface(surface, &error)) {
        EMBER_ERROR("AttachSurface: %s", error.c_str());
        state.renderer.DetachSurface();
    }
}

void OnCommand(android_app* app, int32_t cmd) {
    auto& state = *static_cast<AppState*>(app->userData);
    switch (cmd) {
    case APP_CMD_INIT_WINDOW:
        AttachWindow(app, state);
        break;
    case APP_CMD_TERM_WINDOW:
        state.renderer.DetachSurface();
        break;
    case APP_CMD_RESUME:
        state.running = true;
        break;
    case APP_CMD_PAUSE:
        state.running = false;
        break;
    default:
        break;
    }
}

} // namespace

void android_main(android_app* app) {
    AppState state;
    app->userData = &state;
    app->onAppCmd = OnCommand;

    std::string error;
    const std::vector<const char*> extensions = {"VK_KHR_surface", "VK_KHR_android_surface"};
    state.instance_ready = state.renderer.Initialize(extensions, &error);
    if (!state.instance_ready) {
        EMBER_ERROR("Vulkan instance: %s", error.c_str());
    }

    const char* external = app->activity->externalDataPath;
    const std::filesystem::path files_dir = external ? external : "";
    ember::app::Config config;
    config.game_root = ember::app::LocateGameRoot(files_dir);
    state.engine.Startup(config);

    auto last = std::chrono::steady_clock::now();
    while (!app->destroyRequested) {
        // Block while paused or without a window; poll when there is something to draw.
        const bool can_draw = state.running && state.renderer.HasSurface();
        android_poll_source* source = nullptr;
        int events = 0;
        while (ALooper_pollOnce(can_draw ? 0 : -1, nullptr, &events,
                                reinterpret_cast<void**>(&source)) >= 0) {
            if (source) {
                source->process(app, source);
            }
            if (app->destroyRequested) {
                break;
            }
        }
        if (app->destroyRequested) {
            break;
        }
        // The window may have arrived while the app was already resumed.
        AttachWindow(app, state);

        const auto now = std::chrono::steady_clock::now();
        const double dt = std::chrono::duration<double>(now - last).count();
        last = now;
        if (state.running && state.renderer.HasSurface()) {
            if (!state.engine.Frame(state.renderer, dt)) {
                EMBER_ERROR("Frame failed; stopping");
                ANativeActivity_finish(app->activity);
            }
        }
    }
    state.renderer.Shutdown();
}

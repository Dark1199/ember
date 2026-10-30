// SPDX-License-Identifier: GPL-3.0-or-later
// NativeActivity entry point: owns the Android lifecycle and connects the window to the renderer.
#include <android/input.h>
#include <android/keycodes.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <fstream>
#include <sstream>
#include <thread>

#include "app/engine.h"
#include "app/settings.h"
#include "core/crash.h"
#include "core/log.h"
#include "gfx/renderer.h"

namespace {

struct AppState {
    ember::gfx::Renderer renderer;
    ember::app::Engine engine;
    ember::app::Settings settings;
    int screen_w = 0, screen_h = 0; // window size before the resolution scale, for touch mapping
    std::thread startup;       // mounts the game files off the render/event thread
    bool instance_ready = false;
    bool running = false;      // between APP_CMD_RESUME and APP_CMD_PAUSE
    bool fallback_drawn = false;
    std::uint64_t frames = 0;
};

const char* CommandName(int32_t cmd) {
    switch (cmd) {
    case APP_CMD_INPUT_CHANGED: return "INPUT_CHANGED";
    case APP_CMD_INIT_WINDOW: return "INIT_WINDOW";
    case APP_CMD_TERM_WINDOW: return "TERM_WINDOW";
    case APP_CMD_WINDOW_RESIZED: return "WINDOW_RESIZED";
    case APP_CMD_WINDOW_REDRAW_NEEDED: return "WINDOW_REDRAW_NEEDED";
    case APP_CMD_CONTENT_RECT_CHANGED: return "CONTENT_RECT_CHANGED";
    case APP_CMD_GAINED_FOCUS: return "GAINED_FOCUS";
    case APP_CMD_LOST_FOCUS: return "LOST_FOCUS";
    case APP_CMD_CONFIG_CHANGED: return "CONFIG_CHANGED";
    case APP_CMD_LOW_MEMORY: return "LOW_MEMORY";
    case APP_CMD_START: return "START";
    case APP_CMD_RESUME: return "RESUME";
    case APP_CMD_SAVE_STATE: return "SAVE_STATE";
    case APP_CMD_PAUSE: return "PAUSE";
    case APP_CMD_STOP: return "STOP";
    case APP_CMD_DESTROY: return "DESTROY";
    default: return "?";
    }
}

/// Last-resort visible diagnostic: fills the window with one colour using the CPU, so that a
/// Vulkan failure shows up as magenta instead of an unexplained black screen. Only used when
/// the Vulkan path has failed (a window can be driven by only one API at a time).
void FillWindowSolid(ANativeWindow* window, std::uint32_t abgr) {
    if (!window) {
        return;
    }
    ANativeWindow_setBuffersGeometry(window, 0, 0, WINDOW_FORMAT_RGBA_8888);
    ANativeWindow_Buffer buffer{};
    if (ANativeWindow_lock(window, &buffer, nullptr) != 0) {
        EMBER_WARN("Fallback fill: ANativeWindow_lock failed");
        return;
    }
    auto* pixels = static_cast<std::uint32_t*>(buffer.bits);
    for (int y = 0; y < buffer.height; ++y) {
        std::uint32_t* row = pixels + static_cast<std::size_t>(y) * buffer.stride;
        for (int x = 0; x < buffer.width; ++x) {
            row[x] = abgr;
        }
    }
    ANativeWindow_unlockAndPost(window);
}

constexpr std::uint32_t kMagenta = 0xFFFF00FF; // Vulkan unavailable
constexpr std::uint32_t kOrange = 0xFF0080FF;  // Vulkan present but surface/swapchain failed

bool CanDraw(const AppState& state) {
    return state.running && state.renderer.HasSurface();
}

void AttachWindow(android_app* app, AppState& state) {
    if (!app->window || state.renderer.HasSurface() || state.fallback_drawn) {
        return;
    }
    if (!state.instance_ready) {
        EMBER_ERROR("No Vulkan instance; drawing magenta fallback");
        FillWindowSolid(app->window, kMagenta);
        state.fallback_drawn = true;
        return;
    }
    // Resolution scale: ask the window for a smaller buffer; the compositor scales it up to the
    // screen. Must happen before the swapchain exists, which then reports the smaller extent.
    state.screen_w = ANativeWindow_getWidth(app->window);
    state.screen_h = ANativeWindow_getHeight(app->window);
    if (state.settings.resolution_scale_percent < 100) {
        const int w = state.screen_w, h = state.screen_h;
        const auto size = ember::app::ScaleResolution(w, h, state.settings.resolution_scale_percent);
        const int rc = ANativeWindow_setBuffersGeometry(app->window, size.width, size.height, 0);
        EMBER_LOG("Resolution scale %d%%: window %dx%d -> buffers %dx%d (result %d)",
                  state.settings.resolution_scale_percent, w, h, size.width, size.height, rc);
    }
    const VkAndroidSurfaceCreateInfoKHR info{
        .sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
        .window = app->window,
    };
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    const VkResult created =
        vkCreateAndroidSurfaceKHR(state.renderer.Instance(), &info, nullptr, &surface);
    if (created != VK_SUCCESS) {
        EMBER_ERROR("vkCreateAndroidSurfaceKHR failed (%d); drawing orange fallback", created);
        FillWindowSolid(app->window, kOrange);
        state.fallback_drawn = true;
        return;
    }
    std::string error;
    if (!state.renderer.AttachSurface(surface, &error)) {
        EMBER_ERROR("AttachSurface failed: %s; drawing orange fallback", error.c_str());
        state.renderer.DetachSurface();
        FillWindowSolid(app->window, kOrange);
        state.fallback_drawn = true;
        return;
    }
    EMBER_LOG("Surface attached");
}

int32_t OnInput(android_app* app, AInputEvent* event) {
    auto& state = *static_cast<AppState*>(app->userData);
    if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_MOTION) {
        if ((AMotionEvent_getAction(event) & AMOTION_EVENT_ACTION_MASK) == AMOTION_EVENT_ACTION_DOWN &&
            state.screen_w > 0 && state.screen_h > 0) {
            state.engine.OnTap(AMotionEvent_getX(event, 0) / static_cast<float>(state.screen_w),
                               AMotionEvent_getY(event, 0) / static_cast<float>(state.screen_h));
            return 1;
        }
    } else if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_KEY &&
               AKeyEvent_getKeyCode(event) == AKEYCODE_BACK) {
        // Acted on at key-up; at the title screen the key is left to the system (closes the app).
        return AKeyEvent_getAction(event) == AKEY_EVENT_ACTION_UP && state.engine.OnBack() ? 1 : 0;
    }
    return 0;
}

void OnCommand(android_app* app, int32_t cmd) {
    auto& state = *static_cast<AppState*>(app->userData);
    EMBER_LOG("lifecycle: %s (%d)", CommandName(cmd), cmd);
    switch (cmd) {
    case APP_CMD_INIT_WINDOW:
        state.fallback_drawn = false;
        AttachWindow(app, state);
        break;
    case APP_CMD_TERM_WINDOW:
        state.renderer.DetachSurface();
        state.fallback_drawn = false;
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
    // Logging first: on a phone without adb the file is the only way to see what happened.
    const char* external = app->activity->externalDataPath;
    const char* internal = app->activity->internalDataPath;
    const std::filesystem::path files_dir = external ? external : (internal ? internal : "");
    const bool have_file_log = ember::SetLogFile(files_dir / "ember.log");
    ember::InstallCrashHandlers();
    EMBER_LOG("android_main entered (file log %s: %s/ember.log)", have_file_log ? "on" : "FAILED",
              files_dir.string().c_str());
    EMBER_LOG("externalDataPath=%s internalDataPath=%s", external ? external : "(null)",
              internal ? internal : "(null)");

    AppState state;
    {
        std::ifstream in(files_dir / "settings.txt");
        std::stringstream text;
        text << in.rdbuf();
        state.settings = ember::app::ParseSettings(text.str());
        EMBER_LOG("Settings (%s): resolution scale %d%%, frame cap %d, shadows %s",
                  in ? "settings.txt" : "defaults", state.settings.resolution_scale_percent,
                  state.settings.frame_cap, state.settings.shadows ? "on" : "off");
    }
    app->userData = &state;
    app->onAppCmd = OnCommand;
    app->onInputEvent = OnInput;

    std::string error;
    const std::vector<const char*> extensions = {"VK_KHR_surface", "VK_KHR_android_surface"};
    state.instance_ready = state.renderer.Initialize(extensions, &error);
    if (state.instance_ready) {
        EMBER_LOG("Vulkan instance created");
    } else {
        EMBER_ERROR("Vulkan instance: %s", error.c_str());
    }

    // Scanning the game folder can take a while; do it on a worker so window events keep being
    // processed and the app shows its grey "loading" frame immediately.
    ember::app::Config config;
    config.settings = state.settings;
    config.game_root = ember::app::LocateGameRoot(files_dir);
    // The launcher's "Verify files" button drops this marker; it is consumed so the slow pass
    // runs once per request.
    {
        std::error_code ec;
        const std::filesystem::path marker = files_dir / "verify.request";
        if (std::filesystem::exists(marker, ec)) {
            config.verify_archives = true;
            config.verify_report = files_dir / "verify_result.txt";
            std::filesystem::remove(marker, ec);
        }
    }
    state.startup = std::thread([&state, config] { state.engine.Startup(config); });

    auto last = std::chrono::steady_clock::now();
    while (!app->destroyRequested) {
        // Block while paused or without a window; poll when there is something to draw. The
        // timeout is re-evaluated after every event: a stale value here would keep the loop
        // blocked after the window and resume events arrived, so no frame would ever be drawn.
        int timeout = CanDraw(state) ? 0 : -1;
        android_poll_source* source = nullptr;
        int events = 0;
        while (ALooper_pollOnce(timeout, nullptr, &events, reinterpret_cast<void**>(&source)) >= 0) {
            if (source) {
                source->process(app, source);
            }
            if (app->destroyRequested) {
                break;
            }
            timeout = CanDraw(state) ? 0 : -1;
            source = nullptr;
        }
        if (app->destroyRequested) {
            break;
        }
        if (state.engine.WantsQuit()) {
            ANativeActivity_finish(app->activity);
        }
        // The window may have arrived while the app was already resumed.
        AttachWindow(app, state);

        // Frame cap: wait out the rest of the frame time instead of drawing early.
        if (state.settings.frame_cap > 0 && CanDraw(state)) {
            const auto frame_time = std::chrono::duration<double>(1.0 / state.settings.frame_cap);
            const auto due = last + std::chrono::duration_cast<std::chrono::steady_clock::duration>(frame_time);
            if (std::chrono::steady_clock::now() < due) std::this_thread::sleep_until(due);
        }
        const auto now = std::chrono::steady_clock::now();
        const double dt = std::chrono::duration<double>(now - last).count();
        last = now;
        if (CanDraw(state)) {
            if (!state.engine.Frame(state.renderer, dt)) {
                EMBER_ERROR("Frame failed; stopping");
                ANativeActivity_finish(app->activity);
            } else {
                ++state.frames;
                if (state.frames == 1 || state.frames % 600 == 0) {
                    EMBER_LOG("frame %llu presented",
                              static_cast<unsigned long long>(state.frames));
                }
            }
        }
    }
    EMBER_LOG("android_main exiting");
    if (state.startup.joinable()) {
        state.startup.join();
    }
    state.engine.ReleaseGpu(state.renderer);
    state.renderer.Shutdown();
}

# Ember

Licensed under the GNU General Public License v3.0 or later (see `LICENSE`).

An open-source, from-scratch game engine for ARM64 Android (Vulkan 1.1, tuned for tile-based
GPUs such as the Adreno 612). Like OpenMW, it ships **no game data**: you copy your own legitimate
installation onto the device and the engine reads it at runtime. See `CLEANROOM.md` for the
rules that keep the code original.

## Status: M0 scaffold

What exists today:

- `src/vfs/` case-insensitive virtual file system over your game folder (host-tested).
- `src/gfx/renderer.*` Vulkan 1.1 startup: instance, device, swapchain with display pre-rotation,
  a single tiler-friendly render pass (colour stored, depth transient and never stored),
  2 frames in flight, swapchain recreation when the Android window is lost and regained.
- `src/app/engine.*` startup pipeline: locate and mount the game folder, report what was found,
  draw. The screen breathes dark blue when the folder is found and dark red when it is not.
- `src/platform/android_main.cpp` NativeActivity entry point and window lifecycle.
- CI builds the host tests and a debug APK (untested on a device so far).

## Providing your game files

Copy your installation into the app's private folder, `Android/data/org.ember.engine/files/game/`
(or write a different absolute path on the first line of `files/game_root.txt`). The log prints the
mounted file count and top-level entries.

## Milestones (honest scope)

| M | Goal |
|---|------|
| 0 | Boot, Vulkan clear, mount game folder (this scaffold) |
| 1 | Archive/container readers written from documented formats; `docs/formats/` |
| 2 | Shader pipeline (GLSL to SPIR-V at build time), first textured mesh from game data |
| 3 | Map geometry, camera, free-fly through a level |
| 4 | Character skeleton and animation playback |
| 5+ | Collision, input, game rules, scripts, audio |

Rendering is the easy part. The hard parts are physics and animation data, the scripting and
event systems, and matching original behaviour. Expect years, not weeks, and treat each milestone
as useful on its own.

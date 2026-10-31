// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "app/model_loader.h"
#include "formats/msb.h"
#include "vfs/vfs.h"

namespace ember::app {

/// "/map/MapStudio/m10_01_00_00.msb" -> "m10_01_00_00" (lowercase, no directory or extension).
[[nodiscard]] std::string MapIdFromMsbPath(const std::string& path);

/// Paths to try, in order, for a model named `model_name` of map `map_id`. The naming of model
/// files is NOT confirmed against real data (see docs/formats/msb.md); every candidate is only a
/// guess that Vfs::Exists can confirm, and the loader logs the ones that miss.
[[nodiscard]] std::vector<std::string> ModelPathCandidates(const std::string& map_id,
                                                           const std::string& model_name);

/// Applies scale, then rotation (degrees; X first, then Z, then Y), then translation to a point.
/// The rotation order follows community documentation and is not verified here.
void TransformPoint(const float position[3], const float rotation_degrees[3], const float scale[3],
                    const float in[3], float out[3]);

/// Accumulates instances of models into one mesh whose triangles are grouped by diffuse texture
/// key, so the renderer needs one draw per texture instead of one per mesh per instance.
class WorldBuilder {
public:
    /// Adds a transformed copy of `model`. Returns the number of triangles added.
    std::size_t AddInstance(const LoadedModel& model, const float position[3],
                            const float rotation_degrees[3], const float scale[3]);
    [[nodiscard]] std::size_t Triangles() const noexcept { return triangles_; }
    /// Produces the merged model (one range per texture key) and clears the builder.
    [[nodiscard]] LoadedModel Finish(const std::string& description);
    /// Texture keys ordered by how many triangles use them, most used first (empty keys omitted).
    [[nodiscard]] std::vector<std::string> KeysByUse() const;

private:
    std::vector<float> positions_, uvs_;
    std::vector<std::vector<std::uint32_t>> material_indices_;
    std::vector<std::string> keys_;
    std::size_t triangles_ = 0;
    float min_[3] = {1e30f, 1e30f, 1e30f}, max_[3] = {-1e30f, -1e30f, -1e30f};
    [[nodiscard]] std::uint32_t MaterialFor(const std::string& key);
};

struct WorldOptions {
    std::size_t max_triangles = 450000; ///< memory and GPU budget for the Adreno 612 class
    double time_budget_seconds = 25.0;
};

struct LoadedWorld {
    LoadedModel model;                      ///< all map pieces merged
    std::vector<std::string> hot_textures;  ///< diffuse keys, most used first
    float start[3] = {};                    ///< where the free camera begins
    std::size_t instances = 0;
};

/// Loads the map pieces of one MSB: parses it, resolves each model to a file in the mounted game
/// data, decodes each distinct model once and places every instance. Parts nearest the middle of
/// the map are kept first when the triangle budget runs out. Returns nullopt (with `summary`
/// saying why: counters and the first unresolved names with the paths tried) when nothing was
/// placed.
[[nodiscard]] std::optional<LoadedWorld> LoadWorld(const vfs::Vfs& vfs, const std::string& msb_path,
                                                   const WorldOptions& options, std::string* summary);

} // namespace ember::app

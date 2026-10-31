// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "formats/dds.h"
#include "formats/flver.h"
#include "vfs/bhd5.h"

namespace ember::app {

/// One decoded model: merged positions (xyz triples) and a triangle list, plus the header's box.
struct LoadedModel {
    std::string description; ///< where it came from, for the log
    std::vector<float> positions;
    std::vector<float> uvs; ///< two floats per vertex
    std::vector<std::uint32_t> indices;
    std::vector<formats::FlverDrawRange> ranges;
    /// Per material: lowercase base name (no directory, no extension) of the diffuse texture, or
    /// empty when the material names none.
    std::vector<std::string> material_texture_keys;
    float bounds_min[3] = {};
    float bounds_max[3] = {};
};

enum class FlverFailure { None, Header, NoMeshes, Body, Geometry, Unclean };

/// Decodes one FLVER file into a LoadedModel with no size limits. Fails (reason in `error`, kind in
/// `failure`) on a bad header, tables or geometry, or when the geometry is not clean: dropped
/// triangles, non-finite vertices, or more than 1% of components outside the header's box.
[[nodiscard]] std::optional<LoadedModel> DecodeFlverModel(std::span<const std::byte> bytes,
                                                          const std::string& where, std::string* error,
                                                          FlverFailure* failure = nullptr);

/// Scans the mounted archives (in mount order, entries in index order) for the first model that
/// decodes cleanly and looks like a sensible first thing to show: between 1500 and 80000
/// triangles, no dropped triangles, no non-finite vertices and at most 1% of position components
/// outside the header's bounding box. Stops after `time_budget_seconds`. `summary` receives one
/// line of counters (how many entries and models were examined and why models were rejected) so a
/// failure can be diagnosed from the log.
[[nodiscard]] std::optional<LoadedModel> FindFirstModel(const std::vector<vfs::Bhd5Archive>& archives,
                                                        double time_budget_seconds,
                                                        std::string* summary);

/// A decoded texture: RGBA8 mip chain.
struct LoadedTexture {
    std::string key;
    std::vector<formats::RgbaImage> levels;
};

/// Looks the wanted texture names up in the TPF files stored in the archives' split bundles
/// (see vfs/tpf_scan.h) and decodes each one found, dropping top mip levels above 1024 pixels.
/// Top mip levels larger than `max_dimension` are dropped to save memory.
/// `summary` gets a one-line account (TPFs scanned, textures found/missing, decode problems).
[[nodiscard]] std::vector<LoadedTexture> FindTextures(const std::vector<vfs::Bhd5Archive>& archives,
                                                      const std::vector<std::string>& wanted_keys,
                                                      double time_budget_seconds, std::string* summary,
                                                      std::uint32_t max_dimension = 1024);

/// "N:\\FRPG\\data\\Model\\a\\Tex_A.tga" -> "tex_a".
[[nodiscard]] std::string TextureKey(const std::string& path);

} // namespace ember::app

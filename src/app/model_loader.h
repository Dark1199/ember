// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "vfs/bhd5.h"

namespace ember::app {

/// One decoded model: merged positions (xyz triples) and a triangle list, plus the header's box.
struct LoadedModel {
    std::string description; ///< where it came from, for the log
    std::vector<float> positions;
    std::vector<std::uint32_t> indices;
    float bounds_min[3] = {};
    float bounds_max[3] = {};
};

/// Scans the mounted archives (in mount order, entries in index order) for the first model that
/// decodes cleanly and looks like a sensible first thing to show: between 1500 and 80000
/// triangles, no dropped triangles, no non-finite vertices and at most 1% of position components
/// outside the header's bounding box. Stops after `time_budget_seconds`. `summary` receives one
/// line of counters (how many entries and models were examined and why models were rejected) so a
/// failure can be diagnosed from the log.
[[nodiscard]] std::optional<LoadedModel> FindFirstModel(const std::vector<vfs::Bhd5Archive>& archives,
                                                        double time_budget_seconds,
                                                        std::string* summary);

} // namespace ember::app

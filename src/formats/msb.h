// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ember::formats {

// MSB map files. Layout notes (from public community documentation, NOT yet verified on real
// files by this project) are in docs/formats/msb.md. Only the model and parts tables are decoded;
// the event and point tables are counted.

enum class MsbModelType : std::uint32_t { MapPiece = 0, Object = 1, Enemy = 2, Player = 4, Collision = 5, Navmesh = 6 };
enum class MsbPartType : std::uint32_t {
    MapPiece = 0,
    Object = 1,
    Enemy = 2,
    Player = 4,
    Collision = 5,
    Navmesh = 8,
    DummyObject = 9,
    DummyEnemy = 10,
    ConnectCollision = 11,
};

struct MsbModel {
    std::string name;     ///< raw bytes (Shift-JIS in the file)
    std::string sib_path;
    std::uint32_t type = 0;
    std::uint32_t id = 0;
    std::uint32_t instance_count = 0;
};

struct MsbPart {
    std::string name;
    std::string sib_path;
    std::uint32_t type = 0;
    std::uint32_t id = 0;
    std::uint32_t model_index = 0; ///< index into MsbFile::models
    float position[3] = {};
    float rotation_degrees[3] = {};
    float scale[3] = {1, 1, 1};
    std::uint32_t draw_groups[4] = {};
    std::uint32_t disp_groups[4] = {};
};

struct MsbTable {
    std::string name;
    std::uint32_t entry_count = 0;
};

struct MsbFile {
    std::vector<MsbTable> tables; ///< every table found, in file order
    std::vector<MsbModel> models;
    std::vector<MsbPart> parts;

    /// The model a part refers to, or nullptr when the index is out of range.
    [[nodiscard]] const MsbModel* ModelOf(const MsbPart& part) const noexcept {
        return part.model_index < models.size() ? &models[part.model_index] : nullptr;
    }
    /// Entry count of a table by name (0 when absent).
    [[nodiscard]] std::uint32_t TableCount(const std::string& name) const noexcept;
};

/// Parses the table chain, the model table and the parts table. Every offset and count is
/// range-checked, so damaged or wrongly assumed input fails with a reason (and the first bytes of
/// the file) instead of reading out of bounds. Requires a model and a parts table.
[[nodiscard]] std::optional<MsbFile> ParseMsb(std::span<const std::byte> bytes, std::string* error);

/// One log line: table counts, part counts by type, how many parts name a model that does not
/// exist, and the bounding box of part positions. Meant for the on-device log so wrong layout
/// assumptions are visible as implausible numbers.
[[nodiscard]] std::string SummarizeMsb(const MsbFile& msb);

} // namespace ember::formats

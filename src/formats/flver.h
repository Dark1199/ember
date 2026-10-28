// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace ember::formats {

// ---------------------------------------------------------------------------------------------
// FLVER model files: header only (so far).
//
// Layout of the first 0x5C bytes as described by public community documentation. It has NOT been
// verified against real files by this project: `ParseFlverHeader` checks what can be checked
// (magic, byte order, ranges, finite bounding box) and the startup survey reports statistics over
// real files so wrong field positions show up as implausible values before anything depends on
// them.
//
//   0x00 char[6] "FLVER\0"     0x06 char[2] "L\0" little-endian or "B\0" big-endian
//   0x08 i32 version           0x0C i32 data offset      0x10 i32 data size
//   0x14 i32 dummy count       0x18 i32 material count   0x1C i32 bone count
//   0x20 i32 mesh count        0x24 i32 vertex buffer count
//   0x28 f32[3] bounding box min   0x34 f32[3] bounding box max
//   0x40 i32 face count        0x44 i32 total face count
//   0x48 u8 vertex index size (bits)   0x49 u8 unicode   0x4A u8 unknown   0x4B u8 pad
//   0x4C i32 unknown           0x50 i32 face set count   0x54 i32 buffer layout count
//   0x58 i32 texture count
// ---------------------------------------------------------------------------------------------

inline constexpr std::size_t kFlverHeaderBytes = 0x5C;

struct FlverHeader {
    bool big_endian = false;
    std::uint32_t version = 0;
    std::uint32_t data_offset = 0;
    std::uint32_t data_size = 0;
    std::uint32_t dummy_count = 0;
    std::uint32_t material_count = 0;
    std::uint32_t bone_count = 0;
    std::uint32_t mesh_count = 0;
    std::uint32_t vertex_buffer_count = 0;
    float bounds_min[3] = {};
    float bounds_max[3] = {};
    std::uint32_t face_count = 0;
    std::uint32_t total_face_count = 0;
    std::uint8_t vertex_index_bits = 0;
    bool unicode = false;
    std::uint32_t face_set_count = 0;
    std::uint32_t buffer_layout_count = 0;
    std::uint32_t texture_count = 0;
};

[[nodiscard]] bool IsFlver(std::span<const std::byte> bytes) noexcept;

/// Parses and sanity-checks the header. Fails (with the reason and the first bytes) on a bad
/// magic or byte order, a data range outside the file, implausible counts or a non-finite or
/// inverted bounding box.
[[nodiscard]] std::optional<FlverHeader> ParseFlverHeader(std::span<const std::byte> bytes,
                                                          std::string* error);

} // namespace ember::formats

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

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
/// inverted bounding box (an inverted "empty bounds" box is accepted when there are no meshes).
[[nodiscard]] std::optional<FlverHeader> ParseFlverHeader(std::span<const std::byte> bytes,
                                                          std::string* error);

// ---------------------------------------------------------------------------------------------
// Body tables (also from public community documentation, NOT yet confirmed on real files). After
// the header (padded to 0x80 bytes) the tables follow back to back in this order, with these entry
// sizes: dummies 0x40, materials 0x20, bones 0x80, meshes 0x30, face sets 0x20 (confirmed on real files; 0x1C
// and 0x18 are still tried as fallbacks and the one that validates is used), vertex buffers 0x20, buffer layouts 0x10,
// textures 0x20. Names and extra lists live between the tables and the data block.
//
//   mesh:         +0x00 u8 dynamic  +0x04 material  +0x10 default bone  +0x20 face set count
//                 +0x24 offset of face set index list  +0x28 vertex buffer count
//                 +0x2C offset of vertex buffer index list (offsets are absolute in the file)
//   face set:     +0x00 flags (0 = main level of detail)  +0x04 u8 strip  +0x05 u8 cull
//                 +0x08 index count  +0x0C index offset (relative to the data block)
//   vertex buffer:+0x00 buffer index  +0x04 layout index  +0x08 vertex size  +0x0C vertex count
//                 +0x18 buffer length  +0x1C buffer offset (relative to the data block)
//   layout:       +0x00 member count  +0x0C absolute offset of members, each 0x14 bytes:
//                 +0x04 offset in vertex  +0x08 type (2 = three floats)  +0x0C semantic
//                 (0 = position)  +0x10 index
//
// Every value used is range-checked against the file, and the decoded positions are compared to
// the header's bounding box, so a wrong guess shows up as a failure rather than as garbage.
// ---------------------------------------------------------------------------------------------

struct FlverMesh {
    std::uint32_t material_index = 0;
    std::uint32_t default_bone = 0;
    bool dynamic = false;
    std::vector<std::uint32_t> face_sets;
    std::vector<std::uint32_t> vertex_buffers;
};
struct FlverFaceSet {
    std::uint32_t flags = 0;
    bool triangle_strip = false;
    bool cull_backfaces = false;
    std::uint32_t index_count = 0;
    std::uint32_t index_offset = 0;
};
struct FlverVertexBuffer {
    std::uint32_t buffer_index = 0;
    std::uint32_t layout_index = 0;
    std::uint32_t vertex_size = 0;
    std::uint32_t vertex_count = 0;
    std::uint32_t buffer_length = 0;
    std::uint32_t buffer_offset = 0;
};
struct FlverMember {
    std::uint32_t struct_offset = 0;
    std::uint32_t type = 0;
    std::uint32_t semantic = 0;
    std::uint32_t index = 0;
};

struct FlverMaterial {
    std::string name;
    std::string mtd; ///< material definition name
    std::uint32_t texture_count = 0;
    std::uint32_t texture_index = 0; ///< first entry in FlverBody::textures
};
struct FlverTexture {
    std::string path; ///< as stored, usually a Windows path ending in a file name
    std::string type; ///< usage, e.g. a diffuse or normal-map slot name
};

struct FlverBody {
    std::uint32_t face_set_stride = 0; ///< which candidate entry size validated
    std::vector<FlverMesh> meshes;
    std::vector<FlverFaceSet> face_sets;
    std::vector<FlverVertexBuffer> vertex_buffers;
    std::vector<std::vector<FlverMember>> layouts;
    std::vector<FlverMaterial> materials; ///< entry 0x20; strings UTF-16 when the header says unicode
    std::vector<FlverTexture> textures;   ///< entry 0x20 after the layouts; strings as above
    bool strings_ok = true;               ///< false if a material/texture string could not be read
};

/// Size in bytes of everything from the file start through the last table, assuming the given
/// face-set entry size. A correct layout never exceeds the header's data offset.
[[nodiscard]] std::uint64_t FlverTablesEnd(const FlverHeader& header, std::uint32_t face_set_stride);

/// Reads the mesh, face-set, vertex-buffer and layout tables. Fails with a reason if no
/// candidate layout validates.
[[nodiscard]] std::optional<FlverBody> ParseFlverBody(std::span<const std::byte> bytes,
                                                      const FlverHeader& header, std::string* error);

/// One mesh's slice of the merged triangle list.
struct FlverDrawRange {
    std::uint32_t first_index = 0;
    std::uint32_t index_count = 0;
    std::uint32_t material_index = 0;
};

/// All meshes merged: positions (xyz triples), texture coordinates (uv pairs, first set) and a
/// triangle list. Only the main level of detail of each mesh is used. UVs: semantic 5, type 21
/// (two int16) or 22 (two pairs of int16, the first is used), divided by 1024.
struct FlverGeometry {
    std::vector<float> positions;
    std::vector<float> uvs;
    std::vector<FlverDrawRange> ranges;
    std::uint32_t meshes_with_uvs = 0;
    float uv_min[2] = {0, 0};
    float uv_max[2] = {0, 0};
    std::vector<std::uint32_t> indices;
    std::uint32_t vertex_count = 0;
    std::uint32_t meshes_decoded = 0;
    std::uint32_t meshes_without_positions = 0;
    std::uint32_t meshes_without_faces = 0;
    std::uint32_t strip_sets = 0;
    std::uint32_t list_sets = 0;
    std::uint64_t bad_indices = 0;               ///< triangles dropped: index past the vertex count
    std::uint64_t non_finite_vertices = 0;
    std::uint64_t outside_bounds_components = 0; ///< position components outside the header box
};

[[nodiscard]] std::optional<FlverGeometry> DecodeFlverGeometry(std::span<const std::byte> bytes,
                                                               const FlverHeader& header,
                                                               const FlverBody& body,
                                                               std::string* error);

} // namespace ember::formats
